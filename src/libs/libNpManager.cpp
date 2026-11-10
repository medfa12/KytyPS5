#include "libs/npManagerClient.h"

#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "loader/runtimeNpManagerClient.h"

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <new>
#include <utility>

namespace Libs::NpManagerClient {

// Original 13.60 NpManager: d3a0/d5a0 = event4 register/unregister;
// f6e0/fb20 = event7 register/unregister; de20 -> 8200 = method13;
// f5f0 -> 94d0 = method38. These names describe recovered behavior rather than
// claiming an unpublished SDK spelling for a private export.
LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

namespace {
struct Invocation {
	const void* registration;
	Invocation* previous;
	static thread_local Invocation* current;
	explicit Invocation(const void* r): registration(r), previous(current) { current = this; }
	~Invocation() { current = previous; }
	static unsigned Depth(const void* r) {
		unsigned depth = 0;
		for (auto* p = current; p != nullptr; p = p->previous) {
			if (p->registration == r) { ++depth; }
		}
		return depth;
	}
};
thread_local Invocation* Invocation::current = nullptr;

struct DispatchScope {
	Client* client;
	std::atomic_bool& active;
	std::mutex& mutex;
	DispatchScope* previous;
	static thread_local DispatchScope* current;
	DispatchScope(Client* c, std::atomic_bool& a, std::mutex& m): client(c), active(a), mutex(m), previous(current) {
		current = this;
	}
	~DispatchScope() {
		current = previous;
		// Detach reacquires this mutex after its wait, so it cannot destroy the
		// atomic between our store(false) and notify_all().
		std::lock_guard lock(mutex);
		active.store(false);
		active.notify_all();
	}
	static bool Contains(const Client* c) {
		for (auto* p = current; p; p = p->previous) { if (p->client == c) { return true; } }
		return false;
	}
};
thread_local DispatchScope* DispatchScope::current = nullptr;
} // namespace

struct Client::Registration {
	int user;
	Callback callback;
	void* userdata;
	std::mutex mutex;
	std::condition_variable drained;
	bool retired = false;
	unsigned active = 0;
	Registration(int u, Callback c, void* d): user(u), callback(c), userdata(d) {}

