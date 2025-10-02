# ...new file...
from __future__ import annotations
from dataclasses import dataclass, asdict
from typing import Literal, Dict, Any, Union

def dtype_size_bytes(dtype:str) -> int:
    if dtype == 'fp16':
        return 2
    elif dtype == 'fp32':
        return 4
    elif dtype == 'int8':
        return 1
    else:
        raise ValueError(f"Unsupported data type: {dtype}")

@dataclass
class Conv2DWorkload:
    op: Literal['conv2d'] = 'conv2d'
    N: int = 1
    Cin: int = 1
    Cout: int = 1
    Hin: int = 1
    Win: int = 1
    Kh: int = 1
    Kw: int = 1
    stride_h: int = 1
    stride_w: int = 1
    pad_h: int = 0
    pad_w: int = 0
    dilation_h: int = 1
    dilation_w: int = 1
    data_type: str = 'fp16'

    def output_hw(self):
        Ho = (self.Hin + 2*self.pad_h - self.dilation_h*(self.Kh-1) - 1)//self.stride_h + 1
        Wo = (self.Win + 2*self.pad_w - self.dilation_w*(self.Kw-1) - 1)//self.stride_w + 1
        return Ho, Wo

    def macs(self) -> int:
        Ho, Wo = self.output_hw()
        return self.N * self.Cout * Ho * Wo * self.Cin * self.Kh * self.Kw

    def to_dict(self):
        return asdict(self)

# (M x K) * (K x N) = (M x N)
@dataclass
class GemmWorkload:
    op: Literal['gemm'] = 'gemm'
    M: int = 1 # input rows
    N: int = 1 # output rows
    K: int = 1 # reduction dimension
    layoutA: Literal['row-major', 'col-major'] = 'row-major'
    layoutB: Literal['row-major', 'col-major'] = 'col-major'
    data_type: str = 'fp16'

    def macs(self) -> int:
        return self.M * self.N * self.K

    def to_dict(self):
        return asdict(self)

Workload = Union[Conv2DWorkload, GemmWorkload]


def workload_from_dict(d:Dict[str,Any]) -> Workload:
    op = d.get('op')
    if op == 'conv2d':
        return Conv2DWorkload(**{k:v for k,v in d.items() if k in Conv2DWorkload().__dict__})
    elif op == 'gemm':
        return GemmWorkload(**{k:v for k,v in d.items() if k in GemmWorkload().__dict__})
    else:
        raise ValueError(f"Unsupported workload op: {op}")
