# Blaze Bullet Beast Continuation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. DeepSeek V4 Flash should run at xhigh reasoning, preserve evidence, and stop/revert any strength patch that fails its stated gate.

**Goal:** Continue the custom Blaze engine from the current `codex/bullet-beast` worktree into a reliable, statistically stronger bullet specialist across `0+1`, `0.5+0`, `1+0`, `1+1`, `2+0`, `0+2`, and `2+1`, without copying Stockfish code.

**Architecture:** Keep Blaze's independent position and search core. Finish the in-progress increment-clock tune, replace process/thread latency with persistent runtimes, add a native pure-increment match arena, then improve hot-path search, evaluation, policy/book support, and Lichess integration behind paired-game gates. Stockfish 18 is an opponent and optional offline label source only; it is never a source-code or constant donor.

**Tech Stack:** C++20, MinGW g++ 15.2, Python 3.13, python-chess, Cute Chess 1.4 where its clock parser is sufficient, JSON/PGN evidence manifests, UCI, Lichess BOT API.

## Global Constraints

- Work only in `C:\Users\daniy\Desktop\Projects\NewBot\.worktrees\bullet-beast` on branch `codex/bullet-beast`.
- Current committed head is `9f0f1a1385c364cc70726214660d79d185ade5b6`.
- Preserve the uncommitted edits in `src/blaze/search/time_manager.cpp` and `tests/search/test_time_manager.cpp`; they are the active increment tune, not disposable changes.
- Do not copy Stockfish source, constants, network weights, opening book, or generated code. Keep `docs/clean-room-boundary.md` satisfied.
- Use test-driven development for every behavior change: failing test, observed expected failure, minimal implementation, green suite, then benchmark/match.
- Never accept a patch from NPS alone. Correctness, deadline safety, and paired games are separate gates.
- Do not claim superiority to Stockfish 18 until pinned, equal-resource, paired qualification passes.
- Never overwrite the accepted predecessor binaries. Record SHA-256 before every match.
- Commit one accepted subsystem at a time. Do not bundle rejected tuning constants with unrelated implementation work.

## Exact Resume State

Committed progression:

```text
9f0f1a1 perf: amortize search clock polling
196fbfb test: add persistent bullet latency gate
c3179c6 feat: add bullet-aware clock management
4d25363 perf: add mode-specific qsearch move generation
```

Important binaries:

```text
Accepted pre-bullet predecessor:
  build path: ..\sf18-challenger\build\blaze\blaze.exe
  SHA-256: 1DD0A605BC6AD060A14E3536A6D1AE947FBC70BA5B030DAC0C322F9DD4A160E8

Pre-tune bullet checkpoint:
  build path: build\blaze\blaze-pre-tune.exe
  SHA-256: 1ED6C71537565B2B403DF7C8A618B12F2C03C2AAF9F46C373AD4B944909E6C19

Current tuned working binary before commit:
  build path: build\blaze\blaze.exe
  observed SHA-256: 576A7FC989FD64D05850D26D2C401198661299A2DF323C751572BF8E9D3C1FD0
```

Verified current evidence:

```text
Pre-tune bullet vs accepted predecessor at 0.5+0, 200 paired games:
  98 wins, 66 losses, 36 draws, 58.0%
  estimated +56.1 Elo +/- 44.2, LOS 99.4%

Pre-tune 1+1 latency, 10 persistent samples:
  p50 852.36 ms, max 858.90 ms
Tuned 1+1 latency, 10 persistent samples:
  p50 690.59 ms, max 696.89 ms

Pre-tune 2+1 latency:
  p50 736.07 ms, max 961.26 ms
Tuned 2+1 latency:
  p50 772.04 ms, max 775.12 ms

Current C++ suite after tune:
  109 passed, 0 failed
```

The tuned-versus-pre-tune `1+1` match was started but its result was not retained. Treat it as not run and rerun it.

## File Map

