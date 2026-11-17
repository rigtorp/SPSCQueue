#!/usr/bin/env python3
"""Bounded RC11 checks of the real header, with synchronization mutations."""

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--genmc", default="genmc")
    parser.add_argument("--capacities", type=int, nargs="+", default=[1, 2, 3])
    parser.add_argument("--items", type=int, help="default: capacity + 2 (after priming)")
    parser.add_argument("--unroll", type=int, default=32)
    parser.add_argument("--timeout", type=int, default=120, help="seconds per check")
    parser.add_argument("--output", type=Path, default=Path("build/genmc"))
    args = parser.parse_args()
    if min(args.capacities) < 1 or args.unroll < 1 or args.timeout < 1:
        parser.error("capacities, unroll, and timeout must be positive")
    if args.items is not None and args.items < 1:
        parser.error("items must be positive")
    if not shutil.which(args.genmc):
        parser.error("genmc not found; use the Nix environment documented in README.md")

    root = Path(__file__).resolve().parents[2]
    harness = root / "verification/genmc/queue.cpp"
    args.output.mkdir(parents=True, exist_ok=True)

    def check(label, include, capacity, items, expected_error=None, completed=True):
        command = [
            args.genmc, "-rc11", "-disable-mm-detector", "-disable-estimation",
            f"-unroll={args.unroll}", "--", "-std=c++20", "-fno-sized-deallocation",
            "-D_GLIBCXX_GCC_GTHR_H", "-include", "bits/gthr-single.h",
            f"-I{include}", f"-DQUEUE_CAPACITY={capacity}", f"-DQUEUE_ITEMS={items}",
            f"-DQUEUE_COMPLETED_TRANSFERS={int(completed)}",
            str(harness),
        ]
        print(f"{label}: capacity={capacity}, items={items}, unroll={args.unroll}", flush=True)
        log = args.output / f"{label}.log"
        try:
            result = subprocess.run(command, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True, timeout=args.timeout)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b""
            log.write_text(output.decode(errors="replace") if isinstance(output, bytes) else output)
            raise SystemExit(f"{label}: timed out; no verification result; see {log}") from error
        log.write_text(result.stdout)
        executions = re.search(r"Number of complete executions explored: (\d+)", result.stdout)
        blocked = re.search(r"Number of blocked executions seen:", result.stdout)
        if expected_error:
            passed = result.returncode == 42 and re.search(expected_error, result.stdout)
        else:
            passed = (result.returncode == 0 and executions and int(executions[1]) > 0
                      and (completed or not blocked))
        if not passed:
            print(result.stdout)
            raise SystemExit(f"{label}: unexpected result (exit {result.returncode}); see {log}")
        if expected_error:
            print(f"  detected expected memory-safety error; {log}", flush=True)
        else:
            print(f"  passed, {executions[1]} complete executions; {log}", flush=True)

    for capacity in args.capacities:
        items = args.items if args.items is not None else capacity + 2
        check(f"transfers-capacity-{capacity}", root / "include", capacity, items)
        check(f"attempts-capacity-{capacity}", root / "include", capacity, items,
              completed=False)

    # Mutate copies only: a compiler failure or unsupported operation is not an
    # accepted negative result. Both mutations must produce a memory-safety error.
    header = (root / "include/rigtorp/SPSCQueue.h").read_text()
    mutations = [
        ("relaxed-publication", "writeIdx_.store(nextWriteIdx, std::memory_order_release)", 2,
         "memory_order_release", r"Non-atomic race|Attempt to read from uninitialized memory"),
        ("relaxed-reclamation", "readIdx_.store(nextReadIdx, std::memory_order_release)", 2,
         "memory_order_release", r"Non-atomic race|Unordered writes"),
        ("relaxed-acquire-consumer", "writeIdxCache_ = writeIdx_.load(std::memory_order_acquire)", 2,
         "memory_order_acquire", r"Non-atomic race|Attempt to read from uninitialized memory"),
        ("relaxed-acquire-producer", "readIdxCache_ = readIdx_.load(std::memory_order_acquire)", 2,
         "memory_order_acquire", r"Non-atomic race|Unordered writes"),
    ]
    with tempfile.TemporaryDirectory(prefix="spsc-genmc-") as directory:
        for label, original, count, order, expected_error in mutations:
            if header.count(original) != count:
                raise SystemExit(f"{label}: header changed; update the mutation target")
            include = Path(directory) / label
            destination = include / "rigtorp/SPSCQueue.h"
            destination.parent.mkdir(parents=True)
            destination.write_text(header.replace(original, original.replace(
                order, "memory_order_relaxed")))
            check(label, include, 1, 5, expected_error=expected_error)


if __name__ == "__main__":
    main()
