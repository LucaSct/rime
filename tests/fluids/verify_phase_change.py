# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
"""Offline binary64 transaction proof; Python exact fractions are NOT a runtime dependency.

Build the engine first, then pass the ordinary optimized fluids archive and a scratch directory.
This diagnostic targets GCC/Clang static archives; CTest's portable C++ tests remain the CI gate.
The generated probe only marshals bit patterns through the actual library, never reimplements it.
"""

import argparse
from collections import Counter
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess


PROBE = r'''// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <bit>
#include <cstdint>
#include <iostream>
#include "rime/fluids/phase_change.hpp"
int main() {
    std::uint64_t mass, temperature, fraction, heat;
    const auto unpack = [](auto v) { return std::bit_cast<double>(v); };
    const auto pack = [](double v) { return std::bit_cast<std::uint64_t>(v); };
    while (std::cin >> mass >> temperature >> fraction >> heat) {
        auto sample = rime::fluids::ThermalSample::create(
            unpack(mass), {unpack(temperature), unpack(fraction)});
        if (!sample) { std::cout << "0\n"; continue; }
        const auto before = sample->energy_joules();
        const auto result = sample->apply_heat(unpack(heat));
        const auto state = sample->state();
        std::cout << "1 " << static_cast<int>(result.status) << ' ' << pack(before) << ' '
                  << pack(sample->energy_joules()) << ' ' << pack(result.requested_joules) << ' '
                  << pack(result.applied_joules) << ' ' << pack(result.applied_error_joules) << ' '
                  << pack(result.roundoff_joules) << ' ' << pack(state.temperature_kelvin) << ' '
                  << pack(state.liquid_mass_fraction) << '\n';
    }
    return std::cin.eof() ? 0 : 1;
}
'''


def bits(value):
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def number(value):
    return struct.unpack("<d", struct.pack("<Q", value))[0]


def cases():
    masses = [math.ldexp(1.375, e) for e in [-900, -600, -100, 0, 100, 600, 900]]
    masses += [math.ulp(0.0), 1.0e-320, 1.0e-300, 1.0, 1.0e300, 1.0e305]
    states = [(273.15 + offset, fraction) for offset, fraction in
              [(-20, 0), (-10, 0), (0, 0), (0, 0.25), (0, 0.5), (0, 0.75),
               (0, 1), (10, 1), (20, 1), (40, 1)]]
    # The literal bound and Tm-20 can differ by one binary64 bit. Include both, rather than
    # silently widen the admission interval with an epsilon to make the arithmetic agree.
    states.append((253.15, 0))
    for mass in masses:
        for temperature, fraction in states:
            offset = temperature - 273.15
            latent = mass * 333500.0
            initial_guess = (mass * 2052.0 * offset if offset < 0 else
                             latent + mass * 4179.0 * offset if offset > 0 else
                             latent * fraction)
            quantum = math.ulp(initial_guess)
            for magnitude in [0.0, math.ulp(0.0), quantum * 0.25, quantum * 0.5,
                              quantum * 0.75, quantum, latent / 16.0, latent, latent * 2.0]:
                for sign in [-1.0, 1.0]:
                    yield mass, temperature, float(fraction), sign * magnitude
            for boundary in [mass * 2052.0 * (253.15 - 273.15),
                             latent + mass * 4179.0 * (313.15 - 273.15)]:
                if math.isfinite(boundary) and math.isfinite(initial_guess):
                    direction = -1.0 if boundary < initial_guess else 1.0
                    heat = boundary - initial_guess + direction * math.ulp(boundary) * 0.25
                    yield mass, temperature, float(fraction), heat


def main():
    if not __debug__:
        raise RuntimeError("Exact verification requires assertions; disable -O/PYTHONOPTIMIZE")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--compiler", default="c++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.work_dir.mkdir(parents=True, exist_ok=True)
    # A refused or interrupted rerun must not leave an older successful report looking current.
    (args.work_dir / "exact-result.json").unlink(missing_ok=True)
    source = args.work_dir / "thermal-exact-probe.cpp"
    binary = args.work_dir / "thermal-exact-probe"
    source.write_text(PROBE)
    subprocess.run([args.compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(root / "engine/fluids/include"), str(source),
                    str(args.library.resolve()), "-o", str(binary)], check=True)
    inputs = list(cases())
    payload = "".join(" ".join(str(bits(v)) for v in row) + "\n" for row in inputs)
    result = subprocess.run([str(binary)], input=payload, text=True, capture_output=True,
                            check=True, timeout=60)
    rows = result.stdout.splitlines()
    if len(rows) != len(inputs):
        raise AssertionError("probe omitted transactions")
    counts = Counter()
    for index, (case, row) in enumerate(zip(inputs, rows)):
        try:
            values = [int(v) for v in row.split()]
            if values == [0]:
                mass, temperature, _, _ = case
                assert (not 253.15 <= temperature <= 313.15 or
                        not all(math.isfinite(mass * coefficient) for coefficient in
                                [2052.0, 4179.0, 333500.0]))
                counts["creation_refused"] += 1
                continue
            if len(values) != 10 or values[0] != 1:
                raise AssertionError("malformed probe row")
            status = values[1]
            old, new, requested, applied, applied_low, residual, temperature, fraction = map(number, values[2:])
            mass, _, _, heat = case
            assert 253.15 <= case[1] <= 313.15
            exact = Fraction(old) + Fraction(heat)
            # These are the declared binary64 energy bounds, not a second phase solver.
            lower = Fraction(mass * 2052.0 * (253.15 - 273.15))
            latent = mass * 333500.0
            upper = Fraction(latent + mass * 4179.0 * (313.15 - 273.15))
            assert all(math.isfinite(v) for v in [old, new, requested, applied, applied_low,
                                                  residual, temperature, fraction])
            assert 253.15 <= temperature <= 313.15
            assert 0.0 <= fraction <= 1.0
            if status == 0:
                assert lower <= exact <= upper
                if heat == 0.0:
                    assert bits(new) == bits(old)
                else:
                    assert new == float(exact)
                assert requested == heat
                assert Fraction(new) - Fraction(old) == Fraction(applied) + Fraction(applied_low)
                assert Fraction(heat) == Fraction(applied) + Fraction(applied_low) + Fraction(residual)
                if 0.0 < new < latent:
                    assert 0.0 < fraction < 1.0
                counts["accepted_exact_balance"] += 1
            else:
                assert bits(old) == bits(new)
                assert applied == applied_low == residual == 0.0
                if lower <= exact <= upper:
                    assert status == 3  # inside-domain refusal must be explicitly numerical
                    rounded = float(exact)
                    assert rounded == old or (0 < rounded < latent and rounded / latent == 0.0)
                else:
                    assert status in [2, 3]  # range violation or unrepresentable sum
                counts["refused_atomically"] += 1
        except (AssertionError, OverflowError, ValueError) as error:
            raise AssertionError(f"case {index}, input={case}, probe={row}: {error}") from error
    assert counts["accepted_exact_balance"] > 1000
    assert counts["refused_atomically"] > 100
    report = {"transactions": len(inputs), **dict(counts), "compiler": args.compiler,
              "input_sha256": hashlib.sha256(payload.encode()).hexdigest(),
              "output_sha256": hashlib.sha256(result.stdout.encode()).hexdigest(),
              "library_sha256": hashlib.sha256(args.library.read_bytes()).hexdigest(),
              "arithmetic_reference": "Python fractions.Fraction; no tolerance for energy identities"}
    (args.work_dir / "exact-result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
