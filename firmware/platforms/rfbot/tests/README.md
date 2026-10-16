# RF decoder checks

These tests execute the actual portable C decoder through a host shared library.
They cover all 12 RC-switch protocols, the 433 MHz photodetector and outlet codes,
timing jitter, corruption, zero and 32-bit codes, multiple packets, receive-buffer
boundaries, and sustained noise.

On Linux or macOS, from the RFbot project directory:

```sh
cc -shared -fPIC -O2 -Wall -Wextra -Werror components/RCSwitch/rf_codec.c -o build/rf_codec_host.so
python3 tests/test_rf_codec.py ./build/rf_codec_host.so
```

On Windows, the installed ESP Clang 16 tools can build a DLL without Visual Studio:

```powershell
$compilerDir = 'C:/Espressif/tools/esp-clang/16.0.1-fe4f10a809/esp-clang/bin'
& "$compilerDir/clang.exe" --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -O2 -Wall -Wextra -Werror -c components/RCSwitch/rf_codec.c -o build/rf_codec_host.obj
if ($LASTEXITCODE -ne 0) { throw 'Compile failed' }
& "$compilerDir/lld-link.exe" /dll /noentry /export:rf_decode /out:build/rf_codec_host.dll build/rf_codec_host.obj
if ($LASTEXITCODE -ne 0) { throw 'Link failed' }
python tests/test_rf_codec.py ./build/rf_codec_host.dll
```

Hardware verification after flashing:

1. Leave the bot idle and compare ESP32 temperature and HTTP response time with
   rfbot1 on the older firmware, using the same power supply and RF wiring.
2. Confirm `/rf/relay` reports `enabled: false` after reboot. Send a photodetector
   signal and confirm `/rf/poll` returns its code without a SpeakerBot request.
3. Test outlet ON/OFF, remote bursts, and reception during normal HTTP traffic.
4. Temporarily enable the fallback relay and test it, then disable it again.
5. Interrupt Wi-Fi and restore it. Confirm the retry counter resets after an IP
   is obtained and the fallback hotspot stops once the station recovers.
6. Test OTA and settings writes during RF traffic. The RMT callback runs under
   the driver's normal cache policy; no GPIO handler calls flash code with the
   cache disabled.

The host tests verify decoding. Pulse timing, reception sensitivity, Wi-Fi
recovery, and temperature need physical hardware measurements.
