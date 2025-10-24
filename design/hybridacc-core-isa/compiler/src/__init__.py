"""
Core-ISA Compiler Package
A compiler for the Core-ISA language targeting RISC-V RV32I assembly
"""

__version__ = "0.1.0"
__author__ = "HybridAcc Team"

try:
    from .main import Compiler, main
    from .tokenizer.tokenizer import Tokenizer
    from .parser.parser import Parser
    from .codegen.generator import CodeGenerator
except ImportError:
    from main import Compiler, main
    from tokenizer.tokenizer import Tokenizer
    from parser.parser import Parser
    from codegen.generator import CodeGenerator

__all__ = [
    'Compiler',
    'main',
    'Tokenizer',
    'Parser',
    'CodeGenerator'
]