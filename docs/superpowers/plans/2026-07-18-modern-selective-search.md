# Modern Selective Search Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Blaze materially stronger at bullet by replacing fixed pruning and reduction thresholds with bounded, position-aware selective search.

**Architecture:** Keep Blaze's alpha-beta/PVS core and clean-room evaluator. Add a small heuristic layer for adaptive reductions and pruning, extend the per-search state with bounded quiet/capture/continuation/correction histories, and make `negamax` distinguish root/PV/non-PV behavior from its window. Every heuristic is guarded by tactical exceptions, bounded reductions, unit tests, and paired SPRT evaluation.

**Tech Stack:** C++20, existing Blaze test harness, CuteChess match runner, local and GitHub Actions match farm.

## Global Constraints

- Preserve clean-room implementation; do not copy Stockfish source, constants, or NNUE data.
- Keep legal move, mate, draw, stop, and UCI behavior correct.
- Do not accept a strength change from node count alone; require paired color-swapped games and strict SPRT.
- Test each new helper before its production use; cap extension and reduction magnitudes.

---

### Task 1: Selectivity primitives and history storage

**Files:**
- Create: `src/blaze/search/selectivity.h`
- Create: `tests/search/test_selectivity.cpp`
- Modify: `src/blaze/search/search.h`
- Modify: `src/blaze/search/search.cpp`

- [ ] Add tested pure helpers for adaptive LMR, late-move pruning, reverse futility, razoring, null-move reduction, and delta pruning.
- [ ] Add bounded quiet, capture, continuation, and correction-history storage, reset once per root search.
- [ ] Update history only after confirmed cutoffs and clamp every entry.

### Task 2: Node model and dynamic pruning

**Files:**
- Modify: `src/blaze/search/search.cpp`
- Modify: `tests/search/test_search.cpp`

- [ ] Derive root/PV/non-PV behavior from ply and window width.
- [ ] Add static-evaluation correction, improving detection, razor, reverse futility, late-move pruning, and history-aware LMR only at safe non-PV quiet nodes.
- [ ] Replace fixed null-move reduction with a depth/evaluation-margin reduction and a reduced-depth verification search.

### Task 3: Tactical selectivity

**Files:**
- Modify: `src/blaze/search/search.h`
- Modify: `src/blaze/search/search.cpp`
- Modify: `tests/search/test_search.cpp`

- [ ] Add TT-move singular extensions using an exclusion search, with depth and score guards.
- [ ] Keep check and recapture extensions, but cap total extension growth per line.
- [ ] Add TT-aware quiescence, delta pruning, SEE pruning, and check-safe exceptions.

### Task 4: Match infrastructure on the challenger branch

**Files:**
- Add the verified cloud/local match farm commits from `codex/bullet-beast` without search or evaluation behavior changes.
- Modify: `config/cloud/modern-selective-*.json`

- [ ] Import the immutable cloud pipeline and hybrid local lane.
- [ ] Create separate 0+1, 1+0, and 1+1 paired-SPSRT specs against the immediate pre-search candidate.
- [ ] Run local smoke matches before dispatching cloud qualification.

### Task 5: Verification and qualification

**Files:**
- Modify: `docs/audit-report.md`

- [ ] Run all C++ search tests, experiment tests, cloud-match tests, and clean-room verification.
- [ ] Run a fixed-node tactical/regression suite and record node/depth deltas.
- [ ] Dispatch independent local/cloud paired matches; accept only strict SPRT decisions and record every artifact identity.
