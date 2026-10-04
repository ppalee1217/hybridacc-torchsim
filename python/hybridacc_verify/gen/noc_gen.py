import torch
import math
import re
import numpy as np
from pathlib import Path
from typing import Dict, Any, List, Tuple
from ..utils.config import ScanChainConfig, PERouterMode, NocConvConfig, NocGemmConfig, ConvMode
from ..utils.data import TestData
from ..model.conv import golden_conv2d
from ..model.gemm import golden_gemm
from .pe_gen import DataGenerator

def generate_conv2d_test(config: NocConvConfig) -> List[TestData]:
    """
    Generate Conv2d test case based on config.
    """
    print("Generating Conv2d test data...")
    config.validate()

    # Shapes from config
    N = 1
    H, W, C = config.input_h, config.input_w, config.input_c
    OC, KH, KW = config.out_ch, config.kernel_h, config.kernel_w
    stride = config.stride
    padding = config.padding
    num_pes = config.num_pes
    num_bus = config.num_bus

    torch.manual_seed(config.seed)
    np.random.seed(config.seed)

    # Generate random data
    input_act = torch.randn(N, H, W, C).numpy()
    weight = torch.randn(OC, KH, KW, C).numpy()

    test_data_list = []

    # Determine split configuration
    splits = []
    if KH > num_bus:
        if KH == 5 and num_bus == 4:
             splits = [3, 2]
        else:
             # Generic split: chunks of num_bus
             remaining = KH
             while remaining > 0:
                 splits.append(min(remaining, num_bus))
                 remaining -= min(remaining, num_bus)
    else:
        splits = [KH]

    current_kh_start = 0
    previous_output = None

    # Calculate expected final output shape (based on full kernel)
    # We assume padding applies to the full operation.
    # For the split parts, we will manually slice the input to produce this exact output height.
    out_h_final = (H + 2*padding - KH) // stride + 1
    out_w_final = (W + 2*padding - KW) // stride + 1

    for idx, split_kh in enumerate(splits):
        # Slice weights
        weight_part = weight[:, current_kh_start:current_kh_start+split_kh, :, :]

        # Calculate required input height for this split to match out_h_final
        # H_in = (H_out - 1) * stride + K - 2*P_part
        # We assume P_part=0 for height as we slice the valid region.
        req_h_part = (out_h_final - 1) * stride + split_kh

        input_slice_start = current_kh_start
        input_slice_end = input_slice_start + req_h_part

        # Slice inputs
        # Note: This assumes the original input is large enough (i.e. padding=0 or handled)
        if input_slice_end > H:
             # Fallback or error handling if padding was expected to extend input
             # For the user's specific case (H=20, K=5, P=0), this works.
             print(f"Warning: Input slice [{input_slice_start}:{input_slice_end}] exceeds input height {H}")
             input_slice_end = H

        input_act_part = input_act[:, input_slice_start:input_slice_end, :, :]

        if previous_output is None:
             input_ps_part = torch.randn(N, out_h_final, out_w_final, OC).numpy() # NHWC
        else:
             input_ps_part = previous_output

        # Calculate Golden
        # We use padding=(0, padding) to disable height padding (since we sliced)
        # but keep width padding.
        output_part = golden_conv2d(input_act_part, weight_part, input_ps_part, stride=stride, padding=(0, padding))
        previous_output = output_part

        # Pack weights if needed
        weight_packed = weight_part
        if KW == 5 and C == 2:
             # Pack for k5c2
             # weight_part: (OC, split_kh, 5, 2)
             w_flat = weight_part.reshape(-1, 5, 2).transpose(0, 2, 1) # (N, 2, 5)
             packed = DataGenerator.pack_weight_mode_b(w_flat, 'channels_last') # (N, 6, 2)
             weight_packed = packed.reshape(OC, split_kh, 6, 2)
        elif KW == 7 and C == 1:
             # Pack for k7c1
             # weight_part: (OC, split_kh, 7, 1)
             w_flat = weight_part.reshape(-1, 7, 1).transpose(0, 2, 1) # (N, 1, 7)
             packed = DataGenerator.pack_weight_mode_c(w_flat, 'channels_last') # (N, 12, 1)
             weight_packed = packed.reshape(OC, split_kh, 12, 1)

        # Prepare numpy arrays
        inputs_part = {
            "activation": input_act_part,
            "weight": weight_packed,
            "partial_sum": input_ps_part
        }
        outputs_part = {
            "partial_sum": output_part
        }

        scan_chain = []

        def get_route_mode(row_idx: int, kh: int) -> int:
            if row_idx == 0: # First row
                return PERouterMode.PLI_FROM_BUS_PLO_TO_LN
            elif row_idx == kh-1: # Last row
                return PERouterMode.PLI_FROM_LN_PLO_TO_BUS
            else: # Middle rows
                return PERouterMode.PLI_FROM_LN_PLO_TO_LN

        num_pes_per_bus = num_pes // num_bus

        # Temporal wave count split by output height, output channels, input channels
        if config.ultra_mode:
            out_h_waves = math.ceil(out_h_final / (num_pes_per_bus * num_bus))
        else:
            out_h_waves = math.ceil(out_h_final / num_pes_per_bus)

        out_ch_waves = math.ceil(OC / 16) # Assuming PE processes 16 output channels per wave
        channels_per_packet = ConvMode.channels_from_kernel_size(KW)
        in_ch_waves = math.ceil(C / channels_per_packet)
        temporal_wave_count = out_h_waves * out_ch_waves * in_ch_waves

        for i in range(num_bus):
            for j in range(num_pes_per_bus):
                if config.ultra_mode:
                    # Ultra Mode: Distribute workload across all buses
                    output_row_idx = j
                    enable = (output_row_idx < out_h_final)
                    route_mode = PERouterMode.PLI_FROM_BUS_PLO_TO_BUS
                    pd_id = output_row_idx * stride if enable else 63
                    ps_id = 0 if enable else 63
                    pli_id = output_row_idx if enable else 63
                    plo_id = output_row_idx if enable else 63
                else: # Normal Mode: Each bus handles a chunk of the kernel height
                    enable = (i < split_kh and j < out_h_final)
                    route_mode = PERouterMode.PLI_FROM_BUS_PLO_TO_BUS

                    if enable:
                        route_mode = get_route_mode(i, split_kh)

                    ps_id = i if enable else 63
                    pd_id = (i+j)*stride if enable else 63
                    pli_id = j if (i==0 and enable) else 63
                    plo_id = j if (i==split_kh-1 and enable) else 63

                cfg = ScanChainConfig(
                    ps_id=ps_id,
                    pd_id=pd_id,
                    pli_id=pli_id,
                    plo_id=plo_id,
                    route_mode=route_mode,
                    enable=enable
                )
                scan_chain.append(cfg)

        test_config = {
            "mode": "conv2d",
            "temporal_wave_count": temporal_wave_count,
            "temporal_wave_out_h": out_h_waves,
            "temporal_wave_out_ch": out_ch_waves,
            "temporal_wave_in_ch": in_ch_waves,
            "ultra_mode": config.ultra_mode,
            "kernel_size": split_kh,
            "in_ch": C,
            "stride": stride,
            "out_ch": OC,
            "in_height": input_act_part.shape[1],
            "in_width": input_act_part.shape[2],
            "out_height": output_part.shape[1],
            "out_width": output_part.shape[2],
            "partial_sum_zero": False,
            "seed": config.seed + idx
        }

        name_suffix = f"_part{idx+1}" if len(splits) > 1 else ""

        test_data_list.append(TestData(
            name=f"conv2d_custom{name_suffix}",
            description=f"Conv2d {split_kh}x{KW} (Split {idx+1}/{len(splits)})",
            inputs=inputs_part,
            outputs=outputs_part,
            scan_chain=scan_chain,
            config=test_config
        ))

        current_kh_start += split_kh

    return test_data_list

