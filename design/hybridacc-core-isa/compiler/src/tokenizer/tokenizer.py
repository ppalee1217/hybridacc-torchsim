import re
from typing import List, Iterator, Optional
from .token import Token, TokenType

class Tokenizer:
    def __init__(self, text: str):
        self.text = text
        self.pos = 0
        self.line = 1
        self.column = 1
        self.tokens: List[Token] = []

        # Keywords mapping
        self.keywords = {
            'let': TokenType.LET,
            'for': TokenType.FOR,
            'from': TokenType.FROM,
            'to': TokenType.TO,
            'step': TokenType.STEP,
            'do': TokenType.DO,
            'endfor': TokenType.ENDFOR,
            'if': TokenType.IF,
            'then': TokenType.THEN,
            'else': TokenType.ELSE,
            'end': TokenType.END,
            'continue': TokenType.CONTINUE,
            'vector': TokenType.VECTOR,
            'and': TokenType.AND,
            'or': TokenType.OR,
            'not': TokenType.NOT,
            # Assembly instructions
            'ADD': TokenType.ADD,
            'SUB': TokenType.SUB,
            'MUL': TokenType.MUL,
            'AND': TokenType.AND_OP,
            'OR': TokenType.OR_OP,
            'XOR': TokenType.XOR,
            'SLT': TokenType.SLT,
            'SLTU': TokenType.SLTU,
            'ADDI': TokenType.ADDI,
            'MULI': TokenType.MULI,
            'LUI': TokenType.LUI,
            'LDV': TokenType.LDV,
            'STV': TokenType.STV,
            'LOOP': TokenType.LOOP,
            'JMP': TokenType.JMP,
            'BEQ': TokenType.BEQ,
            'NOP': TokenType.NOP,
            'LI': TokenType.LI,
            'MV': TokenType.MV,
            'J': TokenType.J,
            'RET': TokenType.RET,
            'CALL': TokenType.CALL,
            'VLOAD': TokenType.VLOAD,
            'VSTORE': TokenType.VSTORE,
        }

    def current_char(self) -> Optional[str]:
        if self.pos >= len(self.text):
            return None
        return self.text[self.pos]

    def peek_char(self, offset: int = 1) -> Optional[str]:
        peek_pos = self.pos + offset
        if peek_pos >= len(self.text):
            return None
        return self.text[peek_pos]

    def advance(self):
        if self.pos < len(self.text) and self.text[self.pos] == '\n':
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        self.pos += 1

    def skip_whitespace(self):
        while self.current_char() and self.current_char() in ' \t\r':
            self.advance()

    def read_number(self) -> Token:
        start_line, start_col = self.line, self.column
        num_str = ''

        # Handle hex numbers (0x prefix)
        if self.current_char() == '0' and self.peek_char() and self.peek_char().lower() == 'x':
            num_str += self.current_char()
            self.advance()
            num_str += self.current_char()
            self.advance()

            while self.current_char() and self.current_char() in '0123456789abcdefABCDEF':
                num_str += self.current_char()
                self.advance()

            return Token(TokenType.NUMBER, int(num_str, 16), start_line, start_col)

        # Handle decimal numbers
        while self.current_char() and (self.current_char().isdigit() or self.current_char() == '.'):
            num_str += self.current_char()
            self.advance()

        # Convert to appropriate type
        if '.' in num_str:
            value = float(num_str)
        else:
            value = int(num_str)

        return Token(TokenType.NUMBER, value, start_line, start_col)

    def read_identifier(self) -> Token:
        start_line, start_col = self.line, self.column
        identifier = ''

        while (self.current_char() and
               (self.current_char().isalnum() or self.current_char() in '_')):
            identifier += self.current_char()
            self.advance()

        # Check if it's a register
        if re.match(r'^x([0-9]|[12][0-9]|3[01])$', identifier):
            return Token(TokenType.REGISTER, identifier, start_line, start_col)
        elif re.match(r'^v[01]$', identifier):
            return Token(TokenType.VECTOR_REG, identifier, start_line, start_col)

        # Check if it's a keyword
        token_type = self.keywords.get(identifier, TokenType.IDENTIFIER)
        return Token(token_type, identifier, start_line, start_col)

    def read_string(self) -> Token:
        start_line, start_col = self.line, self.column
        quote_char = self.current_char()
        self.advance()  # Skip opening quote

        string_value = ''
        while self.current_char() and self.current_char() != quote_char:
            if self.current_char() == '\\':
                self.advance()
                if self.current_char() == 'n':
                    string_value += '\n'
                elif self.current_char() == 't':
                    string_value += '\t'
                elif self.current_char() == 'r':
                    string_value += '\r'
                elif self.current_char() == '\\':
                    string_value += '\\'
                elif self.current_char() == quote_char:
                    string_value += quote_char
                else:
                    string_value += self.current_char()
            else:
                string_value += self.current_char()
            self.advance()

        if self.current_char() == quote_char:
            self.advance()  # Skip closing quote
        else:
            raise SyntaxError(f"Unterminated string at line {start_line}, column {start_col}")

        return Token(TokenType.STRING, string_value, start_line, start_col)

    def read_comment(self) -> Token:
        start_line, start_col = self.line, self.column
        comment = ''

        # Skip // or #
        if self.current_char() == '/' and self.peek_char() == '/':
            self.advance()
            self.advance()
        elif self.current_char() == '#':
            self.advance()

        while self.current_char() and self.current_char() != '\n':
            comment += self.current_char()
            self.advance()

        return Token(TokenType.COMMENT, comment.strip(), start_line, start_col)

    def tokenize(self) -> List[Token]:
        while self.pos < len(self.text):
            self.skip_whitespace()

            if not self.current_char():
                break

            char = self.current_char()
            start_line, start_col = self.line, self.column

            # Numbers
            if char.isdigit():
                self.tokens.append(self.read_number())

            # Identifiers and keywords
            elif char.isalpha() or char == '_':
                self.tokens.append(self.read_identifier())

            # Strings
            elif char in '"\'':
                self.tokens.append(self.read_string())

            # Comments
            elif char == '/' and self.peek_char() == '/':
                self.tokens.append(self.read_comment())
            elif char == '#':
                self.tokens.append(self.read_comment())

            # Two-character operators
            elif char == '=' and self.peek_char() == '=':
                self.tokens.append(Token(TokenType.EQ, '==', start_line, start_col))
                self.advance()
                self.advance()
            elif char == '!' and self.peek_char() == '=':
                self.tokens.append(Token(TokenType.NE, '!=', start_line, start_col))
                self.advance()
                self.advance()
            elif char == '<' and self.peek_char() == '=':
                self.tokens.append(Token(TokenType.LE, '<=', start_line, start_col))
                self.advance()
                self.advance()
            elif char == '>' and self.peek_char() == '=':
                self.tokens.append(Token(TokenType.GE, '>=', start_line, start_col))
                self.advance()
                self.advance()

            # Single-character tokens
            elif char == '=':
                self.tokens.append(Token(TokenType.ASSIGN, '=', start_line, start_col))
                self.advance()
            elif char == '+':
                self.tokens.append(Token(TokenType.PLUS, '+', start_line, start_col))
                self.advance()
            elif char == '-':
                self.tokens.append(Token(TokenType.MINUS, '-', start_line, start_col))
                self.advance()
            elif char == '*':
                self.tokens.append(Token(TokenType.MULTIPLY, '*', start_line, start_col))
                self.advance()
            elif char == '/':
                self.tokens.append(Token(TokenType.DIVIDE, '/', start_line, start_col))
                self.advance()
            elif char == '%':
                self.tokens.append(Token(TokenType.MODULO, '%', start_line, start_col))
                self.advance()
            elif char == '<':
                self.tokens.append(Token(TokenType.LT, '<', start_line, start_col))
                self.advance()
            elif char == '>':
                self.tokens.append(Token(TokenType.GT, '>', start_line, start_col))
                self.advance()
            elif char == '(':
                self.tokens.append(Token(TokenType.LPAREN, '(', start_line, start_col))
                self.advance()
            elif char == ')':
                self.tokens.append(Token(TokenType.RPAREN, ')', start_line, start_col))
                self.advance()
            elif char == '[':
                self.tokens.append(Token(TokenType.LBRACKET, '[', start_line, start_col))
                self.advance()
            elif char == ']':
                self.tokens.append(Token(TokenType.RBRACKET, ']', start_line, start_col))
                self.advance()
            elif char == '{':
                self.tokens.append(Token(TokenType.LBRACE, '{', start_line, start_col))
                self.advance()
            elif char == '}':
                self.tokens.append(Token(TokenType.RBRACE, '}', start_line, start_col))
                self.advance()
            elif char == ',':
                self.tokens.append(Token(TokenType.COMMA, ',', start_line, start_col))
                self.advance()
            elif char == ';':
                self.tokens.append(Token(TokenType.SEMICOLON, ';', start_line, start_col))
                self.advance()
            elif char == '.':
                self.tokens.append(Token(TokenType.DOT, '.', start_line, start_col))
                self.advance()
            elif char == '\n':
                self.tokens.append(Token(TokenType.NEWLINE, '\n', start_line, start_col))
                self.advance()

            else:
                raise SyntaxError(f"Unexpected character '{char}' at line {start_line}, column {start_col}")

        self.tokens.append(Token(TokenType.EOF, None, self.line, self.column))
        return self.tokens