import unittest
import sys
import os

# Add the src directory to the path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from main import Compiler

class TestCompiler(unittest.TestCase):
    def setUp(self):
        self.compiler = Compiler()

    def test_simple_program(self):
        source = """
        let x 10
        let y 20
        let result x + y
        """

        try:
            assembly = self.compiler.compile_source(source)
            self.assertIn(".section .data", assembly)
            self.assertIn(".section .text", assembly)
            self.assertIn("_start:", assembly)
            self.assertIn("LI", assembly)
            self.assertIn("ADD", assembly)
        except Exception as e:
            self.fail(f"Compilation failed: {e}")

    def test_for_loop_compilation(self):
        source = """
        for i from 0 to 5 step 1 do
            let temp i * 2
        endfor
        """

        try:
            assembly = self.compiler.compile_source(source)
            self.assertIn("loop_start", assembly)
            self.assertIn("SLT", assembly)
            self.assertIn("BEQ", assembly)
            self.assertIn("JMP", assembly)
            self.assertIn("MUL", assembly)
        except Exception as e:
            self.fail(f"For loop compilation failed: {e}")

    def test_if_statement_compilation(self):
        source = """
        let x 10
        if x < 20 then
            let result 1
        else
            let result 0
        end
        """

        try:
            assembly = self.compiler.compile_source(source)
            self.assertIn("SLT", assembly)
            self.assertIn("BEQ", assembly)
            self.assertIn("else", assembly)
            self.assertIn("endif", assembly)
        except Exception as e:
            self.fail(f"If statement compilation failed: {e}")

    def test_vector_operations(self):
        source = """
        let addr 0x10000
        vector data = VLOAD(addr)
        VSTORE(addr, data)
        """

        try:
            assembly = self.compiler.compile_source(source)
            self.assertIn("LDV", assembly)
            self.assertIn("STV", assembly)
            self.assertIn("v0", assembly)
        except Exception as e:
            self.fail(f"Vector operations compilation failed: {e}")

if __name__ == '__main__':
    unittest.main()