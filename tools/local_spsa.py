"""Run the deterministic SPSA search campaign on one local machine."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any

from tools.cloud_match.spsa import final_vector, initialize_state, plan_trial, validate_config
from tools.experiment.match import validate_evidence_payload


def _canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def _sha256(value: object) -> str:
    return hashlib.sha256(_canonical(value)).hexdigest()


def _read(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def _write(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def _apply_local_trial(
    config: dict[str, Any], state: dict[str, Any], plan: dict[str, Any], summary: dict[str, Any]
) -> tuple[dict[str, Any], dict[str, Any]]:
    match = config["match"]
    evidence = validate_evidence_payload(
        summary, context="local SPSA summary", expected_games=match["games"]
    )
    if (
        evidence.completed_games != match["games"]
        or evidence.clean_games != match["games"]
        or evidence.quarantined_games
        or evidence.abnormal_games
    ):
        raise ValueError("local SPSA requires complete clean evidence")
    if summary.get("candidate_initstr") != plan["candidate_initstr"]:
        raise ValueError("local candidate initialization does not match the deterministic plan")
    if summary.get("baseline_initstr") != plan["baseline_initstr"]:
        raise ValueError("local baseline initialization does not match the deterministic plan")
    if summary.get("local_concurrency") != config["local_concurrency"]:
        raise ValueError("local SPSA requires eight concurrent games")
    if summary.get("configuration", {}).get("time_control") != config["local_time_control"]:
        raise ValueError("local time control does not match the campaign configuration")

    counts = dict(zip(
        ("wins2", "wins1_draw1", "draws2", "losses1_draw1", "losses2"),
        evidence.counts.as_tuple(),
        strict=True,
    ))
    direction = (
        counts["wins2"] + 0.5 * counts["wins1_draw1"]
        - 0.5 * counts["losses1_draw1"] - counts["losses2"]
    ) / evidence.clean_pairs
    if not plan["candidate_is_plus"]:
        direction = -direction

    iteration = state["next_iteration"]
    gain = config["gain"]
    a_k = gain["a0"] * ((gain["A"] + 1) / (gain["A"] + iteration + 1)) ** gain["alpha"]
    centers = [float.fromhex(value) for value in state["centers_hex"]]
    updated: list[float] = []
    gradients: list[float] = []
    for parameter, center in zip(config["parameters"], centers, strict=True):
        span = parameter["max"] - parameter["min"]
        separation = (plan["plus"][parameter["name"]] - plan["minus"][parameter["name"]]) / span
        if separation == 0:
            raise ValueError("zero effective SPSA perturbation")
        gradient = direction / separation
        gradients.append(gradient)
        updated.append(min(1.0, max(0.0, center + a_k * gradient)))

    next_iteration = iteration + 1
    tail_sums = [float.fromhex(value) for value in state["tail_sums_hex"]]
    tail_count = state["tail_count"]
    if next_iteration > config["iterations"] // 2:
        tail_sums = [total + value for total, value in zip(tail_sums, updated, strict=True)]
        tail_count += 1
    record_core = {
        "trial_id": plan["trial_id"],
        "iteration": iteration,
        "lane": "local-windows-16-thread-cpu",
        "direction_hex": direction.hex(),
        "a_k_hex": a_k.hex(),
        "gradients_hex": [value.hex() for value in gradients],
        "centers_after_hex": [value.hex() for value in updated],
        "counts": counts,
        "summary_sha256": _sha256(summary),
        "previous_trial_sha256": state["last_trial_sha256"],
    }
    record = record_core | {"record_sha256": _sha256(record_core)}
    next_state = dict(state)
    next_state.update({
        "next_iteration": next_iteration,
        "centers_hex": [value.hex() for value in updated],
        "tail_sums_hex": [value.hex() for value in tail_sums],
        "tail_count": tail_count,
        "applied_trials": [*state["applied_trials"], plan["trial_id"]],
        "last_trial_sha256": record["record_sha256"],
        "status": "training-complete" if next_iteration == config["iterations"] else "running",
    })
    return next_state, record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--engine-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-iterations", type=int)
    args = parser.parse_args()

    config = _read(args.config)
    validate_config(config)
    if config.get("local_concurrency") != 8:
        raise ValueError("local SPSA is fixed to eight concurrent games")
    if config.get("local_time_control") != "1+0.1":
        raise ValueError("local SPSA is fixed to a 1+0.1 time control")
    if args.max_iterations is not None and args.max_iterations <= 0:
        raise ValueError("max_iterations must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    state_path = output / "state.json"
    state = _read(state_path) if state_path.exists() else initialize_state(
        config, args.engine_commit, args.engine_commit
    )
    if state["engine_commit"] != args.engine_commit or state["controller_commit"] != args.engine_commit:
        raise ValueError("local state is bound to a different engine commit")

    while state["status"] == "running" and (
        args.max_iterations is None or state["next_iteration"] < args.max_iterations
    ):
        plan = plan_trial(config, state)
        trial_dir = output / "trials" / f"{plan['iteration']:04d}-{plan['trial_id']}"
        summary_path = trial_dir / "lane-summary.json"
        if not summary_path.exists():
            command = [
                sys.executable, "-m", "tools.cloud_match.local_lane",
                "--base-spec", str(config["match"]["base_config"]),
                "--candidate-ref", args.engine_commit,
                "--baseline-ref", args.engine_commit,
                "--candidate", str(args.engine),
                "--baseline", str(args.engine),
                "--runner", str(args.runner),
                "--output", str(trial_dir),
                "--games", str(config["match"]["games"]),
                "--opening-start", str(plan["opening_start"]),
                "--concurrency", str(config["local_concurrency"]),
                "--time-control", str(config["local_time_control"]),
                "--threads", "1",
                "--hash-mb", str(config["match"]["hash_mb"]),
                "--candidate-initstr", plan["candidate_initstr"],
                "--baseline-initstr", plan["baseline_initstr"],
                "--source-commit", args.engine_commit,
            ]
            subprocess.run(command, check=True)
        summary = _read(summary_path)
        next_state, record = _apply_local_trial(config, state, plan, summary)
        records = output / "records"
        records.mkdir(exist_ok=True)
        _write(records / f"{plan['iteration']:04d}.json", record)
        _write(state_path, next_state)
        state = next_state
        print(f"completed local SPSA iteration {state['next_iteration']}", flush=True)

    if state["status"] == "training-complete":
        _write(output / "final-vector.json", final_vector(config, state))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
