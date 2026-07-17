"""Immutable public-runner match specification.

Identity model (two-stage freeze):
  Stage 1 (prepare): resolve mutable ref text to 40-char Git SHA-1 commit
    IDs. The spec is valid for planning once commits are resolved.
  Stage 2 (after build): record the built candidate/baseline binary
    SHA-256 digests. Aggregation requires these so mismatched binaries
    can never be pooled.

A Git commit is a 40-character SHA-1; an artifact hash is a 64-character
SHA-256. The implementation deliberately separates the two.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, replace
import hashlib
import json
from pathlib import Path
import re
import subprocess


_GIT_SHA1 = re.compile(r"^[0-9a-f]{40}$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_TIME_CONTROL = re.compile(r"^[0-9]+(?:\.[0-9]+)?\+[0-9]+(?:\.[0-9]+)?$")


@dataclass(frozen=True)
class CloudSprtSpec:
    elo0: float
    elo1: float
    alpha: float
    beta: float


@dataclass(frozen=True)
class CloudMatchSpec:
    schema_version: int
    name: str
    candidate_ref: str
    baseline_ref: str
    candidate_commit: str
    baseline_commit: str
    candidate_sha256: str
    baseline_sha256: str
    games: int
    shards: int
    concurrency: int
    time_control: str
    threads: int
    hash_mb: int
    openings: str
    opening_sha256: str
    sprt: CloudSprtSpec

    @classmethod
    def from_json(cls, path: Path | str) -> "CloudMatchSpec":
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
        if raw.get("schema_version") != 1:
            raise ValueError("schema_version must be 1")
        sprt_raw = raw.get("sprt")
        if not isinstance(sprt_raw, dict):
            raise ValueError("sprt is required")
        spec = cls(
            schema_version=1,
            name=str(raw.get("name", "")).strip(),
            candidate_ref=str(raw.get("candidate_ref", "")).strip(),
            baseline_ref=str(raw.get("baseline_ref", "")).strip(),
            candidate_commit=str(raw.get("candidate_commit", "")).lower(),
            baseline_commit=str(raw.get("baseline_commit", "")).lower(),
            candidate_sha256=str(raw.get("candidate_sha256", "")).lower(),
            baseline_sha256=str(raw.get("baseline_sha256", "")).lower(),
            games=raw.get("games"),
            shards=raw.get("shards"),
            concurrency=raw.get("concurrency"),
            time_control=str(raw.get("time_control", "")),
            threads=raw.get("threads"),
            hash_mb=raw.get("hash_mb"),
            openings=str(raw.get("openings", "")).strip(),
            opening_sha256=str(raw.get("opening_sha256", "")).lower(),
            sprt=CloudSprtSpec(
                elo0=float(sprt_raw["elo0"]),
                elo1=float(sprt_raw["elo1"]),
                alpha=float(sprt_raw["alpha"]),
                beta=float(sprt_raw["beta"]),
            ),
        )
        spec.validate()
        return spec

    def with_resolved_commits(self, candidate_commit: str, baseline_commit: str) -> "CloudMatchSpec":
        return replace(
            self,
            candidate_commit=candidate_commit.lower(),
            baseline_commit=baseline_commit.lower(),
        )

    def with_binary_hashes(self, candidate_sha256: str, baseline_sha256: str) -> "CloudMatchSpec":
        return replace(
            self,
            candidate_sha256=candidate_sha256.lower(),
            baseline_sha256=baseline_sha256.lower(),
        )

    def validate(self) -> None:
        if not self.name or not self.candidate_ref or not self.baseline_ref:
            raise ValueError("name, candidate_ref, and baseline_ref are required")
        # Resolved Git commits are 40-char SHA-1. They are required for any
        # spec that has advanced past planning (commits must be frozen).
        if not _GIT_SHA1.fullmatch(self.candidate_commit):
            raise ValueError("candidate_commit must be a 40-char Git SHA-1")
        if not _GIT_SHA1.fullmatch(self.baseline_commit):
            raise ValueError("baseline_commit must be a 40-char Git SHA-1")
        if not isinstance(self.games, int) or self.games <= 0 or self.games % 2:
            raise ValueError("games must be a positive even number")
        if not isinstance(self.shards, int) or not 1 <= self.shards <= 20:
            raise ValueError("shards must be between 1 and 20")
        if (self.games // 2) % self.shards:
            raise ValueError("game pairs must divide evenly across shards")
        if not isinstance(self.concurrency, int) or not 1 <= self.concurrency <= 2:
            raise ValueError("concurrency must be between 1 and 2")
        if not isinstance(self.threads, int) or not 1 <= self.threads <= 2:
            raise ValueError("threads must be between 1 and 2")
        if not isinstance(self.hash_mb, int) or self.hash_mb <= 0:
            raise ValueError("hash_mb must be positive")
        if not _TIME_CONTROL.fullmatch(self.time_control):
            raise ValueError("time_control must use base+increment seconds")
        if not self.openings or not _SHA256.fullmatch(self.opening_sha256):
            raise ValueError("openings and a valid opening_sha256 are required")
        if self.sprt.elo1 <= self.sprt.elo0:
            raise ValueError("sprt elo1 must exceed elo0")
        if not 0 < self.sprt.alpha < 1 or not 0 < self.sprt.beta < 1:
            raise ValueError("sprt alpha and beta must be between 0 and 1")

    def validate_frozen(self) -> None:
        """Strict check used before/after a cloud run: binary hashes required."""
        self.validate()
        if not _SHA256.fullmatch(self.candidate_sha256):
            raise ValueError("candidate_sha256 must be a 64-char SHA-256 digest")
        if not _SHA256.fullmatch(self.baseline_sha256):
            raise ValueError("baseline_sha256 must be a 64-char SHA-256 digest")

    def canonical_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))

    def resolve_commits(self, repo_root: Path | str) -> tuple[str, str]:
        """Resolve the mutable candidate/baseline refs to Git SHA-1 IDs."""
        repo = Path(repo_root)
        commits: list[str] = []
        for ref in (self.candidate_ref, self.baseline_ref):
            command = ["git", "-C", str(repo), "rev-parse", ref]
            completed = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                check=False,
            )
            if completed.returncode != 0:
                raise ValueError(
                    f"cannot resolve ref {ref!r}: {completed.stderr.strip()}"
                )
            value = completed.stdout.strip()
            if not _GIT_SHA1.fullmatch(value):
                raise ValueError(f"ref {ref!r} did not resolve to a Git SHA-1: {value!r}")
            commits.append(value)
        return commits[0], commits[1]

    def experiment_id(self) -> str:
        return hashlib.sha256(self.canonical_json().encode("utf-8")).hexdigest()[:24]