#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes the fixed text input for the busybox applet tests.

Usage: gen_applet_input.py FILE
"""

import random
import sys

WORDS = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo", "lima",
         "mike", "november", "oscar", "papa", "quebec", "romeo", "sierra", "tango", "uniform", "victor", "whiskey",
         "xray", "yankee", "zulu"]


def main():
    rng = random.Random(20260916)
    with open(sys.argv[1], "w") as f:
        for _ in range(3000):
            n = rng.randrange(1, 9)
            f.write(" ".join(rng.choice(WORDS) for _ in range(n)) + f" {rng.randrange(-100000, 100000)} {rng.random():.6f}\n")


if __name__ == "__main__":
    main()
