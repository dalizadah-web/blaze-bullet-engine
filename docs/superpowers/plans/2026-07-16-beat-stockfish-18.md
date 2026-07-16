# Stockfish 18 Challenger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and statistically qualify an independently implemented Blaze release that scores positively against the pinned official Stockfish 18 AVX2 binary.

**Architecture:** Preserve the verified position core while adding a trustworthy experiment loop, a low-allocation search hot path, independently designed incremental neural evaluation, adaptive selective search, and full-tree SMP. Every functional change is isolated and accepted by paired-game SPRT; the final claim requires equal-hardware STC, LTC, eight-thread, and second-machine qualification.

**Tech Stack:** C++20, MinGW g++, Python 3.13, python-chess, NumPy, PyTorch/CUDA for training, Cute Chess or FastChess for games, JSON/JSONL manifests, AVX2/AVX-512 integer inference, optional user-supplied Syzygy files.

## Global Constraints

- No Stockfish source, neural network, opening book, compiled component, or evaluation output may enter Blaze's source tree, dependency graph, training labels, or release artifacts.
- The pinned qualification opponent SHA-256 is `C86215FA1977D53B82ED854540A4C7B025BE4CD042276C85BA3DE53FB9118911`.
- Standard-chess correctness, exact perft, differential move generation, UCI lifecycle stress, and clean-room verification must remain green after every task.
- Functional patches merge only after their own paired STC gate; architectural releases also require LTC.
- Dataset, network, opening, engine, compiler, and match artifacts require cryptographic hashes and provenance manifests.
- The final claim requires pentanomial SPRT `alpha=0.05`, `beta=0.05`, `elo0=0`, `elo1=5` at both 10+0.1 and 60+0.6, followed by a positive 60,000-game eight-thread regression and a second x86-64 machine family.
- No absolute Elo claim is permitted from a tactical suite, node benchmark, or small match.

---

### Task 1: Replace the score-only test helper with a reproducible paired-match gate

**Files:**
- Create: `tools/experiment/__init__.py`
- Create: `tools/experiment/pentanomial.py`
- Create: `tools/experiment/match.py`
- Create: `tools/experiment/manifest.py`
- Create: `tools/experiment/test_pentanomial.py`
- Create: `tools/experiment/test_manifest.py`
- Create: `config/matches/sf18-stc.json`
- Create: `config/matches/sf18-ltc.json`
- Create: `config/matches/patch-stc.json`
- Create: `config/matches/patch-ltc.json`
- Create: `testdata/openings/smoke-v1.epd`
- Create: `provenance/openings.json`
- Modify: `Makefile.blaze`
- Deprecate after parity: `tools/sprt.py`

**Interfaces:**
- Produces: `Pentanomial(wins2, wins1_draw1, draws2, losses1_draw1, losses2)`.
- Produces: `sprt_llr(counts, elo0, elo1) -> float` and `sprt_decision(...) -> Literal["accept", "reject", "continue"]`.
- Produces: `MatchSpec.from_json(path)`, `run_match(spec) -> MatchResult`, and `write_manifest(result, path)`.
- Consumes: exact engine paths, SHA-256 hashes, opening hash, time control, concurrency, thread/hash settings, seed, and Cute Chess/FastChess path.

- [ ] **Step 1: Add failing pentanomial unit tests**

```python
import math
import unittest
from tools.experiment.pentanomial import Pentanomial, sprt_decision, sprt_llr

class PentanomialTests(unittest.TestCase):
    def test_color_swapped_pair_mapping(self):
        counts = Pentanomial.from_pair_scores([(1.0, 1.0), (1.0, 0.5), (0.5, 0.5), (0.0, 0.5), (0.0, 0.0)])
        self.assertEqual(counts.as_tuple(), (1, 1, 1, 1, 1))

    def test_symmetric_results_are_neutral(self):
        counts = Pentanomial(20, 40, 80, 40, 20)
        self.assertAlmostEqual(sprt_llr(counts, 0.0, 5.0), 0.0, delta=0.25)

    def test_decisive_positive_pairs_accept(self):
        self.assertEqual(sprt_decision(Pentanomial(500, 100, 20, 0, 0), 0.0, 5.0, 0.05, 0.05), "accept")
```

- [ ] **Step 2: Run the tests and verify import failure**

Run: `python -m unittest tools.experiment.test_pentanomial -v`

Expected: failure because `tools.experiment.pentanomial` does not exist.

- [ ] **Step 3: Implement the pentanomial model and immutable match schema**

```python
@dataclass(frozen=True)
class Pentanomial:
    wins2: int
    wins1_draw1: int
    draws2: int
    losses1_draw1: int
    losses2: int

    def as_tuple(self) -> tuple[int, int, int, int, int]:
        return (self.wins2, self.wins1_draw1, self.draws2,
                self.losses1_draw1, self.losses2)
```

Use the five pair outcomes as the observations in the likelihood computation;
do not collapse them to single-game scores. Reject an odd number of games, a
missing reverse-color partner, a hash mismatch, or a worker result without a
complete PGN.

