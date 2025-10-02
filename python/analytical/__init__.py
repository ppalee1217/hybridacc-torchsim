# ...new file...
"""HybridAcc analytical mapping & cost modeling package.

核心功能:
- 讀取 workload (Conv2D / GEMM)
- 產生 heuristic tiling 與 mapping 參數
- 建立 schedule IR (JSON 結構)
- 粗略 latency / energy 成本模型
- 輸出報告與硬體設定檔

CLI: ha-analytical (在 pyproject.toml 的 [project.scripts])
"""
from .gen_tiling import generate_tiling
from .mapper import build_mapping
from .ir_builder import build_schedule_ir
from .cost_model import CostModel, AnalysisReport
from .exporters.emit_ir import write_schedule_ir
from .exporters.emit_hw_config import write_hw_config
from .exporters.emit_report import write_analysis_artifacts
from .workloads import Conv2DWorkload, GemmWorkload, Workload, workload_from_dict
from .hardware import HardwareSpec, PEHardwareSpec, ArrayHardwareSpec, DEFAULT_HW_SPEC

__all__ = [
    "generate_tiling", "build_mapping", "build_schedule_ir", "CostModel", "AnalysisReport",
    'write_schedule_ir','write_hw_config','write_analysis_artifacts',
    'Conv2DWorkload','GemmWorkload','Workload','workload_from_dict',
    'HardwareSpec','PEHardwareSpec','ArrayHardwareSpec','DEFAULT_HW_SPEC'
]
