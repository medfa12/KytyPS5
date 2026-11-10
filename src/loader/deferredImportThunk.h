#ifndef KYTY_LOADER_DEFERREDIMPORTTHUNK_H_
#define KYTY_LOADER_DEFERREDIMPORTTHUNK_H_

#include "common/abi.h"

#include <memory>

namespace Loader {

// Returns a validated, nonzero SysV target, not an API result. On resolution
// failure return the owner's audited nonreturning failure-gateway address.
// The resolver must return normally so its host RAII completes before the
// tail transfer; never jump out of its live C++ frames. It must not throw
// through generated code or change owner/TLS state on a successful resolution.
// A null return is an internal invariant trap, not an app-failure mechanism.
using DeferredImportResolver = uint64_t (KYTY_SYSV_ABI *)(void* record, uint64_t caller) noexcept;

// Baseline AMD64 SysV (SSE/SSE2), not Microsoft x64, AVX/AVX-512 or a custom
// guest ABI. Saves RAX (including AL varargs state), RDI/RSI/RDX/RCX/R8/R9,
// R10 and XMM0..7. Stack arguments, original return address and entry RSP are
// untouched; RBX/RBP/R12..15 and ABI control state are callback-preserved.
// R11 and flags are scratch. x87 return values work because this tail-jumps.
class DeferredImportThunk final {
public:
	// VirtualMemory::Init must already have run. Independent creations/calls are
	// concurrent-safe. Null arguments or VM allocation/protection/cache failures
	// return nullptr; allocation/code-generator exceptions may propagate.
	static std::unique_ptr<DeferredImportThunk> Create(void* record, DeferredImportResolver resolver);
	~DeferredImportThunk();

	DeferredImportThunk(const DeferredImportThunk&) = delete;
	DeferredImportThunk& operator=(const DeferredImportThunk&) = delete;
	DeferredImportThunk(DeferredImportThunk&&) = delete;
	DeferredImportThunk& operator=(DeferredImportThunk&&) = delete;

	uint64_t GetAddress() const { return m_address; }

private:
	DeferredImportThunk() = default;
	uint64_t m_address = 0;
};

// The owner must publish only a completed object (normal release/acquire
// synchronization). Code is immutable RX; the record and resolver outlive all
// calls. Before destroying a thunk/record/provider, revoke every published
// reference AND quiesce calls, including the resolved target's execution.
// This primitive does not cache a target, patch GOTs, pin providers, implement
// generation checks or protect against a stale copied function pointer.

} // namespace Loader

#endif /* KYTY_LOADER_DEFERREDIMPORTTHUNK_H_ */
