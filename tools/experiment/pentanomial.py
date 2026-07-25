"""Pentanomial likelihood and sequential test helpers for paired games.

Each observation is a color-swapped pair.  The five categories contain the
candidate's average score per game: 1.0, 0.75, 0.5, 0.25, and 0.0.  Likelihoods
use an exponentially tilted multinomial distribution constrained to the score
expected by each Elo hypothesis.  This retains pair information instead of
collapsing the match into independent Bernoulli games.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Iterable, Literal, Sequence


Decision = Literal["accept", "reject", "continue"]
_CATEGORY_SCORES = (1.0, 0.75, 0.5, 0.25, 0.0)
_VALID_GAME_SCORES = frozenset((0.0, 0.5, 1.0))


@dataclass(frozen=True)
class Pentanomial:
    wins2: int = 0
    wins1_draw1: int = 0
    draws2: int = 0
    losses1_draw1: int = 0
    losses2: int = 0

    def __post_init__(self) -> None:
        if any(value < 0 for value in self.as_tuple()):
            raise ValueError("pentanomial counts cannot be negative")

    def as_tuple(self) -> tuple[int, int, int, int, int]:
        return (
            self.wins2,
            self.wins1_draw1,
            self.draws2,
            self.losses1_draw1,
            self.losses2,
        )

    @property
    def pairs(self) -> int:
        return sum(self.as_tuple())

    @classmethod
    def from_pair_scores(
        cls,
        pairs: Iterable[Sequence[float]],
    ) -> "Pentanomial":
        counts = [0, 0, 0, 0, 0]
        category_by_total = {2.0: 0, 1.5: 1, 1.0: 2, 0.5: 3, 0.0: 4}
        for pair in pairs:
            if len(pair) != 2:
                raise ValueError("each pair must contain two candidate scores")
            first, second = float(pair[0]), float(pair[1])
            if first not in _VALID_GAME_SCORES or second not in _VALID_GAME_SCORES:
                raise ValueError("each game score must be 0, 0.5, or 1")
            counts[category_by_total[first + second]] += 1
        return cls(*counts)


def expected_score(elo: float) -> float:
    """Return logistic expected game score for an Elo difference."""

    return 1.0 / (1.0 + 10.0 ** (-elo / 400.0))


def _logsumexp(values: Sequence[float]) -> float:
    maximum = max(values)
    return maximum + math.log(sum(math.exp(value - maximum) for value in values))


def _tilted_probabilities(
    counts: Sequence[int],
    target_score: float,
) -> tuple[float, float, float, float, float]:
    # Jeffreys smoothing keeps every category available when a short test has
    # not observed it yet.  The same empirical nuisance distribution is used
    # for both hypotheses.
    smoothed = tuple(value + 0.5 for value in counts)
    base_logs = tuple(math.log(value) for value in smoothed)

    def probabilities(lam: float) -> tuple[float, float, float, float, float]:
        logits = tuple(
            base + lam * score
            for base, score in zip(base_logs, _CATEGORY_SCORES, strict=True)
        )
        normalizer = _logsumexp(logits)
        return tuple(math.exp(value - normalizer) for value in logits)  # type: ignore[return-value]

    lower = -128.0
    upper = 128.0
    for _ in range(100):
        middle = (lower + upper) / 2.0
        trial = probabilities(middle)
        mean = sum(
            probability * score
            for probability, score in zip(trial, _CATEGORY_SCORES, strict=True)
        )
        if mean < target_score:
            lower = middle
        else:
            upper = middle
    return probabilities((lower + upper) / 2.0)


def _log_likelihood(counts: Sequence[int], target_score: float) -> float:
    probabilities = _tilted_probabilities(counts, target_score)
    return sum(
        count * math.log(probability)
        for count, probability in zip(counts, probabilities, strict=True)
        if count > 0
    )


def sprt_llr(counts: Pentanomial, elo0: float, elo1: float) -> float:
    """Return log P(data|elo1) / P(data|elo0) for paired outcomes."""

    if elo1 <= elo0:
        raise ValueError("elo1 must be greater than elo0")
    if counts.pairs == 0:
        return 0.0
    observed = counts.as_tuple()
    return _log_likelihood(observed, expected_score(elo1)) - _log_likelihood(
        observed, expected_score(elo0)
    )


def sprt_bounds(alpha: float, beta: float) -> tuple[float, float]:
    if not 0.0 < alpha < 1.0 or not 0.0 < beta < 1.0:
        raise ValueError("alpha and beta must be strictly between 0 and 1")
    return (
        math.log(beta / (1.0 - alpha)),
        math.log((1.0 - beta) / alpha),
    )


def sprt_decision(
    counts: Pentanomial,
    elo0: float,
    elo1: float,
    alpha: float = 0.05,
    beta: float = 0.05,
) -> Decision:
    lower, upper = sprt_bounds(alpha, beta)
    value = sprt_llr(counts, elo0, elo1)
    if value <= lower:
        return "reject"
    if value >= upper:
        return "accept"
    return "continue"
