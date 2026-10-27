#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
"""
make.py — the audio fixtures test_audio_file.cpp decodes, and how they were made.

The signal is integer arithmetic (`sample` below), so the test computes the
same samples itself and compares a lossless decode exactly, with no reference
file. Lossy and companded formats get a reference decode from ffmpeg, raw
32-bit float, little-endian, interleaved (`.f32`), and the test compares
sample for sample within arithmetic precision: two decoders of the same
MP3 or Vorbis stream agree to the sample, and a fault in ours does not.

Run from this directory with ffmpeg, lame and flac on the path. The outputs
are committed; this script is their provenance.
"""
import os
import struct
import subprocess
import sys
import wave

OUT = os.path.dirname(os.path.abspath(__file__))


def sample(i: int, c: int) -> int:
    """A 16-bit sample: runs, a ramp and steps, different per channel, integer only."""
    n = i + c * 7919
    if n % 900 < 150:
        return 0
    if n % 900 < 500:
        return ((n % 900) - 150) * 46 - 8000
    return 13797 if n % 2 == 0 else -13797


def frames(count: int, channels: int):
    return [[sample(i, c) for c in range(channels)] for i in range(count)]


def write_wav(path, rate, channels, count, width, kind="pcm"):
    """kind: pcm (8/16/24/32-bit as `width` bytes), f32, f64."""
    data = bytearray()
    for frame in frames(count, channels):
        for v in frame:
            if kind == "f32":
                data += struct.pack("<f", v / 32768.0)
            elif kind == "f64":
                data += struct.pack("<d", v / 32768.0)
            elif width == 1:
                data += struct.pack("<B", (v >> 8) + 128)
            elif width == 2:
                data += struct.pack("<h", v)
            elif width == 3:
                data += struct.pack("<i", v << 8)[:3]
            else:
                data += struct.pack("<i", v << 16)
    if kind == "pcm":
        with wave.open(path, "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(width)
            w.setframerate(rate)
            w.writeframes(bytes(data))
        return
    # Float WAV by hand: wave.py writes PCM only.
    bits = 32 if kind == "f32" else 64
    fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * bits // 8,
                      channels * bits // 8, bits)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def ffmpeg(src, dst, *opts):
    run("ffmpeg", "-y", "-hide_banner", "-i", src, *opts, dst)


def reference(src, trim_to_stream=False):
    """ffmpeg's decode of `src`, as raw f32le, beside it.

    ffmpeg's Vorbis decode runs on past the stream's end (the last window's
    overlap, 518 frames here) where every other decoder stops at the granule
    position, so an Ogg reference is cut to the length ffprobe reports."""
    out = os.path.splitext(src)[0] + ".f32"
    if trim_to_stream:
        ts = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=duration_ts",
                             "-of", "csv=p=0", src], check=True, capture_output=True, text=True)
        ffmpeg(src, out, "-af", f"atrim=end_sample={int(ts.stdout.strip())}", "-f", "f32le", "-c:a", "pcm_f32le")
    else:
        ffmpeg(src, out, "-f", "f32le", "-c:a", "pcm_f32le")


