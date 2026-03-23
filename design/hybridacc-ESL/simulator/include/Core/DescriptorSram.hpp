#pragma once

#include "Core/Types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hybridacc::core {

/**
 * @brief Local descriptor SRAM image plus typed section decoding helpers.
 *
 * The model stores raw 32-bit words for inspection and decodes the `.hacc.job`
 * and `.hacc.block` sections into structured descriptors that can be loaded into
 * the execution complex.
 */
class DescriptorSram {
public:
	explicit DescriptorSram(std::size_t word_count = 16384) : words_(word_count, 0u) {}

	void reset() {
		std::fill(words_.begin(), words_.end(), 0u);
	}

	void write_word(uint32_t byte_addr, uint32_t value) {
		const std::size_t index = byte_addr / 4u;
		if (index >= words_.size()) {
			words_.resize(index + 1u, 0u);
		}
		words_[index] = value;
	}

	uint32_t read_word(uint32_t byte_addr) const {
		const std::size_t index = byte_addr / 4u;
		return (index < words_.size()) ? words_[index] : 0u;
	}

	std::vector<HaccJobDesc> decode_job_section(uint32_t dst_base, const std::vector<uint32_t>& payload_words) const {
		std::vector<HaccJobDesc> jobs;
		const std::size_t base_index = dst_base / 4u;
		const std::size_t total_words = payload_words.size();
		for (std::size_t i = 0; i + 15 < total_words; i += 16) {
			const std::size_t idx = base_index + i;
			HaccJobDesc job{};
			job.version = words_.at(idx + 0);
			job.flags = words_.at(idx + 1);
			job.block_desc_base = words_.at(idx + 2);
			job.block_desc_count = words_.at(idx + 3);
			job.profile_table_base = words_.at(idx + 4);
			job.profile_table_count = words_.at(idx + 5);
			job.dma_rule_base = words_.at(idx + 6);
			job.dma_rule_count = words_.at(idx + 7);
			job.agu_rule_base = words_.at(idx + 8);
			job.agu_rule_count = words_.at(idx + 9);
			job.patch_base = words_.at(idx + 10);
			job.patch_count = words_.at(idx + 11);
			job.pe_program_base = words_.at(idx + 12);
			job.scan_chain_base = words_.at(idx + 13);
			job.required_cluster_mask = words_.at(idx + 14);
			job.required_caps = words_.at(idx + 15);
			jobs.push_back(job);
		}
		return jobs;
	}

	std::vector<HaccBlockDesc> decode_block_section(uint32_t dst_base, const std::vector<uint32_t>& payload_words) const {
		std::vector<HaccBlockDesc> blocks;
		const std::size_t base_index = dst_base / 4u;
		const std::size_t total_words = payload_words.size();
		for (std::size_t i = 0; i + 7 < total_words; i += 8) {
			const std::size_t idx = base_index + i;
			HaccBlockDesc block{};
			block.block_id = words_.at(idx + 0);
			block.profile_id = static_cast<uint16_t>(words_.at(idx + 1) & 0xFFFFu);
			block.pe_program_id = static_cast<uint16_t>((words_.at(idx + 1) >> 16) & 0xFFFFu);
			block.loop_rank = static_cast<uint16_t>(words_.at(idx + 2) & 0xFFFFu);
			block.flags = static_cast<uint16_t>((words_.at(idx + 2) >> 16) & 0xFFFFu);
			block.repeat_count = words_.at(idx + 3);
			block.cluster_mask = words_.at(idx + 4);
			block.section_map_seed = words_.at(idx + 5);
			blocks.push_back(block);
		}
		return blocks;
	}

private:
	std::vector<uint32_t> words_;
};

} // namespace hybridacc::core