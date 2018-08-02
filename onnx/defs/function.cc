// Copyright (c) Facebook Inc. and Microsoft Corporation.
// Licensed under the MIT license.

#include "onnx/defs/function.h"
#include "onnx/checker.h"
#include "onnx/defs/schema.h"
#include "onnx/string_utils.h"

namespace ONNX_NAMESPACE {
using namespace checker;
FunctionBuilder& FunctionBuilder::SetDomain(const std::string& domain) {
  domain_ = domain;
  return *this;
}

const std::string& FunctionBuilder::GetDomain() const {
  return domain_;
}

FunctionBuilder& FunctionBuilder::SetBuildFunction(BuildFunction build_func) {
  build_func_ = build_func;
  return *this;
}

BuildFunction FunctionBuilder::GetBuildFunction() const {
  return build_func_;
}

Status FunctionBuilderRegistry::Register(
    const FunctionBuilder& function_builder) {
  std::lock_guard<std::mutex> lock(mutex_);
  function_builders.push_back(function_builder);
  return Status::OK();
}

// Get functions for specific domain.
Status FunctionBuilderRegistry::GetFunctions(
    const std::string& domain,
    /*out*/
    std::multimap<std::string, std::unique_ptr<FunctionProto>>* function_set)
    const {
  if (nullptr == function_set) {
    return Common::Status(
        Common::CHECKER,
        Common::INVALID_ARGUMENT,
        "function_set should not be nullptr.");
  }

  for (auto func_builder : function_builders) {
    if (func_builder.GetDomain() != domain) {
      continue;
    }
    std::unique_ptr<FunctionProto> function_proto;
    auto status = func_builder.GetBuildFunction()(&function_proto);
    if (!status.IsOK()) {
      return status;
    }

    CheckerContext ctx;
    std::unordered_map<std::string, int> op_set;
    auto version_range =
        OpSchemaRegistry::DomainToVersionRange::Instance().Map().at(
            func_builder.GetDomain());
    if (function_proto->since_version() > version_range.second ||
        function_proto->since_version() < version_range.first) {
      fail_check("Invalid function version in '", function_proto->name(), "'");
    }
    op_set.insert({func_builder.GetDomain(), (int)function_proto->since_version()});
    ctx.set_opset_imports(op_set);
    ctx.set_is_main_graph(false);
    LexicalScopeContext lex_ctx;
    try {
      check_function(*function_proto, ctx, lex_ctx);
    } catch (ValidationError& ex) {
      return Common::Status(
          Common::CHECKER, Common::INVALID_PROTOBUF, ex.what());
    }

    auto& func_name = function_proto->name();
    // Check no op version conflicts.
    auto range = function_set->equal_range(func_name);
    for (auto i = range.first; i != range.second; ++i) {
      auto version = i->second->since_version();
      if (function_proto->since_version() == version) {
        // There's already a function with same name/since_version registered.
        return Common::Status(
            Common::CHECKER,
            Common::FAIL,
            ONNX_NAMESPACE::MakeString(
                "A function (",
                func_name,
                ") with version (",
                version,
                ") has already been registered."));
      }
    }
    function_set->emplace(func_name, std::move(function_proto));
  }

  return Common::Status::OK();
}

FunctionBuilderRegistry& FunctionBuilderRegistry::OnnxInstance() {
  static FunctionBuilderRegistry func_builder_registry;
  return func_builder_registry;
}

std::string RenameTensorNode(
  const std::string& func_name,
  int counter,
  const std::string& internal_name
) {
  std::string new_name =
    "Func_" + func_name + "_" + std::to_string(counter) + "_" + internal_name;
  return new_name;
}

void FunctionExpandHelper(
  const FunctionProto& func,
  const NodeProto& node,
  int counter,
  GraphProto& g
) {
  std::string function_name = func.name();
  int version = (int)func.since_version();
  std::unordered_map<std::string, std::string> input_names_map;
  std::unordered_map<std::string, std::string> output_names_map;
  std::unordered_map<std::string, AttributeProto> attr_map;

  for (int idx = 0; idx < node.input_size(); ++idx) {
    input_names_map[func.input()[idx]] = node.input()[idx];
  }
  for (int idx = 0; idx < node.output_size(); ++idx) {
    output_names_map[func.output()[idx]] = node.output()[idx];
  }

  for (auto& attr : node.attribute()) {
    attr_map[attr.name()] = attr;
  }

  for (auto& function_node : func.node()) {
    NodeProto* new_node = g.add_node();
    new_node->CopyFrom(function_node);
    new_node->clear_input();
    new_node->clear_output();
    new_node->clear_attribute();
    for (auto& input : function_node.input()) {
      if (input_names_map.count(input)) {
        new_node->add_input(input_names_map[input]);
      }
      else {
        new_node->add_input(RenameTensorNode(func.name(), counter, input));
      }
    }
    for (auto& output : function_node.output()) {
      if (output_names_map.count(output)) {
        new_node->add_output(output_names_map[output]);
      }
      else {
        new_node->add_output(RenameTensorNode(func.name(), counter, output));
      }
    }
    for (auto& attr : function_node.attribute()) {
      AttributeProto* new_attr = new_node->add_attribute();
      if (attr.has_ref_attr_name()) {
        new_attr->CopyFrom(attr_map[attr.ref_attr_name()]);
      }
      else {
        new_attr->CopyFrom(attr);
      }
    }
  }
}

Status DecomposeGraph(ModelProto& input_model) {
  auto g = input_model.graph();
  const std::string& domain = input_model.has_domain() ? input_model.domain() : "";
  GraphProto new_g = GraphProto(g);

  const std::vector<OpSchema> op_schemas = OpSchemaRegistry::get_all_schemas();
  std::unordered_set<std::string> registered_schemas;
  for (const auto& op : op_schemas) {
    registered_schemas.insert(op.Name());
  }

  std::multimap<std::string, std::unique_ptr<FunctionProto>> pfunction_map;
  FunctionBuilderRegistry& function_registry =
    FunctionBuilderRegistry::OnnxInstance();
  Common::Status status =
    function_registry.GetFunctions(domain, &pfunction_map);

  new_g.clear_node();
  std::unordered_map<std::string, int> func_counter;

  for (int idx = 0; idx < g.node_size(); ++idx) {
    auto& node = g.node()[idx];
    if (registered_schemas.count(node.op_type())) {
      auto temp_node = new_g.add_node();
      temp_node->CopyFrom(node);
    }
    else if (!pfunction_map.count(node.op_type())) {
      throw std::runtime_error(
        "Failed to recognize op/function '" + node.op_type() +
        "'!");
    }
    else {
      func_counter.count(node.op_type()) ?
        func_counter[node.op_type()]++ : func_counter[node.op_type()] = 0;
      FunctionExpandHelper(*(pfunction_map.find(node.op_type())->second),
        node,
        func_counter[node.op_type()],
        new_g
      );
    }
  }
  delete input_model.release_graph();
  input_model.set_allocated_graph(new GraphProto(new_g));
  return Status::OK();
}
} // namespace ONNX_NAMESPACE
