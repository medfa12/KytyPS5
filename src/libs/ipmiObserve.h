#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <intrin.h>
#define KYTY_IPMI_CALLER reinterpret_cast<uint64_t>(_ReturnAddress())
#else
#define KYTY_IPMI_CALLER reinterpret_cast<uint64_t>(__builtin_return_address(0))
#endif

namespace Libs::IpmiObserve {

enum class Bank : uint64_t { Lifecycle, Invoke, Recovery, Poll, Gateway, Count };
enum class Event : uint64_t {
	ClientCreate = 1, ServerCreate, ConnectEnter, ConnectExit, Disconnect, Destroy,
	InvokeEnter, InvokeExit, RawEnter, RawExit, Input, DispatcherEnter, DispatcherExit,
	PollEnter, PollExit, GatewayEnter, GatewayExit,
	PsnFailure, NpMemoryResult, NpAllocatorResult, NpMutexQuery = 21,
	NpMemoryCallback = 22,
	NpHeap = 23,
	StackFailure = 24,
	AppIdResult = 25,
	NpGlobalResult = 26,
	NetPool = 27,
	SslContext = 28,
};
enum Flag : uint64_t {
	NameReadable = 1, VtableReadable = 2, DescriptorReadable = 4, ResultReadable = 8,
	PreviewReadable = 16, PreviewUnchanged = 32, RecoveryTarget = 64,
	RawWrittenReadable = 128,
	MutexByteReadable = 256,
};
using Values = std::array<uint64_t, 16>;
constexpr std::array<uint64_t, 5> CAPACITIES {256, 256, 512, 64, 128};
constexpr std::array<uint64_t, 5> OFFSETS {0, 256, 512, 1024, 1088};
constexpr size_t CAPACITY = 1216;
constexpr uint64_t MAGIC = 0x3142534f494d5049; // "IPMIOSB1"

struct Record {
	uint64_t event = 0, tick = 0, call = 0;
	Values values {};
	std::array<char, 32> name {};
	std::array<uint8_t, 24> preview {};
	uint64_t host_tid = 0;
	std::array<uint64_t, 4> reserved {};
};
struct alignas(64) Slot {
	std::atomic<uint64_t> commit {0};
	Record record {};
};
struct alignas(64) Buffer {
	uint64_t magic = MAGIC, version = 1, pid = 0, frequency = 0, start_tick = 0;
	std::atomic<uint64_t> calls {0}, ready {0};
	std::array<uint64_t, 5> capacities = CAPACITIES;
	std::array<std::atomic<uint64_t>, 5> attempted {}, dropped {};
	std::array<uint64_t, 10> reserved {};
	std::array<Slot, CAPACITY> slots {};
	void Append(Bank bank, const Record& record) noexcept {
		const auto kind = static_cast<size_t>(bank);
		if (kind >= CAPACITIES.size()) { return; }
		const auto index = attempted[kind].fetch_add(1, std::memory_order_relaxed);
		if (index >= CAPACITIES[kind]) {
			dropped[kind].fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const auto slot = OFFSETS[kind] + index;
		slots[slot].record = record;
		slots[slot].commit.store(slot + 1, std::memory_order_release);
	}
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(sizeof(Record) == 248 && sizeof(Slot) == 256);
static_assert(offsetof(Buffer, slots) == 256 && sizeof(Buffer) == 311552);

// Initialize only during host library registration, before guest execution.
// Mapping and its handles intentionally live for the process lifetime.
void Initialize() noexcept;
bool Enabled() noexcept;
Buffer* GetBuffer() noexcept;
// Path available for host-side tests/decoding; never used by producer hooks.
const wchar_t* OutputPath() noexcept;
bool Read(const void* source, void* destination, size_t size) noexcept;
void Emit(Bank bank, Event event, const Values& values = {}, const char* name = nullptr) noexcept;
void ClientCreated(void* client, const void* config, void* storage, uint64_t caller) noexcept;
void ServerCreated(void* server, const void* config, void* arg2, void* storage, uint64_t caller) noexcept;
// Fixed 13.60 ShellCore call sites; preserves the printf argument context.
void PsnFailure(uint64_t caller, const std::array<uint64_t, 6>& arguments) noexcept;
void NpMutexQuery(const void* mutex, int result, uint64_t caller) noexcept;
void NpMemoryCreated(uint64_t caller, size_t size, const char* name, const void* buffer,
                     uint32_t mode, int32_t status) noexcept;
void NpMemoryCallback(uint64_t caller, uint32_t operation, size_t size, const void* pointer,
                       const void* context, const void* result, uint64_t count) noexcept;
// Event23: 16 callbacks, 8 ordinary/8 Ex descriptors, 8 fixed native Ex-new raw0x40 callbacks.
void NpHeapRecord(const Values& values) noexcept;
// Read-only snapshot at stack-check entry; first four calls, independent recovery bank.
void StackFailure(uint64_t caller, const void* frame, uint64_t guard) noexcept;

// Event27 / Recovery: values={caller, operation(0=create,1=destroy,2=occupied,3=config),
// id32, result32, size32, flags32, name_pointer}; values14 carries NameReadable.
// Occupied has caller/result/flags/name_pointer=0, and copies trusted host bytes
// without guest reads. host_tid links its one-time snapshot to the failing call.
// Independent reservations keep the first ENFILE and up to128 entries available
// after routine churn: at most96 + 1 + 128 =225 Recovery records (287 left).
constexpr uint32_t NetPoolRoutineLimit = 96, NetPoolOccupiedLimit = 128;
constexpr uint32_t NetPoolRecordLimit = NetPoolRoutineLimit + 1 + NetPoolOccupiedLimit;
struct NetPoolEntry {
	int32_t id = 0, size = 0;
	std::array<char, 32> name {}; // Bounded bytes; may have no terminating NUL.
};
void NetPoolCreated(uint64_t caller, const char* name, int32_t size, int32_t flags, int32_t result) noexcept;
void NetPoolDestroyed(uint64_t caller, int32_t id, int32_t result) noexcept;
// Configuration is an observation of caller bytes after the setter, not an
// atomic snapshot of effective state. values8..11 retain all32 observed bytes.
void NetPoolConfigured(uint64_t caller, const void* config, int32_t result) noexcept;
// Called after releasing the pool mutex, on the failing create's host thread.
// Reserves its following NetPoolCreated record via TLS, so a concurrent failing
// wrapper cannot steal the first-full result reservation. At most one snapshot.
void NetPoolFullSnapshot(const std::array<NetPoolEntry, NetPoolOccupiedLimit>& entries) noexcept;

// Event28 / Recovery: values={caller, operation(0=init,1=term,2=occupied),
// id32, result32, size64}. Failed init has id32=UINT32_MAX. Occupied has
// caller/result=0; host_tid pairs the snapshot with its failed init record.
// 64 routine + 32 occupied + 1 reserved failed init = 97 records maximum.
// Together with NetPool's225, at most322 slots, leaving190 before other traffic.
constexpr uint32_t SslRoutineLimit = 64, SslOccupiedLimit = 32;
constexpr uint32_t SslRecordLimit = SslRoutineLimit + SslOccupiedLimit + 1;
struct SslEntry {
	int32_t id = 0; // Full public Id::Create(slot, Ssl).ToInt(), not a raw slot.
	uint64_t size = 0;
};
void SslInitialized(uint64_t caller, uint64_t size, int32_t result) noexcept;
void SslTerminated(uint64_t caller, int32_t id, int32_t result) noexcept;
// Called once after releasing the Network mutex, on the failing init's thread.
// TLS reserves that thread's following SslInitialized record independently of
// routine churn and concurrent failures. Trusted fixed host bytes only.
void SslFullSnapshot(const std::array<SslEntry, SslOccupiedLimit>& entries) noexcept;

// Descriptor layouts mirror the existing HLE, not a newly inferred wire ABI.
struct Input { const void* data; uint64_t size; };
struct Output { void* data; uint64_t capacity; uint64_t written; };
static_assert(sizeof(Input) == 16 && sizeof(Output) == 24);

class Invocation {
public:
	Invocation(void* client, uint32_t method, const void* inputs, uint32_t input_count,
	           void* outputs, uint32_t output_count, int* result, uint64_t caller,
	           bool raw = false, const uint64_t* raw_written = nullptr) noexcept;
	void Finish(int transport) noexcept;
private:
	bool active = false, raw = false;
	Bank bank = Bank::Invoke;
	Record entry {};
	void Snapshot(Record& record) noexcept;
};
} // namespace Libs::IpmiObserve
