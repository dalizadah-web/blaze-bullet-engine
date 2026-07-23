# Quarantine note: tools/aggregate_shards.py

This file was an untracked ad-hoc aggregation helper that incorrectly
scored pentanomial results and silently combined shard files from separate
runs. It must never be used for an acceptance decision.

Known issues:
- W1D1 counted as 1.0 instead of 1.5; L1D1 counted as 0.0 instead of 0.5.
- It hardcoded two specific run IDs and overwrote shards by index.
- It did not validate candidate/baseline commit, binary SHA-256, or opening SHA-256.
- It was not included in any test suite and is not part of the production path.

Authoritative replacement:
- tools/cloud_match/aggregate.py
- tools/cloud_match/spec.py
- tools/experiment/pentanomial.py

If forensic reproduction is required, git history can recover the exact
contents, but all valid scoring must pass through the production aggregator.