# Blaze Engine-Strength Modernization Design

**Date:** 2026-07-18

**Status:** Approved for detailed planning

## Objective

Turn Blaze into a substantially stronger, faster, and more reliable custom chess
engine. Architecture compatibility is not a goal: production search, evaluation,
transposition-table, attack generation, move generation, SEE, move ordering,
selective search, parallel search, and time management may all be replaced when
the replacement has stronger evidence.

Blaze remains independently engineered. Stockfish 18 may be used as an external
UCI opponent, oracle, and source of publicly described concepts. Its source,
constants, implementation details, and network weights are not Blaze inputs.

## Non-negotiable constraints

- Work only on `codex/bullet-beast` in the existing isolated worktree.
- Preserve accepted commit `297926e2700134eb0a6d7c16663b4a23ff4b1b34`.
- Do not merge or push unless the user later requests it.
- Make one coherent mechanism per candidate commit.
- Keep classical and neural evaluation changes separate from search changes.
- Keep old hot-path implementations only as temporary debug oracles; remove
  competing production paths after differential validation.
- Record all accepted and rejected experiments. Do not tune on confirmation
  openings or silently change binaries, networks, runners, or settings.

## Program architecture

The modernization proceeds through seven dependent layers. A later layer cannot
be used to hide a failed earlier gate.

### 1. Correctness foundation

Introduce explicit `Root`, `PV`, and `NonPV` search semantics. Correct and test
null-move eligibility and verification, parallel depth-one root search,
extension budgeting, maximum-ply behavior in check, rule-50-safe TT use,
mate-distance bounds, repetition behavior, and SEE promotion/en-passant/king
safety.

Pruning remains conservative until this layer passes tactical, deterministic,
and match gates. Every pruning mechanism must state which node types, depths,
checks, mate windows, and material classes permit it.

### 2. Measurement foundation

Create a versioned deterministic search corpus that records FEN, configuration,
best move, score, PV, completed depth, selective depth, node count, and
termination reason. Add separate, game-disjoint tuning and confirmation opening
suites large enough to avoid the current ten-position correlation problem.

An immutable accepted-baseline manifest binds source commit, binary hash,
compiler and flags, network hash, opening hash, match-runner hash, UCI settings,
hardware class, and benchmark signature. Infrastructure failures are
quarantined; genuine crashes, illegal moves, and time losses are reported
separately.

### 3. Hot-path core

Replace the current mutex-heavy TT with cache-line-aligned packed clusters and
low-contention concurrent access. Use fast indexing, prefetch, depth/age/bound
replacement, static evaluation, PV metadata, `hashfull`, and rule-50 protection.

Replace ray-walking production sliding attacks with generated lookup tables and
runtime CPU dispatch. Add cached occupancies, checkers, and pins, then direct
legal generation for captures, quiets, evasions, quiet checks, and all moves.
The current generators remain exhaustive differential oracles until large
seeded parity runs pass.

Replace recursive position-copying SEE with an allocation-free bitboard swap
list and `see_ge(move, threshold)`. Build a lazy staged MovePicker with this
production order:

1. TT move.
2. Good captures and promotions.
3. Killers and countermove.
4. High-history quiets.
5. Remaining quiets.
6. Bad captures.

Search stacks, repetition storage, PV storage, and node accounting use fixed
capacity and perform no per-node allocation, thread creation, or global locking.

### 4. Selective search

Add or replace mechanisms independently in this order:

1. Mate-distance pruning and progressive aspiration.
2. Stack and TT static evaluation with improving/opponent-worsening signals.
3. Reverse futility and guarded razoring.
4. Parent/child futility, late-move pruning, and SEE pruning.
5. Internal iterative reductions when no reliable TT move exists.
6. Logarithmic adaptive LMR.
7. Rich histories and correction histories.
8. Improved ProbCut.
9. Singular verification, multicut, and negative extensions.
10. First-class qsearch.

LMR begins with a depth/move-number table and adjusts reductions using node type,
expected node role, improving, TT evidence, checks, tactical status, move and
continuation histories, and reduced-search outcome. Adjustments are bounded and
individually testable.

Qsearch receives TT bounds, mate bounds, direct evasions, no stand-pat in check,
staged good/bad captures, promotions, selected checks, delta pruning, SEE
threshold pruning, and capture-count/late-move controls. A slow unpruned qsearch
remains a test oracle.

### 5. Search learning and independent research

Use saturating and decaying quiet, capture, continuation, pawn, low-ply,
countermove, TT-move, and correction histories. Reward successful moves and
penalize searched failures. Histories persist appropriately across searches and
belong to long-lived workers rather than being reset by root tasks.

Research candidates are developed in isolated commits and never bundled with a
standard mechanism:

- Search confidence from score volatility, root stability, TT agreement, and
  history reliability, bounded to at most one ply of LMR adjustment.
