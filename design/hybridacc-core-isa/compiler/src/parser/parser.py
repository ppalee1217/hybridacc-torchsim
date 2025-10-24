from typing import List, Optional, Union

# Handle both relative and absolute imports
try:
    from ..tokenizer.token import Token, TokenType
    from ..ast_nodes.nodes import *
except ImportError:
    from tokenizer.token import Token, TokenType
    from ast_nodes.nodes import *

class ParseError(Exception):
    """Exception raised when parsing fails"""
    pass

class Parser:
    def __init__(self, tokens: List[Token]):
        self.tokens = tokens
        self.current = 0

    def is_at_end(self) -> bool:
        return self.peek().type == TokenType.EOF

    def peek(self) -> Token:
        return self.tokens[self.current]

    def previous(self) -> Token:
        return self.tokens[self.current - 1]

    def advance(self) -> Token:
        if not self.is_at_end():
            self.current += 1
        return self.previous()

    def check(self, token_type: TokenType) -> bool:
        if self.is_at_end():
            return False
        return self.peek().type == token_type

    def match(self, *types: TokenType) -> bool:
        for token_type in types:
            if self.check(token_type):
                self.advance()
                return True
        return False

    def consume(self, token_type: TokenType, message: str) -> Token:
        if self.check(token_type):
            return self.advance()

        current_token = self.peek()
        raise ParseError(f"{message} at line {current_token.line}, column {current_token.column}")

    def synchronize(self):
        """Recover from parse errors by finding the next statement boundary"""
        self.advance()

        while not self.is_at_end():
            if self.previous().type in [TokenType.NEWLINE, TokenType.SEMICOLON]:
                return

            if self.peek().type in [TokenType.LET, TokenType.FOR, TokenType.IF, TokenType.VECTOR, TokenType.ENDFOR, TokenType.END]:
                return

            self.advance()

    def parse(self) -> ProgramNode:
        statements = []

        while not self.is_at_end():
            # Skip newlines, comments, and semicolons
            if self.match(TokenType.NEWLINE, TokenType.COMMENT, TokenType.SEMICOLON):
                continue

            try:
                stmt = self.statement()
                if stmt:
                    statements.append(stmt)
                    # Optionally consume semicolon after statement
                    self.match(TokenType.SEMICOLON)
            except ParseError as e:
                print(f"Parse error: {e}")
                self.synchronize()

        return ProgramNode(statements)

    def statement(self) -> Optional[ASTNode]:
        if self.match(TokenType.LET):
            return self.variable_declaration()

        if self.match(TokenType.VECTOR):
            return self.vector_declaration()

        if self.match(TokenType.FOR):
            return self.for_statement()

        if self.match(TokenType.IF):
            return self.if_statement()

        if self.match(TokenType.CONTINUE):
            return ContinueStatementNode()

        return self.expression_statement()

    def variable_declaration(self) -> VariableDeclarationNode:
        name_token = self.consume(TokenType.IDENTIFIER, "Expected variable name")
        name = name_token.value

        var_type = None

        # Check for assignment operator or direct value
        if self.match(TokenType.ASSIGN):
            value = self.expression()
        else:
            # Check if next token might be a type annotation
            if self.check(TokenType.IDENTIFIER) and self.peek().value == "vector":
                self.advance()  # consume 'vector'
                var_type = "vector"

            value = self.expression()

        return VariableDeclarationNode(name, value, var_type)

    def vector_declaration(self) -> VariableDeclarationNode:
        name_token = self.consume(TokenType.IDENTIFIER, "Expected vector variable name")
        name = name_token.value

        self.consume(TokenType.ASSIGN, "Expected '=' after vector variable name")
        value = self.expression()

        return VariableDeclarationNode(name, value, "vector")

    def for_statement(self) -> ForLoopNode:
        variable_token = self.consume(TokenType.IDENTIFIER, "Expected loop variable name")
        variable = variable_token.value

        self.consume(TokenType.FROM, "Expected 'from' after loop variable")
        start = self.expression()

        self.consume(TokenType.TO, "Expected 'to' after start expression")
        end = self.expression()

        step = LiteralNode(1, "number")  # Default step
        if self.match(TokenType.STEP):
            step = self.expression()

        self.consume(TokenType.DO, "Expected 'do' after loop header")

        body = []
        while not self.check(TokenType.ENDFOR) and not self.is_at_end():
            if self.match(TokenType.NEWLINE, TokenType.COMMENT):
                continue

            stmt = self.statement()
            if stmt:
                body.append(stmt)

        self.consume(TokenType.ENDFOR, "Expected 'endfor' to close loop")

        return ForLoopNode(variable, start, end, step, body)

    def if_statement(self) -> IfStatementNode:
        condition = self.expression()

        self.consume(TokenType.THEN, "Expected 'then' after if condition")

        then_body = []
        while (not self.check(TokenType.ELSE) and
               not self.check(TokenType.END) and
               not self.is_at_end()):
            if self.match(TokenType.NEWLINE, TokenType.COMMENT):
                continue

            stmt = self.statement()
            if stmt:
                then_body.append(stmt)

        else_body = None
        if self.match(TokenType.ELSE):
            else_body = []
            while not self.check(TokenType.END) and not self.is_at_end():
                if self.match(TokenType.NEWLINE, TokenType.COMMENT):
                    continue

                stmt = self.statement()
                if stmt:
                    else_body.append(stmt)

        self.consume(TokenType.END, "Expected 'end' to close if statement")

        return IfStatementNode(condition, then_body, else_body)

    def expression_statement(self) -> Optional[ASTNode]:
        expr = self.expression()
        return expr

    def expression(self) -> ASTNode:
        return self.assignment()

    def assignment(self) -> ASTNode:
        expr = self.logical_or()

        if self.match(TokenType.ASSIGN):
            value = self.assignment()

            if isinstance(expr, IdentifierNode):
                return AssignmentNode(expr.name, value)

            raise ParseError("Invalid assignment target")

        return expr

    def logical_or(self) -> ASTNode:
        expr = self.logical_and()

        while self.match(TokenType.OR):
            operator = self.previous().value
            right = self.logical_and()
            expr = BinaryOperationNode(expr, operator, right)

        return expr

    def logical_and(self) -> ASTNode:
        expr = self.equality()

        while self.match(TokenType.AND):
            operator = self.previous().value
            right = self.equality()
            expr = BinaryOperationNode(expr, operator, right)

        return expr

    def equality(self) -> ASTNode:
        expr = self.comparison()

        while self.match(TokenType.EQ, TokenType.NE):
            operator = self.previous().value
            right = self.comparison()
            expr = BinaryOperationNode(expr, operator, right)

        return expr

    def comparison(self) -> ASTNode:
        expr = self.term()

        while self.match(TokenType.GT, TokenType.GE, TokenType.LT, TokenType.LE):
            operator = self.previous().value
            right = self.term()
            expr = BinaryOperationNode(expr, operator, right)

        return expr

    def term(self) -> ASTNode:
        expr = self.factor()

        while self.match(TokenType.MINUS, TokenType.PLUS):
            operator = self.previous().value
            right = self.factor()
            expr = BinaryOperationNode(expr, operator, right)

        return expr

    def factor(self) -> ASTNode:
        expr = self.unary()

        while self.match(TokenType.DIVIDE, TokenType.MULTIPLY, TokenType.MODULO):
            operator = self.previous().value
            right = self.unary()
            expr = BinaryOperationNode(expr, operator, right)

        return expr

    def unary(self) -> ASTNode:
        if self.match(TokenType.NOT, TokenType.MINUS):
            operator = self.previous().value
            right = self.unary()
            return UnaryOperationNode(operator, right)

        return self.call()

    def call(self) -> ASTNode:
        expr = self.primary()

        while True:
            if self.match(TokenType.LPAREN):
                expr = self.finish_call(expr)
            else:
                break

        return expr

    def finish_call(self, callee: ASTNode) -> ASTNode:
        arguments = []

        if not self.check(TokenType.RPAREN):
            arguments.append(self.expression())
            while self.match(TokenType.COMMA):
                arguments.append(self.expression())

        self.consume(TokenType.RPAREN, "Expected ')' after arguments")

        if isinstance(callee, IdentifierNode):
            return FunctionCallNode(callee.name, arguments)

        raise ParseError("Can only call functions")

    def primary(self) -> ASTNode:
        if self.match(TokenType.NUMBER):
            return LiteralNode(self.previous().value, "number")

        if self.match(TokenType.STRING):
            return LiteralNode(self.previous().value, "string")

        if self.match(TokenType.IDENTIFIER):
            return IdentifierNode(self.previous().value)

        if self.match(TokenType.LPAREN):
            expr = self.expression()
            self.consume(TokenType.RPAREN, "Expected ')' after expression")
            return expr

        # Handle function calls like VLOAD, VSTORE
        if self.match(TokenType.VLOAD, TokenType.VSTORE):
            func_name = self.previous().value
            self.consume(TokenType.LPAREN, f"Expected '(' after {func_name}")

            arguments = []
            if not self.check(TokenType.RPAREN):
                arguments.append(self.expression())
                while self.match(TokenType.COMMA):
                    arguments.append(self.expression())

            self.consume(TokenType.RPAREN, "Expected ')' after function arguments")
            return FunctionCallNode(func_name, arguments)

        raise ParseError(f"Unexpected token {self.peek().type} at line {self.peek().line}")