- [ ] **Step 4: Add frozen opponent and patch match configurations**

`sf18-stc.json` uses 10+0.1, paired openings, one thread during patch testing,
64 MiB hash, and the pinned opponent hash. `sf18-ltc.json` changes only the time
control to 60+0.6. Both set `alpha=0.05`, `beta=0.05`, `elo0=0`, `elo1=5`.
`patch-stc.json` and `patch-ltc.json` use the same controls but accept candidate
and baseline hashes from the experiment invocation instead of pinning SF18.
Create the smoke EPD from independently generated legal Blaze positions and
record its generator, seed, license, and SHA-256 in `provenance/openings.json`.

- [ ] **Step 5: Add Make targets and run the harness tests**

Run: `mingw32-make -f Makefile.blaze experiment-test`

Expected: all experiment unit tests pass and the existing C++ test target is
unchanged.

- [ ] **Step 6: Reproduce the 20-game audit as a harness smoke test**

Run: `python -m tools.experiment.match --config config/matches/sf18-stc.json --games 20 --output build/experiments/sf18-smoke`

Expected: 20 valid paired games, exact hashes in `manifest.json`, no time loss or
illegal move, and a result matching the current qualitative conclusion that
Blaze loses decisively.

- [ ] **Step 7: Commit**

```text
git add Makefile.blaze config/matches testdata/openings provenance/openings.json tools/experiment
git commit -m "test: add paired pentanomial experiment gate"
```

### Task 2: Fix neural evaluation across parallel search

**Files:**
- Modify: `src/blaze/search/search.cpp`
- Modify: `tests/search/test_search.cpp`
- Modify: `tests/uci/test_session.cpp`

**Interfaces:**
- Consumes: `Searcher(TranspositionTable&, const NetworkEvaluator*)`.
- Produces: identical evaluator selection for every root worker and descendant.

- [ ] **Step 1: Write a failing parallel-network equivalence test**

Create a deterministic test network whose score on a queen-up FEN differs by at
least 300 centipawns from the classical evaluator. Search the same position to
fixed depth with one and four threads and assert identical score sign, mate
status, and legal best move. Also assert that changing the supplied network
changes the four-thread root score.

```cpp
CHECK_EQ(sign(many.score), sign(one.score));
CHECK(root.is_legal(many.best_move));
CHECK(std::abs(many.score - classical.score) >= 300);
```

- [ ] **Step 2: Run the focused test and verify failure**

Run: `mingw32-make -f Makefile.blaze test`

Expected: the four-thread network assertion fails because child searchers use
the classical evaluator.

- [ ] **Step 3: Pass the evaluator into every parallel worker**

```cpp
Searcher child_searcher(table_, network_);
```

Keep evaluator ownership in `UciSession`; workers receive a read-only pointer
whose lifetime exceeds the joined search.

- [ ] **Step 4: Verify fixed-depth and UCI behavior**

Run: `mingw32-make -f Makefile.blaze test uci-stress verify-clean-room`

Expected: all tests and 1000 UCI cycles pass; one- and four-thread neural tests
use the same evaluation implementation.

- [ ] **Step 5: Commit**

```text
git add src/blaze/search/search.cpp tests/search/test_search.cpp tests/uci/test_session.cpp
git commit -m "fix: preserve neural evaluation in parallel search"
```

### Task 3: Remove recursive heap allocation from principal variations and search state

**Files:**
- Create: `src/blaze/search/pv_line.h`
- Create: `src/blaze/search/stack.h`
- Modify: `src/blaze/search/search.h`
- Modify: `src/blaze/search/search.cpp`
- Modify: `tests/search/test_search.cpp`
- Modify: `tools/benchmark.py`

**Interfaces:**
- Produces: `PvLine::clear()`, `PvLine::prepend(Move, const PvLine&)`, `PvLine::span()`.
- Produces: `SearchStackEntry` with current move, static evaluation, extension
  count, killers, excluded move, and continuation-history pointer.
- Keeps: public `SearchResult::pv` as `std::vector<Move>` only at the UCI boundary.

- [ ] **Step 1: Add fixed-capacity PV tests**

```cpp
PvLine child;
child.prepend(move_b, PvLine{});
PvLine parent;
parent.prepend(move_a, child);
CHECK_EQ(parent.size(), 2U);
CHECK_EQ(parent[0], move_a);
CHECK_EQ(parent[1], move_b);
```

- [ ] **Step 2: Implement the fixed PV container**

```cpp
class PvLine {
public:
    static constexpr std::size_t capacity = 128;
    void clear() noexcept { size_ = 0; }
    void prepend(Move move, const PvLine& child) noexcept;
    [[nodiscard]] std::span<const Move> span() const noexcept;
private:
    std::array<Move, capacity> moves_{};
    std::uint8_t size_ = 0;
};
```

`prepend` copies at most `capacity - 1` child moves. No recursive search call may
construct or grow a vector.

