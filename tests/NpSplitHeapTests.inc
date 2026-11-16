// Included inside the memory-test fixture namespace. Parent includes
// libs/npMemoryHeap.h, loader/runtimeLinker.h, kernel/memory.h and common/threads.h.
// Direct calls exercise the four export implementations; lead owns NID binding.
namespace NpSplitHeapTest {
namespace Np = Libs::NpMemoryHeap;
constexpr const char* Name = "NpSplitHeap";

// The SNS caller's own layout, not the combined NpMemoryHeap::Heap layout.
struct SnsMemory {
	uint32_t id = 0, mode = 0, initialized = 0, padding = 0;
	Np::MappingRecord mapping {};
	Np::Handle handle = nullptr;
	uint32_t count = 0, padding2 = 0;
	Np::Allocator ordinary {};
	Np::AllocatorEx extended {};
};
static_assert(sizeof(SnsMemory) == 0x80 && offsetof(SnsMemory, mapping) == 0x10 &&
              offsetof(SnsMemory, handle) == 0x28 && offsetof(SnsMemory, ordinary) == 0x38);
uint64_t requested_raw = 0;
void* returned_raw = nullptr;
void* freed_raw = nullptr;
void* freed_context = nullptr;
uint32_t callback_depth = 0;

void* KYTY_SYSV_ABI Allocate(size_t size, void* context) {
	requested_raw = size;
	callback_depth = Common::GetHostLockCriticalCounter()->load();
	auto* state = static_cast<SnsMemory*>(context);
	returned_raw = Np::Malloc(&state->handle, size);
	return returned_raw;
}
void* KYTY_SYSV_ABI Reallocate(void* pointer, size_t size, void* context) {
	return Np::Realloc(&static_cast<SnsMemory*>(context)->handle, pointer, size);
}
void KYTY_SYSV_ABI Free(void* raw, void* context) {
	freed_raw = raw;
	freed_context = context;
	callback_depth = Common::GetHostLockCriticalCounter()->load();
	Np::Free(&static_cast<SnsMemory*>(context)->handle, raw);
}

// Data-contract fixtures for native N+185a0 scalar new / paired header delete.
// These exercise the actual callback backend, not a loaded native provider.
void* HeaderNew(size_t object_size, Np::Allocator* descriptor) {
	auto* raw = static_cast<uint8_t*>(descriptor->allocate(object_size + 0x10, descriptor->context));
	if (raw == nullptr) { return nullptr; }
	std::memcpy(raw, &descriptor, sizeof(descriptor));
	return raw + 0x10;
}
void HeaderDelete(void* object) {
	auto* raw = static_cast<uint8_t*>(object) - 0x10;
	Np::Allocator* descriptor = nullptr;
	std::memcpy(&descriptor, raw, sizeof(descriptor));
	descriptor->free(raw, descriptor->context);
}

struct GuardedRecord {
	uint64_t before = UINT64_C(0x123456789abcdef0);
	Np::MappingRecord record {};
	uint64_t after = UINT64_C(0xfedcba9876543210);
};
void Canaries(const GuardedRecord& value) {
	Check(Name, value.before == UINT64_C(0x123456789abcdef0) && value.after == UINT64_C(0xfedcba9876543210),
	      "mapping helper wrote beyond its 24-byte record");
}
} // namespace NpSplitHeapTest

