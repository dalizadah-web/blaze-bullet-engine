"""Deterministic, constrained SPSA planning and evidence application."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import math
from pathlib import Path
import re
from typing import Any

from tools.experiment.match import MatchEvidence, validate_evidence_payload
from tools.experiment.pentanomial import sprt_decision, sprt_llr


_GIT_SHA1 = re.compile(r"^[0-9a-f]{40}$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")


def _canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def _sha256(value: object) -> str:
    return hashlib.sha256(_canonical(value)).hexdigest()


def _read(path: Path | str) -> dict[str, Any]:
    value = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("JSON root must be an object")
    return value


def _write(path: Path | str, value: object) -> None:
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _validate_protocol(protocol: dict[str, Any], *, qualification: bool) -> None:
    if not isinstance(protocol.get("hash_mb"), int) or protocol["hash_mb"] <= 0:
        raise ValueError("hash_mb must be a positive integer")
    peak = protocol.get("required_peak_concurrent_games")
    if not isinstance(peak, int) or isinstance(peak, bool) or not 1 <= peak <= protocol.get("games", 0):
        raise ValueError("required_peak_concurrent_games must fit within the match")
    for key in ("base_config_sha256", "opening_sha256"):
        if not isinstance(protocol.get(key), str) or not _SHA256.fullmatch(protocol[key]):
            raise ValueError(f"{key} must be a lowercase SHA-256 digest")
    for key in ("base_config", "name", "openings"):
        if not isinstance(protocol.get(key), str) or not protocol[key]:
            raise ValueError(f"{key} is required")
    sprt = protocol.get("sprt")
    if not isinstance(sprt, dict) or set(sprt) != {"elo0", "elo1", "alpha", "beta"}:
        raise ValueError("a complete SPRT specification is required")
    if not all(isinstance(sprt[key], (int, float)) for key in sprt):
        raise ValueError("SPRT values must be numeric")
    if sprt["elo1"] <= sprt["elo0"] or not 0 < sprt["alpha"] < 1 or not 0 < sprt["beta"] < 1:
        raise ValueError("invalid SPRT specification")
    if qualification and protocol.get("opening_positions", 0) <= 0:
        raise ValueError("qualification opening_positions must be positive")


def validate_config(config: dict[str, Any]) -> None:
    if config.get("schema_version") != 1:
        raise ValueError("SPSA config schema_version must be 1")
    if not isinstance(config.get("campaign_id"), str) or not re.fullmatch(
        r"[a-z0-9][a-z0-9._-]{0,63}", config["campaign_id"]
    ):
        raise ValueError("campaign_id must be a safe stable identifier")
    seed = config.get("master_seed")
    if not isinstance(seed, str) or len(seed) != 64:
        raise ValueError("master_seed must be 64 hexadecimal characters")
    try:
        bytes.fromhex(seed)
    except ValueError as exc:
        raise ValueError("master_seed must be hexadecimal") from exc
    iterations = config.get("iterations")
    if not isinstance(iterations, int) or iterations < 2:
        raise ValueError("iterations must be at least 2")
    match = config.get("match")
    if not isinstance(match, dict):
        raise ValueError("match configuration is required")
    games = match.get("games")
    shards = match.get("shards")
    concurrency = match.get("concurrency")
    openings = match.get("openings_per_iteration")
    if not isinstance(games, int) or games <= 0 or games % 2:
        raise ValueError("match.games must be a positive even integer")
    if not isinstance(shards, int) or not 1 <= shards <= 40 or games // 2 < shards:
        raise ValueError("match.shards cannot exceed 40 or the available pairs")
    if concurrency != 2 or match.get("threads") != 1 or match.get("time_control") != "1+0":
        raise ValueError("the cloud campaign is fixed to concurrency=2, threads=1, time_control=1+0")
    if not isinstance(openings, int) or openings <= 0 or games != openings * 2:
        raise ValueError("games must equal twice openings_per_iteration")
    if match.get("training_positions", 0) < iterations * openings:
        raise ValueError("training suite is too small for non-overlapping iterations")
    _validate_protocol(match, qualification=False)
    qualification = config.get("qualification")
    if not isinstance(qualification, dict):
        raise ValueError("qualification configuration is required")
    if (
        qualification.get("games") != 2 * qualification.get("opening_positions", 0)
        or qualification.get("shards") != 40
        or qualification.get("concurrency") != 2
        or qualification.get("threads") != 1
        or qualification.get("time_control") != "1+0"
    ):
        raise ValueError("qualification must be a complete paired 40x2 1+0 lane")
    _validate_protocol(qualification, qualification=True)
    gain = config.get("gain")
    if not isinstance(gain, dict):
        raise ValueError("gain schedule is required")
    for name in ("a0", "c0", "alpha", "gamma", "A"):
        if not isinstance(gain.get(name), (int, float)) or gain[name] <= 0:
            raise ValueError(f"gain.{name} must be positive")
    parameters = config.get("parameters")
    if not isinstance(parameters, list) or not parameters:
        raise ValueError("parameters must be a non-empty list")
    names: set[str] = set()
    for parameter in parameters:
        if not isinstance(parameter, dict):
            raise ValueError("each parameter must be an object")
        name = parameter.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("parameter names must be non-empty and unique")
        names.add(name)
        low, default, high, step = (
            parameter.get("min"),
            parameter.get("default"),
            parameter.get("max"),
            parameter.get("step"),
        )
        if not all(isinstance(value, int) for value in (low, default, high, step)):
            raise ValueError(f"integer bounds/default/step required for {name}")
        if (
            not low < high
            or not low <= default <= high
            or step <= 0
            or (default - low) % step
            or (high - low) % step
        ):
            raise ValueError(f"invalid bounds or grid for {name}")


def initialize_state(
    config: dict[str, Any], engine_commit: str, controller_commit: str
) -> dict[str, Any]:
    validate_config(config)
    if not _GIT_SHA1.fullmatch(engine_commit):
        raise ValueError("engine_commit must be a lowercase 40-character Git SHA-1")
    if not _GIT_SHA1.fullmatch(controller_commit):
        raise ValueError("controller_commit must be a lowercase 40-character Git SHA-1")
    centers = [
        (parameter["default"] - parameter["min"]) / (parameter["max"] - parameter["min"])
        for parameter in config["parameters"]
    ]
    plan_hash = _sha256(config)
    return {
        "schema_version": 1,
        "campaign_id": config["campaign_id"],
        "config_sha256": plan_hash,
        "engine_commit": engine_commit,
        "controller_commit": controller_commit,
        "next_iteration": 0,
        "centers_hex": [value.hex() for value in centers],
        "tail_sums_hex": [(0.0).hex() for _ in centers],
        "tail_count": 0,
        "applied_trials": [],
        "last_trial_sha256": "0" * 64,
        "status": "running",
    }


def _validate_state(config: dict[str, Any], state: dict[str, Any]) -> None:
    validate_config(config)
    if state.get("schema_version") != 1:
        raise ValueError("unsupported SPSA state schema")
    if state.get("campaign_id") != config.get("campaign_id"):
        raise ValueError("state campaign identity mismatch")
    if state.get("config_sha256") != _sha256(config):
        raise ValueError("state does not belong to this SPSA configuration")
    if not _GIT_SHA1.fullmatch(str(state.get("engine_commit", ""))):
        raise ValueError("state has an invalid engine commit")
    if not _GIT_SHA1.fullmatch(str(state.get("controller_commit", ""))):
        raise ValueError("state has an invalid controller commit")
    iteration = state.get("next_iteration")
    if not isinstance(iteration, int) or isinstance(iteration, bool) or not 0 <= iteration <= config["iterations"]:
        raise ValueError("state has an invalid next iteration")
    expected_status = "training-complete" if iteration == config["iterations"] else "running"
    if state.get("status") != expected_status:
        raise ValueError("state status contradicts its iteration")
    count = len(config["parameters"])
    for key in ("centers_hex", "tail_sums_hex"):
        values = state.get(key)
        if not isinstance(values, list) or len(values) != count:
            raise ValueError(f"state has invalid {key}")
        try:
            decoded = [float.fromhex(value) for value in values]
        except (TypeError, ValueError) as exc:
            raise ValueError(f"state has invalid {key}") from exc
        if any(not math.isfinite(value) for value in decoded):
            raise ValueError(f"state has non-finite {key}")
        if key == "centers_hex" and any(not 0.0 <= value <= 1.0 for value in decoded):
            raise ValueError("state centers are outside the normalized bounds")
    expected_tail = max(0, iteration - config["iterations"] // 2)
    if state.get("tail_count") != expected_tail:
        raise ValueError("state tail count contradicts its iteration")
    tail_sums = [float.fromhex(value) for value in state["tail_sums_hex"]]
    if any(not 0.0 <= value <= expected_tail for value in tail_sums):
        raise ValueError("state tail sums contradict its iteration")
    trials = state.get("applied_trials")
    if (
        not isinstance(trials, list)
        or len(trials) != iteration
        or len(set(trials)) != len(trials)
        or any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{24}", value) for value in trials)
    ):
        raise ValueError("state has an invalid applied-trial sequence")
    if not isinstance(state.get("last_trial_sha256"), str) or not _SHA256.fullmatch(state["last_trial_sha256"]):
        raise ValueError("state has an invalid trial-chain hash")
    if (iteration == 0) != (state["last_trial_sha256"] == "0" * 64):
        raise ValueError("state trial-chain head contradicts its iteration")


def _digest(config: dict[str, Any], domain: str, iteration: int) -> bytes:
    key = bytes.fromhex(config["master_seed"])
    message = f"blaze-spsa-v1/{domain}/{iteration}".encode("ascii")
    return hmac.new(key, message, hashlib.sha256).digest()


def _signs(config: dict[str, Any], iteration: int, count: int) -> list[int]:
    bits = bytearray()
    block = 0
    while len(bits) * 8 < count:
        bits.extend(_digest(config, f"perturb/{block}", iteration))
        block += 1
    return [1 if (bits[index // 8] >> (index % 8)) & 1 else -1 for index in range(count)]


def _quantize(parameter: dict[str, Any], normalized: float) -> int:
    low, high, step = parameter["min"], parameter["max"], parameter["step"]
    raw = low + (high - low) * min(1.0, max(0.0, normalized))
    index = math.floor((raw - low) / step + 0.5)
    return min(high, max(low, low + index * step))


def _payload(parameters: list[dict[str, Any]], values: list[int]) -> str:
    return ",".join(f"{parameter['name']}={value}" for parameter, value in zip(parameters, values, strict=True))


def plan_trial(config: dict[str, Any], state: dict[str, Any]) -> dict[str, Any]:
    _validate_state(config, state)
    iteration = state["next_iteration"]
    if iteration >= config["iterations"]:
        raise ValueError("campaign has no remaining training iterations")
    parameters = config["parameters"]
    centers = [float.fromhex(value) for value in state["centers_hex"]]
    signs = _signs(config, iteration, len(parameters))
    c_k = config["gain"]["c0"] / ((iteration + 1) ** config["gain"]["gamma"])
    plus = [_quantize(parameter, center + c_k * sign) for parameter, center, sign in zip(parameters, centers, signs, strict=True)]
    minus = [_quantize(parameter, center - c_k * sign) for parameter, center, sign in zip(parameters, centers, signs, strict=True)]
    for index, (parameter, plus_value, minus_value) in enumerate(zip(parameters, plus, minus, strict=True)):
        if plus_value != minus_value:
            continue
        center_value = _quantize(parameter, centers[index])
        low = max(parameter["min"], center_value - parameter["step"])
        high = min(parameter["max"], center_value + parameter["step"])
        if low == high:
            raise ValueError(f"cannot form a distinct perturbation for {parameter['name']}")
        plus[index], minus[index] = (high, low) if signs[index] > 0 else (low, high)
    candidate_is_plus = bool(_digest(config, "role", iteration)[0] & 1)
    fixed = "UseNNUE=true\nEvalFile=nn-c288c895ea92.nnue\nSPSA Params="
    plus_init = fixed + _payload(parameters, plus)
    minus_init = fixed + _payload(parameters, minus)
    candidate_init = plus_init if candidate_is_plus else minus_init
    baseline_init = minus_init if candidate_is_plus else plus_init
    trial_core = {
        "schema_version": 1,
        "campaign_id": state["campaign_id"],
        "iteration": iteration,
        "engine_commit": state["engine_commit"],
        "centers_hex": state["centers_hex"],
        "signs": signs,
        "c_k_hex": c_k.hex(),
        "plus": dict(zip((item["name"] for item in parameters), plus, strict=True)),
        "minus": dict(zip((item["name"] for item in parameters), minus, strict=True)),
        "candidate_is_plus": candidate_is_plus,
        "candidate_initstr": candidate_init,
        "baseline_initstr": baseline_init,
        "opening_start": iteration * config["match"]["openings_per_iteration"] + 1,
        "opening_suite_positions": config["match"]["openings_per_iteration"],
        "previous_trial_sha256": state["last_trial_sha256"],
    }
    return trial_core | {"trial_id": _sha256(trial_core)[:24]}


def apply_trial(
    config: dict[str, Any],
    state: dict[str, Any],
    plan: dict[str, Any],
    dispatch: dict[str, Any],
    summary: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    expected = plan_trial(config, state)
    if plan != expected:
        raise ValueError("trial plan is not the deterministic next plan")
    run_id = _validate_dispatch(dispatch, state, trial_id=plan["trial_id"], iteration=plan["iteration"])
    match = config["match"]
    evidence, _ = _validate_summary(
        summary,
        protocol=match,
        source_run_id=run_id,
        expected_spec={
        "candidate_commit": state["engine_commit"],
        "baseline_commit": state["engine_commit"],
        "candidate_initstr": plan["candidate_initstr"],
        "baseline_initstr": plan["baseline_initstr"],
        "games": match["games"],
        "shards": match["shards"],
        "concurrency": match["concurrency"],
        "time_control": match["time_control"],
        "threads": match["threads"],
        "opening_start": plan["opening_start"],
        "opening_suite_positions": plan["opening_suite_positions"],
        "opening_repeats": 1,
        },
    )
    counts = dict(zip(
        ("wins2", "wins1_draw1", "draws2", "losses1_draw1", "losses2"),
        evidence.counts.as_tuple(),
        strict=True,
    ))
    pair_count = evidence.clean_pairs
    direction = (
        counts["wins2"] + 0.5 * counts["wins1_draw1"]
        - 0.5 * counts["losses1_draw1"] - counts["losses2"]
    ) / pair_count
    if not plan["candidate_is_plus"]:
        direction = -direction
    iteration = state["next_iteration"]
    gain = config["gain"]
    a_k = gain["a0"] * ((gain["A"] + 1) / (gain["A"] + iteration + 1)) ** gain["alpha"]
    parameters = config["parameters"]
    centers = [float.fromhex(value) for value in state["centers_hex"]]
    plus = plan["plus"]
    minus = plan["minus"]
    gradients: list[float] = []
    updated: list[float] = []
    for parameter, center in zip(parameters, centers, strict=True):
        span = parameter["max"] - parameter["min"]
        separation = (plus[parameter["name"]] - minus[parameter["name"]]) / span
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
        "direction_hex": direction.hex(),
        "a_k_hex": a_k.hex(),
        "gradients_hex": [value.hex() for value in gradients],
        "centers_after_hex": [value.hex() for value in updated],
        "counts": counts,
        "source_run_id": run_id,
        "dispatch_sha256": _sha256(dispatch),
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


def final_vector(config: dict[str, Any], state: dict[str, Any]) -> dict[str, int]:
    _validate_state(config, state)
    if state["status"] != "training-complete":
        raise ValueError("final vector is sealed until training is complete")
    parameters = config["parameters"]
    if state["tail_count"]:
        normalized = [
            float.fromhex(total) / state["tail_count"] for total in state["tail_sums_hex"]
        ]
    else:
        normalized = [float.fromhex(value) for value in state["centers_hex"]]
    return {
        parameter["name"]: _quantize(parameter, value)
        for parameter, value in zip(parameters, normalized, strict=True)
    }


def qualification_report(
    config: dict[str, Any],
    state: dict[str, Any],
    final: dict[str, Any],
    dispatch: dict[str, Any],
    summary: dict[str, Any],
) -> dict[str, Any]:
    _validate_state(config, state)
    expected_vector = final_vector(config, state)
    expected_final = _final_document(config, state, expected_vector)
    if final != expected_final:
        raise ValueError("qualification vector is not the frozen tail average")
    run_id = _validate_dispatch(dispatch, state, qualification=True)
    qualification = config["qualification"]
    evidence, decision = _validate_summary(
        summary,
        protocol=qualification,
        source_run_id=run_id,
        expected_spec={
        "candidate_commit": state["engine_commit"],
        "baseline_commit": state["engine_commit"],
        "candidate_initstr": final["initstr"],
        "baseline_initstr": "UseNNUE=true\nEvalFile=nn-c288c895ea92.nnue",
        "games": qualification["games"],
        "shards": qualification["shards"],
        "concurrency": qualification["concurrency"],
        "threads": qualification["threads"],
        "time_control": qualification["time_control"],
        "opening_start": 1,
        "opening_suite_positions": qualification["opening_positions"],
        "opening_repeats": 1,
        },
    )
    wdl = evidence.clean_wdl
    score = (wdl.get("wins", 0) + 0.5 * wdl.get("draws", 0)) / qualification["games"]
    elo = 400.0 * math.log10(score / (1.0 - score)) if 0.0 < score < 1.0 else None
    return {
        "schema_version": 1,
        "campaign_id": state["campaign_id"],
        "engine_commit": state["engine_commit"],
        "final_parameters": expected_vector,
        "clean_wdl": wdl,
        "score": score,
        "elo": elo,
        "sprt_decision": decision,
        "accepted": decision == "accept",
        "source_run_id": run_id,
        "dispatch_sha256": _sha256(dispatch),
        "summary_sha256": _sha256(summary),
    }


def _validate_summary(
    summary: dict[str, Any],
    *,
    protocol: dict[str, Any],
    expected_spec: dict[str, Any],
    source_run_id: int,
) -> tuple[MatchEvidence, str]:
    if summary.get("schema_version") != 3:
        raise ValueError("cloud summary must use schema version 3")
    if summary.get("source_run_id") != source_run_id or summary.get("source_run_attempt") != 1:
        raise ValueError("cloud summary source run identity mismatch")
    spec = summary.get("spec")
    if not isinstance(spec, dict):
        raise ValueError("cloud summary lacks a frozen spec")
    expected = {
        "schema_version": 2,
        "name": protocol["name"],
        "candidate_ref": expected_spec["candidate_commit"],
        "baseline_ref": expected_spec["baseline_commit"],
        "games": protocol["games"],
        "shards": protocol["shards"],
        "concurrency": protocol["concurrency"],
        "threads": protocol["threads"],
        "hash_mb": protocol["hash_mb"],
        "time_control": protocol["time_control"],
        "openings": protocol["openings"],
        "opening_sha256": protocol["opening_sha256"],
        "sprt": protocol["sprt"],
        **expected_spec,
    }
    for key, value in expected.items():
        if spec.get(key) != value:
            raise ValueError(f"cloud summary mismatch for {key}")
    hashes = (spec.get("candidate_sha256"), spec.get("baseline_sha256"))
    if any(not isinstance(value, str) or not _SHA256.fullmatch(value) for value in hashes):
        raise ValueError("cloud summary has invalid engine hashes")
    if hashes[0] != hashes[1]:
        raise ValueError("cloud summary did not use byte-identical engines")
    artifacts = summary.get("artifacts")
    if not isinstance(artifacts, dict):
        raise ValueError("cloud summary lacks artifact identities")
    required_artifacts = {
        "candidate_commit": expected_spec["candidate_commit"],
        "baseline_commit": expected_spec["baseline_commit"],
        "candidate_sha256": hashes[0],
        "baseline_sha256": hashes[1],
        "openings_sha256": protocol["opening_sha256"],
    }
    for key, value in required_artifacts.items():
        if artifacts.get(key) != value:
            raise ValueError(f"cloud artifact mismatch for {key}")
    if not isinstance(artifacts.get("runner_sha256"), str) or not _SHA256.fullmatch(artifacts["runner_sha256"]):
        raise ValueError("cloud summary has an invalid runner hash")
    canonical_spec = dict(spec)
    canonical_spec.pop("opening_count", None)
    if summary.get("experiment_id") != _sha256(canonical_spec)[:24]:
        raise ValueError("cloud summary experiment identity mismatch")
    if summary.get("lane") != "cloud-linux-github-hosted" or summary.get("shards") != protocol["shards"]:
        raise ValueError("cloud summary lane identity mismatch")
    if summary.get("peak_concurrent_games", 0) < protocol["required_peak_concurrent_games"]:
        raise ValueError("cloud match did not achieve the required game concurrency")
    evidence = validate_evidence_payload(
        summary, context="SPSA cloud summary", expected_games=protocol["games"]
    )
    if (
        evidence.completed_games != protocol["games"]
        or evidence.clean_games != protocol["games"]
        or evidence.quarantined_games != 0
        or evidence.abnormal_games
    ):
        raise ValueError("SPSA requires complete clean evidence")
    sprt = protocol["sprt"]
    llr = sprt_llr(evidence.counts, sprt["elo0"], sprt["elo1"])
    decision = sprt_decision(
        evidence.counts, sprt["elo0"], sprt["elo1"], sprt["alpha"], sprt["beta"]
    )
    if not isinstance(summary.get("llr"), (int, float)) or not math.isclose(
        summary["llr"], llr, rel_tol=0.0, abs_tol=1e-12
    ):
        raise ValueError("cloud summary LLR mismatch")
    if summary.get("decision") != decision:
        raise ValueError("cloud summary SPRT decision mismatch")
    return evidence, decision


def _validate_dispatch(
    dispatch: dict[str, Any],
    state: dict[str, Any],
    *,
    trial_id: str | None = None,
    iteration: int | None = None,
    qualification: bool = False,
) -> int:
    if dispatch.get("schema_version") != 1:
        raise ValueError("unsupported dispatch receipt schema")
    if dispatch.get("controller_commit") != state["controller_commit"]:
        raise ValueError("dispatch controller identity mismatch")
    if dispatch.get("child_run_attempt") != 1:
        raise ValueError("only first-attempt child evidence is admissible")
    run_id = dispatch.get("child_run_id")
    if not isinstance(run_id, int) or isinstance(run_id, bool) or run_id <= 0:
        raise ValueError("dispatch receipt has an invalid child run ID")
    if qualification:
        if dispatch.get("campaign_id") != state["campaign_id"] or dispatch.get("kind") != "qualification":
            raise ValueError("qualification dispatch identity mismatch")
    elif dispatch.get("trial_id") != trial_id or dispatch.get("iteration") != iteration:
        raise ValueError("training dispatch identity mismatch")
    return run_id


def _final_document(
    config: dict[str, Any], state: dict[str, Any], vector: dict[str, int]
) -> dict[str, Any]:
    payload = _payload(config["parameters"], list(vector.values()))
    return {
        "schema_version": 1,
        "campaign_id": state["campaign_id"],
        "config_sha256": state["config_sha256"],
        "engine_commit": state["engine_commit"],
        "selection": "mean-of-final-half",
        "tail_count": state["tail_count"],
        "parameters": vector,
        "initstr": "UseNNUE=true\nEvalFile=nn-c288c895ea92.nnue\nSPSA Params=" + payload,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    init_parser = subparsers.add_parser("init")
    init_parser.add_argument("--config", type=Path, required=True)
    init_parser.add_argument("--engine-commit", required=True)
    init_parser.add_argument("--controller-commit", required=True)
    init_parser.add_argument("--output", type=Path, required=True)
    plan_parser = subparsers.add_parser("plan")
    plan_parser.add_argument("--config", type=Path, required=True)
    plan_parser.add_argument("--state", type=Path, required=True)
    plan_parser.add_argument("--output", type=Path, required=True)
    apply_parser = subparsers.add_parser("apply")
    apply_parser.add_argument("--config", type=Path, required=True)
    apply_parser.add_argument("--state", type=Path, required=True)
    apply_parser.add_argument("--plan", type=Path, required=True)
    apply_parser.add_argument("--dispatch", type=Path, required=True)
    apply_parser.add_argument("--summary", type=Path, required=True)
    apply_parser.add_argument("--output", type=Path, required=True)
    apply_parser.add_argument("--record", type=Path, required=True)
    final_parser = subparsers.add_parser("final")
    final_parser.add_argument("--config", type=Path, required=True)
    final_parser.add_argument("--state", type=Path, required=True)
    final_parser.add_argument("--output", type=Path, required=True)
    qualify_parser = subparsers.add_parser("qualify")
    qualify_parser.add_argument("--config", type=Path, required=True)
    qualify_parser.add_argument("--state", type=Path, required=True)
    qualify_parser.add_argument("--final", type=Path, required=True)
    qualify_parser.add_argument("--dispatch", type=Path, required=True)
    qualify_parser.add_argument("--summary", type=Path, required=True)
    qualify_parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    config = _read(args.config)
    if args.command == "init":
        _write(args.output, initialize_state(config, args.engine_commit, args.controller_commit))
    elif args.command == "plan":
        _write(args.output, plan_trial(config, _read(args.state)))
    elif args.command == "apply":
        state, record = apply_trial(
            config,
            _read(args.state),
            _read(args.plan),
            _read(args.dispatch),
            _read(args.summary),
        )
        _write(args.output, state)
        _write(args.record, record)
    elif args.command == "final":
        state = _read(args.state)
        vector = final_vector(config, state)
        _write(args.output, _final_document(config, state, vector))
    else:
        _write(
            args.output,
            qualification_report(
                config,
                _read(args.state),
                _read(args.final),
                _read(args.dispatch),
                _read(args.summary),
            ),
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
