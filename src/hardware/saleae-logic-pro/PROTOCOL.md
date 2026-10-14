# Saleae Logic Pro 8 — USB protocol (Logic 2 firmware)

Reverse-engineered from usbmon captures of Logic 2 2.4.46 and from
`libgraph_server_shared.so` (radare2 + r2ghidra). Device: Logic Pro 8,
VID:PID `21a9:1005`, hardware rev 11.0.0, FX3 firmware
"Logic Pro Firmware Compiled Oct 25 2022 16:42:00".

Status legend: ✅ confirmed on hardware · 🔍 seen in trace, meaning inferred · ❓ unknown

## USB

- SuperSpeed (USB 3.0), one interface, three bulk endpoints (1024-byte max packet):
  - `EP1 OUT` (0x01): commands
  - `EP1 IN` (0x81): command responses
  - `EP2 IN` (0x82): sample stream
- **The FX3 firmware is RAM-only.** At power-up the device enumerates as the FX3
  ROM bootloader on USB 2.0 (VID:PID still `21a9:1005`, iManufacturer
  "Cypress", iProduct "WestBridge", no endpoints). The host downloads firmware
  with the standard FX3 vendor request `0xA0` (wValue/wIndex = address,
  ≤4096-byte chunks), finishing with a zero-length `0xA0` to the entry point
  `0x4000e020`. The device then re-enumerates as "Saleae" / "Logic Pro" on
  SuperSpeed. Logic 2's download is byte-identical to the FX3 image embedded in
  `libgraph_server_shared.so` at `0x79a7f0` (sections 0x100, 0x40003000,
  0x40013000, 0x40030000; the data section starts with the reseed key
  `8e 24 4b 35`).
- The FPGA is volatile. The host uploads the bitstream on every connect.

## Command channel framing ✅

Each request is one bulk OUT transfer. Each request except a reseed gets one
bulk IN response; requests with nothing to return still get a 1-byte status.

Plaintext request layout (as Logic 2 builds it, `fcn.02f9c690`):

```
byte 0   : (cmd & 0xd7) | (plain_flag ? 0x08 : 0)
byte 1   : cmd
byte 2.. : arguments
```

The legacy libsigrok driver sends `0x00` in byte 0, and the firmware accepts
that too ✅ (byte 0 seems to be redundant or checked only loosely).

### Encryption ✅

A 32-bit LFSR state `L`, the same algorithm as libsigrok's `saleae-logic-pro`:

```
iterate(L): repeat (L & 0x1f) + 35 times:
    L = (L >> 1) | ((L ^ L>>1 ^ L>>21 ^ L>>31) & 1) << 31
```

