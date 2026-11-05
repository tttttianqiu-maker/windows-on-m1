import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "m1n1_windows" / "proxyclient"))

from m1n1.chainload_stub import STUB_WORDS, ChainloadStub  # noqa: E402

ASM = """1:
ldp x4, x5, [x1], #16
stp x4, x5, [x2]
dc cvau, x2
ic ivau, x2
add x2, x2, #16
sub x3, x3, #16
cbnz x3, 1b
ldr x1, 2f
br x1
nop
2: .quad 0x1122334455667788
"""


class ChainloadStubTest(unittest.TestCase):
    def test_layout(self):
        stub = ChainloadStub(0x1122334455667788, 0x10003800000)
        self.assertEqual(stub.len, 48)
        self.assertEqual(struct.unpack_from("<Q", stub.data, 40)[0], 0x1122334455667788)
        with self.assertRaises(ValueError):
            ChainloadStub(0, 0x10003800004)

    @unittest.skipUnless(shutil.which("llvm-mc") or shutil.which("llvm-mc-18"), "no llvm-mc")
    def test_matches_llvm(self):
        mc = shutil.which("llvm-mc") or shutil.which("llvm-mc-18")
        objcopy = shutil.which("llvm-objcopy") or shutil.which("llvm-objcopy-18")
        with tempfile.TemporaryDirectory() as d:
            s, o, b = Path(d, "s.s"), Path(d, "s.o"), Path(d, "s.bin")
            s.write_text(ASM)
            subprocess.run([mc, "-triple=aarch64", "-filetype=obj", str(s), "-o", str(o)], check=True)
            subprocess.run([objcopy, "-O", "binary", "-j", ".text", str(o), str(b)], check=True)
            self.assertEqual(struct.unpack("<10I", b.read_bytes()[:40]), STUB_WORDS)
            self.assertEqual(b.read_bytes(), ChainloadStub(0x1122334455667788, 0).data)


if __name__ == "__main__":
    unittest.main()
