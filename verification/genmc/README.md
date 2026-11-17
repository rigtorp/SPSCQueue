# Bounded Weak-Memory Verification (GenMC)

The optional [GenMC](https://github.com/MPI-SWS/genmc) harness includes the
queue header and checks one producer and one consumer under RC11. From the
repository root, use the pinned Nix environment (Linux):

```sh
nix develop path:./verification/genmc --command python3 verification/genmc/run.py
```

## Scenarios

The default checks use capacities 1, 2, and 3. A sequential fill/drain first
checks full/empty behavior and positions both indices at the last ring index.
The concurrent phase then transfers respectively 3, 4, and 5 elements, crossing
two wrap boundaries:

- **Completed transfers (`transfers-capacity-*`)**: assumes each
  enqueue/dequeue attempt succeeds; executions where an attempt fails are
  blocked rather than retried.
- **Partial histories (`attempts-capacity-*`)**: makes the same number of
  attempts without those assumptions, retaining full/empty failures and
  checking any remaining elements after joining the threads.

Both scenarios assert FIFO order, two-word payload publication, and final
emptiness. Payload destruction writes to the slot to exercise synchronization
before reuse. GenMC also checks for races on non-atomic storage.

## Mutation checks

The runner checks temporary copies of the header with synchronization operations
weakened to `memory_order_relaxed`:

- Producer publication (`writeIdx_.store(..., memory_order_release)`) and
  consumer acquire (`reader_.writeIdxCache = writeIdx_.load(memory_order_acquire)`)
  must be rejected for an uninitialized read or race.
- Consumer reclamation (`readIdx_.store(..., memory_order_release)`) and
  producer acquire (`writer_.readIdxCache = readIdx_.load(memory_order_acquire)`)
  must be rejected for a race or unordered writes.

Compilation errors and timeouts are treated as failures, not successful checks.
The repository header is never mutated. Logs are written to `build/genmc/`.

## Scope and limitations

These are bounded safety checks, not a proof for arbitrary capacities, element
types, allocators, or operation counts, and not a proof of progress or general
linearizability. Concurrent operations exercise `try_emplace`, `front`, and
`pop`; they do not cover every API overload. Loops are bounded to 32 iterations
at the LLVM IR level; the runner requires at least one complete execution per
positive check.

It uses GenMC's thread API, ordinary `std::atomic` operations, unsized
deallocation, and libstdc++'s single-thread backend for unrelated
standard-library internals that GenMC does not model. To work around GenMC
v0.20.0's `llvm.memset` promotion failure on `alignas(kFalseSharingRange)`
structs with adjacent zero-initialized fields, `run.py` rewrites the temporary
header copy to initialize those fields individually without changing the memory
orders or synchronization logic. Use `--help` to change capacities, item count,
loop bound, timeout, or log directory. Nix and GenMC are not required for normal
builds.
