from __future__ import annotations
from dataclasses import dataclass, asdict
from typing import Dict, Any

@dataclass
class PEHardwareSpec:
    kc_fold: int = 12              # reduction micro-kernel (Cin*Kh*Kw linearized)
    ofold: int = 16                # parallel output channels per PE
    # GEMM 專用 tile
    gemm_m_fold: int = 8
    gemm_n_fold: int = 12
    gemm_k_fold: int = 32
    # 容量 (原 reg_file_bytes → pe_sram_size)
    pe_sram_size: int = 256
    # 能耗參數 (pJ)
    mac_energy_pj: float = 1.2
    ifm_read_byte_energy_pj: float = 8.0
    ifm_write_byte_energy_pj: float = 8.0
    weight_read_byte_energy_pj: float = 8.0
    ofm_write_byte_energy_pj: float = 9.0
    ps_hop_energy_pj: float = 0.2
    ps_hop_latency: int = 2        # cycles per hop along chain
    wave_transition_latency: int = 20  # flush / refill cost cycles

    def to_dict(self) -> Dict[str,Any]:
        return asdict(self)

@dataclass
class GlobalBufferSpec:
    count: int = 6
    size_kb: int = 256
    read_byte_energy_pj: float = 8.0
    write_byte_energy_pj: float = 8.0

    def to_dict(self):
        return asdict(self)

@dataclass
class ArrayHardwareSpec:
    rows: int = 7
    cols: int = 8
    arrays_per_cluster: int = 3

    def to_dict(self):
        return {
            'rows': self.rows,
            'cols': self.cols,
            'arrays_per_cluster': self.arrays_per_cluster,
        }

@dataclass
class HardwareSpec:
    pe: PEHardwareSpec = PEHardwareSpec()
    array: ArrayHardwareSpec = ArrayHardwareSpec()
    gb: GlobalBufferSpec = GlobalBufferSpec()

    def to_dict(self):
        return {'pe': self.pe.to_dict(), 'array': self.array.to_dict(), 'gb': self.gb.to_dict()}

DEFAULT_HW_SPEC = HardwareSpec()