- [ ] **Step 3: Convert negamax and quiescence to stack/PvLine parameters**

Allocate `std::array<SearchStackEntry, 132>` and root `PvLine` once in
`Searcher::search`. Pass an index/pointer to recursion. Convert the final root
span to `SearchResult::pv` after an iteration completes.

- [ ] **Step 4: Add benchmark telemetry**

Extend `tools/benchmark.py` to emit median and interquartile nodes per second
over at least three repetitions. Record compiler flags, engine hash, and CPU
name beside the result so timing noise is visible.

- [ ] **Step 5: Verify correctness and speed**

Run: `mingw32-make -f Makefile.blaze test verify-clean-room`

Run: `python tools/benchmark.py --engine build/blaze/blaze.exe --milliseconds 1000 --positions 10`

Expected: 93+ tests pass, PVs remain legal, and median NPS does not regress by
more than 1%. A larger speedup is useful but not required for this structural
task.

- [ ] **Step 6: Commit**

```text
git add src/blaze/search tests/search/test_search.cpp tools/benchmark.py
git commit -m "perf: move search PV and stack state off the heap"
```

### Task 4: Replace the locked transposition table with a packed concurrent table

**Files:**
- Modify: `src/blaze/search/transposition_table.h`
- Modify: `src/blaze/search/transposition_table.cpp`
- Modify: `tests/search/test_transposition_table.cpp`
- Modify: `tests/search/test_search.cpp`

**Interfaces:**
- Keeps: `store`, `probe`, `resize`, `clear`, `new_search`, `capacity`.
- Adds: `prefetch(std::uint64_t key)` and `hashfull() -> int` in permille.
- Produces: power-of-two cluster count and mask-based indexing.

- [ ] **Step 1: Add packing, replacement, and torn-read tests**

```cpp
static_assert(std::is_trivially_copyable_v<TTData>);
CHECK(table.capacity() >= requested_entries);
CHECK(table.hashfull() >= 0);
CHECK(table.hashfull() <= 1000);
```

Extend the current stress test so eight writers/readers operate for at least one
million operations. Every successful probe must return a valid bound, move, and
depth range; a missed/torn entry is allowed to appear only as a miss.

- [ ] **Step 2: Implement atomic key/payload entries**

Use two 64-bit atomics per entry. Writers publish payload then verification key
with release ordering. Readers load key, payload, then key again with acquire
ordering and accept only matching key reads. Pack move, signed score, signed
depth, bound, generation, and a key fragment into the payload. Do not place a
mutex in a cluster or take a table-wide lock during probe/store.

- [ ] **Step 3: Use mask indexing and prefetch**

Round cluster count down to the nearest power of two and store
`cluster_mask_ = cluster_count_ - 1`. Replace `% cluster_count_` with
`key & cluster_mask_`. Prefetch the next child position's cluster before the
recursive call.

- [ ] **Step 4: Run race stress, fixed-depth search, and benchmark**

Run: `mingw32-make -f Makefile.blaze test`

Expected: concurrent stress passes and fixed-depth single/parallel scores remain
stable. The deterministic benchmark must improve median NPS by at least 10% or
the implementation is profiled and revised before merge.

- [ ] **Step 5: Pass an STC non-regression match**

Run: `python -m tools.experiment.match --candidate build/blaze/blaze.exe --baseline build/baselines/pre-packed-tt.exe --config config/matches/patch-stc.json`

Expected: SPRT accepts non-regression under the dedicated simplification bounds
recorded in the experiment manifest.

- [ ] **Step 6: Commit**

```text
git add src/blaze/search/transposition_table.* tests/search
git commit -m "perf: add packed concurrent transposition table"
```

### Task 5: Add fast attack generation and mode-specific legal move generation

**Files:**
- Create: `src/blaze/core/sliding_attacks.h`
- Create: `src/blaze/core/sliding_attacks.cpp`
- Create: `src/blaze/core/check_info.h`
- Modify: `src/blaze/core/attacks.cpp`
- Modify: `src/blaze/core/movegen.h`
- Modify: `src/blaze/core/movegen.cpp`
- Modify: `src/blaze/core/position.h`
- Modify: `tests/core/test_attacks.cpp`
- Modify: `tests/core/test_movegen.cpp`
- Modify: `tools/differential_perft.py`

**Interfaces:**
- Produces: `enum class GenType { Captures, Quiets, Evasions, QuietChecks, All }`.
- Produces: `generate<GenType>(const Position&, const CheckInfo&, MoveList&)`.
- Produces: `CheckInfo` containing checkers, pinned pieces, king square, and pin
  rays.

- [ ] **Step 1: Add exhaustive sliding-attack parity tests**

For every square and every subset of its relevant rook/bishop occupancy mask,
compare the new lookup result to the existing ray walker. Test both scalar and
BMI2 paths on capable hardware.

- [ ] **Step 2: Implement generated lookup tables with CPU dispatch**

