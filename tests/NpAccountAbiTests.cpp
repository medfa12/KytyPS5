// Standalone component fixture. Calls the production registrations through SysV
// pointers to verify owner rejection; Client wire tests use an independent IPC
// fixture. Attaching the fixture Session must never authorize production exports.
#include "libs/npManagerClient.h"
#include "loader/symbolDatabase.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace Libs::OfflineLogin::NpManager { void InitOfflineLoginNpManager_1(Loader::SymbolDatabase*); }

namespace {
namespace Np = Libs::NpManagerClient;
void Check(const char* name, bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "%s: %s\n", name, message); std::abort(); }
}
void CheckOk(const char* name, int result, const char* message) { Check(name, result == 0, message); }
#include "NpManagerClientTests.inc"
constexpr const char* Name = "NpAccountAbi";
constexpr int Absent = static_cast<int>(0x80550002u);
constexpr int Invalid = static_cast<int>(0x80550003u);
constexpr int Unavailable = static_cast<int>(0x80559b1au);
constexpr int Io = static_cast<int>(0x80020005u);
using StateFn = int (KYTY_SYSV_ABI *)(int32_t, uint32_t*);
using OfflineFn = int (KYTY_SYSV_ABI *)(int32_t, Np::OfflineAccountId*);
int KYTY_SYSV_ABI FixtureState(int user, uint32_t* out) { return Np::Session().GetSigninState(user, out); }
int KYTY_SYSV_ABI FixtureActiveState(int user, uint32_t* out) { return Np::Session().GetSigninState(user, out, true); }
int KYTY_SYSV_ABI FixtureOffline(int user, Np::OfflineAccountId* out) { return Np::Session().GetOfflineAccountId(user, out); }
static_assert(sizeof(Np::OfflineAccountId) == 40);
static_assert(static_cast<uint32_t>(Np::SigninState::Invalid) == 0 &&
              static_cast<uint32_t>(Np::SigninState::NotSignedUp) == 1 &&
              static_cast<uint32_t>(Np::SigninState::SignedOut) == 2 &&
              static_cast<uint32_t>(Np::SigninState::SignedIn) == 3);

struct Endpoint: Np::Endpoint {
	bool connected = true, shape_ok = true;
	unsigned calls = 0, discoveries = 0, opens = 0, disconnects = 0;
	uint32_t method = 0, state = 0;
	int32_t user = 0;
	int transport = 0, service = 0;
	size_t written = 4;
	bool Connected() const noexcept override { return connected; }
	Np::Reply Invoke(uint32_t m, std::span<const uint8_t> input, std::span<uint8_t> output) noexcept override {
		if (m == 0) {
			++discoveries;
			shape_ok = shape_ok && input.empty() && output.size() == 32;
			if (output.size() == 32) {
				std::fill(output.begin(), output.end(), 0);
				std::memcpy(output.data(), "account-fixture-events", sizeof("account-fixture-events"));
			}
			return {0, 0, 32};
		}
		++calls;
		method = m;
		shape_ok = shape_ok && (m == 0xe || m == 0xf) && input.size() == 4 && output.size() == 4;
		if (input.size() == 4) { std::memcpy(&user, input.data(), 4); }
		// Dirty even failed/short replies: the client must stage before publication.
		if (output.size() == 4) { std::memcpy(output.data(), &state, 4); }
		return {transport, service, written};
	}
	int OpenEvents(const char* name) noexcept override {
		++opens;
		shape_ok = shape_ok && std::strcmp(name, "account-fixture-events") == 0;
		return 0;
	}
	int Poll(Np::Event&) noexcept override { return 1; }
	void Disconnect() noexcept override { connected = false; ++disconnects; }
};

