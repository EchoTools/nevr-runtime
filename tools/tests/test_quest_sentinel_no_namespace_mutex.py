#!/usr/bin/env python3
"""No namespace-scope std::mutex in the Quest sentinel (#1616).

A namespace-scope std::mutex has a destructor, so its translation unit gets a _GLOBAL__sub_I initializer
that tools/check_quest_static_init.sh refuses. This reads the sources, so it runs without an NDK.
"""

from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SENTINEL = REPO / "src" / "quest" / "sentinel"

# A declaration at column 0 of a std::mutex object (not a function returning one, not a reference or pointer to one).
NAMESPACE_MUTEX = re.compile(r"^std::mutex\s+\w+\s*(\{[^}]*\})?\s*;", re.MULTILINE)


class QuestSentinelNoNamespaceMutexTest(unittest.TestCase):
    def test_texture_menu_hook_has_no_namespace_scope_mutex(self) -> None:
        text = (SENTINEL / "texture_menu_hook.cpp").read_text()
        self.assertEqual([], NAMESPACE_MUTEX.findall(text))

    def test_no_sentinel_source_declares_a_namespace_scope_mutex(self) -> None:
        offenders = [
            path.name
            for path in sorted(SENTINEL.glob("*.cpp"))
            if NAMESPACE_MUTEX.search(path.read_text())
        ]
        self.assertEqual([], offenders)


if __name__ == "__main__":
    unittest.main()
