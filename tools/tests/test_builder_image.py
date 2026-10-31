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


def dockerfile_text() -> str:
    return "\n".join(line for line in DOCKERFILE.read_text().splitlines() if not line.lstrip().startswith("#"))


def dockerfile_pins() -> dict[str, str]:
    """name -> version of every package the Dockerfile's `pacman -S` line installs."""
    m = re.search(r"pacman -S --noconfirm --needed((?:\s*\\?\n?\s*[^\s&\\]+)+)", dockerfile_text())
    assert m, "the Dockerfile has no `pacman -S --noconfirm --needed` line"
    pins = {}
    for token in m.group(1).replace("\\", " ").split():
        name, eq, version = token.partition("=")
        pins[name] = version if eq else ""
    return pins


def toolchain_step_versions() -> dict[str, str]:
    """name -> version the build.yml `Toolchain versions` step asserts."""
    text = (WORKFLOWS / "build.yml").read_text()
    m = re.search(r"- name: Toolchain versions\n\s+run: \|\n(.*?)\n\n", text, re.S)
    assert m, "build.yml has no `Toolchain versions` step"
    m2 = re.search(r"want=\$\(cat <<'EOF'\n(.*?)\n\s*EOF", m.group(1), re.S)
    assert m2, "the Toolchain versions step has no `want=$(cat <<'EOF' ... EOF)` list of expected versions"
    out = {}
    for line in m2.group(1).splitlines():
        name, version = line.split()
        out[name] = version
    return out


def dockerfile_tag() -> str:
    return hashlib.sha256(DOCKERFILE.read_bytes()).hexdigest()[:12]


class BuilderImageTest(unittest.TestCase):
    def test_every_job_that_builds_runs_in_the_image_tagged_with_the_dockerfile_hash(self):
        want = f"{IMAGE}:{dockerfile_tag()}"
        for name in USERS:
            text = (WORKFLOWS / name).read_text()
            tags = re.findall(rf"image: ({re.escape(IMAGE)}:\S+)", text)
            # build.yml runs two jobs in the image (installer and build, #1770); the other workflows run one
            expected = [want, want] if name == "build.yml" else [want]
            self.assertEqual(tags, expected, f"{name}: builder image tag is not the Dockerfile hash; "
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

    def test_the_base_image_is_a_dated_tag_pinned_by_digest(self):
        froms = re.findall(r"^FROM (\S+)", DOCKERFILE.read_text(), re.M)
        self.assertEqual(len(froms), 1, froms)
        self.assertRegex(froms[0], r"^archlinux:base-devel-\d{8}\.0\.\d+@sha256:[0-9a-f]{64}$",
                         "FROM must name a dated base-devel tag and its sha256 digest, never the moving base-devel tag")

    def test_pacman_reads_a_dated_archive_snapshot_not_the_live_mirrors(self):
        text = dockerfile_text()
        self.assertRegex(text, r"ARG ARCH_SNAPSHOT=\d{4}/\d{2}/\d{2}\b")
        self.assertIn("https://archive.archlinux.org/repos/${ARCH_SNAPSHOT}/$$repo/os/$$arch".replace("$$", "\\$"), text)
        self.assertIn("pacman -Syyuu --noconfirm", text)
        self.assertNotIn("pacman -Syu", text)

    def test_every_package_the_dockerfile_installs_has_an_exact_version(self):
        unpinned = sorted(name for name, version in dockerfile_pins().items() if not version)
        self.assertEqual(unpinned, [], "packages installed without name=version")

    def test_the_toolchain_step_asserts_exactly_the_versions_the_dockerfile_installs(self):
        step, pins = toolchain_step_versions(), dockerfile_pins()
        for name in ("mingw-w64-gcc", "mingw-w64-binutils", "mingw-w64-headers", "mingw-w64-crt",
                     "mingw-w64-winpthreads", "cmake", "ninja", "wine", "go", "python"):
            self.assertIn(name, step, f"the Toolchain versions step does not assert {name}")
        for name, version in step.items():
            self.assertEqual(pins.get(name), version, f"{name}: build.yml asserts {version}, Dockerfile installs {pins.get(name)}")

    def test_the_toolchain_step_fails_on_a_different_version(self):
        text = (WORKFLOWS / "build.yml").read_text()
        step = re.search(r"- name: Toolchain versions\n\s+run: \|\n(.*?)\n\n", text, re.S).group(1)
        self.assertIn("got=$(printf '%s\\n' \"$want\" | cut -d' ' -f1 | xargs pacman -Q)", step)
        self.assertIn("test \"$(printf '%s\\n' \"$got\" | sort)\" = \"$(printf '%s\\n' \"$want\" | sort)\"", step,
                      "the step must compare pacman -Q with the expected list, not only print it")


if __name__ == "__main__":
    unittest.main()
