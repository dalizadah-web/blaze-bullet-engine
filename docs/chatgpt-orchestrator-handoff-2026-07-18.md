# Blaze bullet-engine audit and ChatGPT orchestration handoff

**Audit date:** 2026-07-18  
**Workspace:** `C:\Users\daniy\Desktop\Projects\NewBot\.worktrees\bullet-beast`  
**Current branch/head:** `codex/bullet-beast` at `aa50f42331323ec06c05b4f5aa4d04437e3d57b9`  
**Public repository:** <https://github.com/dalizadah-web/blaze-bullet-engine>

## Executive assessment

Blaze is a clean-room C++20 chess-engine research base with sound legal-move
and UCI foundations, a basic neural-evaluation path, a conventional
alpha-beta/PVS search, bullet-aware clock management, and a working public
GitHub Actions match farm. It is not close to Stockfish 18 in absolute playing
strength. The only pinned direct comparison in the original audit was a
0/20 result for Blaze at `2+0.02`; Blaze searched about depth 7--10 where
Stockfish 18 reported about depth 19--21 on the measured one-thread suite.

The most recent DeepSeek increment-cap conclusion must **not** be treated as
valid. Four cloud runs completed correctly, but an untracked ad-hoc script
mis-scored their pentanomial results and mixed shard files from separate runs.
The official strict summaries show all four runs as **inconclusive**, not
rejected. They also compare each tune against commit `4d25363`, not against
the claimed pre-tune control. No valid experiment currently proves that the
growth cap helps or hurts playing strength.

## Non-negotiable clean-room boundary

- Keep Blaze independent. Do not copy Stockfish source, constants, generated
  code, NNUE weights, books, or tablebase data.
- Stockfish 18 may be an opponent and an offline label source only where the
  dataset/license/provenance is recorded.
- Preserve the verified position/move-generation core unless a focused,
  test-backed reason requires a change.
- Do not claim that Blaze beats Stockfish 18 without pinned equal-resource,
  color-swapped, statistically significant qualification.

## Verified current repository state

### Branch and history

`codex/bullet-beast` is at v4 of the growth-cap tune:

```text
aa50f42 tune: v4 - growth cap 0.60*inc + 0.08*rem for bankroll <=3s
14f4315 tune: v3 - growth cap 0.55*inc + 0.06*rem for bankroll <=2s
82375b5 tune: reduce growth cap to 0.50*inc + 0.05*rem (v2)
e029b90 tune: preserve clock growth in increment bullet
e5d7f7b feat: make 48-way hybrid matches the default
4d25363 perf: add mode-specific qsearch move generation
```

`main` / `origin/main` remains at `e5d7f7b`. The statement that the tune was
reverted is therefore not reflected in the current worktree or remote
`codex/bullet-beast` branch. The active source still applies this cap:

```cpp
if (clock.increment > Milliseconds(0) && bankroll <= Milliseconds(3000)) {
    const Milliseconds growth_cap =
        scaled(clock.increment, 0.60) + scaled(clock.remaining, 0.08);
    result.hard = std::min(
        result.hard, std::max(growth_cap, base + Milliseconds(1)));
}
```

There is also an untracked file, `tools/aggregate_shards.py`. It is not a
production component and must not be used to make an acceptance decision.

### Fresh verification on 2026-07-18

```text
109 C++ tests: PASS
12 cloud-match tests: PASS
19 experiment/latency/manifest tests: PASS
```

The user/DeepSeek report says seven latency controls passed with zero deadline
misses. That is useful reported evidence, but the raw 2,100-sample output was
not retained in the audited artifact set, so it is not independently
reproducible from the workspace as-is.

## Engine capability audit

### What is solid and worth preserving

- Strict FEN parsing, incremental make/unmake, legal move generation, Zobrist
  keys, repetition handling, and canonical perft coverage.
- Start-position perft was previously recorded through depth 6 and Kiwipete
  through depth 4; earlier differential testing matched python-chess on
  1,000 random legal positions at depth 3.
- Robust UCI lifecycle: `go`, `stop`, `ponderhit`, `quit`, legal-bestmove
  handling, and standard time-control parsing are covered.
