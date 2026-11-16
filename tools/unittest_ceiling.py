"""Runs a unittest suite under the ceiling of tools/test_shelf.json: each test runs at most the ceiling, and a shelved test is skipped.

ADR 0044 recorded reason: the verify recipe runs the Python suite under plain unittest on an image with no pytest (#820), where
pytest-timeout and tools/tests/conftest.py do not exist, so the ceiling and the shelf's skips need a runner of their own here,
and unittest's loader and result classes are Python.

    unittest_ceiling.py [--shelf FILE] discover -s DIR -p PATTERN

A test that is still running when the ceiling passes is named on standard error and the whole run ends with exit code 124
(unittest runs tests in the main thread and cannot interrupt one safely, so the run stops there, as timeout(1) would stop it;
the signal-based alternative would take SIGALRM from the tests that use it). A test the shelf names is reported skipped with
the entry's reason and date. Exit codes: 0 every test passed or was skipped, 1 a test failed, 2 the arguments or the shelf are wrong.
"""

import os
import pathlib
import sys
import threading
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import shelf_reader  # noqa: E402

EXIT_OVER_CEILING = 124
EXIT_USAGE = 2


def iter_tests(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from iter_tests(item)
        else:
            yield item


def shelve(suite, shelf: dict) -> int:
    """Marks every test of `suite` the shelf names as skipped, with the entry's reason; returns how many."""
    count = 0
    for test in iter_tests(suite):
        name = getattr(test, "_testMethodName", None)
        if name is None:
            continue
        unit = test.__class__.__module__.rsplit(".", 1)[-1]
        reason = shelf_reader.skip_reason(shelf, unit, test.__class__.__name__, name)
        if reason:
            setattr(test, name, unittest.skip(reason)(getattr(test, name)))
            count += 1
    return count


def result_class(ceiling_s: int):
    class CeilingResult(unittest.TextTestResult):
        timer = None

        def startTest(self, test):
            super().startTest(test)
            self.timer = threading.Timer(ceiling_s, self.expire, [test.id()])
            self.timer.daemon = True
            self.timer.start()

        def stopTest(self, test):
            if self.timer is not None:
                self.timer.cancel()
            super().stopTest(test)

        def expire(self, test_id):
            sys.stderr.write(f"unittest_ceiling: FAIL {test_id} ran longer than {ceiling_s} s: the run is stopped\n")
            sys.stderr.flush()
            os._exit(EXIT_OVER_CEILING)

    return CeilingResult


def main(argv: list) -> int:
    shelf_path = shelf_reader.SHELF_PATH
    if argv[:1] == ["--shelf"] and len(argv) >= 2:
        shelf_path, argv = pathlib.Path(argv[1]), argv[2:]
    if len(argv) != 5 or argv[0] != "discover" or argv[1] != "-s" or argv[3] != "-p":
        print("usage: unittest_ceiling.py [--shelf FILE] discover -s DIR -p PATTERN", file=sys.stderr)
        return EXIT_USAGE
    try:
        shelf = shelf_reader.load(shelf_path)
    except shelf_reader.ShelfError as exc:
        print(f"unittest_ceiling: FAIL {exc}", file=sys.stderr)
        return EXIT_USAGE
    suite = unittest.TestLoader().discover(argv[2], pattern=argv[4])
    shelved = shelve(suite, shelf)
    print(f"unittest_ceiling: ceiling {shelf['ceiling_s']} s per test; {shelved} test(s) shelved by {shelf_path.name}", file=sys.stderr)
    result = unittest.TextTestRunner(resultclass=result_class(shelf["ceiling_s"]), verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
