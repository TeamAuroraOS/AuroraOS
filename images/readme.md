# A tool to convert png and jpg to `.apf. (Aurora Picture File)

## Create file
pip install --user Pillow
python apf_tool.py convert photo.png
python apf_tool.py convert photo.jpg out.apf --alpha   # keep transparency (RGBA)


## Open file
python apf_tool.py open out.apf              # pops it up in your default viewer
python apf_tool.py open out.apf --save x.png  # or convert it back to PNG