- Search baseline: iterative deepening, alpha-beta/PVS, TT, aspiration,
  null move, LMR, SEE-informed captures, killers/history/countermove, basic
  ProbCut-style probing, and root splitting.
- Bullet clock manager reserves submission latency, handles zero-main-time
  increment controls, uses hard/soft deadlines, and adapts worker count to
  the hard budget.
- Test and evidence foundations: manifests record artifact hashes, paired PGN
  parsing validates color reversal/opening identity, and the strict cloud
  aggregator rejects incomplete, duplicate, mismatched, or malformed shards.

### Main strength gaps relative to Stockfish 18

These are not small tuning gaps; they dominate the expected strength ceiling.

1. **Evaluation/training (P0):** Blaze's current evaluation is primarily
   handcrafted; its optional small network is not a Stockfish-class
   incrementally updated NNUE. The training path is tiny and weakly labelled
   compared with large, curated self-play/deep-search datasets.
2. **Search selectivity (P0/P1):** pruning, reductions, extensions,
   histories, correction/evaluation feedback, and quiescence are much less
   mature. Current quiescence and move ordering leave tactical/selective depth
   on the table.
3. **Hot-path performance (P1):** the TT is lock-heavy; some historical data
   structures allocate in the search hot path; sliding attacks and SMP are not
   optimized to current engine standards.
4. **Parallelism (P1):** the engine is principally root-split rather than a
   refined full-tree SMP design. Earlier audit notes that parallel child search
   had an NN evaluation propagation issue; verify this against current source
   before neural work.
5. **Experiment maturity (P1):** the new cloud farm is far better than the
   original setup, but experiment identity and baseline selection still need
   stronger discipline. A candidate ref string is not enough: resolve and
   record the exact source commit and binary hash in every comparison.
6. **Endgame/product completeness (P2):** no Syzygy, Chess960, MultiPV, WDL,
   mature analysis output, NUMA/CPU dispatch, or large-scale thread behavior.

## Distributed testing infrastructure

The repository now has a real public distributed match farm.

- GitHub Actions workflow: `.github/workflows/cloud-match.yml`.
- Public Linux runners: up to 20 shard jobs; each job plays two concurrent
  one-thread engine games, so the cloud lane can run up to 40 games at once.
- Local hybrid lane: eight concurrent local games on the 16-thread Windows
  PC (two one-thread engines per game).
- Default launcher: `powershell -File tools/cloud_match.ps1 -Action Run`.
  It invokes the hybrid path; `-CloudOnly` suppresses local games.
- Default workload: 400 cloud games plus 400 local games, nominally up to 48
  concurrent games and 800 total games.
- Artifacts include PGNs, runner logs, match manifests, SHA-256 identities,
  strict shard manifests, and aggregate JSON/Markdown summaries.

The cloud-only system was verified end-to-end in GitHub run
`29611226809` (40 games, 20 shards, strict aggregate success). There are
several hybrid output directories, but none has a completed
`hybrid-summary.json`; do not claim the automatic 48-way orchestrator is
fully end-to-end qualified until one complete hybrid run produces that file.

## DeepSeek increment-tune audit: critical correction

### What was actually run

All four runs were valid 200-game, 20-shard, one-thread, 16 MiB,
color-swapped cloud matches at `1+1`, with the same opening-file identity and
the same baseline binary SHA-256:

```text
baseline source ref: 4d25363fef79ff2025670e248ed07b3d81747d3a
baseline binary SHA-256: 7660006768db1b067a9b7b8b57cbbde7aea3c92d38abcdcf589d0941a66ce489
```

This baseline is the older qsearch commit, not the pre-tune bullet checkpoint
(`e5d7f7b`). Consequently, none of these four matches is a valid direct test
of “growth cap versus pre-tune.”

### Strict aggregate results

For a pentanomial pair `(W2, W1D1, D2, L1D1, L2)`, candidate points are:

```text
2, 1.5, 1, 0.5, 0
```

and score percentage is:

