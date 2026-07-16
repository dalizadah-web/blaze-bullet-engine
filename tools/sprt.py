#!/usr/bin/env python3
"""Compute a sequential probability-ratio test from match results."""

from __future__ import annotations

import argparse
import math


def expected_score(elo: float) -> float:
    return 1.0 / (1.0 + 10.0 ** (-elo / 400.0))


def llr(wins: int, draws: int, losses: int, elo0: float, elo1: float) -> float:
    p0 = expected_score(elo0)
    p1 = expected_score(elo1)
    # Draws are assigned half a win for a conservative score-only SPRT.
    score = wins + draws * 0.5
    games = wins + draws + losses
    if games == 0 or p0 in (0.0, 1.0) or p1 in (0.0, 1.0):
        return 0.0
    return score * math.log(p1 / p0) + (games - score) * math.log((1 - p1) / (1 - p0))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wins", type=int, required=True)
    parser.add_argument("--draws", type=int, required=True)
    parser.add_argument("--losses", type=int, required=True)
    parser.add_argument("--elo0", type=float, default=0.0)
    parser.add_argument("--elo1", type=float, default=10.0)
    parser.add_argument("--alpha", type=float, default=0.05)
    parser.add_argument("--beta", type=float, default=0.05)
    args = parser.parse_args()
    if min(args.wins, args.draws, args.losses) < 0:
        parser.error("results cannot be negative")
    lower = math.log(args.beta / (1 - args.alpha))
    upper = math.log((1 - args.beta) / args.alpha)
    value = llr(args.wins, args.draws, args.losses, args.elo0, args.elo1)
    decision = "continue"
    if value <= lower:
        decision = "reject elo1"
    elif value >= upper:
        decision = "accept elo1"
    print(f"LLR={value:.4f} bounds=[{lower:.4f}, {upper:.4f}] decision={decision}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
