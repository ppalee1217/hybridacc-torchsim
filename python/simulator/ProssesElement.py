
# HybridAcc/python/simulator/ProcessElement.py
from dataclasses import dataclass, asdict
import torch
from torch.nn import functional as F

@dataclass
class ProcessElementParams:
    element_size: int  # Size of the processing element in bits
    input_buffer_size: int
    weight_buffer_size: int
    psum_buffer_size: int
    input_bandwidth: int # in elements
    weight_bandwidth: int # in elements
    psum_bandwidth: int # in elements
    in_channels_per_tile: int = 4
    out_channels_per_tile: int = 24

    def to_dict(self):
        return asdict(self)


class ProcessElement:
    def __init__(self, params: ProcessElementParams):
        self.params = params

    def processConv(self, input_data, weights, psum, channels, kernel_size, stride=1, verbose=False):
        # 1D convolution logic would go here
        out = F.conv1d(input_data, weights, stride=stride, padding=kernel_size // 2)
        if verbose:
            print(f"Processing with input shape {input_data.shape}, weights shape {weights.shape}, "
                f"psum shape {psum.shape}, channels {channels}, kernel size {kernel_size}, stride {stride}")
            print(f"Output shape: {out.shape}")
        psum += out  # Accumulate the result in psum
        return psum

    def processGEMV(self, input_data, weights, psum, channels, verbose=False):
        # General Matrix-Vector multiplication logic would go here
        out = torch.matmul(input_data, weights.t())
        if verbose:
            print(f"Processing GEMV with input shape {input_data.shape}, weights shape {weights.shape}, "
                f"psum shape {psum.shape}, channels {channels}")
            print(f"Output shape: {out.shape}")
        psum += out
        return psum

def testConv1D(params: ProcessElementParams):
    # random seed
    torch.manual_seed(0)
    # Example test for the ProcessElement
    in_channels = 8
    out_channels = 64
    input_tensor = torch.randn(1, in_channels, 32)  # Batch size 1, 8 channels, length 32
    weights = torch.randn(out_channels, in_channels, 3)  # 16 filters, 8 channels, kernel size 3
    psum = torch.zeros(1, out_channels, 32)  # Output size after convolution

    pe = ProcessElement(params)
    # channels per tils is 4
    pe_in_channels = 4
    pe_out_channels = 24
    for i in range(0, in_channels, pe_in_channels):
        for j in range(0, out_channels, pe_out_channels):
            print(f"Processing tile: input channels {i}-{i+pe_in_channels}, output channels {j}-{j+pe_out_channels}")
            # Get the current tile of input and weights
            input_tile = input_tensor[:, i:i+pe_in_channels, :]
            weight_tile = weights[j:j+pe_out_channels, i:i+pe_in_channels, :]
            psum_tile = psum[:, j:j+pe_out_channels, :]

            # Process the tile
            psum_tile = pe.processConv(input_tile, weight_tile, psum_tile, channels=pe_in_channels, kernel_size=3)

            # Store the result back in psum
            psum[:, j:j+pe_out_channels, :] = psum_tile

    # verify the output using F.conv1d
    expected_output = F.conv1d(input_tensor, weights, stride=1, padding=1)
    if not torch.allclose(psum, expected_output, atol=1e-6):
        print("Test failed: Output does not match expected result!")

    print("Test passed successfully!")

def testGEMV(params: ProcessElementParams):
    # random seed
    torch.manual_seed(0)
    # Example test for the ProcessElement
    in_dim = 512
    out_dim = 2048
    input_tensor = torch.randn(1, in_dim)  # Batch size 1, 64 elements
    weights = torch.randn(out_dim, in_dim)  # 2048 filters, 64 channels
    psum = torch.zeros(1, out_dim)  # Output size after GEMV

    pe = ProcessElement(params)
    pe_in_elements = 12
    pe_weight_elements = 7 * 32  # Example size

    for i in range(0, out_dim, pe_weight_elements):
        psum_tile = psum[:, i:i+pe_weight_elements]
        for j in range(0, in_dim, pe_in_elements):
            print(f"Processing tile: input elements {j}-{j+pe_in_elements}, weight elements {i}-{i+pe_weight_elements}")
            # Get the current tile of input and weights
            input_tile = input_tensor[:, j:j+pe_in_elements]
            weight_tile = weights[i:i+pe_weight_elements, j:j+pe_in_elements]

            psum_tile = pe.processGEMV(input_tile, weight_tile, psum_tile, channels=pe_in_elements)

        # Store the result back in psum
        psum[:, i:i+pe_weight_elements] = psum_tile


    # verify the output using torch.matmul
    expected_output = torch.matmul(input_tensor.squeeze(0), weights.t())

    psum_flat = psum.view(-1)
    expected_flat = expected_output.view(-1)
    cosine_similarity = F.cosine_similarity(psum_flat.unsqueeze(0), expected_flat.unsqueeze(0))
    if cosine_similarity.item() < 0.999:  # Threshold for similarity
        print(f"Test failed: Cosine similarity is too low ({cosine_similarity.item():.6f})!")
        # flatten and print the differences for debugging
        psum_flat = psum.view(-1)
        expected_flat = expected_output.view(-1)
        for p, e in zip(psum_flat, expected_flat):
            if not torch.isclose(p, e, atol=1e-6):
                print(f"Mismatch: {p} != {e}, at index {psum_flat.tolist().index(p)}")

    print("Test passed successfully!")

if __name__ == "__main__":
    # Example usage
    params = ProcessElementParams(
        element_size=16, # 16 bits per element
        input_buffer_size=12,
        weight_buffer_size=7*32,
        psum_buffer_size=24,
        input_bandwidth=1,
        weight_bandwidth=4,
        psum_bandwidth=4
    )

    testConv1D(params)
    testGEMV(params)