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
        self.assertEqual(package["version"], "0.1.27")
        self.assertEqual(package["version"], version)
        self.assertEqual(component["id"], "x11-spi-touch-output")
        self.assertEqual(component["readiness"]["mode"], "x11-display")
        self.assertEqual(component["env"]["DISPLAY_ID"], ":24")
        self.assertEqual(component["env"]["DEBUG"], "0")
        self.assertEqual(component["env"]["CH347_DEBUG_OVERLAY"], "0")
        self.assertEqual(
            component["env"]["CH347_DEBUG_OVERLAY_ITEMS"],
            "fps,dirty,bytes,cpu",
        )
        self.assertEqual(component["env"]["XCAP_IDLE_FPS"], "0")
        self.assertEqual(component["env"]["CH347_MAX_RECTS"], "1")
        self.assertIn(
            "mipc.event:publish:msys.role.notification-presenter",
            component["permissions"],
        )
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
            "scripts/msys_ch347_link_notice.py",
            "msys_x11_session/display_session.py",
            "files/x11display/scripts/start_ch347_dirty_usb_x11.sh",
            "files/x11display/scripts/ch347_display_config.sh",
            "files/x11display/scripts/stop_ch347_dirty_usb_x11.sh",
            "files/x11display/scripts/ch347_dirty_usb_x11_daemon.sh",
            "files/x11display/ch347/libch347spi.so",
            "files/x11display/ch347/fps.env",
            "files/x11display/ch347/debug_overlay.env",
            "files/x11display/ch347/cursor.env",
            "files/x11display/ch347/touch_calibration.env",
            "files/x11display/ch347/rotation.env",
            "files/x11display/src/ch347_dirty_usb_sink.c",
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

    def test_sink_exposes_non_intrusive_refresh_counters(self) -> None:
        sink = (ROOT / "files/x11display/bin/ch347_dirty_usb_sink").read_bytes()
        for field in (
            b"dirty_stats frame=",
            b"zero_damage=",
            b"full_refreshes=",
            b"large_refreshes=",
            b"last_sent_pixels=",
        ):
            with self.subTest(field=field):
                self.assertIn(field, sink)
        self.assertIn(b"CH347_DEBUG_OVERLAY", sink)
        self.assertIn(b"SINK RSS:", sink)

    def test_capture_does_not_advertise_latest_frame_coalescing(self) -> None:
        capture = (
            ROOT / "files/x11display/bin/xdamage_shm_capture"
        ).read_bytes()
        self.assertNotIn(b"mailbox_policy=latest", capture)

    def test_single_bbox_diagnostics_do_not_claim_the_40_percent_fallback_is_active(self) -> None:
        sink = (ROOT / "files/x11display/bin/ch347_dirty_usb_sink").read_bytes()
        daemon = (
            ROOT / "files/x11display/scripts/ch347_dirty_usb_x11_daemon.sh"
        ).read_text(encoding="utf-8")

        self.assertIn(b"full_pct=inactive(single-bbox)", sink)
        self.assertIn('CH347_FULL_AREA_POLICY="inactive-single-bbox"', daemon)

        source = (
            ROOT / "files/x11display/src/ch347_dirty_usb_sink.c"
        ).read_text(encoding="utf-8")
        self.assertIn(
            'unsigned int max_rects = env_u32("CH347_MAX_RECTS", 1);',
            source,
        )
        self.assertIn("if (max_rects == 1 && !stale_ms)", source)
        self.assertIn("full_pct=inactive(single-bbox)", source)

    def test_cpu_overlay_source_and_six_item_contract_are_bundled(self) -> None:
        source = (
            ROOT / "files/x11display/src/ch347_dirty_usb_sink.c"
        ).read_text(encoding="utf-8")
        provider = (
            ROOT / "scripts/msys_ch347_x11_provider.sh"
        ).read_text(encoding="utf-8")
        overlay = (
            ROOT / "files/x11display/ch347/debug_overlay.env"
        ).read_text(encoding="ascii")
        defaults = (
            ROOT / "files/x11display/ch347/ch347_best_params.env"
        ).read_text(encoding="ascii")

        self.assertIn("#define DEBUG_OVERLAY_CPU (1u << 5)", source)
        self.assertIn("DEBUG_OVERLAY_ALL_ITEMS ((1u << 6) - 1u)", source)
        self.assertIn('overlay->cpu_stat_path = "/proc/stat";', source)
        self.assertIn('"CPU:%.1f%%"', source)
        self.assertIn(
            '"CH347_DEBUG_OVERLAY_ITEMS", 1, 63,',
            source,
        )
        self.assertIn("memory cpu; do", provider)
        self.assertIn("cpu) bit=32", provider)
        self.assertIn("CH347_DEBUG_OVERLAY_ITEMS=39", overlay)
        self.assertIn(
            "CH347_DEBUG_OVERLAY_ITEMS=fps,dirty,bytes,cpu", defaults
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
        self.assertIn('export CH347_DEBUG_OVERLAY_FILE="$target"', provider)
        self.assertIn('export CH347_CURSOR_FILE="$target"', provider)
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
        self.assertIn('CH347_CURSOR=0', (ROOT / "files/x11display/ch347/ch347_best_params.env").read_text(encoding="utf-8"))
        self.assertIn('CH347_MAX_RECTS="${CH347_MAX_RECTS:-1}"', start)
        self.assertIn('CH347_MAX_RECTS="${CH347_MAX_RECTS:-1}"', daemon)
        self.assertIn('CH347_CURSOR="${CH347_CURSOR:-0}"', start)
        self.assertIn('CH347_CURSOR="${CH347_CURSOR:-0}"', daemon)
        self.assertIn('CH347_RESTART_MAX="${CH347_RESTART_MAX:-0}"', start)
        self.assertIn('CH347_RESTART_MAX="${CH347_RESTART_MAX:-0}"', daemon)
        self.assertIn('status=x-session-lost', daemon)
        self.assertNotIn('dirty_usb_x11_restart_x', daemon)
        self.assertIn("ch347_read_display_config", provider)
        self.assertIn("publish_applied_display_config", provider)
        self.assertIn("ch347_write_debug_overlay_config", provider)
        self.assertIn("ch347_write_cursor_config", provider)
        self.assertIn("ch347_write_rotation_config", provider)
        self.assertIn("load_cursor_config", provider)
        self.assertIn("trap request_runtime_reload USR1", provider)
        self.assertIn('kill -USR1 "$daemon_pid"', provider)
        self.assertIn("trap request_runtime_reload USR1", daemon)
        self.assertIn('xrandr --size "${new_width}x${new_height}"', daemon)
        self.assertIn('kill -USR1 "$STREAM_CAP_PID"', daemon)
        self.assertIn('kill -USR1 "$STREAM_SINK_PID"', daemon)
        self.assertIn('if [ -n "${finished_pid:-}" ]', daemon)
        self.assertIn("pause_capture_for_rotation", daemon)
        self.assertIn("resume_capture_after_rotation 1", daemon)
        self.assertIn('sleep "$MONITOR_INTERVAL" || true', provider)
        self.assertNotIn("stop_x_stack\n    apply_runtime_config", daemon)
        self.assertIn('CH347_DEBUG_OVERLAY="${CH347_DEBUG_OVERLAY:-0}"', start)
        self.assertIn('CH347_DEBUG_OVERLAY="${CH347_DEBUG_OVERLAY:-0}"', daemon)
        self.assertIn("if owns_stack; then", daemon)
        self.assertIn("shared state preserved", daemon)
        self.assertIn("publish_ch347_link_state degraded", daemon)
        self.assertIn("observe_ch347_link_notice", provider)

    def test_rejected_bitmap_font_experiment_is_not_in_the_maf(self) -> None:
        ignored = (ROOT / ".msys-packageignore").read_text(encoding="utf-8").splitlines()
        self.assertIn("files/x11display/fonts/", ignored)
        provider = (ROOT / "scripts/msys_ch347_x11_provider.sh").read_text(encoding="utf-8")
        daemon = (ROOT / "files/x11display/scripts/ch347_dirty_usb_x11_daemon.sh").read_text(encoding="utf-8")
        self.assertNotIn("X11_CORE_FONT", provider)
        self.assertNotIn("X11_CORE_FONT", daemon)


if __name__ == "__main__":
    unittest.main()
