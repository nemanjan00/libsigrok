#!/usr/bin/env python3
"""Check a sigrok logic capture of a generate-la-pattern signal.

Rebuilds the pattern value of every sample from the mapped channels, then
verifies that each new value is the pattern's next state. Reports dropped or
extra steps, the measured step rate and the samples per step.

    la-check capture.sr --mode count --rate 1e6
    la-check capture.sr --mode gray --rate 2e6 --map 0,1,2,4,5,6,7,8,29,28,30,31
    la-check capture.sr --mode count --clocked --map 0,1,2,4,5,6,7,8,29,28,30,31

--clocked is for captures sampled on an external clock (state mode): every
sample must be one step. Drive the analyzer's clock input with pattern bit 0
of a count pattern; the data pins then carry bits 1 and up, which count up by
one on every rising edge of the clock.
"""

import argparse
import configparser
import re
import sys
import zipfile

import numpy as np

from la_patterns import PRBS_TAPS
UNITS = {"Hz": 1, "kHz": 1e3, "MHz": 1e6, "GHz": 1e9}


def load_sr(path):
    """Return (samples as uint array, samplerate, {bit: channel name})."""
    with zipfile.ZipFile(path) as z:
        meta = configparser.ConfigParser()
        meta.read_string(z.read("metadata").decode())
        dev = meta["device 1"]
        unitsize = int(dev["unitsize"])
        m = re.match(r"([\d.]+)\s*(\w+)", dev["samplerate"])
        rate = float(m.group(1)) * UNITS[m.group(2)]
        names = {int(k[5:]) - 1: v for k, v in dev.items() if re.fullmatch(r"probe\d+", k)}
        base = dev.get("capturefile", "logic-1")
        parts = sorted((int(n.rsplit("-", 1)[1]), n) for n in z.namelist()
                       if re.fullmatch(re.escape(base) + r"-\d+", n))
        raw = b"".join(z.read(n) for _, n in parts)
    dtype = {1: np.uint8, 2: np.uint16, 4: np.uint32, 8: np.uint64}[unitsize]
    return np.frombuffer(raw, dtype=dtype).astype(np.uint64), rate, names


def auto_map(data):
    """Map pattern bits to channels by toggle count (count/gray modes)."""
    toggles = []
    changes = np.bitwise_xor(data[1:], data[:-1])
    for ch in range(64):
        n = int(np.count_nonzero((changes >> np.uint64(ch)) & np.uint64(1)))
        if n:
            toggles.append((n, ch))
    toggles.sort(reverse=True)
    return [ch for _, ch in toggles]


def pattern_values(data, chmap):
    v = np.zeros(len(data), dtype=np.uint64)
    for bit, ch in enumerate(chmap):
        v |= ((data >> np.uint64(ch)) & np.uint64(1)) << np.uint64(bit)
    return v


def to_gray(b):
    return b ^ (b >> 1)


def from_gray(g):
    b = g.copy()
    shift = 1
    while shift < 64:
        b ^= b >> np.uint64(shift)
        shift <<= 1
    return b


