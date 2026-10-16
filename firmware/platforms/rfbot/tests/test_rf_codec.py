"""Run against a host build of rf_codec.c: python test_rf_codec.py codec.dll."""
import ctypes
import random
import sys
import unittest


class Pulse(ctypes.Structure):
    _fields_ = [("duration", ctypes.c_uint16), ("level", ctypes.c_uint8)]


class Decoded(ctypes.Structure):
    _fields_ = [("code", ctypes.c_uint32), ("bits", ctypes.c_uint),
                ("protocol", ctypes.c_uint), ("pulse", ctypes.c_uint),
                ("consumed", ctypes.c_size_t)]


# Independent fixtures from the RC-switch protocol definitions.
PROTOCOLS = [
    (350, (1, 31), (1, 3), (3, 1), False),
    (650, (1, 10), (1, 2), (2, 1), False),
    (100, (30, 71), (4, 11), (9, 6), False),
    (380, (1, 6), (1, 3), (3, 1), False),
    (500, (6, 14), (1, 2), (2, 1), False),
    (450, (23, 1), (1, 2), (2, 1), True),
    (150, (2, 62), (1, 6), (6, 1), False),
    (200, (3, 130), (7, 16), (3, 16), False),
    (200, (130, 7), (16, 7), (16, 3), True),
    (365, (18, 1), (3, 1), (1, 3), True),
    (270, (36, 1), (1, 2), (2, 1), True),
    (320, (36, 1), (1, 2), (2, 1), True),
]


def frame(protocol, code=0x553F4C, bits=24, pulse=None):
    base, sync, zero, one, inverted = PROTOCOLS[protocol-1]
    base = pulse or base
    def pair(factors):
        return [Pulse(base*factors[0], int(not inverted)),
                Pulse(base*factors[1], int(inverted))]
    pulses = pair(sync)
    for bit in range(bits-1, -1, -1):
        pulses += pair(one if code & (1 << bit) else zero)
    return pulses + pair(sync)


lib = ctypes.CDLL(sys.argv.pop(1))
lib.rf_decode.argtypes = [ctypes.POINTER(Pulse), ctypes.c_size_t, ctypes.POINTER(Decoded)]
lib.rf_decode.restype = ctypes.c_bool


def decode(pulses):
    data = (Pulse * len(pulses))(*pulses)
    decoded = Decoded()
    return lib.rf_decode(data, len(data), ctypes.byref(decoded)), decoded


class DecoderTests(unittest.TestCase):
    def test_supported_protocols(self):
        for protocol in range(1, 13):
            with self.subTest(protocol=protocol):
                ok, out = decode(frame(protocol))
                self.assertTrue(ok)
                self.assertEqual((out.code, out.bits), (0x553F4C, 24))
                # Protocols 11 and 12 differ only in nominal base pulse width.
                self.assertEqual(out.protocol, 11 if protocol == 12 else protocol)

    def test_433_photodetector_and_outlets(self):
        for code in [123456, 122222, 5576451, 5576460, 5584131, 5584140, 1381827, 1381836]:
            ok, out = decode(frame(1, code, pulse=185))
            self.assertTrue(ok)
            self.assertEqual((out.code, out.bits, out.pulse), (code, 24, 185))

    def test_zero_and_32_bit_codes(self):
        for code in [0, 0xFFFFFFFF, 0x80000001]:
            ok, out = decode(frame(1, code, bits=32, pulse=185))
            self.assertTrue(ok)
            self.assertEqual((out.code, out.bits), (code, 32))

    def test_jitter(self):
        pulses = frame(1, pulse=185)
        for i, pulse in enumerate(pulses):
            pulse.duration += [-12, 0, 12][i % 3]
        ok, out = decode(pulses)
        self.assertTrue(ok)
        self.assertEqual(out.code, 0x553F4C)

    def test_partial_and_corrupt_frames(self):
        pulses = frame(1, pulse=185)
        self.assertFalse(decode(pulses[8:])[0])
        self.assertFalse(decode(pulses[:-2])[0])
        pulses[20].duration = 17
        self.assertFalse(decode(pulses)[0])

    def test_oversized_frame(self):
        self.assertFalse(decode(frame(1, 0x1553F4C, bits=33, pulse=185))[0])

    def test_noise_then_valid_frame(self):
        noise = [Pulse(90+i, i % 2) for i in range(100)]
        ok, out = decode(noise + frame(1, pulse=185))
        self.assertTrue(ok)
        self.assertEqual(out.code, 0x553F4C)

    def test_distinct_packets_in_one_capture(self):
        first = frame(1, 123456, pulse=185)
        pulses = first + frame(1, 122222, pulse=185)[2:]
        ok, out = decode(pulses)
        self.assertTrue(ok)
        self.assertEqual(out.code, 123456)
        ok, second = decode(pulses[out.consumed-2:])
        self.assertTrue(ok)
        self.assertEqual(second.code, 122222)

    def test_chunk_boundary(self):
        pulses = frame(1, bits=32, pulse=185)
        for split in range(1, len(pulses)):
            previous = pulses[:split][-68:]
            ok, out = decode(previous + pulses[split:])
            self.assertTrue(ok)
            self.assertEqual((out.code, out.bits), (0x553F4C, 32))

    def test_sustained_short_noise(self):
        rng = random.Random(433)
        for _ in range(100):
            noise = [Pulse(rng.randrange(1, 1400), i % 2) for i in range(452)]
            self.assertFalse(decode(noise)[0])


if __name__ == "__main__":
    unittest.main()
