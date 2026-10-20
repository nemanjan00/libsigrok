"""Pattern definitions shared by the generate-la-pattern applet and la-check.

Plain Python, so the checker doesn't need Glasgow or Amaranth.
"""

import enum


class Mode(enum.IntEnum):
    COUNT  = 0 # binary counter: bit N toggles every 2**N steps
    GRAY   = 1 # Gray code counter: exactly one bit changes per step
    PRBS   = 2 # maximal-length Galois LFSR over the pin count
    WALK   = 3 # one high bit rotating across the pins
    STATIC = 4 # fixed value


# Maximal-length Galois LFSR feedback masks by width (period 2**width - 1).
# The state shifts right and XORs in the mask when the shifted-out bit is 1.
# Found by exhaustive search: the lowest such mask for each width.
PRBS_TAPS = {
    2: 0x3, 3: 0x5, 4: 0x9, 5: 0x12, 6: 0x21, 7: 0x41, 8: 0x8e, 9: 0x108,
    10: 0x204, 11: 0x402, 12: 0x829, 13: 0x100d, 14: 0x2015, 15: 0x4001, 16: 0x8016,
}

MAX_WIDTH = 16


def prbs_next(state: int, width: int) -> int:
    return (state >> 1) ^ (PRBS_TAPS[width] if state & 1 else 0)


def gray(value: int) -> int:
    return value ^ (value >> 1)