Generate table contents from the existing independent ray function at startup
or compile time. Use BMI2 `pext` where available and a portable compressed-index
fallback. The verifier must record the generator and reject hard-coded external
attack tables.

- [ ] **Step 3: Add check/pin-aware generation tests**

Cover single check, double check, absolute pins, en-passant discovered check,
castling through attack, every promotion, and positions with 218 legal moves.
Assert that each generation mode is disjoint where intended and their union
equals `All`.

- [ ] **Step 4: Implement direct legal generation**

Generate king evasions from the enemy attack map, restrict double check to king
moves, mask pinned piece targets to their pin ray, and apply the special
en-passant occupancy test. Keep the current make/unmake legality filter behind a
debug flag and assert parity during tests.

- [ ] **Step 5: Run exhaustive correctness gates**

Run: `mingw32-make -f Makefile.blaze test perft-driver`

Run: `python tools/differential_perft.py --driver build/blaze/perft_driver.exe --positions 10000 --depth 3 --seed 20260716`

Expected: exact canonical perft and 10000/10000 differential positions.

- [ ] **Step 6: Benchmark and STC-test the complete replacement**

Expected: at least 20% median NPS improvement in move-generation-heavy
positions and no STC regression versus the immediately previous baseline.

- [ ] **Step 7: Commit**

```text
git add src/blaze/core tests/core tools/differential_perft.py
git commit -m "perf: add check-aware mode-specific move generation"
```

### Task 6: Introduce a staged move picker and disciplined quiescence search

**Files:**
- Create: `src/blaze/search/move_picker.h`
- Create: `src/blaze/search/move_picker.cpp`
- Modify: `src/blaze/search/see.cpp`
- Modify: `src/blaze/search/search.cpp`
- Create: `tests/search/test_move_picker.cpp`
- Modify: `tests/search/test_search.cpp`

**Interfaces:**
- Produces: `MovePicker::next() -> Move` with TT, good-capture, promotion,
  killer/countermove, quiet, and bad-capture stages.
- Produces: `qsearch(Position&, AlphaBetaWindow, SearchStackEntry*)` with TT,
  delta, SEE, and check-evasion handling.

- [ ] **Step 1: Test stage completeness and uniqueness**

For tactical, quiet, and in-check FENs, consume the picker and assert every legal
move appears exactly once, TT move appears first when legal, non-losing captures
precede quiets, and deferred losing captures remain available.

- [ ] **Step 2: Implement lazy stage generation**

Do not score or sort quiets until the quiet stage is reached. Score captures by
SEE, victim value, promotion, and capture history. Use selection of the next
highest score instead of sorting an unused tail.

- [ ] **Step 3: Add qsearch safety tests**

Use positions where a nominally losing capture is the only check evasion, where
a promotion changes the delta bound, where stalemate has no captures, and where
an en-passant capture opens a rook line. Compare pruned and unpruned qsearch
scores on a fixed corpus.

- [ ] **Step 4: Implement qsearch TT/delta/SEE pruning**

Probe/store qsearch bounds. Never stand-pat in check. Apply delta pruning only
outside check and outside mate-score windows. Skip a losing capture only when it
is not a promotion, check evasion, or checking move and the SEE margin proves it
cannot raise alpha.

- [ ] **Step 5: Verify and measure**

Run all tests, the tactical corpus, deterministic benchmark, and paired STC.
Accept only if correctness is identical and SPRT passes; node reduction without
playing-strength evidence is insufficient.

- [ ] **Step 6: Commit**

```text
git add src/blaze/search tests/search
git commit -m "search: add staged move picking and selective qsearch"
```

### Task 7: Build adaptive selective search and multi-dimensional histories

**Files:**
- Create: `src/blaze/search/history.h`
- Create: `src/blaze/search/parameters.h`
- Create: `src/blaze/search/node.h`
- Split from: `src/blaze/search/search.cpp`
- Create: `src/blaze/search/search_node.cpp`
- Modify: `tests/search/test_search.cpp`
- Create: `tests/search/test_history.cpp`
- Create: `config/search/default-parameters.json`

**Interfaces:**
- Produces: typed saturating histories for quiet, capture, continuation, pawn,
  countermove, and correction signals.
- Produces: explicit `NodeType::{Root, Pv, NonPv}` template or equivalent typed
  dispatch.
- Produces: one versioned `SearchParameters` object included in experiment
  manifests.

- [ ] **Step 1: Add history saturation/decay tests**

```cpp
HistoryScore score;
score.update(4000);
CHECK(score.value() <= HistoryScore::maximum);
score.update(-4000);
CHECK(score.value() >= HistoryScore::minimum);
```

- [ ] **Step 2: Move existing history/killers/countermoves behind the new API**

This refactor must preserve the fixed-depth benchmark signature. Run 5,000
paired non-regression games before adding new selectivity.

- [ ] **Step 3: Add concepts one commit and one SPRT at a time**

Implement in this order: mate-distance pruning, reverse futility, razoring,
late-move pruning, internal iterative reduction, adaptive LMR, null-move
verification, singular extension, capture history, continuation history, pawn
history, then correction histories. Every concept receives:

