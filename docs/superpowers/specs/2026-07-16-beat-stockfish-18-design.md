# Clean-room Stockfish 18 challenger design

Date: 2026-07-16

## Objective

Evolve Blaze into an independently implemented CPU chess engine that is
stronger than the pinned official Stockfish 18 AVX2 release under a public,
reproducible head-to-head protocol. No Stockfish source, neural network,
opening book, or compiled component may enter Blaze's source tree, dependency
graph, training inputs, or release artifacts.

This objective is a research target, not a guaranteed delivery date. A claim of
success is permitted only after the qualification gates in this design pass.

## Definition of "better than Stockfish 18"

Primary protocol:

- Opponent SHA-256:
  `C86215FA1977D53B82ED854540A4C7B025BE4CD042276C85BA3DE53FB9118911`.
- Standard chess, AMD64, equal hardware, equal thread count, equal hash, equal
  tablebase access, and paired colors from the same opening.
- Eight physical cores per engine and 1024 MiB hash for the primary test.
- STC: 10 seconds plus 0.1 seconds per move.
- LTC: 60 seconds plus 0.6 seconds per move.
- A versioned, license-audited, unbalanced opening suite with every opening
  repeated once with colors reversed.
- Pentanomial SPRT with `alpha=0.05`, `beta=0.05`, `elo0=0`, and `elo1=5`.
- The candidate must pass both STC and LTC, then score above 50% with a 95%
  confidence interval excluding 50% in a 60,000-game eight-thread regression.
- The final test must be repeated on a second x86-64 machine family.

Secondary gates:

- No illegal move, crash, timeout loss, duplicate bestmove, or corrupt PGN in
  100,000 tournament games.
- Exact standard-chess perft and differential move generation remain green.
- Analysis score is calibrated to empirical WDL and continuous UCI output is
  protocol-correct.
- A clean rebuild reproduces the binary, network identity, dataset manifests,
  and match configuration.

No absolute Elo label is part of the acceptance criterion.

## Considered approaches

### A. Independent CPU alpha-beta engine (selected)

Preserve Blaze's verified position core, then build a modern experiment loop,
incremental neural evaluator, high-throughput search core, selective search, and
full-tree SMP. This respects the clean-room requirement and produces a genuinely
custom engine. It is the most engineering-intensive route and requires large
CPU/GPU experiment budgets.

### B. GPL fork of Stockfish 18

Start from Stockfish 18 and attempt to land statistically winning patches. This
is the shortest technical route to "Stockfish 18 plus improvement," but the
result is a Stockfish derivative under GPLv3 and violates the selected
clean-room/custom boundary. It is rejected for this project.

### C. GPU policy/value search engine

Build an MCTS or hybrid neural engine around a large GPU model. This can create
a differentiated engine, but equal-hardware comparison becomes ambiguous and
the inference/training budget rises sharply. It is retained only as a later
research branch; the primary challenger remains CPU-only.

## System architecture

The program is split into seven independently testable subsystems.

### 1. Experiment and qualification service

A local runner launches paired games through Cute Chess or FastChess, validates
the exact engine/network/opening hashes, records machine and compiler metadata,
and computes pentanomial SPRT. A coordinator later distributes immutable match
chunks to homogeneous workers and merges signed result manifests.

Functional code changes cannot merge into the current-best branch unless their
own STC gate passes. Larger changes must also pass LTC. Diagnostic suites can
reject a change but cannot establish an Elo gain by themselves.

### 2. Position and hot-path core

The existing `Position` API remains the source of truth while internals gain:

- cached color occupancy, checker, pinned-piece, and attack metadata;
- independently generated constant-time sliding attacks selected by CPU feature;
- direct generation modes for captures, quiets, evasions, and quiet checks;
- an incremental evaluator accumulator stored in `StateInfo` or a dedicated
  stack frame;
- fixed-capacity search stacks and principal-variation buffers;
- a packed concurrent TT without per-probe mutexes.

Every replacement is differential-tested against the current implementation
before the old path is removed.

### 3. Selective alpha-beta search

Search is expressed through explicit root, PV, non-PV, and quiescence node
types. A staged move picker produces TT move, good captures, tactical
promotions, killers/countermoves, scored quiets, and deferred bad captures.

The selective layer adds only independently tested concepts: mate-distance
bounds, adaptive null move with verification, reverse futility, razoring,
late-move pruning, adaptive LMR, internal iterative reduction, singular
extensions, SEE/delta quiescence pruning, capture/continuation/pawn histories,
and correction histories. Each concept has a safety predicate and an isolated
SPRT result. Search constants live in a typed parameter registry and are tuned
only on training openings; confirmation uses a disjoint opening set.

### 4. Independent neural evaluation

