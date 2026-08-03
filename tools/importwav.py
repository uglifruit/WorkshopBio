#!/usr/bin/env python3
"""importwav.py — turn recordings into the .raw one-shots the card bakes in.

Drop WAV files into samples/incoming/ named for their mode and variant, then:

    python tools/importwav.py

It reads samples/incoming/*.wav, converts each to 8-bit signed mono 48kHz,
trims silence, normalises, applies a short fade-out, and writes samples/*.raw
ready for the build. Uses only the Python standard library — no ffmpeg needed.

NAMING
    horses_1.wav  horses_2.wav  horses_3.wav  horses_4.wav
    geese_1.wav   frogs_1.wav   rain_1.wav    meteors_1.wav   cicadas_1.wav

The number is the round-robin variant, and in HORSES it is the hoof:
    1 = left hind   2 = left fore   3 = right hind   4 = right fore
Hind hooves strike lower and heavier than fores on a real animal, so putting
them in the right slots is most of what makes a gait sound like an animal.
Elsewhere the four are just different individuals.

Fewer than four is fine — missing variants reuse the ones you did supply, and a
bare `horses.wav` with no number is used for all four.

WHAT MAKES A GOOD SOURCE
  * A single isolated hit. No reverb tail you cannot trim, no other animals.
  * Trimmed tight to the transient; the attack is what identifies the sound.
  * Dry. The card has no reverb, so recorded ambience is baked in forever.
  * Mono or stereo (stereo is summed), any sample rate, 8/16/24/32-bit or float.

LENGTH GUIDE (the card holds ~41 seconds of audio in total)
    horses   ~180ms    rain     ~120ms
    geese    ~400ms    meteors  ~700ms
    frogs    ~300ms    cicadas  ~200ms
Longer is allowed; you just spend more flash. The script warns past 1 second.

Options:
    --keep-level   skip normalisation (use if you tuned relative levels yourself)
    --no-trim      skip silence trimming
"""
import array
import os
import struct
import sys
import wave

SR = 48000
IN_DIR = os.path.join("samples", "incoming")
OUT_DIR = "samples"
MODES = ["horses", "geese", "frogs", "rain", "meteors", "cicadas"]
WARN_MS = 1000


def decode(raw, width, nch):
    """Bytes -> list of float -1..1, summed to mono. Handles 8/16/24/32-bit
    PCM and 32-bit float. Written out longhand because the stdlib `audioop`
    module was removed in Python 3.13."""
    if width == 1:
        # WAV 8-bit is UNSIGNED, unlike every other depth.
        chans = [(b - 128) / 128.0 for b in raw]
    elif width == 3:
        n = len(raw) // 3
        chans = []
        for i in range(n):
            b0, b1, b2 = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
            v = b0 | (b1 << 8) | (b2 << 16)
            if v & 0x800000:
                v -= 0x1000000
            chans.append(v / 8388608.0)
    elif width == 2:
        a = array.array("h")
        a.frombytes(raw[:len(raw) // 2 * 2])
        chans = [v / 32768.0 for v in a]
    elif width == 4:
        a = array.array("i")
        a.frombytes(raw[:len(raw) // 4 * 4])
        chans = [v / 2147483648.0 for v in a]
    else:
        raise ValueError(f"unsupported sample width {width}")

    if nch <= 1:
        return chans
    # Sum channels to mono.
    return [sum(chans[i:i + nch]) / nch for i in range(0, len(chans) - nch + 1, nch)]


def decode_float32(raw, nch):
    n = len(raw) // 4
    chans = list(struct.unpack("<%df" % n, raw[:n * 4]))
    if nch <= 1:
        return chans
    return [sum(chans[i:i + nch]) / nch for i in range(0, len(chans) - nch + 1, nch)]


def resample(data, src_rate):
    """Linear resample to SR. Fine for one-shots; these are short and we are
    heading for 8-bit anyway."""
    if src_rate == SR or not data:
        return data
    ratio = src_rate / float(SR)
    n = int(len(data) / ratio)
    out = []
    for i in range(n):
        x = i * ratio
        j = int(x)
        f = x - j
        a = data[j]
        b = data[j + 1] if j + 1 < len(data) else 0.0
        out.append(a + (b - a) * f)
    return out


def read_wav(path):
    """Return (samples as list of float -1..1 at SR, source description)."""
    with wave.open(path, "rb") as w:
        nch, width, rate, nframes = (w.getnchannels(), w.getsampwidth(),
                                     w.getframerate(), w.getnframes())
        comp = w.getcomptype()
        raw = w.readframes(nframes)

    kind = "float" if comp == "FLOA" else f"{width * 8}-bit"
    desc = f"{rate}Hz {kind} {'stereo' if nch == 2 else 'mono'}"

    if comp == "FLOA" or (comp not in ("NONE", "PCM ") and width == 4):
        data = decode_float32(raw, nch)
    else:
        data = decode(raw, width, nch)

    return resample(data, rate), desc


def trim(data, floor=0.004):
    """Drop leading and trailing near-silence, keeping a tiny pre-roll."""
    start = 0
    while start < len(data) and abs(data[start]) < floor:
        start += 1
    end = len(data)
    while end > start and abs(data[end - 1]) < floor:
        end -= 1
    start = max(0, start - 48)          # 1ms of pre-roll so attacks stay intact
    return data[start:end] if end > start else data


def normalise(data, target=0.92):
    peak = max((abs(v) for v in data), default=0.0)
    if peak < 1e-6:
        return data
    g = target / peak
    return [v * g for v in data]


def fade_out(data, ms=4):
    n = min(int(SR * ms / 1000), len(data))
    for i in range(n):
        data[len(data) - n + i] *= 1.0 - (i / n)
    return data


def write_raw(name, data):
    out = bytearray()
    for v in data:
        s = int(round(v * 127))
        out.append((max(-128, min(127, s))) & 0xFF)
    path = os.path.join(OUT_DIR, f"{name}.raw")
    with open(path, "wb") as f:
        f.write(out)
    return path, len(out)


def main():
    keep_level = "--keep-level" in sys.argv
    no_trim = "--no-trim" in sys.argv

    if not os.path.isdir(IN_DIR):
        os.makedirs(IN_DIR, exist_ok=True)
        print(f"Created {IN_DIR}/ — put your .wav files there and re-run.")
        print(__doc__)
        return

    wavs = sorted(f for f in os.listdir(IN_DIR) if f.lower().endswith(".wav"))
    if not wavs:
        print(f"No .wav files in {IN_DIR}/")
        print(__doc__)
        return

    os.makedirs(OUT_DIR, exist_ok=True)
    total = 0
    unknown = []
    for fn in wavs:
        stem = os.path.splitext(fn)[0].lower()
        mode = stem.split("_")[0]
        if mode not in MODES:
            unknown.append(fn)
            continue

        data, desc = read_wav(os.path.join(IN_DIR, fn))
        if not no_trim:
            data = trim(data)
        if not keep_level:
            data = normalise(data)
        data = fade_out(data)

        path, n = write_raw(stem, data)
        total += n
        ms = n / 48.0
        flag = "  <-- long, consider trimming" if ms > WARN_MS else ""
        print(f"  {fn:<22} {desc:<22} -> {os.path.basename(path):<16} "
              f"{ms:5.0f}ms{flag}")

    for fn in unknown:
        print(f"  {fn}: SKIPPED — name must start with one of {MODES}")

    print(f"\nWrote {total} bytes ({total / 48000.0:.2f}s). "
          f"Flash holds about 41s in total.")
    print("Now rebuild: cmake --build build")


if __name__ == "__main__":
    main()
