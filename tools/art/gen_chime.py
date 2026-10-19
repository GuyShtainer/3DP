"""Generate banner.wav: a short three-note FM chime (44.1 kHz, 16-bit stereo, ~1.9 s, peak -4 dBFS).

Usage: python3 tools/art/gen_chime.py [output_dir]   (default: repo root)
"""
import math
import sys
import wave
from pathlib import Path

RATE = 44100
DURATION = 1.9
PEAK_DBFS = -4.0
FADE_IN = 0.004
FADE_OUT = 0.15
# (start seconds, frequency Hz, pan -1..1): an ascending major triad, E5 A5 C#6.
NOTES = [(0.00, 659.26, -0.25), (0.18, 880.00, 0.0), (0.36, 1108.73, 0.25)]


def note_sample(t: float, freq: float) -> float:
    """One FM bell voice: sine carrier phase-modulated at 2:1, index decaying with time."""
    if t < 0.0:
        return 0.0
    amp = math.exp(-t * 3.2)
    index = 2.2 * math.exp(-t * 6.0)
    phase = 2.0 * math.pi * freq * t
    return amp * math.sin(phase + index * math.sin(phase * 2.0))


def render() -> bytes:
    """Return interleaved little-endian int16 stereo PCM."""
    frames = int(RATE * DURATION)
    left = [0.0] * frames
    right = [0.0] * frames
    for start, freq, pan in NOTES:
        gain_l, gain_r = 0.5 * (1.0 - pan), 0.5 * (1.0 + pan)
        for i in range(frames):
            value = note_sample(i / RATE - start, freq)
            left[i] += value * gain_l
            right[i] += value * gain_r
    for i in range(frames):
        t = i / RATE
        env = min(1.0, t / FADE_IN) * min(1.0, (DURATION - t) / FADE_OUT)
        left[i] *= env
        right[i] *= env
    peak = max(max(abs(v) for v in left), max(abs(v) for v in right))
    scale = (10.0 ** (PEAK_DBFS / 20.0)) * 32767.0 / peak
    out = bytearray()
    for l_val, r_val in zip(left, right):
        out += int(round(l_val * scale)).to_bytes(2, "little", signed=True)
        out += int(round(r_val * scale)).to_bytes(2, "little", signed=True)
    return bytes(out)


def main() -> None:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    with wave.open(str(out_dir / "banner.wav"), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(render())


if __name__ == "__main__":
    main()
