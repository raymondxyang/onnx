git submodule update --init --recursive
set ONNX_BUILD_TESTS=1
set ONNX_ML=1
python setup.py bdist_wheel --universal --dist-dir .