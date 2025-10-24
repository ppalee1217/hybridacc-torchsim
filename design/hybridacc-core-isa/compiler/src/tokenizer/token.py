from enum import Enum, auto
from dataclasses import dataclass
from typing import Optional, Any

class TokenType(Enum):
    # Literals
    IDENTIFIER = auto()
    NUMBER = auto()
    STRING = auto()

    # Keywords
    LET = auto()
    FOR = auto()
    FROM = auto()
    TO = auto()
    STEP = auto()
    DO = auto()
    ENDFOR = auto()
    IF = auto()
    THEN = auto()
    ELSE = auto()
    END = auto()
    CONTINUE = auto()
    VECTOR = auto()

    # Operators
    ASSIGN = auto()      # =
    PLUS = auto()        # +
    MINUS = auto()       # -
    MULTIPLY = auto()    # *
    DIVIDE = auto()      # /
    MODULO = auto()      # %

    # Comparison
    EQ = auto()          # ==
    NE = auto()          # !=
    LT = auto()          # <
    LE = auto()          # <=
    GT = auto()          # >
    GE = auto()          # >=

    # Logical
    AND = auto()         # and
    OR = auto()          # or
    NOT = auto()         # not

    # Punctuation
    LPAREN = auto()      # (
    RPAREN = auto()      # )
    LBRACKET = auto()    # [
    RBRACKET = auto()    # ]
    LBRACE = auto()      # {
    RBRACE = auto()      # }
    COMMA = auto()       # ,
    SEMICOLON = auto()   # ;
    DOT = auto()         # .

    # Assembly Instructions
    ADD = auto()
    SUB = auto()
    MUL = auto()
    AND_OP = auto()
    OR_OP = auto()
    XOR = auto()
    SLT = auto()
    SLTU = auto()
    ADDI = auto()
    MULI = auto()
    LUI = auto()
    LDV = auto()
    STV = auto()
    LOOP = auto()
    JMP = auto()
    BEQ = auto()
    NOP = auto()
    LI = auto()
    MV = auto()
    J = auto()
    RET = auto()
    CALL = auto()

    # Functions
    VLOAD = auto()
    VSTORE = auto()

    # Special
    NEWLINE = auto()
    EOF = auto()
    COMMENT = auto()

    # Registers
    REGISTER = auto()    # x0, x1, ..., x31
    VECTOR_REG = auto()  # v0, v1

@dataclass
class Token:
    type: TokenType
    value: Any
    line: int
    column: int

    def __repr__(self) -> str:
        return f"Token({self.type}, {self.value!r}, {self.line}:{self.column})"