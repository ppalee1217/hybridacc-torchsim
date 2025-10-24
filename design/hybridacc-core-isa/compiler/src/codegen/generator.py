from typing import List, Dict, Optional

# Handle both relative and absolute imports
try:
    from ..ast_nodes.nodes import *
except ImportError:
    from ast_nodes.nodes import *

class RegisterAllocator:
    """Advanced register allocator with register spilling support"""

    def __init__(self):
        # Use more registers - x4-x30 (leave x31 for emergency)
        self.registers = [f"x{i}" for i in range(4, 31)]
        self.used_registers = set()
        self.variable_registers = {}  # Maps variable names to registers
        self.spilled_variables = {}   # Maps variable names to memory offsets
        self.temp_counter = 0
        self.spill_counter = 0
        self.reserved_registers = {"x1", "x2", "x3", "x31"}  # Reserve more registers
        self.register_usage_count = {}
        self.temp_register_pool = []
        self.variable_priority = {}  # Track variable access frequency

    def allocate_register(self, var_name: Optional[str] = None) -> str:
        # If it's a variable and already has a register, reuse it
        if var_name and var_name in self.variable_registers:
            return self.variable_registers[var_name]

        # If variable is spilled, load it back
        if var_name and var_name in self.spilled_variables:
            reg = self._load_spilled_variable(var_name)
            return reg

        # For temporary registers, try to reuse from pool first
        if not var_name and self.temp_register_pool:
            reg = self.temp_register_pool.pop()
            self.used_registers.add(reg)
            return reg

        # Find an unused register
        for reg in self.registers:
            if reg not in self.used_registers:
                self.used_registers.add(reg)
                if var_name:
                    self.variable_registers[var_name] = reg
                    self.variable_priority[var_name] = self.variable_priority.get(var_name, 0) + 1
                return reg

        # If no free registers and it's a variable, try spilling
        if var_name:
            return self._spill_and_allocate(var_name)

        # For temporary registers, try to find least recently used
        best_reg = self._find_best_temp_register()
        if best_reg:
            self.used_registers.add(best_reg)
            return best_reg

        # Last resort: use emergency register
        emergency_reg = "x31"
        self.used_registers.add(emergency_reg)
        return emergency_reg

    def _spill_and_allocate(self, var_name: str) -> str:
        """Spill least important variable and allocate register for new variable"""
        # Find variable with lowest priority to spill
        min_priority = float('inf')
        victim_var = None
        victim_reg = None

        for var, reg in self.variable_registers.items():
            priority = self.variable_priority.get(var, 0)
            if priority < min_priority:
                min_priority = priority
                victim_var = var
                victim_reg = reg

        if victim_var and victim_reg:
            # Spill the victim variable to memory
            self.spill_counter += 1
            self.spilled_variables[victim_var] = self.spill_counter * 4  # 4-byte aligned

            # Remove from register allocation
            del self.variable_registers[victim_var]

            # Allocate the freed register to new variable
            self.variable_registers[var_name] = victim_reg
            self.variable_priority[var_name] = self.variable_priority.get(var_name, 0) + 1

            return victim_reg

        # If can't spill, use a simple mapping strategy
        simple_reg = f"x{4 + (hash(var_name) % 20)}"  # Map to x4-x23
        self.variable_registers[var_name] = simple_reg
        return simple_reg

    def _load_spilled_variable(self, var_name: str) -> str:
        """Load a spilled variable back into a register"""
        # Find a register for the spilled variable
        reg = self.allocate_register()  # This will get a temp register

        # Move it to variable register mapping
        if reg in self.temp_register_pool:
            self.temp_register_pool.remove(reg)

        self.variable_registers[var_name] = reg
        del self.spilled_variables[var_name]

        return reg

    def _find_best_temp_register(self) -> Optional[str]:
        """Find the best temporary register to reuse"""
        for reg in self.registers:
            # Check if this register is not used by any variable
            is_variable_reg = any(reg == var_reg for var_reg in self.variable_registers.values())
            if not is_variable_reg:
                return reg
        return None

    def free_register(self, reg: str):
        """Free a register back to the pool"""
        if (reg and reg.startswith("x") and
            reg not in self.reserved_registers and
            reg not in self.variable_registers.values()):
            self.used_registers.discard(reg)
            if reg not in self.temp_register_pool:
                self.temp_register_pool.append(reg)

    def get_temp_register(self) -> str:
        """Get a temporary register"""
        self.temp_counter += 1
        return self.allocate_register()

    def free_temp_register(self, reg: str):
        """Free a temporary register more aggressively"""
        if (reg and reg.startswith("x") and
            reg not in self.reserved_registers and
            reg not in self.variable_registers.values()):
            self.used_registers.discard(reg)
            if reg not in self.temp_register_pool:
                self.temp_register_pool.append(reg)

    def reset_temp_pool(self):
        """Reset temporary register pool at the end of each statement"""
        # Don't clear completely, just remove duplicates
        self.temp_register_pool = list(set(self.temp_register_pool))

    def get_register_stats(self):
        """Get register allocation statistics for debugging"""
        return {
            'total_registers': len(self.registers),
            'used_registers': len(self.used_registers),
            'variable_registers': len(self.variable_registers),
            'spilled_variables': len(self.spilled_variables),
            'temp_pool_size': len(self.temp_register_pool),
            'available_registers': len(self.registers) - len(self.used_registers)
        }

