# Run and evaluate a cloud bullet experiment

1. Confirm the candidate is committed and pushed. Record `git rev-parse HEAD`.
2. Select a pushed baseline ref. For incremental tuning, use the last accepted candidate.
3. Prefer the hybrid launcher when the local machine is available:

   `powershell -File tools/hybrid_match.ps1 -CandidateRef <candidate> -BaselineRef <baseline> -CloudGames 400 -LocalGames 400`

   This targets 40 simultaneous cloud games plus 8 simultaneous local games. Use the cloud-only commands below when the PC must remain free.

4. Dispatch a 40-game cloud smoke test:

   `powershell -File tools/cloud_match.ps1 -Action Run -CloudOnly -Games 40 -Shards 20 -CandidateRef <candidate> -BaselineRef <baseline>`

5. Wait for it:

   `powershell -File tools/cloud_match.ps1 -Action Watch`

6. If infrastructure and engine health are clean, dispatch the full 48-way hybrid test:

   `powershell -File tools/cloud_match.ps1 -Action Run -CandidateRef <candidate> -BaselineRef <baseline>`

7. Download the immutable evidence:

   `powershell -File tools/cloud_match.ps1 -Action Download -Output artifacts/cloud-match/<candidate-id>`

8. Read `result-*/summary/summary.json`, or `hybrid-summary.json` for a hybrid run. Accept or reject only the computed decision. If the decision is `continue`, run more games; do not reinterpret it as a win.
9. Report candidate ref, baseline ref, time control, games, pentanomial counts, LLR, decision, per-lane hashes, and artifact path.

For a 1+0 test, pass `-TimeControl "1+0"`. For 0+1, CuteChess notation is `0+1`. Keep concurrency at two games per runner through the checked-in spec.
