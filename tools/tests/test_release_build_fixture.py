"""#745: a release build must not fetch the real signed fixture from a host outside GitHub."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import just_recipe  # noqa: E402


def test_the_verify_step_of_build_yml_marks_a_release_build():
    text = (ROOT / ".github/workflows/build.yml").read_text()
    step = re.search(r"- name: Verify\n(.*?)run: just verify", text, re.S)
    assert step, "no 'Verify' step running just verify"
    assert "NEVR_RELEASE_BUILD: ${{ steps.release.outputs.version != '' && '1' || '' }}" in step.group(1)


def test_test_auth_unit_fetches_only_outside_a_release_build():
    recipe = just_recipe.recipe((ROOT / "justfile").read_text(), "test-auth-unit")
    assert recipe, "no test-auth-unit recipe in the justfile"
    release = re.search(r'if \[\[ "\$\{NEVR_RELEASE_BUILD:-\}" == "1" \]\]; then(.*?)\n    else(.*?)\n    fi', recipe, re.S)
    assert release, "no NEVR_RELEASE_BUILD branch in test-auth-unit"
    assert "fetch_real_signed_dll" not in release.group(1)
    assert "GTEST_FILTER='-RealSigned.*'" in release.group(1)
    assert "fetch_real_signed_dll.py build/real-signed-cache" in release.group(2)
