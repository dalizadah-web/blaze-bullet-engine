# Clean-Room Search and UCI Implementation Plan

**Goal:** Turn the verified Blaze position core into a standalone, responsive UCI engine with deterministic evaluation, iterative alpha-beta search, a race-safe transposition table, enforceable time limits, and reliability tests.

**Boundary:** All production code lives under `src/blaze`; no legacy or vendored engine source is compiled or copied. Strength features are accepted only after correctness and protocol tests remain green.

## Task 1: Classical Bootstrap Evaluation

Create `src/blaze/eval/classical.{h,cpp}` and `tests/eval/test_classical.cpp`. First write failing tests for side-to-move symmetry, material ordering, positional differentiation, and bounded mate-safe values. Implement independently authored material, piece-square, mobility, pawn, and king-safety terms. Commit as `feat: add deterministic Blaze evaluation`.

## Task 2: Clustered Transposition Table

Create `src/blaze/search/transposition_table.{h,cpp}` and `tests/search/test_transposition_table.cpp`. First test replacement, exact/lower/upper bounds, generation aging, resize/clear, mate-score normalization, and multi-threaded probe/store stress. Implement fixed-size clusters with synchronized access and no mutable shared entry reads. Commit as `feat: add race-safe clustered transposition table`.

## Task 3: Correct Alpha-Beta Search

Create `src/blaze/search/search.{h,cpp}` and `tests/search/test_search.cpp`. First test mate-in-one, stalemate, checkmate scores, legal best moves, node/depth limits, principal variation legality, and deterministic repetition/50-move handling. Implement iterative deepening, negamax alpha-beta/PVS, quiescence, TT probing, terminal detection, and cooperative cancellation. Commit as `feat: add iterative Blaze search`.

## Task 4: Search Ordering and Selectivity

Extend search tests with tactical positions and node-count regression fixtures. Add staged ordering for TT move, captures, killers, and history; then add aspiration windows, null-move pruning, late-move reductions, check extensions, and conservative futility/razoring one feature at a time. Require identical legal results and a lower fixed-depth node count before each feature remains. Commit each strength feature separately.

## Task 5: Time Management and Asynchronous UCI

Create `src/blaze/uci/limits.{h,cpp}`, `src/blaze/uci/session.{h,cpp}`, `src/blaze/main.cpp`, and UCI tests. First test parsing for `position`, `go`, `setoption`, malformed input, movetime, clock/increment, moves-to-go, node/depth limits, `stop`, `ponderhit`, and repeated `go/stop/isready/quit`. Implement one input thread plus one joinable search worker; ensure stop is idempotent and every accepted search emits exactly one legal `bestmove`. Commit as `feat: add responsive asynchronous UCI engine`.

## Task 6: Production Build and Reliability Gates

Add the `blaze.exe` target, UCI transcript driver, 1,000-cycle protocol stress, sanitizer-compatible build, clean-room binary scan, and deterministic benchmark command. Record executable/compiler/flags hashes. Commit as `test: gate Blaze engine reliability`.

## Task 7: Strength Measurement Handoff

Add reproducible match manifests and scripts for paired openings, Elo confidence intervals, and SPRT. Establish a local baseline against an external Stockfish executable without linking or importing it. Only after the reliability gate passes, begin isolated evaluation/search tuning and the independently trained Blaze-network phase.
