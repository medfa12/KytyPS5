// Included inside VirtualMemoryAllocationTests.cpp's anonymous namespace.
void TestFixedProgramMemoryAllocation() {
	using namespace Libs::LibKernel::Memory;
	using Mode = Common::VirtualMemory::Mode;
	const char* test = "FixedProgramMemoryAllocation";
	constexpr uint64_t base = 0x400000;
	constexpr uint64_t page = SceKernelPageSize;
	constexpr uint64_t requested = page + 17;
	constexpr uint64_t extent = page * 2;
	constexpr uint64_t adjacent = base + extent;
	constexpr uint64_t probe = adjacent + page;
	const auto entry_depth = Common::GetHostLockCriticalCounter()->load();

	// Release successful allocations even if a Check throws. Never release an
	// allocation that this test did not successfully create.
	struct Cleanup {
		uint64_t main = 0;
		uint64_t data = 0;
		~Cleanup() {
			if (data) { FreeGuestMemory(data, page); }
			if (main) { FreeGuestMemory(main, extent); }
		}
	} cleanup;

	Check(test, TestGuestAddressRangeIsOwned(base, extent + page), "fixed test range not owned");
	Check(test, TestPlaceholderRangeIsFree(base, extent + 2 * page), "fixture precondition: fixed test range not initially free");
	uint64_t allocated = 0;
	bool nested_depth_restored = false;
	{
		Common::HostLockCriticalGuard outer;
		const auto nested_depth = Common::GetHostLockCriticalCounter()->load();
		allocated = AllocateProgramMemoryAt(base, requested, Mode::ExecuteReadWrite, "fixed_program");
		nested_depth_restored = nested_depth > entry_depth &&
		                        Common::GetHostLockCriticalCounter()->load() == nested_depth;
	}
	cleanup.main = allocated;
	Check(test, nested_depth_restored, "nested critical depth not restored after successful allocation");
	Check(test, Common::GetHostLockCriticalCounter()->load() == entry_depth, "successful allocation leaked critical depth");
	Check(test, allocated == base, "fixed program allocation failed or moved");
	Check(test, !TestPlaceholderRangeIsFree(base, extent), "program owner not committed");
	Check(test, TestPlaceholderRangeIsFree(base - page, page) &&
	                TestPlaceholderRangeIsFree(adjacent, page), "allocation exceeded rounded bounds");
	ExpectRange(test, Query(test, base), base, base + extent,
	            SceKernelProtCpuRead | SceKernelProtCpuRw | SceKernelProtCpuExec,
	            0, 0, 0, 1, "fixed_program");
	const auto* initial = reinterpret_cast<const uint8_t*>(base);
	Check(test, std::all_of(initial, initial + extent, [](uint8_t v) { return v == 0; }),
	      "initial rounded allocation is not zero-filled");
	auto* bytes = reinterpret_cast<uint8_t*>(base);
	bytes[0] = 0x53; bytes[extent - 1] = 0xa7;

	const auto collision = [&](uint64_t address, uint64_t size, const char* label) {
		const auto result = AllocateProgramMemoryAt(address, size, Mode::ReadWrite, "collision");
		// A hypothetical incorrect fallback is ours to release, but an occupied
		// address must never be freed as though this call created it.
		if (result != 0 && (result < base || result >= base + extent)) {
			FreeGuestMemory(result, (size + page - 1) & ~(page - 1));
		}
		Check(test, result == 0, label);
	};
	collision(base, extent, "exact collision did not fail");
	collision(base + page, 2 * page, "partial collision did not fail");
	Check(test, Common::GetHostLockCriticalCounter()->load() == entry_depth, "collision leaked critical depth");
	Check(test, bytes[0] == 0x53 && bytes[extent - 1] == 0xa7, "collision changed existing bytes");
	ExpectRange(test, Query(test, base), base, base + extent,
	            SceKernelProtCpuRead | SceKernelProtCpuRw | SceKernelProtCpuExec,
	            0, 0, 0, 1, "fixed_program");
	Check(test, TestPlaceholderRangeIsFree(base - page, page) &&
	                TestPlaceholderRangeIsFree(adjacent, page), "collision consumed adjacent placeholder");

	const auto data = AllocateProgramMemoryAt(adjacent, page, Mode::ReadWrite, "fixed_data");
	// Do not release the already-owned main allocation if a broken allocator
	// returns an overlapping address rather than a new mapping.
	if (data != 0 && (data < base || data >= base + extent)) { cleanup.data = data; }
	Check(test, data == adjacent, "adjacent independent allocation failed or moved");
	ExpectRange(test, Query(test, adjacent), adjacent, adjacent + page,
	            SceKernelProtCpuRead | SceKernelProtCpuRw, 0, 0, 0, 1, "fixed_data");

	constexpr uint64_t value = 0x8a7152c3de4906bfull;
	constexpr uint64_t data_address = adjacent + 0x80;
	std::memcpy(reinterpret_cast<void*>(data_address), &value, sizeof(value));
#if defined(__x86_64__) || defined(_M_X64)
	// A synthetic x86-64 function, not a firmware image: movabs rax,[absolute
	// data address]; ret. Its literal points into a separate RW allocation.
	std::array<uint8_t, 11> code {0x48, 0xa1, 0, 0, 0, 0, 0, 0, 0, 0, 0xc3};
	for (size_t i = 0; i < 8; ++i) { code[2 + i] = static_cast<uint8_t>(data_address >> (8 * i)); }
	std::memcpy(bytes, code.data(), code.size());
	Check(test, Common::VirtualMemory::FlushInstructionCache(base, code.size()), "instruction cache flush failed");
	using Function = uint64_t(KYTY_SYSV_ABI*)();
	Check(test, reinterpret_cast<Function>(base)() == value, "fixed code did not read absolute data address");
#endif
	Check(test, bytes[extent - 1] == 0xa7, "independent data or code changed last sentinel");

	// This API requires Code ownership. Its guest metadata may change, while
	// the host mapping intentionally remains writable for program patching.
	SetProgramMemoryProtection(base, extent, Mode::ExecuteReadWrite);
	ExpectRange(test, Query(test, base), base, base + extent,
	            SceKernelProtCpuRead | SceKernelProtCpuRw | SceKernelProtCpuExec,
	            0, 0, 0, 1, "fixed_program");

	Check(test, FreeGuestMemory(base, extent), "free fixed program failed");
	cleanup.main = 0;
	ExpectUnmapped(test, base);
	Check(test, TestPlaceholderRangeIsFree(base, extent), "free did not restore placeholders");
	Check(test, !TestPlaceholderRangeIsFree(adjacent, page) &&
	                *reinterpret_cast<const uint64_t*>(data_address) == value,
	      "free changed adjacent independent allocation");
	const auto reused = AllocateProgramMemoryAt(base, requested, Mode::ReadWrite, "fixed_reuse");
	if (reused != 0 && (reused < adjacent || reused >= adjacent + page)) { cleanup.main = reused; }
	Check(test, reused == base, "fixed allocation could not reuse freed address");
	Check(test, std::all_of(initial, initial + extent, [](uint8_t v) { return v == 0; }),
	      "reused fixed allocation exposed old bytes");

	const auto invalid = [&](uint64_t address, uint64_t size, const char* name, const char* label) {
		Check(test, AllocateProgramMemoryAt(address, size, Mode::ReadWrite, name) == 0, label);
	};
	invalid(0, page, "invalid", "zero address accepted");
	invalid(probe, 0, "invalid", "zero size accepted");
	invalid(probe, page, nullptr, "null name accepted");
	invalid(probe + 1, page, "invalid", "unaligned address accepted");
	invalid(probe, UINT64_MAX, "invalid", "size rounding overflow accepted");
	invalid(UINT64_MAX - page + 1, page, "invalid", "address plus size overflow accepted");
	invalid(0x10000, page, "invalid", "address outside guest spaces accepted");
	Check(test, Common::GetHostLockCriticalCounter()->load() == entry_depth, "outside-owned failure leaked critical depth");
	Check(test, AllocateProgramMemoryAt(probe, page, static_cast<Mode>(8), "invalid_mode") == 0,
	      "invalid protection mode accepted");
	Check(test, TestPlaceholderRangeIsFree(adjacent + page, page),
	      "invalid protection mode changed free placeholder");
	Check(test, TestPlaceholderRangeIsFree(base - page, page) &&
	                !TestPlaceholderRangeIsFree(base, extent) &&
	                !TestPlaceholderRangeIsFree(adjacent, page) &&
	                TestPlaceholderRangeIsFree(adjacent + page, page),
	      "invalid request changed ownership bounds");
	ExpectRange(test, Query(test, base), base, base + extent,
	            SceKernelProtCpuRead | SceKernelProtCpuRw, 0, 0, 0, 1, "fixed_reuse");
	Check(test, *reinterpret_cast<const uint64_t*>(data_address) == value, "invalid request changed adjacent data");

	Check(test, FreeGuestMemory(base, extent), "free reused program failed"); cleanup.main = 0;
	Check(test, FreeGuestMemory(adjacent, page), "free adjacent data failed"); cleanup.data = 0;
	ExpectUnmapped(test, base); ExpectUnmapped(test, adjacent);
	Check(test, TestPlaceholderRangeIsFree(base, extent + page), "cleanup did not restore placeholders");
	std::printf("[host]    %-48s ok\n", test);
}
