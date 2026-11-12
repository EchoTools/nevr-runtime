"""#1441: the SDK's address registry describes the PC binary's two functions the way the binary does.

`sdk/include/nevr/pc/address_registry.h` gives a `Signature:` comment for each published address, and a plugin author
hooks or calls the address with that type. The disassembly of `echovr.exe` (sha256 b6d08277e5846900…) contradicts two
of them:

- VA_SET_CAN_BE_GRABBED (0x140909f10) is an element destructor. It reads one argument (`this`); its second argument is
  never read. Calling it as a setter destroys five sub-objects of `this`.
- VA_PRECISION_SLEEP_WAIT (0x1401CE0B0) reads one argument (`microseconds`, in rcx). rdx and r8 are the function's own
  locals, not arguments.

These checks read the registry and count the parameters in each `Signature:` line for the two addresses, and fail if a
comment again gives a different count.
"""

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
REGISTRY = REPO / "sdk" / "include" / "nevr" / "pc" / "address_registry.h"

# The argument count the PC binary reads, from the disassembly (see the module docstring and #1441).
BINARY_ARG_COUNT = {
    "VA_SET_CAN_BE_GRABBED": 1,
    "VA_PRECISION_SLEEP_WAIT": 1,
}


def signature_for(text: str, name: str) -> str:
    """The `Signature:` line of the block that ends with `static constexpr uint64_t <name> = ...;`."""
    lines = text.splitlines()
    decl = re.compile(r"static constexpr uint64_t " + re.escape(name) + r"\s*=")
    for i, line in enumerate(lines):
        if decl.search(line):
            for j in range(i - 1, max(-1, i - 12), -1):
                if "Signature:" in lines[j]:
                    return lines[j]
                if lines[j].strip() == "" or lines[j].lstrip().startswith("static constexpr"):
                    break
            raise AssertionError(f"{name}: no Signature: comment above its declaration")
    raise AssertionError(f"{name}: not declared in {REGISTRY.relative_to(REPO)}")


def arg_count(signature: str) -> int:
    """Count the parameters between the first '(' and its matching ')' in a Signature: line."""
    body = signature.split("Signature:", 1)[1]
    start = body.index("(")
    depth = 0
    for k in range(start, len(body)):
        if body[k] == "(":
            depth += 1
        elif body[k] == ")":
            depth -= 1
            if depth == 0:
                inner = body[start + 1 : k].strip()
                if inner in ("", "void"):
                    return 0
                return inner.count(",") + 1
    raise AssertionError(f"unbalanced parentheses in: {signature!r}")


class AddressRegistrySignatureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = REGISTRY.read_text(encoding="utf-8")

    def test_each_checked_address_has_the_binary_argument_count(self):
        for name, expected in BINARY_ARG_COUNT.items():
            with self.subTest(name=name):
                sig = signature_for(self.text, name)
                self.assertEqual(
                    arg_count(sig),
                    expected,
                    f"{name}: the Signature: comment gives {arg_count(sig)} argument(s), the binary reads {expected}: {sig.strip()}",
                )

    def test_set_can_be_grabbed_is_not_described_as_a_setter(self):
        block_start = self.text.index("static constexpr uint64_t VA_SET_CAN_BE_GRABBED")
        block = self.text[max(0, block_start - 600) : block_start]
        self.assertNotIn("Setter", block, "VA_SET_CAN_BE_GRABBED is an element destructor, not a setter (#1441)")
        self.assertIn("destructor", block.lower(), "VA_SET_CAN_BE_GRABBED's comment must say what the function is (#1441)")

    def test_arg_count_parses_the_shapes_the_registry_uses(self):
        self.assertEqual(arg_count("// Signature: void(int64_t microseconds)"), 1)
        self.assertEqual(arg_count("// Signature: void(int64_t a, int64_t b, LARGE_INTEGER* c)"), 3)
        self.assertEqual(arg_count("// Signature: void __fastcall (void* this, bool canBeGrabbed)"), 2)
        self.assertEqual(arg_count("// Signature: void(void)"), 0)
        self.assertEqual(arg_count("// Signature: float(void* root, const char* path, float d, int32_t r)"), 4)


if __name__ == "__main__":
    unittest.main()
