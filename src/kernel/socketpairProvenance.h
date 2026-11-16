#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <intrin.h>
#define KYTY_SPP_CALLER reinterpret_cast<uint64_t>(_ReturnAddress())
#else
#define KYTY_SPP_CALLER reinterpret_cast<uint64_t>(__builtin_return_address(0))
#endif

namespace Libs::LibKernel::FileSystem {
struct File;
namespace SocketpairProvenance {

// Temporary, independent observation buffer. No authorization/ownership policy.
enum class Event : uint64_t { Create = 1, Publish, Close, Delete, ReadDispatch, Socketpair };
enum class Bank : size_t { Lifecycle, Socketpair, NotificationRead, OtherRead };
constexpr std::array<uint64_t, 4> Capacities {32, 8, 8, 8};
constexpr std::array<uint64_t, 4> Offsets {0, 32, 40, 48};
constexpr size_t Capacity = 56;
constexpr uint64_t Magic = 0x0031564f52505053ull; // "SPPROV1\0"
constexpr uint64_t NoSlot = UINT64_MAX;

struct Record {
	uint64_t sequence = 0, event = 0, caller = 0, host_tid = 0, fs_context = 0;
	uint64_t fd = UINT64_MAX, object = 0, generation = 0, kind = 0;
	std::array<uint64_t, 6> detail {};
};
struct alignas(64) Slot {
	std::atomic<uint64_t> commit {0};
	Record record {};
};
struct alignas(64) Buffer {
	uint64_t magic = Magic, version = 1, pid = 0, bytes = 7424;
	std::atomic<uint64_t> ready {0}, sequence {0}, generations {0};
	uint64_t reserved0 = 0;
	std::array<uint64_t, 4> capacities = Capacities;
	std::array<std::atomic<uint64_t>, 4> reserved {}, dropped {};
	std::array<uint64_t, 12> padding {};
	std::array<Slot, Capacity> slots {};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic_bool::is_always_lock_free);
static_assert(sizeof(Record) == 120 && sizeof(Slot) == 128);
static_assert(offsetof(Buffer, slots) == 256 && sizeof(Buffer) == 7424);

// Called only by FileSystem::Initialize, before guest execution. Windows only.
void Initialize() noexcept;
bool Enabled() noexcept;
void Created(File* file) noexcept;
void Observe(Event event, int fd, File* file, uint64_t caller, uint64_t count = 0) noexcept;

// Forward the POSIX wrapper's raw guest RA without changing guest ABI signatures.
// TLS scope restores nested callers; it does not establish guest authority.
class ReadCallerScope {
public:
	explicit ReadCallerScope(uint64_t caller) noexcept;
	~ReadCallerScope();
	ReadCallerScope(const ReadCallerScope&) = delete;
	ReadCallerScope& operator=(const ReadCallerScope&) = delete;
private:
	uint64_t previous = 0;
	bool active = false;
};
uint64_t ReadCaller(uint64_t fallback) noexcept;

// Reservation precedes both safe copies. At most eight calls perform RPM.
// Finish observes exactly the result of the one forwarded KStub invocation.
class SocketpairCall {
public:
	SocketpairCall(uint64_t caller, uint64_t domain, uint64_t type,
	               uint64_t protocol, uint64_t output) noexcept;
	void Finish(uint64_t result) noexcept;
	SocketpairCall(const SocketpairCall&) = delete;
	SocketpairCall& operator=(const SocketpairCall&) = delete;
private:
	uint64_t slot = NoSlot;
	Record record {};
};
} // namespace SocketpairProvenance
} // namespace Libs::LibKernel::FileSystem
