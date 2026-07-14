from __future__ import annotations

import json
import re
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def elf_machine(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b"\x7fELF":
        raise ValueError(f"not an ELF file: {path}")
    if data[4] != 2 or data[5] != 1:
        raise ValueError(f"ELF must be 64-bit little-endian: {path}")
    return struct.unpack_from("<H", data, 18)[0]


class OpenStickPackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))

    def test_identity_version_and_display_role_are_stable(self) -> None:
        package = self.manifest["package"]
        component = self.manifest["components"][0]
        project = (ROOT / "pyproject.toml").read_text(encoding="utf-8")
        version = re.search(r'(?m)^version\s*=\s*"([^"]+)"', project).group(1)
        self.assertEqual(package["id"], "org.msys.openstick.ch347")
        self.assertEqual(package["version"], version)
        self.assertEqual(component["id"], "x11-spi-touch-output")
        self.assertEqual(component["readiness"]["mode"], "x11-display")
        self.assertEqual(component["env"]["DISPLAY_ID"], ":24")
        self.assertEqual(component["env"]["DEBUG"], "0")
        self.assertEqual(component["env"]["XCAP_IDLE_FPS"], "0")
        self.assertEqual(component["env"]["CH347_MAX_RECTS"], "1")
        display_output = next(
            item for item in component["provides"]
            if item.get("role") == "display-output"
        )
        self.assertEqual(
            display_output,
            {
                "role": "display-output",
                "exclusive": True,
                "priority": 100,
                "x-msys-contract": {
                    "id": "org.msys.role.display-output.v1",
                    "version": "1.0.0",
                },
            },
        )

    def test_every_runtime_entry_is_inside_the_package(self) -> None:
        component = self.manifest["components"][0]
        package_entry = component["exec"][1].removeprefix("@package/")
        self.assertTrue((ROOT / package_entry).is_file())
        required = (
            "scripts/msys_display_session_state.py",
            "msys_x11_session/display_session.py",
            "files/x11display/scripts/start_ch347_dirty_usb_x11.sh",
            "files/x11display/scripts/ch347_display_config.sh",
            "files/x11display/scripts/stop_ch347_dirty_usb_x11.sh",
            "files/x11display/scripts/ch347_dirty_usb_x11_daemon.sh",
            "files/x11display/ch347/libch347spi.so",
            "files/x11display/ch347/fps.env",
            "files/x11display/ch347/touch_calibration.env",
            "files/x11display/ch347/rotation.env",
            "files/x11display/xorg/xorg.conf",
        )
        for relative in required:
            with self.subTest(path=relative):
                self.assertTrue((ROOT / relative).is_file())

    def test_shipped_binaries_are_aarch64_not_workstation_builds(self) -> None:
        for name in (
            "ch347_dirty_usb_sink",
            "ch347_st7796_test",
            "xdamage_shm_capture",
        ):
            with self.subTest(binary=name):
                self.assertEqual(
                    elf_machine(ROOT / "files/x11display/bin" / name),
                    183,
                )
        self.assertEqual(
            elf_machine(ROOT / "files/x11display/ch347/libch347spi.so"),
            183,
        )

    def test_provider_prefers_package_root_and_x11display_is_relocatable(self) -> None:
        provider = (ROOT / "scripts/msys_ch347_x11_provider.sh").read_text(
            encoding="utf-8"
        )
        start = (
            ROOT / "files/x11display/scripts/start_ch347_dirty_usb_x11.sh"
        ).read_text(encoding="utf-8")
        daemon = (
            ROOT / "files/x11display/scripts/ch347_dirty_usb_x11_daemon.sh"
        ).read_text(encoding="utf-8")
        self.assertIn("MSYS_PACKAGE_ROOT/files/x11display", provider)
        self.assertIn("MSYS_APP_STATE_DIR", provider)
        self.assertIn('export CH347_FPS_FILE="$target"', provider)
        self.assertIn('export CH347_TOUCH_CAL_FILE="$target"', provider)
        self.assertIn('export CH347_ROTATION_FILE="$target"', provider)
        self.assertIn("$X11DISPLAY_ROOT/scripts/start_ch347_dirty_usb_x11.sh", provider)
        self.assertIn('PROJECT_DIR="$(cd "$SCRIPT_DIR/.."', start)
        self.assertIn('PROJECT_DIR="${PROJECT_DIR:-$(cd "$SCRIPT_DIR/.."', daemon)
        self.assertIn('export CH347_STACK_OWNER_FILE="$OWNER_FILE"', provider)
        self.assertIn('export CH347_STACK_OWNER_TOKEN="$OWNER_TOKEN"', provider)
        self.assertIn("migrate_legacy_idle_capture_default", provider)
        self.assertIn("PUBLISHED_DISPLAY_SIGNATURE", provider)
        self.assertIn("display_signature()", provider)
        self.assertIn(
            'MONITOR_INTERVAL="${MSYS_CH347_MONITOR_INTERVAL:-0.5}"',
            provider,
        )
        self.assertIn(
            'DISPLAY_FAIL_LIMIT="${MSYS_CH347_DISPLAY_FAIL_LIMIT:-3}"',
            provider,
        )
        self.assertIn(
            'MISSING_PID_LIMIT="${MSYS_CH347_MISSING_PID_LIMIT:-3}"',
            provider,
        )
        self.assertIn('sleep "$MONITOR_INTERVAL"', provider)
        self.assertIn("load_display_rotation", provider)
        self.assertIn('XCAP_ROTATION="$DISPLAY_ROTATION"', daemon)
        self.assertIn('CH347_DISPLAY_ROTATION="$DISPLAY_ROTATION"', daemon)
        self.assertNotIn("prepare_x11_core_font", provider)
        self.assertNotIn("fc-match", provider)
        self.assertNotIn("ttc_extract", provider)
        self.assertIn('STACK_OWNER_FILE="${CH347_STACK_OWNER_FILE:-}"', daemon)
        self.assertIn('STACK_OWNER_TOKEN="${CH347_STACK_OWNER_TOKEN:-}"', daemon)
        self.assertIn('XCAP_IDLE_FPS=0', (ROOT / "files/x11display/ch347/fps.env").read_text(encoding="utf-8"))
        self.assertIn('DEBUG=0', (ROOT / "files/x11display/ch347/fps.env").read_text(encoding="utf-8"))
        self.assertIn('CH347_MAX_RECTS=1', (ROOT / "files/x11display/ch347/ch347_best_params.env").read_text(encoding="utf-8"))
        self.assertIn('CH347_RESTART_MAX=0', (ROOT / "files/x11display/ch347/ch347_best_params.env").read_text(encoding="utf-8"))
        self.assertIn('CH347_MAX_RECTS="${CH347_MAX_RECTS:-1}"', start)
        self.assertIn('CH347_MAX_RECTS="${CH347_MAX_RECTS:-1}"', daemon)
        self.assertIn('CH347_RESTART_MAX="${CH347_RESTART_MAX:-0}"', start)
        self.assertIn('CH347_RESTART_MAX="${CH347_RESTART_MAX:-0}"', daemon)
        self.assertIn('status=x-session-lost', daemon)
        self.assertNotIn('dirty_usb_x11_restart_x', daemon)
        self.assertIn("ch347_read_display_config", provider)
        self.assertIn("publish_applied_display_config", provider)
        self.assertIn("if owns_stack; then", daemon)
        self.assertIn("shared state preserved", daemon)

    def test_rejected_bitmap_font_experiment_is_not_in_the_maf(self) -> None:
        ignored = (ROOT / ".msys-packageignore").read_text(encoding="utf-8").splitlines()
        self.assertIn("files/x11display/fonts/", ignored)
        provider = (ROOT / "scripts/msys_ch347_x11_provider.sh").read_text(encoding="utf-8")
        daemon = (ROOT / "files/x11display/scripts/ch347_dirty_usb_x11_daemon.sh").read_text(encoding="utf-8")
        self.assertNotIn("X11_CORE_FONT", provider)
        self.assertNotIn("X11_CORE_FONT", daemon)


if __name__ == "__main__":
    unittest.main()
