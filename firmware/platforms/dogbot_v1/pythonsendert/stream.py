import requests
import subprocess
import os

def stream_to_dog(audio_file_path):
    print(f"Loading {audio_file_path}...")
    
    if not os.path.exists(audio_file_path):
        print(f"File not found: {audio_file_path}")
        return

    # Use ffmpeg to convert any audio to 16kHz, 16-bit, Mono PCM
    command = [
        'ffmpeg',
        '-i', audio_file_path,
        '-f', 's16le',       # raw PCM, 16-bit signed little-endian
        '-acodec', 'pcm_s16le',
        '-ar', '16000',      # 16 kHz sample rate
        '-ac', '1',          # Mono (1 channel)
        '-'                  # Output to stdout
    ]
    
    print("Converting audio with ffmpeg...")
    try:
        # Run ffmpeg and capture stdout
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
        raw_pcm_data = result.stdout
    except subprocess.CalledProcessError as e:
        print(f"FFmpeg conversion failed: {e}")
        return
    except FileNotFoundError:
        print("FFmpeg is not installed or not in PATH.")
        return
    
    url = "http://dogbot.local:81/audio"
    print(f"Streaming {len(raw_pcm_data)} bytes to {url}...")
    
    try:
        response = requests.post(url, data=raw_pcm_data)
        print("Finished! ESP32 Response:", response.text)
    except Exception as e:
        print(f"Error streaming to {url}: {e}")

if __name__ == "__main__":
    stream_to_dog("sample.mp3")
