
from typing import Dict, Any, List
import math

class MVPCompiler:
    """
    Minimum Viable Product Compiler/Scheduler for HybridAcc.
    Generates MMIO configurations for HDDU and AGUs based on tensor shapes and hardware constraints.
    """

    # AGU Register Offsets (match AddressGenerateUnit.hpp)
    REG_BASE_ADDR   = 0x00
    REG_BASE_ADDR_H = 0x04
    REG_ITER01      = 0x08
    REG_ITER23      = 0x0C
    REG_STRIDE0     = 0x10
    REG_STRIDE1     = 0x14
    REG_STRIDE2     = 0x18
    REG_STRIDE3     = 0x1C
    REG_CTRL        = 0x20
    REG_STATUS      = 0x24
    REG_LANE_CFG    = 0x28
    REG_TAG_BASE    = 0x40
    REG_TAG_STRIDE0 = 0x44
    REG_TAG_STRIDE1 = 0x48
    REG_TAG_CTRL    = 0x4C
    REG_MASK_CFG    = 0x54

    def __init__(self, hardware_config: Dict[str, Any]):
        self.num_pes = hardware_config.get("num_pes", 64)
        self.num_bus = hardware_config.get("num_bus", 4)
        self.spm_bank_size = hardware_config.get("spm_bank_size", 4096)

    def compile_conv2d(self,
                       input_shape: List[int],
                       weight_shape: List[int],
                       output_shape: List[int],
                       kernel_size: List[int],
                       stride: int,
                       padding: int,
                       dram_mapping: Dict[str, int]) -> Dict[str, Any]:
        """
        Generates firmware config for Conv2d.

        Args:
            input_shape: [N, H_in, W_in, C_in] (Actual data shape on DRAM for this wave)
            weight_shape: [OC, KH, KW, C_in]
            output_shape: [N, H_out, W_out, OC]
            kernel_size: [KH, KW]
            stride: int
            padding: int
            dram_mapping: Dict mapping buffer names to base addresses

        Returns:
            Dict containing 'hddu', 'agu', 'dma' configurations.
        """

        # Unpack shapes
        N, H_in, W_in, C_in = input_shape
        N_out, H_out, W_out, OC = output_shape
        KH, KW = kernel_size

        if N_out != N:
            raise ValueError(f"Batch size mismatch between input/output shapes: {N} vs {N_out}")

        # AGU Assignments
        # Bank 0: Weight (PS)
        # Bank 1: Activation (PD)
        # Bank 2: Partial Sum Input (PLI)
        # Bank 3: Partial Sum Output (PLO)

        configs = {}

        # --- 1. PS (Weight) Config ---
        # Logic from test_noc_sim:
        # Loop Order (Outer -> Inner): OCH -> KH -> KW -> ICH_chunk
        # One AGU transaction = 1 packed word (64 bits = 8 bytes).
        pkt_size = 8
        in_ch_pack = max(1, math.ceil(C_in / 4))
        out_ch_pack = max(1, math.ceil(OC / 4))

        configs["agu_0"] = self._gen_agu_config(
            base_addr=dram_mapping["weight"],
            iter0=in_ch_pack, stride0=pkt_size,
            iter1=KW, stride1=in_ch_pack * pkt_size,
            iter2=KH, stride2=KW * in_ch_pack * pkt_size,
            iter3=OC,  stride3=KH * KW * in_ch_pack * pkt_size,
            tag_base=0,
            tag_sel=1,   # Tie Tag to Loop 1 (KH)
            tag_stride=1
        )

        # --- 2. PD (Activation) Config ---
        # Logic from test_noc_sim:
        # Loop Order: IW (Outer) -> IH (Inner) -> ICH_chunk
        # Access pattern assumes we iterate W then H.

        configs["agu_1"] = self._gen_agu_config(
            base_addr=dram_mapping["activation"],
            iter0=in_ch_pack, stride0=pkt_size,
            iter1=H_in, stride1=W_in * in_ch_pack * pkt_size,
            iter2=W_in, stride2=in_ch_pack * pkt_size,
            iter3=1, stride3=0,
            tag_base=0,
            tag_sel=1, # Loop 1 (IH) drives tag
            tag_stride=1
        )

        # --- 3. PLI/PLO (Partial Sum) Config ---
        # Logic: OW (Outer) -> OH (Inner) -> OCH (Chunks)
        # Memory Layout: (OH, OW, OCH) -> Standard H,W,C.

        configs["agu_2"] = self._gen_agu_config(
            base_addr=dram_mapping["partial_sum"],
            iter0=out_ch_pack, stride0=pkt_size,
            iter1=H_out, stride1=W_out * out_ch_pack * pkt_size,
            iter2=W_out, stride2=out_ch_pack * pkt_size,
            iter3=1, stride3=0,
            tag_base=0,
            tag_sel=1, # Loop 1 (OH) drives tag
            tag_stride=1
        )

        configs["agu_3"] = self._gen_agu_config(
            base_addr=dram_mapping["output"],
            iter0=out_ch_pack, stride0=pkt_size,
            iter1=H_out, stride1=W_out * out_ch_pack * pkt_size,
            iter2=W_out, stride2=out_ch_pack * pkt_size,
            iter3=1, stride3=0,
            tag_base=0,
            tag_sel=1, # Loop 1 (OH) drives tag
            tag_stride=1
        )

        return {
            "hddu": {
                "base_addr": 0x1000,
                "global_ctrl": 0x800,
                "registers": {
                    0x800: 0x01, # Enable HDDU
                    0x808: 0xF,  # Enable all ports
                    0x80C: 0x0
                }
            },
            "agu": {
                "base_addr": 0x0000,
                "num_agus": 4,
                "configs": configs
            },
            "dma": {
                 "base_addr": 0x0000,
                 "transfers": []
            }
        }

    def _gen_agu_config(self,
                        base_addr: int,
                        iter0: int, stride0: int,
                        iter1: int, stride1: int,
                        iter2: int, stride2: int,
                        iter3: int, stride3: int,
                        tag_base: int = 0,
                        tag_sel: int = 0,
                        tag_stride: int = 1) -> Dict[str, int]:
        """
        Generates register values for a single AGU bank.
        """
        return {
            self.REG_BASE_ADDR: base_addr & 0xFFFFFFFF,
            self.REG_BASE_ADDR_H: (base_addr >> 32) & 0xFFFFFFFF,

            # Pack Iters: (High << 16) | Low
            self.REG_ITER01: ((iter1 & 0xFFFF) << 16) | (iter0 & 0xFFFF),
            self.REG_ITER23: ((iter3 & 0xFFFF) << 16) | (iter2 & 0xFFFF),

            self.REG_STRIDE0: stride0,
            self.REG_STRIDE1: stride1,
            self.REG_STRIDE2: stride2,
            self.REG_STRIDE3: stride3,

            self.REG_TAG_BASE: tag_base,
            self.REG_TAG_STRIDE0: tag_stride,
            self.REG_TAG_STRIDE1: tag_stride,
            self.REG_TAG_CTRL: tag_sel & 0xF,

            self.REG_CTRL: 0x1, # Start/Enable
            self.REG_LANE_CFG: 0x1,
            self.REG_MASK_CFG: 0xF
        }

    def _prod(self, shape: List[int]) -> int:
        res = 1
        for s in shape:
            res *= s
        return res

    def _gen_agu_linear_read(self, base_addr: int, size_bytes: int) -> Dict[str, int]:
        """Legacy helper for simple linear read."""
        count = size_bytes // 8
        return {
            self.REG_BASE_ADDR: base_addr,
            self.REG_ITER01: (0 << 16) | (max(0, count-1) & 0xFFFF),
            self.REG_STRIDE0: 8,
            self.REG_CTRL: 0x1,
        }

    def _gen_agu_linear_write(self, base_addr: int, size_bytes: int) -> Dict[str, int]:
        """Legacy helper for simple linear write."""
        count = size_bytes // 8
        return {
            self.REG_BASE_ADDR: base_addr,
            self.REG_ITER01: (0 << 16) | (max(0, count-1) & 0xFFFF),
            self.REG_STRIDE0: 8,
            self.REG_CTRL: 0x1,
        }
