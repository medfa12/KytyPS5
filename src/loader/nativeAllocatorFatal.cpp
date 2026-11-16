#include "loader/nativeAllocatorFatal.h"

#include <algorithm>
#include <atomic>

namespace Loader::NativeAllocatorFatal {
namespace {
// Preserve 32 legacy registrations / 96-slot admission. Release-publishing the
// state makes an entire validated profile visible, never a registered prefix.
struct Batch {
	std::array<std::atomic<uint64_t>, ProfileSites.size()> sites {};
	// Low byte: published width. High bits: retirement generation, preventing a
	// reader of an old batch from accepting partially rewritten reused storage.
	std::atomic<uint64_t> state {0};
};
std::array<Batch, 32> fatal_batches {};
constexpr uint64_t RetiredGeneration = UINT64_MAX & ~uint64_t {0xff};
static_assert(std::atomic<uint64_t>::is_always_lock_free);

bool RegisterSites(std::span<const uint64_t> sites) {
	if (sites.empty() || sites.size() > ProfileSites.size()) { return false; }
	for (size_t i = 0; i < sites.size(); ++i) {
		if (sites[i] == 0 || Contains(sites[i]) ||
		    std::find(sites.begin(), sites.begin() + i, sites[i]) != sites.begin() + i) { return false; }
	}
	Batch* available = nullptr;
	size_t used = 0;
	for (auto& batch: fatal_batches) {
		const auto state = batch.state.load(std::memory_order_acquire);
		const auto width = state & 0xff;
		used += width;
		if (width == 0 && state != RetiredGeneration && available == nullptr) { available = &batch; }
	}
	if (available == nullptr || used > 96 || sites.size() > 96 - used) { return false; }
	for (size_t i = 0; i < sites.size(); ++i) { available->sites[i].store(sites[i], std::memory_order_release); }
	available->state.fetch_or(sites.size(), std::memory_order_release);
	return true;
}
}

bool Register(const std::array<uint64_t, 3>& sites) { return RegisterSites(sites); }

bool RegisterProfile(uint64_t base, std::span<const uint8_t> image) {
	if (base == 0 || base > UINT64_MAX - ProfileExtent || !MatchProfile(image)) { return false; }
	std::array<uint64_t, ProfileSites.size()> sites {};
	for (size_t i = 0; i < sites.size(); ++i) { sites[i] = base + ProfileSites[i]; }
	return RegisterSites(sites);
}

bool ContainsProfile(uint64_t base) {
	if (base == 0 || base > UINT64_MAX - ProfileExtent) { return false; }
	for (auto& batch: fatal_batches) {
		const auto state = batch.state.load(std::memory_order_acquire);
		if ((state & 0xff) != ProfileSites.size()) { continue; }
		bool matches = true;
		for (size_t i = 0; i < ProfileSites.size(); ++i) {
			matches &= batch.sites[i].load(std::memory_order_acquire) == base + ProfileSites[i];
		}
		if (matches && batch.state.load(std::memory_order_acquire) == state) { return true; }
	}
	return false;
}

void Unregister(uint64_t base, uint64_t size) {
	for (auto& batch: fatal_batches) {
		const auto state = batch.state.load(std::memory_order_acquire);
		const auto width = state & 0xff;
		if (width == 0) { continue; }
		size_t survivors = 0;
		bool intersects = false;
		for (size_t i = 0; i < width; ++i) {
			const auto pc = batch.sites[i].load(std::memory_order_acquire);
			if (pc == 0) { continue; }
			if (pc >= base && pc - base < size) { intersects = true; } else { ++survivors; }
		}
		if (!intersects) { continue; }
		const auto generation = state & ~uint64_t {0xff};
		// Saturation permanently prevents reuse instead of wrapping the generation.
		const auto retired = generation == RetiredGeneration ? generation : generation + 0x100;
		batch.state.store(retired, std::memory_order_release);
		if (survivors != 0) {
			// Preserve partial-range retirement; production retires whole providers
			// after execution quiesces. Partial holes retain their slot reservation.
			for (size_t i = 0; i < width; ++i) {
				const auto pc = batch.sites[i].load(std::memory_order_acquire);
				if (pc >= base && pc - base < size) { batch.sites[i].store(0, std::memory_order_release); }
			}
			batch.state.store(retired | width, std::memory_order_release);
		}
	}
}

bool Contains(uint64_t pc) {
	if (pc == 0) { return false; }
	for (auto& batch: fatal_batches) {
		const auto state = batch.state.load(std::memory_order_acquire);
		const auto width = state & 0xff;
		bool found = false;
		for (size_t i = 0; i < width; ++i) {
			found |= batch.sites[i].load(std::memory_order_acquire) == pc;
		}
		if (found && batch.state.load(std::memory_order_acquire) == state) { return true; }
	}
	return false;
}

bool MatchCode(std::span<const uint8_t> helper, std::span<const uint8_t> caller) {
	constexpr std::array<uint8_t, 21> expected_helper {
	    0x55, 0x48, 0x89, 0xe5,
	    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
	    0xcd, 0x45, 0x90, 0x0f, 0x0b,
	};
	constexpr std::array<uint8_t, 7> expected_caller {0xe8, 0x0b, 0x21, 0, 0, 0x0f, 0x0b};
	return helper.size() == expected_helper.size() && caller.size() == expected_caller.size() &&
	       std::equal(helper.begin(), helper.end(), expected_helper.begin()) &&
	       std::equal(caller.begin(), caller.end(), expected_caller.begin());
}

bool MatchProfile(std::span<const uint8_t> image) {
	for (const auto& range: ProfileRanges) {
		if (range.rva > image.size() || range.size > image.size() - range.rva) { return false; }
	}
	if (!MatchCode(image.subspan(0x150b0, 21), image.subspan(0x12fa0, 7))) { return false; }
	// test eax,eax; je success; twelve adapted NOPs; INT45; native successors.
	// None of these five INT45s has an immediately following fallback UD2.
	constexpr std::array<std::array<uint8_t, 11>, 5> suffixes {{
	    {0x8b, 0x0f, 0x48, 0x8d, 0x47, 0x40},
	    {0xb8, 0x01, 0x00, 0x00, 0x00},
	    {0x41, 0x89, 0xc4, 0x41, 0x8b, 0x86, 0xf4, 0x03, 0x00, 0x00},
	    {0x8b, 0x83, 0x10, 0x04, 0x00, 0x00},
	    {0x48, 0x83, 0xc4, 0x08, 0x5b, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3},
	}};
	for (size_t i = 0; i < suffixes.size(); ++i) {
		const auto& range = ProfileRanges[i + 2];
		const auto code = image.subspan(range.rva, range.size);
		if (code[0] != 0x85 || code[1] != 0xc0 || code[2] != 0x74 || code[3] != (i == 2 ? 0x11 : 0x0e) ||
		    !std::all_of(code.begin() + 4, code.begin() + 16, [](uint8_t b) { return b == 0x90; }) ||
		    code[16] != 0xcd || code[17] != 0x45 ||
		    !std::equal(code.begin() + 18, code.end(), suffixes[i].begin())) { return false; }
	}
	return true;
}

} // namespace Loader::NativeAllocatorFatal