def plan_gemm_waves(M: int, N: int, K: int, num_pes: int, num_bus: int) -> Dict[str, Any]:
    """
    Per-wave tiling plan of the NoC GEMM test.

    Returns the global tile grid, the chosen per-wave tile shape, the wave
    counts and the per-wave tile lists that test_noc_sim reads from config.txt.
    """
    # PE Capability
    PE_M, PE_N = 12, 8
    PE_K = 32 # PE processes 32 K-dim per step/pass

    # Calculate Grid Size
    grid_m = (M + PE_M - 1) // PE_M
    grid_n = (N + PE_N - 1) // PE_N
    grid_k = (K + PE_K - 1) // PE_K  # K-splits

    print(f"Grid Layout: M={grid_m}, N={grid_n}, K_split={grid_k}")

    # Calculate Temporal Waves if hardware resources are insufficient
    pes_per_bus = num_pes // num_bus

    # Choose M/N tile shape per wave to fit PE budget
    def choose_mn_tiles(grid_m: int, grid_n: int, pe_budget: int):
        best = None
        prefer_n_cap = max(1, pe_budget // 2)
        single_k_wave = grid_k <= num_bus
        for m_tiles in range(min(grid_m, pe_budget), 0, -1):
            max_n = min(grid_n, pe_budget // m_tiles)
            for n_tiles in range(max_n, 0, -1):
                waves_m = math.ceil(grid_m / m_tiles)
                waves_n = math.ceil(grid_n / n_tiles)
                waves = waves_m * waves_n
                area = m_tiles * n_tiles
                aspect = abs((grid_m / max(grid_n, 1)) - (m_tiles / max(n_tiles, 1)))
                balance = abs(waves_m - waves_n)
                if single_k_wave:
                    n_bias = min(n_tiles, prefer_n_cap)
                    score = (waves, -n_bias, -m_tiles, balance, aspect, -area)
                else:
                    score = (waves, -n_tiles, -m_tiles, -area, aspect)
                if best is None or score < best[0]:
                    best = (score, m_tiles, n_tiles, waves_m, waves_n)
        if best is None:
            return 1, 1, grid_m, grid_n
        _, m_tiles, n_tiles, waves_m, waves_n = best
        return m_tiles, n_tiles, waves_m, waves_n

    m_tiles_per_wave, n_tiles_per_wave, wave_m, wave_n = choose_mn_tiles(grid_m, grid_n, pes_per_bus)

    def split_tiles(total_tiles: int, waves: int) -> List[int]:
        if waves <= 0:
            return []
        base = total_tiles // waves
        rem = total_tiles % waves
        tiles = [base + 1 if i < rem else base for i in range(waves)]
        return tiles

    # Number of waves needed for K-dimension (if K-splits > Buses)
    k_tiles_per_wave = num_bus if num_bus > 0 else 1
    wave_k = math.ceil(grid_k / k_tiles_per_wave)

    grid_m_per_wave = split_tiles(grid_m, wave_m)
    grid_n_per_wave = split_tiles(grid_n, wave_n)
    grid_k_per_wave = split_tiles(grid_k, wave_k)

    return {
        "grid_m": grid_m,
        "grid_n": grid_n,
        "grid_k": grid_k,
        "m_tiles_per_wave": m_tiles_per_wave,
        "n_tiles_per_wave": n_tiles_per_wave,
        "k_tiles_per_wave": k_tiles_per_wave,
        "wave_m": wave_m,
        "wave_n": wave_n,
        "wave_k": wave_k,
        "grid_m_per_wave": grid_m_per_wave,
        "grid_n_per_wave": grid_n_per_wave,
        "grid_k_per_wave": grid_k_per_wave,
    }


def gemm_wave_grid(plan: Dict[str, Any], ultra_mode: bool) -> Tuple[int, int]:
    """
    Per-wave (grid_m, grid_n) that the static scan chain must encode.

    The scan chain is loaded once per test, so it can only describe a wave
    shape that every wave shares. test_noc_sim derives each wave's tags from
    that wave's own tile count (n_tiles = this wave's N tiles), so a ragged
    plan would leave PEs waiting for tags that are never sent.
    """
    m_per_wave = plan["grid_m_per_wave"]
    n_per_wave = plan["grid_n_per_wave"]
    if len(set(m_per_wave)) != 1 or len(set(n_per_wave)) != 1:
        raise ValueError(
            "GEMM wave plan is ragged (per-wave tiles M=%s, N=%s); a static scan "
            "chain cannot express per-wave tile counts" % (m_per_wave, n_per_wave))
    if not ultra_mode and plan["wave_m"] * plan["wave_n"] > 1:
        # test_noc_sim's non-ultra GEMM path tags PS/PD/PLI with global tile
        # indices, which no single wave-local scan chain can match.
        raise ValueError(
            "non-ultra GEMM NoC test supports a single M/N wave only "
            "(plan has %d M waves x %d N waves)" % (plan["wave_m"], plan["wave_n"]))
    return m_per_wave[0], n_per_wave[0]


def build_gemm_scan_chain(num_pes: int, num_bus: int, grid_m: int, grid_n: int,
                          grid_k: int, ultra_mode: bool) -> List[ScanChainConfig]:
    """
    GEMM K-chain scan chain with wave-local tags.

    grid_m / grid_n are the tile counts of one wave, as in the cc lowering
    (hybridacc_cc.lowering.compute_scan_chain_gemm is called with
    grid_m_per_wave / grid_n_per_wave): PE j of a bus holds tile
    (m_idx = j // grid_n, n_idx = j % grid_n) of the current wave, and
    pli_id = plo_id = m_idx * grid_n + n_idx. test_noc_sim sends the same
    wave-local tags in ultra mode.
    """
    # --- Scan Chain Construction ---
    scan_chain = []

    # Calculate physical layout
    # We assign Bus `b` to handle `K-split = b`.
    # Inside Bus, we place the (M, N) grid.

    pes_per_bus = num_pes // num_bus

    def get_route_mode(k_idx: int, k_total: int) -> int:
        # Chain flow: Bus 0 (Start) -> ... -> Bus N (End)
        if k_idx == 0:
            # First stage: Read from BUS (or zero), output to Neighbor (Next Stage)
            return PERouterMode.PLI_FROM_BUS_PLO_TO_LN
        elif k_idx == k_total - 1:
            # Last stage: Read from Neighbor, Accumulate, output to BUS (Final Memory)
            return PERouterMode.PLI_FROM_LN_PLO_TO_BUS
        else:
            # Middle stage: Read from Neighbor, Accumulate, output to Neighbor
            return PERouterMode.PLI_FROM_LN_PLO_TO_LN

    for b in range(num_bus):
        # Current K-slice index
        k_idx = b

        # Check if this bus is part of the active K-chain
        is_active_k_layer = (k_idx < grid_k)

        # Determine Routing Mode for this layer
        r_mode = get_route_mode(k_idx, grid_k) if is_active_k_layer else PERouterMode.PLI_FROM_BUS_PLO_TO_BUS

        for j in range(pes_per_bus):
            # Map j to (m, n) within this layer
            # Simple Row-Major mapping of one wave's MxN grid
            # Capability per Bus: pes_per_bus
            # Required: grid_m * grid_n (per wave)

            m_idx = j // grid_n
            n_idx = j % grid_n

            is_active_pe = is_active_k_layer and (m_idx < grid_m)

            if is_active_pe:
                # Active PE
                # ps_id: Tiled Weight (B_kn) -> Shared by PEs with same (k, n)
                # pd_id: Tiled Input Act (A_mk) -> Shared by PEs with same (k, m)
                # pli_id: PS Input (D_mn) -> Only for first bus (k=0), Shared by (m, n)
                # plo_id: PS Output (C_mn) -> Only for last bus (k=last), Shared by (m, n)

                if ultra_mode:
                    # Ultra Mode: Reuse tags across buses
                    ps_id = n_idx
                    pd_id = m_idx
                else:
                    # Normal Mode
                    # ps_id (Weight B)
                    ps_id = k_idx * grid_n + n_idx
                    # pd_id (Input A)
                    pd_id = k_idx * grid_m + m_idx

                # pli_id (PS Input) - used only if route_mode reads from BUS
                pli_id = (m_idx * grid_n + n_idx) if k_idx == 0 else 63

                # plo_id (PS Output) - used only if route_mode writes to BUS
                plo_id = (m_idx * grid_n + n_idx) if k_idx == grid_k - 1 else 63

                enable = True
                route_mode = r_mode
            else:
                # Inactive PE
                ps_id, pd_id, pli_id, plo_id = 63, 63, 63, 63
                enable = False
                route_mode = PERouterMode.PLI_FROM_BUS_PLO_TO_BUS # Default/Passthrough

            cfg = ScanChainConfig(
                ps_id=ps_id,
                pd_id=pd_id,
                pli_id=pli_id,
                plo_id=plo_id,
                route_mode=route_mode,
                enable=enable
            )
            scan_chain.append(cfg)

    return scan_chain


def _pe_sys_flags(operands: str) -> set:
    inner = operands.strip()
    if inner.startswith("(") and inner.endswith(")"):
        inner = inner[1:-1]
    return {flag.strip().upper() for flag in inner.split(",") if flag.strip()}


def parse_gemm_pe_program_waves(asm_text: str) -> Dict[str, int]:
    """
    Read the wave loop counts of a hand-written GEMM NoC PE program.

    Two program shapes are recognised; anything else raises ValueError:
    - cc GEMM template shape (design/hybridacc-cc/kernel/template/gemm.asm):
      an outermost ``LOOPIN n`` whose body starts with ``SYS.SYNC (SWAPDM)``
      (NUM_OF_KERNEL_LOAD_LOOP, one weight payload per N wave) directly
      containing a ``LOOPIN m`` whose body starts with a ``SYS.CTRL`` raising
      ``LDMA.ACT`` (NUM_OF_KERNEL_REUSE_LOOP, one compute pass per M wave).
    - single-payload shape (testbench/noc/gemm): one top-level SWAPDM, no wave
      loops and a VPSUMR drain that runs once; it expresses a 1 x 1 plan.

    Returns {"wave_n": ..., "wave_m": ..., "sdma_loop": ...}. ``sdma_loop`` is
    the SDMA.LOOP phase count (1 when absent: SDMA treats 0 as one phase).
    """
    if "$(" in asm_text:
        raise ValueError("PE program still contains template parameters")

    frames: List[Dict[str, Any]] = []
    stack: List[int] = []
    awaiting_first: List[int] = []
    swaps: List[Tuple[int, Tuple[int, ...]]] = []
    sdma_loops: List[int] = []
    drain_depths: List[int] = []

    for lineno, raw in enumerate(asm_text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        label = re.match(r"^[A-Za-z_]\w*:\s*(.*)$", line)
        if label:
            line = label.group(1).strip()
        if not line:
            continue
        parts = line.split(None, 1)
        mnemonic = parts[0].upper()
        operands = parts[1] if len(parts) > 1 else ""

        for idx in awaiting_first:
            frames[idx]["first"] = (mnemonic, operands)
        awaiting_first = []

        if mnemonic == "LOOPIN":
            frames.append({"count": int(operands.split()[0], 0), "line": lineno,
                           "parent": stack[-1] if stack else None, "first": None})
            stack.append(len(frames) - 1)
            awaiting_first.append(len(frames) - 1)
        elif mnemonic == "LOOPEND":
            if not stack:
                raise ValueError(f"line {lineno}: LOOPEND without LOOPIN")
            stack.pop()
        elif mnemonic == "SYS.SYNC" and "SWAPDM" in _pe_sys_flags(operands):
            swaps.append((lineno, tuple(stack)))
        elif mnemonic == "SDMA.LOOP":
            sdma_loops.append(int(operands.split()[0], 0))
        elif mnemonic in ("VPSUMR", "VPSUM"):
            drain_depths.append(len(stack))
    if stack:
        raise ValueError("unbalanced LOOPIN/LOOPEND")
    if len(swaps) != 1:
        raise ValueError(f"expected exactly one SYS.SYNC (SWAPDM), found {len(swaps)}")
    if len(sdma_loops) > 1:
        raise ValueError(f"expected at most one SDMA.LOOP, found {len(sdma_loops)}")

    def first_is(frame: Dict[str, Any], mnemonic: str, flag: str) -> bool:
        first = frame["first"]
        return first is not None and first[0] == mnemonic and flag in _pe_sys_flags(first[1])

    n_loops = [i for i, f in enumerate(frames) if first_is(f, "SYS.SYNC", "SWAPDM")]
    m_loops = [i for i, f in enumerate(frames) if first_is(f, "SYS.CTRL", "LDMA.ACT")]
    swap_line, swap_stack = swaps[0]

    if n_loops or m_loops:
        if len(n_loops) != 1 or len(m_loops) != 1:
            raise ValueError(f"expected one N-wave and one M-wave LOOPIN, found "
                             f"{len(n_loops)} and {len(m_loops)}")
        n_loop, m_loop = n_loops[0], m_loops[0]
        if frames[n_loop]["parent"] is not None or frames[m_loop]["parent"] != n_loop:
            raise ValueError("M-wave LOOPIN must sit directly inside an outermost N-wave LOOPIN")
        if swap_stack != (n_loop,):
            raise ValueError(f"line {swap_line}: SWAPDM must run once per N wave")
        wave_n, wave_m = frames[n_loop]["count"], frames[m_loop]["count"]
    else:
        if swap_stack:
            raise ValueError(f"line {swap_line}: SWAPDM inside a loop that is not an N-wave loop")
        if any(depth > 1 for depth in drain_depths):
            raise ValueError("VPSUMR drain runs more than once without wave loops")
        wave_n, wave_m = 1, 1

    return {"wave_n": wave_n, "wave_m": wave_m,
            "sdma_loop": sdma_loops[0] if sdma_loops else 1}


def check_gemm_pe_program(pe_program, plan: Dict[str, Any]) -> Dict[str, int]:
    """
    Raise ValueError unless the PE program's wave loops follow the plan.

    The cc GEMM lowering sets NUM_OF_KERNEL_LOAD_LOOP = wave_n,
    NUM_OF_KERNEL_REUSE_LOOP = wave_m and NUM_OF_KERNEL_PREFETCH_SETS
    (SDMA.LOOP) = wave_k * wave_n (hybridacc_cc/lowering.py); the hand-written
    NoC fixtures hard-code the same values. With fewer SDMA phases than N
    waves, SDMA is idle when a later N wave's weights arrive and the NoC
    stalls on PS.
    """
    found = parse_gemm_pe_program_waves(Path(pe_program).read_text())
    expected = {
        "wave_n": plan["wave_n"],
        "wave_m": plan["wave_m"],
        "sdma_loop": plan["wave_k"] * plan["wave_n"],
    }
    mismatches = [f"{key} {found[key]} != plan {value}"
                  for key, value in expected.items() if found[key] != value]
    if mismatches:
        raise ValueError(f"{pe_program}: PE program does not follow the GEMM wave plan "
                         f"({'; '.join(mismatches)})")
    return found


def plan_gemm_test(config: NocGemmConfig, pe_program=None) -> Tuple[Dict[str, Any], List[ScanChainConfig]]:
    """
    Wave plan and scan chain of a NoC GEMM test (no tensor data).

    If pe_program (path to the fixture's PE assembly) is given, its wave loops
    are checked against the plan; a mismatch raises ValueError.
    """
    plan = plan_gemm_waves(config.M, config.N, config.K, config.num_pes, config.num_bus)
    wave_grid_m, wave_grid_n = gemm_wave_grid(plan, config.ultra_mode)
    if pe_program is not None:
        check_gemm_pe_program(pe_program, plan)
    scan_chain = build_gemm_scan_chain(config.num_pes, config.num_bus, wave_grid_m, wave_grid_n,
                                       plan["grid_k"], config.ultra_mode)
    return plan, scan_chain


def generate_gemm_test(config: NocGemmConfig, pe_program=None) -> TestData:
    """
    Generate GEMM test case based on config.
    C = A * B + D (Input PS)
    Mapping strategy:
    - Tile the M, N dimensions onto a grid of PEs.
    - Split K dimension across Buses for spatial accumulation (NoC vertical accumulation).

    If pe_program (path to the fixture's PE assembly) is given, its wave loops
    are checked against the plan before any data is generated.
    """
    print("Generating GEMM test data with K-axis NoC accumulation...")
    config.validate()

    M, N, K = config.M, config.N, config.K
    num_pes = config.num_pes # Hardware Total PEs (e.g., 64)
    num_bus = config.num_bus # Hardware Buses (e.g., 3)

    plan, scan_chain = plan_gemm_test(config, pe_program)
    grid_m, grid_n, grid_k = plan["grid_m"], plan["grid_n"], plan["grid_k"]
    m_tiles_per_wave = plan["m_tiles_per_wave"]
    n_tiles_per_wave = plan["n_tiles_per_wave"]
    k_tiles_per_wave = plan["k_tiles_per_wave"]
    wave_m, wave_n, wave_k = plan["wave_m"], plan["wave_n"], plan["wave_k"]
    grid_m_per_wave = plan["grid_m_per_wave"]
    grid_n_per_wave = plan["grid_n_per_wave"]
    grid_k_per_wave = plan["grid_k_per_wave"]

    temporal_wave_count = wave_m * wave_n * wave_k

    # We map K-splits to Buses.
    # Requirement: We need at least grid_k buses to chain them vertically efficiently.
    # (Or complex folding, but assuming 1-to-1 mapping for this test)
    if grid_k > num_bus:
        print(f"Warning: K-split ({grid_k}) > Num Buses ({num_bus}). Accumulation chain might not fit simply.")
        # We proceed but data might be truncated or wrap-around logic is needed.
        # For this specific user request (K=96/32=3, Bus=3), it fits perfectly.

    torch.manual_seed(config.seed)
    np.random.seed(config.seed)

    # Generate random data
    A = torch.randn(M, K).numpy()
    B = torch.randn(K, N).numpy()
    D = torch.randn(M, N).numpy() # Input PS

    # Calculate GEMM
    C = golden_gemm(A, B, D)

    # Rename keys to match Conv2D filenames (activation, weight, partial_sum)
    # This ensures test_noc_sim.cpp loads them correctly.
    inputs = {
        "activation": A,
        "weight": B,
        "partial_sum": D
    }
    outputs = {
        "partial_sum": C
    }

    print(f"GEMM K-Split Scan-Chain Generated.")
    print(f"  Mapping: K-split {grid_k} layers mapped to first {grid_k} buses.")
    print(f"  Temporal Waves: {temporal_wave_count} (M waves: {wave_m}, N waves: {wave_n}, K waves: {wave_k})")
    print(f"  Wave Tile Size: M={m_tiles_per_wave}, N={n_tiles_per_wave}, K={k_tiles_per_wave}")
    print(f"  Per-wave tiles: M={grid_m_per_wave}, N={grid_n_per_wave}, K={grid_k_per_wave}")

    test_config = {
        "mode": "gemm",
        "M": M,
        "N": N,
        "K": K,
        "partial_sum_zero": False,
        "seed": config.seed,
        "grid_m": grid_m,
        "grid_n": grid_n,
        "grid_k": grid_k,
        "wave_m": wave_m,
        "wave_n": wave_n,
        "wave_k": wave_k,
        "grid_m_per_wave": grid_m_per_wave,
        "grid_n_per_wave": grid_n_per_wave,
        "grid_k_per_wave": grid_k_per_wave,
        "ultra_mode": "True" if config.ultra_mode else "False"
    }

    return TestData(
        name=f"gemm_{M}x{N}x{K}",
        description=f"GEMM M={M}, N={N}, K={K}, K-Split",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config=test_config
    )
