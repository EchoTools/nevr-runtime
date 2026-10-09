import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CXX = "x86_64-w64-mingw32-g++"


class HeaderIncludeOrder(unittest.TestCase):
    def test_ws_bridge_header_does_not_precede_winsock2(self):
        """#195: a header that pulls in <windows.h> without WIN32_LEAN_AND_MEAN drags in
        winsock.h, and a later <winsock2.h> then warns (-Wcpp) under MinGW and errors
        under MSVC. Compile ws_bridge.h on its own, then <winsock2.h>, with that
        warning promoted to an error."""
        with tempfile.TemporaryDirectory() as tmp:
            tu = Path(tmp) / "order.cpp"
            tu.write_text('#include "runtime/compat/ws_bridge.h"\n#include <winsock2.h>\n')
            proc = subprocess.run(
                [CXX, "-std=c++17", "-fsyntax-only", "-Werror=cpp", f"-I{ROOT / 'src'}", str(tu)],
                capture_output=True,
                text=True,
            )
        self.assertEqual(proc.returncode, 0, proc.stderr)


if __name__ == "__main__":
    unittest.main()
