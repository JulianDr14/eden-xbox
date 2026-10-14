#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent mathematical oracle for the isolated smooth(a*W)/W control.

No GPU driver or guest shader data is needed. Native GPU comparisons remain
necessary: this checks the representation, not an Xbox implementation.
"""
import math
import random

rng = random.Random(0x4E5057)
cases = 0
for _ in range(4096):
    w0 = rng.uniform(1, 10000) * rng.choice((-1, 1))
    w1 = rng.uniform(1, 10000)
    a0, a1 = rng.uniform(-100, 100), rng.uniform(-100, 100)
    t = rng.uniform(0, 1)
    w = (1-t)*w0 + t*w1
    if abs(w) < 0.01:
        continue
    # Reference clipper factor derived by projecting an edge to framebuffer.
    screen_t = t*w1/w
    expected = (1-screen_t)*a0 + screen_t*a1
    represented = ((1-t)*a0*w0 + t*a1*w1)/w
    assert math.isclose(represented, expected, rel_tol=1e-10, abs_tol=1e-9)
    cases += 1

for _ in range(4096):
    # Positive post-clip W and arbitrary framebuffer barycentric coordinates.
    weights = [rng.random() for _ in range(3)]
    total = sum(weights)
    weights = [weight/total for weight in weights]
    ws = [rng.uniform(0.01, 10000) for _ in range(3)]
    attrs = [rng.uniform(-100, 100) for _ in range(3)]
    reciprocal_w = sum(weight/w for weight, w in zip(weights, ws))
    smooth_weighted_attribute = sum(weight*a for weight, a in zip(weights, attrs))/reciprocal_w
    d3d_position_w = 1/reciprocal_w
    represented = smooth_weighted_attribute/d3d_position_w
    expected = sum(weight*a for weight, a in zip(weights, attrs))
    assert math.isclose(represented, expected, rel_tol=1e-10, abs_tol=1e-9)
    cases += 1
print(f'PASS {cases} clip/interpolation oracle cases, including negative pre-clip W')
