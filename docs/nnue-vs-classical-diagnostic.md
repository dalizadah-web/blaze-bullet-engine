# NNUE Versus Classical Diagnostic

Cloud run: [30262507351](https://github.com/dalizadah-web/blaze-bullet-engine/actions/runs/30262507351)

This is a diagnostic matchup, not a strength claim: both sides used the same
resolved source revision and identical compiled binary. The candidate enabled
direct AVX2 NNUE; the baseline set `UseNNUE=false`.

| Field | Value |
|---|---|
| Source commit | `b280d552f96eb837cbed7aea4d330f083c635ba8` |
| Candidate/baseline binary SHA-256 | `3748f668b3b1693ce6caab3def2af0aaa1f0214111ec3a1c24a8a40d01ad4665` |
| Candidate initialization | `UseNNUE=true`, `EvalFile=nn-c288c895ea92.nnue` |
| Baseline initialization | `UseNNUE=false` |
| Games / pairs | 2000 / 1000 color-swapped pairs |
| Time control | `0.5+0` |
| Threads / hash | 1 / 16 MB |
| Openings | 100 positions, 10 explicit repeats, SHA-256 `5a53816436fe460d788fe1334fc9be27c89ee9bc1d0bdb1ab9745e3081d404bc` |
| Shards | 20 GitHub-hosted Linux runners |

## Result

- Candidate W/D/L: **765 / 500 / 735**
- Clean evidence: **2000 / 2000 games**, **1000 / 1000 pairs**
- Quarantined games: **0**
- Pentanomial: `116 174 436 172 102`
- SPRT (`0.0`, `5.0`, alpha/beta `0.05`): **continue**, LLR `0.368578`
- Terminations: all 2000 were ordinary; no time losses, illegal moves, disconnects, stalls, malformed records, or runner failures.

The strict aggregate artifact is
[`result-45d68617545f6c885e714461`](https://github.com/dalizadah-web/blaze-bullet-engine/actions/runs/30262507351/artifacts/8651539586)
(SHA-256 `bd8b1144ca1711817a76a0006fe3efe723dac6db3289397505f6c87289521fa6`).
