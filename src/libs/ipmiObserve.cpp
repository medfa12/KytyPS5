#include "libs/ipmiObserve.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::IpmiObserve {
namespace {
std::atomic<Buffer*> buffer {nullptr};
std::atomic<uint32_t> np_heap_callbacks {0};
std::atomic<uint32_t> np_heap_ex_new_callbacks {0};
std::atomic<uint32_t> np_heap_descriptors {0}, np_heap_ex_descriptors {0}, stack_failures {0};
std::atomic<uint32_t> net_pool_routine {0}, net_pool_snapshots {0};
thread_local bool net_pool_snapshot_result = false;
std::atomic<uint32_t> ssl_routine {0}, ssl_snapshots {0};
thread_local bool ssl_snapshot_result = false;
wchar_t output_path[32768] {};

bool ReserveBudget(std::atomic<uint32_t>& count, uint32_t limit) noexcept {
	auto value = count.load(std::memory_order_relaxed);
	do {
		if (value >= limit) { return false; }
	} while (!count.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
	return true;
}

void Publish(Bank bank, Record record) noexcept {
	auto* output = buffer.load(std::memory_order_acquire);
	if (output == nullptr) { return; }
#ifdef _WIN32
	LARGE_INTEGER now {};
	QueryPerformanceCounter(&now);
	record.tick = static_cast<uint64_t>(now.QuadPart);
	record.host_tid = GetCurrentThreadId();
#endif
	output->Append(bank, record);
}
bool Name(const char* source, std::array<char, 32>& name) noexcept {
	name.fill(0);
	if (source == nullptr) { return false; }
	// Byte-wise checked reads stop at NUL, including at a readable page boundary.
	for (size_t i = 0; i < name.size() - 1; ++i) {
		if (!Read(source + i, &name[i], 1)) { name.fill(0); return false; }
		if (name[i] == '\0') { return true; }
	}
	return true; // bounded, terminated preview
}
} // namespace

Buffer* GetBuffer() noexcept { return buffer.load(std::memory_order_acquire); }
bool Enabled() noexcept { return GetBuffer() != nullptr; }
const wchar_t* OutputPath() noexcept { return output_path; }

bool Read(const void* source, void* destination, size_t size) noexcept {
	// Never follow arbitrary payload pointers with memcpy or VirtualQuery alone.
	// Partial/faulting copies are unavailable, not successful zero-valued snapshots.
	std::memset(destination, 0, size);
#ifdef _WIN32
	SIZE_T copied = 0;
	if (source != nullptr && ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) && copied == size) {
		return true;
	}
	std::memset(destination, 0, size);
#endif
	return false;
}

void Initialize() noexcept {
#ifdef _WIN32
	if (Enabled()) { return; }
	const char* enabled = std::getenv("KYTY_IPMI_OBSERVE");
	if (enabled == nullptr || std::string_view(enabled) != "1") { return; }
	try {
		wchar_t local[32768] {};
		const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
		if (length == 0 || length >= 32768) { return; }
		LARGE_INTEGER frequency {}, now {};
		if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&now)) { return; }
		auto directory = std::filesystem::path(local) / "Temp" / "opencode";
		std::filesystem::create_directories(directory);
		auto path = directory / ("ipmi-observe-" + std::to_string(GetCurrentProcessId()) +
		                         "-" + std::to_string(now.QuadPart) + ".bin");
		if (path.native().size() >= std::size(output_path)) { return; }
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
		    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) { return; }
		HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, sizeof(Buffer), nullptr);
		void* view = mapping == nullptr ? nullptr : MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Buffer));
		if (mapping != nullptr) { CloseHandle(mapping); }
		CloseHandle(file);
		if (view == nullptr) { DeleteFileW(path.c_str()); return; }
		auto* result = new (view) Buffer {};
		result->pid = GetCurrentProcessId();
		result->frequency = static_cast<uint64_t>(frequency.QuadPart);
		result->start_tick = static_cast<uint64_t>(now.QuadPart);
		std::memcpy(output_path, path.c_str(), (path.native().size() + 1) * sizeof(wchar_t));
		result->ready.store(1, std::memory_order_release);
		buffer.store(result, std::memory_order_release);
	} catch (...) {
		// Observation failure has no effect on emulation.
	}
#endif
}