void TestNpSplitHeap() {
	using namespace NpSplitHeapTest;
	namespace Memory = Libs::LibKernel::Memory;
	NpHeapTest::Api api;
	const auto map_memory = api.Resolve("gMlY6eewr-c", &Np::MapMemory);
	const auto create_heap = api.Resolve("B+yGIX1+BTI", &Np::CreateHeap);
	const auto destroy_heap = api.Resolve("wVdn78HKc30", &Np::DestroyHeap);
	const auto unmap_memory = api.Resolve("vjwlDmsGtME", &Np::UnmapMemory);
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	Loader::Program process;
	process.compiled_sdk_version = 0x13590001;
	struct RestoreContext {
		Loader::Program* previous = Loader::RuntimeLinker::GetCurrentProgram();
		~RestoreContext() { Loader::RuntimeLinker::SetCurrentProgram(previous); }
	} restore;
	Loader::RuntimeLinker::SetCurrentProgram(&process);
	void* optional_base = reinterpret_cast<void*>(uintptr_t {0x1234});
	Check(Name, Np::MapMemory(0x10000, "split", &optional_base, nullptr) == Np::InvalidHeap &&
	            optional_base == reinterpret_cast<void*>(uintptr_t {0x1234}) && Np::UnmapMemory(nullptr) == Np::InvalidHeap,
	      "null mapping record contract");
	SnsMemory sns;
	sns.ordinary = {Allocate, Reallocate, Free, &sns};
	CheckOk(Name, map_memory(0x10000, "SceNpSnsVsh", &optional_base, &sns.mapping), "SNS 64 KiB map");
	Check(Name, sns.mapping.size == 0x10000 && sns.mapping.reserved == 0 && sns.mapping.mapping == optional_base &&
	            sns.handle == nullptr, "map must publish backing without creating an arena");
	CheckOk(Name, create_heap(&sns.handle, optional_base, sns.mapping.size, "SceNpSnsVsh"), "SNS nonowning arena create");
	Check(Name, sns.handle != nullptr, "create returned success without a live handle");
	Memory::VirtualQueryInfo info {};
	CheckOk(Name, Memory::KernelVirtualQuery(optional_base, 0, &info, sizeof(info)), "query mapped SNS backing");
	Check(Name, info.protection == 3 && info.is_flexible && info.end - info.start == 0x10000, "map protection/extent");
	std::memset(optional_base, 0xa5, 0x10000);
	void* object = HeaderNew(0x18, &sns.ordinary);
	Check(Name, object != nullptr && requested_raw == 0x28 && returned_raw == optional_base &&
	            object == static_cast<uint8_t*>(returned_raw) + 0x10 && callback_depth == depth,
	      "SNS object 0x18 must request 0x28 (40 decimal) raw bytes, with one native header");
	Np::Allocator* saved_descriptor = nullptr;
	uint64_t padding = 0;
	std::memcpy(&saved_descriptor, returned_raw, sizeof(saved_descriptor));
	std::memcpy(&padding, static_cast<uint8_t*>(returned_raw) + 8, sizeof(padding));
	Check(Name, saved_descriptor == &sns.ordinary && padding == UINT64_C(0xa5a5a5a5a5a5a5a5), "native header/padding contract");
	std::memset(object, 0x6b, 0x18);
	HeaderDelete(object);
	Check(Name, freed_raw == returned_raw && freed_context == &sns && callback_depth == depth,
	      "delete must return raw header base with original SNS context outside host locks");
	void* reused = Np::Malloc(&sns.handle, 0x28);
	Check(Name, reused == returned_raw, "paired delete did not release the raw allocation");
	Np::Free(&sns.handle, reused);

	const auto old_handle = sns.handle;
	for (uint64_t size: {uint64_t {0}, uint64_t {0x5a0}, uint64_t {0x5a1}, uint64_t {0x1001}, UINT64_MAX}) {
		Check(Name, Np::CreateHeap(&sns.handle, optional_base, size, "invalid-size") == Libs::NpManagerMemory::HeapFailure &&
		            sns.handle == old_handle, "failed create changed slot or ignored audited size validation");
	}
	Check(Name, Np::CreateHeap(&sns.handle, nullptr, 0x10000, "null-backing") == Libs::NpManagerMemory::HeapFailure &&
	            Np::CreateHeap(&sns.handle, static_cast<uint8_t*>(optional_base) + 1, 0x1000, "unaligned") ==
	                Libs::NpManagerMemory::HeapFailure && sns.handle == old_handle,
	      "null/misaligned backing changed the old handle");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	{
		SYSTEM_INFO system {};
		GetSystemInfo(&system);
		const size_t page = system.dwPageSize;
		struct Pages {
			void* base;
			~Pages() { if (base != nullptr) { VirtualFree(base, 0, MEM_RELEASE); } }
		} pages {VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY)};
		Check(Name, pages.base != nullptr, "allocate read-only backing fixture");
		Check(Name, Np::CreateHeap(&sns.handle, pages.base, page * 2, "readonly") == Libs::NpManagerMemory::HeapFailure &&
		            sns.handle == old_handle, "read-only coverage created an arena or changed old slot");
		DWORD previous = 0;
		Check(Name, VirtualProtect(pages.base, page, PAGE_READWRITE, &previous) != 0 &&
		            VirtualProtect(static_cast<uint8_t*>(pages.base) + page, page, PAGE_NOACCESS, &previous) != 0,
		      "configure partial backing coverage");
		Check(Name, Np::CreateHeap(&sns.handle, pages.base, page * 2, "partial-coverage") == Libs::NpManagerMemory::HeapFailure &&
		            sns.handle == old_handle, "create validated only the first backing page");
	}
