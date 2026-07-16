from __future__ import annotations

import unittest

from pathlib import Path
from tempfile import TemporaryDirectory

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

    def test_affine_is_composed_between_raw_calibration_and_rotation(self) -> None:
        with TemporaryDirectory() as temporary:
            path = Path(temporary) / "touch-affine.env"
            path.write_text(
                "MSYS_TOUCH_AFFINE_REVISION=4\n"
                "CH347_TOUCH_AFFINE_00=0.8\nCH347_TOUCH_AFFINE_01=0\n"
                "CH347_TOUCH_AFFINE_02=0.1\nCH347_TOUCH_AFFINE_10=0\n"
                "CH347_TOUCH_AFFINE_11=1\nCH347_TOUCH_AFFINE_12=0\n"
                "CH347_TOUCH_AFFINE_20=0\nCH347_TOUCH_AFFINE_21=0\n"
                "CH347_TOUCH_AFFINE_22=1\n",
                encoding="ascii",
            )
            self.assertEqual(
                tuple(round(value, 9) for value in ch347_transform({
                    "CH347_TOUCH_AFFINE_FILE": str(path),
                    "CH347_TOUCH_INVERT_X": "1",
                    "CH347_DISPLAY_ROTATION": "right",
                })),
                (0.0, 1.0, 0.0, 0.8, 0.0, 0.1, 0.0, 0.0, 1.0),
            )


if __name__ == "__main__":
    unittest.main()
