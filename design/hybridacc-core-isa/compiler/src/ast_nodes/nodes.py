from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import List, Optional, Any, Union
from enum import Enum

class ASTNodeType(Enum):
    PROGRAM = "program"
    VARIABLE_DECLARATION = "variable_declaration"
    FOR_LOOP = "for_loop"
    IF_STATEMENT = "if_statement"
    ASSIGNMENT = "assignment"
    BINARY_OPERATION = "binary_operation"
    UNARY_OPERATION = "unary_operation"
    FUNCTION_CALL = "function_call"
    IDENTIFIER = "identifier"
    LITERAL = "literal"
    BLOCK = "block"
    CONTINUE_STATEMENT = "continue_statement"
    ASSEMBLY_INSTRUCTION = "assembly_instruction"

class ASTNode(ABC):
    """Base class for all AST nodes"""
    
    @abstractmethod
    def accept(self, visitor):
        pass
    
    @property
    @abstractmethod
    def node_type(self) -> ASTNodeType:
        pass

@dataclass
class ProgramNode(ASTNode):
    statements: List[ASTNode]
    
    def accept(self, visitor):
        return visitor.visit_program(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.PROGRAM

@dataclass
class VariableDeclarationNode(ASTNode):
    name: str
    value: ASTNode
    var_type: Optional[str] = None
    
    def accept(self, visitor):
        return visitor.visit_variable_declaration(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.VARIABLE_DECLARATION

@dataclass
class ForLoopNode(ASTNode):
    variable: str
    start: ASTNode
    end: ASTNode
    step: ASTNode
    body: List[ASTNode]
    
    def accept(self, visitor):
        return visitor.visit_for_loop(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.FOR_LOOP

@dataclass
class IfStatementNode(ASTNode):
    condition: ASTNode
    then_body: List[ASTNode]
    else_body: Optional[List[ASTNode]] = None
    
    def accept(self, visitor):
        return visitor.visit_if_statement(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.IF_STATEMENT

@dataclass
class AssignmentNode(ASTNode):
    target: str
    value: ASTNode
    
    def accept(self, visitor):
        return visitor.visit_assignment(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.ASSIGNMENT

@dataclass
class BinaryOperationNode(ASTNode):
    left: ASTNode
    operator: str
    right: ASTNode
    
    def accept(self, visitor):
        return visitor.visit_binary_operation(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.BINARY_OPERATION

@dataclass
class UnaryOperationNode(ASTNode):
    operator: str
    operand: ASTNode
    
    def accept(self, visitor):
        return visitor.visit_unary_operation(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.UNARY_OPERATION

@dataclass
class FunctionCallNode(ASTNode):
    name: str
    arguments: List[ASTNode]
    
    def accept(self, visitor):
        return visitor.visit_function_call(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.FUNCTION_CALL

@dataclass
class IdentifierNode(ASTNode):
    name: str
    
    def accept(self, visitor):
        return visitor.visit_identifier(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.IDENTIFIER

@dataclass
class LiteralNode(ASTNode):
    value: Any
    literal_type: str  # 'number', 'string', 'boolean'
    
    def accept(self, visitor):
        return visitor.visit_literal(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.LITERAL

@dataclass
class BlockNode(ASTNode):
    statements: List[ASTNode]
    
    def accept(self, visitor):
        return visitor.visit_block(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.BLOCK

@dataclass
class ContinueStatementNode(ASTNode):
    
    def accept(self, visitor):
        return visitor.visit_continue_statement(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.CONTINUE_STATEMENT

@dataclass
class AssemblyInstructionNode(ASTNode):
    opcode: str
    operands: List[str]
    
    def accept(self, visitor):
        return visitor.visit_assembly_instruction(self)
    
    @property
    def node_type(self) -> ASTNodeType:
        return ASTNodeType.ASSEMBLY_INSTRUCTION

# Visitor pattern interface
class ASTVisitor(ABC):
    """Abstract base class for AST visitors"""
    
    @abstractmethod
    def visit_program(self, node: ProgramNode):
        pass
    
    @abstractmethod
    def visit_variable_declaration(self, node: VariableDeclarationNode):
        pass
    
    @abstractmethod
    def visit_for_loop(self, node: ForLoopNode):
        pass
    
    @abstractmethod
    def visit_if_statement(self, node: IfStatementNode):
        pass
    
    @abstractmethod
    def visit_assignment(self, node: AssignmentNode):
        pass
    
    @abstractmethod
    def visit_binary_operation(self, node: BinaryOperationNode):
        pass
    
    @abstractmethod
    def visit_unary_operation(self, node: UnaryOperationNode):
        pass
    
    @abstractmethod
    def visit_function_call(self, node: FunctionCallNode):
        pass
    
    @abstractmethod
    def visit_identifier(self, node: IdentifierNode):
        pass
    
    @abstractmethod
    def visit_literal(self, node: LiteralNode):
        pass
    
    @abstractmethod
    def visit_block(self, node: BlockNode):
        pass
    
    @abstractmethod
    def visit_continue_statement(self, node: ContinueStatementNode):
        pass
    
    @abstractmethod
    def visit_assembly_instruction(self, node: AssemblyInstructionNode):
        pass