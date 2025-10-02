from __future__ import annotations
from typing import Dict, Any, List
from math import ceil
from .gen_tiling import TilingResult
from .hardware import HardwareSpec, DEFAULT_HW_SPEC
from .workloads import dtype_size_bytes
import numpy as np

# -----------------------------------------------------------------------------
# MappingResult (重新設計, 分區語義 + 舊介面相容)
# -----------------------------------------------------------------------------
class MappingResult:
    def __init__(self,
                 policy: str,
                 pe_row_dim: str,
                 pe_col_dim: str,
                 horizontal_parallel_tiles: int,
                 array_assignment: Dict[str, Any],
                 waves: int,
                 pe_weight_groups: list,
                 pe_activation_groups: list,
                 pe_output_groups: list,
                 pe_row_groups: list,
                 *,
                 parallel: dict|None=None,
                 temporal: dict|None=None,
                 resource: dict|None=None,
                 extra: dict|None=None):
        self.policy=policy
        self.axes={'row':pe_row_dim,'col':pe_col_dim}
        self.parallel=parallel or {'horizontal_tiles':horizontal_parallel_tiles}
        self.temporal=temporal or {'waves':waves}
        self.resource=resource or {}
        self.assignment=array_assignment or {}
        self.pe_groups={
            'weight':pe_weight_groups,
            'activation':pe_activation_groups,
            'output':pe_output_groups,
            'row':pe_row_groups,
        }
        self.extra=extra or {}
        self._legacy={'horizontal_parallel_tiles':horizontal_parallel_tiles,'waves':waves}
    # --- 相容屬性 ---
    @property
    def pe_row_dim(self): return self.axes['row']
    @property
    def pe_col_dim(self): return self.axes['col']
    @property
    def horizontal_parallel_tiles(self): return self.parallel.get('horizontal_tiles', self._legacy['horizontal_parallel_tiles'])
    @property
    def waves(self): return self.temporal.get('waves', self._legacy['waves'])
    @property
    def pe_weight_groups(self): return self.pe_groups['weight']
    @property
    def pe_activation_groups(self): return self.pe_groups['activation']
    @property
    def pe_output_groups(self): return self.pe_groups['output']
    @property
    def pe_row_groups(self): return self.pe_groups['row']
    def to_dict(self):
        return {
            'policy':self.policy,
            'axes':self.axes,
            'parallel':self.parallel,
            'temporal':self.temporal,
            'resource':self.resource,
            'array_assignment':self.assignment,
            'pe_groups':self.pe_groups,
            'extra':self.extra,
            # legacy flat keys
            'pe_row_dim':self.pe_row_dim,
            'pe_col_dim':self.pe_col_dim,
            'horizontal_parallel_tiles':self.horizontal_parallel_tiles,
            'waves':self.waves,
            'pe_weight_groups':self.pe_weight_groups,
            'pe_activation_groups':self.pe_activation_groups,
            'pe_output_groups':self.pe_output_groups,
            'pe_row_groups':self.pe_row_groups,
        }