void AccountExports() {
	Loader::SymbolDatabase symbols;
	Libs::OfflineLogin::NpManager::InitOfflineLoginNpManager_1(&symbols);
	Libs::InitNpManagerClient_1(&symbols);
	const auto resolve = [&](const char* nid, const char* expression) {
		const auto* entry = symbols.FindRegistration(
		    {nid, "NpManager", 1, "NpManager", 1, 1, Loader::SymbolType::Func}, expression);
		Check(Name, entry && entry->vaddr, "missing/conflicting production registration");
		return entry->vaddr;
	};
	const std::array<StateFn, 2> exports {
	    reinterpret_cast<StateFn>(resolve("6miba-pcQt8", "Libs::NpManagerClient::GetSigninStateForUser")),
	    reinterpret_cast<StateFn>(resolve("UAA2-ZTmgJc", "Libs::NpManagerClient::GetActiveSigninStateForUser"))};
	const auto offline_export = reinterpret_cast<OfflineFn>(resolve("70Swvw7h6ck", "Libs::NpManagerClient::GetOfflineAccountIdForUser"));
	const std::array<StateFn, 2> getters {FixtureState, FixtureActiveState};
	const OfflineFn offline = FixtureOffline;
	auto& client = Np::Session();
	client.Detach();
	struct StateBuffer { uint32_t before, value, after; };
	constexpr StateBuffer saved {0x10203040, 0xa7a7a7a7, 0x50607080};
	StateBuffer output = saved;
	struct OfflineBuffer { std::array<uint8_t, 8> before; Np::OfflineAccountId id; std::array<uint8_t, 8> after; } identity;
	static_assert(sizeof(OfflineBuffer) == 56);
	std::memset(&identity, 0xa7, sizeof(identity));
	const auto original = identity;
	for (auto getter: getters) {
		Check(Name, getter(-1, nullptr) == Absent && getter(17, &output.value) == Absent &&
		            std::memcmp(&output, &saved, sizeof(output)) == 0, "absent state/error precedence or output changed");
	}
	Check(Name, offline(-1, nullptr) == Absent && offline(17, &identity.id) == Absent &&
	            std::memcmp(&identity, &original, sizeof(identity)) == 0, "absent offline ID changed caller buffer");
	auto endpoint = std::make_shared<Endpoint>();
	CheckOk(Name, client.Attach(endpoint), "attach discovered/opened endpoint");
	Check(Name, endpoint->discoveries == 1 && endpoint->opens == 1, "missing real discovery/event-open prerequisite");
	for (auto getter: exports) {
		Check(Name, getter(17, &output.value) == Absent && getter(-1, nullptr) == Absent &&
		            std::memcmp(&output, &saved, sizeof(output)) == 0, "host caller borrowed fixture Session");
	}
	Check(Name, offline_export(17, &identity.id) == Absent &&
	            std::memcmp(&identity, &original, sizeof(identity)) == 0 && endpoint->calls == 0,
	      "production export used host fixture endpoint or changed output");
	using CreateFn = int (KYTY_SYSV_ABI *)();
	using RequestFn = int (KYTY_SYSV_ABI *)(int);
	const auto create_request = reinterpret_cast<CreateFn>(resolve("xZk+QcivrFE", "Libs::NpManagerClient::CreatePrivateRequest"));
	const auto delete_request = reinterpret_cast<RequestFn>(resolve("HneC+SpeLwc", "Libs::NpManagerClient::DeletePrivateRequest"));
	const auto abort_request = reinterpret_cast<RequestFn>(resolve("AUuzKQIwhXY", "Libs::NpManagerClient::AbortPrivateRequest"));
	Check(Name, create_request() == Absent && delete_request(7001) == Absent && abort_request(7001) == Absent &&
	            endpoint->calls == 0, "private lifecycle registration fabricated a handle or borrowed fixture Session");
	// The 8/7-argument native ticket ABIs include stack arguments. A query fixture
	// must not authorize them, and unavailable transport must not look like an
	// empty successful service response (the QR flow immediately parses JSON).
	using TicketFn = int (KYTY_SYSV_ABI *)(int, int, const char*, const char*, char*, size_t, char*, size_t);
	using LoginTicketFn = int (KYTY_SYSV_ABI *)(int, const char*, const char*, char*, size_t, char*, size_t);
	const auto ticket = reinterpret_cast<TicketFn>(resolve("IxqaB0MHl0s", "CreateAuthenticationTicket"));
	const auto login_ticket = reinterpret_cast<LoginTicketFn>(resolve("xR8S67myUos", "LoginCreateAuthenticationTicket"));
	std::array<char, 96> ticket_output;
	ticket_output.fill('!');
	const auto saved_ticket_output = ticket_output;
	Check(Name, ticket(1, 17, "consoleSignin", "{}", ticket_output.data() + 8, 32,
	                   ticket_output.data() + 56, 32) == Absent &&
	            login_ticket(1, "consoleSignin", "{}", ticket_output.data() + 8, 32,
	                         ticket_output.data() + 56, 32) == Absent &&
	            ticket_output == saved_ticket_output && endpoint->calls == 0,
	      "uninitialized ticket transport returned success, modified output or borrowed query endpoint");
	Check(Name, ticket(0, -1, nullptr, nullptr, nullptr, 0, nullptr, 0) == Absent &&
	            login_ticket(0, nullptr, nullptr, nullptr, 0, nullptr, 0) == Absent,
	      "ticket initialization error must precede argument validation");
	for (size_t i = 0; i < getters.size(); ++i) {
		const auto getter = getters[i];
		const auto before = endpoint->calls;
		Check(Name, getter(-1, nullptr) == Invalid && endpoint->calls == before, "null output reached endpoint");
		for (int32_t user: {17, -1, 0, 0x12345678}) {
			for (uint32_t state = 0; state != 4; ++state) {
				endpoint->state = state;
				output = saved;
				CheckOk(Name, getter(user, &output.value), "all private states including Invalid are accepted");
				Check(Name, output.value == state && output.before == saved.before && output.after == saved.after &&
				            endpoint->method == 0xe + i && endpoint->user == user && endpoint->shape_ok,
				      "typed user/state payload, exact four-byte extent or method mismatch");
			}
		}
		const auto failed = [&](int expected) {
			output = saved;
			Check(Name, getter(17, &output.value) == expected && std::memcmp(&output, &saved, sizeof(output)) == 0,
			      "failure precedence or failed reply modified output");
		};
		endpoint->transport = static_cast<int>(0x80020035u);
		endpoint->service = static_cast<int>(0x80550006u);
		endpoint->written = 0;
		failed(endpoint->transport);
		endpoint->transport = 0;
		failed(endpoint->service);
		endpoint->service = 0;
		for (size_t written: {size_t(0), size_t(3), size_t(5), size_t(-1)}) { endpoint->written = written; failed(Io); }
		endpoint->written = 4;
		for (uint32_t state: {4u, 0xffffffffu, 0x80000000u}) { endpoint->state = state; failed(Io); }
	}
	const auto calls = endpoint->calls;
	for (int32_t user: {-1, 0, 17, -2}) {
		const auto expected = user == -1 ? Invalid : Unavailable;
		Check(Name, offline(user, &identity.id) == expected && offline(user, nullptr) == expected &&
		            offline(user, reinterpret_cast<Np::OfflineAccountId*>(uintptr_t(1))) == expected &&
		            std::memcmp(&identity, &original, sizeof(identity)) == 0 && endpoint->calls == calls,
		      "offline ID inspected/wrote output or invented an IPC request/identity");
	}
	endpoint->connected = false;
	for (auto getter: getters) { Check(Name, getter(17, nullptr) == Absent, "lost endpoint validation precedence"); }
	Check(Name, offline(-1, &identity.id) == Absent, "disconnected offline availability precedence");
	client.Detach();
	Check(Name, endpoint->disconnects == 1, "detach did not release endpoint");
	for (auto getter: getters) { Check(Name, getter(17, &output.value) == Absent, "detached query returned cached state"); }
	auto replacement = std::make_shared<Endpoint>();
	replacement->state = 2;
	CheckOk(Name, client.Attach(replacement), "reconnect");
	for (auto getter: getters) {
		Check(Name, getter(18, &output.value) == 0 && output.value == 2 && replacement->user == 18,
		      "reconnected getter did not use new endpoint reply");
	}
	Check(Name, offline(18, &identity.id) == Unavailable && std::memcmp(&identity, &original, sizeof(identity)) == 0,
	      "reconnect synthesized offline identity");
	client.Detach();
}
} // namespace

int main() {
	static Common::Subsystems subsystems;
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	AccountExports();
	TestNpManagerClient();
	std::puts("NP account ABI: production NID owner rejection; fixture SysV states, IPC/error contracts, offline-ID canaries, reconnect and client regressions passed");
}
