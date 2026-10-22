"""#749: the Quest OBB skip chooses its data root before it hooks the game.

Install() reaches libr15's GOT, which no host test can; the host test (obb_skip_hook_test) covers the choice
and what the hook serves. What it cannot see is that Install() makes the choice at all, and before the path
hook can answer, so that is pinned here by source shape.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]


class InstallChoosesTheRootFirst(unittest.TestCase):
    def test_install_resolves_the_root_with_the_real_filesystem_before_the_first_thunk(self):
        source = (ROOT / "src/quest/sentinel/obb_skip_hook.cpp").read_text()
        body = extract_braced_function(source, "bool Install() noexcept {")
        self.assertIn("ResolveRoot(&DirectoryExists)", body)
        self.assertLess(body.index("ResolveRoot(&DirectoryExists)"), body.index("PathThunk::Arm"))


if __name__ == "__main__":
    unittest.main()
