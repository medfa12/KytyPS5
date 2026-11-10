#include "loader/nativeSslSmoke.h"
#include "loader/runtimeLinker.h"
#include "loader/elf.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace Loader::NativeSslSmoke {
namespace {
constexpr HostContract Hosts[] {
	{"F8bUHwAG284","LibKernel::PthreadMutexattrInit"},
	{"iMp8QpE+XO4","LibKernel::PthreadMutexattrSettype"},
	{"smWEktiyyG0","LibKernel::PthreadMutexattrDestroy"},
	{"cmo1RIYva9o","LibKernel::PthreadMutexInit"},
	{"9UK1vLZQft4","LibKernel::PthreadMutexLock"},
	{"tn3VlD0hG60","LibKernel::PthreadMutexUnlock"},
	{"2Of0f+3mhhE","LibKernel::PthreadMutexDestroy"},
	{"mL8NDH86iQI","Memory::KernelMapNamedFlexibleMemory"},
	{"cQke9UuBQOk","Memory::KernelMunmap"},
	{"WB66evu8bsU","LibKernel::KernelGetCompiledSdkVersion"},
	{"jh+8XiK4LeE","Memory::KernelIsAddressSanitizerEnabled"},
	{"Yrwoq3bti3c","LibKernel::SceKernelIsPs4Process"},
	{"MhC53TKmjVA","Kernel::SysctlByName"},
	{"Ou3iL1abvng","LibKernel::stack_chk_fail"},
	{"9BcDykPmo1I","LibKernel::get_error_addr"},
	{"HoLVWNanBBc","LibKernel::getpid"},
	{"3eqs37G74-s","LibKernel::PthreadGetthreadid"},
	{"Q-8duzyUNn0","Kernel::QueryInternalLibcDiagnosticMode"},
	{"Wl2o5hOVZdw","LibKernel::KernelBacktrace"},
	{"FxVZqBAA7ks","LibKernel::write"},
	{"VADc3MNQ3cM","LibKernel::Signal"},
	{"rNhWz+lvOMU","LibKernel::KernelSetThreadDtors"},
	{"pB-yGZ2nQ9o","LibKernel::KernelSetThreadAtexitCount"},
	{"WhCc1w3EhSI","LibKernel::KernelSetThreadAtexitReport"},
	{"959qrazPIrg","LibKernel::KernelGetProcParam"},
	{"f7uOxY9mM1U","&LibKernel::g_stack_chk_guard",SymbolType::Object},
};
}

std::span<const HostContract> HostContracts() noexcept { return Hosts; }
Mode RequestedMode() noexcept {
	static const Mode mode = [] {
		const char* smoke = std::getenv("KYTY_NATIVE_SSL_SMOKE");
		const char* production = std::getenv("KYTY_NATIVE_SSL");
		return SelectMode(smoke ? smoke : "", production ? production : "");
	}();
	return mode;
}
bool ProductionRequested() noexcept { return RequestedMode() == Mode::Production; }

bool ProviderStartup::Begin(unsigned provider, bool primary_thread, bool default_arguments) noexcept {
	if (m_failed || m_invoking || m_next >= 2 || provider != m_next || !primary_thread || !default_arguments) { return false; }
	m_invoking = true;
	return true;
}
bool ProviderStartup::Complete(int result) noexcept {
	if (!m_invoking || m_next >= 2) { return false; }
	m_results[m_next] = result;
	m_returned[m_next++] = true;
	m_invoking = false;
	m_failed = result != 0;
	return true;
}
ImportPolicy ProductionImport(const DynamicInfo& info, std::string_view raw, SymbolType type, SymbolResolve& query) {
	const auto ids = Common::Split(std::string(raw), '#');
	const auto* entry = ids.empty() ? nullptr : PublicExport(ids[0]);
	if (ids.size() != 3) { return entry ? ImportPolicy::Rejected : ImportPolicy::Unrelated; }
	const LibraryId* lib = nullptr;
	const ModuleId* mod = nullptr;
	unsigned libraries = 0, modules = 0;
	// Imports only. Export metadata cannot rescue missing/ambiguous import identity.
	for (const auto& l: info.import_libs) { if (l.id == ids[1]) { lib = &l; ++libraries; } }
	for (const auto& m: info.import_modules) { if (m.id == ids[2]) { mod = &m; ++modules; } }
	const bool ssl = (lib && lib->name == "libSceSsl") || (mod && mod->name == "libSceSsl");
	if (!ssl && !entry) { return ImportPolicy::Unrelated; }
	if (!entry || libraries != 1 || modules != 1 || type != entry->type ||
	    lib->name != "libSceSsl" || lib->version != 1 || mod->name != "libSceSsl" ||
	    (mod->version_major != 1 && mod->version_major != 2) || mod->version_minor != 1) { return ImportPolicy::Rejected; }
	// Explicit same-library ABI compatibility: library1/module1.1 -> pinned module2.1.
	query = {entry->nid, "libSceSsl", 1, "libSceSsl", 2, 1, entry->type};
	return ImportPolicy::Accepted;
}
bool HostMatches(const SymbolRecord* record, const HostContract& contract) noexcept {
	return record && record->vaddr != 0 && record->dbg_name == contract.registration;
}