class CodeGenerator(ASTVisitor):
    """Code generator that converts AST to RISC-V assembly"""

    def __init__(self):
        self.code = []
        self.data_section = []
        self.register_allocator = RegisterAllocator()
        self.constants = {}
        self.loop_labels = []
        self.label_counter = 0
        self.vector_variables = {}  # Track vector variables and their associated vector registers

    def generate_label(self, prefix: str = "L") -> str:
        self.label_counter += 1
        return f"{prefix}{self.label_counter}"

    def emit(self, instruction: str):
        """Emit an assembly instruction"""
        self.code.append(f"    {instruction}")

    def emit_label(self, label: str):
        """Emit a label"""
        self.code.append(f"{label}:")

    def emit_comment(self, comment: str):
        """Emit a comment"""
        self.code.append(f"    # {comment}")

    def add_constant(self, name: str, value: int):
        """Add a constant to the data section"""
        self.constants[name] = value
        self.data_section.append(f"{name}:          .word {value}")

    def visit_program(self, node: ProgramNode) -> str:
        # Generate data section
        self.code.append(".section .data")
        self.code.append("# Constants")

        # Add predefined constants
        predefined_constants = {
            "WEIGHT_BASE_ADDR": 0x00000000,
            "INPUT_BASE_ADDR": 0x00010000,
            "OUTPUT_BASE_ADDR": 0x00020000,
            "TEMP_BUFFER_ADDR": 0x00030000,
            "NOC_PS_OFFSET": 0x00,
            "NOC_PD_OFFSET": 0x40,
            "NOC_PI_OFFSET": 0x80,
            "NOC_PO_OFFSET": 0xc0,
            "CORE_ROWS": 4,
            "CORE_COLS": 16,
            "KERNEL_SIZE": 3,
            "STRIDE": 1,
            "PADDING": 1,
            "CONV2D_PASS_CO": 64,
            "CONV2D_PASS_CI_K3": 4,
            "CONV2D_PASS_HO": 16,
            "CONV2D_PASS_HI": 18,
            "CONV2D_PASS_WO": 200,
            "CONV2D_PASS_WI": 202
        }

        for name, value in predefined_constants.items():
            self.add_constant(name, value)

        self.code.extend(self.data_section)
        self.code.append("")

        # Generate text section
        self.code.append(".section .text")
        self.code.append(".global _start")
        self.code.append("")
        self.code.append("_start:")

        # Load constants into registers
        self.emit_comment("Load constants into registers")
        self.emit("LUI x1, 0x00000      # WEIGHT_BASE_ADDR high")
        self.emit("LUI x2, 0x00001      # INPUT_BASE_ADDR high")
        self.emit("LUI x3, 0x00002      # OUTPUT_BASE_ADDR high")
        self.code.append("")

        # Process statements with better register management
        for i, stmt in enumerate(node.statements):
            self.emit_comment(f"Statement {i+1}")
            stmt.accept(self)
            # Reset temporary register pool after each top-level statement
            self.register_allocator.reset_temp_pool()

            # Add register stats as comment for debugging
            stats = self.register_allocator.get_register_stats()
            self.emit_comment(f"Registers: {stats['used_registers']}/{stats['total_registers']} used, {stats['temp_pool_size']} in pool")

        # End program
        self.code.append("")
        self.emit_label("program_end")
        self.emit("NOP")

        return "\n".join(self.code)

    def visit_variable_declaration(self, node: VariableDeclarationNode):
        self.emit_comment(f"Variable declaration: {node.name}")

        # Evaluate the value expression
        value_reg = node.value.accept(self)

        # Handle vector variables differently
        if node.var_type == "vector":
            # For vector variables, we don't allocate a general register
            # Vector data stays in vector registers (v0, v1)
            # The variable name is just a symbolic reference to the vector register
            self.emit_comment(f"Vector variable {node.name} uses vector register {value_reg}")
            self.vector_variables[node.name] = value_reg
            return

        # For regular variables, allocate a general register
        var_reg = self.register_allocator.allocate_register(node.name)

        # Only emit MV if both are general registers
        if value_reg != var_reg and value_reg.startswith("x") and var_reg.startswith("x"):
            self.emit(f"MV {var_reg}, {value_reg}")
            if value_reg.startswith("x") and value_reg not in self.register_allocator.variable_registers.values():
                self.register_allocator.free_register(value_reg)
        elif value_reg.startswith("v"):
            # If the value is in a vector register, we can't move it to a general register
            # Just document that this variable refers to the vector register
            self.emit_comment(f"Variable {node.name} refers to vector register {value_reg}")
        else:
            # Direct assignment case
            pass

    def visit_for_loop(self, node: ForLoopNode):
        loop_start_label = self.generate_label("loop_start")
        loop_end_label = self.generate_label("loop_end")
        loop_continue_label = self.generate_label("loop_continue")

        self.loop_labels.append((loop_continue_label, loop_end_label))

        self.emit_comment(f"For loop: {node.variable}")

        # Initialize loop variable
        start_reg = node.start.accept(self)
        loop_var_reg = self.register_allocator.allocate_register(node.variable)

        if start_reg != loop_var_reg:
            self.emit(f"MV {loop_var_reg}, {start_reg}")

        # Evaluate end condition
        end_reg = node.end.accept(self)

        # Loop start
        self.emit_label(loop_start_label)

        # Check loop condition
        temp_reg = self.register_allocator.get_temp_register()
        self.emit(f"SLT {temp_reg}, {loop_var_reg}, {end_reg}")
        self.emit(f"BEQ {temp_reg}, x0, {loop_end_label}")

        # Loop body
        for stmt in node.body:
            stmt.accept(self)

        # Continue label (for continue statements)
        self.emit_label(loop_continue_label)

        # Increment loop variable
        step_reg = node.step.accept(self)
        self.emit(f"ADD {loop_var_reg}, {loop_var_reg}, {step_reg}")

        # Jump back to loop start
        self.emit(f"JMP {loop_start_label}")

        # Loop end
        self.emit_label(loop_end_label)

        # Clean up
        self.register_allocator.free_register(temp_reg)
        if end_reg.startswith("_temp"):
            self.register_allocator.free_register(end_reg)
        if step_reg.startswith("_temp"):
            self.register_allocator.free_register(step_reg)

        self.loop_labels.pop()

    def visit_if_statement(self, node: IfStatementNode):
        else_label = self.generate_label("else")
        end_label = self.generate_label("endif")

        self.emit_comment("If statement")

        # Evaluate condition
        condition_reg = node.condition.accept(self)

        # Branch to else if condition is false
        self.emit(f"BEQ {condition_reg}, x0, {else_label}")

        # Then body
        for stmt in node.then_body:
            stmt.accept(self)

        # Jump to end
        self.emit(f"JMP {end_label}")

        # Else label
        self.emit_label(else_label)

        # Else body
        if node.else_body:
            for stmt in node.else_body:
                stmt.accept(self)

        # End label
        self.emit_label(end_label)

        # Clean up
        if condition_reg.startswith("_temp"):
            self.register_allocator.free_register(condition_reg)

    def visit_assignment(self, node: AssignmentNode):
        self.emit_comment(f"Assignment: {node.target}")

        # Evaluate value
        value_reg = node.value.accept(self)

        # Get or allocate register for target
        target_reg = self.register_allocator.allocate_register(node.target)

        if value_reg != target_reg:
            self.emit(f"MV {target_reg}, {value_reg}")
            if value_reg.startswith("_temp"):
                self.register_allocator.free_register(value_reg)

    def visit_binary_operation(self, node: BinaryOperationNode) -> str:
        left_reg = node.left.accept(self)
        right_reg = node.right.accept(self)
        result_reg = self.register_allocator.get_temp_register()

        if node.operator == '+':
            self.emit(f"ADD {result_reg}, {left_reg}, {right_reg}")
        elif node.operator == '-':
            self.emit(f"SUB {result_reg}, {left_reg}, {right_reg}")
        elif node.operator == '*':
            self.emit(f"MUL {result_reg}, {left_reg}, {right_reg}")
        elif node.operator == '<':
            self.emit(f"SLT {result_reg}, {left_reg}, {right_reg}")
        elif node.operator == '>':
            # Implement > as (right < left)
            self.emit(f"SLT {result_reg}, {right_reg}, {left_reg}")
        elif node.operator == '>=':
            # Implement >= as !(left < right)
            temp_reg = self.register_allocator.get_temp_register()
            self.emit(f"SLT {temp_reg}, {left_reg}, {right_reg}")
            self.emit(f"XOR {result_reg}, {temp_reg}, 1")
            self.register_allocator.free_temp_register(temp_reg)
        elif node.operator == '<=':
            # Implement <= as !(right < left)
            temp_reg = self.register_allocator.get_temp_register()
            self.emit(f"SLT {temp_reg}, {right_reg}, {left_reg}")
            self.emit(f"XOR {result_reg}, {temp_reg}, 1")
            self.register_allocator.free_temp_register(temp_reg)
        elif node.operator == '==':
            # Implement == as !(a < b) AND !(b < a)
            temp1_reg = self.register_allocator.get_temp_register()
            temp2_reg = self.register_allocator.get_temp_register()
            self.emit(f"SLT {temp1_reg}, {left_reg}, {right_reg}")
            self.emit(f"SLT {temp2_reg}, {right_reg}, {left_reg}")
            self.emit(f"OR {result_reg}, {temp1_reg}, {temp2_reg}")
            self.emit(f"XOR {result_reg}, {result_reg}, 1")
            self.register_allocator.free_temp_register(temp1_reg)
            self.register_allocator.free_temp_register(temp2_reg)
        elif node.operator == '!=':
            # Implement != as (a < b) OR (b < a)
            temp1_reg = self.register_allocator.get_temp_register()
            temp2_reg = self.register_allocator.get_temp_register()
            self.emit(f"SLT {temp1_reg}, {left_reg}, {right_reg}")
            self.emit(f"SLT {temp2_reg}, {right_reg}, {left_reg}")
            self.emit(f"OR {result_reg}, {temp1_reg}, {temp2_reg}")
            self.register_allocator.free_temp_register(temp1_reg)
            self.register_allocator.free_temp_register(temp2_reg)
        elif node.operator == 'and':
            self.emit(f"AND {result_reg}, {left_reg}, {right_reg}")
        elif node.operator == 'or':
            self.emit(f"OR {result_reg}, {left_reg}, {right_reg}")
        else:
            raise ValueError(f"Unsupported binary operator: {node.operator}")

        # Free temporary registers more aggressively
        if left_reg and left_reg.startswith("x") and left_reg not in self.register_allocator.variable_registers.values():
            self.register_allocator.free_temp_register(left_reg)
        if right_reg and right_reg.startswith("x") and right_reg not in self.register_allocator.variable_registers.values():
            self.register_allocator.free_temp_register(right_reg)

        return result_reg

    def visit_unary_operation(self, node: UnaryOperationNode) -> str:
        operand_reg = node.operand.accept(self)
        result_reg = self.register_allocator.get_temp_register()

        if node.operator == '-':
            self.emit(f"SUB {result_reg}, x0, {operand_reg}")
        elif node.operator == 'not':
            self.emit(f"XOR {result_reg}, {operand_reg}, 1")
        else:
            raise ValueError(f"Unsupported unary operator: {node.operator}")

        if operand_reg.startswith("_temp"):
            self.register_allocator.free_register(operand_reg)

        return result_reg

    def visit_function_call(self, node: FunctionCallNode) -> str:
        if node.name == "VLOAD":
            # Handle vector load - return vector register directly
            addr_reg = node.arguments[0].accept(self)
            self.emit(f"LDV v0, 0({addr_reg})")
            # Clean up address register if it's temporary
            if addr_reg.startswith("x") and addr_reg not in self.register_allocator.variable_registers.values():
                self.register_allocator.free_temp_register(addr_reg)
            return "v0"  # Return vector register identifier

        elif node.name == "VSTORE":
            # Handle vector store
            addr_reg = node.arguments[0].accept(self)
            if len(node.arguments) > 1:
                data_reg = node.arguments[1].accept(self)
                # If data_reg is a vector register, use it directly
                if data_reg.startswith("v"):
                    self.emit(f"STV {data_reg}, 0({addr_reg})")
                else:
                    # If it's a general register, we need special handling
                    # For now, assume v0 contains the data
                    self.emit(f"STV v0, 0({addr_reg})")
            else:
                # Default to v0
                self.emit(f"STV v0, 0({addr_reg})")

            # Clean up address register if it's temporary
            if addr_reg.startswith("x") and addr_reg not in self.register_allocator.variable_registers.values():
                self.register_allocator.free_temp_register(addr_reg)
            return "v0"

        else:
            raise ValueError(f"Unsupported function: {node.name}")

    def visit_identifier(self, node: IdentifierNode) -> str:
        if node.name in self.register_allocator.variable_registers:
            return self.register_allocator.variable_registers[node.name]
        elif node.name in self.vector_variables:
            return self.vector_variables[node.name]
        elif node.name in self.constants:
            # Load constant value
            result_reg = self.register_allocator.get_temp_register()
            value = self.constants[node.name]
            if value <= 2047 and value >= -2048:  # 12-bit immediate
                self.emit(f"LI {result_reg}, {value}")
            else:
                # Load large constant
                high = (value >> 12) & 0xFFFFF
                low = value & 0xFFF
                if low > 2047:
                    high += 1
                    low -= 4096
                self.emit(f"LUI {result_reg}, {high}")
                if low != 0:
                    self.emit(f"ADDI {result_reg}, {result_reg}, {low}")
            return result_reg
        else:
            raise ValueError(f"Unknown identifier: {node.name}")

    def visit_literal(self, node: LiteralNode) -> str:
        result_reg = self.register_allocator.get_temp_register()

        if node.literal_type == "number":
            value = int(node.value)
            if value <= 2047 and value >= -2048:  # 12-bit immediate
                self.emit(f"LI {result_reg}, {value}")
            else:
                # Load large constant
                high = (value >> 12) & 0xFFFFF
                low = value & 0xFFF
                if low > 2047:
                    high += 1
                    low -= 4096
                self.emit(f"LUI {result_reg}, {high}")
                if low != 0:
                    self.emit(f"ADDI {result_reg}, {result_reg}, {low}")
        else:
            raise ValueError(f"Unsupported literal type: {node.literal_type}")

        return result_reg

    def visit_block(self, node: BlockNode):
        for stmt in node.statements:
            stmt.accept(self)

    def visit_continue_statement(self, node: ContinueStatementNode):
        if self.loop_labels:
            continue_label, _ = self.loop_labels[-1]
            self.emit(f"JMP {continue_label}")
        else:
            raise ValueError("Continue statement outside of loop")

    def visit_assembly_instruction(self, node: AssemblyInstructionNode):
        operands = " ".join(node.operands)
        self.emit(f"{node.opcode} {operands}")