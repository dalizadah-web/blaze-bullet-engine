"""Immutable artifact identities and experiment manifests."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import sys
from typing import Any, Mapping


def sha256_file(path: Path | str) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


@dataclass(frozen=True)
class ArtifactIdentity:
    path: str
    sha256: str
    size_bytes: int

    @classmethod
    def from_path(cls, path: Path | str) -> "ArtifactIdentity":
        resolved = Path(path).resolve(strict=True)
        if not resolved.is_file():
            raise ValueError(f"artifact is not a regular file: {resolved}")
        return cls(
            path=str(resolved),
            sha256=sha256_file(resolved),
            size_bytes=resolved.stat().st_size,
        )

    def verify(self) -> None:
        artifact = Path(self.path)
        if not artifact.is_file():
            raise ValueError(f"artifact is missing: {artifact}")
        actual_size = artifact.stat().st_size
        if actual_size != self.size_bytes:
            raise ValueError(
                f"artifact size mismatch for {artifact}: "
                f"expected {self.size_bytes}, got {actual_size}"
            )
        actual_hash = sha256_file(artifact)
        if actual_hash != self.sha256:
            raise ValueError(
                f"SHA-256 mismatch for {artifact}: expected {self.sha256}, got {actual_hash}"
            )


@dataclass(frozen=True)
class ExperimentManifest:
    schema_version: int
    experiment_id: str
    created_utc: str
    source_commit: str
    candidate: ArtifactIdentity
    opponent: ArtifactIdentity
    openings: ArtifactIdentity
    runner: ArtifactIdentity
    configuration: Mapping[str, Any]
    environment: Mapping[str, str]

    @classmethod
    def create(
        cls,
        *,
        experiment_id: str,
        source_commit: str,
        candidate: ArtifactIdentity,
        opponent: ArtifactIdentity,
        openings: ArtifactIdentity,
        runner: ArtifactIdentity,
        configuration: Mapping[str, Any],
    ) -> "ExperimentManifest":
        if not experiment_id.strip():
            raise ValueError("experiment_id cannot be empty")
        if not source_commit.strip():
            raise ValueError("source_commit cannot be empty")
        for artifact in (candidate, opponent, openings, runner):
            artifact.verify()
        return cls(
            schema_version=1,
            experiment_id=experiment_id,
            created_utc=datetime.now(timezone.utc).isoformat(),
            source_commit=source_commit,
            candidate=candidate,
            opponent=opponent,
            openings=openings,
            runner=runner,
            configuration=dict(configuration),
            environment={
                "hostname": platform.node(),
                "machine": platform.machine(),
                "os": platform.platform(),
                "processor": platform.processor(),
                "python": sys.version.split()[0],
            },
        )

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2, sort_keys=True) + "\n"


def write_manifest(manifest: ExperimentManifest, path: Path | str) -> None:
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_text(manifest.to_json(), encoding="utf-8", newline="\n")
    os.replace(temporary, destination)
