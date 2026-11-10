#include "loader/symbolDatabase.h"
#include "loader/npWorkQueueExports.h"
#include "loader/npEventQueueExports.h"
#include "loader/npCalloutExports.h"
#include "loader/npHttpPoolExports.h"
#include "loader/npAlternateStartup.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using Loader::SymbolDatabase;
using Loader::SymbolResolve;
using Loader::SymbolType;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SymbolDatabaseTests: failed: %s\n", message);
		std::abort();
	}
}

SymbolResolve Make(const std::string& name, const std::string& library, SymbolType type,
                   const std::string& module = "libKernel") {
	return SymbolResolve {name, library, 1, module, 1, 1, type};
}

void TestExactLastDuplicateWins() {
	SymbolDatabase db;
	auto           s = Make("nidA", "libKernel", SymbolType::Func);
	db.Add(s, 0x1000);
	db.Add(s, 0x2000, "second");
	const auto* r = db.Find(s);
	Check(r != nullptr && r->vaddr == 0x2000 && r->dbg_name == "second", "exact: last duplicate wins");
}

void TestFallbackFirstInsertionWins() {
	SymbolDatabase db;
	db.Add(Make("nidA", "libKernel", SymbolType::Func), 0x1000);
	db.Add(Make("nidA", "libOther", SymbolType::Func), 0x2000);
	db.Add(Make("nidA", "libKernel", SymbolType::Func), 0x3000);
	const auto* n = db.FindByNid("nidA", SymbolType::Func);
	Check(n != nullptr && n->vaddr == 0x1000, "nid fallback: first insertion wins");
	const auto* m = db.FindByName("nidA", SymbolType::Func);
	Check(m != nullptr && m->vaddr == 0x1000, "name fallback: first insertion wins");
	// Exact lookup still sees the later duplicate.
	Check(db.Find(Make("nidA", "libKernel", SymbolType::Func))->vaddr == 0x3000, "exact vs fallback");
}

void TestTypeSegregation() {
	SymbolDatabase db;
	db.Add(Make("sym", "libKernel", SymbolType::Object), 0x10);
	db.Add(Make("sym", "libKernel", SymbolType::Func), 0x20);
	Check(db.FindByNid("sym", SymbolType::Func)->vaddr == 0x20, "type: func");
	Check(db.FindByNid("sym", SymbolType::Object)->vaddr == 0x10, "type: object");
	Check(db.FindByName("sym", SymbolType::TlsModule) == nullptr, "type: absent type");
	Check(db.FindByNid("sym", SymbolType::NoType) == nullptr, "type: absent notype");
}

void TestAddressFilter() {
	SymbolDatabase db;
	db.Add(Make("nid", "libA", SymbolType::Func), 0x100);
	db.Add(Make("nid", "libB", SymbolType::Func), 0x200);
	db.Add(Make("nid", "libC", SymbolType::Func), 0x200, "dup-addr");
	Check(db.FindByNid("nid", SymbolType::Func, 0)->vaddr == 0x100, "vaddr 0 means any");
	const auto* r = db.FindByNid("nid", SymbolType::Func, 0x200);
	Check(r != nullptr && r->dbg_name.empty(), "vaddr filter: first matching address");
	Check(db.FindByNid("nid", SymbolType::Func, 0x300) == nullptr, "vaddr filter: no match");
}

void TestGrowthAndPrefixes() {
	SymbolDatabase db;
	Check(db.FindByNid("late", SymbolType::Func) == nullptr, "growth: initially absent");
	for (int i = 0; i < 5000; i++) {
		db.Add(Make("n" + std::to_string(i), "libKernel", SymbolType::Func), 0x1000 + i);
	}
	db.Add(Make("late", "libKernel", SymbolType::Func), 0x9999);
	Check(db.FindByNid("late", SymbolType::Func)->vaddr == 0x9999, "growth: later addition found");
	Check(db.FindByNid("n4999", SymbolType::Func)->vaddr == 0x1000 + 4999, "growth: last bulk");
	// Prefix must end before '[': "n1" must not match "n10".
	Check(db.FindByNid("n1", SymbolType::Func)->vaddr == 0x1001, "prefix boundary");
	// Queries containing '[' keep the old starts_with semantics.
	Check(db.FindByName("late[libKernel_v1]", SymbolType::Func) != nullptr, "bracketed query");
	Check(db.FindByName("late[libKern", SymbolType::Func) == nullptr, "partial bracket query");
}

