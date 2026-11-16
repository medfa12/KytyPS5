#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>

namespace Libs::NpMemoryArena {

// OS coverage query only. Call outside state locks, with lifecycle exclusion
// supplied by the caller; backing must remain mapped/RW throughout its use.
bool RangeReadWrite(void* buffer, size_t size);

// Bounded allocation metadata over caller-owned backing. No internal locking,
// suspension guards, mapping ownership, callbacks, counters, or guest ABI.
// The caller serializes every operation (including queries) and protects its
// host-lock scope against suspension. Initialize requires an empty arena and
// already-validated backing; Reset invalidates blocks without unmapping it.
class Arena {
public:
	Arena() = default;
	Arena(const Arena&) = delete;
	Arena& operator=(const Arena&) = delete;

	bool Initialize(void* buffer, size_t size);
	void Reset();
	void* Allocate(size_t size);
	void* Reallocate(void* pointer, size_t size);
	void Free(void* pointer);
	size_t LiveBlocks() const;
	std::shared_ptr<void> AcquireRange(uintptr_t address, size_t size);
	bool HasLeases() const;
	bool HasPendingFrees() const;
	void Reclaim(); // metadata only; no callbacks or mapping operations

private:
	static constexpr size_t Alignment = 16;
	struct AllocationGeneration { uint64_t generation; uintptr_t address; };
	struct Block {
		uintptr_t address;
		size_t size;
		size_t requested; // zero for a free span; allocations of size zero use one byte
		uint64_t generation = 0;
		bool pending_free = false;
		std::shared_ptr<AllocationGeneration> lease {};
	};
	using Iterator = std::list<Block>::iterator;
	Iterator Find(void* pointer);
	void Release(Iterator block);
	std::list<Block> blocks;
	uint64_t next_generation = 0;
};

} // namespace Libs::NpMemoryArena
