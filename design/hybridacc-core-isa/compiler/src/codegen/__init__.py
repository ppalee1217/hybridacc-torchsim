"""
Code generation module for Core-ISA Compiler
Handles code generation from AST to RISC-V assembly
"""

from .generator import CodeGenerator, RegisterAllocator

__all__ = ['CodeGenerator', 'RegisterAllocator']