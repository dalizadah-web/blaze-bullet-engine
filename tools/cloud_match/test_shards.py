import unittest

from tools.cloud_match.shards import pair_indexes, pair_slots


class ShardTests(unittest.TestCase):
    def test_assigns_every_pair_exactly_once(self) -> None:
        assignments = [pair_indexes(400, shard, 20) for shard in range(20)]
        flattened = [pair_index for shard in assignments for pair_index in shard]
        self.assertEqual(sorted(flattened), list(range(200)))
        self.assertEqual(len(flattened), len(set(flattened)))
        self.assertTrue(all(len(shard) == 10 for shard in assignments))

    def test_rejects_invalid_pair_geometry(self) -> None:
        with self.assertRaises(ValueError):
            pair_indexes(3, 0, 1)
        with self.assertRaises(ValueError):
            pair_indexes(20, 2, 2)

    def test_assigns_each_repeat_cycle_and_slot_once_for_10k(self) -> None:
        assignments = [pair_slots(500, 10, shard, 20) for shard in range(20)]
        flattened = [slot for shard in assignments for slot in shard]
        self.assertEqual(len(flattened), 5000)
        self.assertEqual(len(set(flattened)), 5000)
        self.assertEqual(set(flattened), {
            (cycle, slot) for cycle in range(10) for slot in range(500)
        })
        self.assertTrue(all(len(shard) == 250 for shard in assignments))


if __name__ == "__main__":
    unittest.main()
