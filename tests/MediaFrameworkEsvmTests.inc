// Include inside VirtualMemoryAllocationTests' fixture namespace. Top-level
// includes: libs/mediaFrameworkEsvm.h, graphics/presentation/videoOutStatus.h, kernel/semaphore.h,
// loader/runtimeProcessIdentity.h. Dispatch TestMediaFrameworkEsvm() in its own
// process after InitSubsystems. Link mediaFrameworkEsvm.cpp with the existing
// kernel/SHM implementation; this is NOT a fake resource backend or app run.
static int mf_snapshot_pid = -1;
static int mf_snapshot_calls = 0;
static int mf_snapshot_action = 0;
static bool mf_snapshot_available = true;
static Libs::MediaFramework::EsvmPresentationSnapshot mf_snapshot;
static bool MediaFrameworkSnapshotFixture(int pid, Libs::MediaFramework::EsvmPresentationSnapshot* out) {
	using namespace Libs::MediaFramework;
	mf_snapshot_pid = pid;
	++mf_snapshot_calls;
	if (!mf_snapshot_available) { return false; }
	*out = mf_snapshot;
	// Reentry proves that neither framework lock is held across the callback.
	if (mf_snapshot_action == 1) { EsvmTerminate(); }
	if (mf_snapshot_action == 2) { EsvmUnregisterClient("citroncore"); }
	if (mf_snapshot_action == 3) { SetEsvmSnapshotReader(MediaFrameworkSnapshotFixture); }
	return true;
}

