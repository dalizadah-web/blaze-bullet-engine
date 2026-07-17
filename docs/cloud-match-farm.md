# Distributed bullet match farm

This repository can run candidate-versus-baseline bullet matches on up to 20 public GitHub-hosted runners. Each four-vCPU runner executes two concurrent games because each game has two one-thread engines. That gives about 40 simultaneous cloud games. The hybrid launcher also runs eight games on the local 16-thread CPU, for about 48 simultaneous games in total.

The farm is for high-throughput strength testing and tuning. Public cloud machines have variable load and latency, so local deadline tests remain the authority for 0+1, 1+0, and other clock-sensitive qualification.

## One-time setup

1. Install GitHub CLI:

   ```powershell
   winget install --id GitHub.cli --exact --source winget
   ```

2. Sign in:

   ```powershell
   gh auth login --web
   ```

3. Push `codex/bullet-beast` to a public GitHub repository and set it as `origin`. The workflow file must exist on the branch selected by `-WorkflowRef`.

Public repositories receive free, unlimited use of standard GitHub-hosted runners, subject to GitHub's acceptable-use and concurrency policies. GitHub Free currently permits 20 concurrent standard hosted jobs, and each public `ubuntu-24.04` runner has four vCPUs. See [GitHub-hosted runner specifications](https://docs.github.com/en/actions/reference/runners/github-hosted-runners), [Actions limits](https://docs.github.com/en/enterprise-cloud@latest/actions/reference/limits), and [Actions billing](https://docs.github.com/en/actions/concepts/billing-and-usage). Anything pushed publicly may already have been cloned or forked; changing the repository to private later cannot retract those copies.

## Daily use

From VS Code, run `Terminal > Run Task` and choose `Default Match: Run 48-way hybrid`. Status, watch, download, and cloud-only tasks are beside it.

To use every available CPU directly, run:

```powershell
powershell -File tools/hybrid_match.ps1
```

The hybrid command dispatches the 20-runner cloud lane first, builds committed local candidate and baseline refs, runs eight local games concurrently, waits for cloud completion, downloads all evidence, and writes `artifacts/hybrid/<timestamp>/hybrid-summary.json`. Windows and Linux artifacts remain separate in the report; only compatible game outcomes are pooled.

The default run command automatically uses both lanes (about 48 simultaneous games). The equivalent commands are:

```powershell
powershell -File tools/cloud_match.ps1 -Action Run
powershell -File tools/cloud_match.ps1 -Action Status
powershell -File tools/cloud_match.ps1 -Action Watch
powershell -File tools/cloud_match.ps1 -Action Download
```

To run only the 40-way GitHub lane and leave the local CPU free, add `-CloudOnly`.

Run a different bullet control or candidate explicitly:

```powershell
powershell -File tools/cloud_match.ps1 -Action Run `
  -CandidateRef codex/my-search-change `
  -BaselineRef codex/bullet-beast `
  -Games 800 -Shards 20 -TimeControl "1+0"
```

Valid geometry requires an even number of games and `(games / 2) % shards == 0`. With 20 shards, use totals divisible by 40: 40, 80, 400, 800, and so on.

## What the workflow guarantees

The build job checks out both refs, compiles them once, freezes both engines plus CuteChess and the opening file, and records hashes. Every shard downloads that exact bundle. Opening pairs are assigned deterministically and colors are swapped. The final job rejects missing shards, duplicate game IDs, inconsistent hashes, incomplete PGNs, or malformed pair counts before calculating the pentanomial LLR and SPRT decision.

Evidence is retained as GitHub artifacts:

- `frozen-match-*`: exact binaries, runner, opening book, and match spec;
- `shard-*`: PGN, runner log, local manifest, and strict shard manifest;
- `result-*`: validated `summary.json` and `summary.md`.

## Tuning loop

Commit and push one coherent engine change. Run 40 games first to catch crashes, illegal moves, or integration failures. Then run at least 400 games against the last accepted version. A decision of `continue` is inconclusive, not a win. Increase games while retaining the same candidate, baseline, time control, opening identity, and SPRT hypotheses if evidence is still needed.

For DeepSeek/Cline, keep this repository open in VS Code and point it to `.clinerules/20-cloud-matches.md` and `.clinerules/workflows/run-cloud-match.md`. A sufficient instruction is:

> Use the checked-in cloud match workflow. Test my committed candidate against the last accepted baseline, wait for completion, download the evidence, and report the strict aggregate. Follow the Cline cloud-match rules exactly.
