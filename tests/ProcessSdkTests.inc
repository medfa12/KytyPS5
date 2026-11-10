// Included by VirtualMemoryAllocationTests.cpp inside its anonymous namespace.
// The parent includes libs/processSdk.h and loader/processMetadata.h, and owns
// dispatch/build integration. TestProcessSdkVersion also runs parser fixtures.
void TestProcessSdkMetadata() {
	constexpr const char* test = "ProcessSdkMetadata";
	// Literal ORBI process header: size 0x60, version 5, legacy SDK 0x13590001,
	// and a deliberately different Prospero SDK 0x13600007. Remaining bytes zero.
	constexpr std::array<uint8_t, 0x60> valid {
	    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x4f, 0x52, 0x42, 0x49, 0x05, 0x00, 0x00, 0x00,
	    0x01, 0x00, 0x59, 0x13, 0x07, 0x00, 0x60, 0x13,
	};
	Check(test, Loader::ReadCompiledSdkVersion(valid) == 0x13590001,
	      "version-five header must select legacy SDK, not adjacent Prospero SDK");
	const std::span<const uint8_t> bytes(valid);
	for (size_t length: {size_t {0}, size_t {7}, size_t {8}, size_t {11}, size_t {15},
	                     size_t {16}, size_t {19}}) {
		Check(test, Loader::ReadCompiledSdkVersion(bytes.first(length)) == 0,
		      "short/truncated process header must leave SDK zero");
	}
	Check(test, Loader::ReadCompiledSdkVersion(bytes.first(0x14)) == 0x13590001,
	      "complete legacy field must not require the adjacent Prospero field or full declared size");
	auto bad_magic = valid;
	bad_magic[8] = 0;
	Check(test, Loader::ReadCompiledSdkVersion(bad_magic) == 0, "bad ORBI magic accepted");
	auto small_size = valid;
	small_size[0] = 0x13;
	Check(test, Loader::ReadCompiledSdkVersion(small_size) == 0, "declared size below 0x14 accepted");
	small_size[0] = 0;
	Check(test, Loader::ReadCompiledSdkVersion(small_size) == 0, "zero declared size accepted");
	auto minimum_size = valid;
	minimum_size[0] = 0x14;
	Check(test, Loader::ReadCompiledSdkVersion(minimum_size) == 0x13590001, "minimum declared size rejected");
	auto version_zero = valid;
	version_zero[0x0c] = 0;
	Check(test, Loader::ReadCompiledSdkVersion(version_zero) == 0x13590001,
	      "version zero must still use the legacy SDK field");
	auto sdk_zero = valid;
	sdk_zero[0x10] = 0;
	sdk_zero[0x12] = 0;
	sdk_zero[0x13] = 0;
	Check(test, Loader::ReadCompiledSdkVersion(sdk_zero) == 0,
	      "SDK zero must not fall back to the nonzero Prospero field");
}