1. a tactical/safety regression FEN;
2. a parameter entry with a documented unit and range;
3. an STC experiment against the immediately previous accepted binary;
4. an LTC confirmation after STC accepts.

- [ ] **Step 4: Constrain extensions**

Track extension budget on `SearchStackEntry`. Check, recapture, and singular
extensions must be conditional and cannot increase a path by more than the
tested budget. Add forced-mate and perpetual-check tests that reach maximum
search ply without overflow.

- [ ] **Step 5: Tune only on the training opening split**

Use SPSA or Bayesian search over bounded parameters. Freeze the chosen vector,
then run a fresh confirmation on a disjoint opening corpus and both STC/LTC.
Store all tried vectors, including failures, to prevent winner's-curse reuse.

- [ ] **Step 6: Commit each accepted feature separately**

Example commit sequence:

```text
git commit -m "search: add adaptive late-move reductions"
git commit -m "search: add continuation history"
git commit -m "search: correct static evaluation from search history"
```

### Task 8: Implement the version-2 incremental neural evaluator

**Files:**
- Create: `src/blaze/eval/features_v2.h`
- Create: `src/blaze/eval/accumulator.h`
- Create: `src/blaze/eval/accumulator.cpp`
- Create: `src/blaze/eval/simd.h`
- Create: `src/blaze/eval/simd_scalar.cpp`
- Create: `src/blaze/eval/simd_avx2.cpp`
- Create: `src/blaze/eval/simd_avx512.cpp`
- Modify: `src/blaze/eval/network.h`
- Modify: `src/blaze/eval/network.cpp`
- Modify: `src/blaze/core/position.h`
- Modify: `src/blaze/core/position.cpp`
- Create: `tests/eval/test_accumulator.cpp`
- Modify: `tests/eval/test_network_loader.cpp`

**Interfaces:**
- Produces: `FeatureSchemaV2::active_features(Position, Perspective)`.
- Produces: `Accumulator::refresh`, `apply_move`, `undo_move`, and
  `NetworkEvaluator::evaluate(const Position&, const Accumulator&)`.
- Produces: scalar/AVX2/AVX-512 kernels with bit-identical `std::int32_t` output.

- [ ] **Step 1: Specify and freeze the feature schema**

Use side-relative king context, piece-square relations, pawn relations, and
independently designed attack/threat buckets. Assign every feature a stable
integer ID and serialize the schema hash into the network header. Version 1
networks remain loadable only through the explicit legacy option.

- [ ] **Step 2: Add refresh-versus-incremental parity tests**

For 100,000 seeded legal move sequences, refresh from the board after every move
and compare with the incrementally updated accumulator. Include captures,
promotions, en passant, both castles, null moves, and complete unmake sequences.

- [ ] **Step 3: Implement accumulator deltas in state restoration**

Record added/removed feature IDs in a fixed-capacity delta stored beside
`StateInfo`. Ordinary moves update only affected piece and attack features;
king-bucket changes may trigger a perspective refresh. Unmake replays the exact
inverse delta.

- [ ] **Step 4: Add SIMD parity and throughput tests**

Run 1,000,000 fixed accumulator inferences through scalar and each supported SIMD
kernel. Require exact score equality and record evaluations per second. Select
the kernel at startup from detected CPU capabilities.

- [ ] **Step 5: Keep v2 explicit until an accepted network exists**

Package a small smoke-test v2 network for tests only. Production `readyok` must
fail closed when v2 is explicitly requested without a valid network. Do not make
v2 the default until Task 9 promotes a network through a paired match. Keep
classical evaluation available through a debug-only UCI option, not as silent
fallback after v2 becomes the default.

- [ ] **Step 6: Verify and commit**

Run all unit/perft/differential/clean-room gates before committing.

```text
git add src/blaze/eval src/blaze/core tests/eval tests/core
git commit -m "eval: add independent incremental neural evaluator v2"
```

### Task 9: Build the provenance-first self-play and neural training pipeline

**Files:**
- Create: `training/requirements.lock`
- Create: `training/schema.py`
- Create: `training/generate.py`
- Create: `training/shard.py`
- Create: `training/dataset.py`
- Create: `training/model.py`
- Create: `training/train.py`
- Create: `training/quantize.py`
- Create: `training/validate.py`
- Create: `training/recipes/v2-100m.json`
- Create: `training/recipes/v2-1b.json`
- Create: `training/tests/test_schema.py`
- Create: `training/tests/test_split.py`
- Create: `training/tests/test_quantize.py`
- Create: `provenance/training-dependencies.json`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: immutable `GameRecord` and `PositionRecord` schemas with engine hash,
  network hash, seed, ply, FEN/key, search score, WDL result, policy visits, and
  source license.
- Produces: `DatasetManifest` with shard hashes and game-disjoint split IDs.
- Produces: quantized `BLAZENET` v2 plus training recipe and validation report.

