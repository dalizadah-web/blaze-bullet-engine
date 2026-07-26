# NNUE Delta Construction Profile

`make -f Makefile.blaze profile` exposes sampled timing for each delta phase.
The scopes and counters are guarded by `BLAZE_NNUE_BENCHMARK`; normal release
builds contain neither clocks nor counter updates. The debug/full-delta oracle
is separately named as `delta_debug_oracle`, and is absent from the release
profile (zero calls).

The eight-position, one-thread depth-8 profile measured the following largest
pre-optimization delta phases:

| Phase | Estimated ns | Calls | Share of delta |
| --- | ---: | ---: | ---: |
| Fixed attacker discovery | 808,594,469 | 2,857,646 | 33.9% |
| Slider discovery after move | 668,527,960 | 3,536,672 | 28.1% |
| Threat removal/addition emission | 399,564,387 | 2,105,712 | 16.8% |
| Threat descriptor generation | 357,753,436 | 2,784,738 | 15.0% |

Move decoding, state copying, before/after occupancy capture, changed-square
generation, fixed-attacker discovery, before/after slider discovery, piece and
square lookup, descriptor generation, emission, and debug-oracle work are all
reported independently. `tools/profile_nnue_components.py` now additionally
reports each delta phase as `percent_delta`.

The retained optimization targets the largest phase. A single initialized
fixed-attacker mask lookup returns pawn, knight, and king reverse-attack masks
for each color. The caller then emits the known piece type directly instead of
calling three separately initialized attack accessors and recovering the piece
from its square. This preserves the position-to-NNUE delta boundary and avoids
the rejected source-dedup design.

The same profile reduced fixed-attacker discovery to 420,311,699 estimated ns
(21.0% of delta time). The next largest phase is after-move slider discovery
(34.8%), which is intentionally left for the next evidence-driven task.

On the AMD Ryzen 7 7700 / MinGW g++ 15.2.0 depth-8 eight-position matrix with
ten repetitions, the post-task-2 baseline median was 158,013 NPS (IQR 15,499)
and the candidate was 172,897 NPS (IQR 15,406). Nodes and selected best moves
were unchanged for every sample.
