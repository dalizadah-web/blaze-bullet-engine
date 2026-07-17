# Distributed GitHub Match Farm Design

Date: 2026-07-17

## Objective

Give Blaze an easy, reproducible, zero-cost distributed match system while the
repository is public. A user or Cline task should be able to launch a candidate
versus baseline experiment from GitHub or VS Code, run up to 20 hosted workers
in parallel, and download one verified aggregate result without manually
partitioning openings or combining PGNs.

The initial target is approximately 40 simultaneous one-thread matches: 20
GitHub-hosted four-vCPU runners, each running two matches. This is a throughput
target, not a promise of identical CPU performance across hosted VMs.

## Public Repository Boundary

The repository will be public during development so standard GitHub-hosted
runners remain free under GitHub's current public-repository policy. Match jobs
are directly related to developing and testing Blaze.

Changing the repository to private later prevents new unauthenticated access,
but does not retract source already cloned, cached, mirrored, or forked while it
was public. No API tokens, private keys, paid-service credentials, or personal
data may ever be committed. Network weights and books are public if committed
or uploaded as public Actions artifacts.

When the repository becomes private, hosted execution must switch to the
account's included Actions minutes, paid minutes, or self-hosted runners. The
workflow remains usable, but it is no longer unlimited free hosted compute.

## Architecture

The system has five bounded components:

1. **Experiment preparation** validates inputs, resolves candidate and baseline
   commits, hashes openings, and creates a deterministic shard manifest.
2. **GitHub workflow orchestration** builds frozen artifacts once and launches
   a maximum of 20 matrix jobs.
3. **Match workers** consume disjoint complete color-swapped opening pairs and
   emit PGN plus an immutable result manifest.
4. **Aggregation** rejects corrupt, duplicated, incomplete, or mixed-artifact
   shards before calculating pentanomial statistics and deadline failures.
5. **Developer entry points** expose the same operations through GitHub's Run
   workflow form, VS Code tasks, PowerShell helpers, and Cline rules/workflows.

```text
workflow_dispatch inputs
        |
        v
prepare + build candidate/baseline
        |
        v
20 matrix shards, max-parallel 20
        |
        v
PGN + shard.json + timing.json artifacts
        |
        v
aggregate validation and summary.json
        |
        +--> GitHub job summary
        +--> downloadable experiment artifact
```

## Repository Layout

```text
.github/workflows/distributed-match.yml
.vscode/tasks.json
.clinerules/20-cloud-matches.md
.clinerules/workflows/run-cloud-match.md
config/cloud/default-match.json
docs/cloud-match-farm.md
tools/cloud_match/__init__.py
tools/cloud_match/spec.py
tools/cloud_match/shards.py
tools/cloud_match/aggregate.py
tools/cloud_match/worker.py
tools/cloud_match/test_spec.py
tools/cloud_match/test_shards.py
tools/cloud_match/test_aggregate.py
tools/cloud_match.ps1
```

Existing `tools/experiment` pentanomial, manifest, and paired-PGN code remains
the source of truth. The cloud layer calls it; it does not implement a second
statistical model.

## Experiment Inputs

The manual GitHub form accepts:

- candidate git ref, defaulting to the selected branch SHA;
- baseline git ref, defaulting to the last accepted commit;
- total even game count;
- number of shards from 1 through 20;
- time control;
- engine threads and hash size;
- match concurrency per runner, default 2 and maximum 2;
- opening file path and expected SHA-256;
- optional experiment label;
- runner image fixed to `ubuntu-24.04` for the first release.

The workflow rejects odd games, games not divisible into complete reversed-color
pairs, missing refs, hash mismatches, nonexistent opening files, thread counts
outside 1-2, concurrency outside 1-2, and shard counts outside 1-20.

The default cloud strength protocol uses one thread per engine and two matches
per runner. More local concurrency is not exposed because it oversubscribes a
four-vCPU runner.

## Build and Artifact Freezing

One build job checks out the candidate and baseline commits into separate
directories, builds warning-clean release binaries, runs the C++ and experiment
tests, and records:

- source commit;
- binary SHA-256;
- compiler identity and flags;
- runner image;
- network and book hashes when enabled;
- opening SHA-256;
- complete experiment specification.

The job uploads a build bundle consumed by every shard. Workers never rebuild
or select a different binary. The bundle is retained for 14 days and receives a
unique experiment ID derived from immutable inputs, not from a display label.

Linux is the canonical cloud platform. Blaze's build output may retain the
historical `blaze.exe` filename on Linux, but the workflow marks it executable
and invokes it as a native ELF binary. A pinned Linux Cute Chess binary or a
source-built, hash-recorded runner is included in the build bundle; the existing
Windows `cutechess-cli.exe` is not executed under Linux.

## Deterministic Sharding

An opening pair is the indivisible unit. Pair index `p` belongs to shard
`p % shard_count`. Each shard receives both colors of that opening and a stable
game ID containing the experiment ID, opening index, repeat index, and color.

Every `shard.json` contains:

