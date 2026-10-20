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
| [fredzo/libsigrok](https://github.com/fredzo/libsigrok/tree/rigol-dho-support) `rigol-dho-support` ([PR #246](https://github.com/sigrokproject/libsigrok/pull/246)) | Rigol DHO800/900/1000/4000 in `rigol-ds` (`PROTOCOL_V6`) | via `rigol-mho900` |
| branch `rigol-mho900` | Rigol MHO98 on top of PR #246, DHO/MHO memory samplerate fix, waveform-source check | merged |
| nemanjan00 (`0e6ef292`) | fork only: `NK Technologies` IDN accepted as Rigol (the lab's MHO98) | own commit |
| branch `dslogic-u3pro32` | DSLogic U3Pro32 support (32 channels, `2a0e:002c`), U3 threshold fix, samplerate/channel check, U3 udev rules | merged |
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
## Branches: kept upstreamable

Every feature lives on a **topic branch based on upstream `master`**, so it
can be sent upstream as is. `master` is only where the branches are combined.

- Branch from `origin/master`, not from this fork's `master`:
  `git checkout -b <topic> origin/master`.
- Only upstreamable changes go on a topic branch: driver code and its docs
  (e.g. `src/hardware/<driver>/PROTOCOL.md`). Fork-only files such as this
  `AGENTS.md` stay on `master`.
- Commit to the topic branch first, push it, then merge it into `master` with
  `--no-ff` and push `master`. Both must always carry the same driver
  changes.
- To refresh a topic branch for upstreaming, rebase it on the latest
  `origin/master` (topic branches may be force-pushed; `master` never).
- Current topic branches: `saleae-logic-pro8` (Saleae Logic Pro 8) and
  `dslogic-u3pro32` (DSLogic U3Pro32) and `rigol-mho900` (Rigol MHO98).
  The SLogic work and the rest of the
  DSLogic work are maintained upstream-style in their own repos
  (`christian/*`, `sipeed/slogic-dev`) and are merged from there.
- Exception: `dslogic-u3pro32` is based on `christian/master`, not
  `origin/master`, because the U3 (DS_API_V2) code it extends only exists
  there. Upstream it together with or after that work. Likewise
  `rigol-mho900` is based on `fredzo/rigol-dho-support` (PR #246).

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
- **Capture registers** are written exactly the way Logic 2 writes them.
  Digital rate: 8-bit divider `0x0b` (500 MHz / (5·n)), or `0x1a` = power-of-two
  decimation for 500/250/125 MS/s. `0x1a` must always be written (its
  default divides by 8). USB throttle (`0x14`):
  `ceil(100e6 / (bits_per_s / 32))`.
- **Analog (A0–A7)** shares the samplerate with digital, at the calibrated
  rates (50 MHz / 1, 4, 8, 16, 32, 64). The stream frames are digital words,
  then 3-word groups of 8 × 12-bit two's complement samples per channel.
  Volts and frequency-response filters come from Logic 2's per-device
  calibration file (`~/.config/Logic/calibrations/<id>-*.cal`), or a copy
  named `saleae-logic-pro-<id>.cal` in a firmware dir; without one, nominal
  ±10.5 V and no filtering.
- **Status register `0x40`:** `0x20` means OK; bit 7 is set in normal
  operation (`0xa0`); bit 0 means overrun. The stream sustains 1 Gbit/s; the
  device FIFO overruns within ~0.1 s if no reads are queued, so the driver
  resubmits before converting and queues ~0.4 s of transfers.
- **There's no hardware trigger.** Saleae triggers in software; the driver
  uses `soft_trigger_logic` (no pre-trigger data when analog is enabled).
- **The data endpoint can stay halted** after a killed session. The driver
  clears the halt on EP1/EP2 at open and at acquisition start.
- **sigrok-cli output caveats with analog:** the `csv` output module
  overflows its heap when analog packet sizes vary (an upstream bug, with
  any driver that doesn't enable every channel), and `-O binary` sends
  analog through the slow text formatter. Use `-o file.sr` or a
  libsigrok consumer.

Verified on hardware: every digital rate 1–500 MS/s (125 and 250 MS/s
included), analog up to 50 MS/s, mixed 8 digital + 4 analog at 12.5 MS/s,
all 8 analog at 6.25 MS/s, a UART decoded byte-exact from both digital and
analog channels, voltages matching Logic 2 within 10 mV, triggers, and
recovery after a killed session.
The Logic Pro 16 path (legacy firmware) is untested.

## DSLogic U3Pro32 (`dreamsourcelab-dslogic`)

Hardware: `2a0e:002c`, FX3 (firmware in flash, no upload) + FPGA, USB 3.
Built from DSView's `libsigrok4DSL/hardware/DSL/` (`dsl.h`, `dsl.c`).

- **FPGA bitstream:** DSView's `DSLogicU3Pro32.bin`
  (`/usr/share/DSView/res/`), installed as
  `dreamsourcelab-dslogic-u3pro32-fpga.fw` in a firmware dir. It differs
  from the U3Pro16 one.
- **32 channels** come from `DSLOGIC_CAPS_LA_CH32` (same bit as DSView's
  `CAPS_FEATURE_LA_CH32`). `ch_en` is 32 bits; logic packets use
  unitsize 4 (16-channel devices keep 2).
- **Second config block:** after the main FPGA config, the 32-channel
  FPGA wants a separate bulk transfer with the trigger masks of channels
  16-31 (`sync`, header `_DS_CFG(96, 96)` = `0x6060`, six 16-stage
  arrays, `0xffff` padding, end sync). `DSL_CTL_BULK_WR` still announces
  only the main block's length, like DSView.
- **Rate limits** (DSView channel modes, checked at acquisition start):
  buffered 8 ch 1 GHz / 16 ch 500 MHz / 32 ch 250 MHz; continuous on USB 3
  3 ch 1 GHz / 6 ch 500 MHz / 12 ch 250 MHz / 30 ch 100 MHz /
  32 ch 50 MHz; continuous on USB 2 is 10-100 MHz.
- **Threshold:** the U3 series scales the DAC as `vth / 3.3 * 1.5 / 2.5`,
  not `vth / 5` (DSView).
- **Pre-trigger:** if the trigger condition is already true when the
  capture is armed (e.g. a free-running clock), the FPGA fires at once and
  there is almost no pre-trigger data. That's the hardware, not a bug.
- **Extra pins** (datasheet `DSLogic_U3Pro32_Datasheet.pdf`): `CK` is the
  external clock (state mode, max 50 MHz), `TI` is "reserved" (no function,
  DSView doesn't use it either), `TO` pulses once per capture, at the trigger
  or at the start when there's none, with no configuration. Nothing to
  implement for `TI`/`TO`.
- **Transfers can end mid-block** (a 64-sample block is 8 bytes x enabled
  channels): e.g. with an external clock slower than the samplerate,
  transfers time out part-filled. The worker carries the cut-off bytes over
  to the next transfer; it used to abort with "Invalid transfer length!".

Verified on hardware (2026-10-20) with a Glasgow revC3 as the signal
source: channel mapping on 0-2, 4-8 and 28-31, a 6 MHz clock measured in every
mode above, triggers on low and high channels (incl. pre-trigger at
10/50/90 %), external clock on the `C` pin, 30 ch @ 100 MHz streamed for
2 s without loss, and the threshold at 1.8 V. The U3Pro16 path was not
re-tested (no hardware).

Later the same day, with `~/sigrok/la-bench` (Glasgow pattern applet +
`la_check.py`): count/gray/prbs/walk patterns at up to 24 MHz step rate
checked sample by sample (0 errors, buffered and continuous, 12 and 32
channels), external clock at 1/12/24 MHz in both modes, and `TO` edges
counted on the Glasgow.

Bench wiring: Glasgow A0-A2 -> ch 0-2, A3-A7 -> ch 4-8, B4/B5/B6/B7 ->
ch 29/28/30/31, B0/B1/B2 -> DSLogic `O`/`I`/`C`.

## Rigol MHO98 (`rigol-ds`)

LAN only so far: `rigol-ds:conn=tcp-raw/<ip>/5555`. The MHO900 series
uses the DHO dialect (`PROTOCOL_V6`, PR #246). MHO98: 4 analog channels,
16 digital with the logic probe, 1 mV/div minimum, 200 ps/div to
500 s/div.

- **Memory/segmented samplerate** comes from `:ACQ:SRAT?`. `WAV:XINC?`
  follows `WAV:MODE`, which is still `NORM` when the rate is read, and
  gives the screen point spacing (1000x off at 1 Mpts).
- **`WAV:SOUR` is read back** on V6: config writes skip `*OPC?`, so a
  refused source (a channel that's switched off, or D0-D15 without the
  logic probe) used to be read as the previous source, e.g. CH1's samples
  delivered as D0.
- **Memory mode sends `:STOP`** (PR #246), so the scope stays stopped
  afterwards; press Run or send `:RUN`.
- The driver turns off analog channels that aren't selected, on the scope
  too (e.g. `-C CH1` hides CH2).
- "Read should have been completed" is logged after every block on
  `tcp-raw`: that transport treats a full 1-byte read (the trailing
  newline) as "more may follow". Harmless, all `rigol-ds` models.
- Use `-o file.sr` rather than `-O csv`: the csv output module overflows
  on the variable block sizes (see the Logic Pro 8 notes).

Verified (2026-10-20, firmware 00.01.03, CH1 on the probe-comp output,
CH2 on the built-in generator G1, 1 kHz): live (1000 pts/frame,
multi-frame) and memory captures of 10 kpts and 1 Mpts with the right
amplitude, frequency and samplerate; timebase, vdiv, coupling, probe
factor and trigger get/set. Digital channels are untested (no logic
probe); segmented mode is untested.

## Testing gotchas (lab PC)

- Linux USB autosuspend on hubs can hide a replugged device. Fix it with
  `echo on > /sys/bus/usb/devices/<hub>/power/control`.
- Hub per-port power switching (`uhubctl`) doesn't actually cut VBUS on the
  powered hub; only a physical unplug resets a Logic Pro 8.
- `sigrok-cli --continuous` stops at once when stdin is `/dev/null`. Feed it
  (`sleep N | sigrok-cli ...`) and stop with SIGINT.
- The system `libsigrokdecode` on the lab PC is broken: every decoder fails
  with a `g_variant_get_type` assertion, even with the demo driver.
