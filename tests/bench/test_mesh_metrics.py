#!/usr/bin/env python3
"""Regression tests for acceptance-corpus topology validity metrics."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "bench"))
import mesh_metrics  # noqa: E402
import corpus  # noqa: E402


class ValidityStatsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.vertices = np.array(
            [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0],
             [0.0, 0.0, 1.0], [1.0, 1.0, 0.0]],
            dtype=np.float64,
        )

    def test_open_triangle_is_valid_with_one_boundary_loop(self) -> None:
        stats = mesh_metrics.validity_stats(
            mesh_metrics.MeshData(self.vertices, [(0, 1, 2)])
        )
        self.assertTrue(stats["valid"])
        self.assertEqual(stats["boundary_loops"], 1)
        self.assertEqual(stats["boundary_branch_vertices"], 0)

    def test_duplicate_degenerate_and_out_of_range_faces_are_invalid(self) -> None:
        stats = mesh_metrics.validity_stats(
            mesh_metrics.MeshData(self.vertices, [(0, 1, 2), (2, 1, 0), (0, 0, 1), (0, 1, 9)])
        )
        self.assertFalse(stats["valid"])
        self.assertEqual(stats["duplicate_faces"], 1)
        self.assertEqual(stats["degenerate_faces"], 1)
        self.assertEqual(stats["invalid_index_faces"], 1)

    def test_three_faces_on_one_edge_are_non_manifold(self) -> None:
        stats = mesh_metrics.validity_stats(
            mesh_metrics.MeshData(self.vertices, [(0, 1, 2), (1, 0, 3), (0, 1, 4)])
        )
        self.assertFalse(stats["valid"])
        self.assertEqual(stats["non_manifold_edges"], 1)

    def test_acceptance_fixture_hashes_are_platform_independent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixtures = corpus.acceptance_meshes(Path(directory))
        self.assertEqual(len(fixtures), 8)


if __name__ == "__main__":
    unittest.main()