void TestMediaFrameworkEsvm() try {
	constexpr const char* test = "MediaFrameworkEsvm";
	using namespace Libs::MediaFramework;
	namespace Sem = Libs::LibKernel::Semaphore;
	namespace Fs = Libs::LibKernel::FileSystem;
	using Loader::RuntimeLinker;
	Fs::Initialize();
	struct FileSystemLifetime {
		~FileSystemLifetime() { Fs::Shutdown(); }
	} filesystem_lifetime;
	constexpr int invalid = static_cast<int>(0x81790001u);
	constexpr int full = static_cast<int>(0x81790007u);
	constexpr int not_initialized = static_cast<int>(0x8179000du);
	constexpr int failed = static_cast<int>(0x81790fffu);
	struct Restore {
		Loader::Program* previous = RuntimeLinker::GetCurrentProgram();
		~Restore() { SetEsvmSnapshotReader(nullptr); RuntimeLinker::SetCurrentProgram(previous); }
	} restore;
	RuntimeLinker::SetCurrentProgram(nullptr);
	Check(test, ReleaseEsvmOwner(100), "clear direct owner fixture");
	SetEsvmSnapshotReader(nullptr);
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	const auto before_semaphores = Sem::SemaResourcesForTest();
	const auto before_descriptors = Fs::TestAnonymousShmDescriptorCount();
	EsvmInit24 config {24, 0, 0x1fff, 700, 0};
	struct Guarded {
		uint64_t before;
		EsvmResult56 result;
		uint64_t after;
	};
	Guarded output;
	auto poison = [&] { std::memset(&output, 0xa5, sizeof(output)); };
	auto untouched = [&] {
		const auto* bytes = reinterpret_cast<const uint8_t*>(&output);
		return std::all_of(bytes, bytes + sizeof(output), [](auto b) { return b == 0xa5; });
	};
	auto no_resources = [&] {
		const auto r = EsvmResourcesForTest();
		Check(test, r.clients == 0 && r.mappings == 0 && r.semaphores == 0 && r.descriptors == 0,
		      "no retained client resources after rollback/teardown");
		const auto actual = Sem::SemaResourcesForTest();
		Check(test, actual.handles == before_semaphores.handles && actual.names == before_semaphores.names &&
		            actual.objects == before_semaphores.objects && Fs::TestAnonymousShmDescriptorCount() == before_descriptors,
		      "real kernel semaphore and anonymous-SHM descriptor registries returned to baseline");
	};
	poison();
	Check(test, EsvmTerminate() == 0 && EsvmTerminate() == 0, "terminate before init is idempotent");
	Check(test, EsvmInitialize(nullptr, false) == invalid && EsvmInitialize(&config, true) == invalid,
	      "init validates null and second byte before state");
	Check(test, EsvmInitialize(reinterpret_cast<const EsvmInit24*>(uintptr_t {1}), false) == invalid,
	      "bad initialization pointer does not escape to guest-fault handler");
	Check(test, EsvmRegisterClient(nullptr) == invalid && EsvmUnregisterClient(nullptr) == invalid &&
	            EsvmGetStatus(1, nullptr) == invalid, "null argument precedence");
	Check(test, EsvmRegisterClient("citroncore") == not_initialized &&
	            EsvmUnregisterClient("citroncore") == not_initialized &&
	            EsvmGetStatus(1, &output.result) == not_initialized && untouched(), "uninitialized gate preserves output");
	Check(test, EsvmInitialize(&config, false) == 0 && EsvmInitialize(&config, false) == 0,
	      "framework init is idempotent, distinct from native Esvm init");
	EsvmInit24 unusual {0xdeadbeef, 0xffffffff, 0, -1, 0xffffffff};
	Check(test, EsvmInitialize(&unusual, false) == 0, "do not invent client size/reserved validation");
	Check(test, EsvmInitialize(&config, false) == 0, "restore ordinary native request");
	Check(test, EsvmGetStatus(1, reinterpret_cast<EsvmResult56*>(uintptr_t {1})) == invalid &&
	            EsvmRegisterClient(reinterpret_cast<const char*>(uintptr_t {1})) == invalid,
	      "invalid status/name addresses do not publish results or resources");
	std::array<char, 65> long_name;
	long_name.fill('x'); long_name.back() = '\0';
	Check(test, EsvmRegisterClient("") == invalid && EsvmRegisterClient("other") == invalid &&
	            EsvmRegisterClient(long_name.data()) == invalid, "register admits only exact citroncore");
	Check(test, EsvmUnregisterClient("") == 0 && EsvmUnregisterClient("other") == 0,
	      "missing PID registration/name is not an error");
	Check(test, EsvmUnregisterClient(long_name.data()) == invalid,
	      "rejected unregister send leaves zero request ID and returns invalid argument");
	poison();
	Check(test, EsvmGetStatus(1, &output.result) == failed && untouched(), "no presentation provider fails closed");
	Check(test, EsvmGetStatus(UINT32_MAX, &output.result) == 0, "invalid selector completes without a provider");
	EsvmResult56 zero {};
	Check(test, std::memcmp(&output.result, &zero, sizeof(zero)) == 0 &&
	            output.before == UINT64_C(0xa5a5a5a5a5a5a5a5) && output.after == output.before,
	      "invalid selector returns exactly 56 zero bytes with canaries intact");
	// Exercise the actual production adapter before any presentation startup.
	SetEsvmSnapshotReader(ReadEsvmGraphicsSnapshot);
	Check(test, EsvmGetStatus(1, &output.result) == 0 && (output.result.payload[0] & 0x21) == 0x21,
	      "real absent VideoOut snapshot is disconnected/unusable, never all-zero usable flags");
	for (bool connected: {false, true}) {
		for (bool usable: {false, true}) {
			Libs::VideoOut::VideoOutOutputSnapshot source {};
			source.connected = connected;
			source.usable = usable;
			source.resolution = 2;
			source.dynamic_range = 1;
			source.refresh_rate = 13;
			const auto encoded = EncodeEsvmPresentationSnapshot(source);
			Check(test, Libs::VideoOut::VideoOutPortStatusIsDeviceConnected(encoded.port_status.data()) == int(connected) &&
			            Libs::VideoOut::VideoOutPortStatusIsUsable(encoded.port_status.data()) == int(usable) &&
			            Libs::VideoOut::VideoOutOutputStatusIsHdcp14Enabled(encoded.output_status.data()) == 0 &&
			            Libs::VideoOut::VideoOutOutputStatusIsHdcp23Enabled(encoded.output_status.data()) == 0,
			      "bridge encodes independent connection/usability and does not create HDCP support");
			uint32_t resolution = 0, range = 0;
			uint64_t refresh = 0;
			std::memcpy(&resolution, encoded.output_status.data(), 4);
			std::memcpy(&range, encoded.output_status.data() + 4, 4);
			std::memcpy(&refresh, encoded.output_status.data() + 8, 8);
			Check(test, resolution == 2 && range == 1 && refresh == 13, "output payload preserves guest mode fields");
		}
	}
	SetEsvmSnapshotReader(nullptr);

	for (auto point: {EsvmResourceFailure::ShmOpen, EsvmResourceFailure::ShmResize,
	                  EsvmResourceFailure::ShmMap, EsvmResourceFailure::FirstSemaphore,
	                  EsvmResourceFailure::SecondSemaphore, EsvmResourceFailure::ReceiverSetup}) {
		EsvmFailNextResourceForTest(point);
		Check(test, EsvmRegisterClient("citroncore") == failed, "resource failure is not client admission");
		no_resources();
		Check(test, EsvmRegisterClient("citroncore") == 0, "retry acquires actual resources");
		const auto resources = EsvmResourcesForTest();
		Check(test, resources.clients == 1 && resources.mappings == 1 && resources.semaphores == 2 && resources.descriptors == 0,
		      "one shared mapping and two semaphores; mapping retains closed descriptor backing");
		Check(test, EsvmUnregisterClient("not-the-registered-name") == 0, "unregister is PID-keyed, not name-keyed");
		no_resources();
	}
	Check(test, EsvmRegisterClient("citroncore") == 0 && EsvmRegisterClient("citroncore") == 0,
	      "PID replacement recreates without an already-registered error");
	const auto replaced = Sem::SemaResourcesForTest();
	Check(test, replaced.handles == before_semaphores.handles + 2 && replaced.names == before_semaphores.names + 2 &&
	            replaced.objects == before_semaphores.objects + 2, "replacement retires old real semaphores");
	EsvmFailNextResourceForTest(EsvmResourceFailure::SecondSemaphore);
	Check(test, EsvmRegisterClient("citroncore") == failed, "failed replacement does not retain the prior admission");
	no_resources();
	Check(test, EsvmRegisterClient("citroncore") == 0, "recreate direct client for status");
	mf_snapshot = {};
	// Explicit CPU fixtures, not claims about actual display/HDCP support.
	for (size_t index = 0; index < mf_snapshot.output_status.size(); ++index) {
		mf_snapshot.output_status[index] = static_cast<uint8_t>(index + 1);
	}
	for (size_t index = 0; index < mf_snapshot.port_status.size(); ++index) {
		mf_snapshot.port_status[index] = static_cast<uint8_t>(0xe0 + index);
	}
	mf_snapshot_available = true;
	mf_snapshot_action = 0;
	mf_snapshot_calls = 0;
	SetEsvmSnapshotReader(MediaFrameworkSnapshotFixture);
	for (uint32_t selector: {0u, 1u}) {
		poison();
		Check(test, EsvmGetStatus(selector, &output.result) == 0 && mf_snapshot_pid == 100 &&
		            output.result.result == 0 && output.result.reserved == 0, "coherent direct-owner snapshot");
		std::array<uint8_t, 48> expected {};
		if (selector == 0) { expected = mf_snapshot.output_status; }
		else { std::copy(mf_snapshot.port_status.begin(), mf_snapshot.port_status.end(), expected.begin()); }
		Check(test, output.result.payload == expected && output.before == UINT64_C(0xa5a5a5a5a5a5a5a5) &&
		            output.after == output.before, "selector 0 copies 48, selector 1 copies 32 then 16 zero bytes");
	}
	mf_snapshot.port_result = invalid;
	Check(test, EsvmGetStatus(1, &output.result) == 0 && output.result.result == invalid,
	      "embedded device error is separate from successful transport completion");
	mf_snapshot.port_result = 0;
	mf_snapshot_available = false;
	poison();
	Check(test, EsvmGetStatus(0, &output.result) == failed && untouched(), "failed snapshot preserves caller bytes");
	mf_snapshot_available = true;
	const auto calls = mf_snapshot_calls;
	Check(test, EsvmGetStatus(2, &output.result) == 0 && mf_snapshot_calls == calls &&
	            std::memcmp(&output.result, &zero, sizeof(zero)) == 0, "unknown selector never queries graphics");
	for (int action: {2, 3, 1}) {
		mf_snapshot_action = action;
		poison();
		const int code = EsvmGetStatus(1, &output.result);
		Check(test, code == (action == 1 ? not_initialized : failed) && untouched(),
		      "reentrant unregister/provider replacement/terminate invalidates stale snapshot without deadlock");
	}
	mf_snapshot_action = 0;
	no_resources();
	Check(test, EsvmTerminate() == 0, "terminate after callback retirement remains idempotent");

	// Resident runtime owners, matching the process-identity fixture contract.
	std::array<Loader::Program*, 11> owners {};
	std::array<int, 11> pids {};
	for (size_t index = 0; index < owners.size(); ++index) {
		owners[index] = new Loader::Program;
		owners[index]->unique_id = static_cast<int>(792000 + index);
		owners[index]->application_owner = owners[index];
		pids[index] = Loader::RegisterRuntimeProcessIdentity(owners[index], static_cast<uint32_t>(792000 + index));
		Check(test, pids[index] > 100, "runtime fixture has explicit PID");
		RuntimeLinker::SetCurrentProgram(owners[index]);
		Check(test, EsvmInitialize(&config, false) == 0, "owner-specific session initialized");
		Check(test, EsvmRegisterClient("citroncore") == (index < 10 ? 0 : full), "server capacity is ten PID resources");
	}
	Loader::Program private_module;
	private_module.application_owner = owners[0];
	RuntimeLinker::SetCurrentProgram(&private_module);
	Check(test, EsvmRegisterClient("citroncore") == 0 && EsvmResourcesForTest().clients == 10,
	      "private module replaces same PID even at capacity");
	Check(test, EsvmGetStatus(1, &output.result) == 0 && mf_snapshot_pid == pids[0], "snapshot receives application PID");
	Check(test, EsvmTerminate() == 0 && EsvmResourcesForTest().clients == 9, "termination only releases calling owner");
	RuntimeLinker::SetCurrentProgram(owners[10]);
	Check(test, EsvmRegisterClient("citroncore") == 0, "freed PID slot is reusable");
	Loader::Program unknown;
	unknown.application_owner = &unknown;
	RuntimeLinker::SetCurrentProgram(&unknown);
	poison();
	Check(test, EsvmInitialize(&config, false) == failed && EsvmRegisterClient("citroncore") == failed &&
	            EsvmUnregisterClient("citroncore") == failed && EsvmTerminate() == failed &&
	            EsvmGetStatus(1, &output.result) == failed && untouched() && EsvmResourcesForTest().clients == 10,
	      "unknown runtime owner cannot borrow PID 100 or another session");
	Check(test, Loader::DeactivateRuntimeProcessIdentity(owners[1]), "retire owner with no guest threads");
	RuntimeLinker::SetCurrentProgram(owners[10]);
	Check(test, EsvmGetStatus(1, &output.result) == 0 && EsvmResourcesForTest().clients == 9,
	      "inactive owner resources reaped on next entry");
	for (size_t index = 0; index < owners.size(); ++index) {
		Check(test, ReleaseEsvmOwner(pids[index]), "explicit owner teardown hook");
		Check(test, Loader::DeactivateRuntimeProcessIdentity(owners[index]), "retire runtime fixture");
	}
	RuntimeLinker::SetCurrentProgram(nullptr);
	no_resources();
	Check(test, EsvmResourcesForTest().sessions == 0, "all owner bookkeeping retired");
	Check(test, EsvmInitialize(&config, false) == 0 && EsvmRegisterClient("citroncore") == 0,
	      "direct owner has live resources at host shutdown");
	Check(test, ShutdownEsvmService() && ShutdownEsvmService(), "host shutdown is retryable and idempotent");
	no_resources();
	Check(test, EsvmInitialize(&config, false) == failed && EsvmRegisterClient("citroncore") == not_initialized,
	      "late guest calls cannot recreate resources after host shutdown");
	Check(test, Common::GetHostLockCriticalCounter()->load() == depth, "critical guards balanced");
} catch (const std::exception& error) {
	Check("MediaFrameworkEsvm", false, error.what());
}
