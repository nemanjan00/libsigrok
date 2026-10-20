"""Glasgow applet that drives known patterns for testing logic analyzers.

The pattern advances one step every (divisor + 1) system clock cycles and
keeps running after the applet exits, so a capture can be taken at leisure.
Pattern bit N goes to the N-th pin given with ``--pins``.
"""

import asyncio

from amaranth import *
from amaranth.lib import wiring, io, cdc
from amaranth.lib.wiring import In

from glasgow.support import logging
from glasgow.abstract import AbstractAssembly, GlasgowPin
from glasgow.applet import GlasgowAppletError, GlasgowAppletV2

from la_patterns import Mode, PRBS_TAPS


__all__ = ["Mode", "LAPatternComponent", "LAPatternInterface",
           "SenseComponent", "SenseInterface"]


class LAPatternComponent(wiring.Component):
    enable:  In(1)
    mode:    In(3)
    divisor: In(24)
    value:   In(16)

    def __init__(self, ports):
        self._ports = ports

        super().__init__()

    def elaborate(self, platform):
        m = Module()

        width = len(self._ports.pins)
        m.submodules.buffer = buffer = io.FFBuffer("o", self._ports.pins)
        m.d.comb += buffer.oe.eq(self.enable)

        # Every pattern is exactly as wide as the pins, so its sequence on the
        # pins is complete: no truncated Gray code, no LFSR state off-pin.
        counter = Signal(width)
        lfsr    = Signal(width, init=1)
        walk    = Signal(width, init=1)
        taps    = PRBS_TAPS.get(width, 0)

        timer = Signal.like(self.divisor)
        step  = Signal()
        with m.If(~self.enable):
            # Start every run from the same state.
            m.d.sync += [
                timer.eq(0),
                counter.eq(0),
                lfsr.eq(1),
                walk.eq(1),
            ]
        with m.Elif(timer == 0):
            m.d.sync += timer.eq(self.divisor)
            m.d.comb += step.eq(1)
        with m.Else():
            m.d.sync += timer.eq(timer - 1)

        with m.If(step):
            m.d.sync += counter.eq(counter + 1)
            m.d.sync += lfsr.eq(Mux(lfsr[0], (lfsr >> 1) ^ taps, lfsr >> 1))
            m.d.sync += walk.eq(walk.rotate_left(1))

        pattern = Signal(width)
        with m.Switch(self.mode):
            with m.Case(Mode.COUNT):
                m.d.comb += pattern.eq(counter)
            with m.Case(Mode.GRAY):
                m.d.comb += pattern.eq(counter ^ (counter >> 1))
            with m.Case(Mode.PRBS):
                m.d.comb += pattern.eq(lfsr)
            with m.Case(Mode.WALK):
                m.d.comb += pattern.eq(walk)
            with m.Default():
                m.d.comb += pattern.eq(self.value)
        m.d.comb += buffer.o.eq(pattern)

        return m


class SenseComponent(wiring.Component):
    """Edge counters on input pins, e.g. a logic analyzer's trigger output.

    Inputs are sampled at the system clock (48 MHz), so pulses shorter than
    one cycle (~21 ns) can be missed.
    """

    def __init__(self, ports):
        self._ports = ports
        count = len(ports.sense)

        super().__init__({
            "clear": In(1),
            "level": wiring.Out(count),
            **{f"rise{n}": wiring.Out(32) for n in range(count)},
            **{f"fall{n}": wiring.Out(32) for n in range(count)},
        })

    def elaborate(self, platform):
        m = Module()

        m.submodules.buffer = buffer = io.Buffer("i", self._ports.sense)
        level = Signal.like(self.level)
        m.submodules.sync = cdc.FFSynchronizer(buffer.i, level)
        last = Signal.like(level)
        m.d.sync += last.eq(level)
        m.d.comb += self.level.eq(level)

        for n in range(len(level)):
            rise, fall = getattr(self, f"rise{n}"), getattr(self, f"fall{n}")
            with m.If(self.clear):
                m.d.sync += [rise.eq(0), fall.eq(0)]
            with m.Else():
                with m.If(level[n] & ~last[n]):
                    m.d.sync += rise.eq(rise + 1)
                with m.If(~level[n] & last[n]):
                    m.d.sync += fall.eq(fall + 1)

        return m


class SenseInterface:
    def __init__(self, logger: logging.Logger, assembly: AbstractAssembly, *,
                 pins: tuple[GlasgowPin]):
        self._logger = logger
        self.pins = pins

        ports = assembly.add_port_group(sense=pins)
        component = assembly.add_submodule(SenseComponent(ports))
        self._clear = assembly.add_rw_register(component.clear)
        self._level = assembly.add_ro_register(component.level)
        self._rise = [assembly.add_ro_register(getattr(component, f"rise{n}"))
                      for n in range(len(pins))]
        self._fall = [assembly.add_ro_register(getattr(component, f"fall{n}"))
                      for n in range(len(pins))]

    async def clear(self):
        await self._clear.set(True)
        await self._clear.set(False)

    async def read(self) -> list[tuple[int, int, int]]:
        """(level, rising edges, falling edges) of every sense pin."""
        level = await self._level.get()
        return [((level >> n) & 1, await self._rise[n].get(), await self._fall[n].get())
                for n in range(len(self.pins))]


