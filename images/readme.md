# Aurora Picture File (.apf)

pip install --user Pillow
python apf_tool.py convert photo.png
python apf_tool.py convert photo.jpg out.apf --alpha
python apf_tool.py open out.apf
python apf_tool.py open out.apf --save x.png