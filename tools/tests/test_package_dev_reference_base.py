"""The development Quest APK is repacked from the reference build, as the release APK is (#1995, the #1994 sibling).

`just package-dev` used to repack the dev APK from the store APK, which has none of the reference build's combat entries, so a
tester build had no Quest combat content. The dev path now takes the reference APK (NEVR_QUEST_REFERENCE_APK), prepares the base
with tools/quest_release_base.py, repacks from that base, and checks the result with check-release.

The checks read the justfile text. They do not run the build.
"""

from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
JUSTFILE = REPO / "justfile"
HEADER = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*( [^:]*)?:")


def recipe_text(name: str) -> str:
    """The lines of recipe `name`, from its header to the next recipe header."""
    lines = JUSTFILE.read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith(f"{name} ") and HEADER.match(line))
    body = [lines[start]]
    for line in lines[start + 1:]:
        if line and not line[0].isspace() and not line.startswith("#") and HEADER.match(line):
            break
        body.append(line)
    return "\n".join(body)


class PackageDevFromTheReferenceBase(unittest.TestCase):
    def test_package_dev_takes_the_reference_apk_not_the_store_apk(self) -> None:
        header = recipe_text("package-dev").splitlines()[0]
        self.assertIn('reference_apk=env("NEVR_QUEST_REFERENCE_APK")', header)
        self.assertNotIn("NEVR_STORE_APK", header)

    def test_the_dev_quest_apk_takes_its_base_from_the_reference_build(self) -> None:
        body = recipe_text("_quest-apk")
        self.assertIn("quest_release_base.py prepare", body, "_quest-apk does not prepare the reference base")
        self.assertIn('android-repack-apk "$base"', body, "_quest-apk does not repack from the prepared base")
        self.assertNotIn("store_apk", body, "_quest-apk still repacks from the store APK")

    def test_the_dev_apk_is_checked_against_the_base_after_the_repack(self) -> None:
        body = recipe_text("_quest-apk")
        repack = body.index("android-repack-apk")
        check = body.index("quest_release_base.py check-release")
        self.assertLess(repack, check, "check-release must run after the repack")
        self.assertIn('--base "$base"', body[check:])

    def test_package_dev_hands_the_reference_apk_to_the_dev_quest_apk(self) -> None:
        self.assertIn('just _quest-apk "{{ reference_apk }}"', recipe_text("package-dev"))


if __name__ == "__main__":
    unittest.main()
