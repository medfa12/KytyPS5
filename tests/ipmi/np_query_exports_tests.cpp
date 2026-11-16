// Real export/client implementation with a loader resolver fixture. This checks
// routing and lease duration, not native credential admission or remote PSN.
#include "libs/npManagerClient.h"
#include "loader/runtimeNpManagerClient.h"
#include "loader/symbolDatabase.h"
#include "common/logging/log.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
namespace N = Libs::NpManagerClient;
static std::atomic<std::shared_ptr<N::Client>> selected;
static thread_local bool admitted = false;
namespace Loader::RuntimeNpManagerClient {
std::shared_ptr<N::Client> AcquireQueryClient() noexcept { return admitted ? selected.load() : nullptr; }
std::optional<int> QueryLocalSigninState(int, uint32_t*) { return std::nullopt; }
}
namespace Loader { void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {} }
namespace Log { bool IsSilent() { return true; } void Write(fmt::text_style, std::string_view) {} }
namespace Libs { void PrintName(const char*, const char*, const char*) {} }

struct Endpoint: N::Endpoint {
	std::atomic_bool connected {true}, hold {false}, entered {false}, release {false};
	unsigned calls = 0;
	uint32_t last_method = 0;
	bool Connected() const noexcept override { return connected; }
	N::Reply Invoke(uint32_t method, std::span<const uint8_t> input, std::span<uint8_t> output) noexcept override {
		std::fill(output.begin(), output.end(), 0);
		if (!method) {
			CHECK(input.empty() && output.size() == 32);
			std::memcpy(output.data(), "query-fixture", sizeof("query-fixture"));
		} else {
			++calls; last_method = method;
			CHECK(input.size() == (method == 0x13 ? 8u : 4u));
			int user; std::memcpy(&user, input.data(), 4); CHECK(user == 17);
			if (hold) { entered = true; while (!release) { std::this_thread::yield(); } }
			output[0] = 2;
		}
		return {0, 0, output.size()};
	}
	int OpenEvents(const char*) noexcept override { return 0; }
	int Poll(N::Event&) noexcept override { return 1; }
	void Disconnect() noexcept override { connected = false; }
};
int main() {
	auto global = std::make_shared<Endpoint>();
	CHECK(N::Session().Attach(global) == 0);
	uint32_t state = 0xaabbccdd;
	CHECK(N::GetSigninStateForUser(17, &state) == N::NotInitialized && state == 0xaabbccdd && global->calls == 0);
	auto endpoint = std::make_shared<Endpoint>();
	auto client = std::make_shared<N::Client>();
	CHECK(client->Attach(endpoint) == 0);
	selected = client;
	CHECK(N::GetSigninStateForUser(17, &state) == N::NotInitialized && endpoint->calls == 0);
	admitted = true;
	CHECK(N::GetSigninStateForUser(17, &state) == 0 && state == 2 && endpoint->last_method == 0xe);
	CHECK(N::GetActiveSigninStateForUser(17, &state) == 0 && endpoint->last_method == 0xf);
	int service = -1;
	CHECK(N::GetServiceState38(17, &service) == 0 && service == 2 && endpoint->last_method == 0x38);
	uint8_t data[20] {};
	CHECK(N::GetUserData13ForUser(17, data) == 0 && data[0] == 2 && endpoint->last_method == 0x13);
	N::OfflineAccountId id; std::memset(&id, 0xab, sizeof(id));
	CHECK(N::GetOfflineAccountIdForUser(17, &id) == N::OfflineAccountIdUnavailable && uint8_t(id.id[0]) == 0xab);
	CHECK(N::RegisterEvent4(17, nullptr, nullptr) == N::NotInitialized);
	CHECK(global->calls == 0);
	std::weak_ptr<N::Client> retained = client;
	endpoint->hold = true;
	std::thread query([&] { admitted = true; CHECK(N::GetSigninStateForUser(17, &state) == 0 && state == 2); });
	while (!endpoint->entered) { std::this_thread::yield(); }
	selected.store({}); client.reset();
	CHECK(!retained.expired()); // the exported call owns its resolver lease
	endpoint->release = true;
	query.join();
	CHECK(retained.expired() && !endpoint->connected);
	CHECK(N::GetSigninStateForUser(17, &state) == N::NotInitialized && global->calls == 0);
	N::Session().Detach();
	std::cout << "NP production exports: resolver selection, no Session fallback, wire methods and in-flight lease passed\n";
}
