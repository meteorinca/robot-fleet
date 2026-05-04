import sys
import subprocess
import os
import requests
import asyncio

try:
    import edge_tts
except ImportError:
    print("Please install edge-tts: pip install edge-tts")
    sys.exit(1)

async def generate_and_stream(text, voice="en-US-GuyNeural"):
    print(f"Generating TTS for: '{text}' using voice {voice}...")
    temp_mp3 = "temp_tts.mp3"
    
    communicate = edge_tts.Communicate(text, voice)
    await communicate.save(temp_mp3)
    
    print("Converting audio to 16kHz PCM...")
    command = [
        'ffmpeg',
        '-i', temp_mp3,
        '-f', 's16le',
        '-acodec', 'pcm_s16le',
        '-ar', '16000',
        '-ac', '1',
        '-'
    ]
    
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
        raw_pcm_data = result.stdout
    except subprocess.CalledProcessError as e:
        print(f"FFmpeg conversion failed: {e}")
        return
    except FileNotFoundError:
        print("FFmpeg is not installed or not in PATH.")
        return
    finally:
        if os.path.exists(temp_mp3):
            os.remove(temp_mp3)
            
    url = "http://dogbot.local:81/audio"
    print(f"Streaming {len(raw_pcm_data)} bytes to {url}...")
    
    try:
        response = requests.post(url, data=raw_pcm_data)
        print("Finished! ESP32 Response:", response.text)
    except Exception as e:
        print(f"Error streaming to {url}: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python speak.py 'Hello, I am a dog bot'")
        sys.exit(1)
        
    text = " ".join(sys.argv[1:])
    asyncio.run(generate_and_stream(text))
