"""Deterministic color-pair sharding."""


def pair_indexes(total_games: int, shard_index: int, shard_count: int) -> list[int]:
    if total_games <= 0 or total_games % 2:
        raise ValueError("total_games must be a positive even number")
    if shard_count <= 0 or not 0 <= shard_index < shard_count:
        raise ValueError("invalid shard index or count")
    pair_count = total_games // 2
    return [index for index in range(pair_count) if index % shard_count == shard_index]
