# Distributed bullet testing

Use the repository's GitHub Actions match farm for high-throughput candidate-versus-baseline testing. Do not create a second cloud harness.

## Operating contract

- Treat `config/cloud/default-match.json` as the canonical default.
- Launch with `powershell -File tools/cloud_match.ps1 -Action Run`.
- To use all available compute, launch `powershell -File tools/hybrid_match.ps1`; this combines 20 four-vCPU GitHub runners with 8 concurrent local games on the 16-thread PC.
- Inspect with `-Action Status`, wait with `-Action Watch`, and fetch evidence with `-Action Download`.
- Use complete color-swapped pairs. Games must be even and `(games / 2)` must divide evenly by 1-20 shards.
- Default to `threads=1`, `hash_mb=16`, and 20 shards for bullet throughput.
- Candidate and baseline must be immutable pushed refs. Never test uncommitted source.
- Never report a result from partial shards. `tools.cloud_match.aggregate` is the authority and must validate every shard.
- Preserve PGN, shard manifests, frozen hashes, `summary.json`, and `summary.md` for every accepted tuning claim.
- Cloud timing is heterogeneous. Use cloud games for playing-strength selection; use the local bullet latency and deadline gates for subsecond clock correctness.
- Keep local and Linux cloud artifacts as separate lanes because their binary hashes differ. Only `tools.cloud_match.combine` may pool their compatible pentanomial evidence.
- Change one coherent search/time-management idea per candidate. Run a smoke match first, then the full match.

Read `docs/cloud-match-farm.md` before changing the workflow or interpreting a result.