- `src/blaze/search/time_manager.*`: pure clock-allocation policy; no UCI parsing or network I/O.
- `src/blaze/search/search.*`: iterative deepening, node search, stopping, root parallelism.
- `src/blaze/search/transposition_table.*`: shared TT; currently lock-heavy and a major hot-path target.
- `src/blaze/eval/network.*`: network loading and inference; inference still reconstructs features from the board.
- `src/blaze/uci/limits.*`: converts UCI `go` fields into `SearchLimits`.
- `src/blaze/uci/session.*`: UCI lifecycle; still creates a new search thread for each `go`.
- `tools/bullet_latency.py`: persistent-process seven-control latency measurement.
- `tools/experiment/`: paired match, manifest, and pentanomial statistics.
- `bench.epd`: ten-position smoke corpus only; it is not a sufficient qualification opening suite.

---

### Task 1: Finish and Gate the Active Increment Tune

**Files:**
- Modify: `src/blaze/search/time_manager.cpp`
- Modify: `tests/search/test_time_manager.cpp`
- Create: `build/experiments/increment-tune/manifest.json` through the runner, not by hand

**Interfaces:**
- Consumes: `BulletTimeManager::allocate(ClockState, LatencyBudget, SearchTelemetry)`.
- Preserves: pure-increment `0+1` and `0+2` behavior.
- Produces: a hard cap for bullet increment controls equal to the lower of the existing hard limit and `0.60 * increment + 0.08 * remaining`, never below `base + 1 ms`.

- [ ] **Step 1: Confirm the worktree contains only the intended tune**

Run:

```powershell
git status --short
git diff -- src/blaze/search/time_manager.cpp tests/search/test_time_manager.cpp
```

Expected: exactly those two tracked files are modified; build artifacts remain ignored.

- [ ] **Step 2: Re-run the focused red/green proof**

The required test is:

```cpp
TEST_CASE(increment_bullet_keeps_a_clock_growth_and_jitter_reserve) {
    const MoveBudget one_plus_one = BulletTimeManager::allocate(
        ClockState{1000ms, 1000ms, 0, 0}, LatencyBudget{30ms, 0ms}, SearchTelemetry{});
    const MoveBudget two_plus_one = BulletTimeManager::allocate(
        ClockState{2000ms, 1000ms, 0, 0}, LatencyBudget{30ms, 0ms}, SearchTelemetry{});
    CHECK(one_plus_one.target <= 550ms);
    CHECK(one_plus_one.hard <= 700ms);
    CHECK(two_plus_one.hard <= 800ms);
}
```

Run: `mingw32-make -f Makefile.blaze test`

Expected: `109 passed, 0 failed`.

- [ ] **Step 3: Run a larger latency confirmation**

Run:

```powershell
python tools/bullet_latency.py --engine build/blaze/blaze.exe --positions 100 --repetitions 3 --threads 1 --move-overhead 30
```

Accept only when all 2,100 samples are legal and deadline misses are zero. Require `1+1 p99 <= 725 ms`, `2+1 p99 <= 825 ms`, `0+1 p99 <= 700 ms`, and `0+2 p99 <= 1350 ms` on the Ryzen 7 7700 reference machine.

- [ ] **Step 4: Run tuned versus pre-tune increment matches**

Run 400 paired games per control:

```powershell
..\..\cutechess-cli.exe -engine cmd=build\blaze\blaze.exe name=Tuned -engine cmd=build\blaze\blaze-pre-tune.exe name=PreTune -each proto=uci tc=1+1 option.Threads=1 option.Hash=16 -openings file=..\..\bench.epd format=epd order=sequential -repeat -games 400 -rounds 1 -concurrency 8 -draw movenumber=80 movecount=10 score=10 -resign movecount=5 score=800
```

Repeat with `tc=2+1`. Accept the tune if neither control is below 47.5% and the combined score is at least 49.0%, with fewer time losses than pre-tune. If rejected, change only `0.60` and `0.08`, add a test for the revised bound, rebuild, and rerun both controls.

- [ ] **Step 5: Verify and commit the tune**

Run:

```powershell
mingw32-make -f Makefile.blaze test experiment-test uci-stress verify-clean-room
git diff --check
```

Expected: 109 C++ tests, 19 Python experiment tests, 1,000 legal UCI cycles, and clean-room verification pass.

Commit:

```powershell
git add src/blaze/search/time_manager.cpp tests/search/test_time_manager.cpp
git commit -m "tune: preserve clock growth in increment bullet"
```

### Task 2: Build a Native Pure-Increment Bullet Match Arena

