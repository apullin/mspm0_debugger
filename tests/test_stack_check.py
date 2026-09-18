"""Regression tests for the C1104 build-time stack-budget gate."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_stack import (analyze, canonical, check_budget, library_frame,
                         parse_code, parse_symbols, read_frames)


class StackCheckTests(unittest.TestCase):
    def test_old_budget_fails_and_shared_buffer_budget_passes(self):
        with self.assertRaisesRegex(ValueError, "budget exceeded"):
            check_budget(424, 344, 64)
        check_budget(432, 512, 64)
        with self.assertRaisesRegex(ValueError, "budget exceeded"):
            check_budget(456, 512, 64)

    def test_compiler_clones_and_bounded_frames(self):
        self.assertEqual(canonical("foo.constprop.12"), "foo.constprop")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "file.su"
            path.write_text("x.c:1:1:foo\t16\tstatic\nx.c:2:1:bar\t32\tdynamic,bounded\n")
            self.assertEqual(read_frames(directory), {"foo": 16, "bar": 32})
            path.write_text("x.c:1:1:foo\t16\tdynamic\n")
            with self.assertRaisesRegex(ValueError, "unbounded"):
                read_frames(directory)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "no .su"):
                read_frames(directory)

    def test_linked_calls_tail_branches_and_library_frames(self):
        # Address-based resolution is essential: objdump can annotate a
        # branch with an unrelated absolute linker symbol, not a function.
        code = parse_code("""00000010 <Reset_Handler>:
  10: b510       push {r4, lr}
  12: f000 f815  bl 40 <worker.constprop.0>
  16: bd10       pop {r4, pc}
00000040 <worker.constprop.0>:
  40: b500       push {lr}
  42: b089       sub sp, #36
  44: f000 f810  bl 80 <lib>
  48: b009       add sp, #36
  4a: bd00       pop {pc}
00000080 <lib>:
  80: b530       push {r4, r5, lr}
  82: b083       sub sp, #12
  84: e70e       b.n a2 <_Min_Stack_Size+0x2>
000000a0 <leaf>:
  a0: b500       push {lr}
  a2: 2000       movs r0, #0
  a4: bd00       pop {pc}
""")
        symbols = parse_symbols("00000010 W Reset_Handler\n20000400 B __StackTop\n")
        depth, path = analyze(code, {"Reset_Handler": 8, "worker.constprop": 40}, symbols)
        self.assertEqual(depth, 8 + 40 + 24 + 4)
        self.assertIn("worker.constprop(40)", path)
        self.assertIn("lib(24) -> leaf(4)", path)

    def test_unknown_or_repeated_stack_adjustments_fail(self):
        for instruction in ((0x10, "mov", "sp, r3"), (0x10, "sub", "sp, r3")):
            with self.assertRaisesRegex(ValueError, "unsupported stack"):
                library_frame("bad", [instruction])
        with self.assertRaisesRegex(ValueError, "loop crosses"):
            library_frame("loop", [(0x10, "push", "{r4, lr}"),
                                   (0x12, "b.n", "10 <loop>")])

    def test_recursion_and_indirect_calls_fail(self):
        symbols = {"Reset_Handler": 0x10}
        for instruction, reason in (((0x10, "bl", "10 <Reset_Handler>"), "recursive"),
                                    ((0x10, "blx", "r3"), "indirect"),
                                    ((0x10, "bx", "r3"), "indirect")):
            with self.assertRaisesRegex(ValueError, reason):
                analyze({0x10: ("Reset_Handler", [instruction])}, {"Reset_Handler": 8}, symbols)

    def test_only_empty_startup_constructor_loops_are_exempt(self):
        code = {0x10: ("Reset_Handler", [(0x10, "blx", "r3"), (0x12, "blx", "r3")])}
        symbols = {"Reset_Handler": 0x10, "__preinit_array_start": 0x20,
                   "__preinit_array_end": 0x20, "__init_array_start": 0x20,
                   "__init_array_end": 0x20}
        self.assertEqual(analyze(code, {"Reset_Handler": 8}, symbols)[0], 8)
        symbols["__init_array_end"] = 0x24
        with self.assertRaisesRegex(ValueError, "indirect"):
            analyze(code, {"Reset_Handler": 8}, symbols)

    def test_custom_interrupt_requires_a_model(self):
        code = {0x10: ("Reset_Handler", [(0x10, "bx", "lr")])}
        symbols = {"Reset_Handler": 0x10, "Default_Handler": 0x20, "UART0_IRQHandler": 0x40}
        with self.assertRaisesRegex(ValueError, "interrupt handler"):
            analyze(code, {"Reset_Handler": 8}, symbols)


if __name__ == "__main__":
    unittest.main()