- **Request**: `out[i] = in[i] ^ (L >> (8*(i%4)))`. Byte 0 is the exception:
  bits `0x28` stay plaintext (they're the flags) and only `& 0xd7` is XORed.
  Then `L = iterate(L)`.
- **Response**: `in[i] ^= L >> (8*(i%4))`, then `L = iterate(L)`.

So the LFSR steps once per request and once per response.

### Reseed ✅

Byte 0 bit `0x20` marks a reseed. The packet is always 5 bytes, gets no
response, and doesn't step the LFSR. The host chooses a random 32-bit seed
`s` (little-endian bytes `s0..s3`):

```
K = 0x354b248e               (fixed key; bytes k0..k3 little-endian = 8e 24 4b 35)
pkt = [ ((s0 & 0xd7) | 0x20) ^ (k0 & 0xd7),
        s0 ^ k1, s1 ^ k2, s2 ^ k3, s3 ^ k0 ]
L = s
```

The legacy driver's reseed `20 24 4b 35 8e` is exactly the seed `s = 0`
case, which is why its "keep L = 0" approach works. ✅ (verified on device)

Logic 2 reseeds right after opening the device, and again after reading the
crypto-chip slots.

## Commands

| cmd  | name             | request args                         | response | status |
|------|------------------|--------------------------------------|----------|--------|
| 0x7d | ping             | `55`                                 | `aa`     | ✅ 🔍 sent after every reseed |
| 0x8b | firmware version | –                                    | 128 B ASCII, NUL-padded | ✅ |
| 0x07 | read EEPROM      | `33 81 addr16le len16le`             | `len` bytes | ✅ |
| 0x7e | bitstream init   | –                                    | 1 B (0 = ok) | 🔍 |
| 0x7f | bitstream chunk  | `len16le data[len]` (len ≤ 1020)     | 1 B (0 = ok) | 🔍 |
| 0x80 | write FPGA regs  | `n (reg val)*n`                      | 1 B      | 🔍 |
| 0x81 | read FPGA regs   | `n reg*n`                            | n B      | 🔍 |
| 0x8c | crypto chip I2C  | see below                            |          | 🔍 |

### EEPROM (cmd 0x07) 🔍
- `addr 0x08, len 8`: device ID / serial. The Logic 2 log shows `Device ID: 0x707dc167abeb989e`.
- `addr 0x20, len 6`: `52 45 56 0b ff ff` = `"REV" 11` → hardware revision 11.0.0.

### Bitstream upload 🔍
`0x7e`, then about 254 × `0x7f` chunks of 1020 bytes, with the last one shorter.
Logic 2 uploads its own Logic Pro 8 bitstream. The chunks start with
`5f 7f fc 03 ff 00 "Lattice Semiconductor Corporation"`; the first 4 bytes
are the command header, so the FPGA is a Lattice part. After upload, the host
writes reg `0x7f = 0xaa` and reads it back as a sanity check (same as the
legacy driver).

### FPGA init after bitstream (Logic 2 order) 🔍
```
W 7f=aa ; W 7f=aa ; R 7f -> aa
ADC writes (reg 03=idx, 04=lo, 05=hi):
  [00]=0001  [0f]=0000  [11]=0777  [12]=4444  [2a]=1111  [2b]=1111  [42]=8040  [46]=8204
W 00=04 ; W 00=00
R 40 -> 20 ; R 40 -> a0
W 00=08 ; W 00=00
R 19 ; R 4a
W 15=00
ADC [0f]=0100
W 00=00 ; R 19
crypto-chip slot reads (6 × 32 bytes)
W 12=04
RESEED ; ping
W 0f=00 10=0f 11=00   (LED R/G/B)
```

### Crypto chip (cmd 0x8c) 🔍
An ATSHA204-style chip on I2C. Logic 2 reads 6 data slots:
```
> 8c 01 0800 | 77 07 02 82 40 0n crc16     (write 8 bytes: Read cmd, zone 0x82, addr 0x40 | n)
> 8c 01 0123 88                            (read 0x23 = 35 bytes)
< 23 | 23 <32 bytes> crc16
> 8c 00 0100 bb                            (idle/sleep?)
```
Slot 5 is all zeros. The contents are probably calibration data.

## Firmware command dispatcher ✅ (static RE)

The FX3 firmware Logic 2 downloads to RAM ("Oct 25 2022 16:42:00") is embedded in
`libgraph_server_shared.so`. It's an FX3 boot image (`CY..`) at file offset
`0x79a7f0` with SYSMEM loaded at `0x40003000`. `process_raw_command`
(`0x400032e8`) dispatches on byte 1 through a 140-entry jump table covering
`cmd` 0x01–0x8c.

**Any command outside the table, or an unused slot, is fatal.** It calls
`fault("CommandProcessing.c", 414, "process_raw_command", "unrecognized command")`
(`0x400060f0`), which loops forever: it blinks the error code on the RGB LED
(FPGA reg 0x0f..) and sleeps 1 s. No watchdog fires, and nothing is written
to flash or EEPROM. EP0 keeps working, but the command thread is dead: EP1 OUT
accepts one more packet and then NAKs. **Recovery needs real power removal**
(unplug). A USB reset, hub port power switching on a self-powered hub, or a PC
reboot doesn't help.

A reseed whose seed the host and device disagree on makes the next command
decode to garbage and trips this fault. Never send a reseed with a guessed format.

### Reset paths ✅ (static RE)
- **cmd 0x7c = FX3 hard reset.** It waits 100 ms, then clears bit 31 of
  `GCTL_CONTROL` (0xE0050000, HARD_RESET_N) and spins. It goes through the
  command thread, so it does nothing once that thread is in the fault loop.
- **cmd 0x7d = ping:** the response is `~arg` (`55` → `aa`).
- **EP0 vendor requests** (bmRequestType 0x40, device): the setup callback
  (`0x40004b18`) stores wValue/wIndex in a context slot and ACKs. Nothing in
  the firmware reads that slot back, so there's no vendor-request reset.
  Standard requests only do endpoint halt handling.
- So once the firmware faults, nothing over USB can recover it; it needs a real power cycle.

## Capture configuration ✅

FPGA registers are written with one cmd `0x80` before cmd `0x01` (start
capture), then read back. The meanings come from static RE of Logic 1.x
(`Logic2FpgaDevice::SetCaptureParameters`, `DeviceCalculateMixRate`) and are
confirmed against usbmon traces of Logic 2 on the Lattice bitstream:

| reg | meaning |
|---|---|
| `0x00` | control: `0x01` = run, `0x00` = stop |
| `0x06` | digital channel mask (`0x07`: high byte, 16-channel devices only) |
| `0x08` | analog channel mask (`0x09`: high byte, 16-channel devices only) |
| `0x0a` | analog divider `d = 50 MHz / analog rate`; reg = `d`, or `d − 1` for `d < 4` |
| `0x0b` | digital divider: rate = 500 MHz / (5·n), **8-bit n** (Lattice bitstream); `0` with `0x1a` below |
| `0x0c` | unused on the Lattice bitstream (16-bit divider high byte on the legacy Xilinx one) |
| `0x1a` | with `0x0b = 0`: power-of-two decimation of 500 MHz (`0` = 500, `1` = 250, `2` = 125 MS/s). **It must be written**: left at its default, the stream ran at 1/8 of the rate |
| `0x1b` | `0` |
| `0x0d` | analog words per stream frame (see below) |
| `0x0e` | digital words per stream frame |
| `0x12` | digital IO threshold option, `1 << index` (Logic 2 writes `0x04` for 3.3 V, `0x01` by default) |
| `0x13` | `0` |
| `0x14` | USB throttle `min(255, ceil(100e6 / (bits_per_s / 32)))`, with `bits_per_s = rate · (n_dig + 12 · n_ana)` |

Logic 2's digital rates for the Pro 8: 500, 250, 125, 100, 50, 25, 20, 12.5,
10, 6.25, 5, 4, 2.5, 2, 1 MS/s. Nothing below ~392 kHz is possible (n ≤ 255).

Examples from Logic 2 traces (`reg=val`, in write order):

```
10 MS/s D0-7:            00=00 08=00 06=ff 0a=00 1b=00 0b=0a 0c=00 1a=00 0d=00 0e=08 12=04 13=00 14=28
250 MS/s D2:             00=00 08=00 06=04 0a=00 1b=00 0b=00 0c=00 1a=01 0d=00 0e=01 12=01 13=00 14=0d
125 MS/s D2:             ...                           0b=00       1a=02 ...                        14=1a
D2 6.25M + A2 1.5625M:   00=00 08=04 06=04 0a=20 1b=00 0b=10 0c=00 1a=00 0d=03 0e=01 12=04 13=00 14=80
A2 50 MS/s:              00=00 08=04 06=00 0a=00 1b=00 0b=00 0c=00 1a=00 0d=03 0e=00 12=01 13=00 14=06
```

## Sample stream (EP2) ✅

The stream is a sequence of frames of 32-bit little-endian words:

```
[ 0x0e digital words ] [ 0x0d analog words ]
```

- **Digital word:** 32 consecutive samples of one channel, earliest sample
  in bit 31. One word per enabled channel, in ascending channel order.
- **Analog group:** 3 words = 96 bits = 8 consecutive samples of one channel.
  Each sample is **12-bit two's complement**, sample 0 in bits 0–11 of the
  96-bit little-endian value (`w0 | w1 << 32 | w2 << 64`). Groups rotate over
  the enabled analog channels in ascending order, so a frame holds
  `rounds × n_ana` groups.
- **Mux values** (vendor `DeviceCalculateMixRate`, 32-bit words, 12-bit ADC):
  `dw = dig_rate / 32`, `ag = ana_rate / 8`, `g = gcd(dw, ag)`;
  `0x0e = n_dig · dw/g`, `0x0d = 3 · n_ana · ag/g`. Digital only:
  `0x0d = 0`, `0x0e = n_dig`. Analog only: `0x0e = 0`, `0x0d = 3 · n_ana`.
  With equal rates (what the sigrok driver uses): `0x0e = n_dig`,
  `0x0d = 12 · n_ana`, so a frame is 32 sample periods.

## Analog calibration ✅

The analog sample rate is 50 MHz / d. Logic 2 calibrates 50, 12.5, 6.25, 3.125,
1.5625 MS/s and 781.25 kS/s, which are also valid digital rates. It keeps a
per-device JSON calibration file at
`~/.config/Logic/calibrations/<device id decimal>-<timestamp>-<n>.cal`. The
device ID is the 8-byte EEPROM serial (cmd `0x07`, addr `0x08`) read
little-endian, e.g. `0x707dc167abeb989e` = `8105847555346897054`.

- **`fullScaleVoltageRanges[ch]`** gives `{minimumVoltage, maximumVoltage}`:
  the volts at codes −2048 and +2047. Conversion is linear:
  `V = vmin + (code + 2048) · (vmax − vmin) / 4095`. The ranges are about
  ±10.5 V, different per channel.
- **`calibrationSets[rate][ch]`** gives `df1SosCoefficients`: 5 cascaded
  biquads (`b0 b1 b2 a1 a2`, direct form I) with unity DC gain that flatten
  the front-end frequency response, plus `groupDelay` in samples (5, or 3 at
  50 MS/s, the same on all channels). Logic 2 aligns analog with digital by
  this delay.
- `hittiteAdcGainRegisterValue` is 0 on this unit. The ADC is a Hittite
  part, configured via FPGA regs `0x03`/`0x04`/`0x05` (index, value lo, hi).

Checked on hardware: a 3.3 V UART line reads 3.289 / 0.012 V (high/low) in
sigrok, against 3.280 / 0.008 V in Logic 2.

## Triggers ✅

The hardware has no trigger unit: no trigger registers in the capture setup,
and no trigger command in the FX3 dispatcher. Saleae streams continuously and
detects triggers on the host. The libsigrok driver uses sigrok's software
trigger (`soft_trigger_logic_*`) with `captureratio` for pre-trigger data.

## Capture sequence (Logic 2, Logic Pro 8 / Lattice bitstream) ✅

From usbmon traces of Logic 2 captures, replayed by the sigrok driver and
verified on hardware:

```
W 15=03 ; ADC[0f]=0000 ; R 40 ; R 4a ; R 40        (ADC power up)
W <register block above> ; R <same regs>           (one cmd 0x80, then read back)
cmd 0x01 -> 07 00
W 00=01                                            (run; stream on EP2)
...
W 00=00 ; cmd 0x02 -> 07 00 ; R 40 ; R 4a
W 15=00 ; ADC[0f]=0100                             (ADC power down)
```

- **Status reg `0x40`:** `0x20` = idle/ok. Bit 7 is set during normal operation
  (`0xa0`). **Bit 0 = sample buffer overrun** (`0xa1`). It stays set until the
  next reset.
- **USB throughput:** 1 Gbit/s sustained, e.g. 500 MS/s × 2 ch, 125 MS/s × 8
  ch, or 50 MS/s analog (600 Mbit/s); 2 Gbit/s overruns. Logic 2 uses about
  1.6 MB bulk transfers. The device FIFO is small: if the host stops queueing
  reads for even ~0.1 s at 75 MB/s, it overruns and stops. Keep enough
  transfers queued (the driver queues ~0.4 s, with ~10 ms per transfer) and
  resubmit before converting.
- **Crypto handshake at capture start:** Logic 2 does a nonce + sign
  over cmd 0x8c, but it doesn't change the command-channel LFSR, and capture
  works without it.
- **Endpoint halt:** after an aborted session (host killed mid-capture),
  EP2 transfers fail with `LIBUSB_TRANSFER_ERROR` until the host sends
  CLEAR_FEATURE(ENDPOINT_HALT) on `0x82`. Logic 2 clears `0x81` on open.