def main():
    os.chdir(OUT)
    # Sources: short, so the fixtures stay small. Names say rate-channels.
    st = "src-44100-2.wav";  write_wav(st, 44100, 2, 4410, 2)    # 0.1 s stereo
    mono22 = "src-22050-1.wav"; write_wav(mono22, 22050, 1, 4410, 2)
    mono8 = "src-8000-1.wav";  write_wav(mono8, 8000, 1, 2000, 2)
    six = "src-48000-6.wav";   write_wav(six, 48000, 6, 2400, 2)
    hi = "src-96000-2.wav";    write_wav(hi, 96000, 2, 4800, 3)   # 24-bit

    # ── WAV, every encoding the header promises ──────────────────────────────
    os.rename(st, "wav-s16.wav")
    write_wav("wav-u8.wav", 44100, 2, 4410, 1)
    write_wav("wav-s24.wav", 44100, 2, 4410, 3)
    write_wav("wav-s32.wav", 44100, 2, 4410, 4)
    write_wav("wav-f32.wav", 44100, 2, 4410, 4, "f32")
    write_wav("wav-f64.wav", 44100, 2, 4410, 8, "f64")
    write_wav("wav-6ch.wav", 48000, 6, 2400, 2)               # plain fmt, 6 channels
    ffmpeg("wav-6ch.wav", "wav-6ch-extensible.wav", "-c:a", "pcm_s16le", "-ch_layout", "5.1")
    ffmpeg(mono8, "wav-alaw.wav", "-c:a", "pcm_alaw");      reference("wav-alaw.wav")
    ffmpeg(mono8, "wav-mulaw.wav", "-c:a", "pcm_mulaw");    reference("wav-mulaw.wav")
    ffmpeg("wav-s16.wav", "wav-ima-adpcm.wav", "-c:a", "adpcm_ima_wav"); reference("wav-ima-adpcm.wav")
    ffmpeg("wav-s16.wav", "wav-ms-adpcm.wav", "-c:a", "adpcm_ms");       reference("wav-ms-adpcm.wav")
    ffmpeg("wav-s16.wav", "rf64-s16.wav", "-c:a", "pcm_s16le", "-rf64", "always")
    ffmpeg("wav-s16.wav", "w64-s16.w64", "-c:a", "pcm_s16le", "-f", "w64")
    ffmpeg("wav-s24.wav", "w64-s24.w64", "-c:a", "pcm_s24le", "-f", "w64")

    # ── AIFF and AIFC ────────────────────────────────────────────────────────
    ffmpeg("wav-s16.wav", "aiff-s16.aiff", "-c:a", "pcm_s16be")
    ffmpeg("wav-s24.wav", "aiff-s24.aiff", "-c:a", "pcm_s24be")
    ffmpeg("wav-u8.wav", "aiff-s8.aiff", "-c:a", "pcm_s8")
    ffmpeg("wav-f32.wav", "aifc-f32.aiff", "-c:a", "pcm_f32be")
    ffmpeg("wav-6ch.wav", "aiff-6ch.aiff", "-c:a", "pcm_s16be")

    # ── FLAC ─────────────────────────────────────────────────────────────────
    run("flac", "--silent", "--force", "--best", "-o", "flac-s16.flac", "wav-s16.wav")
    run("flac", "--silent", "--force", "-o", "flac-s24-96k.flac", hi)
    run("flac", "--silent", "--force", "-o", "flac-mono.flac", mono22)
    run("flac", "--silent", "--force", "-o", "flac-6ch.flac", "wav-6ch-extensible.wav")
    run("flac", "--silent", "--force", "-o", "flac-s8.flac", "wav-u8.wav")

    # ── MP3: both MPEG versions and 2.5, CBR and VBR, with and without tags ──
    run("lame", "--silent", "--noreplaygain", "-b", "128", "wav-s16.wav", "mp3-cbr.mp3")
    run("lame", "--silent", "--noreplaygain", "-V", "5", "wav-s16.wav", "mp3-vbr.mp3")
    run("lame", "--silent", "--noreplaygain", "-t", "-b", "128", "wav-s16.wav", "mp3-noxing.mp3")
    run("lame", "--silent", "--noreplaygain", "-b", "64", mono22, "mp3-22050-mono.mp3")   # MPEG-2
    run("lame", "--silent", "--noreplaygain", "-b", "32", mono8, "mp3-8000-mono.mp3")     # MPEG-2.5
    run("lame", "--silent", "--noreplaygain", "-b", "128", "--add-id3v2", "--tt", "Fixture",
        "--ta", "clockwork", "wav-s16.wav", "mp3-id3v2.mp3")
    run("lame", "--silent", "--noreplaygain", "-b", "128", "--id3v1-only", "--tt", "Fixture",
        "wav-s16.wav", "mp3-id3v1.mp3")
    for f in ("mp3-cbr", "mp3-vbr", "mp3-noxing", "mp3-22050-mono", "mp3-8000-mono",
              "mp3-id3v2", "mp3-id3v1"):
        reference(f + ".mp3")

    # ── Ogg Vorbis ───────────────────────────────────────────────────────────
    ffmpeg("wav-s16.wav", "ogg-q3.ogg", "-c:a", "libvorbis", "-q:a", "3");  reference("ogg-q3.ogg", True)
    ffmpeg(mono22, "ogg-mono.ogg", "-c:a", "libvorbis", "-q:a", "1");       reference("ogg-mono.ogg", True)

    for s in (mono22, mono8, six, hi):
        os.remove(s)
    total = 0
    for name in sorted(os.listdir(OUT)):
        if name == os.path.basename(__file__):
            continue
        size = os.path.getsize(name)
        total += size
        print(f"{size:7d}  {name}")
    print(f"{total:7d}  total")


if __name__ == "__main__":
    sys.exit(main())
