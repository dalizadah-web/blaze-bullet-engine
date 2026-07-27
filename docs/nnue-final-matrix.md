# Final NNUE Benchmark Matrix

Eight deterministic positions, five runs each, seed `20260726`, Ryzen 7 7700, MinGW g++ 15.2.0, pinned `nn-c288c895ea92.nnue`.

## Lane Identities

| Lane | Engine SHA-256 | Backend |
|---|---|---|
| avx2 | `249bb2695de6a4519b46b492e1f8a19a5803584bb23fb0f18aabdc4796f0811e` | avx2 |
| scalar | `249bb2695de6a4519b46b492e1f8a19a5803584bb23fb0f18aabdc4796f0811e` | scalar |
| classical | `249bb2695de6a4519b46b492e1f8a19a5803584bb23fb0f18aabdc4796f0811e` | classical |
| legacy_bridge | `1d23fe9c750fb056eb2a520669177672343714f08139df15b55e8d5c05e2540b` | avx2 |

## Search Matrix

| Evaluator | Limit | T | Median NPS | IQR NPS | Scaling | Scalar/AVX2 | Direct/classical | Direct/legacy |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| avx2 | depth 4 | 1 | 37149.5 | 16590.9 | 1.000 | 1.193 | 0.084 | 1.149 |
| classical | depth 4 | 1 | 444615.4 | 66615.4 | 1.000 | - | - | - |
| legacy_bridge | depth 4 | 1 | 32331.7 | 20005.8 | 1.000 | - | - | - |
| scalar | depth 4 | 1 | 31139.6 | 11522.1 | 1.000 | - | - | - |
| avx2 | depth 4 | 2 | 90625.0 | 36509.3 | 1.220 | 1.311 | 0.141 | 0.787 |
| classical | depth 4 | 2 | 642214.3 | 126133.0 | 0.722 | - | - | - |
| legacy_bridge | depth 4 | 2 | 115105.1 | 47099.8 | 1.780 | - | - | - |
| scalar | depth 4 | 2 | 69122.2 | 26802.1 | 1.110 | - | - | - |
| avx2 | depth 4 | 4 | 105734.5 | 54852.7 | 0.712 | 1.236 | 0.122 | 0.893 |
| classical | depth 4 | 4 | 863947.3 | 225094.3 | 0.486 | - | - | - |
| legacy_bridge | depth 4 | 4 | 118345.5 | 47628.8 | 0.915 | - | - | - |
| scalar | depth 4 | 4 | 85552.1 | 48604.9 | 0.687 | - | - | - |
| avx2 | depth 4 | 8 | 114040.0 | 62576.2 | 0.384 | 1.107 | 0.129 | 1.064 |
| classical | depth 4 | 8 | 886573.7 | 215431.8 | 0.249 | - | - | - |
| legacy_bridge | depth 4 | 8 | 107154.3 | 45701.4 | 0.414 | - | - | - |
| scalar | depth 4 | 8 | 103029.5 | 54175.3 | 0.414 | - | - | - |
| avx2 | depth 6 | 1 | 106912.6 | 30091.1 | 1.000 | 1.494 | 0.215 | 1.251 |
| classical | depth 6 | 1 | 497546.6 | 51914.2 | 1.000 | - | - | - |
| legacy_bridge | depth 6 | 1 | 85429.1 | 56203.9 | 1.000 | - | - | - |
| scalar | depth 6 | 1 | 71550.3 | 13826.1 | 1.000 | - | - | - |
| avx2 | depth 6 | 2 | 212468.0 | 82541.8 | 0.994 | 1.589 | 0.314 | 0.442 |
| classical | depth 6 | 2 | 675580.3 | 85050.2 | 0.679 | - | - | - |
| legacy_bridge | depth 6 | 2 | 480898.2 | 195747.6 | 2.815 | - | - | - |
| scalar | depth 6 | 2 | 133741.6 | 45325.1 | 0.935 | - | - | - |
| avx2 | depth 6 | 4 | 330188.1 | 126994.7 | 0.772 | 1.590 | 0.362 | 0.643 |
| classical | depth 6 | 4 | 912729.3 | 221608.1 | 0.459 | - | - | - |
| legacy_bridge | depth 6 | 4 | 513901.5 | 254603.8 | 1.504 | - | - | - |
| scalar | depth 6 | 4 | 207627.1 | 65713.6 | 0.725 | - | - | - |
| avx2 | depth 6 | 8 | 373504.0 | 174222.0 | 0.437 | 1.524 | 0.362 | 0.722 |
| classical | depth 6 | 8 | 1032737.9 | 431328.9 | 0.259 | - | - | - |
| legacy_bridge | depth 6 | 8 | 517163.9 | 273284.2 | 0.757 | - | - | - |
| scalar | depth 6 | 8 | 245032.3 | 80419.1 | 0.428 | - | - | - |
| avx2 | depth 8 | 1 | 170315.1 | 12837.3 | 1.000 | 1.841 | 0.335 | 1.058 |
| classical | depth 8 | 1 | 508707.9 | 47398.0 | 1.000 | - | - | - |
| legacy_bridge | depth 8 | 1 | 160906.8 | 75211.3 | 1.000 | - | - | - |
| scalar | depth 8 | 1 | 92530.0 | 7676.3 | 1.000 | - | - | - |
| avx2 | depth 8 | 2 | 290967.8 | 77844.0 | 0.854 | 1.950 | 0.403 | 0.391 |
| classical | depth 8 | 2 | 722040.1 | 188601.8 | 0.710 | - | - | - |
| legacy_bridge | depth 8 | 2 | 744056.4 | 304457.7 | 2.312 | - | - | - |
| scalar | depth 8 | 2 | 149238.4 | 30883.0 | 0.806 | - | - | - |
| avx2 | depth 8 | 4 | 451689.1 | 111585.7 | 0.663 | 1.878 | 0.535 | 0.487 |
| classical | depth 8 | 4 | 844480.5 | 491804.2 | 0.415 | - | - | - |
| legacy_bridge | depth 8 | 4 | 927035.6 | 411628.7 | 1.440 | - | - | - |
| scalar | depth 8 | 4 | 240514.9 | 56716.4 | 0.650 | - | - | - |
| avx2 | depth 8 | 8 | 470045.9 | 237770.6 | 0.345 | 1.565 | 0.415 | 0.454 |
| classical | depth 8 | 8 | 1133592.9 | 597025.5 | 0.279 | - | - | - |
| legacy_bridge | depth 8 | 8 | 1035386.0 | 544748.2 | 0.804 | - | - | - |
| scalar | depth 8 | 8 | 300441.1 | 138587.4 | 0.406 | - | - | - |
| avx2 | nodes 10000 | 1 | 71428.6 | 4204.7 | 1.000 | 1.343 | 0.171 | 0.736 |
| classical | nodes 10000 | 1 | 416666.7 | 87482.0 | 1.000 | - | - | - |
| legacy_bridge | nodes 10000 | 1 | 97087.4 | 4797.6 | 1.000 | - | - | - |
| scalar | nodes 10000 | 1 | 53191.5 | 7082.2 | 1.000 | - | - | - |
| avx2 | nodes 10000 | 2 | 88107.4 | 3464.1 | 0.617 | 1.198 | 0.141 | 637.085 |
| classical | nodes 10000 | 2 | 625000.0 | 78431.4 | 0.750 | - | - | - |
| legacy_bridge | nodes 10000 | 2 | 138.3 | 25.1 | 0.001 | - | - | - |
| scalar | nodes 10000 | 2 | 73529.4 | 4600.2 | 0.691 | - | - | - |
| avx2 | nodes 10000 | 4 | 94788.9 | 5689.3 | 0.332 | 1.119 | 0.114 | 699.979 |
| classical | nodes 10000 | 4 | 833333.3 | 242424.2 | 0.500 | - | - | - |
| legacy_bridge | nodes 10000 | 4 | 135.4 | 23.0 | 0.000 | - | - | - |
| scalar | nodes 10000 | 4 | 84745.8 | 5752.1 | 0.398 | - | - | - |
| avx2 | nodes 10000 | 8 | 86206.9 | 9948.0 | 0.151 | 0.957 | 0.069 | 646.535 |
| classical | nodes 10000 | 8 | 1250000.0 | 317460.3 | 0.375 | - | - | - |
| legacy_bridge | nodes 10000 | 8 | 133.3 | 25.1 | 0.000 | - | - | - |
| scalar | nodes 10000 | 8 | 90090.1 | 5156.4 | 0.212 | - | - | - |
| avx2 | nodes 100000 | 1 | 156863.6 | 15239.7 | 1.000 | 1.690 | 0.314 | 0.323 |
| classical | nodes 100000 | 1 | 500000.0 | 86163.0 | 1.000 | - | - | - |
| legacy_bridge | nodes 100000 | 1 | 485436.9 | 20039.0 | 1.000 | - | - | - |
| scalar | nodes 100000 | 1 | 92807.4 | 7822.5 | 1.000 | - | - | - |
| avx2 | nodes 100000 | 2 | 264668.8 | 38496.2 | 0.844 | 1.745 | 0.381 | 10.373 |
| classical | nodes 100000 | 2 | 694444.4 | 190790.4 | 0.694 | - | - | - |
| legacy_bridge | nodes 100000 | 2 | 25515.8 | 27117.4 | 0.026 | - | - | - |
| scalar | nodes 100000 | 2 | 151632.2 | 18582.5 | 0.817 | - | - | - |
| avx2 | nodes 100000 | 4 | 369687.0 | 64672.4 | 0.589 | 1.479 | 0.420 | 14.729 |
| classical | nodes 100000 | 4 | 881074.4 | 369027.2 | 0.441 | - | - | - |
| legacy_bridge | nodes 100000 | 4 | 25099.1 | 25616.8 | 0.013 | - | - | - |
| scalar | nodes 100000 | 4 | 250001.6 | 37847.0 | 0.673 | - | - | - |
| avx2 | nodes 100000 | 8 | 400006.4 | 70740.3 | 0.319 | 1.328 | 0.316 | 15.848 |
| classical | nodes 100000 | 8 | 1265822.8 | 634890.7 | 0.316 | - | - | - |
| legacy_bridge | nodes 100000 | 8 | 25239.6 | 27015.0 | 0.006 | - | - | - |
| scalar | nodes 100000 | 8 | 301204.8 | 63961.2 | 0.406 | - | - | - |

