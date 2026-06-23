from __future__ import annotations

import json
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

from src.diagnostics import RuntimeDiagnostics
from src.process import DEFAULT_GAME_PROCESS_NAME, process_name_matches
from src.protocol import (
    BridgeResponse,
    decode_request_line,
    decode_response_line,
    encode_response_line,
    make_request,
    encode_request_line,
)
from src.cli import _KeyEdgeTrigger, _same_key
from src.core import Color, PlanConfig, compose_plan


def _sample_color(_u: float, _v: float) -> Color:
    return Color(0.2, 0.3, 0.4)


class ComposePlanTests(unittest.TestCase):
    def test_default_front_plan_uses_requested_sample_count(self) -> None:
        plan = compose_plan(
            PlanConfig(
                sample_count=2048,
                min_front_hits=409,
                target_front_hits=819,
                preferred_front_hits=2048,
            ),
            _sample_color,
        )

        self.assertEqual(plan.total_samples, 2048)
        self.assertEqual(len(plan.front_samples), 2048)
        self.assertEqual(len(plan.side_samples), 0)
        self.assertEqual(len(plan.back_samples), 0)

    def test_side_back_distribution_preserves_total_sample_count(self) -> None:
        plan = compose_plan(
            PlanConfig(
                sample_count=100,
                min_front_hits=20,
                target_front_hits=65,
                preferred_front_hits=100,
                include_side=True,
                include_back=True,
            ),
            _sample_color,
        )

        self.assertEqual(plan.total_samples, 100)
        self.assertGreater(len(plan.side_samples), 0)
        self.assertGreater(len(plan.back_samples), 0)


class ServiceTriggerTests(unittest.TestCase):
    def test_key_edge_trigger_fires_once_per_press(self) -> None:
        states = iter([False, True, True, False, True])
        trigger = _KeyEdgeTrigger(lambda: next(states))

        self.assertFalse(trigger.consume())
        self.assertTrue(trigger.consume())
        self.assertFalse(trigger.consume())
        self.assertFalse(trigger.consume())
        self.assertTrue(trigger.consume())

    def test_same_trigger_and_stop_key_is_detected_case_insensitively(self) -> None:
        self.assertTrue(_same_key("f10", "F10"))
        self.assertFalse(_same_key("", "F10"))
        self.assertFalse(_same_key("f9", "F10"))


class ProcessTests(unittest.TestCase):
    def test_process_name_matcher_finds_default_game_process_name(self) -> None:
        self.assertTrue(process_name_matches("PenguinHotel-Win64-Shipping.exe", DEFAULT_GAME_PROCESS_NAME))
        self.assertTrue(process_name_matches(r"C:\Game\PenguinHotel-Win64-Shipping.exe", DEFAULT_GAME_PROCESS_NAME))
        self.assertFalse(process_name_matches("Chameleon-Win64-Shipping.exe", DEFAULT_GAME_PROCESS_NAME))


class DiagnosticsTests(unittest.TestCase):
    def test_jsonl_writer_and_status_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            diagnostics = RuntimeDiagnostics(tmp, console=False)
            diagnostics.event("unit_event", stage="unit", message="hello", details={"value": 1})
            diagnostics.merge_status(process={"attached": False, "target_name": DEFAULT_GAME_PROCESS_NAME})

            events = (Path(tmp) / "events.jsonl").read_text(encoding="utf-8").strip().splitlines()
            self.assertEqual(len(events), 1)
            self.assertEqual(json.loads(events[0])["event"], "unit_event")

            status = json.loads((Path(tmp) / "last_status.json").read_text(encoding="utf-8"))
            self.assertEqual(status["process"]["target_name"], DEFAULT_GAME_PROCESS_NAME)
            self.assertTrue((Path(tmp) / "runtime.log").exists())


class ProtocolTests(unittest.TestCase):
    def test_protocol_round_trips_commands_and_response(self) -> None:
        for command in ("ping", "capabilities", "paint_full_route"):
            request = make_request(command, {"route": "f10_full_body_metallic_then_front"}, request_id=f"{command}-id")
            decoded = decode_request_line(encode_request_line(request))
            self.assertEqual(decoded.type, command)
            self.assertEqual(decoded.request_id, f"{command}-id")

        response = BridgeResponse(True, "paint_done", applied=2048, message="ok")
        decoded_response = decode_response_line(encode_response_line(response))
        self.assertTrue(decoded_response.success)
        self.assertEqual(decoded_response.stage, "paint_done")
        self.assertEqual(decoded_response.applied, 2048)


class ServiceIntegrationTests(unittest.TestCase):
    def test_service_waits_for_missing_process_and_writes_status(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "src",
                    "--mode",
                    "service",
                    "--game-process-name",
                    "DefinitelyMissingMecchaProcess.exe",
                    "--service-max-frames",
                    "1",
                    "--process-poll-seconds",
                    "0.01",
                    "--log-dir",
                    tmp,
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            events = (Path(tmp) / "events.jsonl").read_text(encoding="utf-8")
            self.assertIn("waiting_for_process", events)
            status = json.loads((Path(tmp) / "last_status.json").read_text(encoding="utf-8"))
            self.assertFalse(status["process"]["attached"])

    def test_service_disables_stop_key_when_it_matches_trigger_key(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "src",
                    "--mode",
                    "service",
                    "--game-process-name",
                    "DefinitelyMissingMecchaProcess.exe",
                    "--service-max-frames",
                    "1",
                    "--service-stop-key",
                    "f10",
                    "--service-trigger-key",
                    "f10",
                    "--process-poll-seconds",
                    "0.01",
                    "--log-dir",
                    tmp,
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            events = (Path(tmp) / "events.jsonl").read_text(encoding="utf-8")
            self.assertIn("stop_key_disabled", events)

    def test_service_trigger_file_can_reach_mock_bridge(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            trigger = Path(tmp) / "trigger.txt"
            trigger.write_text("go", encoding="utf-8")
            port, thread = _start_one_shot_mock_bridge()
            proc_comm = Path("/proc/self/comm")
            process_name = proc_comm.read_text(encoding="utf-8").strip() if proc_comm.exists() else Path(sys.executable).name

            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "src",
                    "--mode",
                    "service",
                    "--adapter",
                    "xenos",
                    "--bridge-path",
                    f"127.0.0.1:{port}",
                    "--bridge-transport",
                    "tcp",
                    "--bridge-wait-response",
                    "--game-process-name",
                    process_name,
                    "--service-trigger-file",
                    str(trigger),
                    "--service-max-frames",
                    "3",
                    "--process-poll-seconds",
                    "0.01",
                    "--frame-delay-ms",
                    "1",
                    "--log-dir",
                    tmp,
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
            thread.join(timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            events = (Path(tmp) / "events.jsonl").read_text(encoding="utf-8")
            self.assertIn("trigger_file_detected", events)
            self.assertIn("plan_generated", events)
            self.assertIn("paint_done", events)


def _start_one_shot_mock_bridge() -> tuple[int, threading.Thread]:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]

    def serve() -> None:
        try:
            conn, _ = listener.accept()
            with conn:
                _ = conn.recv(1024 * 1024)
                response = BridgeResponse(True, "paint_done", applied=2048, failures=0, message="mock paint done")
                conn.sendall(encode_response_line(response))
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    return port, thread


if __name__ == "__main__":
    unittest.main()
