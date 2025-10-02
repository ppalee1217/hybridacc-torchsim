from __future__ import annotations
from dataclasses import dataclass, asdict
from math import ceil
from typing import Dict, Any
from .workloads import Workload, Conv2DWorkload, GemmWorkload
from .hardware import PEHardwareSpec, DEFAULT_HW_SPEC

# 常數: 單 PE micro-kernel capability
KcFOLD = 12  # reduction tile (Cin*Kh*Kw linearized)
OFOLD = 16   # output channel fold
MAX_PS_CHAIN = 7  # 單 array 垂直 7 個 PE 可串聯的 reduction slices

@dataclass
class TilingResult:
    workload: Workload
    pes_per_tile: int  # 預設每個 tile 使用 1 個 PE (未來可擴充)
    num_reduction_slices: int
    spatial: Dict[str, int]
    # conv2d 專用欄位 (若為 gemm 則為 None)
    Cin_fold: int | None = None
    Cout_fold: int | None = None
    Cout_tiles: int | None = None
    # GEMM 專用欄位 (若為 conv2d 則為 None)
    M_fold: int | None = None
    N_fold: int | None = None
    K_fold: int | None = None
    M_tiles: int | None = None
    N_tiles: int | None = None
    K_tiles: int | None = None
    notes: str = ""

    def to_dict(self):
        # 之前覆寫 d['workload']=self.workload 會把已被 asdict() 轉好的 dict 又替換成 dataclass 物件
        # 導致 json.dump 時無法序列化。直接回傳 asdict(self) 讓 dataclass 遞迴處理。
        return asdict(self)


def _derive_output_dims(wl:Dict[str,Any]) -> Dict[str,int]:
    if wl['op'] == 'conv2d':
        Ho = (wl['Hin'] + 2*wl.get('pad_h',0) - wl.get('dilation_h',1)*(wl['Kh']-1) - 1)//wl.get('stride_h',1) + 1
        Wo = (wl['Win'] + 2*wl.get('pad_w',0) - wl.get('dilation_w',1)*(wl['Kw']-1) - 1)//wl.get('stride_w',1) + 1
        return {'H_out':Ho,'W_out':Wo}
    elif wl['op'] == 'gemm':
        return {'M_out': wl['M'], 'N_out': wl['N']}
    else:
        raise ValueError(f"Unsupported op {wl['op']}")


def generate_tiling(workload:Workload, pe_spec:PEHardwareSpec|None=None) -> TilingResult:
    pe = pe_spec or DEFAULT_HW_SPEC.pe
    op = workload.op
    if op == 'conv2d':
        wl:Conv2DWorkload = workload  # type: ignore
        Ho, Wo = wl.output_hw()
        Cin_per_pe = pe.kc_fold // wl.Kw  # 每個 PE 可處理的 Cin 數量
        if Cin_per_pe < 1:
            raise ValueError(f"PE kc_fold {pe.kc_fold} too small for kernel width {wl.Kw}")
        # 每個 PE 可處理的 Cout 數量
        Cout_per_pe = pe.ofold
        num_red_slices = (wl.Cin) // Cin_per_pe # 每個 reduction slice 包含 kc_fold 個 reduction
        Cout_tiles = (wl.Cout + Cout_per_pe - 1) // Cout_per_pe  # 計算 Cout 的 tiles 數量
        spatial = {'H_outer': Ho, 'H_inner': 1, 'W_outer': 1, 'W_inner': Wo}  # 空間維度的映射
        return TilingResult(
            workload=wl,
            pes_per_tile=wl.Kh, # kernel height 決定每個 tile 用多少 PE
            Cin_fold=Cin_per_pe * wl.Kw,
            Cout_fold=Cout_per_pe,
            num_reduction_slices=num_red_slices,
            Cout_tiles=Cout_tiles,
            spatial=spatial)
    elif op == 'gemm':
        wl:GemmWorkload = workload  # type: ignore
        # 使用獨立 GEMM fold 設定
        M_fold = pe.gemm_m_fold
        N_fold = pe.gemm_n_fold
        K_fold = pe.gemm_k_fold
        M_tiles = ceil(wl.M / M_fold)
        N_tiles = ceil(wl.N / N_fold)
        K_tiles = ceil(wl.K / K_fold)
        num_red_slices = K_tiles  # 每個 K_fold 為一 slice
        Cout_tiles = N_tiles  # 對齊 N tiles
        spatial = {'M_outer': M_tiles, 'M_inner': 1, 'N_outer': N_tiles, 'N_inner': 1}
        return TilingResult(
            workload=wl,
            pes_per_tile=1,
            num_reduction_slices=num_red_slices,
            spatial=spatial,
            M_fold=M_fold,
            N_fold=N_fold,
            K_fold=K_fold,
            M_tiles=M_tiles,
            N_tiles=N_tiles,
            K_tiles=K_tiles,
            notes='gemm mapping with dedicated folds')
    else:
        raise ValueError('Unsupported op')
