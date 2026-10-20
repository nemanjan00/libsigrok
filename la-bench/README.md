# la-bench

Test bench for logic analyzers: a [Glasgow](https://glasgow-embedded.org)
applet that drives known patterns, and a checker that verifies sigrok captures
of them sample by sample.

- `glasgow_la_pattern/`: the `generate-la-pattern` applet (out-of-tree).
- `la_check.py`: checks a sigrok `.sr` capture against the pattern.
- `la_patterns.py`: pattern definitions shared by both.

## Install

The applet goes into the Glasgow venv; Glasgow only loads out-of-tree applets
when told to:

```sh
~/.local/share/gw-venv/bin/pip install ~/sigrok/libsigrok/la-bench
export GLASGOW_OUT_OF_TREE_APPLETS=I-am-okay-with-breaking-changes
```

Install it as a copy, not with `pip install -e`: `la-bench/` only exists on
`master`, so an editable install breaks whenever the checkout is on a topic
branch. Reinstall after changing it, from a `master` checkout or an export
(`git archive master la-bench | tar -x -C <dir>`).

`la_check.py` needs numpy and runs with the system Python.

## Patterns

Pattern bit N is driven on the N-th pin of `--pins`, so a pattern is exactly
as wide as the pin list (1-16). The step rate is the 48 MHz system clock
divided by an integer, so up to 48 MHz. The pattern keeps running in the FPGA
after the applet exits.

| Mode | Use |
|---|---|
| `count` | Binary counter, bit N toggles every 2^N steps. Each channel gets its own frequency, so `la-check` can work out the channel map. |
| `gray` | One bit changes per step. Channel skew can't create false states, so every sample is checked: the strictest test. |
| `prbs` | Maximal-length LFSR as wide as the pins (2-16): many bits switch at once. |
| `walk` | One high bit rotating across the pins. |
| `static` | A fixed value (`--value`). |

`--sense PINS` (up to 4) counts edges on inputs, e.g. an analyzer's trigger
output; `--monitor SECONDS` keeps the applet running and prints the counts
when they change. Inputs are sampled at 48 MHz, so pulses under ~21 ns can be
missed.

## Recipes

Channel map, rate and data integrity (here 12 pins on a DSLogic):

```sh
glasgow run generate-la-pattern -V A=3.3,B=3.3 --pins A0:7,B4:7 -m count -r 1e6
sigrok-cli -d dreamsourcelab-dslogic -c samplerate=50M --samples 20M -o cap.sr
python3 la_check.py cap.sr -m count -r 1e6          # prints the channel map it found
glasgow run generate-la-pattern -V A=3.3,B=3.3 --pins A0:7,B4:7 -m gray -r 24e6
sigrok-cli -d dreamsourcelab-dslogic -C <map> -c samplerate=250M --samples 50M -o cap.sr
python3 la_check.py cap.sr -m gray -r 24e6 --map <map>
```

`la_check.py` reports steps, errors (with the sample where each happened),
the measured step rate in ppm against the Glasgow's crystal, and the run
length per step. It exits non-zero on errors. Dropped, repeated and corrupted
samples all show up as errors; in `count`/`prbs`/`walk` mode one-sample
corruption looks like a skew glitch and is filtered, so use `gray` for that.

External clock (state mode): put the analyzer's clock input first in
`--pins`, so it carries bit 0 of a `count` pattern. The data pins change half
a clock period before each rising edge, and every sample must be the previous
one plus 1:

```sh
glasgow run generate-la-pattern -V A=3.3,B=3.3 --pins B2,A0:7,B4:7 -m count -r 48e6   # 24 MHz clock
sigrok-cli -d dreamsourcelab-dslogic -C <map> -c samplerate=100M:external_clock=on --samples 5M -o cap.sr
python3 la_check.py cap.sr -m count --clocked --map <map>
```

Trigger output: sense it while capturing; expect one pulse per capture.

```sh
glasgow run generate-la-pattern -V A=3.3,B=3.3 --pins A0:7 --sense B0 -m count -r 1e6 --monitor 30
```

Fault detection was checked by editing a good capture: a dropped block, a
repeated block and a single flipped sample are all reported at the right
sample.