**Files:**
- Create: `tools/experiment/bullet_match.py`
- Create: `tools/experiment/test_bullet_match.py`
- Create: `testdata/openings/bullet-smoke.epd`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: `BulletControl.parse(text) -> BulletControl`.
- Produces: `GameClock.before_move(color) -> int` and `GameClock.finish_move(color, elapsed_ms) -> bool` where the bool reports a flag.
- Produces: `run_paired_match(spec, candidate, baseline) -> BulletMatchResult` with PGN, per-move latency, time-loss counts, and color-swapped outcomes.

- [ ] **Step 1: Write failing clock-semantic tests**

```python
def test_zero_plus_one_precredits_first_increment():
    clock = GameClock(BulletControl.parse("0+1"))
    assert clock.before_move(chess.WHITE) == 1000
    assert not clock.finish_move(chess.WHITE, 250)
    assert clock.before_move(chess.WHITE) == 1750

def test_elapsed_time_flags_before_increment_is_awarded():
    clock = GameClock(BulletControl.parse("0.5+0"))
    assert clock.finish_move(chess.WHITE, 501)
```

Run: `python -m unittest tools.experiment.test_bullet_match -v`

Expected: import failure because `bullet_match.py` does not exist.

- [ ] **Step 2: Implement deterministic clock accounting**

Use integer microseconds internally:

```python
@dataclass
class GameClock:
    control: BulletControl
    white_us: int = field(init=False)
    black_us: int = field(init=False)

    def __post_init__(self) -> None:
        initial = self.control.base_us or self.control.increment_us
        self.white_us = initial
        self.black_us = initial

    def finish_move(self, color: chess.Color, elapsed_us: int) -> bool:
        remaining = self.white_us if color else self.black_us
        remaining -= elapsed_us
        if remaining <= 0:
            return True
        remaining += self.control.increment_us
        if color:
            self.white_us = remaining
        else:
            self.black_us = remaining
        return False
```

Send the current clocks and increments to both engines with UCI `go`. Measure from the completed stdin flush to the full `bestmove` line using `time.perf_counter_ns()`.

- [ ] **Step 3: Implement paired legal-game execution**

For every opening, play candidate-white then candidate-black. Validate every returned move with python-chess before updating the board. Terminate on checkmate, stalemate, repetition, 50-move draw, insufficient material, illegal move, crash, or flag. Save raw PGN and a JSON record containing both binary hashes and every move latency.

- [ ] **Step 4: Add arena smoke and Make target**

Create the tracked smoke corpus from the existing independent ten-position file:

```powershell
New-Item -ItemType Directory -Force testdata\openings | Out-Null
Copy-Item -LiteralPath ..\..\bench.epd -Destination testdata\openings\bullet-smoke.epd
```

Add:

```make
bullet-match-smoke: $(BLAZE_BIN)
	python -m tools.experiment.bullet_match --candidate $(BLAZE_BIN) --baseline build/blaze/blaze-pre-tune.exe --controls 0+1,0.5+0,0+2 --games 20 --openings testdata/openings/bullet-smoke.epd --output build/experiments/bullet-smoke
```

Run the target. Expected: 60 valid paired games, no duplicate games, exact hashes, and no harness exceptions.

- [ ] **Step 5: Commit**

```powershell
git add Makefile.blaze testdata/openings/bullet-smoke.epd tools/experiment/bullet_match.py tools/experiment/test_bullet_match.py
git commit -m "test: add native pure-increment bullet arena"
```

### Task 3: Replace Per-Move UCI Thread Creation with a Persistent Search Worker

**Files:**
- Create: `src/blaze/uci/search_worker.h`
- Create: `src/blaze/uci/search_worker.cpp`
- Create: `tests/uci/test_search_worker.cpp`
- Modify: `src/blaze/uci/session.h`
- Modify: `src/blaze/uci/session.cpp`

**Interfaces:**
- Produces: `SearchJob { Position root; SearchLimits limits; std::vector<uint64_t> prior_keys; const NetworkEvaluator* network; }`.
- Produces: `SearchWorker::submit(SearchJob)`, `SearchWorker::stop_and_wait()`, and `SearchWorker::shutdown()`.
- Preserves: exactly one `bestmove` per accepted `go`, ponder suppression, and synchronous `stop` completion.

- [ ] **Step 1: Write failing lifecycle tests**