#endif
	Np::TestFailNextHeapCreation();
	Check(Name, Np::CreateHeap(&sns.handle, optional_base, 0x10000, "forced-failure") == Libs::NpManagerMemory::HeapFailure &&
	            sns.handle == old_handle, "postvalidation create failure must preserve old arena");
	void* first = Np::Malloc(&sns.handle, 0x40);
	void* tail = Np::Malloc(&sns.handle, 0xffc0);
	Check(Name, first && tail && Np::Malloc(&sns.handle, 1) == nullptr, "bounded split heap exhaustion");
	std::memset(first, 0x71, 0x40);
	Check(Name, Np::Realloc(&sns.handle, first, 0x80) == nullptr && static_cast<uint8_t*>(first)[0x3f] == 0x71,
	      "failed realloc lost original allocation");
	Np::Handle stale = sns.handle;
	destroy_heap(&sns.handle); // Outstanding blocks invalidated; mapping remains caller-owned.
	Check(Name, sns.handle == nullptr && Np::Malloc(&stale, 1) == nullptr && Np::Realloc(&stale, first, 1) == nullptr,
	      "destroy left a live copied handle");
	CheckOk(Name, Memory::KernelVirtualQuery(optional_base, 0, &info, sizeof(info)), "destroy must not unmap backing");
	static_cast<uint8_t*>(optional_base)[0xffff] = 0x5a;
	Np::DestroyHeap(&sns.handle); // Native zero-handle destroy is a no-op.
	CheckOk(Name, Np::CreateHeap(&sns.handle, optional_base, 0x10000, nullptr), "recreate with native default diagnostic name");
	Check(Name, sns.handle != stale && Np::Malloc(&stale, 1) == nullptr, "new arena reused retired handle identity");
	Np::DestroyHeap(&stale);
	void* active = sns.ordinary.allocate(0x40, sns.ordinary.context);
	Check(Name, active != nullptr, "retiring stale copied slot destroyed replacement arena");

	GuardedRecord second;
	second.record.reserved = UINT64_MAX;
	CheckOk(Name, Np::MapMemory(0x4001, "split-second", nullptr, &second.record), "rounding/optional output map");
	Check(Name, second.record.size == 0x8000 && second.record.reserved == 0, "map rounding/reserved output");
	Np::Handle retained = sns.handle;
	CheckOk(Name, Np::CreateHeap(&sns.handle, second.record.mapping, second.record.size, "duplicate"), "duplicate slot create");
	Check(Name, sns.handle != retained && Np::Realloc(&retained, active, 0x40) == active,
	      "successful duplicate create implicitly destroyed previous heap");
	Check(Name, sns.ordinary.allocate(0x40, sns.ordinary.context) == second.record.mapping,
	      "retained native descriptor did not follow current handle slot");
	Np::DestroyHeap(&sns.handle);
	Check(Name, Np::Realloc(&retained, active, 0x40) == active, "destroy crossed arena ownership");
	Np::DestroyHeap(&retained);
	CheckOk(Name, Np::UnmapMemory(&second.record), "unmap separately-owned second backing");
	Check(Name, second.record.mapping == nullptr && second.record.size == 0x8000 && second.record.reserved == 0,
	      "unmap clobbered mapping metadata");
	Canaries(second);

	GuardedRecord failed;
	failed.record = {0x10000, UINT64_MAX, optional_base};
	void* untouched = reinterpret_cast<void*>(uintptr_t {0x5678});
	Check(Name, Np::MapMemory(0x4001, nullptr, &untouched, &failed.record) == Libs::LibKernel::KERNEL_ERROR_EFAULT &&
	            failed.record.size == 0x8000 && failed.record.reserved == 0 && failed.record.mapping == optional_base &&
	            untouched == reinterpret_cast<void*>(uintptr_t {0x5678}), "map failure partial writes/optional output");
	for (uint64_t size: {uint64_t {0}, UINT64_MAX}) {
		failed.record.reserved = 0x1234;
		Check(Name, Np::MapMemory(size, "wrap", &untouched, &failed.record) == Libs::LibKernel::KERNEL_ERROR_EINVAL &&
		            failed.record.size == 0 && failed.record.reserved == 0 && failed.record.mapping == optional_base &&
		            untouched == reinterpret_cast<void*>(uintptr_t {0x5678}), "zero/wrapping map must preserve failed outputs");
	}
	// Kernel rejects length zero; native unmap wrapper still clears backing and acknowledges.
	failed.record.reserved = 0x1234;
	CheckOk(Name, Np::UnmapMemory(&failed.record), "unmap ignores backend failure");
	Check(Name, failed.record.mapping == nullptr && failed.record.size == 0 && failed.record.reserved == 0x1234,
	      "unmap-failure field preservation");
	CheckOk(Name, Memory::KernelVirtualQuery(optional_base, 0, &info, sizeof(info)), "failed unmap must not remove mapping");
	Canaries(failed);
	sns.mapping.reserved = 0x12345678;
	CheckOk(Name, unmap_memory(&sns.mapping), "release original backing after heap retirement");
	Check(Name, sns.mapping.mapping == nullptr && sns.mapping.size == 0x10000 && sns.mapping.reserved == 0x12345678 &&
	            Memory::KernelVirtualQuery(optional_base, 0, &info, sizeof(info)) != 0, "unmap ownership/field contract");
	CheckOk(Name, Np::UnmapMemory(&sns.mapping), "zero-backing unmap");
	Check(Name, Common::GetHostLockCriticalCounter()->load() == depth, "split heap leaked host-lock critical depth");
}
