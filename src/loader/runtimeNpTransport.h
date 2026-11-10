#pragma once

#include <cstdint>

namespace Loader {
struct Program;
bool InstallSourceBoundLoginMgrTransport(Program* core);
bool RetireSourceBoundLoginMgrTransport();
struct SourceBoundNpTransportEpoch {
	uint64_t generation = 0;
	int owner_pid = -1;
	bool closing = false;
	bool live = false;
};
// Host-only BSD errno APIs. Inspection preserves output when no epoch exists.
// A generation is published only after successful source-bound Install and is
// never reused. Standalone NpNativeTransport::Install does not create an epoch.
// Source-bound installations must use these Install/Retire lifecycle wrappers;
// direct native Uninstall is not an alternate epoch-management API.
int InspectSourceBoundNpTransportEpoch(SourceBoundNpTransportEpoch& output) noexcept;
// Irreversible for this installed generation. Requires the ACTUAL admitted Core
// owner pthread/current program/PID, even on an idempotent close. Closing only
// seals manager-client Start admission; it does not revoke owner liveness or
// cancel native work. A stale generation cannot close a replacement transport.
int CloseSourceBoundNpTransportEpoch(uint64_t expected_generation) noexcept;
// Explicit integration seam. Requires the retained, exact-image Core credential
// admission and a published cohost identity; never installs a replacement service.
bool InstallSourceBoundNpTransport(Program* core);
// Native clients/dispatcher must already have drained, stopped and joined.
// Busy preserves the transport owner and prevents mapping release.
// The epoch is cleared only after successful Uninstall, even if later allocator
// retirement needs retry. This lifecycle seam serializes with client admission.
bool RetireSourceBoundNpTransport();
}
