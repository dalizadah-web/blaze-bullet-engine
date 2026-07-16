# Blaze clean-room audit

Date: 2026-07-16

## Executive result

Blaze is now a self-contained, warning-clean C++20 chess engine with strict UCI
lifecycle handling, legal move generation, incremental make/unmake, a clustered
transposition table, iterative alpha-beta/PVS search, selective null move/LMR,
aspiration windows, check extensions, and a classical evaluation function.

The implementation is not a 2950-Elo engine and there is no evidence that it
beats Stockfish. The current code is a sound platform for further engine work,
but the requested rating target requires a substantially stronger search and a
trained evaluation network.

## Correctness and reliability evidence

- 93 unit/integration tests pass with zero failures.
- Canonical perft is exact through start-position depth 6 (`119060324`) and
  Kiwipete depth 4.
- Differential random-position testing matches python-chess on 1000/1000
  positions at depth 3.
- Clean-room verification passes: no Stockfish/vendor source or network data is
  compiled or shipped by the clean-room target.
- Repeated process gate: 10/10 clean launches.
- UCI stress gate: 1000/1000 cycles return one legal best move, including stop,
  infinite, ponder, and readiness traffic.
- The network loader rejects missing, malformed, dimension-mismatched, and
  checksum-invalid files. `UseNNUE=true` now constructs the independently
  specified quantized evaluator and fails closed rather than silently using an
  unvalidated fallback.

## Measured strength/performance

The local opponent is `stockfish.exe`, identified as Stockfish 17.1. It is not
the current official Stockfish release; Stockfish 18 was released on 2026-01-31
([official release](https://stockfishchess.org/blog/2026/stockfish-18/)).

On ten deterministic legal positions at 1000 ms per move:

| engine | average nodes | observed depth |
| --- | ---: | ---: |
| Blaze | 530,985 | 7-9 |
| local Stockfish 17.1 | 1,132,981 | 20-25 |

Blaze therefore searches about 47% as many nodes as this local Stockfish build
at the same wall-clock budget. The latest pinned 2+0.02 smoke match completed
10 games with 0 wins, 0 draws, and 10 losses for Blaze. This is not a formal Elo
rating, but it is decisive evidence that the 2950/Stockfish-beating requirement
has not been met.

## What remains for a custom Stockfish-beating engine

The clean-room base can support the work, but beating modern Stockfish is not a
small tuning change. The next high-value engineering stages are:

1. Replace the classical-only default with an independently trained NNUE-style
   evaluator and add a verified training/data pipeline.
2. Replace the naive root split with a proper shared-tree SMP search; the current
   multi-thread mode is race-safe but slower than one thread in benchmarks.
3. Add stronger tactical/selective search (SEE capture pruning, extensions,
   correction history, singular/ProbCut-style pruning) with per-feature tests.
4. Add a reproducible self-play tournament and SPRT gate against a pinned
   Stockfish binary before accepting any Elo claim.

An optional local network can be generated with `mingw32-make -f Makefile.blaze
train-network` and enabled through UCI `EvalFile` plus `UseNNUE`; it is not part
of the default release binary.

No Stockfish source, network, book, or tablebase asset is used by the clean-room
engine. The Polyglot reader and network evaluator are disabled unless explicitly
configured; the trainer generates optional local networks from generated legal
positions and a transparent teacher.
