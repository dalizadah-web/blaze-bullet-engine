import unittest

from tools.cloud_match.shards import pair_indexes


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


if __name__ == "__main__":
    unittest.main()
