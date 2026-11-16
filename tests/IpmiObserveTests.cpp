#include "libs/ipmiObserve.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace Libs::IpmiObserve;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (false)

static void PublicationAndBudgets() {
	auto b = std::make_unique<Buffer>();
	// Producer suspended after reservation: readers must skip its hole; other
	// producers and lifecycle/recovery banks must still progress independently.
	b->attempted[1].fetch_add(1);
	std::atomic<bool> done {false};
	std::thread reader([&] {
		while (!done.load(std::memory_order_acquire)) {
			for (size_t i = OFFSETS[1] + 1; i < OFFSETS[2]; ++i) {
				if (b->slots[i].commit.load(std::memory_order_acquire) == i + 1) {
					const auto& r = b->slots[i].record;
					CHECK(r.values[2] == (r.values[0] ^ r.values[1]));
					CHECK(r.preview[23] == 0xa5);
				}
			}
		}
	});
	std::vector<std::thread> writers;
	for (uint64_t t = 0; t < 4; ++t) {
		writers.emplace_back([&, t] {
			for (uint64_t i = 0; i < 1000; ++i) {
				Record r; r.event = 7; r.values = {t, i, t ^ i}; r.preview[23] = 0xa5;
				b->Append(Bank::Invoke, r);
			}
		});
	}
	for (auto& w: writers) { w.join(); }
	done.store(true, std::memory_order_release); reader.join();
	CHECK(b->attempted[1] == 4001 && b->dropped[1] == 4001 - CAPACITIES[1]);
	CHECK(b->slots[OFFSETS[1]].commit == 0);
	for (size_t i = OFFSETS[1] + 1; i < OFFSETS[2]; ++i) { CHECK(b->slots[i].commit == i + 1); }
	const auto saved = b->slots[OFFSETS[2] - 1].record;
	b->Append(Bank::Invoke, {});
	CHECK(std::memcmp(&saved, &b->slots[OFFSETS[2] - 1].record, sizeof(saved)) == 0);
	for (Bank bank: {Bank::Lifecycle, Bank::Recovery, Bank::Poll, Bank::Gateway}) {
		const auto k = static_cast<size_t>(bank);
		b->Append(bank, {});
		CHECK(b->slots[OFFSETS[k]].commit == OFFSETS[k] + 1);
		CHECK(b->dropped[k] == 0);
	}
}

