from __future__ import annotations

import json
import struct
from pathlib import Path

from .ir import CompilerContext, HaccPackage


class ElfPackager:
    SECTION_LAYOUT = [
        (".hacc.core", "core_firmware", 0x6, 0x00000000),
        (".hacc.job", "job_section", 0x2, 0x10000000),
        (".hacc.block", "block_section", 0x2, 0),
        (".hacc.profile", "profile_section", 0x2, 0),
        (".hacc.dma", "dma_section", 0x2, 0),
        (".hacc.agu", "agu_section", 0x2, 0),
        (".hacc.nlu", "nlu_section", 0x2, 0),
        (".hacc.pe", "pe_section", 0x2, 0),
        (".hacc.scan", "scan_section", 0x2, 0),
        (".hacc.patch", "patch_section", 0x2, 0),
    ]

    @staticmethod
    def write_elf(package: HaccPackage, path: str) -> bool:
        shstrtab = bytearray(b"\x00")
        name_offsets: list[int] = []
        sections = []
        for name, attr, flags, addr in ElfPackager.SECTION_LAYOUT:
            name_offsets.append(len(shstrtab))
            shstrtab.extend(name.encode("ascii") + b"\x00")
            words = list(getattr(package, attr))
            data = struct.pack(f"<{len(words)}I", *words) if words else b""
            sections.append((name, data, flags, addr))
        shstrtab_name = len(shstrtab)
        shstrtab.extend(b".shstrtab\x00")

        offset = 52
        section_offsets: list[int] = []
        for _, data, _, _ in sections:
            offset = (offset + 3) & ~3
            section_offsets.append(offset)
            offset += len(data)
        shstrtab_offset = (offset + 3) & ~3
        offset = shstrtab_offset + len(shstrtab)
        shoff = (offset + 3) & ~3
        shnum = len(sections) + 2

        header = struct.pack(
            "<16sHHIIIIIHHHHHH",
            b"\x7fELF" + bytes([1, 1, 1]) + b"\x00" * 9,
            2,
            0,
            1,
            0,
            0,
            shoff,
            0,
            52,
            0,
            0,
            40,
            shnum,
            shnum - 1,
        )

        shdrs = [b"\x00" * 40]
        for index, (_, data, flags, addr) in enumerate(sections):
            shdrs.append(
                struct.pack(
                    "<IIIIIIIIII",
                    name_offsets[index],
                    1,
                    flags,
                    addr,
                    section_offsets[index],
                    len(data),
                    0,
                    0,
                    4,
                    0,
                )
            )
        shdrs.append(struct.pack("<IIIIIIIIII", shstrtab_name, 3, 0, 0, shstrtab_offset, len(shstrtab), 0, 0, 1, 0))

        file_path = Path(path)
        with file_path.open("wb") as handle:
            handle.write(header)
            for index, (_, data, _, _) in enumerate(sections):
                while handle.tell() < section_offsets[index]:
                    handle.write(b"\x00")
                handle.write(data)
            while handle.tell() < shstrtab_offset:
                handle.write(b"\x00")
            handle.write(shstrtab)
            while handle.tell() < shoff:
                handle.write(b"\x00")
            for shdr in shdrs:
                handle.write(shdr)
        return True

    @staticmethod
    def generate_debug_json(package: HaccPackage, ctx: CompilerContext) -> str:
        schedules = ctx.schedules or [ctx.schedule]
        primary_schedule = schedules[0]
        required_cluster_mask = 0
        for block in ctx.blocks:
            required_cluster_mask |= block.cluster_mask

        payload = {
            "stage": "stage0",
            "op_name": ctx.op.name,
            "op_type": int(ctx.op.op_type),
            "input_shape": [ctx.op.input.n, ctx.op.input.c, ctx.op.input.h, ctx.op.input.w],
            "output_shape": [ctx.op.output.n, ctx.op.output.c, ctx.op.output.h, ctx.op.output.w],
            "schedule_count": len(schedules),
            "loop_extents": list(primary_schedule.loop_extents),
            "loop_names": list(primary_schedule.loop_names),
            "total_waves": sum(schedule.total_waves() for schedule in schedules),
            "num_clusters": primary_schedule.num_clusters,
            "cluster_mask": required_cluster_mask or primary_schedule.cluster_mask,
            "sections": {
                "core_words": len(package.core_firmware),
                "job_words": len(package.job_section),
                "block_words": len(package.block_section),
                "profile_words": len(package.profile_section),
                "dma_words": len(package.dma_section),
                "agu_words": len(package.agu_section),
                "nlu_words": len(package.nlu_section),
                "pe_words": len(package.pe_section),
                "scan_words": len(package.scan_section),
                "patch_words": len(package.patch_section),
            },
            "schedules": [
                {
                    "stage_name": schedule.stage_name,
                    "loop_extents": list(schedule.loop_extents),
                    "loop_starts": list(schedule.loop_starts),
                    "loop_names": list(schedule.loop_names),
                    "total_waves": schedule.total_waves(),
                    "num_clusters": schedule.num_clusters,
                    "cluster_mask": schedule.cluster_mask,
                    "cluster_index": schedule.cluster_index,
                    "cluster_axis": schedule.cluster_axis,
                    "window_h": schedule.window_h,
                    "window_w": schedule.window_w,
                    "input_h_offset": schedule.input_h_offset,
                    "input_w_offset": schedule.input_w_offset,
                    "activation_buffer_tiles": schedule.activation_buffer_tiles,
                    "weight_buffer_tiles": schedule.weight_buffer_tiles,
                    "output_buffer_tiles": schedule.output_buffer_tiles,
                }
                for schedule in schedules
            ],
            "blocks": [
                {
                    "loop_rank": block.loop_rank,
                    "loop_extent": list(block.loop_extent),
                    "cluster_mask": block.cluster_mask,
                    "rule_stride": block.rule_stride,
                    "total_waves": block.total_waves,
                    "patch_count": block.patch_count,
                    "flags": block.block_flags,
                }
                for block in ctx.blocks
            ],
            "patch_count": len(ctx.patches),
            "nlu_rule_count": len(ctx.nlu_rules),
        }
        return json.dumps(payload, indent=2)