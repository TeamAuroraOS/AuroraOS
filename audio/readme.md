# Convert audio files to `.aaf` (Aurora Audio File) for later use in the OS.

pip install pydub simpleaudio
python aaf_tool.py convert song.mp3 song.aaf --rate 8000
python aaf_tool.py play song.aaf