def hardware_spec_from_config(d:Dict[str,Any]) -> HardwareSpec:
    hw = d.get('hardware', d)
    pe_cfg = hw.get('pe', {})
    gb_cfg = hw.get('buffers', {}).get('global', {}) if 'buffers' in hw else hw.get('gb', {})
    array_cfg_raw = hw.get('array_shape', hw.get('array', {}))
    array_grid = hw.get('array_grid', hw.get('array_topology', None))  # 可用 array_grid: [r,c]

    if isinstance(array_cfg_raw, dict):
        array_cfg = {
            'rows': array_cfg_raw.get('rows', array_cfg_raw.get('r', DEFAULT_HW_SPEC.array.rows)),
            'cols': array_cfg_raw.get('cols', array_cfg_raw.get('c', DEFAULT_HW_SPEC.array.cols)),
            'arrays_per_cluster': hw.get('arrays_per_cluster', DEFAULT_HW_SPEC.array.arrays_per_cluster),
            'grid_rows': array_cfg_raw.get('grid_rows', DEFAULT_HW_SPEC.array.grid_rows),
            'grid_cols': array_cfg_raw.get('grid_cols', DEFAULT_HW_SPEC.array.grid_cols),
        }
    elif isinstance(array_cfg_raw, (list, tuple)) and len(array_cfg_raw) >= 2:
        array_cfg = {'rows': array_cfg_raw[0], 'cols': array_cfg_raw[1], 'arrays_per_cluster': hw.get('arrays_per_cluster', DEFAULT_HW_SPEC.array.arrays_per_cluster), 'grid_rows': DEFAULT_HW_SPEC.array.grid_rows, 'grid_cols': DEFAULT_HW_SPEC.array.grid_cols}
    else:
        array_cfg = DEFAULT_HW_SPEC.array.to_dict()

    # 若外部指定 array_grid 覆寫 grid_rows/grid_cols
    if isinstance(array_grid, (list, tuple)) and len(array_grid) == 2:
        gr, gc = int(array_grid[0]), int(array_grid[1])
        if gr*gc == array_cfg['arrays_per_cluster']:
            array_cfg['grid_rows'] = gr
            array_cfg['grid_cols'] = gc
        else:
            # 若不相等，自動調整 arrays_per_cluster 以符合 grid
            array_cfg['arrays_per_cluster'] = gr*gc
            array_cfg['grid_rows'] = gr
            array_cfg['grid_cols'] = gc

    pe = PEHardwareSpec(
        kc_fold = pe_cfg.get('conv1d_patterns', [{}])[0].get('in_channels', pe_cfg.get('kc_fold', DEFAULT_HW_SPEC.pe.kc_fold)),
        ofold = pe_cfg.get('conv1d_patterns', [{}])[0].get('out_channels', pe_cfg.get('ofold', DEFAULT_HW_SPEC.pe.ofold)),
        gemm_m_fold = pe_cfg.get('gemm_tile', {}).get('M', DEFAULT_HW_SPEC.pe.gemm_m_fold),
        gemm_n_fold = pe_cfg.get('gemm_tile', {}).get('N', DEFAULT_HW_SPEC.pe.gemm_n_fold),
        gemm_k_fold = pe_cfg.get('gemm_tile', {}).get('K', DEFAULT_HW_SPEC.pe.gemm_k_fold),
        pe_sram_size = pe_cfg.get('pe_sram_size', pe_cfg.get('reg_file_bytes', DEFAULT_HW_SPEC.pe.pe_sram_size)),  # 向後相容
        mac_energy_pj = pe_cfg.get('mac_energy_pJ', pe_cfg.get('mac_energy_pj', DEFAULT_HW_SPEC.pe.mac_energy_pj)),
        ifm_read_byte_energy_pj = pe_cfg.get('ifm_read_byte_energy_pj', DEFAULT_HW_SPEC.pe.ifm_read_byte_energy_pj),
        ifm_write_byte_energy_pj = pe_cfg.get('ifm_write_byte_energy_pj', DEFAULT_HW_SPEC.pe.ifm_write_byte_energy_pj),
        weight_read_byte_energy_pj = pe_cfg.get('weight_read_byte_energy_pj', DEFAULT_HW_SPEC.pe.weight_read_byte_energy_pj),
        ofm_write_byte_energy_pj = pe_cfg.get('ofm_write_byte_energy_pj', DEFAULT_HW_SPEC.pe.ofm_write_byte_energy_pj),
        ps_hop_energy_pj = pe_cfg.get('ps_fifo_energy_pJ', pe_cfg.get('ps_hop_energy_pj', DEFAULT_HW_SPEC.pe.ps_hop_energy_pj)),
        ps_hop_latency = pe_cfg.get('ps_hop_latency', DEFAULT_HW_SPEC.pe.ps_hop_latency),
        wave_transition_latency = pe_cfg.get('wave_transition_latency', DEFAULT_HW_SPEC.pe.wave_transition_latency),
    )
    gb = GlobalBufferSpec(
        count = gb_cfg.get('count', DEFAULT_HW_SPEC.gb.count),
        size_kb = gb_cfg.get('size_kb', gb_cfg.get('sizeKB', DEFAULT_HW_SPEC.gb.size_kb)),
        read_byte_energy_pj = gb_cfg.get('read_byte_energy_pj', DEFAULT_HW_SPEC.gb.read_byte_energy_pj),
        write_byte_energy_pj = gb_cfg.get('write_byte_energy_pj', DEFAULT_HW_SPEC.gb.write_byte_energy_pj),
    )
    array = ArrayHardwareSpec(**array_cfg)
    return HardwareSpec(pe=pe, array=array, gb=gb)
