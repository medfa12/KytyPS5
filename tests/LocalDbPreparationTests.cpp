#include "loader/runtimeLocalDbPreparation.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
using namespace Loader::LocalDbRecovery;
void Check(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "DbPreparation: %s\n", why); std::abort(); } }
void Write(const std::filesystem::path& path, const std::string& bytes) {
	std::ofstream out(path, std::ios::binary); out.write(bytes.data(), bytes.size()); out.close(); Check(out.good(), "write fixture");
}
std::string Read(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary); Check(input.good(), "read fixture"); return {std::istreambuf_iterator<char>(input), {}};
}
}
void RunLocalDbPreparationTests(const std::filesystem::path& root, const std::filesystem::path& copied_db) {
	using namespace Loader::LocalDbRecovery;
	const auto db = root / "wrapper-app.db", marker = root / "wrapper-app.db-corrupt", force_source = root / "copied-force-report";
	if (copied_db.empty()) { Write(db, "report fixture; not SQLite readiness"); }
	else { std::filesystem::copy_file(copied_db, db); }
	const std::string corruption("\x01\x00\xe1\xfe", 4);
	Write(marker, corruption); Write(force_source, "retained distinct report bytes");
	const auto original_db = Read(db), original_force = Read(force_source);
	PreparationPass pass({db, force_source, root / "force-wrapper-private"});
	std::array<uint8_t, 12> wire; wire.fill(0xa5); const auto untouched = wire;
	Check(pass.ReadEagerWire({1, 4}, wire) == EagerWireStatus::Unavailable && wire == untouched, "getter created wrapper before execution");
	Check(pass.CheckForceRebuildAppInfo().status == WrapperStatus::Unavailable, "check bypassed initialization");
	Check(pass.EstimateForceRebuildAppInfo().status == WrapperStatus::Unavailable && pass.ReadEstimateWire({1, 4}, wire) == EagerWireStatus::Unavailable && wire == untouched,
	      "Estimate exists before selected Eager operation");
	const auto initialized = pass.Initialize();
	Check(initialized.status == WrapperStatus::Completed && initialized.source_report_present, "private force report snapshot failed");
	const auto private_report = initialized.retained_directory / "force_rebuild_appinfo-corrupt";
	Check(Read(private_report) == original_force && Read(initialized.retained_directory / "force-report.input") == original_force,
	      "snapshot omitted exact mutable/archive report bytes");
	auto result = pass.CheckForceRebuildAppInfo();
	Check(result.status == WrapperStatus::Completed && result.eager && result.eager->callback_status == 0 && result.eager->is_corrupt &&
	      result.eager->promote_type == 0 && result.extracted_promotion == 0 && result.post_check == PostCheckStatus::ReportRemoved,
	      "Force wrapper did not combine callback, promotion extraction and distinct post-check");
	Check(!std::filesystem::exists(private_report) && Read(marker) == corruption && Read(force_source) == original_force && Read(db) == original_db,
	      "Force post-check deleted/modified source or AppBrowse report");
	Check(pass.ReadEagerWire({1, 4}, wire) == EagerWireStatus::Found && wire == std::array<uint8_t, 12> {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
	      "native12-byte Eager result layout/promotion/padding differs");
	const auto estimate = pass.EstimateForceRebuildAppInfo();
	Check(estimate.status == WrapperStatus::Completed && estimate.result && estimate.result->callback_status == 0 &&
	      estimate.result->value1 == 1 && estimate.result->value2 == 1 && pass.ReadEstimateWire({1, 4}, wire) == EagerWireStatus::Found &&
	      wire == std::array<uint8_t, 12> {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, "Force Estimate callback/wrapper layout differs");
	result = pass.CheckForceRebuildAppInfo();
	wire.fill(0xa5);
	Check(pass.ReadEstimateWire({1, 4}, wire) == EagerWireStatus::Unavailable && wire == untouched, "new Eager check retained stale Estimate");
	Check(result.eager && result.eager->is_corrupt && result.eager->promote_type == 0 && result.post_check == PostCheckStatus::ReportAbsent,
	      "reused Force object changed promotion or confused post-report absence with app report absence");
	// Initialize is idempotent for the same pass and must not recreate a removed
	// marker or reset a finished operation from source bytes.
	Check(pass.Initialize().status == WrapperStatus::Completed && !std::filesystem::exists(private_report), "repeat initialization replayed force report");
	const auto aggregate = pass.RunMode1();
	const OperationKey expected[] {{1, 4}, {0, 10}, {1, 5}, {1, 0}, {0, 1}, {1, 1}};
	Check(aggregate.operations.size() == 6 && aggregate.recovery_requested && !aggregate.native_producer_registry_available &&
	      aggregate.status == PreparationStatus::PrerequisitesUnavailable && aggregate.first_blocking_operation == OperationKey {0, 10},
	      "partial Force wrapper became complete preparation");
	for (size_t i = 0; i != 6; ++i) {
		Check(aggregate.operations[i].key == expected[i], "native mode1/filter0 execution order changed");
		if (i) { Check(aggregate.operations[i].status == WrapperStatus::Unavailable && !aggregate.operations[i].eager, "missing operation fabricated result"); }
	}
	wire.fill(0xa5);
	Check(pass.ReadEagerWire({1, 0}, wire) == EagerWireStatus::Unavailable && wire == untouched, "Force result served as AppBrowse");
	Check(pass.ReadEagerWire({0, 15}, wire) == EagerWireStatus::InvalidRequest && wire == untouched, "invalid recovery key wrote output");
	Check(pass.ReadEagerWire({1, 4}, std::span<uint8_t>(wire).first(11)) == EagerWireStatus::InvalidRequest && wire == untouched, "short wire buffer accepted");
	// Callback failure still extracts promotion and executes post-check, but has
	// no fabricated native errno/result. It replaces the previous completed slot.
	std::filesystem::remove(marker); std::filesystem::create_directory(marker); Write(private_report, "remove even after failed callback");
	result = pass.CheckForceRebuildAppInfo();
	Check(result.status == WrapperStatus::IoError && !result.eager && result.extracted_promotion == 0 &&
	      result.post_check == PostCheckStatus::ReportRemoved && !std::filesystem::exists(private_report), "callback failure skipped native wrapper tail");
	Check(pass.ReadEagerWire({1, 4}, wire) == EagerWireStatus::OperationFailed && wire == untouched, "failed recheck retained stale native success");
	std::filesystem::remove(marker); Write(marker, corruption);
	std::filesystem::create_directory(private_report);
	result = pass.CheckForceRebuildAppInfo();
	Check(result.status == WrapperStatus::IoError && result.observation.is_corrupt == true && !result.eager && result.post_check == PostCheckStatus::Failed,
	      "host post-hook failure certified complete wrapper");
	std::filesystem::remove(private_report);
	// Magic02 still has corruption evidence but Force callback is false; promotion
	// stays0 since this operation does not copy the report's secondary flag into
	// its object state (the shared Eager callback is a different implementation).
	Write(marker, std::string("\x02\x00\xe1\xfe", 4));
	result = pass.CheckForceRebuildAppInfo();
	Check(result.eager && !result.eager->is_corrupt && result.eager->promote_type == 0, "report secondary was substituted for object promotion state");
	Check(pass.EstimateForceRebuildAppInfo().status == WrapperStatus::Unavailable, "clean Force callback scheduled Estimate");
	Write(marker, corruption);
	PreparationPass other({db, root / "absent-force-report", root / "other-force-private"});
	Check(other.Initialize().status == WrapperStatus::Completed && other.CheckForceRebuildAppInfo().post_check == PostCheckStatus::ReportAbsent,
	      "known absent force report rejected");
	PreparationPass unknown({db, {}, root / "unknown-force-private"});
	Check(unknown.Initialize().status != WrapperStatus::Completed && !std::filesystem::exists(root / "unknown-force-private"), "unknown force path invented absence");
	PreparationPass alias({db, marker, root / "alias-force-private"});
	Check(alias.Initialize().status == WrapperStatus::InputError && !std::filesystem::exists(root / "alias-force-private"), "AppBrowse report substituted for distinct force report source");
	PreparationPass collision({db, force_source, initialized.retained_directory});
	Check(collision.Initialize().status != WrapperStatus::Completed && Read(initialized.retained_directory / "force-report.input") == original_force,
	      "existing private workspace overwritten");
	std::thread worker([&] { for (int i = 0; i != 100; ++i) { pass.CheckForceRebuildAppInfo(); } });
	pass.Stop(); worker.join();
	Check(pass.ReadEagerWire({1, 4}, wire) == EagerWireStatus::Cancelled && wire == untouched &&
	      pass.CheckForceRebuildAppInfo().status == WrapperStatus::Cancelled && pass.Initialize().status == WrapperStatus::Cancelled,
	      "terminal Stop allowed publication/reinitialization");
	Check(other.CheckForceRebuildAppInfo().status == WrapperStatus::Completed, "Stop contaminated another pass");
	Check(Read(marker) == corruption && Read(force_source) == original_force && Read(db) == original_db, "wrapper tests changed preserved inputs");

	std::atomic<bool> cancelled {false};
	std::array<uint8_t, 16> notification {}; notification[0] = 1; notification[4] = 4; notification[6] = 1; notification[7] = 0x10; notification[8] = 1; notification[12] = 4;
	int calls = 0;
	std::vector<PreparationProducerBinding> selected {
		{"fixture-first", -41, [&](const auto& n) -> std::optional<int32_t> { Check(n == notification, "notification/user bytes rewritten"); ++calls; return 7; }},
		{"fixture-second", -42, [&](const auto&) -> std::optional<int32_t> { ++calls; return -99; }},
		{"fixture-third", -43, [&](const auto&) -> std::optional<int32_t> { ++calls; return 0; }}
	};
	auto producer = RunSelectedPreparationProducers(notification, selected, cancelled);
	Check(calls == 3 && producer.status == ProducerRunStatus::ProducerFailed && producer.accumulated_descriptor_error == -41 &&
	      producer.calls[0].returned_status == 7 && producer.calls[1].returned_status == -99, "first descriptor failure accumulation or continue-on-failure differs");
	selected[0].descriptor_failure_code = 0;
	producer = RunSelectedPreparationProducers(notification, selected, cancelled);
	Check(producer.accumulated_descriptor_error == -42, "native zero descriptor field incorrectly latched forever");
	selected[1].invoke = {};
	producer = RunSelectedPreparationProducers(notification, selected, cancelled);
	Check(producer.status == ProducerRunStatus::Unavailable && !producer.calls[1].returned_status, "absent native producer acknowledged");
	Check(RunSelectedPreparationProducers(notification, {}, cancelled).status == ProducerRunStatus::Unavailable, "empty registry certified readiness");
	selected = {{"fixture-success", -1, [](const auto&) -> std::optional<int32_t> { return 0; }}};
	Check(RunSelectedPreparationProducers(notification, selected, cancelled).status == ProducerRunStatus::SelectedListComplete, "complete selected list failed");
	selected[0].invoke = [&](const auto&) -> std::optional<int32_t> { cancelled.store(true); return 0; };
	Check(RunSelectedPreparationProducers(notification, selected, cancelled).status == ProducerRunStatus::Cancelled, "late callback cancellation reported complete");
	cancelled.store(false); notification.fill(0xff);
	Check(RunSelectedPreparationProducers(notification, selected, cancelled).status == ProducerRunStatus::InvalidNotification, "native invalid notification accepted");
	std::puts("DbPreparation: complete Force Eager wrapper, private post-report lifecycle, native wire layout, unavailable operations, selected producer aggregation and Stop passed");
}
