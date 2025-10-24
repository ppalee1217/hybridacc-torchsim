import unittest
import sys
import os

# Add the src directory to the path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from tokenizer.tokenizer import Tokenizer
from tokenizer.token import TokenType

class TestTokenizer(unittest.TestCase):
    def setUp(self):
        self.tokenizer = None

    def test_simple_variable_declaration(self):
        source = "let x 10"
        self.tokenizer = Tokenizer(source)
        tokens = self.tokenizer.tokenize()

        self.assertEqual(tokens[0].type, TokenType.LET)
        self.assertEqual(tokens[1].type, TokenType.IDENTIFIER)
        self.assertEqual(tokens[1].value, "x")
        self.assertEqual(tokens[2].type, TokenType.NUMBER)
        self.assertEqual(tokens[2].value, 10)
        self.assertEqual(tokens[3].type, TokenType.EOF)

    def test_hex_numbers(self):
        source = "let addr 0x00010000"
        self.tokenizer = Tokenizer(source)
        tokens = self.tokenizer.tokenize()

        self.assertEqual(tokens[2].type, TokenType.NUMBER)
        self.assertEqual(tokens[2].value, 0x00010000)

    def test_for_loop(self):
        source = "for i from 0 to 10 step 1 do\nendfor"
        self.tokenizer = Tokenizer(source)
        tokens = self.tokenizer.tokenize()

        # Filter out newlines for easier testing
        tokens = [t for t in tokens if t.type != TokenType.NEWLINE]

        expected_types = [
            TokenType.FOR, TokenType.IDENTIFIER, TokenType.FROM,
            TokenType.NUMBER, TokenType.TO, TokenType.NUMBER,
            TokenType.STEP, TokenType.NUMBER, TokenType.DO,
            TokenType.ENDFOR, TokenType.EOF
        ]

        for i, expected_type in enumerate(expected_types):
            self.assertEqual(tokens[i].type, expected_type)

    def test_function_call(self):
        source = "VLOAD(dm_addr)"
        self.tokenizer = Tokenizer(source)
        tokens = self.tokenizer.tokenize()

        self.assertEqual(tokens[0].type, TokenType.VLOAD)
        self.assertEqual(tokens[1].type, TokenType.LPAREN)
        self.assertEqual(tokens[2].type, TokenType.IDENTIFIER)
        self.assertEqual(tokens[3].type, TokenType.RPAREN)

    def test_register_recognition(self):
        source = "x1 x31 v0 v1"
        self.tokenizer = Tokenizer(source)
        tokens = self.tokenizer.tokenize()

        self.assertEqual(tokens[0].type, TokenType.REGISTER)
        self.assertEqual(tokens[0].value, "x1")
        self.assertEqual(tokens[1].type, TokenType.REGISTER)
        self.assertEqual(tokens[1].value, "x31")
        self.assertEqual(tokens[2].type, TokenType.VECTOR_REG)
        self.assertEqual(tokens[2].value, "v0")
        self.assertEqual(tokens[3].type, TokenType.VECTOR_REG)
        self.assertEqual(tokens[3].value, "v1")

if __name__ == '__main__':
    unittest.main()