Create a worker with a result callback that records `std::this_thread::get_id()`. Submit 100 depth-one jobs and assert all callbacks use one non-caller thread. Submit an infinite job, call `stop_and_wait`, then submit a depth-one job and require both complete without recreating the worker.

- [ ] **Step 2: Implement the worker state machine**

```cpp
class SearchWorker {
public:
    using Callback = std::function<void(SearchResult)>;
    explicit SearchWorker(TranspositionTable& table, Callback callback);
    ~SearchWorker();
    void submit(SearchJob job);
    void stop_and_wait();
private:
    void loop();
    std::jthread thread_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::optional<SearchJob> pending_;
    std::atomic<bool> stop_{false};
    bool active_ = false;
};
```

The condition-variable predicate is `pending_.has_value() || stop_token.stop_requested()`. Clear `active_` and notify waiters only after the callback has completed.

- [ ] **Step 3: Move UciSession search output into the callback**

Construct `SearchWorker` once in `UciSession`. Remove `worker_ = std::thread(...)` from `start_search`. `setoption`, `position`, `ucinewgame`, `stop`, `ponderhit`, and destruction must call `stop_and_wait()` before mutating shared evaluator or position state.

- [ ] **Step 4: Measure submission latency**

Extend `tools/bullet_latency.py` to report `command_to_search_start_us` from a UCI `info string search-start` line available only under a new `DebugLatency` option. Require persistent p99 to improve by at least 100 microseconds or 20%, whichever is smaller, without increasing deadline misses.

- [ ] **Step 5: Verify and commit**

Run: `mingw32-make -f Makefile.blaze test uci-stress repeated-start verify-clean-room`

Commit:

```powershell
git add src/blaze/uci tests/uci tools/bullet_latency.py
git commit -m "perf: keep the UCI search worker alive between moves"
```

### Task 4: Add a Persistent Root Worker Pool and Bullet-Aware Thread Cap

**Files:**
- Create: `src/blaze/search/worker_pool.h`
- Create: `src/blaze/search/worker_pool.cpp`
- Create: `tests/search/test_worker_pool.cpp`
- Modify: `src/blaze/search/search.h`
- Modify: `src/blaze/search/search.cpp`
- Modify: `src/blaze/uci/session.cpp`

**Interfaces:**
- Produces: `WorkerPool::run_root_tasks(std::span<RootTask>, unsigned active_workers)`.
- Consumes: `SearchLimits::recommended_threads` and the user `Threads` ceiling.
- Rule: active workers are `min(user_threads, recommended_threads, legal_root_moves)`; emergency search always uses one.

- [ ] **Step 1: Write pool reuse and determinism tests**

Run 50 depth-four searches through one pool and assert worker thread IDs are stable. At one active worker, require the exact single-thread score and PV. At four workers, require legal moves, equal mate status, and node-budget compliance.

- [ ] **Step 2: Implement long-lived workers**

Workers own local `Searcher` state and wait on a generation counter. Root tasks are claimed with an atomic index. Shared state is limited to the TT, stop token, immutable network, and root result slots. Do not create `std::thread` inside an iterative-deepening loop.

- [ ] **Step 3: Apply automatic bullet caps**

```cpp
limits.threads = std::min(threads_, limits.recommended_threads);
if (limits.regime == SearchRegime::Emergency) limits.threads = 1;
```

Expose `option name AutoThreads type check default true`; when false, retain the exact configured `Threads` value.

- [ ] **Step 4: Gate throughput and strength**

Require no regression at fixed depth, lower p99 at sub-100 ms budgets, and at least 49% in 1,000 paired `0.5+0` games versus the immediately previous commit.

- [ ] **Step 5: Commit**

```powershell
git add src/blaze/search src/blaze/uci tests/search tests/uci
git commit -m "perf: add persistent bullet search workers"
```

### Task 5: Remove Transposition-Table Locks from the Search Hot Path

**Files:**
- Modify: `src/blaze/search/transposition_table.h`
- Modify: `src/blaze/search/transposition_table.cpp`
- Modify: `tests/search/test_transposition_table.cpp`
- Modify: `tests/search/test_search.cpp`

