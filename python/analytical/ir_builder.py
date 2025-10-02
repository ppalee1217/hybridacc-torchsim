from __future__ import annotations
from typing import Dict, Any
from dataclasses import dataclass, asdict
from .workloads import Conv2DWorkload, GemmWorkload  # 新增引入

@dataclass
class ScheduleIR:
    loops: Any
    mapping: Any
    compute: Any
    buffers: Any
    meta: Dict[str, Any]

    def to_dict(self):
        return asdict(self)


def _get(wl, name, default=None):  # 若未來仍可能傳 dict, 做兼容
    if hasattr(wl, name):
        return getattr(wl, name)
    if isinstance(wl, dict):
        return wl.get(name, default)
    return default


def build_schedule_ir(tiling, mapping) -> ScheduleIR:
    wl = tiling.workload
    # 判斷 op 類型
    if isinstance(wl, Conv2DWorkload):
        op = 'conv2d'
    elif isinstance(wl, GemmWorkload):
        op = 'gemm'
    else:  # 後備 (例如傳 dict)
        op = wl['op'] if isinstance(wl, dict) else getattr(wl, 'op', 'unknown')

    if op == 'conv2d':
        Ho = tiling.spatial['H_outer']; Wo = tiling.spatial['W_inner']
        loops = [
            {'name': 'n', 'range': _get(wl, 'N', 1)},
            {'name': 'co', 'range': _get(wl, 'Cout'), 'tile': [tiling.Cout_tiles, tiling.Cout_fold]},
            {'name': 'ho', 'range': Ho, 'tile': [mapping.temporal['ho_rounds'], mapping.parallel['P_ho_sets']]},
            {'name': 'wo', 'range': Wo, 'tile': [tiling.spatial['W_outer'], tiling.spatial['W_inner']]},
            {'name': 'red', 'range': _get(wl, 'Cin') * _get(wl, 'Kh') * _get(wl, 'Kw'), 'tile': [tiling.num_reduction_slices, tiling.Cin_fold]},
        ]
    elif op == 'gemm':
        loops = [
            {'name': 'm', 'range': _get(wl, 'M')},
            {'name': 'n', 'range': _get(wl, 'N'), 'tile': [tiling.Cout_tiles, tiling.Cout_fold]},
            {'name': 'k', 'range': _get(wl, 'K'), 'tile': [tiling.num_reduction_slices, tiling.Cin_fold]},
        ]
    else:
        raise ValueError(f"Unsupported op in build_schedule_ir: {op}")

    mapping_dict = {
        'policy': mapping.policy,
        'axes': getattr(mapping, 'axes', {'row': mapping.pe_row_dim, 'col': mapping.pe_col_dim}),
        'parallel': getattr(mapping, 'parallel', {'horizontal_tiles': mapping.horizontal_parallel_tiles}),
        'temporal': getattr(mapping, 'temporal', {'waves': mapping.waves}),
        'resource': getattr(mapping, 'resource', {}),
        'extra': getattr(mapping, 'extra', {}),
        # legacy 快速訪問欄位 (向後相容)
        'pe_row': mapping.pe_row_dim,
        'pe_col': mapping.pe_col_dim,
        'waves': mapping.waves,
        'horizontal_parallel_tiles': getattr(mapping, 'horizontal_parallel_tiles', None),
        'array_assignment': mapping.assignment,
        # 3D 分組資訊
        'pe_groups': getattr(mapping, 'pe_groups', {
            'weight': getattr(mapping, 'pe_weight_groups', None),
            'activation': getattr(mapping, 'pe_activation_groups', None),
            'output': getattr(mapping, 'pe_output_groups', None),
            'row': getattr(mapping, 'pe_row_groups', None),
        }),
        # 仍保留原本欄位名稱 (若外部 consumer 依賴)
        'pe_weight_groups': getattr(mapping, 'pe_weight_groups', None),
        'pe_activation_groups': getattr(mapping, 'pe_activation_groups', None),
        'pe_output_groups': getattr(mapping, 'pe_output_groups', None),
        'pe_row_groups': getattr(mapping, 'pe_row_groups', None),
    }

    compute = [
        {'op': 'mac', 'tensor': 'O_partial', 'reduce_over': 'red'},
        {'op': 'accumulate_chain', 'length': 7, 'waves': mapping.waves},
    ]
    buffers = {
        'global_ifm': 'shared',
        'global_ofm': 'partitioned',
        'weights': 'replicated_per_pe'
    }

    meta = {
        'version': '0.1', 'creator': 'analytical.ir_builder',
        'notes': 'simplified schedule IR'
    }
    return ScheduleIR(loops=loops, mapping=mapping_dict, compute=compute, buffers=buffers, meta=meta)