	void Call(int event_user, int value) {
		{
			std::lock_guard lock(mutex);
			if (retired) { return; }
			++active;
		}
		struct Lease {
			Registration& r;
			~Lease() {
				std::lock_guard lock(r.mutex);
				--r.active;
				r.drained.notify_all();
			}
		} lease {*this};
		Invocation invocation(this);
		callback(event_user, value, userdata);
	}
};

Client::~Client() { Detach(); }

size_t Client::Capacity(uint32_t channel) {
	return channel == 4 ? 32 : channel == 7 ? 16 : 0;
}

Client::Table& Client::Slots(uint32_t channel) { return channel == 4 ? m_event4 : m_event7; }

int Client::Attach(std::shared_ptr<Endpoint> endpoint) {
	if (!endpoint || !endpoint->Connected()) { return NotInitialized; }
	std::lock_guard lock(m_mutex);
	if (m_endpoint) { return AlreadyInitialized; }
	if (m_detaching != 0 || m_dispatching.load()) { return TransportBusy; }
	// c930 -> 13c10 -> 7850: method0 returns a 32-byte named-event-flag name.
	// A nominally successful connect/invoke without a real reply is insufficient.
	std::array<uint8_t, 32> name {};
	const auto discovery = endpoint->Invoke(0, {}, name);
	int result = discovery.transport_result != 0 ? discovery.transport_result : discovery.service_result;
	if (result == 0 && (discovery.written != name.size() || name[0] == 0 ||
	                   std::find(name.begin(), name.end(), 0) == name.end())) {
		result = TransportIoError;
	}
	if (result == 0) { result = endpoint->OpenEvents(reinterpret_cast<const char*>(name.data())); }
	if (result != 0) {
		endpoint->Disconnect(); // native initialization failure unwinds the client
		return result;
	}
	m_endpoint = std::move(endpoint);
	++m_generation;
	return 0; // real endpoint ownership retained; says nothing about user sign-in
}

void Client::Retire(const std::shared_ptr<Registration>& registration) {
	if (!registration) { return; }
	std::unique_lock lock(registration->mutex);
	registration->retired = true;
	const auto local_depth = Invocation::Depth(registration.get());
	// Self-unregister is permitted. Other concurrent invocations must drain before
	// returning; the current invocation keeps its own lease until normal unwinding.
	registration->drained.wait(lock, [&] { return registration->active <= local_depth; });
}

void Client::Detach() {
	Table event4, event7;
	{
		std::lock_guard lock(m_mutex);
		++m_detaching;
		++m_generation;
		event4.swap(m_event4);
		event7.swap(m_event7);
		if (m_endpoint) {
			for (const auto& record: m_requests) {
				const int request = record.id;
				if (!request || !m_endpoint->Connected()) { continue; }
				std::array<uint8_t, 4> input {};
				std::memcpy(input.data(), &request, sizeof(request));
				m_endpoint->Invoke(6, input, {});
			}
			m_endpoint->Disconnect();
			m_endpoint.reset();
		}
		m_requests.fill({});
		m_request_wakeup.notify_all();
	}
	for (const auto& r: event4) { Retire(r); }
	for (const auto& r: event7) { Retire(r); }
	if (!DispatchScope::Contains(this)) { m_dispatching.wait(true); }
	{
		std::lock_guard lock(m_mutex);
		--m_detaching;
	}
}

bool Client::Connected() const {
	std::lock_guard lock(m_mutex);
	return m_endpoint && m_endpoint->Connected();
}

int Client::CreateRequest() {
	std::lock_guard lock(m_mutex);
	if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
	auto slot = std::find_if(m_requests.begin(), m_requests.end(), [](const auto& r) { return !r.id; });
	if (slot == m_requests.end()) { return RequestSlotsFull; }
	// NpManager +15340 -> +7ad0; ShellCore +ccb0e0 -> +ccec10.
	// Selector0, uint64 payload length at +8, separate four-byte zero payload.
	std::array<uint8_t, 16> header {};
	std::array<uint8_t, 4> payload {};
	const uint64_t length = payload.size();
	std::memcpy(header.data() + 8, &length, sizeof(length));
	const std::array<std::span<const uint8_t>, 2> inputs {header, payload};
	const auto reply = m_endpoint->InvokeInputs(5, inputs, {});
	if (reply.transport_result) { return reply.transport_result; }
	if (reply.service_result < 0) { return reply.service_result; }
	if (!reply.service_result || reply.written ||
	    std::any_of(m_requests.begin(), m_requests.end(), [&](const auto& r) { return r.id == reply.service_result; })) {
		// A duplicate aliases an existing request: retire instead of deleting it
		// or inventing another ID. Zero is not a valid server-issued request ID.
		m_endpoint->Disconnect();
		return TransportIoError;
	}
	slot->id = reply.service_result;
	return slot->id;
}

int Client::DeleteRequest(int request) {
	std::lock_guard lock(m_mutex);
	if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
	std::array<uint8_t, 4> input {};
	std::memcpy(input.data(), &request, sizeof(request));
	const auto reply = m_endpoint->Invoke(6, input, {});
	auto slot = std::find_if(m_requests.begin(), m_requests.end(), [&](const auto& r) { return r.id == request; });
	if (request <= 0 || slot == m_requests.end()) { return RequestNotFound; }
	// Native +15660 sends delete before removing its idle local record. Keep
	// ownership on failure so a retry/Detach can clean up the real server ID.
	if (reply.transport_result) { return reply.transport_result; }
	if (reply.service_result) { return reply.service_result; }
	if (reply.written) { return TransportIoError; }
	slot->deleted = true;
	if (!slot->active) { *slot = {}; }
	m_request_wakeup.notify_all();
	return 0;
}

int Client::AbortRequest(int request) {
	std::lock_guard lock(m_mutex);
	if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
	std::array<uint8_t, 4> input {};
	std::memcpy(input.data(), &request, sizeof(request));
	// +15840 forwards directly; abort does not release the request slot.
	const int result = QueryLocked(7, input, {});
	m_request_wakeup.notify_all();
	return result;
}

int Client::AuthenticationTicket(bool login, int request, int user, const char* ticket_type,
    const char* request_json, char* response_json, size_t response_size, char* correlation_id, size_t correlation_size) {
	std::unique_lock lock(m_mutex);
	// Native exports check initialization before even inspecting caller arguments.
	if (!m_endpoint || !m_endpoint->Connected() || !m_endpoint->RequestEventsAvailable()) { return NotInitialized; }
	if ((login ? request <= 0 : request == 0) || !ticket_type || !request_json || !response_json || !response_size ||
	    (!correlation_id && correlation_size)) { return InvalidArgument; }
	auto slot = std::find_if(m_requests.begin(), m_requests.end(), [&](const auto& r) { return r.id == request; });
	if (slot == m_requests.end() || slot->deleted) { return RequestNotFound; }
	// Do not clear a bit already owned by another wait. Native method8 rejects a
	// second binding too, but clearing first would lose its completion signal.
	if (slot->active) { return RequestAlreadyBound; }
	const uint64_t mask = uint64_t {1} << size_t(slot - m_requests.begin());
	const uint64_t generation = m_generation;
	const auto endpoint = m_endpoint;
	// Bound the two C strings to the admitted transport's combined input limit.
	const auto string_size = [](const char* text, size_t limit) {
		size_t n = 0; while (n < limit && text[n]) { ++n; } return n < limit ? n + 1 : size_t {0};
	};
	const size_t header_size = login ? 4 : 8;
	const size_t type_size = string_size(ticket_type, 0x2000 - header_size);
	if (!type_size) { return InvalidArgument; }
	const size_t json_size = string_size(request_json, 0x2000 - header_size - type_size);
	if (!json_size) { return InvalidArgument; }
	++slot->active;
	int result = endpoint->BindRequest(request, mask);
	if (result) { --slot->active; return result; }
	std::array<uint8_t, 8> submit {};
	std::memcpy(submit.data(), &request, 4);
	std::memcpy(submit.data() + 4, &user, 4);
	const std::array<std::span<const uint8_t>, 3> inputs {{
	    {submit.data(), header_size}, {reinterpret_cast<const uint8_t*>(ticket_type), type_size},
	    {reinterpret_cast<const uint8_t*>(request_json), json_size}}};
	const auto submitted = endpoint->InvokeInputs(login ? 0x88 : 0x65, inputs, {});
	result = submitted.transport_result ? submitted.transport_result : submitted.service_result;
	if (!result && submitted.written) { result = TransportIoError; }
	while (!result) {
		if (slot->deleted) { result = RequestNotFound; break; }
		const int poll = endpoint->PollRequest(mask);
		if (poll != 1) { result = poll; break; }
		// No client lock or guest transport worker is held across the wait. Abort,
		// deletion, callbacks and owner retirement can all make progress.
		m_request_wakeup.wait_for(lock, std::chrono::milliseconds(1));
		if (m_generation != generation || m_endpoint != endpoint) { return NotInitialized; }
		if (!endpoint->Connected()) { result = NotInitialized; break; }
	}
	std::array<uint8_t, 0x801> json {};
	std::array<uint8_t, 0x25> correlation {};
	std::array<Endpoint::Output, 2> outputs {{{json, 0}, {correlation, 0}}};
	if (!result) {
		std::array<uint8_t, 24> header {};
		const uint64_t json_capacity = response_size, correlation_capacity = correlation_size;
		std::memcpy(header.data(), &request, 4);
		std::memcpy(header.data() + 8, &json_capacity, 8);
		std::memcpy(header.data() + 16, &correlation_capacity, 8);
		const std::array<std::span<const uint8_t>, 1> result_inputs {header};
		const auto reply = endpoint->InvokeBuffers(login ? 0x89 : 0x66, result_inputs, outputs);
		result = reply.transport_result ? reply.transport_result : reply.service_result;
		if (!result && (outputs[0].written > json.size() || outputs[1].written > correlation.size() ||
		    outputs[0].written > response_size || outputs[1].written > correlation_size ||
		    reply.written != outputs[0].written + outputs[1].written || !outputs[0].written ||
		    std::find(json.begin(), json.begin() + outputs[0].written, 0) == json.begin() + outputs[0].written ||
		    (outputs[1].written && std::find(correlation.begin(), correlation.begin() + outputs[1].written, 0) ==
		        correlation.begin() + outputs[1].written))) { result = TransportIoError; }
	}
	// Native release method9 also signals/closes an outstanding server alias.
	// A failed release retires the endpoint so no reused bit can inherit that alias.
	const int released = QueryLocked(9, {submit.data(), 4}, {});
	if (released) { endpoint->Disconnect(); if (!result) { result = released; } }
	--slot->active;
	if (slot->deleted && !slot->active) { *slot = {}; }
	if (!result) {
		std::memcpy(response_json, json.data(), outputs[0].written);
		if (outputs[1].written) { std::memcpy(correlation_id, correlation.data(), outputs[1].written); }
	}
	return result;
}

int Client::QueryLocked(uint32_t method, std::span<const uint8_t> input,
                        std::span<uint8_t> output) {
	const auto reply = m_endpoint->Invoke(method, input, output);
	if (reply.transport_result != 0) { return reply.transport_result; }
	if (reply.service_result != 0) { return reply.service_result; }
	return reply.written == output.size() ? 0 : TransportIoError;
}

int Client::GetSigninState(int user, uint32_t* output, bool active) {
	std::array<uint8_t, 4> input {}, reply {};
	std::memcpy(input.data(), &user, sizeof(user));
	int result;
	{
		std::lock_guard lock(m_mutex);
		// 13.60 d2e0/d340: manager availability precedes the null-output check.
		// User -1 is forwarded, not reinterpreted as an output address or local user.
		if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
		if (!output) { return InvalidArgument; }
		result = QueryLocked(active ? 0xf : 0xe, input, reply);
	}
	if (result != 0) { return result; }
	uint32_t state = 0;
	std::memcpy(&state, reply.data(), sizeof(state));
	if (state > static_cast<uint32_t>(SigninState::SignedIn)) { return TransportIoError; }
	// Stage the reply: an endpoint may have filled bytes before returning an
	// error/short reply. Neither those bytes nor a compatibility state escape.
	std::memcpy(output, &state, sizeof(state));
	return 0;
}

int Client::GetOfflineAccountId(int user, OfflineAccountId* output) {
	(void)output;
	std::lock_guard lock(m_mutex);
	// Original 13.60 +eef0 does not inspect or write its output pointer at all.
	// Even an initialized manager cannot provide this ID through that export.
	if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
	return user == -1 ? InvalidArgument : OfflineAccountIdUnavailable;
}

int Client::GetUserData13(int user, void* output20) {
	std::array<uint8_t, 20> output {};
	int result;
	{
		std::lock_guard lock(m_mutex);
		// de75..ded1: initialization precedes argument checks for this export.
		if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
		if (!output20) { return InvalidArgument; }
		if (user == -1) { return UserNotFound; }
		std::array<uint8_t, 8> input {};
		std::memcpy(input.data(), &user, sizeof(user));
		input[4] = 1; // 8200: user int + flag byte; remaining input padding is zero
		result = QueryLocked(0x13, input, output);
	}
	if (result == 0) { std::memcpy(output20, output.data(), output.size()); }
	return result;
}

int Client::GetServiceState38(int user, int* output) {
	// f615 checks the pointer before c930 (unlike the method13 export).
	if (!output) { return InvalidArgument; }
	std::array<uint8_t, 4> input {}, reply {};
	std::memcpy(input.data(), &user, sizeof(user));
	int result;
	{
		std::lock_guard lock(m_mutex);
		if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
		result = QueryLocked(0x38, input, reply);
	}
	if (result == 0) { std::memcpy(output, reply.data(), reply.size()); }
	return result;
}

int Client::Register(uint32_t channel, int user_filter, Callback callback, void* userdata) {
	std::lock_guard lock(m_mutex);
	if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
	const size_t capacity = Capacity(channel);
	if (capacity == 0 || !callback) { return InvalidArgument; }
	auto& slots = Slots(channel);
	auto first = slots.begin();
	auto last = first + capacity;
	const auto empty = std::find(first, last, nullptr);
	if (empty == last) { return CallbackSlotsFull; }
	std::shared_ptr<Registration> registration;
	try { registration = std::make_shared<Registration>(user_filter, callback, userdata); }
	catch (const std::bad_alloc&) { return OutOfMemory; }
	if (std::all_of(first, last, [](const auto& r) { return !r; })) {
		// 78c0: method1 input is two little-endian u32s (channel, queue capacity).
		std::array<uint8_t, 8> input {};
		const uint32_t queue_capacity = 20;
		std::memcpy(input.data(), &channel, 4);
		std::memcpy(input.data() + 4, &queue_capacity, 4);
		const int result = QueryLocked(1, input, {});
		if (result != 0) { return result; } // native first-subscription failure rolls back its slot
	}
	*empty = std::move(registration);
	return static_cast<int>(empty - first) + 1;
}

int Client::Unregister(uint32_t channel, int id) {
	std::shared_ptr<Registration> registration;
	{
		std::lock_guard lock(m_mutex);
		if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
		const size_t capacity = Capacity(channel);
		if (id <= 0 || static_cast<size_t>(id) > capacity) { return InvalidArgument; }
		auto& slots = Slots(channel);
		if (!slots[id - 1]) { return InvalidArgument; }
		registration = std::exchange(slots[id - 1], nullptr);
		if (std::all_of(slots.begin(), slots.begin() + capacity, [](const auto& r) { return !r; })) {
			// Native d634..d67b/fc21..fc67 removes the local callback and ignores
			// backend unsubscribe failure. This success means local removal only.
			std::array<uint8_t, 4> input {};
			std::memcpy(input.data(), &channel, 4);
			QueryLocked(2, input, {}); // 7940: one channel u32, no output buffer
		}
	}
	Retire(registration);
	return 0;
}

int Client::DispatchCallbacks() {
	// The native event manager serializes these callbacks. Reject an overlapping
	// host pump (including recursive pumping) instead of holding a host lock across
	// guest code or allowing two self-unregistering callbacks to wait on each other.
	uint64_t generation;
	{
		std::lock_guard lock(m_mutex);
		if (m_dispatching.load() || m_detaching != 0) { return TransportBusy; }
		if (!m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
		m_dispatching.store(true);
		generation = m_generation;
	}
	DispatchScope scope(this, m_dispatching, m_mutex);
	// A bounded batch leaves further events in the endpoint's queue. No sign-in
	// or initial-state notification is manufactured when a callback is registered.
	for (unsigned count = 0; count < 64; ++count) {
		Event event;
		Table listeners;
		{
			std::lock_guard lock(m_mutex);
			if (generation != m_generation || !m_endpoint || !m_endpoint->Connected()) { return NotInitialized; }
			const int result = m_endpoint->Poll(event);
			if (result == 1) { return 0; }
			if (result != 0) { return result < 0 ? result : TransportIoError; }
			if (Capacity(event.channel) == 0 || event.size != event.payload.size()) { return TransportIoError; }
			listeners = Slots(event.channel);
		}
		int value, user;
		std::memcpy(&value, event.payload.data() + 8, sizeof(value));
		std::memcpy(&user, event.payload.data() + 12, sizeof(user));
		for (const auto& r: listeners) {
			if (r && (r->user == 0xfe || r->user == user)) {
				{
					std::lock_guard lock(m_mutex);
					if (generation != m_generation) { return NotInitialized; }
				}
				r->Call(user, value);
			}
		}
	}
	return 0;
}

Client& Session() { static Client client; return client; }

static int ReportResult(const char* operation, int result) {
	if (result == NotInitialized) {
		static std::atomic_uint reports {0};
		if (reports.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("NP manager client: %s service=SceNpMgrIpc result=0x80550002 owner_endpoint_unavailable\n", operation);
		}
	}
	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetSigninStateForUser(int user, uint32_t* output) {
	PRINT_NAME();
	if (const auto local = Loader::RuntimeNpManagerClient::QueryLocalSigninState(user, output)) { return *local; }
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->GetSigninState(user, output) : NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetActiveSigninStateForUser(int user, uint32_t* output) {
	PRINT_NAME();
	if (const auto local = Loader::RuntimeNpManagerClient::QueryLocalSigninState(user, output)) { return *local; }
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->GetSigninState(user, output, true) : NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetOfflineAccountIdForUser(int user, OfflineAccountId* output) {
	PRINT_NAME();
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->GetOfflineAccountId(user, output) : NotInitialized);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreatePrivateRequest() {
	PRINT_NAME();
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->CreateRequest() : NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI DeletePrivateRequest(int request) {
	PRINT_NAME();
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->DeleteRequest(request) : NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AbortPrivateRequest(int request) {
	PRINT_NAME();
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->AbortRequest(request) : NotInitialized);
}

// 13.60 libSceNpManager +125e0/+135e0 checks native initialization before
// request IDs, pointers or sizes, returning 0x80550002 without touching output.
// An unresolved zero-return fallback made LoginMgr resolve an empty responseJson
// as success, then fail JSON.parse while constructing the QR sign-in page.
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreateAuthenticationTicket(
    int request, int user, const char* type, const char* json, char* response, size_t size, char* correlation, size_t correlation_size) {
	PRINT_NAME();
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->AuthenticationTicket(false, request, user, type, json,
	    response, size, correlation, correlation_size) : NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LoginCreateAuthenticationTicket(
    int request, const char* type, const char* json, char* response, size_t size, char* correlation, size_t correlation_size) {
	PRINT_NAME();
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->AuthenticationTicket(true, request, 0, type, json,
	    response, size, correlation, correlation_size) : NotInitialized);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RegisterEvent4(int user, Callback callback, void* userdata) {
	PRINT_NAME();
	// A query endpoint does not admit guest callbacks on a host fixture pump.
	return ReportResult(__func__, NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnregisterEvent4(int id) {
	PRINT_NAME();
	return ReportResult(__func__, NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RegisterEvent7(int user, Callback callback, void* userdata) {
	PRINT_NAME();
	return ReportResult(__func__, NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnregisterEvent7(int id) {
	PRINT_NAME();
	return ReportResult(__func__, NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetUserData13ForUser(int user, void* output20) {
	PRINT_NAME();
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->GetUserData13(user, output20) : NotInitialized);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetServiceState38(int user, int* output) {
	PRINT_NAME();
	if (!output) { return InvalidArgument; }
	const auto client = Loader::RuntimeNpManagerClient::AcquireQueryClient();
	return ReportResult(__func__, client ? client->GetServiceState38(user, output) : NotInitialized);
}

LIB_DEFINE(InitNpManagerClient_1) {
	LIB_FUNC("IxqaB0MHl0s", CreateAuthenticationTicket);
	LIB_FUNC("xR8S67myUos", LoginCreateAuthenticationTicket);
	LIB_FUNC("S9xDus0Cums", RegisterEvent4);
	LIB_FUNC("zh2KsQZlAN4", UnregisterEvent4);
	LIB_FUNC("E6rzFwsDFwE", RegisterEvent7);
	LIB_FUNC("PafRf+sxnwA", UnregisterEvent7);
	LIB_FUNC("sTtvF4QVhjg", GetUserData13ForUser);
	LIB_FUNC("XRFchqddEVU", GetServiceState38);
}
} // namespace Libs::NpManagerClient

namespace Libs {
void InitNpManagerClient_1(Loader::SymbolDatabase* symbols) {
	NpManagerClient::InitNpManagerClient_1(symbols);
}
} // namespace Libs