- [ ] **Step 1: Add schema and leakage tests**

```python
def test_game_id_never_crosses_splits():
    split = split_games(records, seed=20260716)
    assert ids(split.train).isdisjoint(ids(split.validation))
    assert ids(split.train).isdisjoint(ids(split.test))
    assert ids(split.validation).isdisjoint(ids(split.test))
```

Reject impossible kings, invalid side-to-move, duplicate position identity in a
single split, unlicensed source, missing checksum, or generator hash mismatch.

- [ ] **Step 2: Generate the 100-million-position dataset**

Use released Blaze self-play with randomized legal openings, temperature only
in the opening, adjudication safeguards, and a mixture of time/node budgets.
Sample quiet, tactical, opening, middlegame, and endgame buckets explicitly.
Label with deeper released-Blaze search and final WDL; never label with
Stockfish.

- [ ] **Step 3: Train with a mixed score/WDL objective**

Train sparse accumulators plus output head using game-disjoint validation,
phase/material reweighting, quantization noise, and early stopping on held-out
calibration. Emit loss, Brier score, WDL calibration, and bucket metrics for
every checkpoint.

- [ ] **Step 4: Quantize and verify runtime parity**

Require the C++ scalar runtime and Python reference to agree within one
centipawn on 100,000 held-out positions. Require scalar/SIMD exact equality.

- [ ] **Step 5: Run paired network selection**

Each candidate network plays the current default with fixed engine code. Rank
only after a paired SPRT; validation loss alone cannot promote a net. After the
first v2 net passes STC and LTC, make it the production default and add its
identity to `provenance/core-dependencies.json` as a shipped data asset.

- [ ] **Step 6: Scale through 1B and 10B only when curves justify it**

At each scale, fit strength versus log(data) and validation calibration. Proceed
to the next recipe only if held-out metrics and paired Elo still improve. Add a
100B recipe only if the 10B run remains data-limited.

- [ ] **Step 7: Commit pipeline code and manifests, not bulk shards**

```text
git add training Makefile.blaze provenance/training-dependencies.json provenance/core-dependencies.json
git commit -m "train: add provenance-first neural data pipeline"
```

### Task 10: Replace root splitting with long-lived full-tree SMP and adaptive clocks

**Files:**
- Create: `src/blaze/search/thread_pool.h`
- Create: `src/blaze/search/thread_pool.cpp`
- Create: `src/blaze/search/time_manager.h`
- Create: `src/blaze/search/time_manager.cpp`
- Modify: `src/blaze/search/search.h`
- Modify: `src/blaze/search/search.cpp`
- Modify: `src/blaze/uci/session.cpp`
- Modify: `src/blaze/uci/limits.cpp`
- Create: `tests/search/test_thread_pool.cpp`
- Create: `tests/search/test_time_manager.cpp`

**Interfaces:**
- Produces: `ThreadPool::set_size`, `start_search`, `stop`, `wait`, and root
  result voting.
- Produces: `TimeManager::soft_deadline`, `hard_deadline`, and
  `update_after_iteration(IterationSignals)`.

- [ ] **Step 1: Test lifetime, stop, and neural sharing**

Create/destroy pools at sizes 1, 2, 4, 8, and 16; run repeated go/stop/ponder;
assert no duplicate bestmove, deadlock, or evaluator mismatch. Workers remain
alive between iterative depths and are joined only on resize/shutdown.

- [ ] **Step 2: Implement diversified full-tree workers**

Each worker owns stack, histories, accumulator stack, and root ordering while
sharing the TT and immutable network. Diversify initial aspiration/root order
with deterministic worker IDs. Combine results by completed depth, score,
best-move agreement, and searched nodes.

- [ ] **Step 3: Add topology-aware assignment**

Detect logical/physical cores, default to physical cores for qualification, and
expose an explicit UCI policy. Record topology and affinity in every experiment
manifest. Add NUMA placement only after multi-socket hardware is available for
testing.

- [ ] **Step 4: Implement soft/hard time control**

Calculate a hard deadline from remaining time, increment, moves-to-go, and move
overhead. Adjust the soft deadline after each completed iteration using
best-move stability, score change, aspiration failures, legal move count, and
ponder hit. The hard deadline is never extended.

- [ ] **Step 5: Run scaling and time-forfeit gates**

Measure 1/2/4/8/16-thread NPS and, more importantly, fixed-time Elo. Run 100,000
games over bullet through LTC with randomized GUI traffic and require zero time
forfeits attributable to Blaze.

- [ ] **Step 6: Pass SMP STC/LTC before commit**

The new path must pass one-thread non-regression and eight-thread positive SPRT
against the root-split baseline.

- [ ] **Step 7: Commit**

```text
git add src/blaze/search src/blaze/uci tests/search
git commit -m "search: add full-tree SMP and adaptive time management"
```

### Task 11: Add tablebase, Chess960, and analysis-complete UCI behavior