void TestProcessSdkVersion() {
	TestProcessSdkMetadata();
	constexpr const char* test = "ProcessSdkVersion";
	using QuerySdk = int32_t (KYTY_SYSV_ABI *)(uint32_t*);
	const auto expected_address = reinterpret_cast<uint64_t>(&Libs::LibKernel::KernelGetCompiledSdkVersion);
	auto resolve = [&](const Loader::SymbolDatabase& symbols) {
		const auto* exact = symbols.Find({"WB66evu8bsU", "libkernel", 1, "libkernel", 1, 1,
		                                  Loader::SymbolType::Func});
		const auto* fallback = symbols.FindByNid("WB66evu8bsU", Loader::SymbolType::Func);
		Check(test, exact && fallback && exact->vaddr == expected_address && fallback->vaddr == expected_address,
		      "WB66 exact/NID bindings must use the output-pointer provider");
		return std::array<QuerySdk, 2> {reinterpret_cast<QuerySdk>(exact->vaddr),
		                               reinterpret_cast<QuerySdk>(fallback->vaddr)};
	};
	Loader::SymbolDatabase early;
	Libs::InitLibKernel_1(&early);
	const auto early_queries = resolve(early);
	// Register both WB66 providers in production order. InitAll also starts
	// unrelated subsystems (including the filesystem sandbox); this focused
	// fixture intentionally owns only the SDK providers and process context.
	Libs::ShellCoreFloor::InitShellCoreFloor_1(&early);
	const auto repeated_queries = resolve(early);
	Loader::SymbolDatabase production;
	Libs::InitLibKernel_1(&production);
	Libs::ShellCoreFloor::InitShellCoreFloor_1(&production);
	const auto production_queries = resolve(production);
	const std::array<QuerySdk, 6> queries {early_queries[0], early_queries[1], repeated_queries[0],
	                                      repeated_queries[1], production_queries[0], production_queries[1]};

	Loader::Program shell_core, co_elf, zero_sdk;
	shell_core.unique_id = 720001;
	shell_core.file_name = "SceShellCore.elf";
	shell_core.compiled_sdk_version = 0x13590001;
	co_elf.unique_id = 720002;
	co_elf.file_name = std::filesystem::path("NPXS40087") / "eboot.bin";
	co_elf.compiled_sdk_version = 0x12000042;
	zero_sdk.unique_id = 720003;
	zero_sdk.file_name = "zero-sdk.elf";
	Check(test, zero_sdk.compiled_sdk_version == 0, "new process snapshot must default to SDK zero");
	struct RestoreContext {
		Loader::Program* previous = Loader::RuntimeLinker::GetCurrentProgram();
		~RestoreContext() { Loader::RuntimeLinker::SetCurrentProgram(previous); }
	} restore;
	struct Guarded {
		uint32_t before = 0xaabbccdd;
		uint32_t output = 0xdeadbeef;
		uint32_t after = 0x11223344;
	};
	static_assert(sizeof(Guarded) == 12 && offsetof(Guarded, output) == 4 && offsetof(Guarded, after) == 8);
	const auto initial_depth = Common::GetHostLockCriticalCounter()->load();

	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	if (Loader::RuntimeLinker::GetCurrentProgram() == nullptr) {
		for (auto query: queries) {
			Guarded words;
			const auto saved = words;
			Check(test, query(&words.output) == Libs::LibKernel::KERNEL_ERROR_ESRCH &&
			            std::memcmp(&words, &saved, sizeof(words)) == 0,
			      "missing process must return ESRCH without writing output");
			Check(test, query(nullptr) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
			      "null output must take precedence over missing process");
		}
	} else {
		std::printf("ProcessSdkVersion: missing-process case skipped (main-program fallback present)\n");
	}
	const std::array<Loader::Program*, 5> contexts {&shell_core, &co_elf, &zero_sdk, &co_elf, &shell_core};
	const std::array<uint32_t, 5> expected {0x13590001, 0x12000042, 0, 0x12000042, 0x13590001};
	for (size_t i = 0; i < contexts.size(); ++i) {
		Loader::RuntimeLinker::SetCurrentProgram(contexts[i]);
		for (auto query: queries) {
			Guarded words;
			CheckOk(test, query(&words.output), "query current process SDK");
			Check(test, words.output == expected[i] && words.before == 0xaabbccdd && words.after == 0x11223344,
			      "SDK must follow process snapshot and write exactly four bytes");
			Check(test, query(nullptr) == Libs::LibKernel::KERNEL_ERROR_EINVAL, "null output error");
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	const size_t page_size = info.dwPageSize;
	struct Pages {
		uint8_t* address;
		~Pages() { if (address != nullptr) { VirtualFree(address, 0, MEM_RELEASE); } }
	} pages {static_cast<uint8_t*>(VirtualAlloc(nullptr, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))};
	Check(test, pages.address != nullptr, "allocate SDK page-boundary fixture");
	DWORD old = 0;
	Check(test, VirtualProtect(pages.address + page_size, page_size, PAGE_NOACCESS, &old) != 0,
	      "protect page following SDK output");
	for (size_t i = 0; i < contexts.size(); ++i) {
		Loader::RuntimeLinker::SetCurrentProgram(contexts[i]);
		for (auto query: queries) {
			std::memset(pages.address + page_size - 8, 0xa5, 8);
			auto* output = reinterpret_cast<uint32_t*>(pages.address + page_size - 4);
			CheckOk(test, query(output), "four-byte page-end SDK output");
			uint32_t before = 0;
			std::memcpy(&before, pages.address + page_size - 8, sizeof(before));
			Check(test, *output == expected[i] && before == 0xa5a5a5a5,
			      "page-end SDK mismatch or preceding canary overwritten");
		}
	}
#endif
	Check(test, Common::GetHostLockCriticalCounter()->load() == initial_depth,
	      "SDK query leaked suspension protection");
}
