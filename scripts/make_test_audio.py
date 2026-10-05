#!/usr/bin/env python3
"""Generates synthetic WAV fixtures (PCM mono 16-bit 16 kHz) for manually
exercising examples/transcribe.cpp (Phase 6) or for listening to.

Pure standard library (wave, struct, math, random): no numpy, no ffmpeg.
This is NOT a build dependency -- benchmarks/streaming/streaming_bench.cpp
synthesizes its own audio in-process so the benchmark never depends on
this script having been run first. Run it yourself when you want files
to look at or feed through sasr_wav_info / a future transcribe tool:

    python3 scripts/make_test_audio.py [output_dir]   # default: build/testdata
"""
import math
import os
import random
import struct
import sys
import wave

SAMPLE_RATE = 16000


def write_wav(path, samples, sample_rate=SAMPLE_RATE):
    """samples: a list/sequence of int16 values, mono."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    print(f"wrote {path} ({len(samples) / sample_rate:.2f} s)")


def clamp_i16(x):
    return max(-32768, min(32767, int(x)))


def silence(seconds, sample_rate=SAMPLE_RATE):
    return [0] * int(seconds * sample_rate)


def tone(seconds, amplitude=0.3, freq=440.0, sample_rate=SAMPLE_RATE):
    n = int(seconds * sample_rate)
    scale = amplitude * 32767.0
    return [
        clamp_i16(scale * math.sin(2 * math.pi * freq * i / sample_rate))
        for i in range(n)
    ]


def noise(seconds, amplitude=0.3, rng=None, sample_rate=SAMPLE_RATE):
    rng = rng or random.Random(0)
    n = int(seconds * sample_rate)
    scale = amplitude * 32767.0
    return [clamp_i16(rng.uniform(-scale, scale)) for _ in range(n)]


def rms(samples):
    if not samples:
        return 0.0
    return math.sqrt(sum(float(s) * s for s in samples) / len(samples))


def mix_at_snr_db(signal, snr_db, rng=None):
    """Adds white noise to `signal` scaled so the result has the given
    signal-to-noise ratio in dB."""
    signal_rms = rms(signal)
    noise_rms_target = signal_rms / (10.0 ** (snr_db / 20.0))
    # Uniform noise in [-a, a] has rms = a / sqrt(3).
    amplitude = noise_rms_target * math.sqrt(3.0)
    rng = rng or random.Random(0)
    return [clamp_i16(s + rng.uniform(-amplitude, amplitude)) for s in signal]


def speech_with_pauses(speech_s, pause_s, cycles, amplitude=0.3):
    out = []
    for _ in range(cycles):
        out += tone(speech_s, amplitude=amplitude)
        out += silence(pause_s)
    return out


def read_wav_i16_mono(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise ValueError(f"{path}: expected mono 16-bit PCM")
        n = w.getnframes()
        data = w.readframes(n)
        return list(struct.unpack(f"<{n}h", data)), w.getframerate()


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "testdata")

    write_wav(os.path.join(out_dir, "silence_5s.wav"), silence(5.0))
    write_wav(os.path.join(out_dir, "short_speech_1.5s.wav"), tone(1.5))
    write_wav(os.path.join(out_dir, "continuous_speech_5s.wav"), tone(5.0))
    write_wav(
        os.path.join(out_dir, "speech_short_pauses.wav"),
        speech_with_pauses(1.0, 0.3, cycles=4),
    )
    write_wav(
        os.path.join(out_dir, "speech_long_pauses.wav"),
        speech_with_pauses(1.0, 2.0, cycles=4),
    )

    clean = tone(5.0)
    for snr_db in (20, 10, 5):
        write_wav(
            os.path.join(out_dir, f"noisy_speech_snr{snr_db}db.wav"),
            mix_at_snr_db(clean, snr_db, rng=random.Random(snr_db)),
        )

    # Different sample rates, for exercising WavReader's rejection path
    # (it accepts only 16 kHz; there is no resampler yet -- see CLAUDE.md
    # roadmap, M6). These are synthesized directly at each rate, not
    # resampled from 16 kHz.
    for rate in (8000, 44100, 48000):
        write_wav(
            os.path.join(out_dir, f"wrong_sample_rate_{rate}.wav"),
            tone(1.0, sample_rate=rate),
            sample_rate=rate,
        )

    # Long utterance: repeat tests/data/en1.wav 4x if it's there, to force
    # multiple max_utterance_ms cutoffs with real speech-shaped audio.
    en1_path = os.path.join("tests", "data", "en1.wav")
    if os.path.exists(en1_path):
        samples, rate = read_wav_i16_mono(en1_path)
        if rate == SAMPLE_RATE:
            write_wav(os.path.join(out_dir, "long_utterance_en1x4.wav"), samples * 4)
        else:
            print(f"skipping long_utterance_en1x4.wav: {en1_path} is {rate} Hz, not {SAMPLE_RATE}")
    else:
        print(f"skipping long_utterance_en1x4.wav: {en1_path} not found")


if __name__ == "__main__":
    main()
