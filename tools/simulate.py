#!/usr/bin/env python3
"""simulate.py — sanity-check the physics engines' trigger rates.

A faithful Python port of the integer math in engines.cpp (same Q16 fixed point,
same xorshift, same constants), used to confirm each mode fires at musically
useful rates across the knob range before committing to a hardware trip.

This is a MODEL of engines.cpp, not the code itself — if you change the C++,
change this too, or delete it. It exists because there is no host C++ compiler
on this machine (tools/simulate.cpp is the real harness if you get one).

    python tools/simulate.py
"""

Q = 65536
CTRL = 1500
MASK32 = 0xFFFFFFFF


def xorshift(s):
    s ^= (s << 13) & MASK32
    s ^= s >> 17
    s ^= (s << 5) & MASK32
    return s & MASK32


def rand_q16(s):
    s = xorshift(s)
    return s, s >> 16


def s32(v):
    v &= MASK32
    return v - (1 << 32) if v >= (1 << 31) else v


def mul_q16(a, b):
    return (a * b) >> 16


def decay(v, shift):
    d = v >> shift
    return 0 if d == 0 else v - d


# --- sine table, matching fastmath.cpp -------------------------------------
import math
SIN = [round(32767 * math.sin((i / 256) * (math.pi / 2))) for i in range(257)]


def fast_sin(phase):
    phase &= MASK32
    quad = phase >> 30
    frac = (phase >> 6) & 0xFFFFFF
    idx = frac >> 16
    mu = frac & 0xFFFF
    if quad & 1:
        idx = 255 - idx
        mu = 65536 - mu
        if mu == 65536:
            mu = 0
            idx += 1
    a, b = SIN[idx], SIN[idx + 1]
    v = a + (((b - a) * mu) >> 16)
    return -v if quad & 2 else v


# --- Mode 1: Horses ---------------------------------------------------------
RATIO = [65536, 64225, 67502, 62259]
GAIT = [
    [0b0001, 0b0010, 0b0100, 0b1000],
    [0b0011, 0b0000, 0b1100, 0b0000],
    [0b0001, 0b0110, 0b1000, 0b0000],
    [0b0001, 0b0010, 0b0100, 0b1000],
]


def horses(physics, chaos, pop, ticks, spook_every=0):
    phase = [0] * 4
    last = [0xFF] * 4
    rng = 0x1234
    fires = [0] * 4
    gait = min(physics >> 14, 3)
    within = (physics & 0x3FFF) << 2
    hz_q8 = 150 + (gait * 480) + mul_q16(within, 480)
    inc = (hz_q8 * (1 << 32)) // (256 * CTRL)
    for t in range(ticks):
        if spook_every and t % spook_every == 0 and t:
            phase = [0] * 4
            last = [0xFF] * 4
        for i in range(4):
            ainc = (inc * RATIO[i]) >> 16
            if chaos:
                rng = xorshift(rng)
                wob = (rng >> 17) - 16384
                amt = mul_q16(wob, chaos)
                ainc = ainc + ((ainc * amt) >> 16)
            phase[i] = (phase[i] + ainc) & MASK32
            step = phase[i] >> 30
            if step != last[i]:
                last[i] = step
                if GAIT[gait][step] & (1 << i) and i < pop:
                    fires[i] += 1
    return fires


