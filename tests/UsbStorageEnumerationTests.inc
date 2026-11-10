// Include inside VirtualMemoryAllocationTests.cpp's fixture namespace and run
// TestUsbStorageEnumeration() in a dedicated process after InitSubsystems().
// Required top-level declaration:
// namespace Libs::JscdSmall::UsbStorageForShellUI {
// void InitJscdSmallUsbStorageForShellUI_1(Loader::SymbolDatabase*);
// }
// Link libJscdSmall.cpp and its usual dependencies, including npMemoryArena.cpp,
// npMemoryHeap.cpp and kernel/fileSystem.cpp. Existing fixture headers suffice.
void TestUsbStorageEnumeration() {
	constexpr const char* test = "UsbStorageEnumeration";
	constexpr int32_t invalid = static_cast<int32_t>(0x80f40002u);
	constexpr int32_t status = static_cast<int32_t>(0x80f40009u);
	Loader::SymbolDatabase symbols;
	Libs::JscdSmall::UsbStorageForShellUI::InitJscdSmallUsbStorageForShellUI_1(&symbols);
	auto resolve = [&](const char* nid) {
		const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(test, symbol != nullptr && symbol->vaddr != 0, "missing actual guest export");
		return symbol->vaddr;
	};
	const auto init = reinterpret_cast<int32_t(KYTY_SYSV_ABI *)(void*)>(resolve("BDDZwF5kuTc"));
	const auto term = reinterpret_cast<int32_t(KYTY_SYSV_ABI *)()>(resolve("Wp8zHTocS5E"));
	const auto enumerate = reinterpret_cast<int32_t(KYTY_SYSV_ABI *)(uint32_t*, int32_t*)>(resolve("mryrNITeYvI"));
	struct GuardedIds { uint32_t before; uint32_t ids[8]; uint32_t after; } ids;
	std::memset(&ids, 0xa5, sizeof(ids));
	const auto original_ids = ids;
	struct GuardedCount { uint32_t before; int32_t count; uint32_t after; } count {0x12345678, -1, 0x87654321};
	Check(test, term() == status, "uninitialized termination is STATUS");
	Check(test, enumerate(nullptr, nullptr) == status && enumerate(ids.ids, &count.count) == status && count.count == -1,
	      "readiness precedes pointer checks and preserves outputs");
	Check(test, init(nullptr) == 0 && init(nullptr) == 0, "optional attribute and idempotent initialization");
	Check(test, enumerate(ids.ids, &count.count) == 0 && count.count == 0,
	      "Gallery's -1 sentinel must become an empty count on success");
	Check(test, count.before == 0x12345678 && count.after == 0x87654321 &&
	            std::memcmp(&ids, &original_ids, sizeof(ids)) == 0, "exact four-byte count and untouched unused IDs");
	count.count = 12345;
	Check(test, enumerate(ids.ids, &count.count) == 0 && count.count == 0, "count is output-only, not capacity");
	count.count = -1;
	Check(test, enumerate(nullptr, &count.count) == invalid && count.count == -1 &&
	            enumerate(ids.ids, nullptr) == invalid, "both output pointers are required");
	Check(test, enumerate(reinterpret_cast<uint32_t*>(1), &count.count) == invalid && count.count == -1 &&
	            enumerate(ids.ids, reinterpret_cast<int32_t*>(1)) == invalid,
	      "invalid addresses are fault-contained without changing valid outputs");
	Check(test, enumerate(reinterpret_cast<uint32_t*>(UINTPTR_MAX - 15), &count.count) == invalid && count.count == -1 &&
	            enumerate(ids.ids, reinterpret_cast<int32_t*>(UINTPTR_MAX - 1)) == invalid,
	      "overflowing output ranges are rejected");

	using Mode = Common::VirtualMemory::Mode;
	const auto memory = Libs::LibKernel::Memory::AllocateRuntimeMemory(0, 0x8000, Mode::ReadWrite, "usb-enum-probe");
	Check(test, memory != 0, "allocate guarded enumeration outputs");
	std::memset(reinterpret_cast<void*>(memory), 0xa5, 0x8000);
	Check(test, Common::VirtualMemory::Protect(memory + 0x4000, 0x4000, Mode::NoAccess), "protect enumeration tail");
	auto* edge_ids = reinterpret_cast<uint32_t*>(memory + 0x4000 - 32);
	Check(test, enumerate(edge_ids, &count.count) == 0 && count.count == 0,
	      "fixed eight-ID buffer ending at guard page is sufficient");
	count.count = -1;
	Check(test, enumerate(edge_ids + 1, &count.count) == invalid && count.count == -1,
	      "short ID buffer rejected before writing count");
	auto* edge_count = reinterpret_cast<int32_t*>(memory + 0x4000 - 4);
	Check(test, enumerate(ids.ids, edge_count) == 0 && *edge_count == 0,
	      "count writes exactly four bytes at page boundary");
	std::memset(reinterpret_cast<void*>(memory), 0xa5, 0x4000);
	Check(test, enumerate(ids.ids, reinterpret_cast<int32_t*>(memory + 0x3ffe)) == invalid &&
	            *reinterpret_cast<uint16_t*>(memory + 0x3ffe) == 0xa5a5,
	      "split count output rejected without partial prefix write");
	Check(test, Common::VirtualMemory::Protect(memory, 0x4000, Mode::Read), "protect enumeration outputs read-only");
	Check(test, enumerate(reinterpret_cast<uint32_t*>(memory), &count.count) == invalid && count.count == -1 &&
	            enumerate(ids.ids, reinterpret_cast<int32_t*>(memory)) == invalid &&
	            *reinterpret_cast<const uint32_t*>(memory) == 0xa5a5a5a5,
	      "read-only outputs rejected without modification");
	Check(test, Libs::LibKernel::Memory::FreeGuestMemory(memory, 0x8000), "release guarded enumeration outputs");
	Check(test, std::memcmp(&ids, &original_ids, sizeof(ids)) == 0 &&
	            count.before == 0x12345678 && count.after == 0x87654321, "failure paths preserve all output guards");
	Check(test, term() == 0 && enumerate(ids.ids, &count.count) == status && count.count == -1 && term() == status,
	      "one termination retires even repeatedly initialized service");
	Check(test, init(nullptr) == 0 && enumerate(ids.ids, &count.count) == 0 && count.count == 0 && term() == 0,
	      "reinitialization restores empty guest-device enumeration");
}
