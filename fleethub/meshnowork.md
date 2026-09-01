
If i send a command using testmode from the webui, obviously it sees it and logs it and I hear speakerbot respond. And if I send manual to speakerbot from go it also works perfectly and responds.

BUUUT. IF I Have relay mode disabled on rfbot1.local, even if i hit the test command, I don't hear speakerbot say anything. THis means that the go app is unable to see anything from the rfbot1.local api output which is expected cos it might not be fully turned on or if it is there might be noise etc. How do we fix this complex issue with rx transcription and then using the go app as the actual relay. Right now without the go app, just using esp32, it works perfectly but there is a single point of failure and hard to create mesh network of sensors unless we get GO webapp to be the actual relay.

But if I manually send, or send using Test RF Signal (123456)
Tap anywhere to dismiss ✕
Dispatched RF Code 123456 via RFBot 6 Gateway (433.92 MHz).
SpeakerBot 1 audio alert (/choola) triggered. (I ONLY HEAR CHOOLA TRIGGER IF RELAYMODE ON RFBOT1 IS ENABLED WHICH MEANS THE GO APP IS NOT RX FROM RFBOT1)

Summary of issue:
1. IF RELAYMODE IS ENABLED THEN EVERYTHING WORKS AS INTENDED. If i send test cmd: the speakerbot plays choola
2. BUT THE MOMENT I DISABLE IT ON RFBOT1.LOCAL, The logs show that speakerbot was sent a command (these logs it seems avoid the actual rx and just use the click to register sent), and tx works from go app since we can control lights (using rfbot6 which is tx), but speakerbot obviously says nothing since it was not sent api command from go app (which works from the go app when pressing the Play Audio Tone button)

http://rfbot1.local/rf/poll does show real data (when I send 123456 via the go webapp):
{"code":"1E240","bits":22,"proto":1,"pulse":182,"relayed":true},{"code":"1E240","bits":24,"proto":1,"pulse":181,"relayed":true},{"code":"1E240","bits":24,"proto":1,"pulse":182,"relayed":true}, (the true is not there when relaymode is disabled from rfbot1.local)

This is an easy test: If relaymode is disabled, and I send test cmd using the webui, the speakerbot should play choola We know relay is working. It owuld also be nice for you to check the mesh network regularly and show in telemetry. Also where did my amazing ping app and amazing plots etc go from telemetry webui? Also a way to view the rf logs from the webui should also be there in the telemetry.

Also in the RF Mesh Gateway Nodes section, there should be a nice amazing animated (but minimal) visualiztion of the network and i should be able to click an tx rx and it animates and emits a radio wave animation that then echoes and pulses back from the rx side too. (in real time with real logs)

Also make sure to build for raspberry pi when done since I copy paste the pi3_Deploy folder, stop and rerun setup.h to get it working 

PLEASE FIX AND make it look nice. avoid emoticons and too verbose text. prefer minimalist