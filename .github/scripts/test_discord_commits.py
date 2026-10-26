#!/usr/bin/env python3
"""Check the Discord commit payload before the workflow sends it."""

import importlib.util
import unittest
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "discord_commits", Path(__file__).with_name("discord_commits.py")
)
commits = importlib.util.module_from_spec(spec)
spec.loader.exec_module(commits)


class CommitPayloadTests(unittest.TestCase):
    def test_commit_links_and_mentions(self):
        event = {
            "repository": {"full_name": "TheMoonPeople/Chronicle"},
            "commits": [{
                "id": "a" * 40,
                "message": "@everyone Fix *menu*\nDetailed body",
                "author": {"name": "Test Author"},
            }],
        }
        payload = commits.payload(event)
        self.assertEqual(payload["username"], "Chronicle")
        self.assertEqual(payload["allowed_mentions"], {"parse": []})
        self.assertIn("https://github.com/TheMoonPeople/Chronicle/commit/", payload["content"])
        self.assertIn(r"\*menu\*", payload["content"])
        self.assertNotIn("Detailed body", payload["content"])

    def test_large_push_stays_within_discord_limit(self):
        event = {
            "repository": {"full_name": "TheMoonPeople/Chronicle"},
            "commits": [
                {"id": f"{i:040x}", "message": "😀" * 200}
                for i in range(40)
            ],
        }
        content = commits.payload(event)["content"]
        self.assertLessEqual(commits.discord_length(content), 2000)
        self.assertIn("more commits", content)


if __name__ == "__main__":
    unittest.main()