**Files:**
- Create: `src/blaze/endgame/syzygy.h`
- Create: `src/blaze/endgame/syzygy.cpp`
- Modify: `src/blaze/core/position.h`
- Modify: `src/blaze/core/position.cpp`
- Modify: `src/blaze/core/movegen.cpp`
- Modify: `src/blaze/uci/session.cpp`
- Modify: `src/blaze/uci/session.h`
- Create: `tests/endgame/test_syzygy.cpp`
- Create: `tests/core/test_chess960.cpp`
- Modify: `tests/uci/test_session.cpp`

**Interfaces:**
- Produces: optional `Tablebase::probe_wdl` and `probe_root_dtz` using
  user-supplied files.
- Produces: `Variant::{Standard, Chess960}` position/castling semantics.
- Adds UCI: Clear Hash, MultiPV, Move Overhead, UCI_Chess960, UCI_ShowWDL,
  SyzygyPath/ProbeDepth/50MoveRule/ProbeLimit, `seldepth`, `hashfull`, `currmove`,
  `nps`, and periodic info output.

- [ ] **Step 1: Add option parsing and protocol transcript tests**

Assert exact option ranges, safe behavior for missing tablebase paths, periodic
info ordering, WDL normalization, MultiPV line count, and Clear Hash behavior.

- [ ] **Step 2: Implement Chess960 castling semantics**

Represent rook origins explicitly, parse/emit compatible FEN rights, handle
king/rook overlap cases, and add canonical Chess960 perft positions. Standard
chess keys and perft must remain unchanged.

- [ ] **Step 3: Integrate optional Syzygy probing**

Probe only within configured piece/depth limits and preserve the 50-move rule.
Translate WDL/DTZ to search bounds and root ordering. Do not ship tablebase data.

- [ ] **Step 4: Verify tournament compatibility**

Run Cute Chess standard/Chess960 matches, 1000-cycle UCI stress, and tablebase
unit fixtures. Run a paired tablebase-on/off match to quantify strength; keep
the feature only if behavior is correct even when Elo is neutral.

- [ ] **Step 5: Commit**

```text
git add src/blaze/endgame src/blaze/core src/blaze/uci tests
git commit -m "feat: add Chess960 Syzygy and analysis UCI support"
```

### Task 12: Scale experiment execution without weakening statistical controls

**Files:**
- Create: `tools/experiment/coordinator.py`
- Create: `tools/experiment/worker.py`
- Create: `tools/experiment/result_store.py`
- Create: `tools/experiment/openings.py`
- Create: `tools/experiment/test_coordinator.py`
- Create: `config/workers/schema.json`
- Create: `provenance/qualification-openings.json`
- Create: `.github/workflows/blaze-ci.yml`

**Interfaces:**
- Produces: immutable signed `WorkChunk` and `ChunkResult` JSON schemas.
- Produces: coordinator deduplication by experiment ID, opening range, reverse
  color, engine hashes, and worker hardware class.

- [ ] **Step 1: Test duplicate, partial, and hostile chunks**

The coordinator must reject duplicate games, wrong hashes, odd/unpaired results,
mixed CPU classes, missing PGNs, impossible outcomes, and worker clock drift
beyond the configured tolerance.

- [ ] **Step 2: Implement resumable immutable chunks**

Assign complete color-swapped opening pairs to one worker. Store raw PGN and
manifest before updating aggregate statistics. A stopped experiment preserves
all accepted chunks and never reuses their game IDs.

- [ ] **Step 3: Add CI correctness lanes**

Run warning-clean builds, unit tests, canonical perft, differential smoke,
clean-room verification, sanitizers on a supported Linux compiler, and UCI
stress. CI never promotes a strength patch; only the experiment service does.

- [ ] **Step 4: Build disjoint training and qualification opening suites**

Start from independently generated Blaze self-play openings and separately
license-approved human PGNs. Use released Blaze search to select positions
across fixed evaluation buckets from -1.50 to +1.50 pawns, deduplicate by
position key and pawn structure, and split by source game before selection.
Training/tuning and qualification suites must have disjoint source games.
Record generator binary/network, source licenses, seeds, selection thresholds,
and hashes in `provenance/qualification-openings.json`. Do not import Stockfish's
testing books.

- [ ] **Step 5: Validate with a known self-match**

Run identical binaries until at least 10,000 paired games. Require a confidence
interval containing zero, no systematic color/worker bias, and reproducible
aggregate counts after coordinator restart.

- [ ] **Step 6: Commit**

```text
git add tools/experiment config/workers provenance/qualification-openings.json .github/workflows/blaze-ci.yml
git commit -m "infra: add distributed paired-game experiment service"
```

### Task 13: Run the three research differentiators as isolated branches

**Files:**
- Create: `research/policy-ordering.md`
- Create: `research/uncertainty-search.md`
- Create: `research/endgame-expert.md`
- Conditional code locations: `training/model.py`, `src/blaze/eval`,
  `src/blaze/search/move_picker.cpp`, `src/blaze/search/search_node.cpp`

