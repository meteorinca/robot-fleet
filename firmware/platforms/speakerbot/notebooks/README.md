# SpeakerBot Jupyter Audio Player

This folder contains tools and notebooks to stream any MP3, WAV, M4A, OGG, or FLAC audio file from Python to SpeakerBot.

## Files
- **[`speakerbot_audio_player.ipynb`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/speakerbot/notebooks/speakerbot_audio_player.ipynb)**: Interactive Jupyter Notebook.
- **[`speakerbot_player.py`](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/speakerbot/notebooks/speakerbot_player.py)**: Python client library with auto-conversion and HTTP streaming.

## Setup Instructions

1. Install Python requirements:
```bash
pip install requests pydub
```
*(Optionally install `ffmpeg` on your machine if you prefer system-level audio decoding).*

2. Drop any `.mp3` or `.wav` files into this `notebooks/` directory.

3. Open `speakerbot_audio_player.ipynb` in Jupyter Notebook or VS Code and run the cells!

## Python Code Example

```python
from speakerbot_player import SpeakerBot

bot = SpeakerBot("http://speakerbot8.local")

# Play any local MP3 file
bot.play_file("my_alarm.mp3", interrupt=True)

# Emergency Stop
bot.stop()

# Play hardware clip
bot.play_clip("bark", repeat=3)
```