#ifdef _WIN32
static void MappedSnapshots() {
	_putenv_s("KYTY_IPMI_OBSERVE", ""); Initialize(); CHECK(!Enabled());
	Invocation disabled(reinterpret_cast<void*>(1), 0x160008, nullptr, 0, nullptr, 0, nullptr, 0);
	disabled.Finish(0); CHECK(!Enabled());
	NpMutexQuery(reinterpret_cast<void*>(1), 1, 0x910ca27c6ull); CHECK(!Enabled());
	NpMemoryCreated(1, 0x4000, reinterpret_cast<const char*>(1), nullptr, 0, -1);
	NpMemoryCallback(2, 0, 0x568, nullptr, nullptr, reinterpret_cast<void*>(1), 1);
	for (size_t i = 0; i < 20; ++i) { NpHeapRecord({1, 4, 1, 1, 0x60}); }
	StackFailure(1, reinterpret_cast<void*>(1), 0);
	NetPoolCreated(1, reinterpret_cast<const char*>(1), 0x4000, 0, 1);
	NetPoolDestroyed(2, -1, -7);
	NetPoolConfigured(3, reinterpret_cast<const void*>(1), -7);
	NetPoolFullSnapshot({});
	SslInitialized(1, UINT64_MAX, static_cast<int32_t>(0x8095f008u));
	SslTerminated(2, -1, -7);
	SslFullSnapshot({});
	CHECK(!Enabled());
	_putenv_s("KYTY_IPMI_OBSERVE", "1"); Initialize(); CHECK(Enabled());
	auto* b = GetBuffer(); CHECK(b->ready.load(std::memory_order_acquire) == 1);
	// Synthetic client matches only the HLE fields the recorder inspects.
	std::array<uint8_t, 0x100> client {};
	const uint64_t vtable = 0x12345678;
	std::memcpy(client.data(), &vtable, sizeof(vtable));
	std::memcpy(client.data() + 0x10, "SceDbPreparationServer", sizeof("SceDbPreparationServer"));
	std::array<uint8_t, 24> status {};
	Output descriptor {status.data(), status.size(), 999};
	int method_result = -99;
	Invocation invocation(client.data(), 0x160008, nullptr, 0, &descriptor, 1, &method_result, 0xabcdef);
	// The production generic fallback changes only these outputs.
	method_result = 0; descriptor.written = 0;
	invocation.Finish(0);
	const auto& before = b->slots[OFFSETS[2]].record;
	const auto& after = b->slots[OFFSETS[2] + 1].record;
	CHECK(before.call == after.call && before.values[1] == 0xabcdef && before.values[2] == vtable);
	CHECK(before.values[11] == 999 && before.values[12] == static_cast<uint32_t>(-99));
	CHECK(after.values[11] == 0 && after.values[12] == 0 && after.values[13] == 0);
	CHECK((after.values[14] & (RecoveryTarget | PreviewReadable | PreviewUnchanged)) ==
	      (RecoveryTarget | PreviewReadable | PreviewUnchanged));
	CHECK((status == std::array<uint8_t, 24> {}));
	Invocation changed(client.data(), 0x160008, nullptr, 0, &descriptor, 1, &method_result, 0);
	status[20] = 3; changed.Finish(-7);
	CHECK(!(b->slots[OFFSETS[2] + 3].record.values[14] & PreviewUnchanged));
	CHECK(b->slots[OFFSETS[2] + 3].record.preview[20] == 3);
	// No blind reads across protection boundaries, including partial snapshots.
	auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	CHECK(pages != nullptr);
	DWORD old = 0; CHECK(VirtualProtect(pages + 4096, 4096, PAGE_NOACCESS, &old));
	descriptor.data = pages + 4096 - 12;
	Invocation unreadable(client.data(), 0x160008, nullptr, 0, &descriptor, 1, &method_result, 0);
	unreadable.Finish(0);
	CHECK(!(b->slots[OFFSETS[2] + 4].record.values[14] & PreviewReadable));
	Invocation invalid(client.data(), 0x160008, reinterpret_cast<void*>(1), 1,
	                   reinterpret_cast<void*>(1), 1, reinterpret_cast<int*>(1), 0);
	invalid.Finish(0);
	CHECK(!(b->slots[OFFSETS[2] + 6].record.values[14] & (DescriptorReadable | ResultReadable | PreviewReadable)));
	CHECK(VirtualFree(pages, 0, MEM_RELEASE));
	// Raw's descriptor is observer-local; an unreadable real size pointer must
	// not be reported as a successful zero written-size snapshot.
	descriptor.data = status.data();
	uint64_t written = 77;
	Invocation raw(client.data(), 0x160008, nullptr, 0, &descriptor, 1, &method_result, 0, true, &written);
	written = 24; raw.Finish(0);
	CHECK(b->slots[OFFSETS[2] + 9].record.values[11] == 77);
	CHECK(b->slots[OFFSETS[2] + 10].record.values[11] == 24);
	CHECK(b->slots[OFFSETS[2] + 10].record.values[14] & RawWrittenReadable);
	Invocation raw_invalid(client.data(), 0x160008, nullptr, 0, &descriptor, 1, &method_result, 0, true,
	                       reinterpret_cast<uint64_t*>(1));
	raw_invalid.Finish(0);
	CHECK(!(b->slots[OFFSETS[2] + 12].record.values[14] & RawWrittenReadable));
	// Observe fixed native failure call sites without consuming printf arguments.
	const char format[] = "sceNpManagerVshInit failed: 0x%x\n";
	const std::array<uint64_t, 6> arguments {reinterpret_cast<uint64_t>(format), 0x81234567, 3, 4, 5, 6};
	const auto saved_arguments = arguments;
	PsnFailure(0, arguments);
	CHECK(b->attempted[4] == 0);
	PsnFailure(0x910c4e192ull, arguments);
	CHECK(arguments == saved_arguments && b->attempted[4] == 2);
	const auto& first_format = b->slots[OFFSETS[4]].record;
	const auto& last_format = b->slots[OFFSETS[4] + 1].record;
	CHECK(first_format.values[2] == 0x81234567 && first_format.values[7] == 0);
	CHECK(first_format.values[14] & PreviewReadable);
	CHECK(std::memcmp(first_format.preview.data(), format, 24) == 0);
	CHECK(last_format.values[7] == 24 && last_format.values[15] == 1);
	CHECK(std::memcmp(last_format.preview.data(), format + 24, sizeof(format) - 24) == 0);
	PsnFailure(0x910c4db75ull, {1, 0, 0, 0, 0, 0});
	CHECK(!(b->slots[OFFSETS[4] + 2].record.values[14] & PreviewReadable));
	// Only the AppCache rejecting query is captured, without modifying the object.
	constexpr uint64_t caller = 0x910ca27c6ull;
	std::array<uint8_t, 0x18> mutex {};
	NpMutexQuery(mutex.data(), 0, caller - 1);
	NpMutexQuery(mutex.data(), 1, caller + 1);
	CHECK(b->attempted[4] == 3);
	NpMutexQuery(mutex.data(), 0, caller);
	const auto& query = b->slots[OFFSETS[4] + 3].record;
	CHECK(query.event == 21 && query.values[0] == reinterpret_cast<uint64_t>(mutex.data()));
	CHECK(query.values[1] == caller && query.values[2] == 0 && query.values[3] == 0);
	CHECK(query.values[14] == MutexByteReadable);
	mutex[0x10] = 0xa5;
	const auto saved_mutex = mutex;
	NpMutexQuery(mutex.data(), 1, caller);
	const auto& initialized_query = b->slots[OFFSETS[4] + 4].record;
	CHECK(initialized_query.values[2] == 1 && initialized_query.values[3] == 0xa5);
	CHECK(initialized_query.values[14] == MutexByteReadable && mutex == saved_mutex);
	NpMutexQuery(reinterpret_cast<void*>(1), 1, caller);
	NpMutexQuery(reinterpret_cast<void*>(UINT64_MAX - 7), 0, caller);
	NpMutexQuery(nullptr, 0, caller);
	for (size_t i = 5; i < 8; ++i) {
		const auto& unavailable = b->slots[OFFSETS[4] + i].record;
		CHECK(unavailable.event == 21 && unavailable.values[3] == 0 && unavailable.values[14] == 0);
	}
	CHECK(b->slots[OFFSETS[4] + 5].record.values[2] == 1);
	// Corrected Create status ABI is tagged, preserving old capture interpretation.
	NpMemoryCreated(0x910c85562ull, 0xc0000, reinterpret_cast<const char*>(1), nullptr, 0,
	                static_cast<int32_t>(0x8055800au));
	const auto& create = b->slots[OFFSETS[4] + 8].record;
	CHECK(create.event == 19 && create.values[1] == 0xc0000 && create.values[2] == 1);
	CHECK(create.values[4] == 0 && create.values[5] == 0x8055800au && create.values[15] == 1);
	NpMemoryCreated(0x1234, 0x800, reinterpret_cast<const char*>(1), mutex.data(), 1, 0);
	CHECK(b->slots[OFFSETS[4] + 9].record.values[3] == reinterpret_cast<uint64_t>(mutex.data()));
	NpMemoryCallback(0x910cc607full, 0, 0x568, nullptr, nullptr, mutex.data(), 1);
	const auto& allocation = b->slots[OFFSETS[4] + 10].record;
	CHECK(allocation.event == 22 && allocation.values[0] == 0x910cc607full);
	CHECK(allocation.values[1] == 0 && allocation.values[2] == 0x568 && allocation.values[6] == 1);
	CHECK(allocation.values[5] == reinterpret_cast<uint64_t>(mutex.data()) && mutex == saved_mutex);
	NpMemoryCallback(0x4321, 2, 0, nullptr, nullptr, nullptr, UINT32_MAX);
	CHECK(b->slots[OFFSETS[4] + 11].record.values[6] == UINT32_MAX);
	for (size_t i = 0; i < 100; ++i) { NpHeapRecord({1, 4, 0x18, 0x1234, 0x60, 0, 0x5678}); }
	CHECK(b->attempted[4] == 28 && b->dropped[4] == 0);
	for (size_t i = 12; i < 28; ++i) {
		const auto& heap = b->slots[OFFSETS[4] + i].record;
		CHECK(heap.event == 23 && heap.values[4] == 0x60 && heap.values[6] == 0x5678);
	}
	// Reproduce startup's descriptor storm independently of the callback budget.
	for (size_t i = 0; i < 200; ++i) {
		NpHeapRecord({1, 2, 0x100, 0, 32, 0, 0x200});
		NpHeapRecord({1, 3, 0x100, 0, 36, 0, 0x220, 0x8055a004});
	}
	CHECK(b->attempted[4] == 44 && b->dropped[4] == 0);
	for (size_t i = 0; i < 100; ++i) {
		NpHeapRecord({0xa80018662ull, 4, 0x118, 0x1234, 0x50, 0, 0x5678});
	}
	CHECK(b->attempted[4] == 44 && b->dropped[4] == 0);
	for (size_t i = 0; i < 100; ++i) {
		NpHeapRecord({0xa80018662ull, 4, 0x118, 0x1234, 0x40, 0, 0x5678});
	}
	CHECK(b->attempted[4] == 52 && b->dropped[4] == 0);
	CHECK(b->slots[OFFSETS[4] + 51].record.values[0] == 0xa80018662ull &&
	      b->slots[OFFSETS[4] + 51].record.values[4] == 0x40);
	// Lifecycle and PsnFailure publication survive both high-volume sources.
	NpHeapRecord({1, 0, 0x100, 0x1234, 0x4000});
	NpHeapRecord({1, 2, 0x100, 0, 32, 0, 0x200});
	PsnFailure(0x910c4db75ull, {1, 0, 0, 0, 0, 0});
	CHECK(b->attempted[4] == 54 && b->slots[OFFSETS[4] + 53].record.event == 18);
	for (size_t i = 54; i < CAPACITIES[4]; ++i) { NpMutexQuery(mutex.data(), 0, caller); }
	const auto last_query = b->slots[OFFSETS[4] + CAPACITIES[4] - 1].record;
	NpMutexQuery(reinterpret_cast<void*>(1), 1, caller);
	CHECK(b->attempted[4] == CAPACITIES[4] + 1 && b->dropped[4] == 1);
	CHECK(std::memcmp(&last_query, &b->slots[OFFSETS[4] + CAPACITIES[4] - 1].record, sizeof(Record)) == 0);
	const auto failure_start = b->attempted[2].load();
	std::array<uint64_t, 96> stack {};
	for (size_t i = 0; i < stack.size(); ++i) { stack[i] = 0x12340000 + i; }
	const auto original_stack = stack;
	StackFailure(0x910123456, stack.data(), 0xabcdef);
	CHECK(b->attempted[2] == failure_start + 9 && stack == original_stack);
	const auto& failure = b->slots[OFFSETS[2] + failure_start].record;
	CHECK(failure.event == 24 && failure.values[0] == 0x910123456 && failure.values[3] == 0xabcdef);
	const auto& chunk = b->slots[OFFSETS[2] + failure_start + 8].record;
	CHECK(chunk.values[3] == 0xfff && chunk.values[15] == stack.back());
	StackFailure(2, reinterpret_cast<void*>(1), 3);
	CHECK(b->slots[OFFSETS[2] + failure_start + 10].record.values[3] == 0);
	StackFailure(2, stack.data(), 3);
	StackFailure(2, stack.data(), 3);
	StackFailure(2, stack.data(), 3);
	CHECK(b->attempted[2] == failure_start + 36);
	// Pool observation has its own225-record ceiling inside Recovery. Routine
	// churn cannot consume the reserved first-full snapshot/result allowance.
	const auto pool_start = b->attempted[2].load();
	const char pool_name[] = "NpSyndySyncLogicPool";
	constexpr uint64_t pool_caller = 0x123456789abcdef0ull; // Synthetic full-width return address.
	NetPoolCreated(pool_caller, pool_name, 0x4000, 0x1234, 17);
	const auto& pool_create = b->slots[OFFSETS[2] + pool_start].record;
	CHECK(pool_create.event == 27 && pool_create.values[0] == pool_caller);
	CHECK(pool_create.values[1] == 0 && pool_create.values[2] == 17 && pool_create.values[3] == 17);
	CHECK(pool_create.values[4] == 0x4000 && pool_create.values[5] == 0x1234 &&
	      pool_create.values[6] == reinterpret_cast<uint64_t>(pool_name));
	CHECK(pool_create.values[14] == NameReadable && std::strcmp(pool_create.name.data(), pool_name) == 0);
	NetPoolDestroyed(0x12345678, -4, static_cast<int32_t>(0x80410109u));
	const auto& pool_destroy = b->slots[OFFSETS[2] + pool_start + 1].record;
	CHECK(pool_destroy.event == 27 && pool_destroy.values[0] == 0x12345678 && pool_destroy.values[1] == 1);
	CHECK(pool_destroy.values[2] == 0xfffffffcu && pool_destroy.values[3] == 0x80410109u);
	CHECK(pool_destroy.name[0] == 0 && pool_destroy.values[14] == 0);
	NetPoolCreated(0xabc, reinterpret_cast<const char*>(1), -1, -2, -3);
	const auto& unavailable_pool = b->slots[OFFSETS[2] + pool_start + 2].record;
	CHECK(unavailable_pool.values[2] == UINT32_MAX && unavailable_pool.values[3] == 0xfffffffdu &&
	      unavailable_pool.values[4] == UINT32_MAX && unavailable_pool.values[5] == 0xfffffffeu);
	CHECK(unavailable_pool.values[14] == 0);
	const std::array<uint64_t, 4> config {1, 0x123456789abcdef0ull, 0x42, 0xfedcba9876543210ull};
	NetPoolConfigured(pool_caller, config.data(), 0);
	const auto& configured = b->slots[OFFSETS[2] + pool_start + 3].record;
	CHECK(configured.values[1] == 3 && configured.values[3] == 0 && configured.values[4] == 1);
	CHECK(configured.values[14] == DescriptorReadable && configured.values[6] == reinterpret_cast<uint64_t>(config.data()));
	CHECK(std::equal(config.begin(), config.end(), configured.values.begin() + 8));
	NetPoolConfigured(pool_caller, nullptr, static_cast<int32_t>(0x80410116u));
	const auto& invalid_config = b->slots[OFFSETS[2] + pool_start + 4].record;
	CHECK(invalid_config.values[3] == 0x80410116u && invalid_config.values[14] == 0);
	std::vector<std::thread> pool_writers;
	for (int i = 0; i < 4; ++i) {
		pool_writers.emplace_back([] {
			for (int j = 0; j < 500; ++j) {
				NetPoolCreated(0xdef, reinterpret_cast<const char*>(1), 0x8000, 0, 2);
				NetPoolDestroyed(0xfed, 2, 0);
			}
		});
	}
	for (auto& writer: pool_writers) { writer.join(); }
	CHECK(b->attempted[2] == pool_start + NetPoolRoutineLimit && b->dropped[2] == 0);
	std::array<NetPoolEntry, NetPoolOccupiedLimit> occupied {};
	for (size_t i = 0; i < occupied.size(); ++i) {
		occupied[i].id = static_cast<int32_t>(i + 1);
		occupied[i].size = static_cast<int32_t>(0x4000 + i);
		occupied[i].name.fill(static_cast<char>('A' + i % 26)); // All32 bytes, no NUL.
	}
	const auto saved_occupied = occupied;
	NetPoolFullSnapshot(occupied);
	// Another thread's failure between snapshot and wrapper return must not
	// consume the failing thread's reserved record or pair with its host_tid.
	std::thread competitor([] { NetPoolCreated(0xbad, "other", 1, 0, static_cast<int32_t>(0x80410117u)); });
	competitor.join();
	CHECK(b->attempted[2] == pool_start + NetPoolRecordLimit - 1);
	NetPoolCreated(pool_caller, pool_name, 0x4000, 0, static_cast<int32_t>(0x80410117u));
	CHECK(b->attempted[2] == pool_start + NetPoolRecordLimit && b->dropped[2] == 0);
	const auto& full = b->slots[OFFSETS[2] + pool_start + NetPoolRecordLimit - 1].record;
	CHECK(full.values[0] == pool_caller && full.values[1] == 0 && full.values[2] == UINT32_MAX &&
	      full.values[3] == 0x80410117u && full.values[4] == 0x4000 && full.values[5] == 0);
	CHECK(std::strcmp(full.name.data(), pool_name) == 0);
	for (size_t i = 0; i < occupied.size(); ++i) {
		const auto& entry = b->slots[OFFSETS[2] + pool_start + NetPoolRoutineLimit + i].record;
		CHECK(entry.event == 27 && entry.values[0] == 0 && entry.values[1] == 2);
		CHECK(entry.values[2] == i + 1 && entry.values[4] == 0x4000 + i && entry.name == saved_occupied[i].name);
		CHECK(entry.host_tid == full.host_tid && entry.values[14] == NameReadable);
		CHECK(occupied[i].id == saved_occupied[i].id && occupied[i].size == saved_occupied[i].size &&
		      occupied[i].name == saved_occupied[i].name);
	}
	NetPoolFullSnapshot(occupied); // Repeated failures/snapshots cannot expand the budget.
	NetPoolCreated(0xbad, reinterpret_cast<const char*>(1), 1, 0, static_cast<int32_t>(0x80410117u));
	NetPoolDestroyed(0xbad, 1, 0);
	CHECK(b->attempted[2] == pool_start + NetPoolRecordLimit && b->dropped[2] == 0);
	CHECK(CAPACITIES[2] - NetPoolRecordLimit == 287);
	// SSL has an independent97-record ceiling. This fixture's earlier Recovery
	// traffic plus both complete budgets and the following reply still fit512.
	const auto ssl_start = b->attempted[2].load();
	CHECK(ssl_start + SslRecordLimit + 2 <= CAPACITIES[2]);
	constexpr uint64_t ssl_caller = 0xfedcba9876543210ull;
	constexpr uint64_t ssl_size = 0x1234567800080000ull;
	constexpr int32_t ssl_full_error = static_cast<int32_t>(0x8095f008u);
	SslInitialized(ssl_caller, ssl_size, 0x2001f);
	const auto& ssl_init = b->slots[OFFSETS[2] + ssl_start].record;
	CHECK(ssl_init.event == 28 && ssl_init.values[0] == ssl_caller && ssl_init.values[1] == 0);
	CHECK(ssl_init.values[2] == 0x2001f && ssl_init.values[3] == 0x2001f && ssl_init.values[4] == ssl_size);
	CHECK(ssl_init.values[14] == 0 && ssl_init.name[0] == 0);
	SslTerminated(ssl_caller - 1, -4, static_cast<int32_t>(0x8095f006u));
	const auto& ssl_term = b->slots[OFFSETS[2] + ssl_start + 1].record;
	CHECK(ssl_term.event == 28 && ssl_term.values[0] == ssl_caller - 1 && ssl_term.values[1] == 1);
	CHECK(ssl_term.values[2] == 0xfffffffcu && ssl_term.values[3] == 0x8095f006u && ssl_term.values[4] == 0);
	SslTerminated(ssl_caller - 2, 0x20000, 0);
	const auto& ssl_term_ok = b->slots[OFFSETS[2] + ssl_start + 2].record;
	CHECK(ssl_term_ok.event == 28 && ssl_term_ok.values[0] == ssl_caller - 2 && ssl_term_ok.values[1] == 1);
	CHECK(ssl_term_ok.values[2] == 0x20000 && ssl_term_ok.values[3] == 0);
	SslInitialized(ssl_caller - 3, UINT64_MAX, ssl_full_error); // No snapshot: ordinary failure budget.
	const auto& ssl_failure = b->slots[OFFSETS[2] + ssl_start + 3].record;
	CHECK(ssl_failure.event == 28 && ssl_failure.values[0] == ssl_caller - 3 && ssl_failure.values[1] == 0);
	CHECK(ssl_failure.values[2] == UINT32_MAX && ssl_failure.values[3] == 0x8095f008u &&
	      ssl_failure.values[4] == UINT64_MAX);
	std::vector<std::thread> ssl_writers;
	for (int i = 0; i < 4; ++i) {
		ssl_writers.emplace_back([] {
			for (int j = 0; j < 500; ++j) {
				SslInitialized(0xabc, UINT64_MAX, 0x20003);
				SslTerminated(0xdef, 0x20003, 0);
			}
		});
	}
	for (auto& writer: ssl_writers) { writer.join(); }
	CHECK(b->attempted[2] == ssl_start + SslRoutineLimit && b->dropped[2] == 0);
	std::array<SslEntry, SslOccupiedLimit> ssl_occupied {};
	for (size_t i = 0; i < ssl_occupied.size(); ++i) {
		ssl_occupied[i].id = static_cast<int32_t>(0x20000 + i); // Full existing HLE SSL handle.
		ssl_occupied[i].size = UINT64_MAX - i;
	}
	const auto saved_ssl_occupied = ssl_occupied;
	SslFullSnapshot(ssl_occupied);
	std::thread ssl_competitor([&] {
		SslFullSnapshot(ssl_occupied); // Cannot steal the first snapshot or its TLS reservation.
		SslInitialized(0xbad, 0x80000, ssl_full_error);
	});
	ssl_competitor.join();
	SslTerminated(0xbad, 0x20000, 0); // Term must not consume this thread's failed-init reservation.
	CHECK(b->attempted[2] == ssl_start + SslRecordLimit - 1);
	SslInitialized(ssl_caller, ssl_size, ssl_full_error);
	CHECK(b->attempted[2] == ssl_start + SslRecordLimit && b->dropped[2] == 0);
	const auto& ssl_full = b->slots[OFFSETS[2] + ssl_start + SslRecordLimit - 1].record;
	CHECK(ssl_full.event == 28 && ssl_full.values[0] == ssl_caller && ssl_full.values[1] == 0);
	CHECK(ssl_full.values[2] == UINT32_MAX && ssl_full.values[3] == 0x8095f008u && ssl_full.values[4] == ssl_size);
	CHECK(ssl_full.host_tid == GetCurrentThreadId());
	for (size_t i = 0; i < ssl_occupied.size(); ++i) {
		const auto& entry = b->slots[OFFSETS[2] + ssl_start + SslRoutineLimit + i].record;
		CHECK(entry.event == 28 && entry.values[0] == 0 && entry.values[1] == 2 && entry.values[3] == 0);
		CHECK(entry.values[2] == 0x20000 + i && entry.values[4] == UINT64_MAX - i);
		CHECK(entry.host_tid == ssl_full.host_tid && entry.values[14] == 0 && entry.name[0] == 0);
		CHECK(ssl_occupied[i].id == saved_ssl_occupied[i].id && ssl_occupied[i].size == saved_ssl_occupied[i].size);
	}
	SslFullSnapshot(ssl_occupied);
	SslInitialized(0xbad, 1, ssl_full_error);
	SslTerminated(0xbad, -1, -7);
	NetPoolCreated(0xbad, "after-ssl", 0x4000, 0, static_cast<int32_t>(0x80410117u));
	CHECK(b->attempted[2] == ssl_start + SslRecordLimit && b->dropped[2] == 0);
	CHECK(SslRecordLimit == 97 && NetPoolRecordLimit + SslRecordLimit == 322);
	CHECK(CAPACITIES[2] - NetPoolRecordLimit - SslRecordLimit == 190);
	uint32_t pool_records = 0, ssl_records = 0;
	for (size_t i = pool_start; i < ssl_start + SslRecordLimit; ++i) {
		const auto& slot = b->slots[OFFSETS[2] + i];
		CHECK(slot.commit.load(std::memory_order_acquire) == OFFSETS[2] + i + 1);
		pool_records += slot.record.event == 27;
		ssl_records += slot.record.event == 28;
	}
	CHECK(pool_records == NetPoolRecordLimit && ssl_records == SslRecordLimit);
	const auto reply_start = b->attempted[2].load();
	Invocation recovery_after_pools_ssl(client.data(), 0x160008, nullptr, 0, &descriptor, 1, &method_result, 0xcafe);
	recovery_after_pools_ssl.Finish(0);
	CHECK(b->attempted[2] == reply_start + 2 && b->dropped[2] == 0);
	CHECK(b->slots[OFFSETS[2] + reply_start].record.event == static_cast<uint64_t>(Event::InvokeEnter));
	CHECK(b->slots[OFFSETS[2] + reply_start + 1].record.event == static_cast<uint64_t>(Event::InvokeExit));
	// Independent file view reads published records without Log/stdout/flush.
	std::ifstream input(std::filesystem::path(OutputPath()), std::ios::binary);
	CHECK(input.good());
	auto bytes = std::make_unique<std::array<char, sizeof(Buffer)>>();
	input.read(bytes->data(), bytes->size()); CHECK(input.gcount() == sizeof(Buffer));
	uint64_t magic = 0, commit = 0;
	std::memcpy(&magic, bytes->data(), sizeof(magic)); CHECK(magic == MAGIC);
	const size_t position = offsetof(Buffer, slots) + OFFSETS[2] * sizeof(Slot);
	std::memcpy(&commit, bytes->data() + position, sizeof(commit)); CHECK(commit == OFFSETS[2] + 1);
	Record persisted; std::memcpy(&persisted, bytes->data() + position + sizeof(uint64_t), sizeof(persisted));
	CHECK(persisted.call == before.call && persisted.values == before.values);
	std::wprintf(L"mapped capture: %ls\n", OutputPath());
}

