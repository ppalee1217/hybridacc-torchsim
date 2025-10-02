from __future__ import annotations
from dataclasses import dataclass, asdict
from typing import Dict, Any
from .hardware import HardwareSpec, PEHardwareSpec, DEFAULT_HW_SPEC
from .gen_tiling import TilingResult
from .mapper import MappingResult
from .workloads import Conv2DWorkload, GemmWorkload

MAC_PER_SLICE = 12  # micro-kernel MAC 數 (per ofold element per cycle 假定)
PE_PER_ARRAY = 28   # 7x4

@dataclass
class AnalysisReport:
    macs: int
    latency_cycles: int
    energy_pj: float
    detail: Dict[str, Any]

    def to_dict(self):
        return asdict(self)

class CostModel:
    def __init__(self, hw:HardwareSpec|None=None):
        self.hw = hw or DEFAULT_HW_SPEC
        self.pe:PEHardwareSpec = self.hw.pe

    def _buffer_capacity_check(self, tiling:TilingResult) -> Dict[str,Any]:
        wl = tiling.workload
        gb = self.hw.gb
        gb_bytes_total = gb.count * gb.size_kb * 1024
        if wl.op == 'conv2d':
            wl_c:Conv2DWorkload = wl  # type: ignore
            Ho, Wo = wl_c.output_hw()
            ifm_bytes = wl_c.Cin * wl_c.Hin * wl_c.Win
            weight_bytes = wl_c.Cin * wl_c.Kh * wl_c.Kw * wl_c.Cout
            ofm_bytes = wl_c.Cout * Ho * Wo
        else:
            wl_g:GemmWorkload = wl  # type: ignore
            ifm_bytes = wl_g.M * wl_g.K
            weight_bytes = wl_g.K * wl_g.N
            ofm_bytes = wl_g.M * wl_g.N
        required = ifm_bytes + weight_bytes + ofm_bytes
        fits = required <= gb_bytes_total
        return {
            'global_buffer_total_bytes': gb_bytes_total,
            'required_estimate_bytes': required,
            'fits': fits,
            'ifm_bytes': ifm_bytes,
            'weight_bytes': weight_bytes,
            'ofm_bytes': ofm_bytes,
        }

    def estimate(self, tiling:TilingResult, mapping:MappingResult) -> AnalysisReport:
        wl = tiling.workload
        if wl.op=='conv2d':
            wl_c:Conv2DWorkload = wl  # type: ignore
            macs = wl_c.macs()
        else:
            wl_g:GemmWorkload = wl  # type: ignore
            macs = wl_g.macs()

        cols = min(mapping.horizontal_parallel_tiles, self.hw.array.cols)
        rows = tiling.num_reduction_slices
        arrays_active = len([g for g in mapping.assignment['groups'] if isinstance(g.get('array'), int)])
        active_pes = cols * rows * arrays_active
        total_pes = self.hw.array.cols * self.hw.array.rows * self.hw.array.arrays_per_cluster
        pe_util = active_pes / total_pes if total_pes else 0.0
        array_util = arrays_active / self.hw.array.arrays_per_cluster if self.hw.array.arrays_per_cluster else 0.0
        row_util = rows / self.hw.array.rows if self.hw.array.rows else 0.0
        col_util = cols / self.hw.array.cols if self.hw.array.cols else 0.0

        meta = mapping.assignment.get('meta', {})
        p_cin_sets = meta.get('P_cin_sets', 1)
        effective_waves = mapping.waves

        usable_mac_per_cycle = active_pes * self.pe.kc_fold  # 假設每 cycle 可完成 kc_fold MAC (簡化)
        core_cycles = macs // max(1, usable_mac_per_cycle) + (macs % max(1, usable_mac_per_cycle) > 0)
        chain_overhead = (rows - 1) * self.pe.ps_hop_latency
        wave_overhead = (effective_waves - 1) * self.pe.wave_transition_latency
        latency_cycles = core_cycles + chain_overhead + wave_overhead

        energy_mac = macs * self.pe.mac_energy_pj
        energy_ps = (rows - 1) * self.pe.ps_hop_energy_pj * arrays_active * cols * mapping.waves
        buf_check = self._buffer_capacity_check(tiling)
        reuse_ifm = 1.0
        reuse_weight = 1.0
        if wl.op=='conv2d':
            wl_c = wl  # type: ignore
            base_reuse_ifm = wl_c.Kh * wl_c.Kw / (wl_c.stride_h * wl_c.stride_w)
            p_ho_sets = meta.get('P_ho_sets', 1)
            if wl_c.stride_h == 1 and p_ho_sets > 1:
                overlap_unique_rows = wl_c.Kh + (p_ho_sets - 1)
                ideal_rows = wl_c.Kh * p_ho_sets
                vertical_factor = ideal_rows / overlap_unique_rows
            else:
                vertical_factor = 1.0
            reuse_ifm = base_reuse_ifm * vertical_factor
            reuse_weight = (tiling.Cout_fold if tiling.Cout_fold else self.pe.ofold) * p_cin_sets

        if wl.op=='conv2d':
            Ho, Wo = wl.output_hw()  # type: ignore
            ifm_bytes_full = wl.Cin * wl.Hin * wl.Win
            weight_bytes_full = wl.Cin * wl.Kh * wl.Kw * wl.Cout
            ofm_bytes_full = wl.Cout * Ho * Wo
        else:
            ifm_bytes_full = wl.M * wl.K
            weight_bytes_full = wl.K * wl.N
            ofm_bytes_full = wl.M * wl.N
        ifm_effective_reads = ifm_bytes_full / max(1.0, reuse_ifm)
        weight_effective_reads = weight_bytes_full / max(1.0, reuse_weight)
        ofm_effective_writes = ofm_bytes_full

        energy_ifm = ifm_effective_reads * self.pe.ifm_read_byte_energy_pj + ofm_effective_writes * self.pe.ofm_write_byte_energy_pj
        energy_weight = weight_effective_reads * self.pe.weight_read_byte_energy_pj
        energy_total = energy_mac + energy_ps + energy_ifm + energy_weight

        grid_meta = meta.get('grid', {})
        gr = grid_meta.get('grid_rows', 1)
        gc = grid_meta.get('grid_cols', self.hw.array.arrays_per_cluster)
        active_arrays = [g['array'] for g in mapping.assignment['groups'] if isinstance(g.get('array'), int)]
        row_active = [0]*gr
        col_active = [0]*gc
        for aid in active_arrays:
            r_idx = aid // gc if gc else 0
            c_idx = aid % gc if gc else 0
            if 0 <= r_idx < gr: row_active[r_idx]+=1
            if 0 <= c_idx < gc: col_active[c_idx]+=1
        grid_row_util = [a/gc for a in row_active] if gc else [1.0]*gr
        grid_col_util = [a/gr for a in col_active] if gr else [1.0]*gc

        detail = {
            'active_pes': active_pes,
            'total_pes': total_pes,
            'pe_utilization': pe_util,
            'array_utilization': array_util,
            'row_utilization': row_util,
            'col_utilization': col_util,
            'usable_mac_per_cycle': usable_mac_per_cycle,
            'core_cycles': core_cycles,
            'chain_overhead': chain_overhead,
            'wave_overhead': wave_overhead,
            'waves': effective_waves,
            'p_cin_sets': p_cin_sets,
            'energy_mac_pj': energy_mac,
            'energy_ps_pj': energy_ps,
            'energy_ifm_pj': energy_ifm,
            'energy_weight_pj': energy_weight,
            'reuse_ifm_est': reuse_ifm,
            'reuse_weight_est': reuse_weight,
            'ifm_effective_reads_bytes': ifm_effective_reads,
            'weight_effective_reads_bytes': weight_effective_reads,
            'ofm_effective_writes_bytes': ofm_effective_writes,
            'buffer_capacity': buf_check,
            'grid_row_active_counts': row_active,
            'grid_col_active_counts': col_active,
            'grid_row_utilization': grid_row_util,
            'grid_col_utilization': grid_col_util,
        }
        return AnalysisReport(macs=macs, latency_cycles=latency_cycles, energy_pj=energy_total, detail=detail)