void TestUnknown() {
	SymbolDatabase db;
	Check(db.FindByNid("x", SymbolType::Func) == nullptr, "empty db nid");
	Check(db.FindByName("x", SymbolType::Func) == nullptr, "empty db name");
	Check(db.Find(Make("x", "libKernel", SymbolType::Func)) == nullptr, "empty db exact");
	db.Add(Make("y", "libKernel", SymbolType::Func), 1);
	Check(db.FindByNid("x", SymbolType::Func) == nullptr, "unknown nid");
	Check(db.FindByName("", SymbolType::Func) == nullptr, "empty name");
	Check(db.Find(Make("y", "libOther", SymbolType::Func)) == nullptr, "unknown exact");
}

void TestIsolatedNpOwnership() {
	SymbolDatabase isolated(SymbolDatabase::ExportPolicy::NpOwnership);
	SymbolDatabase ordinary;
	for (const char* nid: {"V75N47uYdQc", "1x0jThSUr4w", "0gdlCVNNHCI", "VWcTu8wKwlQ", "c8-4aC9opYE"}) {
		auto symbol = Make(nid, "libSceNpCommon", SymbolType::Func, "libSceNpCommon");
		isolated.Add(symbol, 0x910001000);
		Check(isolated.Find(symbol) != nullptr && isolated.FindByNid(nid, SymbolType::Func) != nullptr &&
		      isolated.FindByName(nid, SymbolType::Func) != nullptr, "NP ownership: exact/NID/dlsym exposure");
		symbol.type = SymbolType::Object;
		isolated.Add(symbol, 0xdeadbeef, "wrong type");
		Check(isolated.Find(symbol) == nullptr, "NP ownership: function NID cannot expose an object");
	}
	// Actual unwanted firmware fallback edges: arch termination, heap creation,
	// mutex destructor and WorkQueue destructor. Exercise both insertion paths.
	for (const char* nid: {"UskWpVWxSvg", "B+yGIX1+BTI", "9zi9FTPol74", "1QFKnDJxk3A"}) {
		const auto symbol = Make(nid, "libSceNpCommon", SymbolType::Func, "libSceNpCommon");
		isolated.Add(symbol, 0x910002000);
		isolated.Add(symbol, 0x910003000, "excluded");
		ordinary.Add(symbol, 0x910004000);
		Check(isolated.Find(symbol) == nullptr && isolated.FindByNid(nid, SymbolType::Func) == nullptr &&
		      isolated.FindByName(nid, SymbolType::Func) == nullptr, "NP ownership: all lookup paths exclude unrelated exports");
		Check(ordinary.Find(symbol)->vaddr == 0x910004000, "NP ownership: ordinary databases unchanged");
	}
}

void CheckNpVisibility(const SymbolDatabase& db, const SymbolResolve& symbol, uint64_t address, bool visible) {
	const auto name = SymbolDatabase::GenerateName(symbol);
	const auto prefix = name.substr(0, name.rfind('['));
	for (const auto* record: {db.Find(symbol), db.FindByNid(symbol.name, symbol.type),
	                         db.FindByNid(symbol.name, symbol.type, address),
	                         db.FindByName(symbol.name, symbol.type), db.FindByName(prefix, symbol.type)}) {
		Check(visible ? record != nullptr && record->vaddr == address : record == nullptr,
		      "NP policies: exact/NID/address/name/dlsym lookup visibility");
	}
	// Prefixed provider metadata and normalized import metadata must agree.
	auto normalized = symbol;
	normalized.library = "NpCommon";
	normalized.module = "NpCommon";
	const auto* record = db.Find(normalized);
	Check(visible ? record != nullptr && record->vaddr == address : record == nullptr,
	      "NP policies: prefixed library/module metadata");
}

