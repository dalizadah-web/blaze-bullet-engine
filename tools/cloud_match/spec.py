"""Immutable public-runner match specification."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import re


_SHA256 = re.compile(r"^[0-9a-fA-F]{64}$")
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

    def validate(self) -> None:
        if not self.name or not self.candidate_ref or not self.baseline_ref:
            raise ValueError("name, candidate_ref, and baseline_ref are required")
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

    def canonical_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))

    def experiment_id(self) -> str:
        return hashlib.sha256(self.canonical_json().encode("utf-8")).hexdigest()[:24]
