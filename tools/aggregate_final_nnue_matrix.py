#!/usr/bin/env python3
"""Aggregate the final direct-NNUE benchmark lanes into JSON and Markdown."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def read_lines(path: Path) -> list[str]:
    raw = path.read_bytes()
    encoding = "utf-16" if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    return raw.decode(encoding).splitlines()


def parse_profile(path: Path, evaluator: str) -> tuple[dict, list[dict]]:
    metadata: dict | None = None
    summaries: list[dict] = []
    for line in read_lines(path):
        if line.startswith("profile_metadata="):
            metadata = json.loads(line.removeprefix("profile_metadata="))
        elif line.startswith("profile_summary="):
            summary = json.loads(line.removeprefix("profile_summary="))
            summary["evaluator"] = evaluator
            summary["limit_type"] = "nodes" if summary["node_budget"] else "depth"
            summary["limit"] = summary["node_budget"] or summary["requested_depth"]
            summaries.append(summary)
    if metadata is None or not summaries:
        raise ValueError(f"missing profile metadata or summaries in {path}")
    return metadata, summaries


def component_summary(path: Path) -> dict:
    for line in read_lines(path):
        record = json.loads(line)
        if "aggregate" in record:
            return record["aggregate"]
    raise ValueError(f"missing component aggregate in {path}")


def ratio(numerator: float, denominator: float | None) -> float | None:
    return numerator / denominator if denominator else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--avx2", type=Path, required=True)
    parser.add_argument("--scalar", type=Path, required=True)
    parser.add_argument("--classical", type=Path, required=True)
    parser.add_argument("--legacy-bridge", type=Path, required=True)
    parser.add_argument("--components", type=Path, required=True)
    parser.add_argument("--json-output", type=Path, required=True)
    parser.add_argument("--markdown-output", type=Path, required=True)
    args = parser.parse_args()

    inputs = (("avx2", args.avx2), ("scalar", args.scalar),
              ("classical", args.classical), ("legacy_bridge", args.legacy_bridge))
    metadata: dict[str, dict] = {}
    rows: list[dict] = []
    for evaluator, path in inputs:
        metadata[evaluator], summaries = parse_profile(path, evaluator)
        rows.extend(summaries)

    lookup = {(row["evaluator"], row["limit_type"], row["limit"], row["threads"]): row
              for row in rows}
    for row in rows:
        row["thread_scaling_efficiency"] = ratio(
            row["median_nps"],
            lookup.get((row["evaluator"], row["limit_type"], row["limit"], 1), {}).get("median_nps")
            * row["threads"] if row["threads"] else None,
        )
        if row["evaluator"] == "avx2":
            key = (row["limit_type"], row["limit"], row["threads"])
            row["scalar_to_avx2_speedup"] = ratio(
                row["median_nps"], lookup.get(("scalar", *key), {}).get("median_nps"))
            row["direct_to_classical_ratio"] = ratio(
                row["median_nps"], lookup.get(("classical", *key), {}).get("median_nps"))
            row["direct_to_legacy_bridge_ratio"] = ratio(
                row["median_nps"], lookup.get(("legacy_bridge", *key), {}).get("median_nps"))

    components = component_summary(args.components)
    rows.sort(key=lambda row: (row["limit_type"], row["limit"], row["threads"], row["evaluator"]))
    report = {
        "schema_version": 1,
        "lanes": metadata,
        "configurations": rows,
        "component_profile": components,
        "notes": [
            "Every search lane uses eight deterministic positions, five runs, seed 20260726, and the pinned Big network.",
            "The legacy bridge lane is commit d063114cf8de07e27da74b9c19a6dfa04677927e; it predates direct NNUE kernels.",
            "Legacy fixed-node behavior predates the current exact parallel node-limit implementation; depth results are the comparable legacy lane.",
            "Component timings are one-in-64 sampled estimates from the current AVX2 profile build at depth 8 and one thread.",
        ],
    }
    args.json_output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def number(value: float | None, digits: int = 3) -> str:
        return "-" if value is None else f"{value:.{digits}f}"

    lines = [
        "# Final NNUE Benchmark Matrix",
        "",
        "Eight deterministic positions, five runs each, seed `20260726`, Ryzen 7 7700, MinGW g++ 15.2.0, pinned `nn-c288c895ea92.nnue`.",
        "",
        "## Lane Identities",
        "",
        "| Lane | Engine SHA-256 | Backend |",
        "|---|---|---|",
    ]
    for evaluator, lane in metadata.items():
        lines.append(f"| {evaluator} | `{lane['engine_sha256']}` | {lane['backend']} |")
    lines += [
        "",
        "## Search Matrix",
        "",
        "| Evaluator | Limit | T | Median NPS | IQR NPS | Scaling | AVX2/scalar | Direct/classical | Direct/legacy |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['evaluator']} | {row['limit_type']} {row['limit']} | {row['threads']} | "
            f"{row['median_nps']:.1f} | {row['iqr_nps']:.1f} | "
            f"{number(row['thread_scaling_efficiency'])} | "
            f"{number(row.get('scalar_to_avx2_speedup'))} | "
            f"{number(row.get('direct_to_classical_ratio'))} | "
            f"{number(row.get('direct_to_legacy_bridge_ratio'))} |"
        )
    lines += [
        "",
        "## Component Profile",
        "",
        f"Depth-8 AVX2, one thread: {components['nodes']} nodes, {components['qnodes']} qnodes, "
        f"{components['qsearch_percent']:.2f}% qsearch, {components['evaluations_per_node']:.4f} evaluations/node, "
        f"{components['incremental_evaluation_percent']:.2f}% incremental evaluations.",
        "",
        "| Component | Estimated search share | Calls | Average ns |",
        "|---|---:|---:|---:|",
    ]
    ranked = sorted(components["components"].items(),
                    key=lambda item: item[1]["percent_search"], reverse=True)
    for name, value in ranked:
        lines.append(
            f"| {name} | {value['percent_search']:.3f}% | {value['calls']} | {value['average_ns']:.2f} |"
        )
    lines += ["", "## Notes", ""] + [f"- {note}" for note in report["notes"]]
    args.markdown_output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