```text
(2*W2 + 1.5*W1D1 + D2 + 0.5*L1D1) / (2*pairs) * 100
```

| Tune | Candidate commit | Pentanomial | Correct score | Strict LLR | Strict decision |
| --- | --- | --- | ---: | ---: | --- |
| v1 | `e029b90` | `9 22 36 23 10` | 49.25% | -0.1040 | continue |
| v2 | `82375b5` | `10 21 34 23 12` | 48.50% | -0.1604 | continue |
| v3 | `14f4315` | `7 30 32 23 8` | 51.25% | +0.0888 | continue |
| v4 | `aa50f42` | `13 21 26 28 12` | 48.75% | -0.1229 | continue |

The exact strict summaries live under:

```text
artifacts/cloud-match/29612483385/result-ce60df066110c679c941a8f1/summary.json
artifacts/cloud-match/29613512232/result-ce60df066110c679c941a8f1/summary.json
artifacts/cloud-match/29614739544/result-ce60df066110c679c941a8f1/summary.json
artifacts/cloud-match/29615668703/result-ce60df066110c679c941a8f1/summary.json
```

### Why the reported 38%, 37.5%, 43%, and 36% figures are invalid

`tools/aggregate_shards.py` is an untracked ad-hoc script. Its score formula
is:

```python
(W2 * 2 + W1D1 + D2) / (pairs * 2)
```

It incorrectly gives `W1D1` one point instead of 1.5 and gives `L1D1` zero
instead of 0.5. For v1 it deterministically transforms the correct 49.25%
into the reported 38.00%. It also hardcodes two earlier failed run IDs and
stores each shard by shard index, silently overwriting a shard from one run
with the same index from another run. It neither checks full coverage nor
checks artifact compatibility.

Therefore:

- The 47.5% rejection is invalid.
- The four 200-game results are all too weak to establish a strength claim;
  the production SPRT correctly returned `continue` each time.
- The tests do not establish that the cap is harmful, beneficial, or worth
  retaining.
- v3's 51.25% observed score is not an acceptance either; it is noisy and not
  a pre-tune comparison.

## Immediate orchestration plan for ChatGPT

### Phase 0: reconcile state before changing engine code

1. Treat `aa50f42` as the actual active branch state and `e5d7f7b` as the
   actual pre-tune control. Do not rely on prose saying the tune was reverted.
2. Preserve the four cloud artifacts. Do not delete them; they demonstrate the
   aggregator problem and provide reusable diagnostics.
3. Quarantine or delete `tools/aggregate_shards.py` only after confirming it is
   not needed for forensic reproduction. Never use it for results again.
4. Do not use `git reset --hard`. If the user chooses to remove the cap,
   create an explicit revert commit or a new candidate branch.
5. Fix experiment bookkeeping so the aggregate identity includes resolved
   candidate/baseline commits and/or binary hashes, not only mutable ref text.

### Phase 1: settle the increment-cap question correctly

Run direct paired comparisons at `1+1` and `2+1`:

```text
candidate: one tune version at a time (v1, v2, v3, v4, or no-cap revert)
baseline: e5d7f7b exact pre-tune control
threads: 1 per engine
hash: 16 MiB per engine
openings: one frozen, sufficiently broad paired corpus
colors: always swapped per opening pair
```

Use the production aggregator only. Start with a small smoke test for runner
health, then run enough paired games for an actual SPRT decision. Keep the
SPRT hypotheses fixed (`elo0=0`, `elo1=5`, `alpha=beta=0.05`) and report:

- exact source commits and binary hashes;
- game/pair count and opening hash;
- pentanomial counts;
- correctly calculated score;
- LLR and decision;
- engine-attributable crashes, illegal moves, and time losses;
- latency evidence from the same candidate.

Do not combine v1--v4 as if they were one candidate. Do not accept/reject by a
point-score threshold alone. Decide whether to retain a cap only after a
direct, statistically valid candidate-versus-pre-tune result.

### Phase 2: make the 48-way path demonstrably reliable

1. Run one small end-to-end hybrid match and require
   `artifacts/hybrid/<timestamp>/hybrid-summary.json`.
