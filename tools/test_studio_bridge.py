"""Regression tests for Cinematic Studio author request identity/arguments."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.cinematics import studio_bridge as bridge
from tools.cinematics import voices


class StudioBridgeAuthorTests(unittest.TestCase):
    def test_missing_cinematic_id_uses_unique_request_identity(self) -> None:
        request = {"id": "req_1785449208", "kind": "author", "brief": "A scene"}
        self.assertEqual(bridge.author_cinematic_id(request),
                         "studio_req_1785449208")
        self.assertIn("studio_req_1785449208", bridge.build_author_prompt(request))

    def test_explicit_cinematic_id_is_preserved(self) -> None:
        request = {"id": "req_1", "cinematic_id": "troy_interdiction"}
        self.assertEqual(bridge.author_cinematic_id(request), "troy_interdiction")

    def test_prompt_is_one_argv_entry_without_shell_quoting(self) -> None:
        request = {"id": "req_2", "brief": "Grayson's cargo isn't legal."}
        command = bridge.author_command(request)
        self.assertEqual(command[:4], ["code-puppy", "--agent",
                                       bridge.AGENT_NAME, "-p"])
        self.assertEqual(len(command), 5)
        self.assertIn("Grayson's cargo isn't legal.", command[4])
        self.assertNotIn("'\"'\"'", command[4])

    def test_successful_agent_exit_requires_cinematic_output(self) -> None:
        request = {"id": "req_3", "kind": "author"}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(bridge, "repo_root", return_value=Path(directory)), \
             mock.patch.object(bridge.shutil, "which", return_value="code-puppy"), \
             mock.patch.object(bridge.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="director stopped safely", stderr="")):
            with self.assertRaisesRegex(bridge.BridgeError, "did not create"):
                bridge.process_author(request, [])

    def test_minimax_http_200_logical_error_is_raised(self) -> None:
        payload = {"base_resp": {"status_code": 1004, "status_msg": "login fail"}}
        with self.assertRaisesRegex(RuntimeError, "1004: login fail"):
            voices._decode_t2a_audio(payload)

    def test_minimax_audio_hex_decodes(self) -> None:
        payload = {"base_resp": {"status_code": 0}, "data": {"audio": "494433"}}
        self.assertEqual(voices._decode_t2a_audio(payload), b"ID3")


if __name__ == "__main__":
    unittest.main()
