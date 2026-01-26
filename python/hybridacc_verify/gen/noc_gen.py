import torch
import math
import numpy as np
from typing import Dict, Any, List
from ..utils.config import ScanChainConfig, PERouterMode, NocConvConfig, NocGemmConfig
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
        temporal_wave_count = math.ceil(out_h_final / num_pes_per_bus)

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
                    pli_id = j if i==0 else 63
                    plo_id = j if i==split_kh-1 else 63

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

def generate_gemm_test(config: NocGemmConfig) -> TestData:
    """
    Generate GEMM test case based on config.
    C = A * B + D (Input PS)
    Mapping strategy:
    - Tile the M, N dimensions onto a grid of PEs.
    - Split K dimension across Buses for spatial accumulation (NoC vertical accumulation).
    """
    print("Generating GEMM test data with K-axis NoC accumulation...")
    config.validate()

    M, N, K = config.M, config.N, config.K
    num_pes = config.num_pes # Hardware Total PEs (e.g., 64)
    num_bus = config.num_bus # Hardware Buses (e.g., 3)

    # PE Capability
    PE_M, PE_N = 12, 8
    PE_K = 32 # PE processes 32 K-dim per step/pass

    # Calculate Grid Size
    grid_m = (M + PE_M - 1) // PE_M
    grid_n = (N + PE_N - 1) // PE_N
    grid_k = (K + PE_K - 1) // PE_K  # K-splits

    print(f"Grid Layout: M={grid_m}, N={grid_n}, K_split={grid_k}")

    # Total PEs needed: M_grid * N_grid * K_grid
    active_pes_count = grid_m * grid_n * grid_k

    # Calculate Temporal Waves if hardware resources are insufficient
    pes_per_bus = num_pes // num_bus
    pes_per_layer = grid_m * grid_n # PEs needed for one K-slice (one bus)

    # Number of waves needed inside a bus to cover M*N grid
    mn_waves = math.ceil(pes_per_layer / pes_per_bus)

    # Number of waves needed for K-dimension (if K-splits > Buses)
    if num_bus > 0:
        k_waves = math.ceil(grid_k / num_bus)
    else:
        k_waves = 1

    temporal_wave_count = mn_waves * k_waves

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
            # Simple Row-Major mapping of the MxN grid
            # Capability per Bus: pes_per_bus
            # Required: grid_m * grid_n

            m_idx = j // grid_n
            n_idx = j % grid_n

            is_active_pe = is_active_k_layer and (m_idx < grid_m)

            if is_active_pe:
                # Active PE
                # ps_id: Tiled Weight (B_kn) -> Shared by PEs with same (k, n)
                # pd_id: Tiled Input Act (A_mk) -> Shared by PEs with same (k, m)
                # pli_id: PS Input (D_mn) -> Only for first bus (k=0), Shared by (m, n)
                # plo_id: PS Output (C_mn) -> Only for last bus (k=last), Shared by (m, n)

                if config.ultra_mode:
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

    print(f"GEMM K-Split Scan-Chain Generated.")
    print(f"  Mapping: K-split {grid_k} layers mapped to first {grid_k} buses.")
    print(f"  Temporal Waves: {temporal_wave_count} (MN waves: {mn_waves}, K waves: {k_waves})")

    test_config = {
        "mode": "gemm",
        "temporal_wave_count": temporal_wave_count,
        "M": M,
        "N": N,
        "K": K,
        "partial_sum_zero": False,
        "seed": config.seed,
        "grid_rows": grid_m,
        "grid_cols": grid_n,
        "grid_k": grid_k,
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
