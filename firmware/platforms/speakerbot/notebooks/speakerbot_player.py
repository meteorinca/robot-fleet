"""
SpeakerBot Python Client Library
--------------------------------
Provides easy audio conversion and streaming to SpeakerBot over HTTP REST API.
Supports MP3, WAV, M4A, FLAC, OGG, AAC, and embedded sound clips.
"""

import os
import sys
import time
import threading
import subprocess
import requests

class SpeakerBot:
    def __init__(self, host="http://speakerbot8.local"):
        self.host = host.rstrip('/')
        self._stop_event = threading.Event()  # shared stop flag for streaming

    def ping(self):
        """Check if SpeakerBot is online and reachable."""
        try:
            r = requests.get(f"{self.host}/status", timeout=2)
            return r.status_code == 200
        except Exception:
            return False

    def get_status(self):
        """Fetch status and system details from SpeakerBot."""
        try:
            r = requests.get(f"{self.host}/status", timeout=2)
            return r.json()
        except Exception as e:
            return {"error": str(e)}

    def stop(self):
        """Instantly flush audio queue and stop SpeakerBot playback."""
        # Signal any in-progress play_file / play_file_async to abort streaming
        self._stop_event.set()
        try:
            r = requests.get(f"{self.host}/stop", timeout=2)
            return r.json()
        except Exception as e:
            return {"error": str(e)}

    def play_clip(self, name="bark", repeat=1, interrupt=True):
        """
        Play an embedded sound clip on SpeakerBot.
        Clips: 'bark', 'paulbot', 'huh', 'yes', 'jump', 'ding', 'random'
        """
        params = {
            "name": name,
            "repeat": repeat,
            "interrupt": 1 if interrupt else 0
        }
        try:
            r = requests.get(f"{self.host}/sound", params=params, timeout=5)
            return r.json()
        except Exception as e:
            return {"error": str(e)}

    def play_file(self, file_path, interrupt=True, volume=0.5, chunk_sec=4.0):
        """
        Convert any local audio file (MP3, WAV, M4A, OGG, FLAC) to 16kHz 16-bit mono PCM
        and stream it to SpeakerBot over POST /audio.

        volume: 0.1 to 1.0 (default 0.5 = clean -6dB volume headroom, preventing hardware clipping).
        chunk_sec: seconds of audio per POST chunk (default 4.0s).
                   Larger chunks = fewer POSTs = less clipping risk.
                   For small files the entire audio may fit in a single chunk.
        """
        if not os.path.exists(file_path):
            raise FileNotFoundError(f"Audio file not found: {file_path}")

        # Reset the stop flag so a fresh play always works
        self._stop_event.clear()

        print(f"🎵 Loading audio file: {file_path}")
        pcm_bytes = self._convert_to_pcm16(file_path, volume=volume)

        total_duration_sec = len(pcm_bytes) / 32000.0
        print(f"⚡ Converted 16kHz PCM: {len(pcm_bytes)} bytes (~{total_duration_sec:.1f}s)")

        # --- BUG FIX 1: chunk_sec raised to 4s by default so small MP3s (< 4s)
        #     fit in a SINGLE chunk and are never sent as multiple independent POSTs.
        #     If the firmware plays each POST as a standalone audio clip, multiple
        #     small chunks cause the sound to repeat N times.  One chunk = plays once.
        chunk_size = int(chunk_sec * 32000)
        total_chunks = (len(pcm_bytes) + chunk_size - 1) // chunk_size

        print(f"🚀 Streaming {total_chunks} chunk(s) to {self.host}/audio (Press Kernel Stop to interrupt)...")

        # --- BUG FIX 3: back-pressure pacing ---
        # Track wall-clock time vs audio-time delivered to avoid flooding the device
        # buffer faster than it can drain, which causes buffer-overflow clipping.
        stream_start = time.monotonic()
        audio_delivered_sec = 0.0

        try:
            for i in range(total_chunks):
                # --- BUG FIX 2: honour stop() during streaming ---
                if self._stop_event.is_set():
                    print("\n\n⏹️ Stop requested! Halting stream...")
                    return False

                start = i * chunk_size
                end = min(start + chunk_size, len(pcm_bytes))
                chunk = pcm_bytes[start:end]
                chunk_duration_sec = len(chunk) / 32000.0

                endpoint = f"{self.host}/audio"
                if i == 0 and interrupt:
                    endpoint += "?interrupt=1"

                r = requests.post(
                    endpoint,
                    data=chunk,
                    headers={"Content-Type": "application/octet-stream"},
                    timeout=10
                )
                if r.status_code != 200:
                    print(f"\n❌ Chunk {i+1} failed with HTTP {r.status_code}")
                    return False

                audio_delivered_sec += chunk_duration_sec

                pct = int(((i + 1) / total_chunks) * 100)
                sys.stdout.write(f"\r▶️ Streaming chunk {i+1}/{total_chunks} ({pct}%) ...")
                sys.stdout.flush()

                # --- BUG FIX 3 (continued): pace the sender ---
                # Only start throttling after the first chunk (the first one also
                # signals interrupt so the device clears its buffer immediately).
                # Leave a 0.5s safety margin in the device buffer so we never
                # starve the speaker, but don't get too far ahead either.
                if i > 0:
                    elapsed = time.monotonic() - stream_start
                    # How far ahead of real-time are we?
                    lead = audio_delivered_sec - elapsed - 0.5  # 0.5s desired lead
                    if lead > 0:
                        # We're flooding the device; sleep to let it catch up
                        time.sleep(lead)

            print("\n✅ Playback complete!")
            return True
        except KeyboardInterrupt:
            print("\n\n⏹️ Kernel stop requested! Instantly silencing SpeakerBot...")
            self._stop_event.set()
            self.stop()
            print("✅ SpeakerBot hardware stopped.")
            return False

    def play_file_async(self, file_path, interrupt=True, volume=0.5, chunk_sec=4.0):
        """
        Play audio file in a non-blocking background thread so the Jupyter cell doesn't lock up.
        Call bot.stop() anytime to halt playback!
        """
        # Reset stop flag before spawning thread
        self._stop_event.clear()
        t = threading.Thread(
            target=self.play_file,
            args=(file_path, interrupt, volume, chunk_sec),
            daemon=True
        )
        t.start()
        print("▶️ Playing in background thread... Call bot.stop() in another cell to halt anytime.")
        return t

    def _convert_to_pcm16(self, file_path, volume=0.5):
        """Convert audio file to 16kHz 16-bit signed mono PCM bytes with headroom scaling."""
        # Convert volume factor (0.1..1.0) to dB adjustment
        import math
        volume = max(0.05, min(1.0, volume))
        gain_db = 20.0 * math.log10(volume) # e.g. 0.5 -> -6.02 dB

        # Method 1: Try pydub
        try:
            from pydub import AudioSegment
            audio = AudioSegment.from_file(file_path)

            # Apply gain headroom to prevent resampling overshoots & amp clipping
            audio = audio.apply_gain(gain_db)
            audio = audio.set_channels(1).set_frame_rate(16000).set_sample_width(2)

            # Final safety limiter check
            if audio.max_dBFS > -2.0:
                audio = audio.apply_gain(-2.0 - audio.max_dBFS)

            return audio.raw_data
        except Exception:
            pass

        # Method 2: Try ffmpeg via subprocess
        try:
            cmd = [
                "ffmpeg", "-y", "-i", file_path,
                "-af", f"volume={volume:.2f},aresample=resampler=soxr",
                "-f", "s16le", "-acodec", "pcm_s16le",
                "-ar", "16000", "-ac", "1", "-"
            ]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            return res.stdout
        except Exception:
            # Fallback simple ffmpeg without soxr filter
            cmd = [
                "ffmpeg", "-y", "-i", file_path,
                "-af", f"volume={volume:.2f}",
                "-f", "s16le", "-acodec", "pcm_s16le",
                "-ar", "16000", "-ac", "1", "-"
            ]
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            return res.stdout
