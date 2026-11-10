// Host-only policy/admission fixtures. No firmware calls or SSL context allocation.
namespace NativeSslProductionTest {
namespace Ssl = Loader::NativeSslSmoke;
constexpr const char* Name = "NativeSslProduction";

void ModesAndStartup() {
	for (const char* off: {"", "0", "true", "01", "1 ", "11"}) {
		Check(Name, Ssl::SelectMode(off, off) == Ssl::Mode::Disabled, "non-exact opt-in enabled SSL");
		Check(Name, Ssl::SelectMode("1", off) == Ssl::Mode::Smoke, "smoke mode lost isolation");
		Check(Name, Ssl::SelectMode(off, "1") == Ssl::Mode::Production, "production opt-in rejected");
	}
	Check(Name, Ssl::SelectMode("1", "1") == Ssl::Mode::Conflict, "conflicting modes admitted");
	Ssl::ProviderStartup state;
	Check(Name, !state.Completed() && !state.Complete(0), "completion without real initializer admission");
	Check(Name, !state.Begin(1, true, true) && !state.Begin(0, false, true) && !state.Begin(0, true, false), "order/thread/arguments admitted");
	Check(Name, state.Begin(0, true, true) && !state.Begin(0, true, true) && !state.Begin(1, true, true), "initializer reentry admitted");
	Check(Name, !state.Completed() && !state.Returned(0), "nonreturn treated as completion");
	Check(Name, state.Complete(0) && state.Returned(0) && state.Result(0) == 0 && !state.Completed(), "I return not retained");
	Check(Name, !state.Begin(0, true, true) && state.Begin(1, true, true), "I replay or S order incorrect");
	Check(Name, state.Complete(0) && state.Completed() && state.Returned(1) && !state.Begin(1, true, true), "S return/replay incorrect");
	for (unsigned provider: {0u, 1u}) {
		Ssl::ProviderStartup failed;
		if (provider == 1) { Check(Name, failed.Begin(0, true, true) && failed.Complete(0), "setup I admission"); }
		const int error = static_cast<int>(0x80950017u);
		Check(Name, failed.Begin(provider, true, true) && failed.Complete(error), "real failing initializer rejected before return");
		Check(Name, failed.Returned(provider) && failed.Result(provider) == error && !failed.Completed() &&
		            !failed.Begin(provider + 1, true, true) && !failed.Begin(0, true, true), "raw failure lost or rearmed");
	}
}

void ImportsAndExports() {
	// Retail image evidence: both address and extent differ from the older
	// same-named provider. A mixed hash/table profile must not pass this fixture.
	Check(Name, Ssl::PublicExport("TDfQqO-gMbY")->rva == 0x2240 &&
	            Ssl::PublicExport("w1+L-27nYas")->rva == 0x2a10 &&
	            Ssl::PublicExport("w1+L-27nYas")->size == 0xa0,
	      "retail SSL profile retained non-retail addresses/extents");
	Loader::DynamicInfo info;
	info.import_libs.push_back({"Q", 1, "libSceSsl"});
	info.import_modules.push_back({"Q", 2, 1, "libSceSsl"});
	Loader::SymbolResolve query {};
	Loader::SymbolDatabase provider(Loader::SymbolDatabase::ExportPolicy::NativeSsl);
	for (const auto& e: Ssl::SslPublicExports) {
		std::string raw_name = std::string(e.nid) + "#G#A";
		Loader::Elf64_Sym sym {};
		sym.st_info = (Loader::STB_GLOBAL << 4) | Loader::STT_FUNC;
		sym.st_shndx = 1; sym.st_value = e.rva; sym.st_size = e.size;
		Loader::DynamicInfo raw;
		raw.str_table = raw_name.data(); raw.str_table_size = raw_name.size() + 1;
		raw.symbol_table = &sym; raw.symbol_table_entry_size = sizeof(sym); raw.symbol_table_total_size = sizeof(sym);
		raw.export_libs.push_back({"G", 1, "libSceSsl"});
		raw.export_modules.push_back({"A", 2, 1, "libSceSsl"});
		Check(Name, Ssl::RawExportMatches(raw, e, true), "full native SSL export profile rejected");
		++sym.st_value;
		Check(Name, !Ssl::RawExportMatches(raw, e, true), "changed public SSL export address admitted");
		Loader::SymbolResolve exported {e.nid, "libSceSsl", 1, "libSceSsl", 2, 1, e.type};
		provider.Add(exported, 0x100000 + e.rva);
		for (int major: {1, 2}) {
			info.import_modules[0].version_major = major;
			Check(Name, Ssl::ProductionImport(info, std::string(e.nid) + "#Q#Q", e.type, query) == Ssl::ImportPolicy::Accepted,
			            "audited SSL library/version rejected");
			const auto* exact = Ssl::FindPublicExport(provider, 0x100000, query);
			Check(Name, exact && exact->vaddr == 0x100000 + e.rva && provider.FindByNid(e.nid, e.type) == exact &&
			            provider.FindByName(e.nid, e.type) == exact, "exact/global SSL lookup differs");
		}
	}
	// Read-only audited retail SceShellCore.elf (f2e569ce...) imports, library1/module2.1.
	constexpr const char* shellcore[] {
		"0K1yQ6Lv-Yc", "hdpVEUDFW3s", "-PoIzr3PEk0", "RwXD8grHZHM", "7whYpYfHP74", "R1ePzopYPYM", "7RBSTKGrmDA",
		"xHpt6+2pGYk", "Eo0S65Jy28Q", "DOwXL+FQMEY", "dQReuBX9sD8", "zXvd6iNyfgc", "viRXSHZYd0c", "E4a-ahM57QQ",
		"p5bM5PPufFY", "hi0veU3L2pU", "P14ATpXc4J8", "hwrHV6Pprk4", "w1+L-27nYas", "1VM0h1JrUfA", "iLKz4+ukLqk",
	};
	static_assert(std::size(shellcore) == 21);
	for (const char* nid: shellcore) {
		Check(Name, Ssl::ProductionImport(info, std::string(nid) + "#Q#Q", Loader::SymbolType::Func, query) == Ssl::ImportPolicy::Accepted &&
		            provider.Find(query), "actual ShellCore SSL import unavailable");
	}
	for (unsigned mutation = 0; mutation != 11; ++mutation) {
		Loader::DynamicInfo bad;
		bad.import_libs = info.import_libs; bad.import_modules = info.import_modules;
		auto type = Loader::SymbolType::Func;
		std::string raw = "hdpVEUDFW3s#Q#Q";
		switch (mutation) {
			case 0: bad.import_libs[0].name = "Unrelated"; break;
			case 1: bad.import_modules[0].name = "Unrelated"; break;
			case 2: ++bad.import_libs[0].version; break;
			case 3: bad.import_modules[0].version_major = 3; break;
			case 4: ++bad.import_modules[0].version_minor; break;
			case 5: bad.import_libs.push_back(bad.import_libs[0]); break;
			case 6: bad.import_modules.push_back(bad.import_modules[0]); break;
			case 7: bad.import_libs.clear(); break;
			case 8: bad.import_modules.clear(); break;
			case 9: type = Loader::SymbolType::Object; break;
			case 10: raw = "unknownSSL1#Q#Q"; break;
		}
		Check(Name, Ssl::ProductionImport(bad, raw, type, query) == Ssl::ImportPolicy::Rejected, "bad/ambiguous SSL import could fall back to HLE");
	}
	Check(Name, Ssl::ProductionImport(info, "hdpVEUDFW3s", Loader::SymbolType::Func, query) == Ssl::ImportPolicy::Rejected,
	            "NID-only SSL import accepted");
	Check(Name, Ssl::ProductionImport(info, "unrelated#X#X", Loader::SymbolType::Func, query) == Ssl::ImportPolicy::Unrelated,
	            "unrelated library intercepted");
	Loader::SymbolResolve alien {"hdpVEUDFW3s", "Other", 1, "Other", 2, 1, Loader::SymbolType::Func};
	provider.Add(alien, 0xdead);
	Check(Name, !provider.Find(alien) && provider.FindByNid(alien.name, alien.type)->vaddr != 0xdead,
	            "same NID from unrelated library replaced native SSL");
	Loader::SymbolDatabase conflicting;
	Loader::SymbolResolve native {"hdpVEUDFW3s", "libSceSsl", 1, "libSceSsl", 2, 1, Loader::SymbolType::Func};
	Check(Name, !Ssl::FindPublicExport(conflicting, 0x100000, native), "missing native export resolved");
	auto hle = native; hle.module_version_major = 1;
	conflicting.Add(hle, 0xdead, "HLE SSL");
	conflicting.Add(alien, 0xbeef, "Unrelated same NID");
	Check(Name, !Ssl::FindPublicExport(conflicting, 0x100000, native), "native miss fell back to HLE/unrelated NID");
	conflicting.Add(native, 0xdead);
	Check(Name, !Ssl::FindPublicExport(conflicting, 0x100000, native), "wrong provider address admitted");
	Check(Name, !Ssl::FindPublicExport(provider, UINT64_MAX, native), "wrapped provider address admitted");
}
} // namespace NativeSslProductionTest

void TestNativeSslProduction() {
	NativeSslProductionTest::ModesAndStartup();
	NativeSslProductionTest::ImportsAndExports();
}