# --- Mode 2: Geese ----------------------------------------------------------
def geese(physics, chaos, pop, ticks, spook_every=0):
    excite = [0] * 4
    refr = [0] * 4
    rng = 0x1234
    fires = [0] * 4
    REFRACTORY = CTRL // 16
    spark = (2 + (chaos >> 12)) * (4 + 1 - pop)
    for t in range(ticks):
        if spook_every and t % spook_every == 0 and t:
            excite = [Q] * 4
        fired = 0
        for i in range(pop):
            if refr[i] > 0:
                refr[i] -= 1
            else:
                contagion = (physics + mul_q16(physics, physics)) >> 1
                p = spark + (mul_q16(excite[i], contagion) >> 2)
                rng, r = rand_q16(rng)
                if r < p:
                    fired |= 1 << i
                    refr[i] = REFRACTORY
                    fires[i] += 1
        if fired:
            for i in range(4):
                if not (fired & (1 << i)):
                    continue
                for j in range(4):
                    if j != i:
                        excite[j] = min(excite[j] + Q // 3, Q)
        excite = [decay(e, 6) for e in excite]
    return fires


# --- Mode 3: Frogs ----------------------------------------------------------
DETUNE = [0, 3000, -2200, 5000]


def frogs(physics, chaos, pop, ticks, spook_every=0):
    rng = 0x1234
    phase = []
    for _ in range(4):
        rng = xorshift(rng)
        phase.append(rng)
    fires = [0] * 4
    base_hz_q8 = 400
    base_inc = (base_hz_q8 * (1 << 32)) // (256 * CTRL)
    inv = Q - physics
    K = mul_q16(mul_q16(inv, inv), inv)
    nat = [base_inc + ((base_inc >> 4) * mul_q16(DETUNE[i], chaos) >> 12)
           for i in range(4)]
    n = max(pop, 1)
    for t in range(ticks):
        if spook_every and t % spook_every == 0 and t:
            for i in range(4):
                rng = xorshift(rng)
                phase[i] = rng
        for i in range(n):
            coup = 0
            for j in range(n):
                if j != i:
                    coup += fast_sin((phase[j] - phase[i]) & MASK32)
            if n > 1:
                coup = int(coup / (n - 1))
            adjust = (nat[i] * ((coup * (K >> 1)) >> 15)) >> 16
            before = phase[i]
            phase[i] = (phase[i] + nat[i] + adjust) & MASK32
            if phase[i] < before:
                fires[i] += 1
    return fires


# --- Mode 4: Rain -----------------------------------------------------------
THRESH = [Q, (Q * 115) // 100, (Q * 88) // 100, (Q * 103) // 100]
LEAK = [9, 8, 10, 9]


def rain(physics, chaos, pop, ticks, spook_every=0):
    level = [0] * 4
    rng = 0x1234
    fires = [0] * 4
    downpour = (physics >> 2) + ((physics + mul_q16(physics, physics)) >> 2)
    downpour = min(downpour, Q)
    for t in range(ticks):
        if spook_every and t % spook_every == 0 and t:
            for i in range(4):
                rng, r = rand_q16(rng)
                level[i] += (Q >> 1) + (r >> 1)
        for i in range(pop):
            rng, r = rand_q16(rng)
            drop = mul_q16(r, downpour) >> 5
            if chaos:
                rng, r2 = rand_q16(rng)
                drop += mul_q16(r2 - (Q // 2), chaos) >> 4
            drop = max(drop, 0)
            level[i] += drop
            level[i] = decay(level[i], LEAK[i])
            if level[i] >= THRESH[i]:
                fires[i] += 1
                level[i] = 0
    return fires


# --- Mode 5: Meteors --------------------------------------------------------
def meteors(physics, chaos, pop, ticks, spook_every=0):
    rng = 0x1234
    density = 0
    target = 0
    until = 1
    fires = [0] * 4
    for t in range(ticks):
        until -= 1
        if until == 0:
            rng, target = rand_q16(rng)
            rng = xorshift(rng)
            until = CTRL + (rng % (3 * CTRL))
        density = density + ((target - density) >> 9)
        if spook_every and t % spook_every == 0 and t:
            density = Q
        if physics < Q // 2:
            depth, floor = physics * 2, 0
        else:
            tt = (physics - Q // 2) * 2
            depth, floor = Q - tt, tt
        eff = floor + mul_q16(density, depth)
        p = mul_q16(eff, eff) >> 7
        if chaos:
            rng, r = rand_q16(rng)
            p += mul_q16(r, chaos) >> 10
        for i in range(pop):
            rng, r = rand_q16(rng)
            if r < p:
                fires[i] += 1
    return fires


ENGINES = [("Horses", horses), ("Geese", geese), ("Frogs", frogs),
           ("Rain", rain), ("Meteors", meteors)]


def main():
    secs = 20
    ticks = secs * CTRL
    print(f"Trigger rates per agent (per second), {secs}s runs, 4 agents\n")
    for name, fn in ENGINES:
        print(f"{name}")
        for label, phys, chaos, pop, spook in [
            ("physics 0.00", 0, 0, 4, 0),
            ("physics 0.25", Q // 4, 0, 4, 0),
            ("physics 0.50", Q // 2, 0, 4, 0),
            ("physics 0.75", (3 * Q) // 4, 0, 4, 0),
            ("physics 1.00", Q - 1, 0, 4, 0),
            ("phys 0.5 chaos 1.0", Q // 2, Q - 1, 4, 0),
            ("phys 0.5 +spook/s", Q // 2, 0, 4, CTRL),
            ("phys 0.5 pop=2", Q // 2, 0, 2, 0),
        ]:
            f = fn(phys, chaos, pop, ticks, spook)
            rates = " ".join(f"{x / secs:6.2f}" for x in f)
            print(f"  {label:<20} {rates}")
        print()


if __name__ == "__main__":
    main()
