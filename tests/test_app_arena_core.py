# SPDX-License-Identifier: MIT
# Lewis Moten; Hermes Agent / OpenAI Codex GPT-6.1 Sol (openai-codex).
"""Standalone unlinked production-unit tests; never add the unit to runtime."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AppArenaCore(unittest.TestCase):
    def run_harness(self, name):
        with tempfile.TemporaryDirectory(prefix="app-arena-") as tmp:
            executable = str(Path(tmp) / name)
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1",
                            "-Wall", "-Wextra", "-Werror", "-Iinclude",
                            "tests/" + name + ".c", "core/app_arena_core.c",
                            "-o", executable], cwd=ROOT, check=True)
            subprocess.run([executable], cwd=ROOT, check=True)

    def test_transaction_epoch_teardown(self):
        self.run_harness("app_arena_core")

    def test_actual_uspace_orders_and_exhaustion(self):
        self.run_harness("app_arena_maps")

if __name__ == "__main__":
    unittest.main()
