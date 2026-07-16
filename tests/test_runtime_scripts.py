from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
DAEMON = ROOT / "files/x11display/scripts/ch347_dirty_usb_x11_daemon.sh"
PROVIDER = ROOT / "scripts/msys_ch347_x11_provider.sh"
DISPLAY_CONFIG = ROOT / "files/x11display/scripts/ch347_display_config.sh"
MAIN_MARKER = "# MSYS_CH347_DAEMON_MAIN"


def write_executable(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")
    path.chmod(0o755)


def process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def wait_until(predicate, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("condition did not become true before timeout")


@unittest.skipUnless(shutil.which("bash"), "Bash runtime is required")
class RuntimeScriptTests(unittest.TestCase):
    def daemon_functions(self) -> str:
        source = DAEMON.read_text(encoding="utf-8")
        self.assertIn(MAIN_MARKER, source)
        functions, _marker, _main = source.partition(MAIN_MARKER)
        return functions

    def test_display_config_parser_accepts_only_bounded_canonical_data(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            valid = root / "valid.env"
            valid.write_text(
                "DEBUG=1\nFPS=75\nXCAP_MAX_FPS=75\nXCAP_IDLE_FPS=0\n",
                encoding="ascii",
            )
            command = (
                f'. "{DISPLAY_CONFIG}"; '
                f'ch347_read_display_config "{valid}"; '
                "printf '%s/%s/%s/%s\\n' \"$CH347_CONFIG_DEBUG\" "
                '"$CH347_CONFIG_FPS" "$CH347_CONFIG_MAX_FPS" '
                '"$CH347_CONFIG_IDLE_FPS"'
            )
            accepted = subprocess.run(
                ["bash", "-c", command],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            self.assertEqual(accepted.stdout, "1/75/75/0\n")

            invalid_documents = (
                "DEBUG=true\nFPS=60\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=0\n",
                "DEBUG=0\nFPS=0\nXCAP_MAX_FPS=0\nXCAP_IDLE_FPS=0\n",
                "DEBUG=0\nFPS=60\nFPS=30\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=0\n",
                "DEBUG=0\nFPS=60\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=61\n",
                "DEBUG=0\nFPS=$(touch /tmp/no)\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=0\n",
            )
            for index, document in enumerate(invalid_documents):
                path = root / f"invalid-{index}.env"
                path.write_text(document, encoding="ascii")
                rejected = subprocess.run(
                    [
                        "bash",
                        "-c",
                        f'. "{DISPLAY_CONFIG}"; ch347_read_display_config "{path}"',
                    ],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=5,
                )
                self.assertNotEqual(rejected.returncode, 0, document)

            oversized = root / "oversized.env"
            oversized.write_bytes(b"#" * (16 * 1024 + 1))
            oversized_result = subprocess.run(
                [
                    "bash",
                    "-c",
                    f'. "{DISPLAY_CONFIG}"; ch347_read_display_config "{oversized}"',
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertNotEqual(oversized_result.returncode, 0)

            linked = root / "linked.env"
            linked.symlink_to(valid)
            linked_result = subprocess.run(
                [
                    "bash",
                    "-c",
                    f'. "{DISPLAY_CONFIG}"; ch347_read_display_config "{linked}"',
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertNotEqual(linked_result.returncode, 0)

            target = root / "written.env"
            outside = root / "outside"
            outside.write_text("unchanged\n", encoding="ascii")
            writer = root / "writer.sh"
            write_executable(
                writer,
                f'''#!/bin/bash
set -euo pipefail
. "{DISPLAY_CONFIG}"
target="{target}"
outside="{outside}"
ln -s "$outside" "$target.$$.tmp"
ch347_write_display_config "$target" 1 90 90 0
test ! -L "$target"
test "$(cat "$outside")" = unchanged
test "$(cat "$target")" = $'DEBUG=1\\nFPS=90\\nXCAP_MAX_FPS=90\\nXCAP_IDLE_FPS=0'
''',
            )
            safely_written = subprocess.run(
                ["bash", str(writer)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertEqual(safely_written.returncode, 0, safely_written.stderr)

            overlay = root / "overlay.env"
            overlay.write_text(
                "CH347_DEBUG_OVERLAY=1\n"
                "CH347_DEBUG_OVERLAY_ALPHA=128\n"
                "CH347_DEBUG_OVERLAY_SCALE=1\n"
                "CH347_DEBUG_OVERLAY_ITEMS=25\n"
                "CH347_DEBUG_OVERLAY_INTERVAL_MS=750\n",
                encoding="ascii",
            )
            overlay_command = (
                f'. "{DISPLAY_CONFIG}"; '
                f'ch347_read_debug_overlay_config "{overlay}"; '
                "printf '%s/%s/%s/%s/%s\\n' \"$CH347_CONFIG_OVERLAY_ENABLED\" "
                '"$CH347_CONFIG_OVERLAY_ALPHA" "$CH347_CONFIG_OVERLAY_SCALE" '
                '"$CH347_CONFIG_OVERLAY_ITEMS" "$CH347_CONFIG_OVERLAY_INTERVAL_MS"'
            )
            overlay_result = subprocess.run(
                ["bash", "-c", overlay_command],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertEqual(overlay_result.returncode, 0, overlay_result.stderr)
            self.assertEqual(overlay_result.stdout, "1/128/1/25/750\n")

            overlay.write_text(
                "CH347_DEBUG_OVERLAY=1\n"
                "CH347_DEBUG_OVERLAY_ALPHA=256\n"
                "CH347_DEBUG_OVERLAY_SCALE=1\n"
                "CH347_DEBUG_OVERLAY_ITEMS=7\n"
                "CH347_DEBUG_OVERLAY_INTERVAL_MS=1000\n",
                encoding="ascii",
            )
            rejected_overlay = subprocess.run(
                [
                    "bash",
                    "-c",
                    f'. "{DISPLAY_CONFIG}"; ch347_read_debug_overlay_config "{overlay}"',
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertNotEqual(rejected_overlay.returncode, 0)

            cursor = root / "cursor.env"
            cursor.write_text("CH347_CURSOR=1\n", encoding="ascii")
            cursor_command = (
                f'. "{DISPLAY_CONFIG}"; '
                f'ch347_read_cursor_config "{cursor}"; '
                'printf "%s\\n" "$CH347_CONFIG_CURSOR_ENABLED"; '
                f'ch347_write_cursor_config "{root / "cursor-written.env"}" 1 9'
            )
            cursor_result = subprocess.run(
                ["bash", "-c", cursor_command],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertEqual(cursor_result.returncode, 0, cursor_result.stderr)
            self.assertEqual(cursor_result.stdout, "1\n")
            self.assertEqual(
                (root / "cursor-written.env").read_text(encoding="ascii"),
                "MSYS_GENERATION=9\nCH347_CURSOR=1\n",
            )
            for document in (
                "CH347_CURSOR=true\n",
                "CH347_CURSOR=0\nCH347_CURSOR=1\n",
                "MSYS_GENERATION=9\nCH347_CURSOR=1\n",
                "UNKNOWN=1\n",
            ):
                cursor.write_text(document, encoding="ascii")
                rejected_cursor = subprocess.run(
                    [
                        "bash",
                        "-c",
                        f'. "{DISPLAY_CONFIG}"; '
                        f'ch347_read_cursor_config "{cursor}"',
                    ],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=5,
                )
                self.assertNotEqual(rejected_cursor.returncode, 0, document)

    def test_long_480m_wait_failure_exports_one_degraded_edge(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            state = root / "link.env"
            log = root / "live.log"
            command = self.daemon_functions() + "\nwait_ch347_bound\n"
            driver = root / "link-state-test.sh"
            write_executable(driver, command)
            run = subprocess.run(
                ["bash", str(driver)],
                env={
                    **os.environ,
                    "RUN_DIR": str(root),
                    "CH347_USB_SYS": str(root / "missing-usb"),
                    "CH347_DEVICE_NODE": str(root / "missing-device"),
                    "CH347_LINK_STATE_FILE": str(state),
                    "CH347_WAIT_SEC": "1",
                },
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertNotEqual(run.returncode, 0)
            self.assertTrue(
                state.is_file(),
                "missing degraded state; "
                f"rc={run.returncode} stdout={run.stdout!r} stderr={run.stderr!r}",
            )
            self.assertEqual(
                state.read_text(encoding="ascii"),
                "MSYS_CH347_LINK_STATE=degraded\n",
            )
            self.assertIn(
                "dirty_usb_x11_link_state state=degraded required_speed=480M",
                log.read_text(encoding="utf-8"),
            )

    def test_dimension_probe_consumes_xdpyinfo_and_checks_exact_size(self) -> None:
        source = DAEMON.read_text(encoding="utf-8")
        self.assertNotIn("awk '/dimensions:/{print $2; exit}'", source)
        self.assertIn("validate_x_root_dimensions", source)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            commands = root / "bin"
            commands.mkdir()
            write_executable(
                commands / "xdpyinfo",
                """#!/bin/sh
dimensions=${FAKE_DIMENSIONS:-320x480}
if test "$dimensions" != missing; then
    printf '  dimensions:    %s pixels (84x127 millimeters)\\n' "$dimensions"
fi
i=0
while test "$i" -lt 12000; do
    printf 'visual id: 0x%08x\\n' "$i"
    i=$((i + 1))
done
printf '  depth of root window:    24 planes\\n'
""",
            )
            run_dir = root / "run"
            environment = {
                **os.environ,
                "PATH": f"{commands}:/usr/bin:/bin",
                "PROJECT_DIR": str(ROOT / "files/x11display"),
                "RUN_DIR": str(run_dir),
                "DISPLAY_ID": ":77",
                "WIDTH": "320",
                "HEIGHT": "480",
            }
            script = self.daemon_functions() + "\ntrap - EXIT INT TERM\nvalidate_x_root_dimensions\n"
            driver = root / "dimension-test.sh"
            write_executable(driver, script)

            matched = subprocess.run(
                ["bash", str(driver)],
                env={**environment, "FAKE_DIMENSIONS": "320x480"},
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertEqual(matched.returncode, 0, matched.stderr)

            mismatched = subprocess.run(
                ["bash", str(driver)],
                env={**environment, "FAKE_DIMENSIONS": "480x480"},
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertNotEqual(mismatched.returncode, 0)
            self.assertIn(
                "X root size mismatch expected=320x480 actual=480x480",
                (run_dir / "live.log").read_text(encoding="utf-8"),
            )

    def test_transport_failures_reuse_x_but_x_loss_is_fatal(self) -> None:
        """A loose CH347 must not turn into a counterfeit X11 recovery."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            commands = root / "bin"
            commands.mkdir()
            run_dir = root / "run"
            usb = root / "usb-device"
            usb.mkdir()
            (usb / "busnum").write_text("1\n", encoding="ascii")
            (usb / "devnum").write_text("2\n", encoding="ascii")
            device_node = root / "ch34x_pis0"
            device_node.touch()

            x_launches = root / "x-launches"
            x_active = root / "x-active"
            x_dimensions = root / "x-dimensions"
            x_dimensions.write_text("320x480\n", encoding="ascii")
            sink_count = root / "sink-count"
            sink_active = root / "sink-active"
            capture_active = root / "capture-active"
            usb_reset = root / "usb-reset"

            write_executable(
                commands / "Xorg",
                """#!/bin/bash
set -e
printf '%s\n' "$$" >>"$FAKE_X_LAUNCHES"
printf '%s\n' "$$" >"$FAKE_X_ACTIVE"
trap 'exit 0' INT TERM
while :; do sleep 1; done
""",
            )
            write_executable(
                commands / "xdpyinfo",
                """#!/bin/sh
test -f "$FAKE_X_ACTIVE" || exit 1
pid=$(cat "$FAKE_X_ACTIVE")
kill -0 "$pid" 2>/dev/null || exit 1
dimensions=320x480
test ! -f "$FAKE_X_DIMENSIONS" || read -r dimensions <"$FAKE_X_DIMENSIONS"
printf 'name of display: :77\n'
printf '  dimensions:    %s pixels (84x127 millimeters)\n' "$dimensions"
printf '  depth of root window:    24 planes\n'
""",
            )
            write_executable(
                commands / "xrandr",
                """#!/bin/sh
test "${1:-}" = --size || exit 1
printf '%s\n' "$2" >"$FAKE_X_DIMENSIONS"
""",
            )
            write_executable(
                commands / "lsusb",
                """#!/bin/sh
speed=12M
test ! -f "$FAKE_USB_RESET" || speed=480M
printf '    |__ Port 1: Dev 2, If 4, Class=Vendor Specific Class, Driver=ch34x_pis, %s\\n' "$speed"
""",
            )
            arm = root / "fake-arm"
            write_executable(arm, "#!/bin/sh\nexit 0\n")
            rotation = root / "rotation.env"
            rotation.write_text(
                "CH347_DISPLAY_ROTATION=normal\n", encoding="ascii"
            )
            capture = root / "fake-capture"
            write_executable(
                capture,
                """#!/bin/bash
printf '%s\n' "$$" >"$FAKE_CAPTURE_ACTIVE"
trap 'exit 0' INT TERM
trap ':' USR1
while :; do sleep 1; done
""",
            )
            sink = root / "fake-sink"
            write_executable(
                sink,
                """#!/bin/bash
if test "${1:-}" = "--usb-reset"; then
    : >"$FAKE_USB_RESET"
    exit 0
fi
count=0
test ! -f "$FAKE_SINK_COUNT" || read -r count <"$FAKE_SINK_COUNT"
count=$((count + 1))
printf '%s\n' "$count" >"$FAKE_SINK_COUNT"
printf '%s\n' "$$" >"$FAKE_SINK_ACTIVE"
if test "$count" -eq 1; then
    # Old production sinks could report success after a late USB write error.
    # An unbounded stream ending at all must still enter recovery.
    sleep 0.02
    exit 0
fi
if test "$count" -eq 2; then
    sleep 0.02
    # Deliberately collide with the X-session fatal status.  The daemon must
    # use its explicit failure-domain flag, not classify by rc alone.
    exit 70
fi
if test "$count" -le 5; then
    sleep 0.02
    exit 75
fi
trap 'exit 76' INT TERM
trap ':' USR1
while :; do sleep 1; done
""",
            )

            environment = {
                **os.environ,
                "PATH": f"{commands}:/usr/bin:/bin",
                "PROJECT_DIR": str(ROOT / "files/x11display"),
                "RUN_DIR": str(run_dir),
                "DISPLAY": ":77",
                "DISPLAY_ID": ":77",
                "WIDTH": "320",
                "HEIGHT": "480",
                "APP": "none",
                "WM": "none",
                "CAPTURE": "xdamage",
                "XSERVER": "Xorg",
                "XORG_CONFIG": str(ROOT / "files/x11display/xorg/xorg.conf"),
                "CH347_USB_SYS": str(usb),
                "CH347_DEVICE_NODE": str(device_node),
                "CH347_IFACE_ID": "msys-test:1.4",
                "CH347_WAIT_SEC": "1",
                "CH347_ARM": str(arm),
                "CH347_SINK": str(sink),
                "XCAP_BIN": str(capture),
                "CH347_TOUCH": "0",
                "CH347_RESTART_DELAY_SEC": "0",
                "CH347_X_SESSION_PROBES": "1",
                "XCAP_NICE": "0",
                "CH347_SINK_NICE": "0",
                "XVFB_NICE": "0",
                "FAKE_X_LAUNCHES": str(x_launches),
                "FAKE_X_ACTIVE": str(x_active),
                "FAKE_X_DIMENSIONS": str(x_dimensions),
                "FAKE_SINK_COUNT": str(sink_count),
                "FAKE_SINK_ACTIVE": str(sink_active),
                "FAKE_CAPTURE_ACTIVE": str(capture_active),
                "FAKE_USB_RESET": str(usb_reset),
                "CH347_ROTATION_FILE": str(rotation),
                "MSYS_CH347_APPLIED_ROTATION_FILE": str(
                    run_dir / "rotation.applied.env"
                ),
            }
            process = subprocess.Popen(
                ["bash", str(DAEMON)],
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            spawned: set[int] = set()
            try:
                try:
                    wait_until(
                        lambda: sink_count.is_file()
                        and int(sink_count.read_text().strip()) >= 6
                        and sink_active.is_file()
                        and process_alive(int(sink_active.read_text().strip())),
                        timeout=10,
                    )
                except AssertionError:
                    process.terminate()
                    output, _ = process.communicate(timeout=3)
                    log = (run_dir / "live.log").read_text(encoding="utf-8")
                    self.fail(
                        f"transport recovery did not settle; rc={process.returncode} "
                        f"count={sink_count.read_text().strip() if sink_count.exists() else 'missing'}\n"
                        f"stdout:\n{output}\nlog:\n{log}"
                    )
                self.assertIsNone(process.poll())
                self.assertTrue(usb_reset.exists(), "12M transport was not reset")
                self.assertEqual(
                    (run_dir / "ch347-link-state.env").read_text(encoding="ascii"),
                    "MSYS_CH347_LINK_STATE=healthy\n",
                )
                x_pids = [int(value) for value in x_launches.read_text().splitlines()]
                self.assertEqual(len(x_pids), 1)
                spawned.update(x_pids)

                # A live settings change interrupts daemon wait(1), reloads
                # capture/sink, and must preserve every process in the X
                # session. Bash unsets the `wait -p` destination on the
                # signal path, which previously tripped `set -u` here.
                reload_capture = int(capture_active.read_text().strip())
                reload_sink = int(sink_active.read_text().strip())
                os.kill(process.pid, signal.SIGUSR1)
                wait_until(
                    lambda: "dirty_usb_x11_control_reload applied" in
                    (run_dir / "live.log").read_text(encoding="utf-8")
                )
                time.sleep(0.1)
                self.assertIsNone(process.poll())
                self.assertTrue(process_alive(reload_capture))
                self.assertTrue(process_alive(reload_sink))
                self.assertEqual(
                    int(capture_active.read_text().strip()), reload_capture
                )
                self.assertEqual(int(sink_active.read_text().strip()), reload_sink)
                self.assertEqual(len(x_launches.read_text().splitlines()), 1)

                rotation.write_text(
                    "CH347_DISPLAY_ROTATION=right\n", encoding="ascii"
                )
                os.kill(process.pid, signal.SIGUSR1)
                wait_until(
                    lambda: (run_dir / "rotation.applied.env").is_file()
                    and "CH347_DISPLAY_ROTATION=right" in
                    (run_dir / "rotation.applied.env").read_text(
                        encoding="ascii"
                    )
                )
                time.sleep(0.1)
                self.assertIsNone(process.poll())
                self.assertEqual(x_dimensions.read_text().strip(), "480x320")
                self.assertEqual(
                    int(capture_active.read_text().strip()), reload_capture
                )
                self.assertEqual(int(sink_active.read_text().strip()), reload_sink)
                self.assertTrue(process_alive(reload_capture))
                self.assertTrue(process_alive(reload_sink))
                self.assertEqual(len(x_launches.read_text().splitlines()), 1)

                # The registry is rewritten with current children instead of
                # accumulating dead capture/sink PIDs across unlimited retries.
                registered = [
                    int(value)
                    for value in (run_dir / "pids").read_text().splitlines()
                ]
                self.assertEqual(len(registered), len(set(registered)))
                self.assertLessEqual(len(registered), 4)

                # Capture is supervised independently from the USB sink.  A
                # capture crash must restart the two stream processes without
                # replacing Xorg or reopening X11 clients.
                old_capture = int(capture_active.read_text().strip())
                spawned.add(old_capture)
                os.kill(old_capture, signal.SIGTERM)
                wait_until(
                    lambda: int(sink_count.read_text().strip()) >= 7
                    and int(capture_active.read_text().strip()) != old_capture
                    and process_alive(int(capture_active.read_text().strip())),
                    timeout=6,
                )
                self.assertEqual(len(x_launches.read_text().splitlines()), 1)

                active_sink = int(sink_active.read_text().strip())
                spawned.add(active_sink)
                if capture_active.is_file():
                    spawned.add(int(capture_active.read_text().strip()))
                os.kill(x_pids[0], signal.SIGKILL)
                os.kill(active_sink, signal.SIGTERM)

                output, _ = process.communicate(timeout=8)
                self.assertEqual(process.returncode, 70, output)
                log = (run_dir / "live.log").read_text(encoding="utf-8")
                self.assertIn("reason=unexpected-stream-exit", log)
                self.assertIn("dirty_usb_x11_restart rc=70", log)
                self.assertGreaterEqual(log.count("dirty_usb_x11_restart rc=75"), 3)
                self.assertNotIn("dirty_usb_x11_restart_x", log)
                self.assertIn("dirty_usb_x11_x_session_lost", log)
                self.assertIn("status=x-session-lost rc=70", log)
                self.assertEqual(len(x_launches.read_text().splitlines()), 1)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                for path in (x_active, sink_active, capture_active):
                    if path.is_file():
                        try:
                            spawned.add(int(path.read_text().strip()))
                        except ValueError:
                            pass
                for pid in spawned:
                    if process_alive(pid):
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass

    def test_provider_migration_rejects_symlink_and_oversized_state_before_start(self) -> None:
        for unsafe_kind in ("symlink", "oversized"):
            with self.subTest(kind=unsafe_kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                state = root / "state"
                config_dir = state / "ch347"
                config_dir.mkdir(parents=True)
                fps = config_dir / "fps.env"
                outside = root / "outside.env"
                outside.write_text(
                    "FPS=60\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=1\n",
                    encoding="ascii",
                )
                if unsafe_kind == "symlink":
                    fps.symlink_to(outside)
                else:
                    fps.write_bytes(b"#" * (16 * 1024 + 1))

                marker = root / "start-called"
                start = root / "start.sh"
                stop = root / "stop.sh"
                write_executable(start, f"#!/bin/sh\ntouch '{marker}'\n")
                write_executable(stop, "#!/bin/sh\nexit 0\n")
                run = subprocess.run(
                    ["bash", str(PROVIDER)],
                    env={
                        **os.environ,
                        "RUN_DIR": str(root / "run"),
                        "MSYS_X11DISPLAY_ROOT": str(ROOT / "files/x11display"),
                        "CH347_START_SCRIPT": str(start),
                        "CH347_STOP_SCRIPT": str(stop),
                        "MSYS_APP_STATE_DIR": str(state),
                        "MSYS_GENERATION": "1",
                    },
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=5,
                )
                self.assertNotEqual(run.returncode, 0, run.stdout)
                self.assertFalse(marker.exists())
                self.assertEqual(
                    outside.read_text(encoding="ascii"),
                    "FPS=60\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=1\n",
                )

    def test_provider_publishes_one_failure_and_one_recovery_notice(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            commands = root / "bin"
            commands.mkdir()
            write_executable(
                commands / "xdpyinfo",
                """#!/bin/sh
printf 'name of display: :77\\n'
printf '  dimensions:    320x480 pixels (84x127 millimeters)\\n'
printf '  depth of root window:    24 planes\\n'
""",
            )
            start = root / "start.sh"
            stop = root / "stop.sh"
            write_executable(
                start,
                """#!/bin/bash
set -euo pipefail
mkdir -p "$RUN_DIR"
printf 'MSYS_CH347_LINK_STATE=checking\\n' >"$CH347_LINK_STATE_FILE"
nohup sleep 60 >/dev/null 2>&1 &
printf '%s\\n' "$!" >"$RUN_DIR/pids"
""",
            )
            write_executable(
                stop,
                """#!/bin/sh
if test -f "$RUN_DIR/pids"; then
    while IFS= read -r pid; do
        test -z "$pid" || kill "$pid" 2>/dev/null || true
    done <"$RUN_DIR/pids"
fi
rm -f "$RUN_DIR/pids"
""",
            )
            run_dir = root / "run"
            state_root = root / "state"
            (state_root / "ch347").mkdir(parents=True)
            (state_root / "ch347/fps.env").write_text(
                "DEBUG=0\nFPS=60\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=0\n",
                encoding="ascii",
            )
            runtime_dir = root / "runtime"
            runtime_dir.mkdir()
            ready = run_dir / "ready.json"
            link_state = run_dir / "ch347-link-state.env"
            supervisor, component = socket.socketpair(
                socket.AF_UNIX, socket.SOCK_SEQPACKET
            )
            supervisor.settimeout(2.0)
            process = subprocess.Popen(
                ["bash", str(PROVIDER)],
                env={
                    **os.environ,
                    "PATH": f"{commands}:/usr/bin:/bin",
                    "RUN_DIR": str(run_dir),
                    "MSYS_X11DISPLAY_ROOT": str(ROOT / "files/x11display"),
                    "CH347_START_SCRIPT": str(start),
                    "CH347_STOP_SCRIPT": str(stop),
                    "MSYS_X11_READY_FILE": str(ready),
                    "MSYS_DISPLAY_SESSION_STATE_FILE": str(
                        runtime_dir / "display-session.json"
                    ),
                    "MSYS_RUNTIME_DIR": str(runtime_dir),
                    "MSYS_APP_STATE_DIR": str(state_root),
                    "MSYS_COMPONENT_ID": (
                        "org.msys.openstick.ch347:x11-spi-touch-output"
                    ),
                    "MSYS_GENERATION": "1",
                    "MSYS_CONTROL_FD": str(component.fileno()),
                    "MSYS_PYTHON": sys.executable,
                    "MSYS_LOCALE": "zh-CN",
                    "DISPLAY": ":77",
                    "DISPLAY_ID": ":77",
                    "CH347_TOUCH": "0",
                    "MSYS_CH347_MONITOR_INTERVAL": "0.05",
                    "MSYS_CH347_NOTICE_MIN_INTERVAL_SEC": "10",
                },
                pass_fds=(component.fileno(),),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            try:
                wait_until(lambda: ready.is_file())
                link_state.write_text(
                    "MSYS_CH347_LINK_STATE=degraded\n", encoding="ascii"
                )
                failed = json.loads(supervisor.recv(65536).decode("utf-8"))
                self.assertEqual(failed["payload"]["state"], "degraded")
                self.assertIn("检查接口", failed["payload"]["message"])

                # Rewriting the same state does not create another event.
                link_state.write_text(
                    "MSYS_CH347_LINK_STATE=degraded\n", encoding="ascii"
                )
                supervisor.settimeout(0.2)
                with self.assertRaises(TimeoutError):
                    supervisor.recv(65536)

                link_state.write_text(
                    "MSYS_CH347_LINK_STATE=healthy\n", encoding="ascii"
                )
                supervisor.settimeout(2.0)
                recovered = json.loads(supervisor.recv(65536).decode("utf-8"))
                self.assertEqual(recovered["payload"]["state"], "healthy")
                self.assertIn("恢复到 480M", recovered["payload"]["message"])
                self.assertIsNone(process.poll())
            finally:
                component.close()
                supervisor.close()
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=2)
                elif process.stdout is not None:
                    process.stdout.close()

    def test_provider_handover_does_not_stop_replacement_stack(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            commands = root / "bin"
            commands.mkdir()
            write_executable(
                commands / "xdpyinfo",
                """#!/bin/sh
printf 'name of display: :77\\n'
printf '  dimensions:    320x480 pixels (84x127 millimeters)\\n'
printf '  depth of root window:    24 planes\\n'
""",
            )
            start = root / "start.sh"
            stop = root / "stop.sh"
            provider_wrapper = root / "provider-wrapper.sh"
            write_executable(
                start,
                """#!/bin/bash
set -euo pipefail
mkdir -p "$RUN_DIR"
printf 'DEBUG=%s\nFPS=%s\nXCAP_MAX_FPS=%s\nXCAP_IDLE_FPS=%s\nCH347_CURSOR=%s\n' \
    "$DEBUG" "$FPS" "$XCAP_MAX_FPS" "$XCAP_IDLE_FPS" "$CH347_CURSOR" \
    >"$RUN_DIR/config.$MSYS_GENERATION"
nohup sleep 60 >/dev/null 2>&1 &
child=$!
printf '%s\\n' "$child" >"$RUN_DIR/pids"
printf '%s\\n' "$child" >"$RUN_DIR/child.$MSYS_GENERATION"
""",
            )
            write_executable(
                stop,
                """#!/bin/bash
set -euo pipefail
printf '%s:stop\\n' "${MSYS_GENERATION:-unknown}" >>"$RUN_DIR/actions"
if test -f "$RUN_DIR/pids"; then
    while IFS= read -r pid; do
        test -z "$pid" || kill "$pid" 2>/dev/null || true
    done <"$RUN_DIR/pids"
fi
rm -f "$RUN_DIR/pids"
""",
            )

            run_dir = root / "run"
            runtime_dir = root / "runtime"
            runtime_dir.mkdir()
            state_root = root / "state"
            (state_root / "ch347").mkdir(parents=True)
            (state_root / "ch347" / "fps.env").write_text(
                "FPS=60\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=1\n",
                encoding="ascii",
            )
            ready = run_dir / "ready.json"
            state = runtime_dir / "display-session.json"
            outside_probe = root / "outside-probe"
            outside_probe.write_text("unchanged\n", encoding="ascii")
            write_executable(
                provider_wrapper,
                f'''#!/bin/bash
set -euo pipefail
mkdir -p "$RUN_DIR"
ln -s "{outside_probe}" "$RUN_DIR/msys.provider.owner.$$.tmp"
ln -s "{outside_probe}" "$MSYS_DISPLAY_SESSION_STATE_FILE.owner.$$.tmp"
exec bash "{PROVIDER}"
''',
            )
            base_environment = {
                **os.environ,
                "PATH": f"{commands}:/usr/bin:/bin",
                "RUN_DIR": str(run_dir),
                "MSYS_X11DISPLAY_ROOT": str(ROOT / "files/x11display"),
                "CH347_START_SCRIPT": str(start),
                "CH347_STOP_SCRIPT": str(stop),
                "MSYS_X11_READY_FILE": str(ready),
                "MSYS_DISPLAY_SESSION_STATE_FILE": str(state),
                "MSYS_RUNTIME_DIR": str(runtime_dir),
                "MSYS_APP_STATE_DIR": str(state_root),
                "MSYS_COMPONENT_ID": "org.msys.openstick.ch347:x11-spi-touch-output",
                "MSYS_PYTHON": sys.executable,
                "DISPLAY": ":77",
                "DISPLAY_ID": ":77",
                "CH347_TOUCH": "0",
                "MSYS_CH347_MONITOR_INTERVAL": "0.05",
                "MSYS_CH347_DISPLAY_FAIL_LIMIT": "3",
                "MSYS_CH347_MISSING_PID_LIMIT": "3",
                "MSYS_CH347_LOG_MAX_BYTES": "1024",
                "MSYS_CH347_LOG_CHECK_TICKS": "1",
            }
            processes: list[subprocess.Popen[str]] = []
            child_pids: set[int] = set()
            try:
                first = subprocess.Popen(
                    ["bash", str(provider_wrapper)],
                    env={**base_environment, "MSYS_GENERATION": "1"},
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                processes.append(first)
                wait_until(
                    lambda: ready.is_file()
                    and json.loads(ready.read_text(encoding="utf-8"))["generation"] == 1
                )
                first_child = int((run_dir / "child.1").read_text().strip())
                child_pids.add(first_child)
                self.assertTrue(process_alive(first_child))
                self.assertEqual(
                    outside_probe.read_text(encoding="ascii"),
                    "unchanged\n",
                )
                self.assertTrue((run_dir / "display-config.applied.env").is_file())
                self.assertTrue(
                    (run_dir / "display-config.applied.env")
                    .read_text(encoding="ascii")
                    .startswith("MSYS_GENERATION=1\n")
                )
                self.assertEqual(
                    (run_dir / "cursor.applied.env").read_text(encoding="ascii"),
                    "MSYS_GENERATION=1\nCH347_CURSOR=0\n",
                )
                self.assertEqual(
                    (run_dir / "config.1").read_text(encoding="ascii"),
                    "DEBUG=0\nFPS=60\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=0\n"
                    "CH347_CURSOR=0\n",
                )
                self.assertEqual(
                    (state_root / "ch347" / "fps.env").read_text(encoding="ascii"),
                    "DEBUG=0\nFPS=60\nXCAP_MAX_FPS=60\nXCAP_IDLE_FPS=0\n",
                )

                (state_root / "ch347" / "fps.env").write_text(
                    "DEBUG=1\nFPS=75\nXCAP_MAX_FPS=75\nXCAP_IDLE_FPS=0\n",
                    encoding="ascii",
                )
                (state_root / "ch347" / "cursor.env").write_text(
                    "CH347_CURSOR=1\n",
                    encoding="ascii",
                )

                second = subprocess.Popen(
                    ["bash", str(PROVIDER)],
                    env={**base_environment, "MSYS_GENERATION": "2"},
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                )
                processes.append(second)
                wait_until(
                    lambda: ready.is_file()
                    and json.loads(ready.read_text(encoding="utf-8"))["generation"] == 2,
                    timeout=10,
                )
                first_output, _ = first.communicate(timeout=5)
                self.assertEqual(first.returncode, 0, first_output)
                second_child = int((run_dir / "child.2").read_text().strip())
                child_pids.add(second_child)
                self.assertTrue(process_alive(second_child))
                self.assertTrue((run_dir / "msys.provider.owner").read_text().startswith("2:"))
                self.assertEqual(
                    (run_dir / "display-config.applied.env").read_text(encoding="ascii"),
                    "MSYS_GENERATION=2\n"
                    "DEBUG=1\n"
                    "FPS=75\n"
                    "XCAP_MAX_FPS=75\n"
                    "XCAP_IDLE_FPS=0\n",
                )
                self.assertEqual(
                    (run_dir / "cursor.applied.env").read_text(encoding="ascii"),
                    "MSYS_GENERATION=2\nCH347_CURSOR=1\n",
                )
                self.assertTrue(
                    (run_dir / "config.2")
                    .read_text(encoding="ascii")
                    .endswith("CH347_CURSOR=1\n")
                )
                live_log = run_dir / "live.log"
                live_log.write_bytes(b"x" * 2048)
                live_inode = live_log.stat().st_ino
                wait_until(lambda: live_log.stat().st_size == 0)
                self.assertEqual(live_log.stat().st_ino, live_inode)

                outside_log = root / "outside.log"
                outside_log.write_bytes(b"y" * 2048)
                live_log.unlink()
                live_log.symlink_to(outside_log)
                time.sleep(0.15)
                self.assertEqual(outside_log.read_bytes(), b"y" * 2048)
                self.assertEqual(
                    (run_dir / "actions").read_text(encoding="utf-8").splitlines(),
                    ["2:stop"],
                )

                second.send_signal(signal.SIGTERM)
                second_output, _ = second.communicate(timeout=5)
                self.assertEqual(second.returncode, 0, second_output)
                wait_until(lambda: not process_alive(second_child))
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            process.kill()
                for pid in child_pids:
                    if process_alive(pid):
                        os.kill(pid, signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()
