#include "loader/symbolDatabase.h"
#include "loader/npOwnershipExports.h"
#include "loader/npWorkQueueExports.h"
#include "loader/npCalloutExports.h"
#include "loader/npHttpPoolExports.h"
#include "loader/npDispatcherExports.h"
#include "loader/npEventQueueExports.h"
#include "loader/nativeSslSmoke.h"

#include "common/file.h"

#include <fmt/format.h>
#include <magic_enum.hpp>
#include <utility>

namespace Loader {

constexpr char LIB_PREFIX[] = "libSce";

static std::string UpdateName(const std::string& str) {
	return str.starts_with(LIB_PREFIX) ? Common::RemoveFirst(str, 6) : str;
}

std::string SymbolDatabase::GenerateName(const SymbolResolve& s) {
	auto library = UpdateName(s.library);
	auto module  = UpdateName(s.module);
	return fmt::format("{}[{}_v{}][{}_v{}.{}][{}]", s.name.c_str(), library.c_str(),
	                   s.library_version, module.c_str(), s.module_version_major,
	                   s.module_version_minor, magic_enum::enum_name(s.type));
}

void SymbolDatabase::AddRecord(SymbolRecord&& r, SymbolType type) {
	auto  index   = m_symbols.size();
	auto& by_type = m_prefix_index[std::string(magic_enum::enum_name(type))];
	for (size_t pos = r.name.find('['); pos != std::string::npos; pos = r.name.find('[', pos + 1)) {
		by_type[r.name.substr(0, pos)].push_back(index);
	}
	m_map.insert_or_assign(r.name, index);
	m_symbols.push_back(std::move(r));
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr) {
	if (!Accept(s)) { return; }
	SymbolRecord r {};
	r.name  = GenerateName(s);
	r.vaddr = vaddr;
	AddRecord(std::move(r), s.type);
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr, const std::string& dbg_name) {
	if (!Accept(s)) { return; }
	SymbolRecord r {};
	r.name     = GenerateName(s);
	r.vaddr    = vaddr;
	r.dbg_name = dbg_name;
	AddRecord(std::move(r), s.type);
}

bool SymbolDatabase::Accept(const SymbolResolve& s) const {
	if (m_policy == ExportPolicy::All) { return true; }
	if (s.type != SymbolType::Func) { return false; }
	switch (m_policy) {
		case ExportPolicy::NpOwnership: return IsNpOwnershipExport(s.name);
		case ExportPolicy::NpWorkQueue: return IsNpWorkQueueExport(s.name);
		case ExportPolicy::NpCallout: return IsNpCalloutExport(s.name);
		case ExportPolicy::NpHttpPool: return IsNpHttpPoolExport(s.name);
		case ExportPolicy::NpDispatcher: return IsNpDispatcherExport(s.name);
		case ExportPolicy::NpEventQueue: return IsNpEventQueueExport(s.name);
		case ExportPolicy::NativeSsl: return NativeSslSmoke::PublicExportIdentity(s);
		default: return false;
	}
}

const SymbolRecord* SymbolDatabase::FindRegistration(const SymbolResolve& s, std::string_view expression) const {
	const auto name = GenerateName(s);
	const SymbolRecord* found = nullptr;
	for (const auto& record: m_symbols) {
		if (record.name != name) { continue; }
		if (record.vaddr == 0 || record.dbg_name != expression || (found && found->vaddr != record.vaddr)) { return nullptr; }
		found = &record;
	}
	return found;
}

const SymbolRecord* SymbolDatabase::FindRegistration(std::string_view nid, SymbolType type,
                                                     const SymbolRecord& resolved) const {
	const auto* candidates = FindPrefix(std::string(nid), type);
	if (!candidates || !resolved.vaddr) { return nullptr; }
	const SymbolRecord* found = nullptr;
	for (const auto index: *candidates) {
		const auto& record = m_symbols[index];
		if (record.name != resolved.name) { continue; }
		// Validate every registration of this full identity. A different library
		// at the same address is independent; conflicting exact duplicates are not.
		if (record.vaddr != resolved.vaddr || record.dbg_name != resolved.dbg_name) { return nullptr; }
		found = &record;
	}
	return found;
}

const std::vector<size_t>* SymbolDatabase::FindPrefix(const std::string& prefix,
                                                      SymbolType         type) const {
	auto type_it = m_prefix_index.find(std::string(magic_enum::enum_name(type)));
	if (type_it == m_prefix_index.end()) {
		return nullptr;
	}
	auto it = type_it->second.find(prefix);
	return it == type_it->second.end() ? nullptr : &it->second;
}

void SymbolDatabase::DbgDump(const std::string& folder, const std::string& file_name) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + file_name);

	for (const auto& sym: m_symbols) {
		f.Printf("%" PRIx64 " %s\n", sym.vaddr, sym.name.c_str());
	}

	f.Close();
}

const SymbolRecord* SymbolDatabase::Find(const SymbolResolve& s) const {
	auto it = m_map.find(GenerateName(s));
	if (it == m_map.end()) {
		return nullptr;
	}
	auto index = it->second;
	if (index >= m_symbols.size()) {
		return nullptr;
	}
	return &m_symbols[index];
}

const SymbolRecord* SymbolDatabase::FindByNid(const std::string& nid, SymbolType type,
                                                uint64_t vaddr) const {
	const auto* indices = FindPrefix(nid, type);
	if (indices == nullptr) {
		return nullptr;
	}
	for (auto index: *indices) {
		const auto& symbol = m_symbols[index];
		if (vaddr == 0 || symbol.vaddr == vaddr) {
			return &symbol;
		}
	}
	return nullptr;
}

const SymbolRecord* SymbolDatabase::FindByName(const std::string& name, SymbolType type) const {
	const auto* indices = FindPrefix(name, type);
	return (indices == nullptr || indices->empty()) ? nullptr : &m_symbols[indices->front()];
}

} // namespace Loader
