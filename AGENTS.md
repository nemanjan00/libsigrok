# AGENTS.md

Notes for people and coding agents working on this fork of libsigrok.

## What this fork is

`nemanjan00/libsigrok` `master` is **stock upstream libsigrok plus out-of-tree
driver work merged on top**. Nothing upstream is rewritten. Every addition
comes in as a merge, so each source can be followed in `git log --graph`.

| Source | What it adds | How it got here |
|---|---|---|
| [sigrokproject/libsigrok](https://github.com/sigrokproject/libsigrok) `master` | upstream | merged |
| [christianeisendle/libsigrok](https://github.com/christianeisendle/libsigrok) `master` | DreamSourceLab DSLogic: U3Pro16 support, worker thread for sample processing, optimized/fixed deinterleaving, USB error handling, buffered mode kept, continuous mode by default (16 commits) | merged |
| nemanjan00 (`0fab8d6e`) | DSLogic U3Pro32 profile (`2a0e:002c`) | own commit |
| [sipeed/libsigrok](https://github.com/sipeed/libsigrok/tree/slogic-dev) `slogic-dev` | Sipeed SLogic analyzers (16U3, 32U3, Combo 8) on the shared `libslogic` core, udev rules, `std_i32_idx()` | merged |
| branch `saleae-logic-pro8` (`614918c6`) | Saleae Logic Pro 8 support with the current (Logic 2) protocol, firmware and bitstream | merged |

## Remotes

```sh
git remote add origin    https://github.com/sigrokproject/libsigrok.git
git remote add fork      git@github.com-libsigrok:nemanjan00/libsigrok.git   # push here (deploy key)
git remote add christian https://github.com/christianeisendle/libsigrok.git
git remote add sipeed    https://github.com/sipeed/libsigrok.git
```

`fork` uses the per-project deploy key `~/.ssh/libsigrok_deploy` through the
SSH alias `github.com-libsigrok`.

## Syncing master

Merge; never rebase or force-push `master`:

```sh
git fetch origin && git fetch christian && git fetch sipeed slogic-dev
git checkout master                      # tracks fork/master
git merge origin/master
git merge christian/master
git merge sipeed/slogic-dev
git push fork master
```

After a sync, run a full build with every driver (see below). The sources
touch shared files (`configure.ac`, `src/std.c`, `src/libsigrok-internal.h`,
`contrib/60-libsigrok.rules`), so check that region of a merge carefully.

## Building

```sh
./autogen.sh
mkdir build && cd build
../configure --prefix=$HOME/sigrok/install --disable-bindings
make -j$(nproc) && make install
LD_LIBRARY_PATH=$HOME/sigrok/install/lib sigrok-cli -L | grep -E 'dslogic|saleae-logic-pro|sipeed'
```

To iterate on one driver, use `--disable-all-drivers --enable-<driver>`.
A full build currently has 8 warnings, all in untouched upstream files
(asix-sigma, devantech-eth008, scpi-dmm, scpi-pps, output/bits, hex, vcd).

## Commit conventions

- Use the libsigrok subject style: `<driver>: <imperative summary>`, e.g.
  `saleae-logic-pro: Add Logic Pro 8 support`.
- No AI attribution in commits (no `Co-Authored-By` or "Generated with" lines).
- Small fixes may go straight to `master`. Larger work goes on a topic branch
  that's merged with `--no-ff`, so the merge graph stays readable.

## Saleae Logic Pro 8 (`saleae-logic-pro`)

Hardware: `21a9:1005`, revision 11, FX3 + Lattice ECP5. The legacy Logic 1.x
Pro 8 bitstream is for Xilinx and does **not** work on this hardware.

Firmware files (in `~/.local/share/sigrok-firmware/` or another firmware dir),
both extracted from Saleae Logic 2 (2.4.46, `libgraph_server_shared.so`):

- `saleae-logicpro8-logic2-fx3.fw`: the FX3 boot image at file offset
  `0x79a7f0`, converted to the driver's chunk format
  (`u16 len, u32 addr, u8 final` + data, ≤4 KiB, final record = entry point).
- `saleae-logicpro8-logic2-fpga.bitstream`: the Lattice bitstream
  `Logic_Pro_Top_ADS528x_lse_impl1` (arch `sa5p00`) at offset `0x16f34c0`.
  It's byte-identical to what Logic 2 uploads.

The protocol is reverse engineered from usbmon traces of Logic 2 and from
static analysis of Logic 2 and the FX3 firmware. The full write-up is in
[`src/hardware/saleae-logic-pro/PROTOCOL.md`](src/hardware/saleae-logic-pro/PROTOCOL.md).
Key facts:

- **The FX3 firmware is RAM-only.** At power-up the device is the
  "Cypress WestBridge" bootloader on USB 2. The driver uploads firmware with
  vendor request `0xA0`, then the device re-enumerates on SuperSpeed.
- **Command channel:** EP1, LFSR-encrypted. Reseed packet:
  `seed XOR 0x354b248e`. Samples come on EP2.
- **The firmware only accepts these commands:**
  `0x01 0x02 0x06 0x07 0x0c 0x7c-0x7f 0x80 0x81 0x86-0x89 0x8b 0x8c`.
  **Anything else, or a command decrypted with the wrong LFSR state, puts it
  in a fatal LED-blink loop.** Only removing power recovers it (no USB reset,
  no watchdog). The driver therefore reseeds after any failed transaction.
  **Never send guessed commands to real hardware**: replay what Logic 2
  sends, or work out the command from the firmware first.
- **The capture register block** is copied exactly from Logic 2. Don't write
  `0x07`/`0x09` on the Pro 8, and do write `0x1a`/`0x1b`; otherwise the stream
  runs at 1/8 of the rate.
  - Digital divider: `500 MHz / (5·n)`, or 500 MHz with `n = 0`.
  - USB throttle (`0x14`): `min(255, 100e6 / (rate·nch/32))`.
- **Status register `0x40`:** `0x20` means OK; bit 7 is set in normal
  operation (`0xa0`); bit 0 means overrun. The stream sustains 1 Gbit/s;
  2 Gbit/s overruns.
- **There's no hardware trigger.** Saleae triggers in software; the driver
  uses `soft_trigger_logic`.
- **The data endpoint can stay halted** after a killed session. The driver
  clears the halt on EP1/EP2 at open and at acquisition start.

Verified on hardware: 10 kHz – 100 MHz on 8 channels, 500 MHz on 1–2
channels, sample/time limits, continuous mode, triggers with capture ratio,
a blind UART test decoded byte-exact, and recovery after a killed session.
The Logic Pro 16 path (legacy firmware) is untested.

## Testing gotchas (lab PC)

- Linux USB autosuspend on hubs can hide a replugged device. Fix it with
  `echo on > /sys/bus/usb/devices/<hub>/power/control`.
- Hub per-port power switching (`uhubctl`) doesn't actually cut VBUS on the
  powered hub; only a physical unplug resets a Logic Pro 8.
- `sigrok-cli --continuous` stops at once when stdin is `/dev/null`. Feed it
  (`sleep N | sigrok-cli ...`) and stop with SIGINT.
- The system `libsigrokdecode` on the lab PC is broken: every decoder fails
  with a `g_variant_get_type` assertion, even with the demo driver.
