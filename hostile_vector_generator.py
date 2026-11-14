#!/usr/bin/env python3
"""
Project Shuzi (v8.4) - Hostile 4-Vector Chaos Dataset Generator
Generates deterministic or custom-seeded 6-column ASCII CSV payloads
to stress-test black-box quarantine engines and state machines.

Usage:
  python3 hostile_vector_generator.py --lines 100000 --seed 20261115
  python3 hostile_vector_generator.py --lines 1000000000 --seed 99999  # Full 1B (~44.5GB)
"""

import os
import time
import random
import argparse
from datetime import datetime


def extract_valid_templates(ref_file="hostile_engineer_challenge_10m.csv", sample_size=500):
    valid_lines = []
    if os.path.exists(ref_file):
        with open(ref_file, "rb") as f:
            for _ in range(sample_size):
                line = f.readline()
                if not line:
                    break
                if len(line) < 200 and b"\x00" not in line and line.count(b",") >= 4:
                    valid_lines.append(line.strip())
    return valid_lines


def build_4_vector_attacks(templates, base_ts=1731500000000):
    compact_attacks = []
    heavy_attacks = []

    # [Vector 1] State-Machine Thrashing / Yo-Yo Attack (1ms boundary oscillation)
    yoyo_states = [
        b"1.0,210.0,-65.0,STANDBY,0",
        b"0.5,210.0,-80.0,PRANK,0",
        b"7.9,850.0,-20.0,DISTRESS,1",
        b"1.2,210.0,-65.0,OK,0",
        b"8.1,850.0,-15.0,DISTRESS,1"
    ]
    for i in range(100):
        ts_ms = base_ts + i
        state = yoyo_states[i % len(yoyo_states)]
        compact_attacks.append(f"{ts_ms},".encode() + state)

    for base in templates[:40]:
        parts = base.split(b",")
        if len(parts) < 5:
            continue

        c0 = parts[0]
        c1 = parts[1]
        rest = b",".join(parts[2:])

        # [Vector 2] Arithmetic Overflow & Floating-Point Traps
        compact_attacks.append(b"NaN," + c1 + b"," + rest)
        compact_attacks.append(c0 + b",-Infinity," + rest)
        compact_attacks.append(b"9223372036854775807," + c1 + b"," + rest)
        compact_attacks.append(b"-9223372036854775808," + c1 + b"," + rest)
        long_decimal = b"7." + (b"9" * 100)
        compact_attacks.append(c0 + b"," + long_decimal + b"," + rest)
        compact_attacks.append(c0 + b"," + c1 + b",850\x00.0," + rest)

        # [Vector 3 - Compact] 0-byte empty line, control chars, unclosed quote
        compact_attacks.append(b"")
        compact_attacks.append(c0 + b",\x00\x08\r\n\r\n\xff\xfe\xfd\x1b[31m," + rest)
        compact_attacks.append(c0 + b',"85.5,999.9,DISTRESS,1')

        # [Vector 3 - Heavy] 32KB line & 2048-column explosion
        pad_32k = os.urandom(32768).replace(b"\n", b"A").replace(b"\r", b"B").replace(b",", b"C")
        heavy_attacks.append(base + b"," + pad_32k)
        heavy_attacks.append(c0 + (b"," * 2048) + rest)

        # [Vector 4] Replay Attack & Timestamp Reversal (-10 years, +100 years, spoofed token)
        past_ts = str(base_ts - 315360000000).encode()
        future_ts = str(base_ts + 3153600000000).encode()
        compact_attacks.append(past_ts + b",8.1,850.0,-15.0,DISTRESS,1,HASH_SPOOF_DEADBEAF")
        compact_attacks.append(future_ts + b",0.0,210.0,-65.0,UNLOCK_BYPASS,999,TOKEN_REPLAY_0x00")

    return compact_attacks, heavy_attacks


def generate_hostile_dataset(prefix="shuzi_hostile", total_lines=100_000, seed=20261115):
    run_id = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"{prefix}_{run_id}_{total_lines}.csv"

    templates = extract_valid_templates()
    if not templates:
        templates = [
            b"1731500001000,1.2,210.0,-65.0,STANDBY,0",
            b"1731500002000,8.1,850.0,-20.0,DISTRESS,1",
            b"1731500003000,0.5,210.0,-80.0,PRANK,0",
            b"1731500004000,1.0,210.0,-65.0,OK,0"
        ]

    base_ts = 1731500000000
    compact_attacks, heavy_attacks = build_4_vector_attacks(templates, base_ts)
    rng = random.Random(seed)

    batch_size = min(20000, total_lines)
    num_batches = max(1, min(50, total_lines // batch_size))
    micro_batches = []

    print(f"[GEN] Building {num_batches} randomized micro-batches (seed={seed})...")
    for b_idx in range(num_batches):
        batch = []
        current_ts = base_ts + (b_idx * batch_size * 10)
        for _ in range(batch_size):
            r = rng.random()
            if r < 0.85:
                current_ts += rng.randint(1, 10)
                tpl_parts = rng.choice(templates).split(b",")
                tpl_parts[0] = str(current_ts).encode()
                batch.append(b",".join(tpl_parts))
            elif r < 0.999:
                batch.append(rng.choice(compact_attacks))
            else:
                batch.append(rng.choice(heavy_attacks))
        micro_batches.append(b"\n".join(batch) + b"\n")

    start_t = time.time()
    print(f"[GEN] Writing {total_lines:,} hostile vectors to {filename}...")

    with open(filename, "wb", buffering=16 * 1024 * 1024) as f:
        f.write(templates[0] + b"\n")
        written = 1
        while written < total_lines:
            remaining = total_lines - written
            if remaining >= batch_size:
                f.write(rng.choice(micro_batches))
                written += batch_size
            else:
                lines = rng.choice(micro_batches).splitlines()[:remaining]
                f.write(b"\n".join(lines) + b"\n")
                written += len(lines)

            if (written - 1) % 100_000_000 == 0 and written > 1:
                gb = f.tell() / (1024 ** 3)
                print(f"[PROGRESS] {written-1:,} / {total_lines:,} lines written ({gb:.1f} GB)...")

    elapsed = time.time() - start_t
    size_mb = os.path.getsize(filename) / (1024 ** 2)
    print(f"[DONE] {filename} ready ({size_mb:.2f} MB in {elapsed:.2f}s)")
    return filename


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Project Shuzi v8.4 Hostile CSV Vector Generator")
    parser.add_argument("--lines", type=int, default=100_000, help="Total CSV lines to generate (default: 100,000; 1B = 1000000000)")
    parser.add_argument("--seed", type=int, default=20261115, help="Random seed for reproducible or custom chaos runs")
    parser.add_argument("--prefix", type=str, default="shuzi_hostile", help="Output filename prefix")
    args = parser.parse_args()

    generate_hostile_dataset(prefix=args.prefix, total_lines=args.lines, seed=args.seed)
