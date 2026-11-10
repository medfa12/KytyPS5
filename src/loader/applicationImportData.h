// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_APPLICATION_IMPORT_DATA_H_
#define KYTY_LOADER_APPLICATION_IMPORT_DATA_H_

#include "loader/guestCallBoundary.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <memory>
#include <string>

namespace Loader {
struct Program;

enum class ApplicationSanitizerReplacement { Malloc, New };

// Existing emulator no-sanitizer table ABI: size then 13/12 null pointer words.
// This deliberately does not model active/versioned native sanitizer providers.
// Native absence/caller evidence: docs/vsh-sanitizer-owner-20261105.md.
struct ApplicationSanitizerMallocTable {
	uint64_t size = 112;
	uint64_t hooks[13] {};
};
struct ApplicationSanitizerNewTable {
	uint64_t size = 104;
	uint64_t hooks[12] {};
};
static_assert(sizeof(ApplicationSanitizerMallocTable) == 112);
static_assert(sizeof(ApplicationSanitizerNewTable) == 104);

// Bundled libc's NWtTN10cJzE header. Inactive tracing still requires writable
// mask/table cells: libc clears *mask unconditionally during heap startup.
struct ApplicationHeapTraceInfo {
	uint64_t size = 32;
	uint32_t reserved = 0;
	uint32_t mode = 0;
	uint64_t mask = 0;
	uint64_t table = 0;
};
static_assert(sizeof(ApplicationHeapTraceInfo) == 32);
static_assert(offsetof(ApplicationHeapTraceInfo, size) == 0);
static_assert(offsetof(ApplicationHeapTraceInfo, reserved) == 8);
static_assert(offsetof(ApplicationHeapTraceInfo, mode) == 12);
static_assert(offsetof(ApplicationHeapTraceInfo, mask) == 16);
static_assert(offsetof(ApplicationHeapTraceInfo, table) == 24);

// Owner-pinned RW guest storage for FOUR exact audited HLE object registrations
// and separately classified, API-returned inactive sanitizer/heap-trace cells,
// never a whitelist permitting their original host addresses. Loader serialization
// protects creation/lookup; guest object values are deliberately mutable. The
// executable owner retains this store with its private modules until real drain
// and final Program destruction, including across stop and subsequent launches.
class ApplicationImportData final {
public:
	static constexpr uint64_t MappingSize = 0x4000;
	static constexpr size_t ProgramNameCapacity = 512; // libKernel.cpp: 511 + terminator
	static std::unique_ptr<ApplicationImportData> Create(const Program* owner, GuestCallOwner lifetime) noexcept;
	~ApplicationImportData();
	ApplicationImportData(const ApplicationImportData&) = delete;
	ApplicationImportData& operator=(const ApplicationImportData&) = delete;

	// Check NID + full versioned Object registration + expression + address.
	// Declared ELF size may be zero (unspecified), otherwise it must be exact.
	// Signed addends must remain strictly inside the selected object, not merely
	// somewhere in the backing page (especially not the progname pointer's buffer).
	static bool Supports(const SymbolDatabase& registrations, const std::string& raw_name,
	    const SymbolRecord& resolved, int64_t addend = 0, uint64_t declared_size = 0);
	uint64_t Address(GuestCallOwner lifetime, const SymbolDatabase& registrations,
	    const std::string& raw_name, const SymbolRecord& resolved, int64_t addend = 0,
	    uint64_t declared_size = 0) const;
	bool ContainsObjectAddress(GuestCallOwner lifetime, uint64_t address) const noexcept;
	// Callable query result only. Never added to Supports/ContainsObjectAddress;
	// sharing a backing allocation does not make these importable ELF objects.
	uint64_t SanitizerTable(GuestCallOwner lifetime, ApplicationSanitizerReplacement kind) const noexcept;
	// API-returned cells only, NOT additional importable objects. Repeated queries
	// must not reset guest-written cells; the owner pins them even after stop.
	bool HeapTraceInfo(GuestCallOwner lifetime, ApplicationHeapTraceInfo* output) const noexcept;
	static constexpr size_t HeapTraceTableCount = 64;

private:
	explicit ApplicationImportData(GuestCallOwner lifetime): lifetime_(lifetime) {}
	GuestCallOwner lifetime_;
	uint64_t mapping_ = 0;
};
} // namespace Loader
#endif