void Emit(Bank bank, Event event, const Values& values, const char* name) noexcept {
	auto* output = GetBuffer();
	if (output == nullptr) { return; }
	const auto index = static_cast<size_t>(bank);
	if (index >= CAPACITIES.size()) { return; }
	if (output->attempted[index].load(std::memory_order_relaxed) >= CAPACITIES[index]) {
		output->attempted[index].fetch_add(1, std::memory_order_relaxed);
		output->dropped[index].fetch_add(1, std::memory_order_relaxed);
		return;
	}
	Record record;
	record.event = static_cast<uint64_t>(event);
	record.values = values;
	if (name != nullptr && Name(name, record.name)) { record.values[14] |= NameReadable; }
	Publish(bank, record);
}

void ClientCreated(void* client, const void* config, void* storage, uint64_t caller) noexcept {
	if (!Enabled()) { return; }
	uint64_t vtable = 0;
	const bool readable = Read(client, &vtable, sizeof(vtable));
	Emit(Bank::Lifecycle, Event::ClientCreate,
	    {reinterpret_cast<uint64_t>(client), caller, vtable, reinterpret_cast<uint64_t>(config),
	     reinterpret_cast<uint64_t>(storage), storage == nullptr, 0, 0, 0, 0, 0, 0, 0, 0,
	     readable ? uint64_t {VtableReadable} : 0},
	    client == nullptr ? nullptr : static_cast<const char*>(client) + 0x10);
}

void PsnFailure(uint64_t caller, const std::array<uint64_t, 6>& arguments) noexcept {
	if (!Enabled() || (caller != 0x910c4e192ull && caller != 0x910c4db75ull)) { return; }
	uint64_t stage = 0, service = 0, create_binding = 0, allocator_binding = 0;
	const bool roots = Read(reinterpret_cast<void*>(0x9121578d8ull), &stage, 8) &&
	    Read(reinterpret_cast<void*>(0x912156570ull), &service, 8);
	const bool bindings = Read(reinterpret_cast<void*>(0x91208e368ull), &create_binding, 8) &&
	    Read(reinterpret_cast<void*>(0x91208e388ull), &allocator_binding, 8);
	// Eight independently published chunks cap the original format at 192 bytes.
	// Never format guest arguments or alter the variadic context here.
	for (uint64_t offset = 0; offset < 192; offset += 24) {
		Record record;
		record.event = static_cast<uint64_t>(Event::PsnFailure);
		record.values = {caller, arguments[0], arguments[1], arguments[2], arguments[3],
		    arguments[4], arguments[5], offset, stage, service, create_binding, allocator_binding,
		    roots, bindings};
		bool readable = true, terminated = false;
		for (size_t i = 0; i < record.preview.size(); ++i) {
			if (arguments[0] > UINT64_MAX - offset - i ||
			    !Read(reinterpret_cast<void*>(arguments[0] + offset + i), &record.preview[i], 1)) {
				readable = false;
				break;
			}
			if (record.preview[i] == 0) { terminated = true; break; }
		}
		if (readable) { record.values[14] = PreviewReadable; }
		record.values[15] = terminated;
		Publish(Bank::Gateway, record);
		if (terminated || !readable) { break; }
	}
}

