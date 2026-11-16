#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace Libs::LibKernel::NpDrm {

inline constexpr uint64_t DriverAuthority = UINT64_C(0x4800000000000010);
inline constexpr uint64_t ShellUiAuthority = UINT64_C(0x480000000000000f);
using Authority = std::optional<uint64_t>; // Absent retained input metadata is unknown.

inline constexpr uint32_t Initialize = 0xc0044e05;
inline constexpr uint32_t Terminate = 0xc0044e06;
inline constexpr uint32_t QueryState = 0xc00c4e0f;
inline constexpr uint32_t QueryStateHash = 0xc0144e10;
inline constexpr uint32_t ShellUiRequest = 0xc0344e18; // Subcommand 3: debug network tick.
inline constexpr uint32_t AuditedUnknownRequest = 0xc0044eff;
inline constexpr uint32_t NotInitialized = 0x800f0a32;
inline constexpr uint32_t SecureModuleUnavailable = 0x800f0a0e;
inline constexpr uint32_t SecureClockUnavailable = 0x800f0513;

// Host secure-clock provider: seconds since 2012, or a native negative status.
// Null explicitly models an absent srtc device, independently of NPDRM's SM.
// Called under the device lock; must not throw, reenter, or invoke guest code.
using SecureClockRead = int32_t (*)(uint64_t* seconds);

struct State {
	bool attempted = false;
	uint32_t error_class = 0;
	uint32_t error = 0;
	uint32_t length = 0;
	std::array<uint8_t, 32> digest {};
	int64_t time_adjust = 0;
	int32_t sm_handle = -1;
	std::array<uint8_t, 16 * 1024> activation {};
	bool operator==(const State&) const = default;
};

// A required host-only persistence callback. The adapter implements real guest
// filesystem operations; the core neither opens paths nor owns guest storage.
// Must not throw, invoke guest code, or reenter this Device (including Snapshot).
class Persistence {
public:
	virtual ~Persistence() = default;
	virtual int WriteActivation(std::span<const uint8_t> bytes) noexcept = 0;
};

enum class ResultKind {
	Completed,
	NativeError,
	UnsupportedRequest, // Deliberately has no invented native errno/status.
	InvalidBuffer,      // Adapter supplied an undersized local copy; not a guest errno.
};

struct Result {
	ResultKind kind;
	std::optional<int> transport_errno; // Raw BSD errno: 0, EPERM=1, ENOTTY=25.
};

class Device final {
public:
	explicit Device(Persistence& persistence, SecureClockRead clock = nullptr):
	    m_persistence(persistence), m_clock(clock) {}
	Device(const Device&) = delete;
	Device& operator=(const Device&) = delete;

	// Open/close authorize only; neither resets nor owns global driver state.
	[[nodiscard]] int Open(Authority authority) const;
	[[nodiscard]] int Close(Authority authority) const;
	// The span is already copied into host-local storage. No fd or guest-pointer
	// access occurs here. Only Completed permits the adapter to copy the buffer out.
	[[nodiscard]] Result Ioctl(Authority authority, uint32_t request, std::span<uint8_t> buffer);
	[[nodiscard]] State Snapshot() const;

private:
	Persistence& m_persistence;
	SecureClockRead m_clock;
	mutable std::mutex m_mutex;
	State m_state;
};

} // namespace Libs::LibKernel::NpDrm
