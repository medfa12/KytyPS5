#pragma once

#include "common/abi.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace Loader { class SymbolDatabase; }

namespace Libs::NpManagerClient {

inline constexpr int NotInitialized = static_cast<int>(0x80550002u);
inline constexpr int AlreadyInitialized = static_cast<int>(0x80550001u);
inline constexpr int InvalidArgument = static_cast<int>(0x80550003u);
inline constexpr int OutOfMemory = static_cast<int>(0x80550005u);
inline constexpr int SignedOut = static_cast<int>(0x80550006u);
inline constexpr int UserNotFound = static_cast<int>(0x80550007u);
inline constexpr int CallbackSlotsFull = static_cast<int>(0x80559b04u);
// Observed 13.60 private offline-ID export result; not an available account ID.
inline constexpr int OfflineAccountIdUnavailable = static_cast<int>(0x80559b1au);
// An incomplete successful IPC reply is a transport I/O error, not an NP state.
inline constexpr int TransportIoError = static_cast<int>(0x80020005u);
inline constexpr int TransportBusy = static_cast<int>(0x80020010u);
inline constexpr int RequestSlotsFull = static_cast<int>(0x80559d05u);
inline constexpr int RequestNotFound = static_cast<int>(0x80559d0au);
inline constexpr int RequestAlreadyBound = static_cast<int>(0x80559d1cu);
inline constexpr const char* ServiceName = "SceNpMgrIpc";

// Private NpManager/managed SigninState. This is NOT public SceNpState
// (UNKNOWN=0, SIGNED_OUT=1, SIGNED_IN=2). Local login is not NP enrollment.
enum class SigninState: uint32_t { Invalid = 0, NotSignedUp = 1, SignedOut = 2, SignedIn = 3 };
struct OfflineAccountId {
	char id[37]; // Managed ByValTStr(SizeConst=37), followed by three padding bytes.
	uint8_t padding[3];
};
static_assert(sizeof(OfflineAccountId) == 40 && offsetof(OfflineAccountId, padding) == 37);

using Callback = void (KYTY_SYSV_ABI *)(int user, int value, void* userdata);

struct Reply {
	int transport_result = TransportIoError;
	int service_result = NotInitialized;
	size_t written = 0;
};

struct Event {
	uint32_t channel = 0;
	std::array<uint8_t, 20> payload {};
	size_t size = 0;
};

// Host-side adapter for a REAL accepted/initialized SceNpMgrIpc endpoint.
// This is not a guest vtable or a network wire protocol. An adapter must retain
// its service handle until Disconnect, use bounded operations (Poll is nonblocking),
// and never invoke guest code or reenter Client. Calls are serialized by Client;
// Disconnect cannot cancel an Invoke while that Invoke holds the client mutex.
// Generic libC.cpp ClientConnect's zero return without
// native accept is NOT sufficient to construct such an adapter.
class Endpoint {
public:
	virtual ~Endpoint() = default;
	virtual bool Connected() const noexcept = 0;
	virtual Reply Invoke(uint32_t method, std::span<const uint8_t> input,
	                     std::span<uint8_t> output) noexcept = 0;
	// Preserve descriptor boundaries (method5 uses a header AND payload).
	// Query-only endpoints cannot silently flatten an unsupported request.
	virtual Reply InvokeInputs(uint32_t method, std::span<const std::span<const uint8_t>> inputs,
	                           std::span<uint8_t> output) noexcept {
		if (inputs.size() > 1) { return {0, NotInitialized, 0}; }
		return Invoke(method, inputs.empty() ? std::span<const uint8_t> {} : inputs[0], output);
	}
	struct Output {
		std::span<uint8_t> bytes;
		size_t written = 0;
	};
	// Ticket result IPC has separate JSON/correlation descriptors. written is the
	// actual length per descriptor; Reply::written is their sum. The native adapter
	// stages both buffers until success. Query-only endpoints reject multiple outputs.
	virtual Reply InvokeBuffers(uint32_t method, std::span<const std::span<const uint8_t>> inputs,
	                            std::span<Output> outputs) noexcept {
		for (auto& output: outputs) { output.written = 0; }
		if (outputs.size() > 1) { return {0, NotInitialized, 0}; }
		auto reply = InvokeInputs(method, inputs, outputs.empty() ? std::span<uint8_t> {} : outputs[0].bytes);
		if (!outputs.empty() && !reply.transport_result && !reply.service_result) { outputs[0].written = reply.written; }
		return reply;
	}
	// Retain/open the actual named kernel event flag returned by IPC method0.
	// Copy the name if needed; the pointer is valid only for this call.
	virtual int OpenEvents(const char* event_flag_name) noexcept = 0;
	// Host contract: 0 = event, 1 = no event, negative = real transport error.
	virtual int Poll(Event& event) noexcept = 0;
	// Optional private request transport. Bind creates/clears the client-owned
	// completion flag and invokes method8 on the retained owner worker. Poll uses
	// the full 64-bit mask: 0 = signaled, 1 = pending, negative = error.
	virtual bool RequestEventsAvailable() const noexcept { return false; }
	virtual int BindRequest(int request, uint64_t mask) noexcept { return NotInitialized; }
	virtual int PollRequest(uint64_t mask) noexcept { return NotInitialized; }
	// Release/drain the retained service endpoint. No guest callbacks originate here.
	virtual void Disconnect() noexcept = 0;
};

// One VSH service client. Not an owner-neutral global for shared applications.
// Attach/Detach belong to the service/session lifecycle, never guest imports.
class Client {
public:
	Client() = default;
	~Client();
	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;
	// Attach consumes a candidate only after discovery starts; failed discovery
	// disconnects it. Null/disconnected/busy/already-initialized rejections leave
	// the candidate with its caller. Retry after Detach and its old pump finish.
	int Attach(std::shared_ptr<Endpoint> endpoint);
	// Stop producers before destruction. A callback may Detach, but must not destroy
	// this Client: its dispatch scope remains live until that callback returns.
	void Detach();
	bool Connected() const;
	// Private lifecycle, distinct from public sceNpCreateRequest. Only server IDs.
	int CreateRequest();
	int DeleteRequest(int request);
	int AbortRequest(int request);
	int AuthenticationTicket(bool login, int request, int user, const char* ticket_type,
	    const char* request_json, char* response_json, size_t response_size, char* correlation_id, size_t correlation_size);
	int GetSigninState(int user, uint32_t* output, bool active = false);
	int GetOfflineAccountId(int user, OfflineAccountId* output);
	int GetUserData13(int user, void* output20);
	int GetServiceState38(int user, int* output);
	int Register(uint32_t channel, int user_filter, Callback callback, void* userdata);
	int Unregister(uint32_t channel, int id);
	// Call only on an admitted guest execution thread. No callback is synthesized
	// on registration, and no callback runs under the client's/backend's mutex.
	int DispatchCallbacks();
private:
	struct Registration;
	using Table = std::array<std::shared_ptr<Registration>, 32>;
	static size_t Capacity(uint32_t channel);
	Table& Slots(uint32_t channel);
	static void Retire(const std::shared_ptr<Registration>& registration);
	int QueryLocked(uint32_t method, std::span<const uint8_t> input,
	                std::span<uint8_t> output);
	mutable std::mutex m_mutex;
	std::shared_ptr<Endpoint> m_endpoint;
	uint64_t m_generation = 0;
	unsigned m_detaching = 0;
	std::atomic_bool m_dispatching {false};
	Table m_event4 {};
	Table m_event7 {};
	struct Request { int id = 0; unsigned active = 0; bool deleted = false; };
	// An active deleted record retains its completion bit until release.
	std::array<Request, 64> m_requests {};
	std::condition_variable m_request_wakeup;
};

// Host component fixture only. Production imports resolve the actual owner's
// retained runtime client and never attach to or fall back to this singleton.
Client& Session();

int KYTY_SYSV_ABI GetSigninStateForUser(int user, uint32_t* output);
int KYTY_SYSV_ABI GetActiveSigninStateForUser(int user, uint32_t* output);
int KYTY_SYSV_ABI GetOfflineAccountIdForUser(int user, OfflineAccountId* output);
int KYTY_SYSV_ABI CreatePrivateRequest();
int KYTY_SYSV_ABI DeletePrivateRequest(int request);
int KYTY_SYSV_ABI AbortPrivateRequest(int request);

// Ticket requests require their own initialized native request transport;
// admitting a state-query endpoint does not initialize ticket creation.
int KYTY_SYSV_ABI CreateAuthenticationTicket(int request, int user, const char* ticket_type,
    const char* request_json, char* response_json, size_t response_size, char* correlation_id, size_t correlation_size);
int KYTY_SYSV_ABI LoginCreateAuthenticationTicket(int request, const char* ticket_type,
    const char* request_json, char* response_json, size_t response_size, char* correlation_id, size_t correlation_size);

int KYTY_SYSV_ABI RegisterEvent4(int user, Callback callback, void* userdata);
int KYTY_SYSV_ABI UnregisterEvent4(int id);
int KYTY_SYSV_ABI RegisterEvent7(int user, Callback callback, void* userdata);
int KYTY_SYSV_ABI UnregisterEvent7(int id);
int KYTY_SYSV_ABI GetUserData13ForUser(int user, void* output20);
int KYTY_SYSV_ABI GetServiceState38(int user, int* output);

} // namespace Libs::NpManagerClient

namespace Libs { void InitNpManagerClient_1(Loader::SymbolDatabase* symbols); }
