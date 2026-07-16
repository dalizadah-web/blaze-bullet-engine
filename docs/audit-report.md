# Blaze versus Stockfish 18: final audit

Date: 2026-07-16

## Scope and decision

This audit covers the tracked clean-room engine in `src/blaze`, its tests, and
its build graph. The unrelated legacy and vendor files that remain untracked at
the repository root are not part of Blaze and were not treated as implementation
inputs.

Blaze is a reliable legal-move UCI engine and a useful research base. It is not
currently close to Stockfish 18 in playing strength. The decisive local result
is a 0-20 score against the official Stockfish 18 AVX2 release. The games ended
after 48.1 plies on average. This is a chess-quality gap, not a packaging issue
or a small parameter-tuning gap.

There is no defensible absolute Elo number for Blaze from this sample. Elo is
pool-, hardware-, opening-, and time-control-dependent, and a zero score cannot
produce a finite estimate. The old 2950 target is also not equivalent to
beating Stockfish 18. The actionable target is a statistically significant
positive head-to-head score under a frozen test protocol.

## Evidence collected

### Correctness and reliability

- 93 unit/integration tests pass with zero failures.
- Start-position perft is exact through depth 6 (`119060324`); Kiwipete is exact
  through depth 4.
- Differential move-generation testing matches python-chess on 1000/1000 legal
  random positions at depth 3.
- Clean-room verification passes. No Stockfish source, network, book, or
  tablebase is in Blaze's build dependency graph or release binary.
- Repeated startup passes 10/10 launches.
- UCI lifecycle stress passes 1000/1000 cycles with one legal best move.

### Pinned Stockfish 18 opponent

- Release: official Stockfish 18, published 2026-01-31.
- Binary: `stockfish-windows-x86-64-avx2.exe` from the official `sf_18` release.
- SHA-256: `C86215FA1977D53B82ED854540A4C7B025BE4CD042276C85BA3DE53FB9118911`.
- Host: AMD Ryzen 7 7700, 8 cores / 16 logical processors, Windows 11.
- The audit copy lives only under ignored `build/audit`; it is not a Blaze
  dependency or distributable.

Stockfish's release notes report up to 46 Elo over Stockfish 17, four times as
many won game pairs as lost, the SFNNv10 network with threat inputs, refined
thread/hardware use, correction history, and a reproducible training workflow
using more than 100 billion Lc0-evaluated positions:
<https://stockfishchess.org/blog/2026/stockfish-18/>.

### Head-to-head result

Protocol:

- 20 games, paired colors, sequential openings from `openings.pgn`.
- 1 engine thread each, 64 MiB hash each.
- 2 seconds plus 0.02 seconds per move.
- Four games concurrently; 150 ms time margin.
- Official Stockfish 18 default embedded networks; Blaze classical evaluator.
- PGN: ignored audit artifact `build/audit/blaze-vs-sf18-20.pgn`.

Result:

| Engine | Wins | Draws | Losses | Score |
| --- | ---: | ---: | ---: | ---: |
| Blaze | 0 | 0 | 20 | 0.0% |
| Stockfish 18 | 20 | 0 | 0 | 100.0% |

Ten losses ended in mate and ten by conservative adjudication. Blaze scored
0-10 with White and 0-10 with Black.

A shallow Stockfish 18 diagnostic pass over all 481 Blaze moves found a median
loss of 18 centipawns, but 12.5% of moves lost at least one pawn and 4.2% lost
at least three pawns. The common pattern was gradual strategic deterioration
followed by a forced tactical finish, not only isolated one-move blunders. This
diagnostic used 5,000 nodes per comparison and is not an Elo measurement.

### Search benchmark

Ten deterministic legal positions, seed `20260716`, 1000 ms each:

| Engine/configuration | Average nodes | Observed depth |
| --- | ---: | ---: |
| Blaze, 1 thread | 488,140 | 7-10 |
| Stockfish 18 AVX2, 1 thread | 609,187 | 19-21 |
| Blaze, 4 threads | 1,327,754 | 7-9 |
| Stockfish 18 AVX2, 4 threads | 3,958,283 | 18-23 |

Node counts and reported depth are not directly comparable across engines.
They use different pruning, extensions, evaluation cost, and depth semantics.
The important observation is that Stockfish reaches much more selective search
depth and wins every game despite a modest one-thread raw-node difference.

## Capability comparison