**Interfaces:**
- Keeps: `probe`, `store`, `resize`, `clear`, `new_search`, `capacity`.
- Adds: `prefetch(uint64_t key)` and `hashfull() -> int`.
- Produces: two 64-bit atomics per entry with key verification and no mutex in `probe` or `store`.

- [ ] **Step 1: Add one-million-operation concurrent stress**

Eight threads repeatedly store/probe deterministic key families. A probe may miss during a race, but a hit must decode a valid bound, depth, move, and mate-normalized score. Add `static_assert(std::is_trivially_copyable_v<TTData>)`.

- [ ] **Step 2: Implement verified atomic publication**

Writers store packed payload with relaxed ordering, then a key checksum with release ordering. Readers load key, payload, then key with acquire ordering and accept only equal nonzero key reads. Round clusters to a power of two and index with `key & cluster_mask_`.

- [ ] **Step 3: Add prefetch and UCI hash occupancy**

Prefetch a child's cluster before recursion. Report `hashfull` in UCI info once per completed iteration.

- [ ] **Step 4: Gate and commit**

Require 20% fixed-depth NPS improvement in four-thread search or a positive 2,000-game `0.5+0` result. All concurrency and clean-room tests must pass.

```powershell
git add src/blaze/search/transposition_table.* src/blaze/search/search.cpp tests/search
git commit -m "perf: add lock-free verified transposition entries"
```

### Task 6: Build Modern Selective Search as Isolated Experiments

**Files:**
- Create: `src/blaze/search/move_picker.h`
- Create: `src/blaze/search/move_picker.cpp`
- Create: `src/blaze/search/history.h`
- Create: `src/blaze/search/parameters.h`
- Create: `tests/search/test_move_picker.cpp`
- Create: `tests/search/test_history.cpp`
- Modify: `src/blaze/search/search.cpp`

**Interfaces:**
- Produces: explicit `NodeType::{Root, Pv, NonPv}`.
- Produces: staged TT, good-capture, killer/countermove, quiet, and bad-capture selection.
- Produces: saturating quiet, capture, continuation, pawn, and correction histories.

- [ ] **Step 1: Implement the staged move picker behind parity tests**

Every legal move must appear exactly once. TT move is first when legal; non-losing captures precede quiets; losing captures remain available. Use lazy stage generation and selection, not full-vector sorting.

- [ ] **Step 2: Refactor node types without changing search decisions**

For a fixed FEN corpus at depths 1-8, record best move, score, node count, and PV before the refactor. Require byte-identical records afterward.

- [ ] **Step 3: Add one selective feature per commit and SPRT**

Use this order: mate-distance bounds, reverse futility, razoring, late-move pruning, adaptive LMR, null-move verification, internal iterative reduction, singular extension, capture history, continuation history, pawn history, correction history. Each feature receives a focused tactical test, full suite, benchmark, and at least 2,000 paired `0.5+0` games. Keep only features whose SPRT does not reject non-regression.

- [ ] **Step 4: Tune on training openings only**

Place all constants in `SearchParameters`. Tune with SPSA or Bayesian optimization on a training opening set. Freeze constants before confirmation on a source-game-disjoint suite.

- [ ] **Step 5: Combined confirmation commit**

After individually accepted commits, run 10,000 paired games at `0.5+0`, 5,000 at `1+0`, and 2,000 at `1+1`. Revert the combined stack if interaction loses more than 5 Elo relative to the sum of isolated evidence.

### Task 7: Make Neural Evaluation Incremental and Bullet-Sized

**Files:**
- Create: `src/blaze/eval/accumulator.h`
- Create: `src/blaze/eval/accumulator.cpp`
- Modify: `src/blaze/core/position.h`
- Modify: `src/blaze/core/position.cpp`
- Modify: `src/blaze/eval/network.h`
- Modify: `src/blaze/eval/network.cpp`
- Create: `tests/eval/test_accumulator.cpp`

**Interfaces:**
- Produces: two side-relative 128-wide fast accumulators and an optional 128-wide residual stage.
- Produces: `Accumulator::refresh(Position)`, `apply(MoveDelta)`, `undo(MoveDelta)`.
- Rule: emergency/bullet nodes may use the fast stage; deeper PV/root nodes may add the residual stage.

- [ ] **Step 1: Add refresh-versus-increment parity tests**

