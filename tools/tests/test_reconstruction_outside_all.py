"""src/reconstruction is outside the default build (#1974).

`add_subdirectory(src/reconstruction)` without EXCLUDE_FROM_ALL made every default build compile its 316 translation units, and no
target in the tree links nevr_r15_reconstruction. The subdirectory is excluded from ALL; `just build-reconstruction` builds the
library on request, so the reconstruction still compiles when it changes.

Both checks read the text of the build files; neither builds anything.
"""

from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
ROOT_CMAKE = REPO / "CMakeLists.txt"
JUSTFILE = REPO / "justfile"
LIBRARY = "nevr_r15_reconstruction"
RECIPE = "build-reconstruction"
ADD_LINE = re.compile(r"^add_subdirectory\(src/reconstruction(?P<rest>[^)]*)\)\s*$", re.MULTILINE)
SKIPPED_DIRS = {"build", "extern", "gen", ".git", ".claude", "node_modules"}


def build_files() -> list[pathlib.Path]:
    found = []
    for path in REPO.rglob("*"):
        if not path.is_file() or SKIPPED_DIRS.intersection(path.relative_to(REPO).parts):
            continue
        if path.name == "CMakeLists.txt" or path.suffix == ".cmake":
            found.append(path)
    return found


class ReconstructionIsOutsideAll(unittest.TestCase):
    def test_the_root_adds_the_subdirectory_with_exclude_from_all(self) -> None:
        match = ADD_LINE.search(ROOT_CMAKE.read_text(encoding="utf-8"))
        self.assertIsNotNone(match, "CMakeLists.txt has no add_subdirectory(src/reconstruction) line")
        self.assertRegex(match.group("rest"), r"^\s+EXCLUDE_FROM_ALL$",
                         "add_subdirectory(src/reconstruction) must pass EXCLUDE_FROM_ALL (#1974)")

    def test_no_build_file_outside_the_library_links_it(self) -> None:
        # The exclusion is safe only while nothing outside src/reconstruction depends on the library. A file that does is
        # listed here, and the exclusion then needs a second look.
        linkers = []
        for path in build_files():
            if (REPO / "src" / "reconstruction") in path.parents:
                continue
            code = "\n".join(line.split("#", 1)[0] for line in path.read_text(encoding="utf-8", errors="replace").splitlines())
            if LIBRARY in code:  # a comment that names the library is not a link
                linkers.append(str(path.relative_to(REPO)))
        self.assertEqual(linkers, [], f"a build file outside src/reconstruction names {LIBRARY}")


class ReconstructionRecipe(unittest.TestCase):
    def test_a_recipe_builds_the_library_on_request(self) -> None:
        text = JUSTFILE.read_text(encoding="utf-8")
        self.assertRegex(text, rf"(?m)^{RECIPE}: configure$", f"the justfile has no `{RECIPE}` recipe after configure")
        self.assertRegex(text, rf"(?m)^    cmake --build --preset \{{\{{ preset \}}\}} --target {LIBRARY}$",
                         f"`{RECIPE}` does not build the {LIBRARY} target")


if __name__ == "__main__":
    unittest.main()
