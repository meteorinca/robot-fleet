

## Quick Wiring Reminder:
need a simple voltage divider circuit:

```
     +5V
      |
     [ ] 10kΩ Resistor (fixed)
      |
      +------- To Arduino A0
      |
     [ ] Photoresistor (LDR)
      |
     GND
```

The photoresistor will give readings from 0-1023 just like any other analog photodetector. threshold logic applies.

**If your readings seem inverted** (dark = high values, light = low values), just swap the resistor and LDR positions:

```
     +5V
      |
     [ ] Photoresistor (LDR)
      |
      +------- To Arduino A0
      |
     [ ] 10kΩ Resistor (fixed)
      |
     GND
```

Then adjust your `THRESHOLD` value accordingly.



### What & why

| Line | Purpose |
|------|---------|
| `mySwitch.setProtocol(1)` | Locks the waveform to **EV1527/PT2262** style (short/long pulses) instead of leaving it at the RCSwitch default. |
| `mySwitch.setPulseLength(185)` | Forces the time-unit (`T`) to **185 µs**. Without this, RCSwitch defaults to ~350 µs and your IDF receiver (configured for 185 µs) would see the wrong timings. |
| `mySwitch.setRepeatTransmit(5)` | Sends the 24-bit code **5 times in a row** per `send()` call. This gives the ESP32 RMT peripheral multiple chances to latch a complete frame even if the first edge is missed. |

### Timing check (both sides now match)

| Parameter | Arduino (RCSwitch) | ESP-IDF (RMT) |
|-----------|-------------------|---------------|
| Protocol | 1 | 1 |
| Pulse unit (`T`) | 185 µs | 185 µs |
| Bits | 24 | 24 |
| Sync gap | 31 × 185 = **5.7 ms** | RX timeout = **10 ms** ✓ |
| “0” bit low time | 3 × 185 = **555 µs** | decoder threshold > 370 µs |
| “1” bit low time | 1 × 185 = **185 µs** | decoder threshold ≤ 370 µs |

The 10 ms RMT idle timeout on the ESP32 side is comfortably larger than the 5.7 ms sync gap, so each full packet (including repeats) will be captured cleanly in one shot.