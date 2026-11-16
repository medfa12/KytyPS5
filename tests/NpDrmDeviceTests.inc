// Included inside the memory-test fixture namespace. The parent includes
// kernel/npDrmDevice.h at top level. These callbacks never access the filesystem.
namespace NpDrmDeviceTest {
namespace Drm = Libs::LibKernel::NpDrm;
constexpr const char* Name = "NpDrmDevice";

class RecordingPersistence final: public Drm::Persistence {
public:
	std::atomic<unsigned> calls {0};
	std::atomic<unsigned> active {0};
	std::atomic<unsigned> last_depth {0};
	std::atomic<bool> overlap {false};
	std::atomic<bool> nonempty {false};
	std::atomic<bool> unprotected {false};
	int error = 0; // Changed only while no call is in progress.
	int WriteActivation(std::span<const uint8_t> bytes) noexcept override {
		if (active.fetch_add(1) != 0) { overlap.store(true); }
		if (!bytes.empty()) { nonempty.store(true); }
		const auto depth = Common::GetHostLockCriticalCounter()->load();
		last_depth.store(depth);
		if (depth == 0) { unprotected.store(true); }
		calls.fetch_add(1);
		std::this_thread::yield(); // Host scheduling only, never guest execution.
		active.fetch_sub(1);
		return error;
	}
};

struct Payload {
	std::array<uint8_t, 64> storage;
	Payload() { storage.fill(0xa5); }
	std::span<uint8_t> Bytes(size_t size) { return std::span<uint8_t>(storage).subspan(4, size); }
	void CheckCanaries(size_t size) const {
		Check(Name, std::all_of(storage.begin(), storage.begin() + 4, [](uint8_t b) { return b == 0xa5; }) &&
		            std::all_of(storage.begin() + 4 + size, storage.end(), [](uint8_t b) { return b == 0xa5; }),
		      "payload write crossed its exact boundary");
	}
};

void CheckResult(Drm::Result result, Drm::ResultKind kind, std::optional<int> error) {
	Check(Name, result.kind == kind && result.transport_errno == error, "wrong core disposition/native errno");
}

void CheckWire(const Payload& payload, std::span<const uint8_t> expected) {
	Check(Name, std::equal(expected.begin(), expected.end(), payload.storage.begin() + 4), "wrong output bytes");
	payload.CheckCanaries(expected.size());
}

} // namespace NpDrmDeviceTest

