#include "kernel/npDrmDevice.h"

#include "common/threads.h"

#include <algorithm>

namespace Libs::LibKernel::NpDrm {
namespace {

void WriteU32(std::span<uint8_t> buffer, size_t offset, uint32_t value) {
	for (size_t i = 0; i < 4; ++i) {
		buffer[offset + i] = static_cast<uint8_t>(value >> (i * 8));
	}
}

uint32_t ReadU32(std::span<const uint8_t> buffer, size_t offset) {
	uint32_t value = 0;
	for (size_t i = 0; i < 4; ++i) { value |= uint32_t {buffer[offset + i]} << (8 * i); }
	return value;
}

// SHA256(6d99f4bf8d1e6d9b3f9578821dad6fb4 || 32 zero digest bytes)[0:16].
// Every supported transition of this unavailable-SM core preserves digest=0.
// Adding a digest-changing path requires replacing this state-derived answer.
constexpr std::array<uint8_t, 16> UnavailableStateHash {
	0x26, 0x1c, 0x4b, 0xbf, 0x74, 0x82, 0xc9, 0xf5,
	0x63, 0xf4, 0x42, 0x72, 0xe5, 0x43, 0xf6, 0x55,
};

bool CanOpen(Authority authority) {
	return authority == DriverAuthority || authority == ShellUiAuthority;
}

} // namespace

int Device::Open(Authority authority) const { return CanOpen(authority) ? 0 : 1; }
int Device::Close(Authority authority) const { return CanOpen(authority) ? 0 : 1; }

Result Device::Ioctl(Authority authority, uint32_t request, std::span<uint8_t> buffer) {
	if (authority != DriverAuthority && !(authority == ShellUiAuthority && request == ShellUiRequest)) {
		return {ResultKind::NativeError, 1};
	}
	if (request == AuditedUnknownRequest) {
		return {ResultKind::NativeError, 25};
	}
	size_t required = 0;
	switch (request) {
		case Initialize:
		case Terminate: required = 4; break;
		case QueryState: required = 12; break;
		case QueryStateHash: required = 20; break;
		case ShellUiRequest: required = 52; break;
		default: return {ResultKind::UnsupportedRequest, std::nullopt};
	}
	if (buffer.size() < required) {
		return {ResultKind::InvalidBuffer, std::nullopt};
	}
	if (request == ShellUiRequest && ReadU32(buffer, 8) != 3) {
		return {ResultKind::UnsupportedRequest, std::nullopt};
	}

	// Protect both lock acquisition and ownership against guest-thread suspension.
	// Persistence stays in this transaction; no filesystem lock may be held on entry.
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(m_mutex);
	switch (request) {
		case ShellUiRequest: {
			// Kernel 13.60: b802e9 calls srtc before any NPDRM init check;
			// b80749 writes its negative status at +12 without touching the tick.
			// srtc 70dcad returns 800f0513 when no device exists. Production has
			// no srtc provider; do not infer its state from NPDRM activation.
			uint64_t seconds = 0;
			const int32_t status = m_clock ? m_clock(&seconds) : static_cast<int32_t>(SecureClockUnavailable);
			if (status >= 0) {
				// b804df: unsigned arithmetic deliberately preserves native wrap.
				const uint64_t tick = (seconds + UINT64_C(63460972800) +
				                       static_cast<uint64_t>(m_state.time_adjust)) * UINT64_C(1000000);
				WriteU32(buffer, 0, static_cast<uint32_t>(tick));
				WriteU32(buffer, 4, static_cast<uint32_t>(tick >> 32));
			}
			WriteU32(buffer, 12, status < 0 ? static_cast<uint32_t>(status) : 0);
			break;
		}
		case Initialize:
			m_state.length = 0;
			m_state.attempted = true;
			m_state.error = SecureModuleUnavailable;
			if (m_state.error_class == 0) { m_state.error_class = 0x10000; }
			m_state.activation.fill(0);
			// A zero-length persistence attempt still creates/truncates act.dat.
			(void)m_persistence.WriteActivation(std::span<const uint8_t>(m_state.activation).first(m_state.length));
			WriteU32(buffer, 0, 0);
			break;
		case Terminate:
			WriteU32(buffer, 0, 0);
			break;
		case QueryState:
			if (m_state.attempted) {
				WriteU32(buffer, 0, m_state.error_class);
				WriteU32(buffer, 4, m_state.error);
			}
			WriteU32(buffer, 8, m_state.attempted ? 0 : NotInitialized);
			break;
		case QueryStateHash:
			if (m_state.attempted) {
				std::copy(UnavailableStateHash.begin(), UnavailableStateHash.end(), buffer.begin());
			}
			WriteU32(buffer, 0x10, m_state.attempted ? 0 : NotInitialized);
			break;
		default: return {ResultKind::UnsupportedRequest, std::nullopt};
	}
	return {ResultKind::Completed, 0};
}

State Device::Snapshot() const {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(m_mutex);
	return m_state;
}

} // namespace Libs::LibKernel::NpDrm
