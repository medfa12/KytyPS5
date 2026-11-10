// Included in VirtualMemoryAllocationTests.cpp; guard success AND mismatch.
void TestGuestFsTcbReads() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	constexpr const char* test = "GuestFsTcbReads";
	using Loader::X64InstructionEmulator::TryEmulateFsTcbRead;
	std::array<uint8_t, 0x40> tcb {};
	const uint64_t guard = 0x123456789abcde00ull;
	std::memcpy(tcb.data() + 0x28, &guard, sizeof(guard));
	std::array<uint8_t, 9> load {0x64, 0x48, 0x8b, 0x04, 0x25, 0x28, 0, 0, 0};
	CONTEXT c {}; c.Rip = 100; c.Rdi = 0x4321; c.EFlags = 0xad7;
	Check(test, TryEmulateFsTcbRead(&c, load, tcb) && c.Rax == guard && c.Rip == 109 &&
	            c.Rdi == 0x4321 && c.EFlags == 0xad7, "MOV reads actual TCB and preserves flags/registers");
	const std::array<uint8_t, 9> compare {0x64, 0x48, 0x33, 0x3c, 0x25, 0x28, 0, 0, 0};
	for (const uint64_t difference: {0ull, 1ull, 0x8000000000000003ull}) {
		c.Rdi = guard ^ difference; c.EFlags = 0xad7;
		Check(test, TryEmulateFsTcbRead(&c, compare, tcb) && c.Rdi == difference && c.Rax == guard,
		      "XOR retains arithmetic result, not unconditional guard success");
		Check(test, bool(c.EFlags & 0x40) == (difference == 0) && bool(c.EFlags & 0x80) == bool(difference >> 63) &&
		            !(c.EFlags & 0x801) && (c.EFlags & 0x202) == 0x202,
		      "guard mismatch changes branch flags while preserving control flags");
	}
	load[1] = 0x4c; load[3] = 0x3c;
	Check(test, TryEmulateFsTcbRead(&c, load, tcb) && c.R15 == guard, "extended destination register");
	const auto before = c;
	load[0] = 0x65;
	Check(test, !TryEmulateFsTcbRead(&c, load, tcb) && std::memcmp(&before, &c, sizeof(c)) == 0,
	      "GS and unsupported addressing do not mutate context");
	load[0] = 0x64;
	Check(test, !TryEmulateFsTcbRead(&c, load, std::span(tcb).first(0x2f)), "short TCB rejected");
#endif
}