For normal moves, captures, en passant, both castles, promotions, null moves, and 10,000 random legal sequences, compare incrementally updated accumulators to a fresh board reconstruction after every make and unmake.

- [ ] **Step 2: Add accumulator deltas to StateInfo**

Store removed/added feature indices and prior king context in the existing state stack. Undo must restore without heap allocation.

- [ ] **Step 3: Implement progressive inference**

```cpp
int NetworkEvaluator::evaluate(const Position& position,
                               const Accumulator& accumulator,
                               EvalStage stage) const;
```

`EvalStage::Fast` computes only the first 128 outputs. `EvalStage::Full` adds the residual block. Both clamp outside the mate band and remain side-to-move relative.

- [ ] **Step 4: Gate**

Require bit-exact refresh/increment parity, at least 2x evaluator throughput, no fixed-depth score changes for the same stage, and positive paired results before making the network default.

- [ ] **Step 5: Commit**

```powershell
git add src/blaze/core src/blaze/eval tests/eval
git commit -m "eval: add progressive incremental network inference"
```

### Task 8: Add Reproducible Training, Root Policy, and an Original Bullet Book

**Files:**
- Create: `training/dataset.py`
- Create: `training/model.py`
- Create: `training/train.py`
- Create: `training/export.py`
- Create: `training/test_dataset.py`
- Create: `tools/generate_book.py`
- Create: `provenance/training.json`
- Create: `provenance/bullet-book.json`
- Modify: `src/blaze/uci/book.cpp`

**Interfaces:**
- Dataset record: position, legal moves, Blaze score, WDL result, root visit distribution, generator hash, game ID, ply, and source license.
- Model outputs: value/WDL plus a compact root-only policy logit.
- Book record: independently generated position key, move, weight, source generator hash, and selection score.

- [ ] **Step 1: Test game-disjoint dataset splits and deduplication**

No game ID may cross train/validation/test. Deduplicate exact keys and cap repeated pawn structures per shard. Reject records with illegal best moves, missing provenance, or checksum mismatches.

- [ ] **Step 2: Generate Blaze self-play data**

Start with 10 million positions, then 100 million only if held-out loss and paired net matches improve. Stockfish 18 labels may be stored in a separate teacher experiment manifest, never mixed without an explicit `teacher=sf18` provenance field.

- [ ] **Step 3: Train and export progressive value/policy weights**

Use quantization-aware training and verify scalar integer inference against Python for 10,000 held-out positions. Record dataset hashes, seed, optimizer, dimensions, scales, and checkpoint hash.

- [ ] **Step 4: Generate the original bullet book**

Use Blaze self-play/root visits, not an imported engine book. Limit book depth by measured book-on latency and retain multiple moves where score differences are inside the training noise bound. Validate every encoded move as legal.

- [ ] **Step 5: Prove book-on and book-off separately**

Run the full seven-control matrix. Book-off must remain positive versus the prior Blaze baseline; book-on must not reduce score and must lower opening p99 latency.

- [ ] **Step 6: Commit only accepted artifacts and recipes**

Do not commit raw multi-gigabyte shards. Commit manifests, exporter, accepted compact weights/book if repository policy allows their size, and exact reproduction commands.

### Task 9: Add a Reliable Lichess BOT Connector

**Files:**
- Create: `bot/lichess_bot.py`
- Create: `bot/uci_client.py`
- Create: `bot/clock_model.py`
- Create: `bot/config.example.toml`
- Create: `bot/test_clock_model.py`
- Create: `bot/test_event_replay.py`

**Interfaces:**
- Consumes: Lichess streaming events and game state.
- Produces: one legal move submission per engine turn with measured request/response p50, p95, and p99.
- Updates: UCI `Move Overhead` to `max(configured_floor, rolling_network_p99 + serialization_margin)`.

- [ ] **Step 1: Build replay tests before network access**

Replay challenge, game start, incremental game state, reconnect, duplicate event, opponent move, draw offer, game finish, and aborted game fixtures. Assert exactly-once move submission and correct reconstructed clocks.

- [ ] **Step 2: Implement a persistent engine process per active game**

Keep one UCI process alive, feed complete legal move history, wait for `readyok` after configuration, and terminate it on game completion. Redact the API token from every log and exception.

- [ ] **Step 3: Measure network latency and update overhead**

