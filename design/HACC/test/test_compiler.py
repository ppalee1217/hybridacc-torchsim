from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from hacc import BlockFlag, DataType, Frontend, HaccCompiler, HwConstraint


class HaccCompilerTests(unittest.TestCase):
    def test_conv2d_3x3(self) -> None:
        op = Frontend.create_conv2d("conv3x3_resnet", DataType.INT16, 1, 64, 56, 56, 128, 3, 3, 1, 1, 1, 1)
        self.assertEqual(op.output.h, 56)
        self.assertEqual(op.output.w, 56)
        self.assertEqual(op.output.c, 128)

        result = HaccCompiler().compile(op)
        self.assertTrue(result.success)
        self.assertGreater(result.context.schedule.total_waves(), 0)
        self.assertGreater(len(result.context.blocks), 0)
        self.assertGreater(len(result.package.core_firmware), 0)
        self.assertGreater(len(result.package.pe_section), 0)
        self.assertGreater(len(result.package.block_section), 0)
        self.assertGreater(len(result.package.profile_section), 0)

    def test_conv2d_1x1(self) -> None:
        op = Frontend.create_conv2d("conv1x1_pointwise", DataType.INT16, 1, 128, 28, 28, 256, 1, 1, 1, 1, 0, 0)
        result = HaccCompiler().compile(op)
        self.assertTrue(result.success)
        self.assertEqual(len(result.package.job_section), 19)
        self.assertGreater(len(result.package.pe_section), 0)
        self.assertEqual(len(result.package.pe_section) % 36, 0)

    def test_pe_payload_is_patched_from_template_json(self) -> None:
        conv = Frontend.create_conv2d("conv_template_json", DataType.INT16, 1, 12, 8, 8, 32, 1, 1, 1, 1, 0, 0)
        conv_result = HaccCompiler().compile(conv)
        self.assertTrue(conv_result.success)
        self.assertEqual(conv_result.package.pe_section[0], 0x004C)
        self.assertEqual(conv_result.package.pe_section[1], 0x0004)
        self.assertEqual(conv_result.package.pe_section[3], 0x0BE8)
        self.assertEqual(conv_result.package.pe_section[15], 0x0184)

        gemm = Frontend.create_gemm("gemm_template_json", DataType.INT16, 12, 8, 32)
        gemm_result = HaccCompiler().compile(gemm)
        self.assertTrue(gemm_result.success)
        self.assertEqual(gemm_result.package.pe_section[0], 0x004C)
        self.assertEqual(gemm_result.package.pe_section[1], 0x07F0)
        self.assertEqual(gemm_result.package.pe_section[22], 0x05C4)

    def test_conv_template_loop_count_is_factorized(self) -> None:
        factorized = HaccCompilerTests._factorize_conv_loop_trip_count(3840)
        self.assertEqual((factorized.inner_count, factorized.outer_count), (960, 4))

    @staticmethod
    def _factorize_conv_loop_trip_count(total_count: int):
        from hacc.cluster_lowering import Conv2dLowering

        return Conv2dLowering.factorize_loop_trip_count(total_count, "kernel sets")

    def test_conv2d_with_nlu(self) -> None:
        op = Frontend.create_conv2d("conv3x3_relu", DataType.INT16, 1, 32, 14, 14, 64, 3, 3, 1, 1, 1, 1, True)
        result = HaccCompiler().compile(op)
        self.assertTrue(result.success)
        self.assertGreater(len(result.context.nlu_rules), 0)
        self.assertTrue(any(block.block_flags & int(BlockFlag.BLOCK_FLAG_NLU_PHASE) for block in result.context.blocks))

    def test_gemm_medium(self) -> None:
        op = Frontend.create_gemm("gemm_medium", DataType.INT16, 256, 256, 512)
        result = HaccCompiler().compile(op)
        self.assertTrue(result.success)
        self.assertGreater(result.context.schedule.total_waves(), 0)
        self.assertGreater(len(result.package.core_firmware), 0)
        self.assertGreater(len(result.package.dma_section), 0)

    def test_gemm_large(self) -> None:
        compiler = HaccCompiler(HwConstraint(num_clusters=8))
        op = Frontend.create_gemm("gemm_large", DataType.INT16, 1024, 1024, 2048)
        result = compiler.compile(op)
        self.assertTrue(result.success)
        self.assertLessEqual(result.context.schedule.num_clusters, 8)
        self.assertGreaterEqual(len(result.context.blocks), 1)
        self.assertGreaterEqual(len(result.context.dma_rules), result.context.schedule.total_waves())

    def test_debug_json(self) -> None:
        op = Frontend.create_conv2d("conv_json_test", DataType.INT16, 1, 16, 8, 8, 32, 3, 3, 1, 1, 1, 1)
        result = HaccCompiler().compile(op)
        self.assertTrue(result.success)
        self.assertIn('"stage": "stage0"', result.debug_json)
        payload = json.loads(result.debug_json)
        self.assertEqual(payload["op_name"], "conv_json_test")
        self.assertIn("blocks", payload)

    def test_elf_output(self) -> None:
        op = Frontend.create_conv2d("conv_elf_test", DataType.INT16, 1, 16, 8, 8, 32, 3, 3, 1, 1, 1, 1)
        compiler = HaccCompiler()
        result = compiler.compile(op)
        self.assertTrue(result.success)
        with tempfile.TemporaryDirectory() as tmpdir:
            prefix = Path(tmpdir) / "hacc_elf_test"
            self.assertTrue(compiler.write_outputs(result, str(prefix)))
            elf_data = prefix.with_suffix(".hacc.elf").read_bytes()
            self.assertEqual(elf_data[:4], b"\x7fELF")

    def test_block_compression(self) -> None:
        compiler = HaccCompiler(HwConstraint(num_clusters=8))
        op = Frontend.create_gemm("gemm_compress", DataType.INT16, 1024, 1024, 2048)
        result = compiler.compile(op)
        self.assertTrue(result.success)
        self.assertGreaterEqual(len(result.context.blocks), 1)
        self.assertEqual(sum(1 for block in result.context.blocks if block.block_flags & int(BlockFlag.BLOCK_FLAG_FIRST_BLOCK)), 1)
        self.assertEqual(sum(1 for block in result.context.blocks if block.block_flags & int(BlockFlag.BLOCK_FLAG_LAST_BLOCK)), 1)
        self.assertTrue(all(block.rule_stride > 0 for block in result.context.blocks))

    def test_conv2d_cluster_partition_regression(self) -> None:
        compiler = HaccCompiler(HwConstraint(num_clusters=2))
        op = Frontend.create_conv2d("conv5x5_partition", DataType.INT16, 1, 12, 16, 16, 32, 5, 5, 1, 1, 0, 0)
        result = compiler.compile(op)
        self.assertTrue(result.success)
        self.assertEqual(len(result.context.schedules), 2)
        self.assertEqual(len(result.context.blocks), 4)
        self.assertEqual({block.cluster_mask for block in result.context.blocks}, {1, 2})
        self.assertTrue(all(schedule.cluster_axis == "tile_oc" for schedule in result.context.schedules))

    def test_conv2d_width_tiling_respects_group_buffer(self) -> None:
        compiler = HaccCompiler(HwConstraint(spm_bank_size_kb=1, num_clusters=2))
        op = Frontend.create_conv2d("conv_width_tile", DataType.INT16, 1, 12, 16, 128, 32, 3, 3, 1, 1, 1, 1)
        result = compiler.compile(op)
        self.assertTrue(result.success)
        schedule = result.context.schedules[0]
        self.assertLess(schedule.conv_tile().tile_w, op.output.w)
        self.assertGreater(schedule.loop_extents[1], 1)
        self.assertGreaterEqual(schedule.activation_buffer_tiles, 1)
        self.assertGreaterEqual(schedule.output_buffer_tiles, 1)

    def test_gemm_cluster_partition_regression(self) -> None:
        compiler = HaccCompiler(HwConstraint(num_clusters=2))
        op = Frontend.create_gemm("gemm_partition", DataType.INT16, 37, 65, 96)
        result = compiler.compile(op)
        self.assertTrue(result.success)
        self.assertEqual(result.context.schedules[0].cluster_axis, "tile_n")
        self.assertEqual(len(result.context.blocks), 2)
        self.assertEqual([block.cluster_mask for block in result.context.blocks], [1, 2])
        self.assertGreaterEqual(len(result.context.profiles), result.context.schedule.total_waves())

    def test_gemm_buffer_reuse_metadata(self) -> None:
        compiler = HaccCompiler(HwConstraint(spm_bank_size_kb=4, num_clusters=2))
        op = Frontend.create_gemm("gemm_reuse", DataType.INT16, 128, 128, 256)
        result = compiler.compile(op)
        self.assertTrue(result.success)
        schedule = result.context.schedules[0]
        tile = schedule.gemm_tile()
        self.assertEqual(tile.tile_k, 32)
        self.assertGreaterEqual(schedule.activation_buffer_tiles, 1)
        self.assertGreaterEqual(schedule.weight_buffer_tiles, 1)
        self.assertGreaterEqual(schedule.output_buffer_tiles, 1)

    def test_debug_json_includes_schedule_partition_metadata(self) -> None:
        compiler = HaccCompiler(HwConstraint(num_clusters=2))
        op = Frontend.create_gemm("gemm_debug_partition", DataType.INT16, 37, 65, 96)
        result = compiler.compile(op)
        self.assertTrue(result.success)
        payload = json.loads(result.debug_json)
        self.assertEqual(payload["schedule_count"], 1)
        self.assertIn("loop_starts", payload["schedules"][0])
        self.assertIn("cluster_axis", payload["schedules"][0])
        self.assertIn("activation_buffer_tiles", payload["schedules"][0])


if __name__ == "__main__":
    unittest.main()