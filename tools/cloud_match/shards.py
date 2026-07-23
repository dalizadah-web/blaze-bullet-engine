"""Deterministic color-pair sharding."""


def pair_indexes(total_games: int, shard_index: int, shard_count: int) -> list[int]:
    if total_games <= 0 or total_games % 2:
        raise ValueError("total_games must be a positive even number")
    if shard_count <= 0 or not 0 <= shard_index < shard_count:
        raise ValueError("invalid shard index or count")
    pair_count = total_games // 2
    return [index for index in range(pair_count) if index % shard_count == shard_index]


def pair_slots(
    opening_suite_positions: int,
    opening_repeats: int,
    shard_index: int,
    shard_count: int,
) -> list[tuple[int, int]]:
    """Return each (cycle, suite-slot) assigned to one shard exactly once."""
    if (
        not isinstance(opening_suite_positions, int)
        or isinstance(opening_suite_positions, bool)
        or opening_suite_positions <= 0
    ):
        raise ValueError("opening_suite_positions must be a positive integer")
    if (
        not isinstance(opening_repeats, int)
        or isinstance(opening_repeats, bool)
        or opening_repeats <= 0
    ):
        raise ValueError("opening_repeats must be a positive integer")
    indexes = pair_indexes(
        opening_suite_positions * opening_repeats * 2,
        shard_index,
        shard_count,
    )
    return [
        (index // opening_suite_positions, index % opening_suite_positions)
        for index in indexes
    ]


def game_ids_for_slots(
    experiment_id: str,
    slots: list[tuple[int, int]],
    *,
    include_cycle: bool,
) -> list[str]:
    """Build stable color-paired IDs; cycle-qualified IDs prevent repeat collisions."""
    result: list[str] = []
    for cycle, slot in slots:
        if include_cycle:
            prefix = f"{experiment_id}-c{cycle:04d}-p{slot:06d}"
        else:
            prefix = f"{experiment_id}-p{slot:06d}"
        result.extend((f"{prefix}-w", f"{prefix}-b"))
    return result