```json
{
  "schema_version": 1,
  "experiment_id": "sha256-prefix",
  "shard_index": 0,
  "shard_count": 20,
  "candidate_sha256": "...",
  "baseline_sha256": "...",
  "openings_sha256": "...",
  "runner_sha256": "...",
  "expected_games": 100,
  "game_ids": ["..."]
}
```

The worker writes raw PGN before its result manifest. A cancelled job therefore
cannot publish an apparently complete shard.

## Aggregation and Failure Handling

Aggregation fails closed. It rejects:

- a missing shard;
- wrong engine, opening, runner, or specification hashes;
- duplicate game IDs;
- incomplete color reversal;
- unexpected game counts;
- malformed PGN;
- illegal or unknown results;
- a shard from a different runner architecture;
- missing timing/crash metadata.

Accepted shards produce:

- `summary.json` with W/D/L and pentanomial counts;
- confidence and SPRT state from `tools/experiment/pentanomial.py`;
- crash, illegal-move, and time-loss totals;
- per-shard and aggregate throughput;
- raw PGNs and immutable manifests;
- a Markdown GitHub job summary.

A failed shard can be rerun with the same experiment inputs. Aggregation
deduplicates by game ID and accepts exactly one valid record for every expected
game.

## Timing and Hardware Policy

Hosted VMs have variable CPU scheduling. Cloud results are suitable for paired
strength experiments because candidate and baseline share each runner, but they
are not the authority for sub-second deadline qualification.

The cloud system records CPU model and per-move timing. Fixed-node matches are
the preferred search-development signal. Real `0+1`, `1+0`, `1+1`, and network
jitter gates remain local or run on homogeneous dedicated/self-hosted hardware.
Cloud time-control results are reported, but never replace local deadline gates.

Results from different architectures are not silently merged. The initial
workflow uses only x86-64 `ubuntu-24.04` runners. ARM and self-hosted pools may
be added later as separate hardware classes.

## GitHub Workflow Safety

The workflow uses least-privilege permissions:

```yaml
permissions:
  contents: read
```

It has no repository write token, deployment permission, package publication,
or third-party secrets. It uses a repository-level concurrency group so a user
cannot accidentally launch multiple 20-worker farms from the same branch.
Manual cancellation stops all shards. Every job has a five-hour timeout, below
GitHub's six-hour hosted limit.

Artifact retention is 14 days. Large raw artifacts are compressed, and workers
upload only match evidence, not build trees or caches.

## VS Code and Cline Experience

VS Code tasks provide:

- `Blaze: Validate Cloud Match`;
- `Blaze: Run Local Cloud-Match Smoke`;
- `Blaze: Open GitHub Match Workflow`;
- `Blaze: Aggregate Downloaded Results`.

The open-workflow task uses the configured repository URL. It does not require
GitHub CLI. If `gh` is later installed, the PowerShell helper can dispatch and
watch runs from the terminal; without it, the GitHub web form is the supported
launch path.

`.clinerules/20-cloud-matches.md` tells Cline and DeepSeek V4 Flash to:

- preserve unrelated dirty files, especially the active time-manager tune;
- validate and hash every experiment input;
- never weaken pairing or aggregation checks;
- use no more than 20 hosted shards and two matches per runner;
- treat cloud timing as noisy;
- report artifact URLs and exact commit hashes;
- avoid pushing or making a repository public without explicit user authority.

`.clinerules/workflows/run-cloud-match.md` becomes the reusable Cline slash
workflow. It checks status, validates inputs, runs the local smoke, confirms the
target refs, then either dispatches with `gh` or opens/instructs the web form.

## Testing Strategy

Python unit tests cover schema validation, deterministic sharding, pair
integrity, duplicate detection, missing shards, wrong hashes, malformed PGN,
and aggregate statistics. Tests use small synthetic PGNs and never invoke cloud
services.

A local workflow smoke builds two fixed commits and runs 20 games through one
worker plus aggregation. A GitHub smoke uses two shards and 20 games. Full-scale
execution is enabled only after both pass.

Acceptance requires:

- all existing C++ tests;
- all existing experiment tests;
- all new cloud-match tests;
- 1,000-cycle UCI stress;
- clean-room verification;
- a successful two-shard GitHub smoke;
- exact aggregate equality between one-shard and two-shard runs on the same
  deterministic game fixture.

## Delivery Sequence

1. Add tested schemas and deterministic sharding.
2. Add worker execution and fail-closed aggregation.
3. Add the Linux build and 20-shard GitHub workflow.
4. Add PowerShell, VS Code, documentation, and Cline entry points.
5. Run local tests and a two-shard GitHub smoke.
6. Run the first 20-shard baseline experiment and record throughput.
7. Keep the workflow when public; switch to self-hosted or metered runners when
   the repository becomes private.

## Success Criteria

The system is complete when a user can select candidate/baseline refs in the
GitHub UI, launch one workflow, observe up to 20 concurrent workers, and download
one aggregate artifact whose hashes, pair structure, game count, PGN, and
statistics all verify. Cline must be able to perform the same lifecycle from
the checked-in rules without guessing commands or modifying the active engine
tune.
