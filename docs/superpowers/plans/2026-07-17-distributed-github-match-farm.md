# Distributed GitHub Match Farm Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a one-click, 20-shard public GitHub Actions match farm with verified aggregation plus VS Code and Cline controls.

**Architecture:** A tested Python cloud layer validates experiment specifications, deterministically assigns complete color-swapped pairs to shards, runs the existing strict match harness, and aggregates only hash-consistent results. GitHub Actions builds frozen binaries, fans out workers, and publishes one result artifact; local PowerShell, VS Code tasks, and Cline workflows expose the same lifecycle.

**Tech Stack:** Python 3.13, C++20/MinGW locally, Linux g++ in Actions, python-chess, Cute Chess, GitHub Actions, PowerShell, VS Code tasks, Cline workspace rules.

## Global Constraints

- Preserve uncommitted edits in `src/blaze/search/time_manager.cpp` and `tests/search/test_time_manager.cpp`.
- Public hosted jobs use at most 20 shards, four vCPUs per runner, and two matches per runner.
- Candidate, baseline, runner, and opening hashes must match across every shard.
- Aggregation fails on missing shards, duplicate IDs, malformed PGN, incomplete color pairs, or mixed artifacts.
- Cloud timing is diagnostic; final sub-second deadline qualification stays local or dedicated.
- Workflow permissions are `contents: read`; no secrets or repository writes.

---

### Task 1: Tested cloud specification and deterministic sharding

**Files:**
- Create: `tools/cloud_match/__init__.py`
- Create: `tools/cloud_match/spec.py`
- Create: `tools/cloud_match/shards.py`
- Create: `tools/cloud_match/test_spec.py`
- Create: `tools/cloud_match/test_shards.py`
- Create: `config/cloud/default-match.json`

**Interfaces:**
- Produces: `CloudMatchSpec.from_json(path)`, `validate()`, `experiment_id()`.
- Produces: `pair_indexes(total_games, shard_index, shard_count)`.

- [ ] Write tests rejecting odd games, shard counts outside 1-20, concurrency outside 1-2, invalid hashes, and non-divisible pairs.
- [ ] Run `python -m unittest discover -s tools/cloud_match -p "test_*.py" -v`; verify import failure.
- [ ] Implement immutable dataclasses, SHA-256 validation, canonical JSON hashing, and modulo pair assignment.
- [ ] Run focused tests; require all pass.
- [ ] Commit `test: add cloud match specification and sharding`.

### Task 2: Worker and fail-closed aggregation

**Files:**
- Create: `tools/cloud_match/worker.py`
- Create: `tools/cloud_match/aggregate.py`
- Create: `tools/cloud_match/test_aggregate.py`
- Modify: `tools/experiment/match.py`

**Interfaces:**
- Worker consumes a spec, shard index/count, frozen binaries, runner, and output directory.
- Aggregator consumes shard directories and produces `summary.json` plus `summary.md`.

- [ ] Write synthetic shard tests for valid merge, missing shard, duplicate game ID, wrong hash, wrong experiment ID, and aggregate pentanomial counts.
- [ ] Run focused tests and verify missing implementation failure.
- [ ] Implement shard manifests containing immutable hashes, assigned pair indexes, result counts, PGN path, environment, crashes, illegal moves, and time losses.
- [ ] Implement aggregation that verifies every expected shard and sums `Pentanomial` fields before computing LLR/decision.
- [ ] Add command-line entry points and run all cloud tests.
- [ ] Commit `test: add distributed match workers and aggregation`.

### Task 3: Public GitHub Actions orchestration

**Files:**
- Create: `.github/workflows/distributed-match.yml`
- Create: `tools/cloud_match/build_bundle.py`
- Create: `tools/cloud_match/test_build_bundle.py`
- Modify: `.gitignore`

**Interfaces:**
- Manual inputs: candidate ref, baseline ref, games, shards, time control, openings, opening hash, label.
- Build artifact: candidate, baseline, pinned runner, `build-manifest.json`, and cloud spec.
- Worker artifacts: `shard-N` directories.
- Aggregate artifact: `blaze-match-<experiment-id>`.

- [ ] Write manifest tests proving two refs and binaries remain distinct and hashes round-trip.
- [ ] Implement a build helper producing immutable artifact metadata.
- [ ] Add workflow jobs `prepare`, `match`, and `aggregate`; set `max-parallel: 20`, job timeout 300 minutes, artifact retention 14 days, and least privilege.
- [ ] Add local YAML/schema validation and run Python/C++ suites.
- [ ] Commit `ci: add distributed GitHub match farm`.

### Task 4: VS Code, PowerShell, Cline, and documentation

**Files:**
- Create: `tools/cloud_match.ps1`
- Create: `.vscode/tasks.json`
- Create: `.clinerules/20-cloud-matches.md`
- Create: `.clinerules/workflows/run-cloud-match.md`
- Create: `docs/cloud-match-farm.md`
- Modify: `Makefile.blaze`

**Interfaces:**
- PowerShell actions: `validate`, `smoke`, `open`, `aggregate`, `status`.
- VS Code tasks call those actions.
- Cline workflow validates the dirty tree, preserves the active tune, and launches through `gh` when installed or the web UI otherwise.

- [ ] Add script tests through `validate` and a local fixture smoke.
- [ ] Implement the PowerShell dispatcher with clear missing-remote and missing-`gh` diagnostics.
- [ ] Add VS Code tasks and Cline rules/workflow.
- [ ] Document public-repository setup, workflow use, artifact download, result interpretation, private migration, and the copyable DeepSeek prompt.
- [ ] Run full verification and a local smoke.
- [ ] Commit `docs: add match farm developer controls`.

### Task 5: GitHub publication and live smoke

**Files:** No source changes unless live validation finds a reproducible defect.

- [ ] Install GitHub CLI if absent.
- [ ] Authenticate through the official browser flow.
- [ ] Create or attach a public GitHub repository without rewriting local history.
- [ ] Push `codex/bullet-beast` and preserve the dirty tune locally.
- [ ] Dispatch a two-shard, 20-game smoke.
- [ ] Verify all jobs and download the aggregate artifact.
- [ ] Record the repository/workflow URL and exact usage instructions.

