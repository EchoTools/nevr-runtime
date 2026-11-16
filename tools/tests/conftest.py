"""pytest's hook for the test shelf (tools/test_shelf.json): a test the shelf names is skipped with the entry's reason and date.

ADR 0044 recorded reason: pytest reads conftest.py, which must be Python, and the shelf's Python reader (tools/shelf_reader.py) is imported here.

No test file carries a skip of its own for a shelved test: the skip is generated from the shelf, in one place.
"""

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import shelf_reader  # noqa: E402


def pytest_collection_modifyitems(config, items):
    shelf = shelf_reader.load()
    for item in items:
        unit = pathlib.Path(str(item.path)).stem
        cls = item.cls.__name__ if item.cls is not None else None
        reason = shelf_reader.skip_reason(shelf, unit, cls, getattr(item, "originalname", None) or item.name)
        if reason:
            item.add_marker(pytest.mark.skip(reason=reason))