Maintain a bounded rolling sample of move-submission latency. Use nearest-rank p99 plus 10 ms serialization margin, capped so the engine still receives at least 1 ms in emergency mode.

- [ ] **Step 4: Run shadow and private challenge tests**

First run without submitting moves and compare reconstructed positions. Then run private challenges at each supported control. Require zero illegal submissions, duplicates, stale moves, or engine-attributable flags over 1,000 games before public use.

- [ ] **Step 5: Commit**

```powershell
git add bot
git commit -m "feat: add latency-aware Lichess bot connector"
```

### Task 10: Qualification and Promotion Gates

**Files:**
- Create: `config/matches/bullet-matrix.json`
- Create: `docs/qualification/bullet-release.md`
- Modify: `docs/audit-report.md`

**Interfaces:**
- Consumes: frozen candidate, network, book, prior Blaze, pinned Stockfish 18, opening suite, runner, and machine metadata.
- Produces: raw PGNs, hashes, pentanomial counts, confidence intervals, deadline statistics, and a promotion decision.

- [ ] **Step 1: Freeze artifacts**

Record SHA-256 of candidate, prior Blaze, Stockfish 18, network, book, openings, runner, source commit, compiler, CPU, OS, threads, and hash. No artifact may change during a match series.

- [ ] **Step 2: Pass Blaze regression matrix**

Run at least 10,000 paired games at each of `0+1`, `0.5+0`, `1+0`, `1+1`, `2+0`, `0+2`, and `2+1`. Require zero crashes/illegal moves, fewer than 1 engine-attributable time loss per 100,000 moves, and a combined 95% confidence interval whose lower bound exceeds 50% versus commit `4d25363`.

- [ ] **Step 3: Pass book-off Stockfish 18 tests**

Use equal threads/hash and paired independent openings. Begin with 2,000-game diagnostics per control, then pentanomial SPRT with `elo0=0`, `elo1=5`, `alpha=beta=0.05` for controls where the diagnostic score is within 100 Elo. A loss identifies the next subsystem; it is not hidden with book results.

- [ ] **Step 4: Pass book-on tests**

Repeat with the frozen original book. Require no weaker score than book-off within confidence and lower opening p99 latency.

- [ ] **Step 5: Preserve non-bullet quality**

Run paired regressions at `3+0`, `3+2`, `10+0`, `10+5`, and `60+0.6` against the previous accepted Blaze. Require no control below 48% and combined score at least 49.5%.

- [ ] **Step 6: Publish exact results**

`docs/qualification/bullet-release.md` must state hashes, controls, hardware, openings, raw counts, confidence intervals, crashes, time losses, book state, and failures. It may claim superiority only for protocols whose statistical gate passed.

## DeepSeek V4 Flash xhigh Operating Instructions

Start with this exact sequence:

```powershell
Set-Location C:\Users\daniy\Desktop\Projects\NewBot\.worktrees\bullet-beast
git branch --show-current
git status --short
git diff -- src/blaze/search/time_manager.cpp tests/search/test_time_manager.cpp
mingw32-make -f Makefile.blaze test
```

Then execute Task 1 only. Do not begin Task 2 until the increment tune is either committed after its gates or reverted with recorded evidence. At every task boundary:

```powershell
git status --short
git diff --check
mingw32-make -f Makefile.blaze test experiment-test uci-stress verify-clean-room
```

Report measured outcomes, rejected experiments, exact commit hashes, and the next unchecked step. Never summarize a small match as proof of universal strength.

## Program Checkpoints

1. **Clock safety:** Task 1 passes 2,100 latency samples and increment matches.
2. **Trustworthy bullet evidence:** Task 2 plays pure-increment paired games with exact clocks and manifests.
3. **Persistent runtime:** Tasks 3-4 remove per-move and per-depth thread creation without Elo regression.
4. **Hot-path throughput:** Task 5 removes TT locks and passes concurrency plus match gates.
5. **Modern strength core:** Tasks 6-7 pass isolated and combined bullet matches.
6. **Original learned advantages:** Task 8 passes book-off and book-on proofs.
7. **Product reliability:** Task 9 completes 1,000 private games without protocol defects.
8. **Promotion:** Task 10 passes the seven-control qualification matrix and publishes reproducible evidence.
