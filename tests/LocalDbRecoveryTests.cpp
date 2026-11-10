#include "loader/runtimeLocalDbRecovery.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace {
using namespace Loader::LocalDbRecovery;
void Check(bool ok, const char* why) {
	if (!ok) { std::fprintf(stderr, "LocalDbRecovery: %s\n", why); std::abort(); }
}
std::string Bytes(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	Check(in.good(), "read fixture");
	return {std::istreambuf_iterator<char>(in), {}};
}
void Write(const std::filesystem::path& path, const std::string& bytes) {
	std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size());
	Check(out.good(), "write fixture");
}
}

void RunLocalDbRecoveryTests(const std::filesystem::path& root, const std::filesystem::path& copied_db) {
	using namespace Loader::LocalDbRecovery;
	const auto db = root / "recovery-observation.db";
	const auto marker = root / "recovery-observation.db-corrupt";
	Write(db, "input identity only; report checking does not validate SQLite");
	const auto original_db = Bytes(db);
	EagerCheckPass pass(db);
	Check(pass.Get(1, 4).status == LookupStatus::Unavailable, "getter created result");
	Check(pass.Get(0, 15).status == LookupStatus::InvalidKey && pass.Get(1, 6).status == LookupStatus::InvalidKey &&
	      pass.Get(2, 0).status == LookupStatus::InvalidKey, "invalid key admitted");
	auto result = pass.CheckForceRebuildAppInfo();
	Check(result.status == CheckStatus::Observed && result.is_corrupt == false && !result.report.present,
	      "absent report is not a completed negative force observation");
	// Native-derived truth table: Force uses presence && !secondary, whereas
	// the shared check uses primary || secondary. In particular magic00 differs.
	struct Case { std::string bytes; ReportEncoding encoding; bool primary, secondary, force; };
	const Case cases[] = {
		{std::string("\x00\x00\xe1\xfe", 4), ReportEncoding::Recognized, false, false, true},
		{std::string("\x01\x00\xe1\xfe", 4), ReportEncoding::Recognized, true, false, true},
		{std::string("\x02\x00\xe1\xfe", 4), ReportEncoding::Recognized, false, true, false},
		{"", ReportEncoding::Short, true, false, true},
		{std::string("\x02\x00\xe1", 3), ReportEncoding::Short, true, false, true},
		{"other", ReportEncoding::Unknown, true, false, true},
		{std::string("\x02\x00\xe1\xfe", 4) + "trailing bytes", ReportEncoding::Recognized, false, true, false},
	};
	for (const auto& c: cases) {
		Write(marker, c.bytes);
		result = pass.CheckForceRebuildAppInfo();
		Check(result.status == CheckStatus::Observed && result.report.present && result.report.encoding == c.encoding &&
		      result.report.primary == c.primary && result.report.secondary == c.secondary && result.is_corrupt == c.force,
		      "native report/callback truth table differs");
		const auto retained = pass.Get(1, 4);
		Check(retained.status == LookupStatus::Found && retained.observation->is_corrupt == c.force,
		      "current callback observation not retained");
		Check(Bytes(marker) == c.bytes && Bytes(db) == original_db, "observation changed inputs");
		Check(pass.Get(1, 0).status == LookupStatus::Unavailable && pass.Get(0, 10).status == LookupStatus::Unavailable &&
		      pass.Get(1, 5).status == LookupStatus::Unavailable, "force result promoted to dependent operation");
	}
	// An unsupported file kind must replace the last successful observation with
	// a host failure, not retain a stale clean bit or synthesize guest errno.
	std::filesystem::remove(marker); std::filesystem::create_directory(marker);
	result = pass.CheckForceRebuildAppInfo();
	Check(result.status == CheckStatus::IoError && !result.is_corrupt && result.host_error,
	      "unreadable report presented as clean");
	Check(pass.Get(1, 4).observation->status == CheckStatus::IoError && !pass.Get(1, 4).observation->is_corrupt,
	      "failed recheck retained stale success");
	std::filesystem::remove(marker);
	EagerCheckPass missing(root / "missing-source.db");
	Check(missing.CheckForceRebuildAppInfo().status == CheckStatus::InputError, "missing input inferred clean");
	EagerCheckPass next_pass(db);
	Check(next_pass.Get(1, 4).status == LookupStatus::Unavailable, "new pass inherited results");
	std::thread worker([&] { for (int i = 0; i != 100; ++i) { pass.CheckForceRebuildAppInfo(); } });
	pass.Stop(); worker.join();
	Check(pass.Get(1, 4).status == LookupStatus::Unavailable && pass.CheckForceRebuildAppInfo().status == CheckStatus::Cancelled,
	      "terminal Stop allowed stale or late result publication");
	Check(next_pass.CheckForceRebuildAppInfo().status == CheckStatus::Observed, "Stop affected independent pass");
	if (!copied_db.empty()) {
		const auto source_marker = std::filesystem::path(copied_db.native() + std::filesystem::path("-corrupt").native());
		const auto before = Bytes(source_marker);
		EagerCheckPass actual(copied_db);
		result = actual.CheckForceRebuildAppInfo();
		Check(result.status == CheckStatus::Observed && result.is_corrupt == true && result.report.primary &&
		      !result.report.secondary && result.report.encoding == ReportEncoding::Recognized,
		      "copied native marker did not request force rebuild");
		Check(Bytes(source_marker) == before, "actual copied marker changed");
	}
	std::puts("LocalDbRecovery: native report truth table, force-only retention, error replacement, pass isolation and Stop passed");
}