# -----------------------------------------------------------------------------
# Conv2D 映射：枚舉所有 (P_ho_sets, P_cin_sets)
# -----------------------------------------------------------------------------
def conv2d_mapping(tiling:TilingResult, hw:HardwareSpec|None=None) -> List[MappingResult]:
    hw_spec = hw or DEFAULT_HW_SPEC
    arrays = hw_spec.array.arrays_per_cluster
    rows = hw_spec.array.rows
    cols = hw_spec.array.cols

    stride_h = getattr(tiling.workload, 'stride_h', 1)
    Kh = getattr(tiling.workload, 'Kh', 1)
    Ho = tiling.spatial.get('H_outer', 1)

    groups_of_rows_per_array = max(1, rows // max(1,Kh))  # 每個 array 可切多少 Kh row-block set
    pe_set_capacity = groups_of_rows_per_array * cols * arrays        # 理論 PE set 容量 (僅供估算)
    max_cin_sets = max(1, groups_of_rows_per_array)
    max_ho_sets = min(pe_set_capacity, Ho)
    print(f"Conv2D Mapping: arrays={arrays}, rows={rows}, cols={cols}, Kh={Kh}, Ho={Ho}, groups_of_rows={groups_of_rows_per_array}, pe_set_capacity={pe_set_capacity}, max_cin_sets={max_cin_sets}, max_ho_sets={max_ho_sets}")

    def _build_single(p_ho_sets:int, p_cin_sets:int) -> MappingResult:
        # --- Temporal snippet (保留) --------------------------------------------------
        total_red_slices = tiling.num_reduction_slices # total reduction slices (C_in 分片數)
        Cin_reduction_waves = ceil(total_red_slices / max(1, p_cin_sets)) # C_in reduction waves, 每 wave 處理 p_cin_sets 個 C_in 分片, Psum 累加
        used_pe_sets = p_ho_sets * p_cin_sets if p_ho_sets*p_cin_sets>0 else 1 # 實際使用 PE set 數量
        tiles_per_round = max(1, pe_set_capacity // used_pe_sets) # 每輪可處理多少 Cout tiles, 更換 weight tile, 以及 activation/output group
        Cout_tile_rounds = ceil(tiling.Cout_tiles / tiles_per_round) # Cout tile rounds
        Ho_blocks = min(Ho, p_ho_sets) # Ho blocks (每輪處理多少 Ho block)
        Ho_block_rounds = ceil(Ho / max(1, Ho_blocks)) # Ho block rounds
        processing_passes = Cin_reduction_waves * Cout_tile_rounds * Ho_block_rounds # 總處理波次
        # ------------------------------------------------------------------------------

        # 1. 分派 Cout tiles (依照 tiles_per_round )
        rounds = Cout_tile_rounds  # Consistent with the snippet


        print(f"Mapping p_ho_sets={p_ho_sets}, p_cin_sets={p_cin_sets} => rounds={rounds}")

        # 2. 啟用 PE block (依照 p_cin_sets, tiles_per_round)
        blocks_per_array = rows // max(1, Kh) if Kh else rows
        total_blocks = arrays * blocks_per_array
        enabled_block_indices = set(range(min(p_cin_sets*ceil(p_ho_sets/cols), total_blocks)))
        array_block_enable = {
            aid: [
                b for b in range(blocks_per_array)
                if (aid * blocks_per_array + b) in enabled_block_indices
            ]
            for aid in range(arrays)
        }
        print(f"Array block enable: {array_block_enable}, total enabled blocks={sum(len(v) for v in array_block_enable.values())}")

        groups = []
        for array_id in range(arrays):
            if array_block_enable.get(array_id) == []:
                continue
            groups.append({'array': array_id, 'mode': 'conv2d_cf'})
        active_arrays = [g for g in groups if isinstance(g.get('array'), int)] # To be remove
        array_to_tiles = {g['array']: 0 for g in active_arrays} # To be remove
        print(f"active_arrays: {active_arrays}")

        # 3. PE group mapping
        pe_weight_groups=[]
        pe_activation_groups=[]
        pe_output_groups=[]
        pe_row_groups=[]

        horizontal_parallel=p_ho_sets # 每個 array 水平平行處理多少 Ho tile (activation group)
        for a in range(arrays):
            tiles = array_to_tiles.get(a, [])

            print(f"Array {a}, tiles={tiles}, horizontal_parallel={horizontal_parallel}")

            w_plane=[]
            a_plane=[]
            o_plane=[]
            row_group_vec=[]

            enabled_blocks=set(array_block_enable.get(a, []))

            for r in range(rows): # PE row
                local_block_id = r // max(1,Kh) # PE block id
                in_active = local_block_id in enabled_blocks
                global_block_id = a * blocks_per_array + local_block_id   # 全域

                w_row=[]
                a_row=[]
                o_row=[]

                for c in range(cols): # PE col
                    ar = (r % Kh) # activation row within Kh
                    ac = (c + ((r // Kh) * len(active_arrays) + a) * cols)  # activation col within horizontal_parallel

                    w_rid = r%Kh
                    a_rid = ar + (ac * stride_h)%p_ho_sets
                    o_rid = ac%p_ho_sets

                    pe_active = in_active and ac < min(Ho, p_ho_sets * p_cin_sets)

                    w_row.append(w_rid if pe_active else -1) # weight group (C_in slice)
                    a_row.append(a_rid if pe_active else -1) # activation group (Ho block)
                    o_row.append(o_rid if pe_active else -1) # output group (Ho block)

                w_plane.append(w_row)
                a_plane.append(a_row)
                o_plane.append(o_row)

                if in_active:
                    print(f"  PE row {r} (local {local_block_id}, global {global_block_id}): in_active={in_active}, w_row={w_row}, a_row={a_row}, o_row={o_row}")
                    row_group_vec.append(global_block_id)
                else:
                    print(f"  PE row {r} (local -, global -): in_active={in_active}")

            pe_weight_groups.append(w_plane)
            pe_activation_groups.append(a_plane)
            pe_output_groups.append(o_plane)
            pe_row_groups.append(row_group_vec)

        # 4. 壓縮 activation/output id
        def _compress(t3:list):
            ids=set()
            for A in t3:
                for row in A:
                    for v in row:
                        if isinstance(v,int) and v>=0: ids.add(v)
            remap={old:i for i,old in enumerate(sorted(ids))}
            for A in t3:
                for row in A:
                    for i,v in enumerate(row):
                        if isinstance(v,int) and v>=0:
                            row[i]=remap[v]
            return remap
        act_map=_compress(pe_activation_groups)
        out_map=_compress(pe_output_groups)

        # 5. SRAM per array (粗估)
        SRAM_per_array = []

        for a in range(arrays):
            weight_set = set()
            activation_set = set()
            output_set = set()

            SRAM_usage = {'weight':0, 'activation':0, 'output':0, 'total':0}

            for r in range(rows):
                for c in range(cols):
                    w_id = pe_weight_groups[a][r][c]
                    a_id = pe_activation_groups[a][r][c]
                    o_id = pe_output_groups[a][r][c]
                    row_id = pe_row_groups[a][r] if r < len(pe_row_groups[a]) else -1

                    if row_id < 0 or w_id < 0 and a_id < 0 and o_id < 0:
                        continue

                    weight_set.add((row_id, w_id))
                    activation_set.add((row_id, a_id))
                    output_set.add((row_id, o_id))

            print(f"Array {a}: weight_set={weight_set}, activation_set={activation_set}, output_set={output_set}")

            dtype_size = dtype_size_bytes(getattr(tiling.workload, 'data_type', 'fp16'))

            Cin_per_pe = (tiling.Cin_fold//tiling.workload.Kw)

            SRAM_usage['weight'] = len(weight_set) * Cin_per_pe * tiling.workload.Kw * tiling.Cout_fold * dtype_size
            SRAM_usage['activation'] = len(activation_set) * tiling.spatial['W_inner'] * Cin_per_pe * dtype_size
            SRAM_usage['output'] = len(output_set) * tiling.spatial['W_inner'] * tiling.Cout_fold * dtype_size
            SRAM_usage['total'] = SRAM_usage['weight'] + SRAM_usage['activation'] + SRAM_usage['output']

            SRAM_per_array.append(SRAM_usage)

        # 6. Meta + temporal/parallel/resource
        array_assignment={
            'groups':groups,
            'rounds':rounds,
            'policy':'conv2d_ifm_row_parallel',
            'meta':{
                'kernel_h':Kh,
                'horizontal_semantics':'ifm_row_parallel',
                'activation_group_compact_count':len(act_map),
                'output_group_compact_count':len(out_map),
                'P_ho_sets':p_ho_sets,
                'P_cin_sets':p_cin_sets,
                'pe_set_counts_capacity':pe_set_capacity,
                'ifm_broadcast_across_arrays':True,
                'inter_array_output_accumulate':False
            }
        }
        temporal={'waves':processing_passes,'cin_waves':Cin_reduction_waves,'ho_rounds':Ho_block_rounds,'cout_rounds':Cout_tile_rounds,'tiles_per_round':tiles_per_round}
        parallel={'horizontal_tiles':horizontal_parallel,'P_ho_sets':p_ho_sets,'P_cin_sets':p_cin_sets}
        resource={'pe_set_capacity':pe_set_capacity,'act_compact':len(act_map),'out_compact':len(out_map),'sram_per_array':SRAM_per_array}

        print("[temporal]", temporal)
        print("[parallel]", parallel)
        print("[resource]", resource)

        return MappingResult(policy='conv2d', pe_row_dim='reduction_slice', pe_col_dim='Ho_block',
                             horizontal_parallel_tiles=horizontal_parallel, array_assignment=array_assignment,
                             waves=processing_passes, pe_weight_groups=pe_weight_groups,
                             pe_activation_groups=pe_activation_groups, pe_output_groups=pe_output_groups,
                             pe_row_groups=pe_row_groups, parallel=parallel, temporal=temporal, resource=resource)

    results:List[MappingResult]=[]
    p_cin_sets_range = [i for i in range(1,max_cin_sets+1) if max_cin_sets%i==0]
    p_ho_sets_range = [i for i in range(1,max_ho_sets+1) if max_ho_sets%i==0]
    print(f"Conv2D Mapping: Enumerating P_cin_sets={p_cin_sets_range}, P_ho_sets={p_ho_sets_range}")
    for p_cin in p_cin_sets_range:
        for p_ho in p_ho_sets_range:
            if p_cin * p_ho > pe_set_capacity or p_cin < 1 or p_ho < 1:
                continue
            results.append(_build_single(p_ho, p_cin))

    # ---- Pareto 過濾：最小 waves, 最大 horizontal_tiles ----
    orig_count = len(results)
    pareto=[]
    for i,m in enumerate(results):
        w = m.waves
        h = m.horizontal_parallel_tiles
        dominated=False
        for j,n in enumerate(results):
            if i==j: continue
            w2=n.waves; h2=n.horizontal_parallel_tiles
            if w2 <= w and h2 >= h and (w2 < w or h2 > h):
                dominated=True; break
        if not dominated:
            pareto.append(m)
    # 標記 extra
    for idx,m in enumerate(results):
        m.extra.setdefault('enum_index', idx)
        m.extra.setdefault('enum_total', orig_count)
        m.extra['pareto_selected'] = m in pareto
    # 回傳 Pareto 前緣 (依 waves 升序, horizontal_tiles 降序)
    pareto.sort(key=lambda x:(x.waves, -x.horizontal_parallel_tiles))
    return pareto

# -----------------------------------------------------------------------------
# GEMM 映射
# -----------------------------------------------------------------------------
def gemm_mapping(tiling:TilingResult, hw:HardwareSpec|None=None) -> MappingResult:
    """
    GEMM 映射：
    - N 維被切成 N_tiles
    - 每個 array 橫向最多 cols 個 N_tile 併行
    - 超出容量以 rounds (temporal) 方式覆用
    - waves = reduction_waves * rounds
    - 增加 group id 壓縮與 SRAM 粗估
    """
    hw_spec = hw or DEFAULT_HW_SPEC
    arrays = hw_spec.array.arrays_per_cluster
    rows = hw_spec.array.rows
    cols = hw_spec.array.cols

    N_tiles = getattr(tiling, 'N_tiles', 0)
    reduction_waves = getattr(tiling, 'reduction_waves', 1)

    print(f"GEMM Mapping: arrays={arrays}, rows={rows}, cols={cols}, tiling={tiling}")

    reduction_tiles_per_array = cols  # 每個 array 可處理多少 N_fold reduction tile

    def _build_single(p_m_tiles:int, p_k_tiles) -> MappingResult:
        # 1. 分派 N tiles (依照 cols)
        tiles_per_round = max(1, arrays * reduction_tiles_per_array)



# -----------------------------------------------------------------------------
# Wrapper
# -----------------------------------------------------------------------------
def build_mapping(tiling:TilingResult, hw:HardwareSpec|None=None) -> MappingResult:
    op = getattr(tiling.workload, 'op', None)
    if op == 'conv2d':
        results=conv2d_mapping(tiling, hw)
        return results[0] if results else None
    elif op == 'gemm':
        return gemm_mapping(tiling, hw)
    else:
        raise ValueError(f"Unsupported op: {op}")