void TestResolvedRegistrationIdentity() {
	SymbolDatabase db;
	auto first = Make("sharedBody", "Posix", SymbolType::Func, "libkernel");
	auto second = first; second.library = "libkernel";
	db.Add(first, 0x12340, "Posix::shared_body");
	db.Add(second, 0x12340, "Posix::shared_body");
	const auto resolved = *db.Find(second);
	Check(db.FindRegistration("sharedBody", SymbolType::Func, resolved) != nullptr,
	      "exact resolved registration must not be hidden by an earlier same-address library alias");
	Check(!db.FindRegistration("otherNid", SymbolType::Func, resolved) &&
	      !db.FindRegistration("sharedBody", SymbolType::Object, resolved), "registration NID/type isolation");
	auto forged = resolved; forged.dbg_name = "Other::body";
	Check(!db.FindRegistration("sharedBody", SymbolType::Func, forged), "registration expression mismatch");
	forged = resolved; ++forged.vaddr;
	Check(!db.FindRegistration("sharedBody", SymbolType::Func, forged), "registration address mismatch");
	db.Add(second, 0x12340, "Posix::shared_body");
	Check(db.FindRegistration("sharedBody", SymbolType::Func, resolved) != nullptr, "identical registration repeat");
	db.Add(second, 0x56780, "Posix::shared_body");
	Check(!db.FindRegistration("sharedBody", SymbolType::Func, resolved) &&
	      !db.FindRegistration("sharedBody", SymbolType::Func, *db.Find(second)), "overwritten exact identity remains refused");
	Check(db.FindRegistration("sharedBody", SymbolType::Func, *db.Find(first)) != nullptr,
	      "conflicting library identity must not poison an independent alias");
}