| Area | Blaze now | Stockfish 18 baseline | Impact |
| --- | --- | --- | --- |
| Legal chess | Strict FEN, incremental make/unmake, complete standard move generation, exact tested perft | Mature standard chess and Chess960 core | Blaze's standard-chess foundation is sound |
| Default evaluation | Small handcrafted material/PST/mobility/pawn/king model | SFNNv10 neural evaluation with threat inputs | Critical strength gap |
| Neural inference | Optional 768x256 network; recomputes every hidden accumulator from all pieces; disabled by default | Incrementally updated, quantized, hardware-optimized networks enabled by default | Critical speed and accuracy gap |
| Training data | 8,000 random legal positions by default, labelled by a simple handcrafted teacher | Automated recipes over more than 100 billion Lc0-evaluated positions | Critical generalization gap |
| Main search | Iterative alpha-beta/PVS, TT, aspiration, null move, LMR, ProbCut-style probes, check/recapture extensions | Highly refined selective search with correction history and years of statistically accepted improvements | Critical depth/quality gap |
| Quiescence | Captures/promotions or all evasions; no TT, delta pruning, or losing-capture pruning | Mature selective tactical search | High tactical-efficiency gap |
| Move ordering | TT, MVV/LVA+SEE, killers, history, countermove | Mature staged picker with richer histories | High node-efficiency gap |
| Transposition table | Four-entry clusters protected by mutexes; modulo indexing | Shared high-throughput table used by scalable SMP search | High performance gap |
| Parallel search | Root split, maximum 8 threads | Refined SMP/NUMA behavior, maximum 1024 threads | High scaling and strength gap |
| Endgames | No tablebase probing | Syzygy WDL/DTZ options through seven pieces | Moderate strength/analysis gap |
| UCI/analysis | Hash, Threads, Ponder, UseNNUE, EvalFile; one final info line | MultiPV, WDL, Chess960, Syzygy, NUMA, move overhead, strength limiting, continuous analysis output | Product and analysis gap |
| Experimental method | Unit/perft/stress tests plus a basic score-only SPRT helper | Distributed Fishtest, paired games, STC/LTC and SMP regression testing | Critical development-velocity gap |

Stockfish's documented UCI surface and Lazy SMP terminology are available at
<https://official-stockfish.github.io/docs/stockfish-wiki/UCI-%26-Commands.html>
and <https://official-stockfish.github.io/docs/stockfish-wiki/Terminology.html>.

## Priority findings

### P0: the evaluator cannot support elite play

`tools/train_network.py` generates only 8,000 random positions and teaches the
network a simplified material/geometry score. It contains no game outcome,
deep-search target, tactical target, threat representation, validation split,
calibration, or data-quality gate. The runtime network is piece-square only and
rebuilds all 256 hidden values on every call. Until this is replaced by an
incremental evaluator trained on at least hundreds of millions and eventually
billions of well-labelled positions, search tuning will hit a low ceiling.

### P0: there is no statistically valid improvement loop

The repository has no match runner, paired opening corpus, pentanomial model,
experiment database, or automatic accept/reject gate. `tools/sprt.py` treats
draws as half a Bernoulli win; that is insufficient for modern high-draw engine
testing. Every functional patch needs reproducible paired STC and LTC testing,
otherwise tuning will select noise and regressions.

### P1: parallel NN evaluation is functionally disconnected

`search_parallel()` constructs each child `Searcher` with only the shared
transposition table. It does not pass `network_`. Any search with `Threads > 1`
therefore evaluates child positions classically even when `UseNNUE=true`. This
must be fixed and covered by a score-equivalence test before neural development.

### P1: the hot search path allocates and locks excessively

Every recursive child creates a `std::vector<Move>` principal variation. Every
TT probe/store takes a shared table lock and a cluster mutex, and indexing uses
integer modulo. Quiescence scans legal moves merely to detect stalemate before
evaluating non-check positions. Sliding attacks walk board rays. These choices
consume the budget that should be buying depth.

### P1: selective search is too shallow and coarse

LMR is three fixed thresholds; null-move reduction is nearly fixed; check and
recapture extensions are unconditional; ProbCut is a single fixed probe. There
is no reverse futility, razoring, late-move pruning, internal iterative
reduction, singular extension, capture history, continuation history,
correction history, or evaluation-trend feedback. Quiescence does not prune
obviously losing exchanges. These omissions explain much of the depth gap.

### P1: root-only parallelism wastes strength

Blaze re-creates workers and resets their histories at every iterative depth,
splits only root moves, and performs expensive full-window re-searches. It does
not let independent full-tree searches exchange discoveries through a shared
table and root voting. Four-thread node growth therefore does not translate to
Stockfish-like depth or playing strength.

### P2: time management and analysis feedback are minimal

Clock allocation is a fixed fraction with no best-move stability, score trend,
fail-high/fail-low, branching, or ponder-history feedback. The engine emits one
info record only after search. `go mate N` is mapped to depth `2N`; it is not a
mate-specific proof bound. These issues cost games and make analysis harder to
validate.

### P2: endgame and product completeness trail the reference

There is no Syzygy probing, Chess960, MultiPV, WDL output, hash occupancy,
selective depth, CPU feature dispatch, NUMA policy, or scalable thread limit.
These do not close the main evaluation/search gap, but they are required for a
credible Stockfish-class engine and fair tournament operation.

## What is already worth preserving

- Clean-room provenance and dependency verification.
- Strict position parsing and incremental state restoration.
- Exact perft and differential testing.
- Deterministic Zobrist keys and clustered TT semantics.
- Robust UCI stop/ponder lifecycle and legal best-move stress coverage.
- A small, understandable C++20 codebase that can be profiled and replaced in
  controlled stages.

The correct strategy is not a rewrite of the legal core and not a collection of
untested search tricks. Preserve the verified core, build the experiment system
first, then replace evaluation, hot-path data structures, search selectivity,
and SMP one independently measured change at a time.
