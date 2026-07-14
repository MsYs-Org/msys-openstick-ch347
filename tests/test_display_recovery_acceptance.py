from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
from contextlib import redirect_stdout


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/display_recovery_acceptance.py"
SPEC = importlib.util.spec_from_file_location("display_recovery_acceptance", TOOL)
assert SPEC is not None and SPEC.loader is not None
acceptance = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = acceptance
SPEC.loader.exec_module(acceptance)


def identity(pid: int, start: int | None = None):
    return acceptance.ProcessIdentity(pid, start if start is not None else pid * 10)


def snapshot(
    *,
    xorg: int,
    sink: int,
    capture: int,
    daemon: int,
    provider: int,
    shell: int,
    app: int | None,
):
    components = {
        acceptance.DISPLAY_PROVIDER: identity(provider),
        "org.msys.shell.native:shell": identity(shell),
    }
    manual = {}
    if app is not None:
        components["org.msys.apps:calculator"] = identity(app)
        manual["org.msys.apps:calculator"] = identity(app)
    return {
        "pipeline": {
            "xorg": [identity(xorg)],
            "sink": [identity(sink)],
            "capture": [identity(capture)],
            "daemon": [identity(daemon)],
            "other": [],
        },
        "components": components,
        "component_states": {
            acceptance.DISPLAY_PROVIDER: "ready",
            "org.msys.shell.native:shell": "ready",
            "org.msys.apps:calculator": "ready" if app is not None else "declared",
        },
        "system_ui": {"org.msys.shell.native:shell": identity(shell)},
        "manual_applications": manual,
    }


