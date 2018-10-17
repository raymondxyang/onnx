dir /s /b /a-d "onnx_gtests.exe" >UT.txt & set /p _UT= < UT.txt
%_UT%
dir /b /a-d "*.whl" >WheelFile.txt & set /p _wheel= < WheelFile.txt
pip install %_wheel%
pytest
python onnx/defs/gen_doc.py
python onnx/gen_proto.py