void TestNpDrmDevice() {
	using namespace NpDrmDeviceTest;
	RecordingPersistence persistence;
	Drm::Device device(persistence);
	const auto depth = Common::GetHostLockCriticalCounter()->load();
	Check(Name, device.Snapshot() == Drm::State {}, "initial unavailable-backend state");
	Check(Name, device.Snapshot().activation.size() == 16384, "activation storage must be a real 16 KiB buffer");

	for (Drm::Authority authority: {Drm::Authority {}, Drm::Authority {0}, Drm::Authority {0x480000000000000eull}}) {
		Check(Name, device.Open(authority) == 1 && device.Close(authority) == 1, "unknown/foreign authority opened device");
	}
	for (uint64_t authority: {Drm::DriverAuthority, Drm::ShellUiAuthority}) {
		Check(Name, device.Open(authority) == 0 && device.Close(authority) == 0, "audited open/close authority rejected");
	}
	for (Drm::Authority authority: {Drm::Authority {}, Drm::Authority {0}, Drm::Authority {Drm::ShellUiAuthority},
	                               Drm::Authority {0x480000000000000eull}}) {
		for (uint32_t request: {Drm::Initialize, Drm::Terminate, Drm::QueryState, Drm::QueryStateHash,
		                       Drm::AuditedUnknownRequest}) {
			Payload payload;
			const auto saved = payload.storage;
			CheckResult(device.Ioctl(authority, request, payload.Bytes(24)), Drm::ResultKind::NativeError, 1);
			Check(Name, payload.storage == saved && device.Snapshot() == Drm::State {} && persistence.calls.load() == 0,
			      "EPERM changed payload/state or attempted persistence");
		}
	}
	for (Drm::Authority authority: {Drm::Authority {}, Drm::Authority {0x480000000000000eull}}) {
		Payload payload;
		const auto saved = payload.storage;
		CheckResult(device.Ioctl(authority, Drm::ShellUiRequest, payload.Bytes(24)), Drm::ResultKind::NativeError, 1);
		Check(Name, payload.storage == saved, "denied ShellUI request changed payload");
	}
	for (uint64_t authority: {Drm::DriverAuthority, Drm::ShellUiAuthority}) {
		Payload payload;
		const auto saved = payload.storage;
		CheckResult(device.Ioctl(authority, Drm::ShellUiRequest, payload.Bytes(52)),
		            Drm::ResultKind::UnsupportedRequest, std::nullopt);
		Check(Name, payload.storage == saved, "unsupported authorized ShellUI operation changed output");
	}
	{
		Payload payload;
		const auto saved = payload.storage;
		CheckResult(device.Ioctl(Drm::DriverAuthority, Drm::AuditedUnknownRequest, payload.Bytes(4)),
		            Drm::ResultKind::NativeError, 25);
		// This unclassified opcode must remain explicitly unsupported, not guessed
		// to be native ENOTTY, acknowledged, or routed through a generic success.
		CheckResult(device.Ioctl(Drm::DriverAuthority, 0xc0044e07, payload.Bytes(4)),
		            Drm::ResultKind::UnsupportedRequest, std::nullopt);
		Check(Name, payload.storage == saved && device.Snapshot() == Drm::State {} && persistence.calls.load() == 0,
		      "unknown/unsupported request changed payload or driver state");
	}
	struct RequestBuffer { uint32_t request; size_t size; };
	for (auto [request, size]: std::array<RequestBuffer, 5> {{
	         {Drm::Initialize, 4}, {Drm::Terminate, 4}, {Drm::QueryState, 12}, {Drm::QueryStateHash, 20},
	         {Drm::ShellUiRequest, 52}}}) {
		Payload payload;
		const auto saved = payload.storage;
		CheckResult(device.Ioctl(Drm::DriverAuthority, request, payload.Bytes(size - 1)),
		            Drm::ResultKind::InvalidBuffer, std::nullopt);
		CheckResult(device.Ioctl(std::nullopt, request, payload.Bytes(0)), Drm::ResultKind::NativeError, 1);
		Check(Name, payload.storage == saved && device.Snapshot() == Drm::State {} && persistence.calls.load() == 0,
		      "invalid local span or denied call changed state");
	}
	constexpr std::array<uint8_t, 4> zero_status {};
	const auto check_clock = [&] {
		for (uint64_t authority: {Drm::DriverAuthority, Drm::ShellUiAuthority}) {
			Payload payload;
			auto bytes = payload.Bytes(52);
			bytes[8] = 3;
			bytes[9] = bytes[10] = bytes[11] = 0;
			auto expected = payload.storage;
			expected[16] = 0x13; expected[17] = 0x05; expected[18] = 0x0f; expected[19] = 0x80;
			const auto state = device.Snapshot();
			const auto calls = persistence.calls.load();
			CheckResult(device.Ioctl(authority, Drm::ShellUiRequest, bytes), Drm::ResultKind::Completed, 0);
			Check(Name, payload.storage == expected && device.Snapshot() == state && persistence.calls == calls,
			      "absent secure clock must preserve tick/tail and return payload error independently of NPDRM init");
		}
	};
	check_clock();
	{
		Drm::Device clock_device(persistence, +[](uint64_t* seconds) -> int32_t { *seconds = 123; return 0; });
		Payload payload;
		auto bytes = payload.Bytes(52);
		bytes[8] = 3; bytes[9] = bytes[10] = bytes[11] = 0;
		auto expected = payload.storage;
		const uint64_t tick = (UINT64_C(63460972800) + 123) * 1000000;
		for (size_t i = 0; i < 8; ++i) { expected[4 + i] = static_cast<uint8_t>(tick >> (8 * i)); }
		std::fill(expected.begin() + 16, expected.begin() + 20, 0);
		CheckResult(clock_device.Ioctl(Drm::ShellUiAuthority, Drm::ShellUiRequest, bytes), Drm::ResultKind::Completed, 0);
		Check(Name, payload.storage == expected, "secure-clock success must convert the epoch and preserve the 36-byte tail");
	}
	{
		Payload payload;
		CheckResult(device.Ioctl(Drm::DriverAuthority, Drm::Terminate, payload.Bytes(4)), Drm::ResultKind::Completed, 0);
		CheckWire(payload, zero_status);
		Check(Name, device.Snapshot() == Drm::State {} && persistence.calls.load() == 0, "pre-init term initialized device");
	}
	constexpr std::array<uint8_t, 12> before_state {
	    0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0x32, 0x0a, 0x0f, 0x80};
	constexpr std::array<uint8_t, 20> before_hash {
	    0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
	    0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0x32, 0x0a, 0x0f, 0x80};
	{
		Payload state, hash;
		CheckResult(device.Ioctl(Drm::DriverAuthority, Drm::QueryState, state.Bytes(12)), Drm::ResultKind::Completed, 0);
		CheckResult(device.Ioctl(Drm::DriverAuthority, Drm::QueryStateHash, hash.Bytes(20)), Drm::ResultKind::Completed, 0);
		CheckWire(state, before_state);
		CheckWire(hash, before_hash);
		Check(Name, device.Snapshot() == Drm::State {} && persistence.calls.load() == 0, "pre-init queries mutated state");
	}

	Drm::State initialized;
	initialized.attempted = true;
	initialized.error_class = 0x10000;
	initialized.error = 0x800f0a0e;
	constexpr std::array<uint8_t, 12> after_state {
	    0x00, 0x00, 0x01, 0x00, 0x0e, 0x0a, 0x0f, 0x80, 0x00, 0x00, 0x00, 0x00};
	constexpr std::array<uint8_t, 20> after_hash {
	    0x26, 0x1c, 0x4b, 0xbf, 0x74, 0x82, 0xc9, 0xf5,
	    0x63, 0xf4, 0x42, 0x72, 0xe5, 0x43, 0xf6, 0x55, 0x00, 0x00, 0x00, 0x00};
	for (unsigned attempt = 1; attempt <= 3; ++attempt) {
		persistence.error = attempt == 2 ? 5 : 0; // Failed persistence must not change acknowledgment or state.
		Payload init;
		CheckResult(device.Ioctl(Drm::DriverAuthority, Drm::Initialize, init.Bytes(24)), Drm::ResultKind::Completed, 0);
		CheckWire(init, zero_status); // Extra local bytes are also canaries.
		Check(Name, persistence.calls.load() == attempt && persistence.last_depth.load() == depth + 1 &&
		            !persistence.nonempty.load() && !persistence.unprotected.load() && device.Snapshot() == initialized,
		      "every init must persist length zero and retain the exact unavailable-SM state");
		Payload state, hash, term;
		CheckResult(device.Ioctl(Drm::DriverAuthority, Drm::QueryState, state.Bytes(12)), Drm::ResultKind::Completed, 0);
		CheckResult(device.Ioctl(Drm::DriverAuthority, Drm::QueryStateHash, hash.Bytes(20)), Drm::ResultKind::Completed, 0);
		CheckWire(state, after_state);
		CheckWire(hash, after_hash);
		CheckResult(device.Ioctl(Drm::DriverAuthority, Drm::Terminate, term.Bytes(4)), Drm::ResultKind::Completed, 0);
		CheckWire(term, zero_status);
		Check(Name, device.Close(Drm::DriverAuthority) == 0 && device.Open(Drm::ShellUiAuthority) == 0 &&
		            device.Close(Drm::ShellUiAuthority) == 0 && device.Open(Drm::DriverAuthority) == 0,
		      "close/reopen authorization");
		Check(Name, device.Snapshot() == initialized && persistence.calls.load() == attempt,
		      "query/term/close/reopen reset global state or touched persistence");
		check_clock();
	}
	{
		Payload denied;
		const auto saved = denied.storage;
		CheckResult(device.Ioctl(Drm::ShellUiAuthority, Drm::Initialize, denied.Bytes(4)), Drm::ResultKind::NativeError, 1);
		Check(Name, denied.storage == saved && device.Snapshot() == initialized && persistence.calls.load() == 3,
		      "denied re-init changed an established global state or persisted");
		auto copy = device.Snapshot();
		copy.digest.fill(0xff);
		copy.activation.fill(0xff);
		Check(Name, device.Snapshot() == initialized, "snapshot leaked mutable driver storage");
		RecordingPersistence other_persistence;
		Drm::Device other(other_persistence);
		Payload state;
		CheckResult(other.Ioctl(Drm::DriverAuthority, Drm::QueryState, state.Bytes(12)), Drm::ResultKind::Completed, 0);
		CheckWire(state, before_state);
		Check(Name, other.Snapshot() == Drm::State {} && other_persistence.calls.load() == 0,
		      "independent fixture inherited another device's state");
	}
	// Concurrent calls must each persist, with no overlapping callbacks. Callback
	// bookkeeping is atomic so a locking regression does not race the test itself.
	std::atomic<unsigned> failures {0};
	auto initialize_many = [&] {
		for (unsigned i = 0; i < 16; ++i) {
			std::array<uint8_t, 4> output {0xa5, 0xa5, 0xa5, 0xa5};
			const auto result = device.Ioctl(Drm::DriverAuthority, Drm::Initialize, output);
			if (result.kind != Drm::ResultKind::Completed || result.transport_errno != 0 || output != zero_status) {
				failures.fetch_add(1);
			}
		}
	};
	std::thread first(initialize_many);
	std::thread second(initialize_many);
	first.join();
	second.join();
	Check(Name, failures.load() == 0 && persistence.calls.load() == 35 && !persistence.overlap.load() &&
	            !persistence.nonempty.load() && !persistence.unprotected.load() && device.Snapshot() == initialized,
	      "concurrent init lost persistence, overlapped a transaction, or changed the unavailable state");
	Check(Name, Common::GetHostLockCriticalCounter()->load() == depth, "device leaked host-critical protection");
}

void TestNpDrmIoctlRegistrations() {
	Loader::SymbolDatabase symbols;
	Libs::LibKernelTelemetry::InitKernelTelemetry_1(&symbols);
	const auto expected = reinterpret_cast<uint64_t>(&Libs::ShellHits::Kernel::Ioctl);
	const auto verify = [&] {
		const auto* exact = symbols.Find({"PfccT7qURYE", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
		const auto* fallback = symbols.FindByNid("PfccT7qURYE", Loader::SymbolType::Func);
		Check("NpDrmIoctlRegistrations", exact && fallback && exact->vaddr == expected && fallback->vaddr == expected,
		      "early/late exact and NID ioctl bindings must share descriptor-aware dispatch");
	};
	verify();
	Libs::ShellHits::InitShellHits_1(&symbols);
	verify();
}
