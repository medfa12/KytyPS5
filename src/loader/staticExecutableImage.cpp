#include "loader/staticExecutableImage.h"

#include "common/threads.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/elf.h"

#include <cstring>
#include <exception>
#include <limits>
#include <utility>

namespace Loader {
namespace {

namespace Memory = Libs::LibKernel::Memory;
namespace VirtualMemory = Common::VirtualMemory;

VirtualMemory::Mode SegmentMode(uint32_t flags) {
	uint32_t mode = 0;
	if ((flags & PF_R) != 0) { mode |= static_cast<uint32_t>(VirtualMemory::Mode::Read); }
	if ((flags & PF_W) != 0) { mode |= static_cast<uint32_t>(VirtualMemory::Mode::Write); }
	if ((flags & PF_X) != 0) { mode |= static_cast<uint32_t>(VirtualMemory::Mode::Execute); }
	return static_cast<VirtualMemory::Mode>(mode);
}

StaticExecutableImageResult Failure(StaticExecutableImageError error,
                                    StaticExecutableLayoutError layout_error = StaticExecutableLayoutError::None) {
	StaticExecutableImageResult result;
	result.error = error;
	result.layout_error = layout_error;
	return result;
}

} // namespace

StaticExecutableImage::~StaticExecutableImage() { Reset(); }

StaticExecutableImage::StaticExecutableImage(StaticExecutableImage&& other) noexcept
    : m_layout(std::move(other.m_layout)), m_owned_count(std::exchange(other.m_owned_count, 0)) {
	other.m_layout = {};
}

StaticExecutableImage& StaticExecutableImage::operator=(StaticExecutableImage&& other) noexcept {
	if (this != &other) {
		Reset();
		m_layout = std::move(other.m_layout);
		m_owned_count = std::exchange(other.m_owned_count, 0);
		other.m_layout = {};
	}
	return *this;
}

void StaticExecutableImage::Reset() noexcept {
	while (m_owned_count != 0) {
		const auto& segment = m_layout.segments[m_owned_count - 1];
		bool freed = false;
		{
			Common::HostLockCriticalGuard critical;
			freed = Memory::FreeGuestMemory(segment.page_address, segment.page_size);
		}
		// Failure here is an ownership invariant violation: do not silently
		// discard the record and pretend the transaction was rolled back.
		if (!freed) { std::terminate(); }
		--m_owned_count;
	}
	m_layout = {};
}

StaticExecutableImageResult MapStaticExecutableImage(std::span<const uint8_t> bytes) {
	using Error = StaticExecutableImageError;
	auto planned = PlanStaticExecutableLayout(bytes);
	if (planned.error != StaticExecutableLayoutError::None) {
		return Failure(Error::InvalidLayout, planned.error);
	}

	// Validate the whole plan before taking ownership of any interval. In
	// particular the protection helper rounds sizes internally, and the
	// source must not alias a destination that this call will overwrite.
	constexpr auto limit = std::numeric_limits<uintptr_t>::max();
	const auto source = reinterpret_cast<uintptr_t>(bytes.data());
	if (bytes.size() > limit - source) { return Failure(Error::InvalidMappingRange); }
	const auto source_end = source + bytes.size();
	for (const auto& segment: planned.layout.segments) {
		if ((segment.flags & ~(PF_R | PF_W | PF_X)) != 0) { return Failure(Error::UnsupportedFlags); }
		if (segment.page_address == 0 || segment.page_address > limit || segment.page_size > limit ||
		    segment.page_size > UINT64_MAX - (StaticExecutablePageSize - 1) ||
		    segment.page_size > limit - segment.page_address) {
			return Failure(Error::InvalidMappingRange);
		}
		const auto end = segment.page_address + segment.page_size;
		if (source < end && segment.page_address < source_end) { return Failure(Error::InputOverlapsMapping); }
	}

	StaticExecutableImageResult result;
	result.image.m_layout = std::move(planned.layout);
	for (const auto& segment: result.image.m_layout.segments) {
		const auto allocated = Memory::AllocateProgramMemoryAt(segment.page_address, segment.page_size,
		                                                        VirtualMemory::Mode::ReadWrite,
		                                                        "static executable segment");
		if (allocated == 0) {
			result.image.Reset();
			return Failure(Error::AllocationFailed);
		}
		++result.image.m_owned_count;
		// The full page interval is initialized, including the prefix and tail
		// outside p_memsz, before copying the file-backed subrange.
		std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(segment.page_address)), 0,
		            static_cast<size_t>(segment.page_size));
		if (segment.file_size != 0) {
			std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(segment.address)),
			            bytes.data() + static_cast<size_t>(segment.file_offset),
			            static_cast<size_t>(segment.file_size));
		}
		bool protected_ok = false;
		{
			Common::HostLockCriticalGuard critical;
			protected_ok = Memory::ProtectGuestMemory(segment.page_address, segment.page_size,
			                                           SegmentMode(segment.flags));
		}
		if (!protected_ok) {
			result.image.Reset();
			return Failure(Error::ProtectionFailed);
		}
		if ((segment.flags & PF_X) != 0 &&
		    !VirtualMemory::FlushInstructionCache(segment.page_address, segment.page_size)) {
			result.image.Reset();
			return Failure(Error::InstructionCacheFlushFailed);
		}
	}
	return result;
}

} // namespace Loader