void TestIsolatedNpWorkQueue() {
	// Independent fixtures: an omitted audited export must fail the test rather
	// than disappear from a loop over the implementation's own allowlist.
	constexpr const char* ownership_nids[] = {
	    "0syNkhJANVw", "V75N47uYdQc", "orRb69nSo64", "bKMVqRcCQ1U",
	    "05KEwpDf4Ls", "4il4PZAZOnQ", "iwDNdnEGyhI", "q2USyzLF4kI",
	    "CnDHI7sU+l0", "1x0jThSUr4w", "0gdlCVNNHCI",
	    "mzlILsFx0cU", "a2qdVU8RWb4", "VWcTu8wKwlQ", "kKF3w-XkCWA",
	    "BztTl7QeYqE", "Zh23aSLeeZo", "c8-4aC9opYE", "Cmd4+m7V00c",
	    "vqA9bl6WsF0", "EziLjfyTnKI",
	};
	constexpr const char* queue_nids[] = {
	    "X6NVkdpRnog", "U0YoWwgg8aI", "+dGO+GS2ZXQ", "VnQolo6vTr4",
	    "CznMfhTIvVY", "eTy3L1azX4E", "zmOmSLnqlBQ", "4DE+nnCVRPA",
	    "NeopmYshD0U", "1QFKnDJxk3A", "KQSxXJBepQ4", "laqZEULcfgw",
	    "WF2l-GUIlrw", "HldN461O2Dw", "ga4OW9MGahU", "Oq5aepLkEWg",
	    "JRtw5pROOiM", "417JucZaE3g", "zurkNUps5o8", "5tYi1l9CXD0",
	};
	static_assert(std::size(ownership_nids) == 21 && Loader::NpOwnershipExports.size() == 21);
	static_assert(std::size(queue_nids) == 20 && Loader::NpWorkQueueExports.size() == 20);
	constexpr const char* callout_nids[] = {
	    "dV+zK-Ce-2E", "oLpLfV2Ov9A", "C282U0P6Nwg", "XEXFdmQj5oI", "14PDhhMEBKY",
	    "3ErXia+y89M", "TDuC6To9HJ8", "r0PYNWZLZS8", "EQpGK69EaU8", "xxOTJpEyoj4",
	};
	static_assert(std::size(callout_nids) == 10 && Loader::NpCalloutExports.size() == 10);
	constexpr const char* http_pool_nids[] = {"PznfSvchYJ8", "Vzob5RCgfnY", "jCeUP0CpiNs"};
	static_assert(std::size(http_pool_nids) == 3 && Loader::NpHttpPoolExports.size() == 3);
	static_assert(std::size(ownership_nids) + std::size(queue_nids) + std::size(callout_nids) +
	              std::size(http_pool_nids) == 54);
	constexpr const char* excluded_nids[] = {
	    "kZizwrFvWZY", "dfXSH2Tsjkw", "FaMNvjMA6to", "xHAiSVEEjSI", // Per-heap lifecycle/descriptors.
	    "kwW5qddf+Lo", "wsfyvM+VbUk", "p1vvpKGRXe4", "B+yGIX1+BTI", // Heap allocation/creation.
	    "DI5n4aOdxmk", "P4YpPziLBd4", "jGF+MaB4b-M", "UskWpVWxSvg", // Global getters/Arch.
	    "fhJ5uKzcn0w", "EjMsfO3GCIA", // NP thread create/join.
	    "O1AvlQU33pI", "aTNOl9EB4V4", "Yc+qj4TIEY0", "9zi9FTPol74", // NP mutex.
	    "q2tsVO3lM4A", "bsjWg59A7aE", "1a+iY5YUJcI", // NP condition variable.
	    "pJlGhXEt5CU", // Global SFMT output remains outside the queue policy.
	    "pfJgSA4jO3M", // Standalone old-value increment uses its corrected HLE binding.
	    "Kiwv9r4IZCc", "qgxDBjorUxs", // HTTP connection creation.
	    "tsGVru3hCe8", "Aeu5wVKkF9w", "rGNm+FjIXKk", "Cnp77podkCU", // HTTP requests.
	    "1e2BNwI-XzE", // HTTP send request.
	    "unrelatedNpExport",
	};
	for (const bool debug_name: {false, true}) {
		SymbolDatabase queues(SymbolDatabase::ExportPolicy::NpWorkQueue);
		SymbolDatabase callouts(SymbolDatabase::ExportPolicy::NpCallout);
		SymbolDatabase http_pool(SymbolDatabase::ExportPolicy::NpHttpPool);
		SymbolDatabase dispatcher(SymbolDatabase::ExportPolicy::NpDispatcher);
		SymbolDatabase ownership(SymbolDatabase::ExportPolicy::NpOwnership);
		SymbolDatabase ordinary;
		Check(queues.GetExportPolicy() == SymbolDatabase::ExportPolicy::NpWorkQueue &&
		      callouts.GetExportPolicy() == SymbolDatabase::ExportPolicy::NpCallout &&
		      http_pool.GetExportPolicy() == SymbolDatabase::ExportPolicy::NpHttpPool &&
		      dispatcher.GetExportPolicy() == SymbolDatabase::ExportPolicy::NpDispatcher &&
		      ownership.GetExportPolicy() == SymbolDatabase::ExportPolicy::NpOwnership &&
		      ordinary.GetExportPolicy() == SymbolDatabase::ExportPolicy::All, "NP policies: explicit opt-in");
		uint64_t address = 0x910001000;
		auto exercise = [&](const char* nid, bool queue_visible, bool ownership_visible, bool callout_visible,
		                    bool http_pool_visible, bool dispatcher_only = false) {
			for (const auto type: {SymbolType::Func, SymbolType::Object, SymbolType::NoType,
			                      SymbolType::TlsModule, SymbolType::Unknown}) {
				const auto symbol = Make(nid, "libSceNpCommon", type, "libSceNpCommon");
				for (auto* db: {&queues, &ownership, &callouts, &http_pool, &dispatcher, &ordinary}) {
					if (debug_name) { db->Add(symbol, address, "native NP export"); }
					else { db->Add(symbol, address); }
				}
				CheckNpVisibility(queues, symbol, address, queue_visible && type == SymbolType::Func);
				CheckNpVisibility(ownership, symbol, address, ownership_visible && type == SymbolType::Func);
				CheckNpVisibility(callouts, symbol, address, callout_visible && type == SymbolType::Func);
				CheckNpVisibility(http_pool, symbol, address, http_pool_visible && type == SymbolType::Func);
				CheckNpVisibility(dispatcher, symbol, address,
				                  (http_pool_visible || dispatcher_only) && type == SymbolType::Func);
				CheckNpVisibility(ordinary, symbol, address, true);
				address += 0x10;
			}
		};
		for (const auto* nid: ownership_nids) { exercise(nid, true, true, true, true); }
		for (const auto* nid: queue_nids) { exercise(nid, true, false, true, true); }
		for (const auto* nid: callout_nids) { exercise(nid, false, false, true, true); }
		for (const auto* nid: http_pool_nids) { exercise(nid, false, false, false, true); }
		for (const auto* nid: excluded_nids) {
			const bool dispatcher_only = std::string_view(nid) == "fhJ5uKzcn0w" ||
			                             std::string_view(nid) == "EjMsfO3GCIA";
			exercise(nid, false, false, false, false, dispatcher_only);
		}
	}
}

