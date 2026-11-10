#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"

#include <unordered_map>
#include <vector>
#include <string_view>

namespace Loader {

enum class SymbolType {
	Unknown,
	Func,
	Object,
	TlsModule,
	NoType,
};

struct SymbolRecord {
	std::string name;
	std::string dbg_name;
	uint64_t    vaddr;
};

struct SymbolResolve {
	std::string name;
	std::string library;
	int         library_version;
	std::string module;
	int         module_version_major;
	int         module_version_minor;
	SymbolType  type;
};

class SymbolDatabase {
public:
	enum class ExportPolicy { All, NpOwnership, NpWorkQueue, NpCallout, NpHttpPool, NpDispatcher, NpEventQueue, PrivateSslSmoke, NativeSsl };
	explicit SymbolDatabase(ExportPolicy policy = ExportPolicy::All): m_policy(policy) {}
	virtual ~SymbolDatabase() = default;
	[[nodiscard]] ExportPolicy GetExportPolicy() const { return m_policy; }

	void Add(const SymbolResolve& s, uint64_t vaddr);
	void Add(const SymbolResolve& s, uint64_t vaddr, const std::string& dbg_name);

	[[nodiscard]] const SymbolRecord* Find(const SymbolResolve& s) const;
	// HLE-only caller: reject overwritten/conflicting registrations, not just a resolver hit.
	[[nodiscard]] const SymbolRecord* FindRegistration(const SymbolResolve& s, std::string_view expression) const;
	[[nodiscard]] const SymbolRecord* FindRegistration(std::string_view nid, SymbolType type, const SymbolRecord& resolved) const;
	[[nodiscard]] const SymbolRecord* FindByNid(const std::string& nid, SymbolType type,
	                                              uint64_t vaddr = 0) const;
	[[nodiscard]] const SymbolRecord* FindByName(const std::string& name, SymbolType type) const;

	void DbgDump(const std::string& folder, const std::string& file_name);

	KYTY_CLASS_NO_COPY(SymbolDatabase);

	static std::string GenerateName(const SymbolResolve& s);

private:
	ExportPolicy m_policy;
	[[nodiscard]] bool Accept(const SymbolResolve& s) const;
	void AddRecord(SymbolRecord&& r, SymbolType type);
	[[nodiscard]] const std::vector<size_t>* FindPrefix(const std::string& prefix, SymbolType type) const;

	using PrefixIndex = std::unordered_map<std::string, std::vector<size_t>>;

	std::vector<SymbolRecord>               m_symbols;
	std::unordered_map<std::string, size_t> m_map;
	// Keyed by type name, then by every prefix of the record name that ends right before a '['.
	// Index vectors are in insertion order so lookups match the previous linear scan.
	std::unordered_map<std::string, PrefixIndex> m_prefix_index;
};

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_ */