## Component Profile

Depth-8 AVX2, one thread: 5047635 nodes, 4210910 qnodes, 83.42% qsearch, 0.6726 evaluations/node, 100.00% incremental evaluations.

| Component | Estimated search share | Calls | Average ns |
|---|---:|---:|---:|
| nnue_delta | 34.736% | 7133430 | 1529.54 |
| full_threats_incremental | 14.506% | 9839895 | 463.05 |
| first_affine | 7.341% | 3395130 | 679.19 |
| delta_fixed_attackers | 6.152% | 14288230 | 135.25 |
| delta_emission | 6.052% | 7133430 | 266.49 |
| delta_feature_generation | 5.146% | 7133430 | 226.60 |
| halfka_incremental | 3.818% | 9564150 | 125.41 |
| delta_slider_before | 3.590% | 14288230 | 78.93 |
| delta_slider_after | 3.295% | 14288230 | 72.43 |
| full_threats_refresh | 2.682% | 192905 | 4367.89 |
| halfka_refresh | 2.280% | 468650 | 1528.19 |
| delta_capture_occupancy_before | 1.925% | 7133430 | 84.75 |
| delta_move_decode | 1.551% | 19314270 | 25.23 |
| delta_piece_square_lookup | 1.306% | 14288230 | 28.71 |
| feature_transform | 1.253% | 6790260 | 57.98 |
| delta_copy_into_state | 1.092% | 7133430 | 48.10 |
| hidden_affine | 0.680% | 3395130 | 62.87 |
| activation | 0.628% | 6790260 | 29.05 |
| delta_compute_occupancy_after | 0.549% | 7133430 | 24.17 |
| delta_changed_squares | 0.539% | 7133430 | 23.72 |
| psqt | 0.262% | 3395130 | 24.21 |
| output_layer | 0.255% | 3395130 | 23.62 |
| public_score | 0.251% | 3395130 | 23.20 |
| refresh_cache_lookup | 0.009% | 40 | 68965.00 |
| delta_debug_oracle | 0.000% | 0 | 0.00 |

## Notes

- Every search lane uses eight deterministic positions, five runs, seed 20260726, and the pinned Big network.
- The legacy bridge lane is commit d063114cf8de07e27da74b9c19a6dfa04677927e; it predates direct NNUE kernels.
- Legacy fixed-node behavior predates the current exact parallel node-limit implementation; depth results are the comparable legacy lane.
- Component timings are one-in-64 sampled estimates from the current AVX2 profile build at depth 8 and one thread.
