#!/usr/bin/env python3
"""One-shot, local acceptance test for the two-level CH347 recovery policy.

The default mode is deliberately read-only.  Fault injection happens only
when ``--inject`` is supplied, and then only as root.  This tool runs on the
target itself; a developer needs one SSH command rather than a sequence of
fragile remote PID lookups and kills.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time
from typing import Any, Callable, Iterable


SCHEMA = "msys.display-recovery-acceptance.v1"
DISPLAY_PROVIDER = "org.msys.openstick.ch347:x11-spi-touch-output"
MAX_PACKET = 256 * 1024
RECOVERY_LINE = re.compile(
    r"msysd: display output recovered .*applications_reopened=false"
)
FAULT_LINE = re.compile(
    r"msysd: display output fault fault=display-session-lost"
)
NOTIFICATION_TITLE = "MSYS Notifications"


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    start_ticks: int

    def as_dict(self) -> dict[str, int]:
        return {"pid": self.pid, "start_ticks": self.start_ticks}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def read_limited(path: Path, limit: int) -> bytes:
    with path.open("rb") as stream:
        return stream.read(limit + 1)[:limit]


def parse_stat(data: str) -> tuple[int, int, int, int]:
    """Return pid, pgrp, session and start ticks from Linux /proc/PID/stat."""

    opening = data.find("(")
    closing = data.rfind(")")
    if opening <= 0 or closing <= opening:
        raise ValueError("malformed process stat")
    pid = int(data[:opening].strip())
    fields = data[closing + 1 :].split()
    if len(fields) < 20:
        raise ValueError("short process stat")
    return pid, int(fields[2]), int(fields[3]), int(fields[19])


def parse_environ(data: bytes) -> dict[str, str]:
    result: dict[str, str] = {}
    for entry in data.split(b"\0"):
        if not entry or b"=" not in entry:
            continue
        name, value = entry.split(b"=", 1)
        result[name.decode("utf-8", "replace")] = value.decode(
            "utf-8", "replace"
        )
    return result


def scan_processes(proc_root: Path = Path("/proc")) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    try:
        entries = list(proc_root.iterdir())
    except OSError:
        return records
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            stat_pid, pgrp, session_id, start_ticks = parse_stat(
                (entry / "stat").read_text(encoding="utf-8", errors="replace")
            )
            environ = parse_environ(read_limited(entry / "environ", 1024 * 1024))
            cmdline = read_limited(entry / "cmdline", 128 * 1024).replace(
                b"\0", b" "
            ).decode("utf-8", "replace").strip()
            comm = (entry / "comm").read_text(
                encoding="utf-8", errors="replace"
            ).strip()
        except (FileNotFoundError, ProcessLookupError, PermissionError, OSError, ValueError):
            continue
        records.append(
            {
                "pid": stat_pid,
                "pgrp": pgrp,
                "session": session_id,
                "start_ticks": start_ticks,
                "comm": comm,
                "cmdline": cmdline,
                "component": environ.get("MSYS_COMPONENT_ID"),
                "generation": environ.get("MSYS_GENERATION"),
            }
        )
    return records


def process_identity(record: dict[str, Any]) -> ProcessIdentity:
    return ProcessIdentity(int(record["pid"]), int(record["start_ticks"]))


def primary_component_processes(
    records: Iterable[dict[str, Any]],
) -> dict[str, ProcessIdentity]:
    grouped: dict[str, list[dict[str, Any]]] = {}
    for record in records:
        component = record.get("component")
        if isinstance(component, str) and component:
            grouped.setdefault(component, []).append(record)
    primaries: dict[str, ProcessIdentity] = {}
    for component, candidates in grouped.items():
        leaders = [
            item
            for item in candidates
            if item["pid"] == item["pgrp"] or item["pid"] == item["session"]
        ]
        selected = min(leaders or candidates, key=lambda item: int(item["pid"]))
        primaries[component] = process_identity(selected)
    return primaries


def classify_pipeline_processes(
    records: Iterable[dict[str, Any]], pid_file: Path
) -> dict[str, list[dict[str, Any]]]:
    wanted: set[int] = set()
    try:
        for line in pid_file.read_text(encoding="ascii", errors="strict").splitlines():
            if line.strip().isdigit() and int(line) > 0:
                wanted.add(int(line))
    except (FileNotFoundError, PermissionError, OSError, UnicodeError):
        pass
    groups: dict[str, list[dict[str, Any]]] = {
        "daemon": [],
        "xorg": [],
        "capture": [],
        "sink": [],
        "other": [],
    }
    for record in records:
        if int(record["pid"]) not in wanted:
            continue
        haystack = f"{record.get('comm', '')} {record.get('cmdline', '')}".lower()
        if "ch347_dirty_usb_x11_daemon.sh" in haystack:
            group = "daemon"
        elif re.search(r"(^|[ /])xorg([ .]|$)", haystack):
            group = "xorg"
        elif "xdamage_shm_capture" in haystack or "ffmpeg" in haystack:
            group = "capture"
        elif "ch347_dirty_usb_sink" in haystack:
            group = "sink"
        else:
            group = "other"
        groups[group].append(record)
    for items in groups.values():
        items.sort(key=lambda item: int(item["pid"]))
    return groups


def public_call(
    runtime_dir: Path, method: str, payload: dict[str, Any] | None = None
) -> dict[str, Any]:
    request = {
        "type": "call",
        "id": 1,
        "target": "msys.core",
        "method": method,
        "payload": payload or {},
        "deadline_ms": int(time.monotonic() * 1000 + 5000),
        "idempotent": True,
    }
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(7.0)
    try:
        connection.connect(str(runtime_dir / "control.sock"))
        welcome = recv_line(connection)
        connection.sendall(
            json.dumps(request, separators=(",", ":")).encode("utf-8") + b"\n"
        )
        response = recv_line(connection)
    finally:
        connection.close()
    if welcome.get("type") != "welcome":
        raise RuntimeError("Core control socket returned an invalid welcome")
    if response.get("type") != "return":
        raise RuntimeError(
            f"Core {method} failed: {response.get('code', 'unknown')} "
            f"{response.get('message', '')}".strip()
        )
    payload_result = response.get("payload")
    if not isinstance(payload_result, dict):
        raise RuntimeError(f"Core {method} returned a malformed payload")
    return payload_result


def recv_line(connection: socket.socket) -> dict[str, Any]:
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = connection.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
        if len(data) > MAX_PACKET:
            raise RuntimeError("Core response exceeded the acceptance limit")
    if not data:
        raise RuntimeError("Core closed the control socket without a response")
    document = json.loads(data.decode("utf-8"))
    if not isinstance(document, dict):
        raise RuntimeError("Core response is not an object")
    return document


def component_sets(
    component_documents: Iterable[dict[str, Any]],
    primaries: dict[str, ProcessIdentity],
) -> tuple[dict[str, ProcessIdentity], dict[str, ProcessIdentity]]:
    system_ui: dict[str, ProcessIdentity] = {}
    manual: dict[str, ProcessIdentity] = {}
    for component in component_documents:
        key = component.get("id")
        if not isinstance(key, str) or key not in primaries:
            continue
        lifecycle = component.get("lifecycle")
        windowing = component.get("windowing")
        inherits_x11 = (
            isinstance(windowing, dict)
            and windowing.get("system") == "x11"
            and windowing.get("display") == "inherit"
        )
        if lifecycle == "manual" and inherits_x11:
            manual[key] = primaries[key]
        elif lifecycle in {"background", "session"} and inherits_x11:
            system_ui[key] = primaries[key]
    return system_ui, manual


def identities(records: Iterable[dict[str, Any]]) -> list[ProcessIdentity]:
    return [process_identity(record) for record in records]


def same_identity(
    before: ProcessIdentity | None, after: ProcessIdentity | None
) -> bool:
    return before is not None and after is not None and before == after


def different_identity(
    before: ProcessIdentity | None, after: ProcessIdentity | None
) -> bool:
    return before is not None and after is not None and before != after


def identity_map_json(values: dict[str, ProcessIdentity]) -> dict[str, dict[str, int]]:
    return {key: identity.as_dict() for key, identity in sorted(values.items())}


def first_identity(values: list[ProcessIdentity]) -> ProcessIdentity | None:
    return values[0] if len(values) == 1 else None


def probe_notification(display: str) -> dict[str, Any] | None:
    """Return visible native-shell notification evidence, if present."""

    try:
        tree = subprocess.run(
            ["xwininfo", "-display", display, "-root", "-tree"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1.0,
            check=False,
        )
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
        return None
    if tree.returncode != 0:
        return None
    for line in tree.stdout.splitlines():
        if NOTIFICATION_TITLE not in line:
            continue
        match = re.search(r"\b(0x[0-9a-fA-F]+)\b", line)
        if match is None:
            continue
        window_id = match.group(1)
        try:
            details = subprocess.run(
                ["xwininfo", "-display", display, "-id", window_id],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=1.0,
                check=False,
            )
        except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
            continue
        if details.returncode == 0 and "Map State: IsViewable" in details.stdout:
            return {
                "kind": "visible-x11-window",
                "title": NOTIFICATION_TITLE,
                "window": window_id,
                "observed_at": utc_now(),
            }
    return None


def log_cursor(path: Path) -> dict[str, int | None]:
    try:
        metadata = path.stat()
        return {"inode": int(metadata.st_ino), "offset": int(metadata.st_size)}
    except OSError:
        return {"inode": None, "offset": 0}


def read_log_since(path: Path, cursor: dict[str, int | None]) -> str:
    try:
        metadata = path.stat()
        offset = int(cursor.get("offset") or 0)
        if cursor.get("inode") != int(metadata.st_ino) or metadata.st_size < offset:
            offset = 0
        with path.open("rb") as stream:
            stream.seek(offset)
            return stream.read(2 * 1024 * 1024).decode("utf-8", "replace")
    except OSError:
        return ""


class AcceptanceHarness:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.pid_file = Path(args.run_dir) / "pids"

    def snapshot(self) -> dict[str, Any]:
        records = scan_processes(Path(self.args.proc_root))
        primaries = primary_component_processes(records)
        payload = public_call(Path(self.args.runtime_dir), "list_components")
        components = payload.get("components")
        if not isinstance(components, list):
            raise RuntimeError("Core list_components omitted components")
        system_ui, manual = component_sets(components, primaries)
        pipeline = classify_pipeline_processes(records, self.pid_file)
        states = {
            str(item.get("id")): str(item.get("state"))
            for item in components
            if isinstance(item, dict) and isinstance(item.get("id"), str)
        }
        return {
            "pipeline": {
                name: identities(items)
                for name, items in pipeline.items()
            },
            "components": primaries,
            "component_states": states,
            "system_ui": system_ui,
            "manual_applications": manual,
        }

    @staticmethod
    def snapshot_json(snapshot: dict[str, Any]) -> dict[str, Any]:
        return {
            "pipeline": {
                name: [identity.as_dict() for identity in values]
                for name, values in snapshot["pipeline"].items()
            },
            "components": identity_map_json(snapshot["components"]),
            "component_states": dict(sorted(snapshot["component_states"].items())),
            "system_ui": identity_map_json(snapshot["system_ui"]),
            "manual_applications": identity_map_json(snapshot["manual_applications"]),
        }

    def preflight(self, snapshot: dict[str, Any], injection: str) -> list[dict[str, Any]]:
        pipeline = snapshot["pipeline"]
        checks = [
            check("one_xorg", len(pipeline["xorg"]) == 1, len(pipeline["xorg"])),
            check("one_sink", len(pipeline["sink"]) == 1, len(pipeline["sink"])),
            check("one_capture", len(pipeline["capture"]) == 1, len(pipeline["capture"])),
            check("one_daemon", len(pipeline["daemon"]) == 1, len(pipeline["daemon"])),
            check(
                "display_provider_running",
                DISPLAY_PROVIDER in snapshot["components"],
                snapshot["components"].get(DISPLAY_PROVIDER).as_dict()
                if DISPLAY_PROVIDER in snapshot["components"]
                else None,
            ),
            check("system_ui_running", bool(snapshot["system_ui"]), sorted(snapshot["system_ui"])),
        ]
        if injection in {"xorg", "all"}:
            checks.extend(
                [
                    check(
                        "manual_application_running",
                        bool(snapshot["manual_applications"]),
                        sorted(snapshot["manual_applications"]),
                        "start at least one manual UI app before the destructive Xorg test",
                    ),
                    check(
                        "xwininfo_available",
                        command_available("xwininfo"),
                        "xwininfo",
                        "notification evidence requires xwininfo",
                    ),
                ]
            )
        return checks

    def wait_for(
        self,
        evaluator: Callable[[dict[str, Any], str, dict[str, Any] | None], tuple[bool, list[dict[str, Any]]]],
        cursor: dict[str, int | None],
        *,
        observe_notification: bool,
        notification_gate: Callable[[dict[str, Any]], bool] | None = None,
    ) -> tuple[dict[str, Any], str, dict[str, Any] | None, list[dict[str, Any]]]:
        deadline = time.monotonic() + self.args.timeout
        last_snapshot: dict[str, Any] | None = None
        last_log = ""
        notification: dict[str, Any] | None = None
        last_checks: list[dict[str, Any]] = []
        while time.monotonic() < deadline:
            try:
                last_snapshot = self.snapshot()
            except (OSError, RuntimeError, ValueError, json.JSONDecodeError):
                time.sleep(self.args.poll_interval)
                continue
            if (
                observe_notification
                and notification is None
                and (notification_gate is None or notification_gate(last_snapshot))
            ):
                # Never accept an unrelated toast that happened to be visible
                # on the old X server immediately before fault injection.
                notification = probe_notification(self.args.display)
                if notification is not None:
                    current_xorg = first_identity(last_snapshot["pipeline"]["xorg"])
                    notification["xorg"] = (
                        current_xorg.as_dict() if current_xorg is not None else None
                    )
            last_log = read_log_since(Path(self.args.log_file), cursor)
            done, last_checks = evaluator(last_snapshot, last_log, notification)
            if done:
                return last_snapshot, last_log, notification, last_checks
            time.sleep(self.args.poll_interval)
        if last_snapshot is None:
            last_snapshot = self.snapshot()
        last_log = read_log_since(Path(self.args.log_file), cursor)
        _done, last_checks = evaluator(last_snapshot, last_log, notification)
        return last_snapshot, last_log, notification, last_checks

    def inject_sink(self, before: dict[str, Any]) -> dict[str, Any]:
        sink_before = first_identity(before["pipeline"]["sink"])
        if sink_before is None:
            raise RuntimeError("sink injection requires exactly one sink")
        cursor = log_cursor(Path(self.args.log_file))
        os.kill(sink_before.pid, signal.SIGKILL)

        def evaluate(
            after: dict[str, Any], log_text: str, _notification: dict[str, Any] | None
        ) -> tuple[bool, list[dict[str, Any]]]:
            checks = evaluate_sink(before, after, log_text)
            return all(item["ok"] for item in checks), checks

        after, log_text, _notification, checks = self.wait_for(
            evaluate, cursor, observe_notification=False
        )
        return scenario_result(
            "sink",
            sink_before.pid,
            before,
            after,
            checks,
            relevant_log_lines(log_text),
            None,
        )

    def inject_xorg(self, before: dict[str, Any]) -> dict[str, Any]:
        xorg_before = first_identity(before["pipeline"]["xorg"])
        if xorg_before is None:
            raise RuntimeError("Xorg injection requires exactly one Xorg")
        cursor = log_cursor(Path(self.args.log_file))
        os.kill(xorg_before.pid, signal.SIGKILL)

        def evaluate(
            after: dict[str, Any], log_text: str, notification: dict[str, Any] | None
        ) -> tuple[bool, list[dict[str, Any]]]:
            checks = evaluate_xorg(before, after, log_text, notification)
            return all(item["ok"] for item in checks), checks

        after, log_text, notification, checks = self.wait_for(
            evaluate,
            cursor,
            observe_notification=True,
            notification_gate=lambda current: different_identity(
                xorg_before,
                first_identity(current["pipeline"]["xorg"]),
            ),
        )
        return scenario_result(
            "xorg",
            xorg_before.pid,
            before,
            after,
            checks,
            relevant_log_lines(log_text),
            notification,
        )


def check(name: str, ok: bool, observed: Any, detail: str | None = None) -> dict[str, Any]:
    result: dict[str, Any] = {"name": name, "ok": bool(ok), "observed": observed}
    if detail:
        result["detail"] = detail
    return result


def command_available(name: str) -> bool:
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = Path(directory or ".") / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return True
    return False


def evaluate_sink(
    before: dict[str, Any], after: dict[str, Any], log_text: str
) -> list[dict[str, Any]]:
    old_sink = first_identity(before["pipeline"]["sink"])
    new_sink = first_identity(after["pipeline"]["sink"])
    old_capture = first_identity(before["pipeline"]["capture"])
    new_capture = first_identity(after["pipeline"]["capture"])
    old_xorg = first_identity(before["pipeline"]["xorg"])
    new_xorg = first_identity(after["pipeline"]["xorg"])
    old_daemon = first_identity(before["pipeline"]["daemon"])
    new_daemon = first_identity(after["pipeline"]["daemon"])
    manual_same = {
        key: same_identity(identity, after["manual_applications"].get(key))
        for key, identity in before["manual_applications"].items()
    }
    ui_same = {
        key: same_identity(identity, after["system_ui"].get(key))
        for key, identity in before["system_ui"].items()
    }
    return [
        check("sink_recreated", different_identity(old_sink, new_sink), identity_pair(old_sink, new_sink)),
        check("capture_recreated", different_identity(old_capture, new_capture), identity_pair(old_capture, new_capture)),
        check("xorg_preserved", same_identity(old_xorg, new_xorg), identity_pair(old_xorg, new_xorg)),
        check("daemon_preserved", same_identity(old_daemon, new_daemon), identity_pair(old_daemon, new_daemon)),
        check("system_ui_preserved", bool(ui_same) and all(ui_same.values()), ui_same),
        check(
            "manual_apps_preserved",
            all(manual_same.values()),
            manual_same,
            "vacuously true when no manual app was running",
        ),
        check(
            "no_full_display_recovery",
            RECOVERY_LINE.search(log_text) is None and FAULT_LINE.search(log_text) is None,
            relevant_log_lines(log_text),
        ),
    ]


def evaluate_xorg(
    before: dict[str, Any],
    after: dict[str, Any],
    log_text: str,
    notification: dict[str, Any] | None,
) -> list[dict[str, Any]]:
    old_xorg = first_identity(before["pipeline"]["xorg"])
    new_xorg = first_identity(after["pipeline"]["xorg"])
    old_provider = before["components"].get(DISPLAY_PROVIDER)
    new_provider = after["components"].get(DISPLAY_PROVIDER)
    app_results = {
        key: {
            "old_gone": after["manual_applications"].get(key) != identity,
            "not_reopened": key not in after["manual_applications"],
        }
        for key, identity in before["manual_applications"].items()
    }
    ui_results = {
        key: {
            "recreated": different_identity(identity, after["system_ui"].get(key)),
            "state": after["component_states"].get(key),
        }
        for key, identity in before["system_ui"].items()
    }
    return [
        check("xorg_recreated", different_identity(old_xorg, new_xorg), identity_pair(old_xorg, new_xorg)),
        check("provider_recreated", different_identity(old_provider, new_provider), identity_pair(old_provider, new_provider)),
        check(
            "system_ui_only_recreated",
            bool(ui_results)
            and all(item["recreated"] and item["state"] == "ready" for item in ui_results.values()),
            ui_results,
        ),
        check(
            "manual_apps_not_reopened",
            bool(app_results)
            and all(item["old_gone"] and item["not_reopened"] for item in app_results.values()),
            app_results,
        ),
        check("display_session_lost_logged", FAULT_LINE.search(log_text) is not None, relevant_log_lines(log_text)),
        check(
            "applications_reopened_false",
            RECOVERY_LINE.search(log_text) is not None,
            relevant_log_lines(log_text),
        ),
        check(
            "notification_visible_after_recovery",
            notification is not None,
            notification,
            "a visible native-shell notification window must overlap the recovery event",
        ),
    ]


def identity_pair(
    before: ProcessIdentity | None, after: ProcessIdentity | None
) -> dict[str, dict[str, int] | None]:
    return {
        "before": before.as_dict() if before else None,
        "after": after.as_dict() if after else None,
    }


def relevant_log_lines(text: str) -> list[str]:
    keywords = (
        "display output fault",
        "display output recovered",
        "display unavailable",
        "x-session-lost",
        "dirty_usb_x11_restart",
    )
    return [
        line[:1024]
        for line in text.splitlines()
        if any(keyword in line for keyword in keywords)
    ][-80:]


def scenario_result(
    kind: str,
    killed_pid: int,
    before: dict[str, Any],
    after: dict[str, Any],
    checks: list[dict[str, Any]],
    log_lines: list[str],
    notification: dict[str, Any] | None,
) -> dict[str, Any]:
    return {
        "kind": kind,
        "injected": True,
        "signal": "SIGKILL",
        "killed_pid": killed_pid,
        "ok": all(item["ok"] for item in checks),
        "checks": checks,
        "before": AcceptanceHarness.snapshot_json(before),
        "after": AcceptanceHarness.snapshot_json(after),
        "evidence": {"core_log": log_lines, "notification": notification},
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="locally verify CH347 transport and X-session recovery"
    )
    parser.add_argument("--runtime-dir", default="/tmp/msys-main")
    parser.add_argument("--run-dir", default="/tmp/ch347_dirty_usb_x11")
    parser.add_argument("--log-file", default="/tmp/msysd.log")
    parser.add_argument("--display", default=":24")
    parser.add_argument(
        "--inject",
        choices=("sink", "xorg", "all"),
        help="destructive fault to inject; omission is a read-only inspection",
    )
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--poll-interval", type=float, default=0.2)
    parser.add_argument("--output", help="also atomically write the JSON result here")
    parser.add_argument(
        "--proc-root",
        default="/proc",
        help=argparse.SUPPRESS,
    )
    return parser


def validate_args(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if not 2.0 <= args.timeout <= 300.0:
        parser.error("--timeout must be between 2 and 300 seconds")
    if not 0.05 <= args.poll_interval <= 2.0:
        parser.error("--poll-interval must be between 0.05 and 2 seconds")
    if args.inject and (not hasattr(os, "geteuid") or os.geteuid() != 0):
        parser.error("fault injection must run as root")


def write_result(path: str, document: dict[str, Any]) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.name}.{os.getpid()}.tmp")
    temporary.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, target)


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    validate_args(parser, args)
    result: dict[str, Any] = {
        "schema": SCHEMA,
        "started_at": utc_now(),
        "mode": f"inject-{args.inject}" if args.inject else "read-only",
        "ok": False,
        "configuration": {
            "runtime_dir": args.runtime_dir,
            "run_dir": args.run_dir,
            "log_file": args.log_file,
            "display": args.display,
            "timeout_seconds": args.timeout,
        },
        "preflight": [],
        "scenarios": [],
    }
    status = 1
    try:
        harness = AcceptanceHarness(args)
        baseline = harness.snapshot()
        result["baseline"] = harness.snapshot_json(baseline)
        result["preflight"] = harness.preflight(baseline, args.inject or "read-only")
        preflight_ok = all(item["ok"] for item in result["preflight"])
        if not preflight_ok:
            result["error"] = "preflight failed; no fault was injected"
        elif not args.inject:
            result["ok"] = True
            result["ready_for"] = {
                "sink": True,
                "xorg": bool(baseline["manual_applications"])
                and command_available("xwininfo"),
            }
            status = 0
        else:
            current = baseline
            if args.inject in {"sink", "all"}:
                sink_result = harness.inject_sink(current)
                result["scenarios"].append(sink_result)
                if not sink_result["ok"]:
                    result["error"] = "sink recovery acceptance failed; Xorg was not injected"
                else:
                    current = harness.snapshot()
            if args.inject in {"xorg", "all"} and not result.get("error"):
                # Re-run the destructive preflight after the sink scenario so
                # the Xorg test never relies on a stale process identity.
                xorg_preflight = harness.preflight(current, "xorg")
                if not all(item["ok"] for item in xorg_preflight):
                    result["error"] = "Xorg preflight failed after sink recovery"
                    result["xorg_preflight"] = xorg_preflight
                else:
                    result["scenarios"].append(harness.inject_xorg(current))
            result["ok"] = (
                not result.get("error")
                and bool(result["scenarios"])
                and all(item["ok"] for item in result["scenarios"])
            )
            status = 0 if result["ok"] else 1
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    result["finished_at"] = utc_now()
    if args.output:
        try:
            write_result(args.output, result)
        except OSError as exc:
            result["ok"] = False
            result["output_error"] = str(exc)
            status = 1
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return status


if __name__ == "__main__":
    raise SystemExit(main())