void NpHeapRecord(const Values& values) noexcept {
	if (!Enabled()) { return; }
	auto* budget = values[1] == 2 ? &np_heap_descriptors :
	               values[1] == 3 ? &np_heap_ex_descriptors :
	               values[1] == 4 && values[0] == 0xa80018662ull && values[4] == 0x40 ? &np_heap_ex_new_callbacks :
	               values[1] >= 4 && values[1] <= 6 ? &np_heap_callbacks : nullptr;
	if (budget != nullptr) {
		const uint32_t limit = budget == &np_heap_callbacks ? 16 : 8;
		auto count = budget->load(std::memory_order_relaxed);
		do {
			if (count >= limit) { return; }
		} while (!budget->compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
	}
	Emit(Bank::Gateway, Event::NpHeap, values);
}

void StackFailure(uint64_t caller, const void* frame, uint64_t guard) noexcept {
	if (!Enabled()) { return; }
	auto count = stack_failures.load(std::memory_order_relaxed);
	do {
		if (count >= 4) { return; }
	} while (!stack_failures.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
	const auto base = reinterpret_cast<uint64_t>(frame);
	Emit(Bank::Recovery, Event::StackFailure, {caller, base, 0, guard});
	// Frame is this HLE's compiler frame, not an assumed native canary location.
	// Capture 96 qwords starting with saved frame/return slots; no guest writes.
	for (uint64_t chunk = 0; chunk < 8; ++chunk) {
		const uint64_t offset = chunk * 96;
		if (base > UINT64_MAX - offset - 95) { break; }
		Values values {caller, base + offset, chunk + 1};
		for (size_t i = 0; i < 12; ++i) {
			if (Read(reinterpret_cast<const void*>(base + offset + i * 8), &values[4 + i], 8)) {
				values[3] |= uint64_t {1} << i;
			}
		}
		Emit(Bank::Recovery, Event::StackFailure, values);
	}
}

void NpMemoryCreated(uint64_t caller, size_t size, const char* name, const void* supplied,
                     uint32_t mode, int32_t status) noexcept {
	// Value15 tags corrected status-return ABI; historical event19 had pointer-return semantics.
	Emit(Bank::Gateway, Event::NpMemoryResult,
	    {caller, size, reinterpret_cast<uint64_t>(name), reinterpret_cast<uint64_t>(supplied),
	     mode, static_cast<uint32_t>(status), 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
}

void NetPoolCreated(uint64_t caller, const char* name, int32_t size, int32_t flags, int32_t result) noexcept {
	if (!Enabled()) { return; }
	// Reserve one first-full result independently of the routine budget. No net
	// error conversion here: wrappers forward the actual FinishNetCall result.
	const bool first_full = net_pool_snapshot_result;
	net_pool_snapshot_result = false;
	if (!first_full && !ReserveBudget(net_pool_routine, NetPoolRoutineLimit)) { return; }
	Emit(Bank::Recovery, Event::NetPool,
	    {caller, 0, result >= 0 ? static_cast<uint32_t>(result) : UINT32_MAX,
	     static_cast<uint32_t>(result), static_cast<uint32_t>(size), static_cast<uint32_t>(flags),
	     reinterpret_cast<uint64_t>(name)}, name);
}

void NetPoolDestroyed(uint64_t caller, int32_t id, int32_t result) noexcept {
	if (!Enabled() || !ReserveBudget(net_pool_routine, NetPoolRoutineLimit)) { return; }
	Emit(Bank::Recovery, Event::NetPool, {caller, 1, static_cast<uint32_t>(id), static_cast<uint32_t>(result)});
}

void NetPoolFullSnapshot(const std::array<NetPoolEntry, NetPoolOccupiedLimit>& entries) noexcept {
	if (!Enabled() || !ReserveBudget(net_pool_snapshots, 1)) { return; }
	net_pool_snapshot_result = true;
	for (const auto& entry: entries) {
		if (entry.id <= 0) { continue; } // Unused tail when the instance has32 slots.
		Record record;
		record.event = static_cast<uint64_t>(Event::NetPool);
		record.values = {0, 2, static_cast<uint32_t>(entry.id), 0, static_cast<uint32_t>(entry.size)};
		record.values[14] = NameReadable;
		record.name = entry.name; // Trusted fixed snapshot, no ReadProcessMemory or formatting.
		Publish(Bank::Recovery, record);
	}
}

void NetPoolConfigured(uint64_t caller, const void* config, int32_t result) noexcept {
	if (!Enabled() || !ReserveBudget(net_pool_routine, NetPoolRoutineLimit)) { return; }
	Record record;
	record.event = static_cast<uint64_t>(Event::NetPool);
	record.values = {caller, 3, 0, static_cast<uint32_t>(result), 0, 0, reinterpret_cast<uint64_t>(config)};
	std::array<uint64_t, 4> bytes {};
	if (Read(config, bytes.data(), sizeof(bytes))) {
		record.values[4] = static_cast<uint32_t>(bytes[0]);
		std::copy(bytes.begin(), bytes.end(), record.values.begin() + 8);
		record.values[14] = DescriptorReadable;
	}
	Publish(Bank::Recovery, record);
}

void SslInitialized(uint64_t caller, uint64_t size, int32_t result) noexcept {
	if (!Enabled()) { return; }
	const bool first_full = ssl_snapshot_result;
	ssl_snapshot_result = false;
	if (!first_full && !ReserveBudget(ssl_routine, SslRoutineLimit)) { return; }
	Emit(Bank::Recovery, Event::SslContext,
	    {caller, 0, result >= 0 ? static_cast<uint32_t>(result) : UINT32_MAX,
	     static_cast<uint32_t>(result), size});
}

void SslTerminated(uint64_t caller, int32_t id, int32_t result) noexcept {
	if (!Enabled() || !ReserveBudget(ssl_routine, SslRoutineLimit)) { return; }
	Emit(Bank::Recovery, Event::SslContext, {caller, 1, static_cast<uint32_t>(id), static_cast<uint32_t>(result)});
}

void SslFullSnapshot(const std::array<SslEntry, SslOccupiedLimit>& entries) noexcept {
	if (!Enabled() || !ReserveBudget(ssl_snapshots, 1)) { return; }
	ssl_snapshot_result = true;
	for (const auto& entry: entries) {
		Record record;
		record.event = static_cast<uint64_t>(Event::SslContext);
		record.values = {0, 2, static_cast<uint32_t>(entry.id), 0, entry.size};
		Publish(Bank::Recovery, record);
	}
}

void NpMemoryCallback(uint64_t caller, uint32_t operation, size_t size, const void* pointer,
                      const void* context, const void* result, uint64_t count) noexcept {
	Emit(Bank::Gateway, Event::NpMemoryCallback,
	    {caller, operation, size, reinterpret_cast<uint64_t>(pointer), reinterpret_cast<uint64_t>(context),
	     reinterpret_cast<uint64_t>(result), count});
}

void NpMutexQuery(const void* mutex, int result, uint64_t caller) noexcept {
	if (!Enabled() || caller != 0x910ca27c6ull) { return; }
	const auto address = reinterpret_cast<uint64_t>(mutex);
	Values values {address, caller, static_cast<uint32_t>(result)};
	// Exhausted queries still count as drops, without further guest reads.
	if (GetBuffer()->attempted[4].load(std::memory_order_relaxed) < CAPACITIES[4]) {
		uint8_t initialized = 0;
		if (address != 0 && address <= UINT64_MAX - 0x10 &&
		    Read(reinterpret_cast<const void*>(address + 0x10), &initialized, 1)) {
			values[3] = initialized;
			values[14] = MutexByteReadable;
		}
	}
	Emit(Bank::Gateway, Event::NpMutexQuery, values);
}

void ServerCreated(void* server, const void* config, void* arg2, void* storage, uint64_t caller) noexcept {
	if (!Enabled()) { return; }
	// Copy only the established 0x38-byte Server::Config layout. No handler dereference.
	std::array<uint8_t, 0x38> bytes {};
	std::array<char, 32> name {};
	uint64_t sizes[2] {}, handler = 0, vtable = 0, flags = 0;
	const bool readable = Read(config, bytes.data(), bytes.size());
	if (readable) {
		std::memcpy(sizes, bytes.data(), sizeof(sizes));
		std::memcpy(&handler, bytes.data() + 0x10, sizeof(handler));
		flags = bytes[0x18] | (uint64_t {bytes[0x32]} << 8) | (uint64_t {bytes[0x33]} << 16);
		std::memcpy(name.data(), bytes.data() + 0x19, 25);
	}
	const bool vt = Read(server, &vtable, sizeof(vtable));
	Emit(Bank::Lifecycle, Event::ServerCreate,
	    {reinterpret_cast<uint64_t>(server), caller, vtable, reinterpret_cast<uint64_t>(config),
	     reinterpret_cast<uint64_t>(arg2), reinterpret_cast<uint64_t>(storage), sizes[0], sizes[1], handler, flags,
	     0, 0, 0, 0, (readable ? uint64_t {DescriptorReadable} : 0) | (vt ? uint64_t {VtableReadable} : 0)},
	    readable ? name.data() : nullptr);
}

Invocation::Invocation(void* client, uint32_t method, const void* inputs, uint32_t input_count,
                       void* outputs, uint32_t output_count, int* result, uint64_t caller, bool is_raw,
                       const uint64_t* raw_written) noexcept:
	raw(is_raw) {
	auto* output = GetBuffer();
	if (output == nullptr) { return; }
	const auto invoke_full = output->attempted[1].load(std::memory_order_relaxed) >= CAPACITIES[1];
	const auto recovery_full = output->attempted[2].load(std::memory_order_relaxed) >= CAPACITIES[2];
	if (invoke_full && (method != 0x160008 || recovery_full)) {
		// Unclassified method 0x160008 overflow is counted in Invoke once both
		// banks are full; no guest-memory reads are needed after exhaustion.
		output->attempted[1].fetch_add(1, std::memory_order_relaxed);
		output->dropped[1].fetch_add(1, std::memory_order_relaxed);
		return;
	}
	entry.call = output->calls.fetch_add(1, std::memory_order_relaxed) + 1;
	entry.event = static_cast<uint64_t>(raw ? Event::RawEnter : Event::InvokeEnter);
	entry.values = {reinterpret_cast<uint64_t>(client), caller, 0, method,
	    reinterpret_cast<uint64_t>(inputs), input_count, reinterpret_cast<uint64_t>(outputs), output_count,
	    reinterpret_cast<uint64_t>(result)};
	entry.values[15] = reinterpret_cast<uint64_t>(raw_written);
	if (client != nullptr && Name(static_cast<const char*>(client) + 0x10, entry.name)) {
		entry.values[14] |= NameReadable;
		if (method == 0x160008 && std::strcmp(entry.name.data(), "SceDbPreparationServer") == 0) {
			bank = Bank::Recovery;
			entry.values[14] |= RecoveryTarget;
		}
	}
	const auto kind = static_cast<size_t>(bank);
	if (output->attempted[kind].load(std::memory_order_relaxed) >= CAPACITIES[kind]) {
		output->attempted[kind].fetch_add(1, std::memory_order_relaxed);
		output->dropped[kind].fetch_add(1, std::memory_order_relaxed);
		return;
	}
	active = true;
	if (Read(client, &entry.values[2], sizeof(uint64_t))) { entry.values[14] |= VtableReadable; }
	Snapshot(entry);
	Publish(bank, entry);
	// At most two input descriptors; never dereference their payloads.
	for (uint32_t i = 0; inputs != nullptr && i < input_count && i < 2; ++i) {
		Input descriptor {};
		const auto address = reinterpret_cast<uintptr_t>(inputs) + i * sizeof(Input);
		const bool valid = address >= reinterpret_cast<uintptr_t>(inputs) && Read(reinterpret_cast<void*>(address), &descriptor, sizeof(descriptor));
		Record record;
		record.event = static_cast<uint64_t>(Event::Input);
		record.call = entry.call;
		record.values = {reinterpret_cast<uint64_t>(client), caller, entry.values[2], method, i,
		    reinterpret_cast<uint64_t>(descriptor.data), descriptor.size, address, 0, 0, 0, 0, 0, 0,
		    valid ? uint64_t {DescriptorReadable} : 0};
		Publish(bank, record);
	}
}

void Invocation::Snapshot(Record& record) noexcept {
	record.values[14] &= NameReadable | VtableReadable | RecoveryTarget;
	record.preview.fill(0);
	record.values[9] = record.values[10] = record.values[11] = record.values[12] = 0;
	int result = 0;
	if (Read(reinterpret_cast<void*>(record.values[8]), &result, sizeof(result))) {
		record.values[12] = static_cast<uint32_t>(result);
		record.values[14] |= ResultReadable;
	}
	Output descriptor {};
	if (record.values[7] != 0 && Read(reinterpret_cast<void*>(record.values[6]), &descriptor, sizeof(descriptor))) {
		record.values[9] = reinterpret_cast<uint64_t>(descriptor.data);
		record.values[10] = descriptor.capacity;
		record.values[11] = descriptor.written;
		if (raw) {
			if (Read(reinterpret_cast<void*>(record.values[15]), &record.values[11], sizeof(uint64_t))) {
				record.values[14] |= RawWrittenReadable;
			}
		}
		record.values[14] |= DescriptorReadable;
		if (bank == Bank::Recovery && descriptor.capacity >= record.preview.size() &&
		    Read(descriptor.data, record.preview.data(), record.preview.size())) {
			record.values[14] |= PreviewReadable;
		}
	}
}

void Invocation::Finish(int transport) noexcept {
	if (!active) { return; }
	Record exit = entry;
	exit.event = static_cast<uint64_t>(raw ? Event::RawExit : Event::InvokeExit);
	Snapshot(exit);
	exit.values[13] = static_cast<uint32_t>(transport);
	if ((entry.values[14] & PreviewReadable) && (exit.values[14] & PreviewReadable) &&
	    entry.values[9] == exit.values[9] && entry.preview == exit.preview) {
		exit.values[14] |= PreviewUnchanged;
	}
	Publish(bank, exit);
	active = false;
}
} // namespace Libs::IpmiObserve