def expected_next(vals, mode, width):
    """Next state of every value, and the mask of bits that can be checked."""
    full = (1 << width) - 1
    mask = np.uint64(full)
    if mode == "count":
        return (vals + np.uint64(1)) & mask, full
    if mode == "gray":
        return to_gray((from_gray(vals) + np.uint64(1)) & mask) & mask, full
    if mode == "walk":
        return ((vals << np.uint64(1)) | (vals >> np.uint64(width - 1))) & mask, full
    if mode == "prbs":
        taps = np.uint64(PRBS_TAPS[width])
        nxt = (vals >> np.uint64(1)) ^ np.where(vals & np.uint64(1), taps, np.uint64(0))
        return nxt & mask, full
    raise ValueError(mode)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("capture", help="sigrok session file (.sr)")
    p.add_argument("-m", "--mode", choices=["count", "gray", "prbs", "walk", "static"],
                   default="count")
    p.add_argument("-r", "--rate", type=float, help="step rate in Hz (not with --clocked)")
    p.add_argument("--clocked", action="store_true",
                   help="external clock capture: every sample is one step")
    p.add_argument("--map", help="channel of each pattern bit, comma separated "
                   "(default: from toggle counts, count/gray mode only)")
    p.add_argument("--width", type=int, help="number of pattern bits (default: map length)")
    p.add_argument("--value", type=lambda x: int(x, 0), default=0, help="static mode value")
    p.add_argument("--glitch", type=float,
                   help="runs shorter than this fraction of a step are edge glitches "
                        "from channel skew and are dropped (default: 0.25; 0 in gray "
                        "mode, where one bit changes per step and every sample counts)")
    p.add_argument("--show", type=int, default=5, help="errors to print (default: %(default)s)")
    args = p.parse_args()

    if not args.clocked and not args.rate:
        p.error("--rate is required unless --clocked")
    data, srate, names = load_sr(args.capture)
    if args.map:
        chmap = [int(c) for c in args.map.split(",")]
    elif args.mode in ("count", "gray"):
        chmap = auto_map(data)
        if args.width:
            chmap = chmap[:args.width]
    else:
        sys.exit("--map is needed for prbs/walk/static mode (get it from a count capture)")
    width = args.width or len(chmap)

    print(f"capture:    {len(data)} samples at {srate / 1e6:g} MHz "
          f"({len(data) / srate * 1e3:.3f} ms)")
    print(f"map:        " + ",".join(str(c) for c in chmap) +
          "   (pattern bit -> channel" +
          (", names " + ",".join(names.get(c, str(c)) for c in chmap) if names else "") + ")")
    v = pattern_values(data, chmap)
    if args.clocked:
        nxt, check = expected_next(v[:-1], args.mode, width)
        bad = np.flatnonzero((nxt ^ v[1:]) & np.uint64(check))
        print(f"clocked:    {len(v) - 1} steps, errors: {len(bad)}")
        for i in bad[:args.show]:
            print(f"  error at sample {i + 1}: 0x{int(v[i]):x} -> 0x{int(v[i + 1]):x}, "
                  f"expected 0x{int(nxt[i]):x}")
        return 1 if len(bad) else 0

    spp = srate / args.rate
    print(f"expected:   {spp:.3f} samples per step")

    if args.mode == "static":
        bad = np.count_nonzero(v != np.uint64(args.value))
        print(f"static:     {bad} samples differ from 0x{args.value:x}")
        return 1 if bad else 0

    starts = np.concatenate(([0], np.flatnonzero(np.diff(v)) + 1))
    lengths = np.diff(np.concatenate((starts, [len(v)])))
    vals = v[starts]

    # Channel skew makes a multi-bit change pass through intermediate values
    # for a sample or two; drop those runs and merge what's left.
    if args.glitch is None:
        args.glitch = 0 if args.mode == "gray" else 0.25
    glitch_len = args.glitch * spp
    keep = lengths >= glitch_len
    keep[0] = keep[-1] = True
    glitches = int(np.count_nonzero(~keep))
    starts, vals = starts[keep], vals[keep]
    merged = np.concatenate(([True], vals[1:] != vals[:-1]))
    starts, vals = starts[merged], vals[merged]
    lengths = np.diff(np.concatenate((starts, [len(v)])))

    if len(vals) < 3:
        print("too few steps in the capture")
        return 1
    if spp < 2:
        print("warning: under 2 samples per step, the pattern is aliased; "
              "sample faster or lower the step rate")

    nxt, check = expected_next(vals[:-1], args.mode, width)
    bad = np.flatnonzero((nxt ^ vals[1:]) & np.uint64(check))
    steps = len(vals) - 1
    span = (starts[-1] - starts[1]) / srate      # first to last full transition
    measured = (steps - 1) / span if span > 0 else 0
    inner = lengths[1:-1]

    print(f"steps:      {steps}, errors: {len(bad)}, glitch runs dropped: {glitches}"
          + ("" if check == (1 << width) - 1 else
             f"   (checking {bin(check).count('1')} of {width} bits)"))
    print(f"step rate:  {measured / 1e6:.6f} MHz measured, {args.rate / 1e6:.6f} MHz expected "
          f"({(measured / args.rate - 1) * 1e6:+.1f} ppm)")
    if len(inner):
        print(f"run length: min {inner.min()} max {inner.max()} mean {inner.mean():.3f} samples")
    for i in bad[:args.show]:
        print(f"  error at sample {starts[i + 1]}: 0x{int(vals[i]):x} -> 0x{int(vals[i + 1]):x}, "
              f"expected 0x{int(nxt[i]):x}")
    return 1 if len(bad) else 0


if __name__ == "__main__":
    sys.exit(main())
