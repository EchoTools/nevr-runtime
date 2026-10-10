"""The CI jobs run in the prebuilt builder image, and every workflow names the tag that the
Dockerfile's hash gives, so a Dockerfile change cannot leave a workflow on a stale toolchain."""
from __future__ import annotations

import hashlib
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
DOCKERFILE = REPO / ".github/builder/Dockerfile"
WORKFLOWS = REPO / ".github/workflows"
IMAGE = "ghcr.io/echotools/nevr-runtime-builder"
USERS = ("build.yml", "vcpkg-cache.yml", "defender-scan.yml")


def dockerfile_tag() -> str:
    return hashlib.sha256(DOCKERFILE.read_bytes()).hexdigest()[:12]


class BuilderImageTest(unittest.TestCase):
    def test_every_job_that_builds_runs_in_the_image_tagged_with_the_dockerfile_hash(self):
        want = f"{IMAGE}:{dockerfile_tag()}"
        for name in USERS:
            text = (WORKFLOWS / name).read_text()
            tags = re.findall(rf"image: ({re.escape(IMAGE)}:\S+)", text)
            self.assertEqual(tags, [want], f"{name}: builder image tag is not the Dockerfile hash; "
                                           f"set it to {dockerfile_tag()} (sha256sum of the Dockerfile)")

    def test_no_job_installs_the_toolchain_per_run(self):
        for name in USERS:
            text = (WORKFLOWS / name).read_text()
            self.assertNotIn("pacman -Syu", text, name)
            self.assertNotIn("archlinux:base-devel", text, name)
            self.assertNotIn("go install github.com/bufbuild", text, name)

    def test_the_image_workflow_pushes_the_dockerfile_hash_tag_and_may_write_packages(self):
        text = (WORKFLOWS / "builder-image.yml").read_text()
        self.assertIn("packages: write", text)
        self.assertIn("sha256sum .github/builder/Dockerfile | cut -c1-12", text)
        self.assertIn(f"IMAGE: {IMAGE}", text)
        self.assertIn("workflow_dispatch:", text)

    def test_the_jobs_that_pull_the_image_may_read_packages(self):
        for name in USERS:
            self.assertIn("packages: read", (WORKFLOWS / name).read_text(), name)


if __name__ == "__main__":
    unittest.main()
