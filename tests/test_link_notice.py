from __future__ import annotations

import importlib.util
import json
import socket
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "scripts/msys_ch347_link_notice.py"
SPEC = importlib.util.spec_from_file_location("msys_ch347_link_notice", ENTRY)
assert SPEC is not None and SPEC.loader is not None
notice = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(notice)


class LinkNoticeTests(unittest.TestCase):
    def test_payload_is_localized_and_structured(self) -> None:
        failed = notice.notice_payload("degraded", {"MSYS_LOCALE": "zh-CN"})
        recovered = notice.notice_payload("healthy", {"MSYS_LOCALE": "en-US"})
        self.assertIn("未恢复到 480M", failed["message"])
        self.assertEqual(failed["urgency"], "critical")
        self.assertIn("recovered at 480M", recovered["message"])
        self.assertEqual(recovered["state"], "healthy")
        self.assertEqual(recovered["required_speed_mbps"], 480)

    def test_publish_writes_one_seqpacket_event_without_closing_owner_fd(self) -> None:
        supervisor, component = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        try:
            environment = {
                "MSYS_CONTROL_FD": str(component.fileno()),
                "MSYS_LOCALE": "zh-CN",
            }
            self.assertTrue(notice.publish("degraded", environment))
            packet = json.loads(supervisor.recv(65536).decode("utf-8"))
            self.assertEqual(packet["type"], "event")
            self.assertEqual(packet["topic"], notice.TOPIC)
            self.assertEqual(packet["payload"]["state"], "degraded")
            self.assertIn("检查接口", packet["payload"]["message"])
            component.getsockname()  # publish() detached rather than closing it.
        finally:
            component.close()
            supervisor.close()

    def test_missing_control_descriptor_is_a_clean_noop(self) -> None:
        self.assertFalse(notice.publish("healthy", {}))


if __name__ == "__main__":
    unittest.main()