2. Verify it combines exactly one strict cloud summary and one complete local
   lane with compatible time control, refs, opening hash, SPRT parameters, and
   pair counts.
3. Add a regression test/clear diagnostic for the absolute-output-path path in
   the PowerShell downloader and for a failed local lane.
4. Keep Windows and Linux binary hashes separate. Pool outcomes only as
   explicitly labelled cross-platform meta-analysis, never as one supposedly
   identical binary.

### Phase 3: bullet strength work, in this priority order

1. Build a native exact-clock bullet arena for `0+1`, `0+2`, `1+0`, `1+1`,
   `2+0`, and `2+1`; CuteChess/cloud data alone is not enough for precise
   pure-increment flag accounting.
2. Profile before altering search. Target TT locking, PV/move allocations,
   sliding attacks, move generation, and UCI/search-worker startup cost.
3. Improve selective search one isolated mechanism at a time with tests and
   paired evidence: staged move picker, capture/continuation history, safe
   quiescence pruning, better reductions, and only then more advanced pruning
   or extensions.
4. Make evaluator work the central long-term project: independently designed,
   incrementally updated neural features; versioned network format; exact
   refresh-vs-increment parity; credible labelled data; deterministic training
   and held-out validation.
5. Improve SMP after the hot path and evaluator are stable. Preserve single-
   thread score parity; do not blindly copy Stockfish's Lazy SMP implementation.
6. Treat Stockfish 18 as an external long-term benchmark, not the near-term
   opponent for tuning. First achieve repeatable gains versus Blaze controls,
   then establish a calibrated opponent ladder.

## Operating commands

```powershell
# Default hybrid: approximately 40 cloud + 8 local concurrent games.
powershell -File tools/cloud_match.ps1 -Action Run

# Cloud only.
powershell -File tools/cloud_match.ps1 -Action Run -CloudOnly

# Inspect the latest run.
powershell -File tools/cloud_match.ps1 -Action Status

# Wait and fail on a failed cloud run.
powershell -File tools/cloud_match.ps1 -Action Watch

# Download immutable evidence.
powershell -File tools/cloud_match.ps1 -Action Download

# Local verification.
mingw32-make -f Makefile.blaze test
python -m unittest discover -s tools/experiment -p "test_*.py" -v
python -m unittest discover -s tools/cloud_match -p "test_*.py" -v
```

## Copy/paste instruction for ChatGPT

```text
Act as the evidence-first orchestrator for the clean-room Blaze bullet chess
engine. Read docs/chatgpt-orchestrator-handoff-2026-07-18.md before changing
anything. Do not copy Stockfish source, constants, networks, books, or generated
code. Preserve the legal-move core and make one coherent engine change at a time.

First reconcile the false DeepSeek rejection: the active branch is aa50f42 with
the v4 cap still present; main/e5d7f7b is the pre-tune control. The four 1+1
cloud runs were mis-scored by the untracked tools/aggregate_shards.py. Use only
tools.cloud_match.aggregate / official summary.json files. Correct pentanomial
points are W2=2, W1D1=1.5, D2=1, L1D1=0.5, L2=0. The four runs were all SPRT
continue and compared each candidate to 4d25363, not to e5d7f7b, so they prove
neither benefit nor harm from the cap.

Your first task is a direct, properly recorded candidate-vs-e5d7f7b paired
qualification for the cap at 1+1 and 2+1. Use frozen commits/binary hashes,
one thread and 16 MiB per engine, paired colors/openings, the strict aggregator,
and enough games for a real SPRT decision. Preserve raw PGNs/manifests and report
time losses plus latency. Do not use point-score thresholds alone. Do not run
destructive git commands; create an explicit revert commit or a candidate branch
only after the valid evidence supports it.

Then validate one completed 48-way hybrid run with hybrid-summary.json. After
that, prioritize native exact-clock bullet testing, profile-guided hot-path
work, isolated selective-search improvements, and a clean-room incremental
neural evaluator/training pipeline. Stockfish 18 is a long-term external
benchmark, not code to copy and not a claim to make without equal-resource,
statistically significant qualification.
```
