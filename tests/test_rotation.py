from __future__ import annotations

import unittest

from msys_x11_session.display_session import DisplaySessionError, ch347_transform


class Ch347RotationTransformTests(unittest.TestCase):
    def test_physical_to_logical_rotation_is_composed_after_calibration(self) -> None:
        self.assertEqual(
            ch347_transform({"CH347_DISPLAY_ROTATION": "right"}),
            (0.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 1.0),
        )
        self.assertEqual(
            ch347_transform({"CH347_DISPLAY_ROTATION": "left"}),
            (0.0, -1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0),
        )
        self.assertEqual(
            ch347_transform({
                "CH347_DISPLAY_ROTATION": "inverted",
                "CH347_TOUCH_INVERT_X": "1",
            }),
            (1.0, 0.0, 0.0, 0.0, -1.0, 1.0, 0.0, 0.0, 1.0),
        )

    def test_unknown_rotation_is_rejected(self) -> None:
        with self.assertRaises(DisplaySessionError):
            ch347_transform({"CH347_DISPLAY_ROTATION": "diagonal"})


if __name__ == "__main__":
    unittest.main()