The evaluator uses two side-relative sparse accumulators with king context,
piece-square relations, pawn structure, and independently designed attack/threat
features. Integer inference is incrementally updated on make/unmake and has
scalar, AVX2, and AVX-512 kernels with bit-identical outputs.

Training mixes search scores and game outcomes, uses game-disjoint
train/validation/test splits, quantization-aware fine-tuning, explicit draw/WDL
calibration, and reproducible recipe manifests. Data comes from Blaze self-play
and separately approved, license-compatible sources. Every shard records origin,
license, generator version, seed, and checksum. No Stockfish evaluation or net
is a training label.

Data scale milestones are 100 million, 1 billion, 10 billion, and—if scaling
curves still improve—100 billion positions. The default release embeds or
cryptographically pins the accepted network; classical evaluation remains only
as a debug oracle.

### 5. Parallel search and time control

Long-lived worker threads run diversified full-tree searches sharing the TT and
network but keeping local search stacks and histories. Root voting combines
depth, score, and node effort. CPU topology discovery assigns workers to physical
cores and later NUMA nodes. The thread limit expands beyond eight only after
scaling tests justify it.

Time management maintains soft and hard deadlines. It reacts to best-move
stability, score trend, aspiration failures, legal-move count, prior ponder hit,
and remaining increment. Stop polling is bounded so no search can exceed the
configured move overhead.

### 6. Endgame and UCI surface

Syzygy WDL/DTZ probing is optional and uses user-supplied files; tablebases are
not shipped. The engine adds Chess960, MultiPV, WDL output, selective depth,
hash occupancy, current move information, move overhead, clear hash, and
continuous info reporting. Tournament defaults are deterministic and strength
features are explicitly versioned.

### 7. Research differentiators

Catching up requires implementing established engine fundamentals; surpassing
Stockfish 18 requires an additional source of strength. Three branches are
allowed after the baseline challenger is within 50 Elo at LTC:

1. a small policy head used only for move ordering and reduction confidence;
2. uncertainty-aware search that spends depth where value confidence is low;
3. a gated fortress/endgame expert trained on self-play adjudication failures.

Only one branch enters a candidate at a time. It must beat the same base at STC
and LTC and retain inference speed on AVX2 hardware.

## Data flow

1. A released Blaze generator plays paired self-play and diverse-opponent games.
2. Immutable game records are sampled into position shards with provenance.
3. A deeper released Blaze search supplies score/PV targets; final game result
   supplies WDL targets.
4. The trainer creates checkpoints and reports held-out loss, calibration, and
   saturation by phase/material bucket.
5. Quantized candidates pass bit-exact scalar/SIMD tests and fixed-position
   evaluation tests.
6. Candidates enter paired net matches; only a passing net becomes the default.
7. Every accepted engine/net pair receives a versioned benchmark signature and
   becomes the baseline for subsequent SPRT tests.

## Failure handling

- A worker with a crash, illegal move, time loss, engine hash mismatch, or
  malformed PGN quarantines its whole result chunk.
- Dataset shards with missing provenance, checksum mismatch, duplicate game
  identity, train/test leakage, or impossible positions are rejected before
  training.
- A network with incompatible dimensions, checksum, quantization scale, or SIMD
  parity fails closed before UCI reports `readyok`.
- Search assertions produce a reproducible FEN, move history, seed, limits, and
  engine hash.
- Any functional patch that fails SPRT is reverted from the candidate branch;
  several individually failed ideas are not bundled to manufacture a pass.

## Resource envelope

The following is a planning estimate, not a guarantee:

- Core/search/infra: two to four experienced engine engineers for 6-18 months.
- Neural/data: one ML engineer plus one data/infra engineer.
- Testing: initially 100,000 CPU-core-hours, expanding toward 500,000 or more as
  the engine approaches parity and draw rate rises.
- Training: 10-100 modern GPU-days through the first billion-position models,
  with scale determined by measured learning curves.
- Storage: several terabytes for compressed position shards, checkpoints,
  manifests, and tournament records.

A solo desktop effort can substantially improve Blaze, but a credible attempt
to surpass Stockfish 18 needs distributed testing and training capacity. The
qualification gates do not weaken if that capacity is unavailable.

## Delivery sequence

1. Measurement and clean-room gates.
2. Correctness blockers and profiling.
3. Hot-path position, TT, PV, and move-picker work.
4. Selective search with per-feature SPRT.
5. Incremental evaluator runtime and data pipeline.
6. Successive neural data-scale milestones.
7. Full-tree SMP and adaptive time management.
8. Syzygy, Chess960, and analysis completeness.
9. Research differentiators.
10. Stockfish 18 STC, LTC, eight-thread, second-machine qualification.

The companion implementation plan maps this sequence to concrete files, tests,
commands, commits, and acceptance gates.