class DisplayRecoveryAcceptanceTests(unittest.TestCase):
    def test_default_cli_is_strictly_read_only(self) -> None:
        args = acceptance.build_parser().parse_args([])
        self.assertIsNone(args.inject)
        self.assertEqual(args.runtime_dir, "/tmp/msys-main")
        self.assertEqual(args.run_dir, "/tmp/ch347_dirty_usb_x11")

        baseline = snapshot(
            xorg=100, sink=101, capture=102, daemon=103,
            provider=104, shell=105, app=None,
        )
        output = io.StringIO()
        with (
            mock.patch.object(acceptance.AcceptanceHarness, "snapshot", return_value=baseline),
            mock.patch.object(acceptance.os, "kill") as kill,
            redirect_stdout(output),
        ):
            status = acceptance.main([])
        self.assertEqual(status, 0)
        kill.assert_not_called()
        document = acceptance.json.loads(output.getvalue())
        self.assertEqual(document["mode"], "read-only")
        self.assertTrue(document["ok"])

    def test_parse_stat_handles_spaces_inside_comm(self) -> None:
        # fields 4..22; starttime is the last value in this small fixture.
        numeric = [1, 123, 123] + [0] * 15 + [987654]
        document = "123 (Xorg display :24) S " + " ".join(map(str, numeric))
        self.assertEqual(acceptance.parse_stat(document), (123, 123, 123, 987654))

    def test_process_scan_and_pipeline_classification_use_pid_registry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            commands = {
                10: ("bash", "bash /x/ch347_dirty_usb_x11_daemon.sh", ""),
                11: ("Xorg", "Xorg :24 -config /x/xorg.conf", ""),
                12: ("xdamage_shm_", "/x/xdamage_shm_capture :24", ""),
                13: ("ch347_dirty_us", "/x/ch347_dirty_usb_sink 24 0", ""),
                99: ("unrelated", "/tmp/ch347_dirty_usb_sink", ""),
            }
            for pid, (comm, cmdline, component) in commands.items():
                process = root / str(pid)
                process.mkdir()
                values = [1, pid, pid] + [0] * 15 + [pid * 100]
                (process / "stat").write_text(
                    f"{pid} ({comm}) S " + " ".join(map(str, values)),
                    encoding="utf-8",
                )
                (process / "comm").write_text(comm + "\n", encoding="utf-8")
                (process / "cmdline").write_bytes(cmdline.replace(" ", "\0").encode() + b"\0")
                environment = f"MSYS_COMPONENT_ID={component}\0" if component else ""
                (process / "environ").write_bytes(environment.encode())
            pid_file = root / "pids"
            pid_file.write_text("10\n11\n12\n13\n", encoding="ascii")
            groups = acceptance.classify_pipeline_processes(
                acceptance.scan_processes(root), pid_file
            )
            self.assertEqual([item["pid"] for item in groups["daemon"]], [10])
            self.assertEqual([item["pid"] for item in groups["xorg"]], [11])
            self.assertEqual([item["pid"] for item in groups["capture"]], [12])
            self.assertEqual([item["pid"] for item in groups["sink"]], [13])

    def test_component_sets_only_select_visual_manual_apps(self) -> None:
        primaries = {
            "shell": identity(1),
            "calculator": identity(2),
            "installer": identity(3),
        }
        components = [
            {
                "id": "shell",
                "lifecycle": "background",
                "windowing": {"system": "x11", "display": "inherit"},
            },
            {
                "id": "calculator",
                "lifecycle": "manual",
                "windowing": {"system": "x11", "display": "inherit"},
            },
            {"id": "installer", "lifecycle": "manual", "windowing": {}},
        ]
        system_ui, manual = acceptance.component_sets(components, primaries)
        self.assertEqual(system_ui, {"shell": identity(1)})
        self.assertEqual(manual, {"calculator": identity(2)})

    def test_sink_acceptance_requires_stream_rebuild_and_session_preservation(self) -> None:
        before = snapshot(
            xorg=100, sink=101, capture=102, daemon=103,
            provider=104, shell=105, app=106,
        )
        after = snapshot(
            xorg=100, sink=201, capture=202, daemon=103,
            provider=104, shell=105, app=106,
        )
        checks = acceptance.evaluate_sink(
            before, after, "dirty_usb_x11_restart rc=1 count=0\n"
        )
        self.assertTrue(all(item["ok"] for item in checks), checks)

        broken = snapshot(
            xorg=300, sink=201, capture=202, daemon=303,
            provider=304, shell=305, app=306,
        )
        failed = {item["name"]: item["ok"] for item in acceptance.evaluate_sink(before, broken, "")}
        self.assertFalse(failed["xorg_preserved"])
        self.assertFalse(failed["manual_apps_preserved"])

    def test_xorg_acceptance_requires_no_manual_reopen_log_and_notification(self) -> None:
        before = snapshot(
            xorg=100, sink=101, capture=102, daemon=103,
            provider=104, shell=105, app=106,
        )
        after = snapshot(
            xorg=200, sink=201, capture=202, daemon=203,
            provider=204, shell=205, app=None,
        )
        log = (
            "msysd: display output fault fault=display-session-lost "
            "provider=org.msys.openstick.ch347:x11-spi-touch-output gen=1 display=:24\n"
            "msysd: display output recovered fault=display-session-lost "
            "provider=org.msys.openstick.ch347:x11-spi-touch-output "
            "gen=2 applications_reopened=false\n"
        )
        notification = {"kind": "visible-x11-window", "window": "0x42"}
        checks = acceptance.evaluate_xorg(before, after, log, notification)
        self.assertTrue(all(item["ok"] for item in checks), checks)

        reopened = snapshot(
            xorg=200, sink=201, capture=202, daemon=203,
            provider=204, shell=205, app=306,
        )
        failures = {
            item["name"]: item["ok"]
            for item in acceptance.evaluate_xorg(before, reopened, log, notification)
        }
        self.assertFalse(failures["manual_apps_not_reopened"])

        no_notice = {
            item["name"]: item["ok"]
            for item in acceptance.evaluate_xorg(before, after, log, None)
        }
        self.assertFalse(no_notice["notification_visible_after_recovery"])


if __name__ == "__main__":
    unittest.main()
