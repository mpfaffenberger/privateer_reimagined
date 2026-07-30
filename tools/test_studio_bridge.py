"""Regression tests for Cinematic Studio author request identity/arguments."""
from __future__ import annotations

import unittest

from tools.cinematics import studio_bridge as bridge


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


if __name__ == "__main__":
    unittest.main()