static void PersistenceAfterExit() {
	wchar_t executable[32768] {};
	CHECK(GetModuleFileNameW(nullptr, executable, 32768) != 0);
	std::wstring command = L"\"" + std::wstring(executable) + L"\" --persistence-child";
	STARTUPINFOW startup {}; startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};
	CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
	CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
	DWORD code = 1; CHECK(GetExitCodeProcess(process.hProcess, &code) && code == 0);
	const auto prefix = "ipmi-observe-" + std::to_string(process.dwProcessId) + "-";
	CloseHandle(process.hThread); CloseHandle(process.hProcess);
	bool found = false;
	for (const auto& file: std::filesystem::directory_iterator(std::filesystem::path(OutputPath()).parent_path())) {
		if (file.path().filename().string().find(prefix) != 0) { continue; }
		std::ifstream input(file.path(), std::ios::binary);
		input.seekg(offsetof(Buffer, slots));
		uint64_t commit = 0; Record record;
		input.read(reinterpret_cast<char*>(&commit), sizeof(commit));
		input.read(reinterpret_cast<char*>(&record), sizeof(record));
		if (commit == 1 && record.event == static_cast<uint64_t>(Event::DispatcherExit) &&
		    record.values[0] == 0xdeadbeef) { found = true; }
	}
	CHECK(found);
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
	if (argc == 2 && std::strcmp(argv[1], "--persistence-child") == 0) {
		Initialize(); CHECK(Enabled());
		Emit(Bank::Lifecycle, Event::DispatcherExit, {0xdeadbeef});
		return 0; // No explicit mapping flush/unmap; verify from parent after exit.
	}
#endif
	PublicationAndBudgets();
#ifdef _WIN32
	MappedSnapshots();
	PersistenceAfterExit();
#endif
	std::puts("IPMI observation tests passed");
}
