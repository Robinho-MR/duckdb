//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_allocator.hpp
//
// An arena for yyjson: parsing, building and writing documents costs many tiny allocations, which a bump allocator
// turns into a pointer increment. The memory comes from malloc, not from the engine, and is released all at once by
// Reset().
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_common.hpp"

#include <cstdlib>
#include <new>

namespace duckdb {
namespace json {

class JSONAllocator {
public:
	JSONAllocator() : yyjson_allocator({Allocate, Reallocate, Free, this}) {
	}
	~JSONAllocator() {
		for (auto &chunk : chunks) {
			std::free(chunk.data);
		}
	}
	// The yyjson allocator points back at this object
	JSONAllocator(const JSONAllocator &) = delete;
	JSONAllocator &operator=(const JSONAllocator &) = delete;

	inline yyjson_alc *GetYYAlc() {
		return &yyjson_allocator;
	}

	//! Frees everything allocated so far. Everything yyjson built with this allocator is invalid afterwards. The first
	//! chunk is kept for reuse.
	void Reset() {
		if (chunks.size() > 1) {
			for (idx_t i = 1; i < chunks.size(); i++) {
				std::free(chunks[i].data);
			}
			chunks.resize(1);
		}
		if (!chunks.empty() && chunks[0].size > MAX_RETAINED_SIZE) {
			std::free(chunks[0].data);
			chunks.clear();
		} else if (!chunks.empty()) {
			chunks[0].used = 0;
		}
		last_allocation = nullptr;
		last_allocation_size = 0;
	}

private:
	static constexpr idx_t INITIAL_CHUNK_SIZE = 4096;
	static constexpr idx_t MAX_CHUNK_GROWTH = 16ULL * 1024 * 1024;
	static constexpr idx_t MAX_RETAINED_SIZE = 32ULL * 1024 * 1024;
	static constexpr idx_t ALIGNMENT = 8;

	struct Chunk {
		uint8_t *data;
		idx_t size;
		idx_t used;
	};

	static inline idx_t AlignValue(idx_t value) {
		return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
	}

	void *AllocateAligned(idx_t size) {
		size = AlignValue(size == 0 ? 1 : size);
		if (chunks.empty() || chunks.back().size - chunks.back().used < size) {
			AddChunk(size);
		}
		auto &chunk = chunks.back();
		auto result = chunk.data + chunk.used;
		chunk.used += size;
		last_allocation = result;
		last_allocation_size = size;
		return result;
	}

	void AddChunk(idx_t min_size) {
		idx_t next_size = INITIAL_CHUNK_SIZE;
		if (!chunks.empty()) {
			next_size = std::min(chunks.back().size * 2, MAX_CHUNK_GROWTH);
		}
		next_size = std::max(next_size, AlignValue(min_size));
		auto data = static_cast<uint8_t *>(std::malloc(next_size));
		if (!data) {
			throw std::bad_alloc();
		}
		chunks.push_back({data, next_size, 0});
	}

	void *ReallocateAligned(void *ptr, idx_t old_size, idx_t new_size) {
		old_size = AlignValue(old_size);
		new_size = AlignValue(new_size);
		// Growing the most recent allocation in place is the common case (yyjson's output buffers)
		if (ptr == last_allocation && ptr && !chunks.empty()) {
			auto &chunk = chunks.back();
			if (new_size <= old_size) {
				chunk.used -= old_size - new_size;
				last_allocation_size = new_size;
				return ptr;
			}
			if (chunk.size - chunk.used >= new_size - old_size) {
				chunk.used += new_size - old_size;
				last_allocation_size = new_size;
				return ptr;
			}
		}
		if (new_size <= old_size) {
			return ptr;
		}
		auto result = AllocateAligned(new_size);
		if (ptr) {
			std::memcpy(result, ptr, old_size);
		}
		return result;
	}

	static inline void *Allocate(void *ctx, size_t size) {
		return static_cast<JSONAllocator *>(ctx)->AllocateAligned(size);
	}

	static inline void *Reallocate(void *ctx, void *ptr, size_t old_size, size_t size) {
		return static_cast<JSONAllocator *>(ctx)->ReallocateAligned(ptr, old_size, size);
	}

	static inline void Free(void *, void *) {
		// A bump allocator frees everything at once, in Reset()
	}

private:
	vector<Chunk> chunks;
	void *last_allocation = nullptr;
	idx_t last_allocation_size = 0;
	yyjson_alc yyjson_allocator;
};

} // namespace json
} // namespace duckdb
