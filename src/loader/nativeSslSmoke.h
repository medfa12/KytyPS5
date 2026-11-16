#pragma once

#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Loader {
struct DynamicInfo;
namespace NativeSslSmoke {

enum class Mode { Disabled, Smoke, Production, Conflict };
constexpr Mode SelectMode(std::string_view smoke, std::string_view production) noexcept {
	return smoke == "1" ? (production == "1" ? Mode::Conflict : Mode::Smoke) :
	    production == "1" ? Mode::Production : Mode::Disabled;
}
// One immutable process snapshot, shared with networking. No live environment toggles.
Mode RequestedMode() noexcept;
bool ProductionRequested() noexcept;

// Real DT_INIT admission only: no SSL context/init/stats/term calls.
class ProviderStartup {
public:
	bool Begin(unsigned provider, bool primary_thread, bool default_arguments) noexcept;
	bool Complete(int result) noexcept;
	bool Completed() const noexcept { return m_next == 2 && !m_invoking && !m_failed; }
	unsigned Next() const noexcept { return m_next; }
	bool Returned(unsigned provider) const noexcept { return provider < 2 && m_returned[provider]; }
	int Result(unsigned provider) const noexcept { return m_results[provider]; }
private:
	unsigned m_next = 0;
	bool m_invoking = false;
	bool m_failed = false;
	std::array<bool,2> m_returned {};
	std::array<int,2> m_results {};
};

inline constexpr uint64_t PoolSize = 0x80000;
inline constexpr uint8_t SslHash[32] {
	0xc4,0xcd,0x75,0x8b,0xb1,0xf9,0xaa,0x25,0xff,0xef,0x51,0x7e,0x89,0xe0,0x29,0xef,
	0x4b,0x3f,0x6f,0x3b,0x22,0xb7,0xe5,0x25,0xc1,0xe4,0xbf,0x59,0x1b,0x8f,0xb3,0xa7,
};
struct Export {
	const char* nid;
	uint64_t rva;
	uint64_t size;
	SymbolType type = SymbolType::Func;
};
inline constexpr Export SslExports[] {
	{"hdpVEUDFW3s", 0x340, 0xdf}, {"-PoIzr3PEk0", 0x1850, 0xf2}, {"0K1yQ6Lv-Yc", 0x15f0, 0x65},
};
// Retail 13.60 complete raw public export set (library1/module2.1).
// This retail branch deliberately pins the retail provider, not the other
// firmware tree's same-named module; raw export and same-image hash gates remain.
inline constexpr Export SslPublicExports[] {
	{"+DzXseDVkeI",0x2500,0x19}, {"RwXD8grHZHM",0x36e0,0x8f}, {"qOn+wm28wmA",0x24f0,0xf},
	{"7whYpYfHP74",0x3140,0x88}, {"-PoIzr3PEk0",0x1850,0xf2}, {"R1ePzopYPYM",0x3260,0x92},
	{"7RBSTKGrmDA",0x3300,0x15e}, {"xHpt6+2pGYk",0x3460,0x136}, {"Eo0S65Jy28Q",0x35a0,0x136},
	{"kLB5aGoUJXg",0x2d30,0x131}, {"DOwXL+FQMEY",0x2e70,0xf2}, {"0XcZknp7-Wc",0x2190,0xa4},
	{"dQReuBX9sD8",0x31d0,0x88}, {"brRtwGBu4A8",0x2f70,0x1ce}, {"hdpVEUDFW3s",0x340,0xdf},
	{"Ab7+DH+gYyM",0x1ce0,0x5}, {"0K1yQ6Lv-Yc",0x15f0,0x65}, {"UQ+3Qu7v3cA",0x1cf0,0x5},
	{"TDfQqO-gMbY",0x2240,0x1f6}, {"qIvLs0gYxi0",0x2440,0xa5}, {"TL86glUrmUw",0x1e40,0x8e},
	{"4O7+bRkRUe8",0x1ed0,0x73}, {"tuscfitnhEo",0x9a0,0xa}, {"HJ1n138CQ2g",0x1690,0x77},
	{"zXvd6iNyfgc",0x9b0,0x12a}, {"viRXSHZYd0c",0x1330,0x151}, {"PwsHbErG+e8",0x26d0,0x5},
	{"po1X86mgHDU",0x25b0,0x87}, {"iNjkt9Poblw",0xe20,0xf8}, {"jltWpVKtetg",0x1160,0x10b},
	{"E4a-ahM57QQ",0x2520,0x82}, {"QWSxBzf6lAg",0x1da0,0x94}, {"50R2xYaYZwE",0x1490,0xdb},
	{"p5bM5PPufFY",0xdd0,0x4e}, {"hi0veU3L2pU",0x1100,0x51}, {"IuduYLwFh9c",0x3770,0x1},
	{"P14ATpXc4J8",0x670,0x7}, {"hwrHV6Pprk4",0x1570,0x77}, {"-WqxBRAUVM4",0x28c0,0x144},
	{"w1+L-27nYas",0x2a10,0xa0}, {"g-zCwUKstEQ",0x2830,0x87}, {"1VM0h1JrUfA",0x1270,0x8d},
	{"3-643mGVFJo",0x1cd0,0x5}, {"bKaEtQnoUuQ",0x1d00,0x94}, {"KPh5GncdOcc",0x590,0xdd},
	{"iLKz4+ukLqk",0x26e0,0x144}, {"m-zPyAsIpco",0x2640,0x87}, {"-TbZc8pwPNc",0x2ab0,0x85},
	{"CpdjdFTGv3U",0x2b40,0x1e6}, {"AzUipl-DpIw",0x3780,0xa}, {"9bKYzKP6kYU",0x3790,0x129},
	{"Md+HYkCBZB4",0x38f0,0x1c}, {"ipLIammTj2Q",0x3910,0x98}, {"pBwtarKd7eg",0x39b0,0xfc},
	{"wcVuyTUr5ys",0x38e0,0x3}, {"PRWr3-ytpdg",0x38c0,0x9}, {"cW7VCIMCh9A",0x38d0,0x3},
};
inline const Export* PublicExport(std::string_view nid) noexcept {
	for (const auto& e: SslPublicExports) { if (nid == e.nid) { return &e; } }
	return nullptr;
}
inline bool PublicExportIdentity(const SymbolResolve& query) noexcept {
	const auto* e = PublicExport(query.name);
	return e && query.type == e->type && query.library == "libSceSsl" && query.library_version == 1 &&
	    query.module == "libSceSsl" && query.module_version_major == 2 && query.module_version_minor == 1;
}
inline const SymbolRecord* FindPublicExport(const SymbolDatabase& provider, uint64_t base, const SymbolResolve& query) {
	if (!PublicExportIdentity(query)) { return nullptr; }
	const auto* entry = PublicExport(query.name);
	const auto* record = provider.Find(query);
	return record && base <= UINT64_MAX - entry->rva && record->vaddr == base + entry->rva ? record : nullptr;
}
enum class ImportPolicy { Unrelated, Accepted, Rejected };
ImportPolicy ProductionImport(const DynamicInfo& info, std::string_view raw, SymbolType type, SymbolResolve& query);
inline constexpr Export LibcExports[] {
	{"-hn1tcVHq5Q",0xd6a0,0xe}, {"W6SiVSiCDtI",0x12b20,0xb}, {"OJjm-QOIHlI",0x12b30,0xe5},
	{"LYo3GhIlB38",0x13500,0xf3}, {"gigoVHZvVPE",0x13600,0x11b}, {"Vla-Z+eXlxo",0x12eb0,0xfe},
	{"mfHdJTIvhuo",0x13b80,0xb}, {"8zTFvBIAIN8",0x14700,0x17f}, {"Q3VBxCXhUHs",0x3e00,0x8c5},
	{"DfivPArhucg",0x7ad20,0x3b}, {"Ovb2dSJOAuE",0x42910,0x83}, {"hcuQgD53UxM",0x145d0,0xef},
	{"H2e8t5ScQGc",0x166c0,0x25a},
	{"H8AprKeZtNg",0x1b4480,0x1c8,SymbolType::Object}, {"1TDo-ImqkJc",0x1b40f0,0x1c8,SymbolType::Object},
};
struct HostContract {
	const char* nid;
	const char* registration;
	SymbolType type = SymbolType::Func;
};
std::span<const HostContract> HostContracts() noexcept;
bool RawExportMatches(const DynamicInfo& info, const Export& entry, bool ssl);
bool HostMatches(const SymbolRecord* record, const HostContract& contract) noexcept;

using Init = KYTY_SYSV_ABI int (*)(uint64_t);
using Stats = KYTY_SYSV_ABI int (*)(int, void*);
using Term = KYTY_SYSV_ABI int (*)(int);
struct Endpoints { Init init = nullptr; Stats stats = nullptr; Term term = nullptr; };
enum class Phase { Fresh, Init, Stats, Term, Complete };
enum class Outcome { NotRun, Rejected, InitFailed, StatsFailed, CanaryDamaged, TermFailed, Success };
struct Result {
	Outcome outcome = Outcome::NotRun;
	int id = 0;
	int stats_result = 0;
	int term_result = 0;
	bool init_returned = false;
	bool stats_returned = false;
	bool term_returned = false;
	bool canaries_ok = false;
	std::array<uint8_t,32> stats {};
	std::array<uint8_t,16> before {};
	std::array<uint8_t,16> after {};
};
// Observer never supplies or changes provider state. Called after each normal return.
using Observer = void (*)(void*, Phase returned_phase, int id);
class Control {
public:
	explicit Control(std::string_view option): m_enabled(option == "1") {}
	Control(const Control&) = delete;
	Control& operator=(const Control&) = delete;
	bool Run(Endpoints endpoints, Observer observer = nullptr, void* context = nullptr);
	const Result& GetResult() const noexcept { return m_result; }
	Phase GetPhase() const noexcept { return m_phase; }
private:
	bool m_enabled;
	Phase m_phase = Phase::Fresh;
	Result m_result;
};
} // namespace NativeSslSmoke
} // namespace Loader
