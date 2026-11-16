// Host-only: controlled SysV endpoints and synthetic ELF metadata, no firmware.
namespace NativeSslSmokeTest {
namespace Smoke = Loader::NativeSslSmoke;
constexpr const char* Name = "NativeSslSmoke";
Smoke::Control* active = nullptr;
int returned_id = 7, stats_error = 0, term_error = 0;
unsigned init_calls = 0, stats_calls = 0, term_calls = 0, observed = 0;
bool damage = false, throw_stats = false;
int KYTY_SYSV_ABI Init(uint64_t size);
int KYTY_SYSV_ABI Stats(int id, void* out);
int KYTY_SYSV_ABI Term(int id);
Smoke::Endpoints Endpoints() { return {Init, Stats, Term}; }
int KYTY_SYSV_ABI Init(uint64_t size) {
	++init_calls;
	Check(Name, size == 0x80000 && active->GetPhase() == Smoke::Phase::Init, "init size/phase");
	Check(Name, !active->Run(Endpoints()), "reentrant init replay admitted");
	return returned_id;
}
int KYTY_SYSV_ABI Stats(int id, void* out) {
	++stats_calls;
	Check(Name, id == returned_id && active->GetPhase() == Smoke::Phase::Stats, "stats changed ID/order");
	Check(Name, !active->Run(Endpoints()), "reentrant stats replay admitted");
	if (throw_stats) { throw 91; }
	for (unsigned i = 0; i < 32; ++i) { static_cast<uint8_t*>(out)[i] = static_cast<uint8_t>(i * 3 + 1); }
	if (damage) { const uint8_t byte = 0; std::memcpy(static_cast<uint8_t*>(out) + 32, &byte, 1); }
	return stats_error;
}
int KYTY_SYSV_ABI Term(int id) {
	++term_calls;
	Check(Name, id == returned_id && active->GetPhase() == Smoke::Phase::Term, "term changed ID/order");
	Check(Name, !active->Run(Endpoints()), "reentrant term replay admitted");
	return term_error;
}
void Observe(void*, Smoke::Phase phase, int id) {
	const unsigned expected = phase == Smoke::Phase::Init ? 0u : phase == Smoke::Phase::Stats ? 1u : 2u;
	Check(Name, id == returned_id && observed == expected, "observer stage/ID");
	const auto& result = active->GetResult();
	Check(Name, expected == 0 ? result.init_returned : expected == 1 ? result.stats_returned : result.term_returned, "evidence before return");
	++observed;
}
void Reset() {
	init_calls = stats_calls = term_calls = observed = 0;
	returned_id = 7; stats_error = term_error = 0; damage = throw_stats = false;
}
void Lifecycle() {
	Reset();
	for (const char* option: {"", "0", "true", "01", "1 "}) {
		Smoke::Control off(option); active = &off;
		Check(Name, !off.Run(Endpoints()) && off.GetPhase() == Smoke::Phase::Fresh, "default-off invoked endpoint");
	}
	Check(Name, init_calls == 0, "disabled endpoint called");
	Smoke::Control missing("1"); active = &missing;
	Check(Name, !missing.Run({Init, nullptr, Term}) && !missing.Run(Endpoints()) && init_calls == 0, "partial contract armed/rearmed");
	for (unsigned scenario = 0; scenario != 6; ++scenario) {
		Reset(); Smoke::Control state("1"); active = &state;
		if (scenario == 0) { returned_id = static_cast<int>(0x809517d5u); }
		if (scenario == 1) { stats_error = static_cast<int>(0x8095f003u); }
		if (scenario == 2) { term_error = -55; }
		if (scenario == 3) { damage = true; }
		if (scenario == 4) { returned_id = 0; } // Never substitute a convenient positive ID.
		Check(Name, state.Run(Endpoints(), Observe), "fresh control refused");
		const auto& result = state.GetResult();
		Check(Name, result.id == returned_id && result.init_returned && init_calls == 1 && !state.Run(Endpoints()), "init result/replay lost");
		if (scenario == 0) {
			Check(Name, stats_calls == 0 && term_calls == 0 && !result.stats_returned && !result.term_returned &&
			            result.outcome == Smoke::Outcome::InitFailed, "failed init called stats/term");
		} else {
			Check(Name, stats_calls == 1 && term_calls == 1 && observed == 3 && result.term_returned &&
			            result.stats_result == stats_error && result.term_result == term_error, "term-once/raw error lost");
			for (unsigned i = 0; i < 32; ++i) { Check(Name, result.stats[i] == i * 3 + 1, "actual stats replaced"); }
			const auto expected = scenario == 1 ? Smoke::Outcome::StatsFailed : scenario == 2 ? Smoke::Outcome::TermFailed :
			    scenario == 3 ? Smoke::Outcome::CanaryDamaged : Smoke::Outcome::Success;
			Check(Name, result.outcome == expected && result.canaries_ok == !damage, "outcome/canary classification");
		}
	}
	Reset(); Smoke::Control interrupted("1"); active = &interrupted; throw_stats = true;
	bool propagated = false;
	try { interrupted.Run(Endpoints()); } catch (int value) { propagated = value == 91; }
	Check(Name, propagated && !interrupted.GetResult().stats_returned && term_calls == 0 &&
	            interrupted.GetPhase() == Smoke::Phase::Stats && !interrupted.Run(Endpoints()), "nonreturn converted to success/cleanup/replay");
	active = nullptr;
}
void Profiles() {
	for (bool ssl: {false, true}) {
		const std::span<const Smoke::Export> entries = ssl ? std::span<const Smoke::Export>(Smoke::SslExports) : std::span<const Smoke::Export>(Smoke::LibcExports);
		for (const auto& entry: entries) {
			std::string name = std::string(entry.nid) + (ssl ? "#G#A" : "#C#A");
			std::array<Loader::Elf64_Sym,2> symbols {};
			auto& sym = symbols[0];
			sym.st_info = (Loader::STB_GLOBAL << 4) | (entry.type == Loader::SymbolType::Func ? Loader::STT_FUNC : Loader::STT_OBJECT);
			sym.st_shndx = 1; sym.st_value = entry.rva; sym.st_size = entry.size;
			symbols[1] = sym;
			Loader::DynamicInfo info;
			info.str_table = name.data(); info.str_table_size = name.size() + 1;
			info.symbol_table = symbols.data(); info.symbol_table_entry_size = sizeof(sym); info.symbol_table_total_size = sizeof(sym);
			const char* library = ssl ? "libSceSsl" : "libSceLibcInternal";
			info.export_libs.push_back({ssl ? "G" : "C", 1, library});
			info.export_modules.push_back({"A", ssl ? 2 : 1, 1, library});
			Check(Name, Smoke::RawExportMatches(info, entry, ssl), "audited raw export rejected");
			info.symbol_table_total_size = sizeof(symbols);
			Check(Name, !Smoke::RawExportMatches(info, entry, ssl), "last-wins duplicate admitted");
			info.symbol_table_total_size = sizeof(sym);
			for (unsigned mutation = 0; mutation != 7; ++mutation) {
				const auto original = sym;
				switch (mutation) {
					case 0: ++sym.st_value; break;
					case 1: ++sym.st_size; break;
					case 2: sym.st_shndx = 0; break;
					case 3: sym.st_info = Loader::STT_FUNC; break;
					case 4: sym.st_name = UINT32_MAX; break;
					case 5: ++info.export_libs[0].version; break;
					case 6: ++info.export_modules[0].version_minor; break;
				}
				Check(Name, !Smoke::RawExportMatches(info, entry, ssl), "changed raw export/identity admitted");
				sym = original; info.export_libs[0].version = 1; info.export_modules[0].version_minor = 1;
			}
			--info.str_table_size;
			Check(Name, !Smoke::RawExportMatches(info, entry, ssl), "unterminated raw string admitted");
		}
	}
	Loader::SymbolDatabase hidden(Loader::SymbolDatabase::ExportPolicy::PrivateSslSmoke);
	for (const auto& e: Smoke::SslExports) {
		Loader::SymbolResolve query {e.nid, "libSceSsl", 1, "libSceSsl", 2, 1, e.type};
		hidden.Add(query, e.rva);
		Check(Name, !hidden.Find(query) && !hidden.FindByNid(e.nid, e.type) && !hidden.FindByName(e.nid, e.type), "private SSL export escaped lookup policy");
	}
	for (const auto& h: Smoke::HostContracts()) {
		Loader::SymbolDatabase db;
		Loader::SymbolResolve query {h.nid, "libkernel", 1, "libkernel", 1, 1, h.type};
		db.Add(query, 0x1234, h.registration);
		Check(Name, Smoke::HostMatches(db.FindRegistration(query, h.registration), h), "exact registration rejected");
		db.Add(query, 0x5678, "LateStub");
		Check(Name, !db.FindRegistration(query, h.registration) && !Smoke::HostMatches(db.Find(query), h), "nonzero late stub accepted");
	}
}
} // namespace NativeSslSmokeTest

void TestNativeSslSmoke() {
	NativeSslSmokeTest::Lifecycle();
	NativeSslSmokeTest::Profiles();
}
