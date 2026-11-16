// Included inside VirtualMemoryAllocationTests.cpp's anonymous namespace.
void TestStaticExecutableImageMapping() {
	using namespace Libs::LibKernel::Memory;
	using ImageError = Loader::StaticExecutableImageError;
	using LayoutError = Loader::StaticExecutableLayoutError;
	using Mode = Common::VirtualMemory::Mode;
	const char* test = "StaticExecutableImageMapping";
	constexpr uint64_t base = 0x400000;
	constexpr uint64_t other = 0x600000;
	constexpr uint64_t page = SceKernelPageSize;
	constexpr uint64_t data_delta = 0x10000;
	constexpr uint64_t marker = 0x9f81a7b6c5d4e321ull;
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	const auto check_depth = [&] { Check(test, Common::GetHostLockCriticalCounter()->load() == depth,
	                                     "critical depth not restored"); };
	const auto make_fixture = [](uint64_t address, uint32_t text_flags = 5) {
		std::vector<uint8_t> b(0x8100, 0);
		const auto put = [&](size_t at, uint64_t value, size_t width) {
			for (size_t i = 0; i < width; ++i) { b[at + i] = static_cast<uint8_t>(value >> (i * 8)); }
		};
		const auto ph = [&](size_t index, uint32_t type, uint32_t flags, uint64_t offset,
		                    uint64_t va, uint64_t filesz, uint64_t memsz, uint64_t align) {
			const size_t p = 64 + index * 56;
			put(p, type, 4); put(p + 4, flags, 4); put(p + 8, offset, 8);
			put(p + 16, va, 8); put(p + 32, filesz, 8); put(p + 40, memsz, 8); put(p + 48, align, 8);
		};
		b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
		b[4] = 2; b[5] = 1; b[6] = 1; b[7] = 9; b[8] = 2;
		put(16, 2, 2); put(18, 62, 2); put(20, 1, 4); put(24, address + 0x80, 8);
		put(32, 64, 8); put(52, 64, 2); put(54, 56, 2); put(56, 3, 2);
		ph(0, 1, text_flags, 0x4000, address, 0x100, 0x100, 0x4000);
		ph(1, 1, 6, 0x8000, address + data_delta, 0x30, 0x200, 0x4000);
		ph(2, 7, 4, 0x8010, address + data_delta + 0x10, 8, 0x20, 0x10);
		for (size_t i = 0; i < 0x100; ++i) { b[0x4000 + i] = static_cast<uint8_t>(i ^ 0xa5); }
		for (size_t i = 0; i < 0x30; ++i) { b[0x8000 + i] = static_cast<uint8_t>(i + 0x31); }
		// movabs rax,[absolute data address]; ret. Never execute input from a file.
		b[0x4080] = 0x48; b[0x4081] = 0xa1;
		put(0x4082, address + data_delta, 8); b[0x408a] = 0xc3;
		put(0x8000, marker, 8);
		return b;
	};
	struct PrivateAllocation {
		uint64_t address = 0;
		uint64_t size = 0;
		~PrivateAllocation() { if (address != 0) { FreeGuestMemory(address, size); } }
	};
	const auto empty_failure = [&](const Loader::StaticExecutableImageResult& r, ImageError error, const char* label) {
		Check(test, r.error == error && !r.image.IsMapped() && r.image.Layout().segments.empty(), label);
		check_depth();
	};
	Check(test, TestGuestAddressRangeIsOwned(base, data_delta + page) &&
	                TestPlaceholderRangeIsFree(base, data_delta + page), "fixture precondition: first range unavailable");
	Check(test, TestGuestAddressRangeIsOwned(other, data_delta + page) &&
	                TestPlaceholderRangeIsFree(other, data_delta + page), "fixture precondition: second range unavailable");
	auto bytes = make_fixture(base);
	const auto original = bytes;

	// Failure after text was allocated must roll text back, not touch the later owner.
	{
		PrivateAllocation collision;
		collision.address = AllocateProgramMemoryAt(base + data_delta, page, Mode::ReadWrite, "image_collision");
		collision.size = page;
		Check(test, collision.address == base + data_delta, "collision fixture allocation failed");
		*reinterpret_cast<uint64_t*>(collision.address) = marker;
		auto r = Loader::MapStaticExecutableImage(bytes);
		empty_failure(r, ImageError::AllocationFailed, "later segment collision did not roll back");
		Check(test, TestPlaceholderRangeIsFree(base, page) &&
		                *reinterpret_cast<const uint64_t*>(collision.address) == marker,
		      "collision rollback changed existing mapping or retained text");
		ExpectRange(test, Query(test, collision.address), collision.address, collision.address + page,
		            SceKernelProtCpuRead | SceKernelProtCpuRw, 0, 0, 0, 1, "image_collision");
	}
	Check(test, TestPlaceholderRangeIsFree(base, data_delta + page), "collision cleanup failed");
	{
		// A separate valid mapping guarantees a subsequent protection call can
		// consume the hook even if the mapper unexpectedly fails before reaching it.
		PrivateAllocation hook_sink;
		hook_sink.address = AllocateProgramMemoryAt(other, page, Mode::ReadWrite, "image_hook_sink");
		hook_sink.size = page;
		Check(test, hook_sink.address == other, "protection hook sink allocation failed");
		TestFailNextGuestMemoryProtection();
		auto failed_protection = Loader::MapStaticExecutableImage(bytes);
		const bool sink_protected = ProtectGuestMemory(hook_sink.address, page, Mode::ReadWrite);
		empty_failure(failed_protection, ImageError::ProtectionFailed, "injected protection failure");
		Check(test, sink_protected, "mapper did not consume injected protection failure");
		Check(test, TestPlaceholderRangeIsFree(base, data_delta + page), "protection rollback retained pages");
	}

	{
		auto malformed = bytes; malformed[0] = 0;
		auto r = Loader::MapStaticExecutableImage(malformed);
		empty_failure(r, ImageError::InvalidLayout, "malformed ELF accepted");
		Check(test, r.layout_error == LayoutError::InvalidHeader, "malformed ELF layout error lost");
		malformed = bytes; malformed[64 + 4] |= 0x80;
		auto unsupported = Loader::MapStaticExecutableImage(malformed);
		empty_failure(unsupported, ImageError::UnsupportedFlags, "unknown LOAD flag accepted");
		Check(test, TestPlaceholderRangeIsFree(base, data_delta + page), "invalid inputs allocated pages");
	}
	{
		auto zero = make_fixture(0);
		auto r = Loader::MapStaticExecutableImage(zero);
		empty_failure(r, ImageError::InvalidMappingRange, "zero page mapping accepted");
		Check(test, TestPlaceholderRangeIsFree(base, data_delta + page), "zero mapping changed fixture range");
	}
	{
		PrivateAllocation source;
		source.size = page * 3;
		source.address = AllocateProgramMemoryAt(base, source.size, Mode::ReadWrite, "image_source");
		Check(test, source.address == base, "overlapping source fixture allocation failed");
		std::memcpy(reinterpret_cast<void*>(source.address), bytes.data(), bytes.size());
		const auto input = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(source.address), bytes.size());
		auto r = Loader::MapStaticExecutableImage(input);
		empty_failure(r, ImageError::InputOverlapsMapping, "overlapping source accepted");
		Check(test, std::equal(input.begin(), input.end(), bytes.begin()), "overlapping source modified");
		ExpectRange(test, Query(test, base), base, base + source.size,
		            SceKernelProtCpuRead | SceKernelProtCpuRw, 0, 0, 0, 1, "image_source");
		Check(test, TestPlaceholderRangeIsFree(base + data_delta, page), "source overlap allocated data page");
	}

	{
		auto r = Loader::MapStaticExecutableImage(bytes);
		Check(test, r.error == ImageError::None && r.layout_error == LayoutError::None && r.image.IsMapped(), "valid image mapping failed");
		check_depth();
		const auto& layout = r.image.Layout();
		Check(test, layout.load_bias == 0 && layout.entry == base + 0x80 && layout.owner_address == base &&
		                layout.owner_size == data_delta + page && layout.segments.size() == 2,
		      "image layout values incorrect");
		Check(test, layout.tls && layout.tls->address == base + data_delta + 0x10 &&
		                layout.tls->file_offset == 0x8010 && layout.tls->file_size == 8 &&
		                layout.tls->memory_size == 0x20 && layout.tls->alignment == 0x10, "TLS record incorrect");
		ExpectRange(test, Query(test, base), base, base + page, SceKernelProtCpuRead | SceKernelProtCpuExec,
		            0, 0, 0, 1, "static executable segment");
		ExpectRange(test, Query(test, base + data_delta), base + data_delta, base + data_delta + page,
		            SceKernelProtCpuRead | SceKernelProtCpuRw, 0, 0, 0, 1, "static executable segment");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MEMORY_BASIC_INFORMATION text_info {}, data_info {};
		Check(test, VirtualQuery(reinterpret_cast<void*>(base), &text_info, sizeof(text_info)) != 0 &&
		                text_info.State == MEM_COMMIT && text_info.Protect == PAGE_EXECUTE_READ,
		      "host text protection is not execute-read");
		Check(test, VirtualQuery(reinterpret_cast<void*>(base + data_delta), &data_info, sizeof(data_info)) != 0 &&
		                data_info.State == MEM_COMMIT && data_info.Protect == PAGE_READWRITE,
		      "host data protection is not read-write");
#endif
		const auto* text = reinterpret_cast<const uint8_t*>(base);
		const auto* data = reinterpret_cast<const uint8_t*>(base + data_delta);
		Check(test, std::equal(text, text + 0x100, bytes.begin() + 0x4000), "text bytes differ");
		Check(test, std::equal(data, data + 0x30, bytes.begin() + 0x8000), "data bytes differ");
		Check(test, std::all_of(text + 0x100, text + page, [](uint8_t x) { return x == 0; }), "text page padding not zero");
		Check(test, std::all_of(data + 0x30, data + 0x200, [](uint8_t x) { return x == 0; }) &&
		                std::all_of(data + 0x200, data + page, [](uint8_t x) { return x == 0; }),
		      "data BSS or page padding not zero");
		Check(test, bytes == original, "mapper changed input bytes");
		Check(test, TestPlaceholderRangeIsFree(base + page, data_delta - page), "image consumed gap");
#if defined(__x86_64__) || defined(_M_X64)
		using Function = uint64_t(KYTY_SYSV_ABI*)();
		Check(test, reinterpret_cast<Function>(base + 0x80)() == marker, "synthetic entry did not use absolute data address");
#endif
		// The enclosing owner span is metadata: a separately allocated gap remains ours.
		PrivateAllocation gap;
		gap.address = AllocateProgramMemoryAt(base + page, page, Mode::ReadWrite, "image_gap"); gap.size = page;
		Check(test, gap.address == base + page, "independent gap allocation failed");
		*reinterpret_cast<uint64_t*>(gap.address) = marker;
		*reinterpret_cast<uint8_t*>(base + data_delta + 0x100) = 0xf1;
		Loader::StaticExecutableImage moved(std::move(r.image));
		Check(test, moved.IsMapped() && !r.image.IsMapped() && r.image.Layout().segments.empty(), "move construction ownership incorrect");
		auto other_bytes = make_fixture(other);
		auto second = Loader::MapStaticExecutableImage(other_bytes);
		Check(test, second.error == ImageError::None && second.image.IsMapped(), "second image mapping failed");
		second.image = std::move(moved);
		Check(test, second.image.IsMapped() && second.image.Layout().entry == base + 0x80 &&
		                !moved.IsMapped() && moved.Layout().segments.empty(), "move assignment ownership incorrect");
		Check(test, TestPlaceholderRangeIsFree(other, data_delta + page) &&
		                !TestPlaceholderRangeIsFree(base, page), "move assignment did not release only old image");
		second.image.Reset(); check_depth();
		second.image.Reset(); check_depth();
		Check(test, !second.image.IsMapped() && second.image.Layout().segments.empty() &&
		                TestPlaceholderRangeIsFree(base, page) && TestPlaceholderRangeIsFree(base + data_delta, page) &&
		                *reinterpret_cast<const uint64_t*>(gap.address) == marker, "reset released gap or retained image");
		// Fresh mapping must zero bytes dirtied in the prior BSS, despite reuse.
		auto reused = Loader::MapStaticExecutableImage(bytes);
		Check(test, reused.error == ImageError::None && reused.image.IsMapped(), "image reuse failed");
		Check(test, *reinterpret_cast<const uint8_t*>(base + data_delta + 0x100) == 0, "image reuse did not zero BSS");
		// reused's destructor releases its segments while gap's destructor releases only the gap.
	}
	check_depth();
	Check(test, TestPlaceholderRangeIsFree(base, data_delta + page), "destructor did not release mappings");
	{
		auto execute_only_bytes = make_fixture(base, 1);
		auto r = Loader::MapStaticExecutableImage(execute_only_bytes);
		Check(test, r.error == ImageError::None && r.image.IsMapped(), "execute-only image mapping failed");
		ExpectRange(test, Query(test, base), base, base + page, SceKernelProtCpuExec, 0, 0, 0, 1, "static executable segment");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		MEMORY_BASIC_INFORMATION info {};
		Check(test, VirtualQuery(reinterpret_cast<void*>(base), &info, sizeof(info)) != 0 &&
		                info.State == MEM_COMMIT && info.Protect == PAGE_EXECUTE,
		      "host execute-only protection incorrect");
#endif
	}
	check_depth();
	Check(test, TestPlaceholderRangeIsFree(base, data_delta + page), "execute-only destructor retained mappings");
	std::printf("[host]    %-48s ok\n", test);
}
