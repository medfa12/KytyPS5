#include "libs/ipmiClientConfig.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace A = Libs::IpmiClientAbi;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)

int main() {
	// Independently transcribed native +f40 stores, including untouched holes.
	std::array<uint8_t, 0x15a> memory;
	memory.fill(0xa7);
	using Ctor = void (KYTY_SYSV_ABI *)(void*);
	using Size = uint64_t (KYTY_SYSV_ABI *)(const void*);
	Ctor ctor = &A::Construct;
	Size estimate = &A::Estimate;
	ctor(memory.data() + 1); // Deliberately unaligned, guard at each end.
	CHECK(memory.front() == 0xa7 && memory.back() == 0xa7);
	const auto* cfg = memory.data() + 1;
	auto word = [cfg](size_t offset, size_t size) {
		uint64_t value = 0;
		for (size_t n = 0; n < size; ++n) { value |= uint64_t(cfg[offset + n]) << (8 * n); }
		return value;
	};
	CHECK(cfg[0] == 0 && cfg[0x19] == 0);
	CHECK(word(0x1c, 4) == 0 && word(0x20, 4) == 1 && word(0x38, 4) == 1);
	CHECK(word(0x28, 8) == 0xf00 && word(0x30, 8) == 0xf00);
	CHECK(word(0x40, 8) == 0xf00 && word(0x48, 8) == 0xf00 && word(0x50, 4) == 0);
	for (size_t n = 0x58; n < 0x158; ++n) { CHECK(cfg[n] == 0); }
	for (size_t n = 1; n < 0x19; ++n) { CHECK(cfg[n] == 0xa7); }
	for (size_t n: {0x1a, 0x1b, 0x24, 0x27, 0x3c, 0x3f, 0x54, 0x57}) { CHECK(cfg[n] == 0xa7); }
	CHECK(estimate(cfg) == 0x1000 && estimate(nullptr) == 0x1000);
	// LoginMgr's inline backing limit; DbNotify still receives nonzero storage.
	CHECK(estimate(cfg) > 0 && estimate(cfg) <= 0x4000);
	std::cout << "Native Client::Config ABI/default/backing checks passed\n";
}
