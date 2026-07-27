#!/usr/bin/env python3

import unittest
import sys
from argparse import Namespace
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from auto_crosswalk_roi_mask2former import (
    LightCandidate,
    RoiToolError,
    board_command,
    build_label_mask,
    build_light_core_mask,
    merge_nearby_boxes,
    refine_main_light,
    select_main_light,
    traffic_roi_config_text,
)


def candidate(index: int, score: float) -> LightCandidate:
    return LightCandidate(
        index=index,
        pixel_box=(10 * index, 10, 10 * index + 8, 24),
        normalized_box=(0.1 * index, 0.1, 0.1 * index + 0.05, 0.2),
        support_frames=10,
        persistence=0.5,
        area_ratio=0.001,
        crosswalk_distance=0.2,
        score=score,
    )


class TrafficLightCalibrationTest(unittest.TestCase):
    def test_label_masks_use_direct_ids_without_cross_class_pixels(self) -> None:
        labels = np.asarray([[8, 23, 48], [0, 48, 8]], dtype=np.int16)
        scratch = np.empty(labels.shape, dtype=np.bool_)

        crosswalk = build_label_mask(labels, [8, 23], scratch, np)
        traffic_light = build_label_mask(labels, [48], scratch, np)

        np.testing.assert_array_equal(
            crosswalk,
            np.asarray([[255, 255, 0], [0, 0, 255]], dtype=np.uint8),
        )
        np.testing.assert_array_equal(
            traffic_light,
            np.asarray([[0, 0, 255], [0, 255, 0]], dtype=np.uint8),
        )

    def test_nearby_components_merge_but_separate_lights_remain(self) -> None:
        merged = merge_nearby_boxes(
            [(10, 10, 20, 20), (22, 12, 30, 24), (80, 10, 90, 20)],
            gap=3,
        )
        self.assertEqual(merged, [(10, 10, 30, 24), (80, 10, 90, 20)])

    def test_core_mask_keeps_bright_red_and_green_but_rejects_blue_and_dark(self) -> None:
        image = np.asarray(
            [[[0, 0, 200], [0, 200, 0], [200, 0, 0], [0, 0, 100]]],
            dtype=np.uint8,
        )
        args = Namespace(
            light_core_min_value=160,
            light_core_min_saturation=100,
        )

        core = build_light_core_mask(
            image, np.full((1, 4), 255, dtype=np.uint8), args, np
        )

        np.testing.assert_array_equal(
            core, np.asarray([[True, True, False, False]])
        )

    def test_main_light_refinement_merges_red_green_core_not_nearby_vehicle_light(
        self,
    ) -> None:
        semantic_candidate = LightCandidate(
            index=0,
            pixel_box=(20, 10, 160, 180),
            normalized_box=(0.1, 0.05, 0.8, 0.9),
            support_frames=20,
            persistence=1.0,
            area_ratio=0.01,
            crosswalk_distance=0.2,
            score=0.9,
        )
        core_votes = np.zeros((200, 200), dtype=np.uint16)
        core_votes[30:50, 40:50] = 1
        core_votes[52:70, 40:50] = 1
        core_votes[30:44, 100:110] = 1
        args = Namespace(light_close_ratio=0.01, light_padding_ratio=0.01)

        selection = refine_main_light(
            semantic_candidate, core_votes, args, cv2, np
        )

        self.assertTrue(selection.core_refined)
        self.assertEqual(selection.pixel_box, (38, 28, 52, 72))
        self.assertEqual(selection.core_pixels, 380)

    def test_main_light_uses_score_and_allows_explicit_override(self) -> None:
        candidates = [candidate(0, 0.7), candidate(1, 0.9)]
        self.assertEqual(select_main_light(candidates, None).index, 1)
        self.assertEqual(select_main_light(candidates, 0).index, 0)
        with self.assertRaises(RoiToolError):
            select_main_light(candidates, 2)

    def test_board_command_contains_both_fixed_regions(self) -> None:
        args = Namespace(
            demo="./demo",
            board_model="./model.rknn",
            interval=2,
        )
        command = board_command(
            args,
            "0.1,0.6;0.9,0.6;0.9,0.9",
            "0.5,0.2,0.6,0.4",
        )
        self.assertIn('--roi "0.1,0.6;0.9,0.6;0.9,0.9"', command)
        self.assertIn('--light-roi "0.5,0.2,0.6,0.4"', command)

    def test_roi_config_contains_both_fixed_regions(self) -> None:
        config = traffic_roi_config_text(
            "0.1,0.6;0.9,0.6;0.9,0.9",
            "0.5,0.2,0.6,0.4",
        )
        self.assertIn('roi="0.1,0.6;0.9,0.6;0.9,0.9"\n', config)
        self.assertIn('light_roi="0.5,0.2,0.6,0.4"\n', config)


if __name__ == "__main__":
    unittest.main()
