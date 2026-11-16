// Included after File and g_current_fs_context in fileSystem.cpp. Keeping this
// implementation here avoids a new production/test CMake source dependency.
namespace SocketpairProvenance {
namespace {
std::atomic<Buffer*> g_buffer {nullptr};
static_assert(std::atomic<Buffer*>::is_always_lock_free);
thread_local uint64_t g_read_caller = 0;

uint64_t Reserve(Bank bank) noexcept {
	auto* buffer = g_buffer.load(std::memory_order_acquire);
	if (buffer == nullptr) { return NoSlot; }
	const auto b = static_cast<size_t>(bank);
	auto index = buffer->reserved[b].load(std::memory_order_relaxed);
	while (index < Capacities[b]) {
		if (buffer->reserved[b].compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {
			return Offsets[b] + index;
		}
	}
	buffer->dropped[b].fetch_add(1, std::memory_order_relaxed);
	return NoSlot;
}

Record Start(Event event, uint64_t caller) noexcept {
	Record record;
	record.sequence = g_buffer.load(std::memory_order_relaxed)->sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	record.event = static_cast<uint64_t>(event);
	record.caller = caller;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	record.host_tid = GetCurrentThreadId();
#endif
	// The existing TLS shared_ptr already retains this launch context. Merely
	// record its address: no refcount churn, snapshots, locks or loader lookup.
	record.fs_context = reinterpret_cast<uint64_t>(g_current_fs_context.get());
	return record;
}

void Commit(uint64_t index, const Record& record) noexcept {
	auto& slot = g_buffer.load(std::memory_order_relaxed)->slots[index];
	slot.record = record;
	slot.commit.store(index + 1, std::memory_order_release);
}

bool CopyPair(uint64_t address, uint64_t* packed) noexcept {
	*packed = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const DWORD saved_error = GetLastError();
	SIZE_T copied = 0;
	uint64_t local = 0;
	const bool ok = address != 0 && ReadProcessMemory(GetCurrentProcess(),
	    reinterpret_cast<const void*>(address), &local, sizeof(local), &copied) && copied == sizeof(local);
	SetLastError(saved_error);
	if (ok) { *packed = local; }
	return ok;
#else
	(void)address;
	return false;
#endif
}
} // namespace

void Initialize() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (Enabled()) { return; }
	const char* enabled = std::getenv("KYTY_SOCKETPAIR_PROVENANCE");
	if (enabled == nullptr || std::strcmp(enabled, "1") != 0) { return; }
	// Host initialization only. The view intentionally lasts for the process,
	// like ipmiObserve; no teardown races, explicit flush or guest-path file I/O.
	try {
		wchar_t local[32768] {};
		const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
		LARGE_INTEGER now {};
		if (length == 0 || length >= 32768 || !QueryPerformanceCounter(&now)) { return; }
		auto directory = std::filesystem::path(local) / "Temp" / "opencode";
		std::filesystem::create_directories(directory);
		auto path = directory / ("socketpair-provenance-" + std::to_string(GetCurrentProcessId()) +
		                         "-" + std::to_string(now.QuadPart) + ".bin");
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
		    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) { return; }
		HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, sizeof(Buffer), nullptr);
		void* view = mapping == nullptr ? nullptr : MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Buffer));
		if (mapping != nullptr) { CloseHandle(mapping); }
		CloseHandle(file);
		if (view == nullptr) { DeleteFileW(path.c_str()); return; }
		auto* buffer = new (view) Buffer {};
		buffer->pid = GetCurrentProcessId();
		buffer->ready.store(1, std::memory_order_release);
		g_buffer.store(buffer, std::memory_order_release);
	} catch (...) {
		// Probe setup failure only disables observation.
	}
#endif
}

bool Enabled() noexcept { return g_buffer.load(std::memory_order_acquire) != nullptr; }

void Created(File* file) noexcept {
	if (auto* buffer = g_buffer.load(std::memory_order_acquire); buffer != nullptr) {
		file->probe_generation = buffer->generations.fetch_add(1, std::memory_order_relaxed) + 1;
		file->probe_creator_context = reinterpret_cast<uint64_t>(g_current_fs_context.get());
	}
}

void Observe(Event event, int fd, File* file, uint64_t caller, uint64_t count) noexcept {
	if (!Enabled() || file == nullptr) { return; }
	const auto kind = file->special.load(std::memory_order_relaxed);
	const bool gls = file->probe_gls.load(std::memory_order_relaxed);
	const bool notification = kind == SpecialFile::Notification;
	// All notification/GLS descriptors, regardless of fd; fd8 is additional
	// coverage for the original run. No path parsing/allocation on read/close.
	if (fd != 8 && !notification && !gls) { return; }
	Bank bank = Bank::Lifecycle;
	if (event == Event::ReadDispatch) {
		if (count != 10) { return; }
		// First two eligible reads of each File generation, including its caller.
		// Once sampled, the hot loop performs only loads; no allocation/OS call.
		auto sampled = file->probe_reads.load(std::memory_order_relaxed);
		while (sampled < 2 && !file->probe_reads.compare_exchange_weak(sampled, sampled + 1, std::memory_order_relaxed)) {}
		if (sampled >= 2) { return; }
		bank = notification ? Bank::NotificationRead : Bank::OtherRead;
	}
	const uint64_t slot = Reserve(bank);
	if (slot == NoSlot) { return; }
	auto record = Start(event, caller);
	record.fd = static_cast<uint64_t>(static_cast<int64_t>(fd));
	record.object = reinterpret_cast<uint64_t>(file);
	record.generation = file->probe_generation;
	record.kind = static_cast<uint64_t>(kind);
	record.detail[0] = file->opened.load(std::memory_order_relaxed);
	record.detail[1] = gls;
	record.detail[2] = file->probe_creator_context;
	record.detail[3] = count;
	Commit(slot, record);
}

ReadCallerScope::ReadCallerScope(uint64_t caller) noexcept: previous(g_read_caller), active(caller != 0) {
	if (active) { g_read_caller = caller; }
}
ReadCallerScope::~ReadCallerScope() { if (active) { g_read_caller = previous; } }
uint64_t ReadCaller(uint64_t fallback) noexcept { return g_read_caller != 0 ? g_read_caller : fallback; }

SocketpairCall::SocketpairCall(uint64_t caller, uint64_t domain, uint64_t type,
                               uint64_t protocol, uint64_t output) noexcept {
	slot = Reserve(Bank::Socketpair);
	if (slot == NoSlot) { return; }
	record = Start(Event::Socketpair, caller);
	record.object = output;
	record.detail[0] = domain;
	record.detail[1] = type;
	record.detail[2] = protocol;
	if (CopyPair(output, &record.detail[4])) { record.kind |= 1; }
}

void SocketpairCall::Finish(uint64_t result) noexcept {
	if (slot == NoSlot) { return; }
	record.detail[3] = result;
	if (CopyPair(record.object, &record.detail[5])) { record.kind |= 2; }
	if (record.kind == 3 && record.detail[4] == record.detail[5]) { record.kind |= 4; }
	Commit(slot, record);
	slot = NoSlot;
}
} // namespace SocketpairProvenance