void TestAlternateNpSelection() {
	using namespace Loader::NpAlternateStartup;
	for (unsigned mask = 0; mask < 16; ++mask) {
		std::array<std::string_view, 4> flags;
		for (unsigned i = 0; i < 4; ++i) { flags[i] = mask & (1u << i) ? "1" : "0"; }
		Check(Select("1", flags, "0") == (mask == 0 || mask == 15 ? Selection::Alternate54 : Selection::Invalid),
		      "alternate: only standalone or complete54 combination");
		Check(Select("1", flags, "1") == Selection::Invalid, "alternate: dispatcher always rejected");
		Check(Select("", flags, "0") == Selection::Legacy, "legacy selection preserved");
		Check(Select("", flags, "1", "1") == (mask == 15 ? Selection::Legacy : Selection::Invalid),
		      "regular72 requires whole ownership/worker closure");
		Check(Select("1", flags, "1", "1") == Selection::Invalid, "regular queue cannot widen alternate54");
		Check(Select("", flags, "0", "1") == Selection::Invalid, "regular72 requires dispatcher pair");
	}
	for (auto malformed: {"true", "01", " 1", "2"}) {
		Check(Select(malformed, {}, "") == Selection::Invalid, "malformed alternate rejected");
		Check(Select("1", {malformed, "", "", ""}, "") == Selection::Invalid, "malformed family rejected");
		Check(Select("1", {}, malformed) == Selection::Invalid, "malformed dispatcher rejected");
		Check(Select("", {"1", "1", "1", "1"}, "1", malformed) == Selection::Invalid, "malformed EventQueue rejected");
	}
	Check(Select("1", {}, "") == Selection::Alternate54, "standalone alternate accepted");
}

void TestNpEventQueuePolicy() {
	for (bool debug: {false, true}) {
		SymbolDatabase startup(SymbolDatabase::ExportPolicy::NpEventQueue);
		SymbolDatabase old(SymbolDatabase::ExportPolicy::NpDispatcher);
		uint64_t address = 0x1000;
		auto exercise = [&](const char* nid, bool accepted, bool old_accepted) {
			for (auto type: {SymbolType::Func, SymbolType::Object, SymbolType::NoType}) {
				const auto symbol = Make(nid, "libSceNpCommon", type, "libSceNpCommon");
				for (auto* db: {&startup, &old}) {
					if (debug) { db->Add(symbol, address, "queue policy fixture"); }
					else { db->Add(symbol, address); }
				}
				CheckNpVisibility(startup, symbol, address, accepted && type == SymbolType::Func);
				CheckNpVisibility(old, symbol, address, old_accepted && type == SymbolType::Func);
				address += 0x10;
			}
		};
		for (const auto nid: Loader::NpEventQueueExports) { exercise(nid.data(), true, false); }
		for (const auto nid: Loader::NpOwnershipExports) { exercise(nid.data(), true, true); }
		for (const auto nid: Loader::NpDispatcherExports) { exercise(nid.data(), true, true); }
		exercise("GRvK1ZE+FEQ", false, false); // registry query is not service readiness
		exercise("cMOgkE2M2e8", false, false); // independent event class remains HLE
	}
}

} // namespace

int main() {
	TestExactLastDuplicateWins();
	TestFallbackFirstInsertionWins();
	TestTypeSegregation();
	TestAddressFilter();
	TestGrowthAndPrefixes();
	TestUnknown();
	TestIsolatedNpOwnership();
	TestIsolatedNpWorkQueue();
	TestAlternateNpSelection();
	TestNpEventQueuePolicy();
	TestResolvedRegistrationIdentity();
	std::printf("SymbolDatabaseTests: passed\n");
	return 0;
}