class LAPatternInterface:
    def __init__(self, logger: logging.Logger, assembly: AbstractAssembly, *,
                 pins: tuple[GlasgowPin], name: str = "step"):
        self._logger = logger

        ports = assembly.add_port_group(pins=pins)
        component = assembly.add_submodule(LAPatternComponent(ports))
        self._enable = assembly.add_rw_register(component.enable)
        self._mode   = assembly.add_rw_register(component.mode)
        self._value  = assembly.add_rw_register(component.value)
        self._step   = assembly.add_clock_divisor(component.divisor,
            ref_period=assembly.sys_clk_period, tolerance=0, round_mode="nearest", name=name)

    async def start(self, mode: Mode, rate: int, value: int = 0) -> int:
        """(Re)start the pattern at :py:`rate` steps per second.

        Returns the step rate actually used.
        """
        await self._enable.set(False)
        await self._mode.set(mode)
        await self._value.set(value)
        await self._step.set_frequency(rate)
        await self._enable.set(True)
        return await self._step.get_frequency()

    async def stop(self):
        """Stop the pattern and release the pins (Hi-Z)."""
        await self._enable.set(False)


class GenerateLAPatternApplet(GlasgowAppletV2):
    logger = logging.getLogger(__name__)
    help = "drive known patterns for testing logic analyzers"
    description = """
    Drives a known pattern on up to 16 pins, for checking a logic analyzer's channel mapping,
    samplerate, triggers and data integrity. Pattern bit N goes to the N-th pin of ``--pins``.
    The pattern advances at the step rate (at most the 48 MHz system clock, in integer
    fractions of it) and keeps running after the applet exits.

    Modes:

    * ``count``: binary counter; bit N toggles every 2**N steps
    * ``gray``: Gray code counter; one bit changes per step, so channel skew can't create
      false states
    * ``prbs``: maximal-length LFSR as wide as the pins (2-16), many bits change at once
    * ``walk``: one high bit rotating across the pins
    * ``static``: a fixed value (``--value``)

    Captures can be checked with ``la-check``, which knows the same patterns.
    """

    @classmethod
    def add_build_arguments(cls, parser, access):
        access.add_voltage_argument(parser)
        access.add_pins_argument(parser, "pins", width=range(1, 17), required=True)
        access.add_pins_argument(parser, "sense", width=range(1, 5), help=
            "count edges on PINS, e.g. the analyzer's trigger output")

    def build(self, args):
        with self.assembly.add_applet(self):
            self.assembly.use_voltage(args.voltage)
            self.pattern_iface = LAPatternInterface(self.logger, self.assembly, pins=args.pins)
            self.sense_iface = (SenseInterface(self.logger, self.assembly, pins=args.sense)
                                if args.sense else None)

    @classmethod
    def add_setup_arguments(cls, parser):
        parser.add_argument(
            "-m", "--mode", choices=[mode.name.lower() for mode in Mode], default="count",
            help="pattern to drive (default: %(default)s)")
        parser.add_argument(
            "-r", "--rate", metavar="HZ", type=float, default=1e6,
            help="step rate in Hz, at most 48 MHz (default: %(default)s)")
        parser.add_argument(
            "--value", metavar="VALUE", type=lambda x: int(x, 0), default=0,
            help="value driven in static mode (default: %(default)s)")

    async def setup(self, args):
        if args.mode == "prbs" and len(args.pins) not in PRBS_TAPS:
            raise GlasgowAppletError("prbs mode needs at least 2 pins")
        actual = await self.pattern_iface.start(Mode[args.mode.upper()], int(args.rate),
                                                args.value)
        self.logger.info("driving %s pattern on %d pins at %.6f MHz step rate",
                         args.mode, len(args.pins), actual / 1e6)

    @classmethod
    def add_run_arguments(cls, parser):
        parser.add_argument(
            "--monitor", metavar="SECONDS", type=float,
            help="keep running and print sense pin edge counts when they change, for SECONDS "
                 "(0: until interrupted)")

    async def run(self, args):
        if not self.sense_iface or args.monitor is None:
            return # the pattern keeps running in the FPGA
        await self.sense_iface.clear()
        loop = asyncio.get_running_loop()
        deadline = loop.time() + args.monitor if args.monitor else None
        last = None
        while deadline is None or loop.time() < deadline:
            state = await self.sense_iface.read()
            if state != last:
                print(" ".join(f"{pin}: level={lv} rise={r} fall={f}"
                               for pin, (lv, r, f) in zip(self.sense_iface.pins, state)),
                      flush=True)
                last = state
            await asyncio.sleep(0.05)
