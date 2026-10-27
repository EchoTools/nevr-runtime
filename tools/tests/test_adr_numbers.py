"""Every record in docs/adr/ has its own number (#801).

Two records with one number make "ADR 0008" ambiguous in every citation, so the check fails on any repeated number.
"""

import pathlib
import re
from collections import Counter

ADR_DIR = pathlib.Path(__file__).resolve().parents[2] / "docs" / "adr"
RECORD = re.compile(r"^(\d{4})-.+\.md$")


def test_each_adr_number_names_one_record():
    numbers = Counter()
    for path in ADR_DIR.glob("*.md"):
        match = RECORD.match(path.name)
        if match:
            numbers[match.group(1)] += 1
    repeated = sorted(number for number, count in numbers.items() if count > 1)
    assert not repeated, f"ADR numbers used by more than one record in docs/adr/: {repeated}"
