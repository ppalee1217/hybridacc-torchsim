import unittest
import sys
import os

# Add the src directory to the path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from tokenizer.tokenizer import Tokenizer
from parser.parser import Parser
from ast_nodes.nodes import *

class TestParser(unittest.TestCase):
    def setUp(self):
        self.parser = None

    def parse_source(self, source):
        tokenizer = Tokenizer(source)
        tokens = tokenizer.tokenize()
        # Filter out comments
        filtered_tokens = [token for token in tokens if token.type.name != 'COMMENT']
        self.parser = Parser(filtered_tokens)
        return self.parser.parse()

    def test_variable_declaration(self):
        source = "let x 42"
        ast = self.parse_source(source)

        self.assertIsInstance(ast, ProgramNode)
        self.assertEqual(len(ast.statements), 1)

        stmt = ast.statements[0]
        self.assertIsInstance(stmt, VariableDeclarationNode)
        self.assertEqual(stmt.name, "x")
        self.assertIsInstance(stmt.value, LiteralNode)
        self.assertEqual(stmt.value.value, 42)

    def test_for_loop(self):
        source = """
        for i from 0 to 10 step 1 do
            let temp i + 1
        endfor
        """
        ast = self.parse_source(source)

        self.assertIsInstance(ast, ProgramNode)
        self.assertEqual(len(ast.statements), 1)

        loop = ast.statements[0]
        self.assertIsInstance(loop, ForLoopNode)
        self.assertEqual(loop.variable, "i")
        self.assertIsInstance(loop.start, LiteralNode)
        self.assertEqual(loop.start.value, 0)
        self.assertIsInstance(loop.end, LiteralNode)
        self.assertEqual(loop.end.value, 10)
        self.assertEqual(len(loop.body), 1)

    def test_if_statement(self):
        source = """
        if x < 10 then
            let y 1
        else
            let y 0
        end
        """
        ast = self.parse_source(source)

        stmt = ast.statements[0]
        self.assertIsInstance(stmt, IfStatementNode)
        self.assertIsInstance(stmt.condition, BinaryOperationNode)
        self.assertEqual(stmt.condition.operator, "<")
        self.assertEqual(len(stmt.then_body), 1)
        self.assertEqual(len(stmt.else_body), 1)

    def test_binary_operations(self):
        source = "let result a + b * c"
        ast = self.parse_source(source)

        stmt = ast.statements[0]
        self.assertIsInstance(stmt, VariableDeclarationNode)

        # Should parse as a + (b * c) due to operator precedence
        expr = stmt.value
        self.assertIsInstance(expr, BinaryOperationNode)
        self.assertEqual(expr.operator, "+")
        self.assertIsInstance(expr.left, IdentifierNode)
        self.assertEqual(expr.left.name, "a")
        self.assertIsInstance(expr.right, BinaryOperationNode)
        self.assertEqual(expr.right.operator, "*")

    def test_function_call(self):
        source = "let data VLOAD(address)"
        ast = self.parse_source(source)

        stmt = ast.statements[0]
        self.assertIsInstance(stmt.value, FunctionCallNode)
        self.assertEqual(stmt.value.name, "VLOAD")
        self.assertEqual(len(stmt.value.arguments), 1)
        self.assertIsInstance(stmt.value.arguments[0], IdentifierNode)

if __name__ == '__main__':
    unittest.main()