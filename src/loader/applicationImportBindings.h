// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LOADER_APPLICATION_IMPORT_BINDINGS_H_
#define KYTY_LOADER_APPLICATION_IMPORT_BINDINGS_H_

#include "loader/runtimeGuestExecution.h"
#include "loader/symbolDatabase.h"

#include <string>
#include <vector>

namespace Loader {
struct Program;

// Importer-owned immutable call entries. The importer and every private native
// provider remain mapped through owner quiescence; a copied function pointer is
// not revocable merely by clearing a GOT slot. Destruction requires that same
// external lifetime guarantee as GuestCallBoundary/HostCallReturnThunk.
class ApplicationImportBindings final {
public:
	explicit ApplicationImportBindings(Program* importer);
	~ApplicationImportBindings();
	ApplicationImportBindings(const ApplicationImportBindings&) = delete;
	ApplicationImportBindings& operator=(const ApplicationImportBindings&) = delete;
	uint64_t Bind(uint64_t slot, const std::string& raw_name);
	uint64_t BindResolved(const std::string& nid, const SymbolRecord& symbol);
	std::vector<std::string> AdmissionBlockers();
	bool HasPatchedEntry(uint64_t slot, const std::string& raw_name, uint64_t value) const;
	bool Published() const;
private:
	struct Record;
	static uint64_t KYTY_SYSV_ABI ResolveCall(void* opaque, uint64_t caller) noexcept;
	uint64_t Target(Record& record, const SymbolRecord& symbol);
	std::string Classify(const std::string& raw_name, const SymbolRecord& symbol, bool* host) const;
	Program* importer_;
	std::shared_ptr<RuntimeGuestExecution> execution_;
	mutable GuestExecutionMutex mutex_;
	std::vector<std::unique_ptr<Record>> records_;
};
} // namespace Loader
#endif