**Interfaces:**
- Policy branch produces a compact move-order logit without changing the value
  score.
- Uncertainty branch produces calibrated value variance used only to adjust a
  bounded reduction margin.
- Endgame branch produces a gated correction when material/fortress predicates
  match.

- [ ] **Step 1: Open a branch only after Blaze is within 50 LTC Elo of SF18**

Use the frozen near-parity release as common base. Never combine research ideas
before their individual ablations finish.

- [ ] **Step 2: Implement policy ordering with an inference budget**

Train from Blaze search visit distributions. Cap policy work to the nodes where
move ordering is unresolved and cache output per position. Require net NPS loss
below 5%, lower effective branching factor, and positive STC/LTC SPRT.

- [ ] **Step 3: Implement uncertainty-aware reductions**

Calibrate uncertainty on held-out search error. Permit it to change LMR by at
most one ply and never in check, mate-score windows, or singular verification.
Require tactical non-regression and positive paired SPRT.

- [ ] **Step 4: Implement the fortress/endgame expert**

Train only on self-play positions where deep search changed WDL or failed to
convert/hold. Gate by material and pawn topology, and clamp its correction.
Require held-out endgame calibration plus overall LTC improvement.

- [ ] **Step 5: Promote only the best independently passing branch**

Run a three-way confirmation on a disjoint opening suite. Merge one winner,
rebase the other ideas, and retest them from scratch if still desired.

### Task 14: Execute final Stockfish 18 qualification

**Files:**
- Create: `config/matches/sf18-qualification-stc.json`
- Create: `config/matches/sf18-qualification-ltc.json`
- Create: `config/matches/sf18-qualification-smp.json`
- Create: `docs/qualification/stockfish18-result.md`
- Update: `provenance/core-dependencies.json`
- Update: `docs/audit-report.md`

**Interfaces:**
- Consumes: frozen candidate binary/network, pinned SF18 binary, approved opening
  suite, two hardware classes, and experiment service.
- Produces: signed manifests, raw PGNs, pentanomial counts, confidence intervals,
  crash/time statistics, and a binary success/failure conclusion.

- [ ] **Step 1: Freeze all artifacts before the first game**

Record SHA-256 for candidate binary/network, SF18, opening suite, tablebase
configuration, compiler, source commit, search parameters, CPU microcode, OS,
and match runner. No artifact changes between STC and LTC except the declared
time control.

- [ ] **Step 2: Pass one-thread STC**

Run paired 10+0.1 SPRT with `elo0=0`, `elo1=5`, `alpha=beta=0.05` until accept or
reject. A reject returns the program to the weakest measured subsystem; it does
not trigger parameter fishing on the qualification opening set.

- [ ] **Step 3: Pass one-thread LTC**

Run the identical protocol at 60+0.6. Require zero candidate crashes, illegal
moves, or time forfeits.

- [ ] **Step 4: Pass eight-thread regression**

Run 60,000 paired games at 60+0.6, eight physical cores, 1024 MiB hash, equal
tablebase access. Require score above 50% and a 95% confidence interval whose
lower bound exceeds 50%.

- [ ] **Step 5: Repeat on a second x86-64 CPU family**

Repeat STC and a statistically powered LTC confirmation on a non-Zen-4 system.
The same candidate/network hashes must pass; CPU-specific binaries may differ
only by declared instruction dispatch and must be bit-identical in evaluation.

- [ ] **Step 6: Publish the result without overstating it**

`docs/qualification/stockfish18-result.md` reports exact protocol, all hashes,
pentanomial counts, LLR path, confidence intervals, failures, and hardware. The
headline may say "stronger than the pinned Stockfish 18 release under this
protocol" only if every gate passes. It must not convert the result into an
unsupported universal or human Elo number.

- [ ] **Step 7: Commit the qualification record**

```text
git add config/matches docs/qualification docs/audit-report.md provenance/core-dependencies.json
git commit -m "docs: publish Stockfish 18 qualification result"
```

## Program checkpoints

The work stops for architecture review at these measurable checkpoints:

1. **Foundation:** Tasks 1-2 pass; experiments are trustworthy and NN behavior
   is correct at every thread count.
2. **Search throughput:** Tasks 3-6 pass; benchmark speed improves without Elo
   regression and tactical safety remains exact.
3. **Modern search:** Task 7 passes a combined LTC confirmation.
4. **Neural baseline:** Tasks 8-9 deliver an accepted default v2 net trained on
   at least 100 million positions.
5. **Scale:** Task 9's billion-position learning curve remains positive and
   Blaze reaches within 150 LTC Elo of SF18.
6. **Parallel/product parity:** Tasks 10-12 pass one/eight-thread regressions and
   100,000-game reliability.
7. **Research parity:** Blaze reaches within 50 LTC Elo before Task 13 begins.
8. **Success:** every Task 14 gate passes.

If a checkpoint misses its strength target, profile and measure the responsible
subsystem before proceeding. Adding later features to conceal an earlier failed
gate is prohibited.
