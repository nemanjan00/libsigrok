# libsigrok, with all the hardware

One [libsigrok](https://github.com/sigrokproject/libsigrok) for all the newer
hardware: the library behind [sigrok-cli](https://sigrok.org/wiki/Sigrok-cli) and
[PulseView](https://sigrok.org/wiki/PulseView), with every working out-of-tree
driver merged in.

**The problem:** upstream development has slowed down, so support for newer
devices lives elsewhere: in vendor forks, personal forks and pull requests that
never got merged. Each one is a separate libsigrok, and you can only install one
at a time. With a DSLogic, a Sipeed SLogic and a Rigol DHO on the bench, you'd
need three different forks, and switch between them.

**This fork:** all of that in a single libsigrok. Out-of-tree work is merged
here, fixed where needed, and tested on real hardware. Missing devices get
added. It's bleeding edge: it tracks upstream `master` and the newest code
from every source, not releases.

`master` is upstream libsigrok with everything merged on top. Upstream code
isn't rewritten: every addition comes in as a merge, so `git log --graph` shows
where each change came from. The API stays the same, so it's a drop-in
replacement and PulseView and sigrok-cli work with it unchanged.

## Added and fixed hardware

| Device | Driver | Status | Source |
|---|---|---|---|
| DreamSourceLab DSLogic U3Pro32 | `dreamsourcelab-dslogic` | Tested: all 32 channels, buffered and continuous mode, triggers on every channel, external clock | this fork |
| DreamSourceLab DSLogic U3Pro16 | `dreamsourcelab-dslogic` | Supported, not re-tested here | [christianeisendle](https://github.com/christianeisendle/libsigrok), with this fork's fixes |
| Saleae Logic Pro 8 (current hardware) | `saleae-logic-pro` | Tested: digital up to 500 MS/s, analog, triggers | this fork, Logic 2 protocol |
| Sipeed SLogic Combo 8, 16U3, 32U3 | `sipeed-slogic-analyzer` | Combo 8 tested | [Sipeed](https://github.com/sipeed/libsigrok/tree/slogic-dev) |
| Rigol MHO98 (MHO900 series) | `rigol-ds` | Tested over LAN: analog channels, live and memory captures, settings | this fork |
| Rigol DHO800/900/1000/4000 | `rigol-ds` | Supported, tested by others | [upstream PR #246](https://github.com/sigrokproject/libsigrok/pull/246) |

Fixes beyond adding devices:

- **DSLogic U3 series:** correct voltage threshold scaling; samplerates the
  enabled channels can't reach are refused instead of returning garbage;
  continuous captures with an external clock no longer abort.
- **Rigol DHO/MHO:** correct samplerate for memory captures (it was 1000x too
  low); a channel the scope refuses to send now fails instead of silently
  returning another channel's data.

Per-driver details (protocol notes, rate limits, known issues, exactly what was
tested) are in [`AGENTS.md`](AGENTS.md). Notes on drivers that came from
upstream are in [`README.devices`](README.devices).

## Building

```sh
git clone https://github.com/nemanjan00/libsigrok.git
cd libsigrok
./autogen.sh
mkdir build && cd build
../configure
make -j$(nproc)
sudo make install
```

Then install [`contrib/60-libsigrok.rules`](contrib/60-libsigrok.rules) (and
[`contrib/61-libsigrok-uaccess.rules`](contrib/61-libsigrok-uaccess.rules)) into
`/etc/udev/rules.d/`, so USB devices work without root. If PulseView or
sigrok-cli were built against another libsigrok, rebuild them against this one
or make sure it's found first. `configure --help` lists every option, e.g.
`--disable-all-drivers --enable-<driver>` for a quick single-driver build.

The [sigrok wiki](https://sigrok.org/wiki/Building) has OS-specific build
instructions and an FAQ.

### Requirements

The C library needs a C compiler (gcc or clang), make, pkg-config >= 0.22,
glib >= 2.32 and libzip >= 0.11. Building from git also needs git, autoconf
>= 2.63, automake >= 1.11 and libtool.

Optional, each enabling the drivers or features that use it:

| Library | Used for |
|---|---|
| libusb-1.0 >= 1.0.16 | USB devices (most logic analyzers) |
| libserialport >= 0.1.1 | serial devices |
| hidapi >= 0.8.0 | HID based serial cables |
| bluez / libbluetooth >= 4.0 | Bluetooth and BLE devices |
| libftdi1 >= 1.0 | FTDI based devices |
| librevisa, libgpib, libieee1284 | VISA, GPIB and parallel port devices |
| libtirpc | VXI-11 (fallback when glibc >= 2.26) |
| libgio >= 2.32, nettle | some network drivers |
| zlib | CRC32 in the STF input format |
| check >= 0.9.4 | unit tests (`make check`) |
| doxygen, graphviz | API documentation |

### Bindings

| Bindings | Needs |
|---|---|
| C++ (used by PulseView) | C++11 compiler, autoconf-archive (from git), doxygen (always, not only for docs), Python 3, glibmm-2.4 >= 2.32 or glibmm-2.68 |
| Python | the C++ bindings, SWIG >= 2.0, Python 3 with development files, setuptools, pygobject >= 3.0, numpy |
| Ruby | the C++ bindings, Ruby >= 2.5 with development files, SWIG >= 3.0.8 |
| Java | the C++ bindings, SWIG >= 2.0, a JDK |

Disable the ones you don't need with `--disable-python`, `--disable-ruby` and
`--disable-java`.

## Firmware

Some devices need firmware or FPGA bitstreams that can't be distributed here.
Put them in a sigrok firmware directory, e.g. `~/.local/share/sigrok-firmware/`
or `/usr/share/sigrok-firmware/`.

| Device | Files | Where they come from |
|---|---|---|
| DSLogic U3Pro16 / U3Pro32 | `dreamsourcelab-dslogic-u3pro16-fpga.fw`, `dreamsourcelab-dslogic-u3pro32-fpga.fw` | `DSLogicU3Pro16.bin` / `DSLogicU3Pro32.bin` from [DSView](https://github.com/DreamSourceLab/DSView) (`DSView/res/`), renamed |
| Saleae Logic Pro 8 | `saleae-logicpro8-logic2-fx3.fw`, `saleae-logicpro8-logic2-fpga.bitstream` | extracted from Saleae Logic 2, see [`AGENTS.md`](AGENTS.md) |
| other upstream drivers | see [`README.devices`](README.devices) | mostly [sigrok-firmware](https://sigrok.org/wiki/Firmware) |

## Testing on real hardware

[`la-bench/`](la-bench) is a test bench for logic analyzers: a
[Glasgow](https://glasgow-embedded.org) applet that drives known patterns
(counters, Gray code, PRBS) at up to 48 MHz, and a checker that verifies sigrok
captures of them sample by sample. It reports the channel map, the samplerate
error, and every dropped, repeated or corrupted sample. It found one of the
DSLogic bugs above. See [`la-bench/README.md`](la-bench/README.md).

## Missing something?

If a device works in some other fork or an open pull request, or doesn't work
anywhere yet, open an issue or a pull request. The goal is that nobody has to
pick between forks.

## Branches

- **`master`:** upstream plus everything above. This is the one to use.
- **Topic branches** (`dslogic-u3pro32`, `rigol-mho900`, `saleae-logic-pro8`):
  one per device, kept upstreamable. They're based on upstream `master` or on
  the pull request they extend, and never carry fork-only files.

Fork-only files live on `master` only: this README, `AGENTS.md`, `la-bench/`,
and one compatibility commit for a rebadged Rigol scope.

## License

libsigrok is licensed under the GNU General Public License, version 3 or later.
Some individual files are GPLv2+ or GPLv3+; the library as a whole is GPLv3+.
See the individual source files for the copyright holders.

## Upstream

- Website: <https://sigrok.org/wiki/Libsigrok>
- Source: <https://github.com/sigrokproject/libsigrok>
- Mailing list: <https://lists.sourceforge.net/lists/listinfo/sigrok-devel>
- IRC: `#sigrok` on Libera.Chat