- Tactical density from checks, captures, pins, promotions, and SEE distribution
  to restrict risky pruning.
- Root-effort entropy to allocate time and diversify workers.
- History reliability estimates so sparse or stale tables have less influence.
- Volatility-aware aspiration and time budgets.

Each candidate requires an ablation, deterministic evidence, performance data,
and its own match gate.

### 6. Evaluation

First strengthen evaluation/search interfaces: cache raw and corrected static
evaluations, define stable score semantics, and add phase/material/tactical
diagnostics. Keep the classical evaluator as a transparent reference and
fallback, but do not make constant tuning the long-term strength strategy.

Build a custom incremental neural evaluator with independently specified
side-relative king, pawn, piece-square, attack, and threat features; incremental
accumulators; refresh caching; schema hashes; quantization metadata; and
bit-identical scalar/SIMD kernels. Publicly licensed positions and external UCI
labels are permitted with complete provenance. No third-party network weights
or feature implementation are imported.

Network training and network promotion remain separate from engine-code
candidates. A network becomes default only after inference parity, throughput,
held-out calibration, fixed-node/fixed-time evaluation, and paired matches.

### 7. SMP, UCI, and time management

Replace per-iteration root splitting with persistent full-tree workers. Each
worker owns its stack, histories, evaluator caches, and local statistics while
sharing the packed TT, immutable network, stop state, and root information.
Use deterministic diversification and root voting based on completed depth,
score, agreement, and effort.

Create a persistent UCI search worker with generation IDs, cancellation,
stale-result suppression, reusable state, and clean shutdown. `ponderhit`
converts the current search to clocked mode without restarting.

Time management has immutable hard deadlines and adaptive soft deadlines based
on best-move stability, score trend and variance, aspiration failures, root move
count, root-effort distribution, ponder outcome, remaining time/increment,
measured overhead, and recent deadline error. Emit continuous UCI depth,
seldepth, score, nodes, NPS, time, hashfull, current move, PV, and eventually WDL.

## Evidence and promotion ladder

Every candidate passes the following ladder against the immediately preceding
accepted baseline:

1. Focused unit tests and, where meaningful, mutation tests.
2. Full C++ and Python suites.
3. Canonical perft and large seeded differential move generation.
4. UCI lifecycle/stress and clean-room verification.
5. Deterministic fixed-node corpus comparison.
6. Fixed-node and fixed-time performance benchmarks.
7. A 1,000-game color-paired non-regression gate divided across local and cloud
   workers, with platform strata preserved.
8. Larger sequential STC/LTC confirmation for promising small effects.

The 1,000-game gate detects crashes, lifecycle errors, time losses, and material
regressions; it is not presented as a precise detector of small Elo changes.
Reports include wins, draws, losses, pentanomial counts, time losses, crashes,
illegal moves, binary hashes, configuration hashes, and hardware strata.

An accepted manifest is promoted only after all declared gates pass. A rejected
candidate and its evidence remain recorded so the same failed idea is not
accidentally rediscovered or folded into a bundle.

## Significant-improvement criteria

The program optimizes playing strength first, then speed subject to strength.
“Significant” is demonstrated cumulatively, not inferred from code volume:

- Zero known legality, state-restoration, repetition, mate-bound, or UCI
  lifecycle defects in the covered corpora.
- Materially higher useful NPS and lower effective branching factor on the
  versioned benchmark suite without tactical regression.
- Positive sequential STC evidence for the combined accepted search series,
  followed by LTC confirmation on disjoint openings.
- No material regression on any supported platform/compiler stratum.
- Stable one-thread behavior before multi-thread scaling claims.
- Measurable SMP scaling and zero engine-attributable time losses in the
  qualification matrix.

No fixed Elo target ends research automatically. After each combined
confirmation, profiling identifies the next largest measured weakness and the
cycle continues until further improvement is compute- or evidence-limited.

## Error handling and safety

- TT races must be torn-read safe and validated with stress/sanitizer builds.
- Malformed UCI output, missing artifacts, hash drift, partial match shards, and
  duplicate games fail closed.
- Search cancellation cannot emit stale `bestmove` results.
- Maximum ply, checks, mates, repetitions, and rule-50 boundaries have explicit
  safe behavior rather than falling back silently to ordinary static evaluation.
- Unsupported SIMD paths fall back to a bit-identical scalar implementation.
- Experiment artifacts are immutable after the first game.

## Initial execution boundary

The first implementation plan begins with the remaining correctness foundation:
explicit node types and verified non-PV null move, then parallel depth-one root
search, extension budget, maximum-ply check safety, rule-50-safe TT behavior,
mate-distance pruning, and SEE edge-case tests. Measurement upgrades needed to
judge those candidates are included before aggressive selective search.

The accepted ProbCut commit remains the base. Neural distillation work is paused
until the search and measurement foundations produce trustworthy labels and
comparisons.
