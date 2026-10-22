"""#745: a release build must not fetch the real signed fixture from a host outside GitHub."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_the_verify_step_of_build_yml_marks_a_release_build():
    text = (ROOT / ".github/workflows/build.yml").read_text()
    step = re.search(r"- name: Verify\n(.*?)run: just verify", text, re.S)
    assert step, "no 'Verify' step running just verify"
    assert "NEVR_RELEASE_BUILD: ${{ steps.release.outputs.version != '' && '1' || '' }}" in step.group(1)


def test_test_auth_unit_fetches_only_outside_a_release_build():
    text = (ROOT / "justfile").read_text()
    recipe = text[text.index("\ntest-auth-unit:"):]
    recipe = recipe[: recipe.index("\n\n", recipe.index("fetch_real_signed_dll"))]
    release = re.search(r'if \[\[ "\$\{NEVR_RELEASE_BUILD:-\}" == "1" \]\]; then(.*?)\n    else(.*?)\n    fi', recipe, re.S)
    assert release, "no NEVR_RELEASE_BUILD branch in test-auth-unit"
    assert "fetch_real_signed_dll" not in release.group(1)
    assert "GTEST_FILTER='-RealSigned.*'" in release.group(1)
    assert "fetch_real_signed_dll.py build/real-signed-cache" in release.group(2)
