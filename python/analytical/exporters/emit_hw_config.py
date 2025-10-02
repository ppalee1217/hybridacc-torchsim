# ...new file...
from __future__ import annotations
import yaml
from pathlib import Path

_DEFAULT_HW_CFG = {
  'hardware': {
    'clusters': 1,
    'arrays_per_cluster': 6,
    'array_shape': {'rows':7,'cols':4},
    'pe': {
      'conv1d_patterns': [
        {'k':1,'in_channels':12,'out_channels':16},
        {'k':3,'in_channels':4,'out_channels':16},
        {'k':5,'in_channels':2,'out_channels':16},
        {'k':7,'in_channels':1,'out_channels':16},
      ],
      'gemm_tile': {'M':8,'N':12,'K':32},
      'mac_energy_pJ': 1.2,
      'pe_sram_size': 256,
    },
    'buffers': {
      'global': {'count':6,'size_kb':256,'energy_per_access_pJ':8},
      'ps_fifo_energy_pJ': 0.5,
    },
    'interconnect': {
      'vertical_hop_ps_energy_pJ': 0.2,
      'bandwidth_bytes_per_cycle': 16,
    },
    'freq_MHz': 800,
    'data_type': {
      'int8': {'bits':8,'mac_energy_scale':1.0},
      'fp16': {'bits':16,'mac_energy_scale':2.2},
    }
  }
}

def write_hw_config(out_path, hw_cfg=None):
    cfg = hw_cfg or _DEFAULT_HW_CFG
    p = Path(out_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p,'w') as f:
        yaml.safe_dump(cfg, f, sort_keys=False)
    return str(p)
