// Included inside the memory fixture namespace; no firmware files required.
void TestProsperoSdkVersion() {
	constexpr const char* test = "ProsperoSdkVersion";
	using Loader::ProcessAbi;
	std::array<uint8_t, 0x60> header {
	    0x60, 0, 0, 0, 0, 0, 0, 0, 0x4f, 0x52, 0x42, 0x49, 5, 0, 0, 0,
	    0x01, 0, 0x59, 0x13, 0x07, 0, 0x60, 0x13,
	};
	header[0x58] = 0x42; // Separate build component must never become the query result.
	Check(test, Loader::ReadProsperoCompiledSdkVersion(header) == 0x13600007, "Prospero field selection");
	Check(test, Loader::ReadCompiledSdkVersion(header) == 0x13590001, "legacy field changed");
	for (size_t n = 0; n < 0x18; ++n) {
		Check(test, Loader::ReadProsperoCompiledSdkVersion(std::span<const uint8_t>(header).first(n)) == 0,
		      "truncated field accepted");
	}
	Check(test, Loader::ReadProsperoCompiledSdkVersion(std::span<const uint8_t>(header).first(0x18)) == 0x13600007,
	      "complete field must not require full declared structure");
	for (uint8_t version: {0, 1, 2, 3, 4, 5}) {
		header[0xc] = version;
		Check(test, Loader::ReadProsperoCompiledSdkVersion(header) == (version >= 4 ? 0x13600007u : 0u),
		      "format gate");
	}
	header[0] = 0x14;
	Check(test, Loader::ReadProsperoCompiledSdkVersion(header) == 0x13600007, "invented declared-size 0x18 minimum");
	header[0] = 0x13;
	Check(test, Loader::ReadProsperoCompiledSdkVersion(header) == 0, "short declared size");
	header[0] = 0x60;
	header[8] = 0;
	Check(test, Loader::ReadProsperoCompiledSdkVersion(header) == 0, "bad magic");
	header[8] = 0x4f;
	for (unsigned abi = 0; abi < 256; ++abi) {
		const auto expected = abi < 2 ? ProcessAbi::Legacy : abi == 2 ? ProcessAbi::Prospero : ProcessAbi::Unknown;
		Check(test, Loader::ReadProcessAbi(static_cast<uint8_t>(abi), false) == expected, "executable ABI classification");
		Check(test, Loader::ReadProcessAbi(static_cast<uint8_t>(abi), true) == ProcessAbi::Unknown, "module acquired process ABI");
	}

	Loader::SymbolDatabase symbols;
	Libs::MediaSrvKernel::InitMediaSrvKernelLibkernel_1(&symbols);
	const auto* exact = symbols.Find({"GGeRJk1XdWc", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
	const auto* fallback = symbols.FindByNid("GGeRJk1XdWc", Loader::SymbolType::Func);
	const auto address = reinterpret_cast<uint64_t>(&Libs::LibKernel::KernelGetProsperoCompiledSdkVersion);
	Check(test, exact && fallback && exact->vaddr == address && fallback->vaddr == address, "typed exact/NID provider");
	using Query = int32_t (KYTY_SYSV_ABI *)(uint32_t*);
	const std::array<Query, 2> queries {reinterpret_cast<Query>(exact->vaddr), reinterpret_cast<Query>(fallback->vaddr)};
	struct Restore {
		Loader::Program* previous = Loader::RuntimeLinker::GetCurrentProgram();
		~Restore() { Loader::RuntimeLinker::SetCurrentProgram(previous); }
	} restore;
	Loader::Program core, ui, legacy, zero, unknown, module;
	core.process_abi = ui.process_abi = zero.process_abi = ProcessAbi::Prospero;
	legacy.process_abi = ProcessAbi::Legacy;
	core.compiled_sdk_version = 0x13590001;
	core.prospero_compiled_sdk_version = Loader::ReadProsperoCompiledSdkVersion(header);
	ui.prospero_compiled_sdk_version = 0x12345678;
	legacy.prospero_compiled_sdk_version = module.prospero_compiled_sdk_version = 0xdeadbeef;
	header[0x14] = 0xff; // Guest/source mutation cannot change an existing snapshot.
	Check(test, unknown.process_abi == ProcessAbi::Unknown && zero.prospero_compiled_sdk_version == 0,
	      "known zero and unavailable metadata defaults");
	const std::array<Loader::Program*, 7> contexts {&core, &ui, &legacy, &zero, &unknown, &module, &core};
	const std::array<uint32_t, 7> values {0x13600007, 0x12345678, 0, 0, 0xa5a5a5a5, 0xa5a5a5a5, 0x13600007};
	const std::array<int32_t, 7> statuses {0, 0, Libs::LibKernel::KERNEL_ERROR_EPERM, 0,
	    Libs::LibKernel::KERNEL_ERROR_ESRCH, Libs::LibKernel::KERNEL_ERROR_ESRCH, 0};
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	for (size_t i = 0; i < contexts.size(); ++i) {
		Loader::RuntimeLinker::SetCurrentProgram(contexts[i]);
		for (auto query: queries) {
			alignas(8) std::array<uint8_t, 12> bytes;
			bytes.fill(0xa5);
			Check(test, query(reinterpret_cast<uint32_t*>(bytes.data() + 3)) == statuses[i], "query status");
			uint32_t result = 0;
			std::memcpy(&result, bytes.data() + 3, sizeof(result));
			Check(test, result == values[i], "query selected wrong snapshot");
			for (size_t b = 0; b < bytes.size(); ++b) {
				Check(test, (b >= 3 && b < 7) || bytes[b] == 0xa5, "query overwrote adjacent bytes");
			}
			Check(test, query(nullptr) == Libs::LibKernel::KERNEL_ERROR_EINVAL, "null precedence");
		}
	}
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	if (Loader::RuntimeLinker::GetCurrentProgram() == nullptr) {
		for (auto query: queries) {
			uint32_t result = 0xa5a5a5a5;
			Check(test, query(&result) == Libs::LibKernel::KERNEL_ERROR_ESRCH && result == 0xa5a5a5a5, "missing process wrote output");
			Check(test, query(nullptr) == Libs::LibKernel::KERNEL_ERROR_EINVAL, "missing process/null precedence");
		}
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SYSTEM_INFO info {};
	GetSystemInfo(&info);
	struct Pages {
		uint8_t* p;
		~Pages() { if (p) { VirtualFree(p, 0, MEM_RELEASE); } }
	} pages {static_cast<uint8_t*>(VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))};
	Check(test, pages.p != nullptr, "allocate guard-page fixture");
	DWORD old = 0;
	Check(test, VirtualProtect(pages.p + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old) != 0, "protect following page");
	for (size_t i = 0; i < contexts.size(); ++i) {
		Loader::RuntimeLinker::SetCurrentProgram(contexts[i]);
		for (auto query: queries) {
			auto* output = reinterpret_cast<uint32_t*>(pages.p + info.dwPageSize - 4);
			*output = 0xa5a5a5a5;
			Check(test, query(output) == statuses[i] && *output == values[i], "page-end four-byte result");
		}
	}
#endif
	Check(test, Common::GetHostLockCriticalCounter()->load() == depth, "SDK query leaked host-critical state");
}
