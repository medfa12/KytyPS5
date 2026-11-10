#include "loader/nativeLibcStartup.h"

#include <algorithm>
#include <array>
#include <utility>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

namespace Loader::NativeLibcStartup {
namespace {
// Digest of the selected 13.60 leaf. This is a profile check, not a
// replacement for the loader's separate whole-provider identity verification.
constexpr std::array<uint8_t, 32> LeafDigest {
	0xe2, 0x0e, 0x49, 0x3d, 0x3c, 0xcc, 0x11, 0x0e, 0xbb, 0xf9, 0x3d, 0x27, 0xe8, 0x29, 0x66, 0x03,
	0xeb, 0x65, 0xbf, 0x48, 0x18, 0xcf, 0x29, 0x74, 0xcc, 0x4b, 0xad, 0x68, 0x57, 0x86, 0xbc, 0x8f,
};
} // namespace

bool MatchesLeafDigest(std::span<const uint8_t> readable, size_t offset,
                       std::span<const uint8_t, 32> expected) noexcept {
	if (readable.data() == nullptr || offset > readable.size() || LeafSize > readable.size() - offset) { return false; }
	auto* sha = av_sha_alloc();
	if (!sha) { return false; }
	const bool initialized = av_sha_init(sha, 256) == 0;
	std::array<uint8_t, 32> digest {};
	if (initialized) { av_sha_update(sha, readable.data() + offset, LeafSize); av_sha_final(sha, digest.data()); }
	av_free(sha);
	return initialized && std::equal(digest.begin(), digest.end(), expected.begin());
}

bool MatchesLeafProfile(std::span<const uint8_t> readable, size_t offset) noexcept {
	return MatchesLeafDigest(readable, offset, LeafDigest);
}

Result State::Bind(int32_t owner, Entry entry, bool profile_verified) noexcept {
	if (!m_enabled) { return Result::Disabled; }
	if (m_phase == Phase::Invoking) { return Result::Invoking; }
	if (m_phase != Phase::Fresh) {
		if (owner != m_owner || (m_entry != nullptr && entry != m_entry)) { return Result::ReplacementRejected; }
		if (m_phase == Phase::Invalidated) { return Result::Invalidated; }
		return m_completed ? Result::AlreadyCompleted : Result::AlreadyBound;
	}
	if (m_startup_admitted) { return Result::StartupAlreadyAdmitted; }
	if (owner < 0) { return Result::InvalidOwner; }
	if (entry == nullptr) { return Result::NullEndpoint; }
	if (!profile_verified) { return Result::UnverifiedProfile; }
	m_owner = owner;
	m_entry = entry;
	m_phase = Phase::Bound;
	return Result::Ok;
}

Result State::Prepare(PreparedCall& call) noexcept {
	if (!m_enabled) { return Result::Disabled; }
	if (m_phase == Phase::Invoking) { return Result::Invoking; }
	if (m_phase == Phase::Invalidated) { return Result::Invalidated; }
	if (m_completed) { return Result::AlreadyCompleted; }
	if (m_startup_admitted) { return Result::StartupAlreadyAdmitted; }
	if (m_phase != Phase::Bound) { return Result::NotBound; }
	if (call.m_state != nullptr) { return Result::InvalidInvocation; }
	call.m_state = this;
	call.m_owner = m_owner;
	call.m_entry = m_entry;
	m_call = &call;
	m_phase = Phase::Invoking; // Admission closes before releasing the external lock.
	return Result::Ok;
}

Result PreparedCall::Invoke() {
	if (m_state == nullptr) { return Result::InvalidInvocation; }
	if (m_invoked) { return Result::AlreadyInvoked; }
	if (m_entry == nullptr) { return Result::InvalidInvocation; }
	m_invoked = true;
	const auto entry = std::exchange(m_entry, nullptr);
	entry(); // Actual selected native entry in production. No interpretation of RAX.
	m_returned = true;
	return Result::Ok;
}

Result State::Complete(PreparedCall& call) noexcept {
	if (!m_enabled) { return Result::Disabled; }
	if (call.m_state != this || call.m_owner != m_owner) { return Result::InvalidInvocation; }
	if (call.m_completed) { return Result::AlreadyCompleted; }
	if (m_phase != Phase::Invoking || m_call != &call) { return Result::InvalidInvocation; }
	if (!call.m_returned) { return Result::NotReturned; }
	call.m_completed = true;
	m_call = nullptr;
	m_completed = true;
	m_phase = Phase::Completed;
	return Result::Ok;
}

Result State::RecordStartupAdmission() noexcept {
	if (m_phase == Phase::Invoking) { return Result::Invoking; }
	m_startup_admitted = true;
	return m_enabled ? Result::Ok : Result::Disabled;
}

Result State::InvalidateProvider(int32_t owner) noexcept {
	if (!m_enabled) { return Result::Disabled; }
	if (m_owner < 0 || owner != m_owner) { return Result::UnrelatedProvider; }
	if (m_phase == Phase::Invoking) { return Result::Invoking; }
	m_entry = nullptr; // Clear before caller may unmap, retaining owner and both history bits.
	m_phase = Phase::Invalidated;
	return Result::Ok;
}

} // namespace Loader::NativeLibcStartup
