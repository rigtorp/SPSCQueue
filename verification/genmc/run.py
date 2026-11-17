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
            "-D__gthread_yield=__thread_yield", "-include", "iosfwd",
            "-D_GLIBCXX_OSTREAM_H=1",
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

    # GenMC v0.20.0 fails to promote llvm.memset emitted for aggregate/adjacent
    # zero-initialization on alignas(kFalseSharingRange) structs. Rewrite only
    # the temporary copy used by GenMC so the real header stays unmodified.
    header = (root / "include/rigtorp/SPSCQueue.h").read_text()
    genmc_rewrites = [
        (
            ": writer_{0, 0, nullptr, capacity, allocator} {",
            ": writer_(capacity, allocator), reader_() {",
        ),
        (
            """  struct alignas(kFalseSharingRange) {
    // Private copy of writeIdx avoids loading writeIdx_ on enqueue after a
    // spinning reader has pulled writeIdx_ into Shared state (reduces RTT).
    size_t writeIdx = 0;
    // Cached peer read index reduces cross-core cache coherency traffic on
    // readIdx_ until the queue appears full.
    size_t readIdxCache = 0;
    // Local copies of slots and capacity ride on the same private cache line
    // as the index cache to avoid extra cold-cache fetches of a shared header.
    T *slots = nullptr;
    size_t capacity = 0;""",
            """  struct alignas(kFalseSharingRange) Writer {
    explicit Writer(const size_t cap, const Allocator &alloc)
        : writeIdx(0), readIdxCache(0), capacity(cap), allocator(alloc) {}
    size_t writeIdx;
    T *slots;
    size_t readIdxCache;
    size_t capacity;""",
        ),
        (
            """  struct alignas(kFalseSharingRange) {
    // Private copy of readIdx avoids loading readIdx_ on the reader fast path
    // (shaving ~2 ns off RTT), at the cost of an extra store per pop (~4%
    // lower peak throughput).
    size_t readIdx = 0;
    // Cached peer write index reduces cross-core cache coherency traffic on
    // writeIdx_ until the queue appears empty.
    size_t writeIdxCache = 0;
    T *slots = nullptr;
    size_t capacity = 0;""",
            """  struct alignas(kFalseSharingRange) Reader {
    Reader() : readIdx(0), writeIdxCache(0) {}
    size_t readIdx;
    T *slots;
    size_t writeIdxCache;
    size_t capacity;""",
        ),
    ]
    for target, replacement in genmc_rewrites:
        if header.count(target) != 1:
            raise SystemExit("genmc rewrite target missing; update run.py")
        header = header.replace(target, replacement, 1)

    mutations = [
        ("relaxed-publication", "writeIdx_.store(nextWriteIdx, std::memory_order_release)", 2,
         "memory_order_release", r"Non-atomic race|Attempt to read from uninitialized memory"),
        ("relaxed-reclamation", "readIdx_.store(nextReadIdx, std::memory_order_release)", 2,
         "memory_order_release", r"Non-atomic race|Unordered writes"),
        ("relaxed-acquire-consumer", "reader_.writeIdxCache = writeIdx_.load(std::memory_order_acquire)", 2,
         "memory_order_acquire", r"Non-atomic race|Attempt to read from uninitialized memory"),
        ("relaxed-acquire-producer", "writer_.readIdxCache = readIdx_.load(std::memory_order_acquire)", 2,
         "memory_order_acquire", r"Non-atomic race|Unordered writes"),
    ]
    with tempfile.TemporaryDirectory(prefix="spsc-genmc-") as directory:
        base_include = Path(directory) / "baseline"
        base_dest = base_include / "rigtorp/SPSCQueue.h"
        base_dest.parent.mkdir(parents=True)
        base_dest.write_text(header)

        for capacity in args.capacities:
            items = args.items if args.items is not None else capacity + 2
            check(f"transfers-capacity-{capacity}", base_include, capacity, items)
            check(f"attempts-capacity-{capacity}", base_include, capacity, items,
                  completed=False)

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
