#pragma once

#include "kernel/ipmi.h"
#include "kernel/ipmiWire.h"

namespace Libs::LibKernel::IpmiGateway {

constexpr uint64_t NativeDefaultTag = UINT64_C(0xdeadbadecafebeaf);

// Bounded host collector fields, not a packed native ring record. Names are
// fixed byte arrays (not necessarily NUL-terminated). The sink owns timestamps.
struct DiagnosticCaller {
	Ipmi::Identity identity {};
	std::array<char, 32> process_name {}, thread_name {};
};
struct DiagnosticEvent {
	uint32_t selector_input = 0; // Full selector, including for unknown observation.
	uint16_t selector = 0, handle = 0xffff;
	DiagnosticCaller caller;
	std::array<char, 25> server_name {};
	uint32_t a = 0, b = 0;
	uint64_t p = UINT64_MAX;
};

// Isolated adapter, NOT registered with libSysProc or any production provider.
// Copy callbacks return guest errno (zero on success) and never throw. A failed
// copy may have written a prefix; the adapter does not roll back guest bytes.
// Addresses are opaque, including native objects.
// Callbacks may reenter on another guest identity; no Core lock spans a callback.
struct Callbacks {
	std::function<int(uint64_t, void*, size_t)> copy_in;
	std::function<int(uint64_t, const void*, size_t)> copy_out;
	std::function<Ipmi::Identity()> identity;
	std::function<void(int)> set_errno;
	// Yield/pump guest work; return zero to continue, guest errno to interrupt.
	// Completion wins if the pump both completes a ticket and reports a signal.
	std::function<int()> cooperate;
	std::function<bool(Ipmi::Identity)> has_capability62;
	// Client registration authorization is unverified. Missing policy excludes
	// registration; this host-test opt-in does NOT apply the server's Sce rule.
	std::function<int(Ipmi::Identity, const std::string&)> authorize_client_registration;
	// Explicit HOST policy for the still-unverified connect authorization rules.
	// Missing callback excludes connect. Zero opts into this isolated model only;
	// it is not evidence that native authorization has been implemented.
	std::function<int(Ipmi::Identity, const Ipmi::ConnectionInfo&)> authorize_connect;
	// Optional trusted, atomic caller/name snapshot for diagnostic op10 only.
	// Without it, use identity() and leave names unknown (all zero).
	std::function<DiagnosticCaller()> diagnostic_caller;
	// No enabled predicate / no sink means disabled primary tracing, not failure.
	// All diagnostic callbacks are bounded/nonthrowing and run without Core or
	// gateway locks. A sink copies the event if it needs it after returning.
	std::function<bool()> diagnostic_enabled;
	std::function<void(const DiagnosticEvent&)> diagnostic_submit;
	// Unknown full-width selector observation is independent of primary enable
	// and the low16 sign-bit append filter. It does not change syscall results.
	std::function<void(const DiagnosticEvent&)> diagnostic_unknown;
};

class Gateway final {
public:
	Gateway(Ipmi::Core& core, Callbacks callbacks, IpmiWire::Limits limits = {},
	        IpmiWire::SyncType receive_type = IpmiWire::SyncType::Inline, size_t max_pending = 64);
	~Gateway();
	// All op numbers are hexadecimal. Success writes a LE int32 opResult last;
	// Failure returns -1 and sets injected guest errno. Earlier failures do not
	// write opResult; a fault during final copyout can leave a written prefix.
	// Descriptor is locally zero-filled then exactly desc_size bytes are copied.
	// Interrupted 400/320/310 calls retain the ticket and copied output metadata.
	// Retry with the same PID/TID/op/kid and scalar addresses/size/tag; descriptor
	// contents are not reread. At most one pending operation per identity, 64 total
	// (max_pending may lower this). Conflicting/reentrant wait operations are Busy;
	// non-wait operations still perform their normal ownership checks.
	// Invoke320 terminal completion releases its caller slot before final opResult,
	// including on copy failure. Such a failure never retains a replayable cache.
	// Op10 ignores tag and never enters Pending. Its exact64 copyin precedes
	// tracing, followed by final4 zero copyout; failed copyout may write a prefix
	// and cannot undo trace submission. Each repeat is a new diagnostic operation.
	int Call(uint32_t op, int32_t kid, uint64_t op_result, uint64_t descriptor,
	         uint64_t desc_size, uint64_t tag);
private:
	struct Impl;
	std::unique_ptr<Impl> m;
	Ipmi::Core& core;
	Callbacks callbacks;
	IpmiWire::Limits limits;
	IpmiWire::SyncType receive_type;
};
} // namespace Libs::LibKernel::IpmiGateway
