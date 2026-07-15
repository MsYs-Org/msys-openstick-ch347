"""Publish one bounded CH347 transport-health notification over private mIPC."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
from collections.abc import Mapping, Sequence


TOPIC = "msys.role.notification-presenter"
COMPONENT = "org.msys.openstick.ch347:x11-spi-touch-output"


def _is_chinese(environ: Mapping[str, str]) -> bool:
    locale = (
        environ.get("MSYS_LOCALE")
        or environ.get("LC_ALL")
        or environ.get("LC_MESSAGES")
        or environ.get("LANG")
        or ""
    )
    return locale.casefold().replace("_", "-").startswith("zh")


def notice_payload(state: str, environ: Mapping[str, str]) -> dict[str, object]:
    if state == "degraded":
        message = (
            "显示连接异常：CH347 未恢复到 480M，请检查接口。"
            if _is_chinese(environ)
            else "Display link unavailable: CH347 did not recover at 480M; check the connector."
        )
        title = "CH347 显示连接异常" if _is_chinese(environ) else "CH347 display link unavailable"
        urgency = "critical"
    elif state == "healthy":
        message = (
            "CH347 显示连接已恢复到 480M。"
            if _is_chinese(environ)
            else "CH347 display link recovered at 480M."
        )
        title = "CH347 显示连接已恢复" if _is_chinese(environ) else "CH347 display link recovered"
        urgency = "normal"
    else:
        raise ValueError(f"unsupported CH347 link state: {state}")
    return {
        "title": title,
        "message": message,
        "urgency": urgency,
        "source": COMPONENT,
        "code": "ch347-link-speed",
        "state": state,
        "required_speed_mbps": 480,
    }


def publish(state: str, environ: Mapping[str, str] | None = None) -> bool:
    values = os.environ if environ is None else environ
    descriptor_text = values.get("MSYS_CONTROL_FD", "")
    try:
        descriptor = int(descriptor_text)
    except ValueError:
        return False
    if descriptor < 0:
        return False
    packet = json.dumps(
        {"type": "event", "topic": TOPIC, "payload": notice_payload(state, values)},
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    channel = socket.socket(fileno=descriptor)
    try:
        channel.sendall(packet)
    finally:
        # The provider process owns its descriptor. This short-lived helper
        # must not alter that ownership when its own fd table is torn down.
        channel.detach()
    return True


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--state", choices=("degraded", "healthy"), required=True)
    args = parser.parse_args(argv)
    try:
        sent = publish(args.state)
    except OSError as exc:
        print(f"msys-ch347-notice: publish failed: {exc}", file=sys.stderr)
        return 1
    if not sent:
        print("msys-ch347-notice: MSYS_CONTROL_FD is unavailable", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
