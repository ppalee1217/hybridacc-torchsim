# HybridAcc Python Behavioral Simulator

## compute core
### support operations (per waves)
1. **Convolution 2D**: Implement convolution operations for processing input data with various kernels.
    - 1x1 Convolution:
        - input channels: 12, output channels: 16
    - 3x3 Convolution:
        - input channels: 4, output channels: 24
    - 5x5 Convolution:
        - input channels: 2, output channels: 32
    - 7x7 Convolution:
        - input channels: 1, output channels: 40

## Convolution Tiling Strategy

### Hardware Constraints
- 4 rows of PEs available
- Each PE can compute 1D convolution
- Same row PEs can compute different output heights
- Vertical column outputs can be accumulated for different kernel rows

### Tiling Implementation for Large Kernels

#### 5x5 Convolution Tiling
```
Wave 1: Process kernel rows 0-2 (3 rows)
- Row 0: kernel_row=0, input_height=16*stride
- Row 1: kernel_row=1, input_height=16*stride
- Row 2: kernel_row=2, input_height=16*stride
- Row 3: unused

Wave 2: Process kernel rows 3-4 (2 rows)
- Row 0: kernel_row=3, input_height=16*stride
- Row 1: kernel_row=4, input_height=16*stride
- Row 2,3: unused

Accumulate results from Wave 1 and Wave 2
```

#### 7x7 Convolution Tiling
```
Wave 1: Process kernel rows 0-3 (4 rows)
- Row 0: kernel_row=0, input_height=16*stride
- Row 1: kernel_row=1, input_height=16*stride
- Row 2: kernel_row=2, input_height=16*stride
- Row 3: kernel_row=3, input_height=16*stride

Wave 2: Process kernel rows 4-6 (3 rows)
- Row 0: kernel_row=4, input_height=16*stride
- Row 1: kernel_row=5, input_height=16*stride
- Row 2: kernel_row=6, input_height=16*stride
- Row 3: unused

Accumulate results from Wave 1 and Wave 2
```

### Core Wave Specifications

#### Per PE Computation (1D Convolution)
- 1x1: in_ch=12, out_ch=16, h=1, w=input_width
- 3x3: in_ch=4, out_ch=16, h=1, w=input_width
- 5x5: in_ch=2, out_ch=16, h=1, w=input_width
- 7x7: in_ch=1, out_ch=16, h=1, w=input_width

#### Per Core Wave Computation (2D Convolution)
- 1x1: in_ch=12, out_ch=16, h=64*stride, w=input_width, k_h=1 (single wave)
- 3x3: in_ch=4, out_ch=16, h=16*stride, w=input_width, k_h=3 (single wave)
- 5x5: in_ch=2, out_ch=16, h=16*stride, w=input_width, k_h=3+2 (two waves)
- 7x7: in_ch=1, out_ch=16, h=16*stride, w=input_width, k_h=4+3 (two waves)

### Implementation Tasks
1. **Tiling Scheduler**: Split large kernels into multiple waves
2. **Memory Management**: Handle intermediate results between waves
3. **Accumulator**: Combine partial results from different waves
4. **Resource Allocation**: Optimize PE utilization across waves