bool RawExportMatches(const DynamicInfo& info, const Export& entry, bool ssl) {
	if (!info.symbol_table || !info.str_table || info.symbol_table_entry_size != sizeof(Elf64_Sym) ||
	    info.symbol_table_total_size % sizeof(Elf64_Sym)) { return false; }
	const std::string libid = ssl ? "G" : "C";
	const std::string library = ssl ? "libSceSsl" : "libSceLibcInternal";
	if (std::count_if(info.export_libs.begin(), info.export_libs.end(), [&](const auto& l) { return l.id == libid; }) != 1 ||
	    std::count_if(info.export_modules.begin(), info.export_modules.end(), [](const auto& m) { return m.id == "A"; }) != 1 ||
	    !std::any_of(info.export_libs.begin(), info.export_libs.end(), [&](const auto& l) {
		    return l.id == libid && l.name == library && l.version == 1;
	    }) || !std::any_of(info.export_modules.begin(), info.export_modules.end(), [&](const auto& m) {
		    return m.id == "A" && m.name == library && m.version_major == (ssl ? 2 : 1) && m.version_minor == 1;
	    })) { return false; }
	size_t count = 0;
	for (size_t i = 0; i < info.symbol_table_total_size / sizeof(Elf64_Sym); ++i) {
		const auto& s = info.symbol_table[i];
		if (s.st_name >= info.str_table_size) { return false; }
		const char* begin = info.str_table + s.st_name;
		const auto* end = static_cast<const char*>(std::memchr(begin, 0, info.str_table_size - s.st_name));
		if (!end) { return false; }
		std::string_view raw(begin, end - begin);
		if (raw.substr(0, raw.find('#')) != entry.nid || s.st_shndx == 0) { continue; }
		if (++count != 1 || raw != std::string(entry.nid) + "#" + libid + "#A" ||
		    s.GetBind() != STB_GLOBAL || s.GetType() != (entry.type == SymbolType::Func ? STT_FUNC : STT_OBJECT) ||
		    s.st_value != entry.rva || s.st_size != entry.size) { return false; }
	}
	return count == 1;
}

bool Control::Run(Endpoints endpoints, Observer observer, void* context) {
	if (!m_enabled || m_phase != Phase::Fresh) { return false; }
	if (!endpoints.init || !endpoints.stats || !endpoints.term) {
		m_result.outcome = Outcome::Rejected;
		m_phase = Phase::Complete;
		return false;
	}
	m_phase = Phase::Init;
	m_result.id = endpoints.init(PoolSize);
	m_result.init_returned = true;
	if (observer) { observer(context, Phase::Init, m_result.id); }
	if (m_result.id < 0) {
		m_result.outcome = Outcome::InitFailed;
		m_phase = Phase::Complete;
		return true;
	}
	struct alignas(16) Output {
		std::array<uint8_t,16> before;
		std::array<uint8_t,32> bytes;
		std::array<uint8_t,16> after;
	} output;
	static_assert(offsetof(Output, bytes) == 16 && offsetof(Output, after) == 48 && sizeof(Output) == 64);
	output.before.fill(0xa7); output.bytes.fill(0xcd); output.after.fill(0x5e);
	m_phase = Phase::Stats;
	m_result.stats_result = endpoints.stats(m_result.id, output.bytes.data());
	m_result.stats_returned = true;
	m_result.stats = output.bytes;
	m_result.before = output.before;
	m_result.after = output.after;
	m_result.canaries_ok = std::all_of(output.before.begin(), output.before.end(), [](auto x) { return x == 0xa7; }) &&
	    std::all_of(output.after.begin(), output.after.end(), [](auto x) { return x == 0x5e; });
	if (observer) { observer(context, Phase::Stats, m_result.id); }
	m_phase = Phase::Term; // Published before the only call; no catch/retry around guest failures.
	m_result.term_result = endpoints.term(m_result.id);
	m_result.term_returned = true;
	if (observer) { observer(context, Phase::Term, m_result.id); }
	m_result.outcome = !m_result.canaries_ok ? Outcome::CanaryDamaged :
	    m_result.stats_result != 0 ? Outcome::StatsFailed :
	    m_result.term_result != 0 ? Outcome::TermFailed : Outcome::Success;
	m_phase = Phase::Complete;
	return true;
}
} // namespace Loader::NativeSslSmoke
