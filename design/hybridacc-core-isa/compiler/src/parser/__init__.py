"""
Parser module for Core-ISA Compiler
Handles syntax analysis and AST generation
"""

from .parser import Parser, ParseError

__all__ = ['Parser', 'ParseError']