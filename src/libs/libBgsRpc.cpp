#include "common/abi.h"
#include "libs/libBgsRpc.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

namespace Libs {

namespace BgsRpc {

// ps5-vsh: host-side BGS RPC peer for the RNPS Hermes bundle (NPXS40003).
// The bundle (application.ps.bundle, Hermes bytecode) references
// WebSocketTransport::_onMessageReceived, channel nps-bgs-rpc /
// CALLER_RNPS_BGS_ACCESS_LOG_CHANNEL and logs `RNPS -> BGS RPC latency`.
// Its JS IPC client (initializeIpcClient/waitConnectionAsync) is orphan -
// nothing calls it yet - so this table is the host side that completes the
// handshake the moment anything connects: Connect marks the channel live
// and seeds the login+show messages the first poll observes.

LIB_VERSION("BgsRpc", 1, "BgsRpc", 1, 1);

namespace Detail {

struct Conn {
	bool        connected = false;
	std::queue<std::string> queue;
	std::string url;
};

std::mutex                          g_mutex;
std::unordered_map<uint64_t, Conn>  g_conns;
uint64_t                            g_nextHandle = 1;

constexpr size_t kMaxUrlChars  = 1024;
constexpr size_t kMaxDataChars = 4096;
constexpr size_t kLogCap       = 200;

inline std::string BoundedStr(const char* p, size_t cap) {
	// Null-safe bounded guest/host string copy. No deref without null check.
	if (p == nullptr) {
		return {};
	}
	size_t n = 0;
	while (n < cap && p[n] != '\0') {
		++n;
	}
	return std::string(p, n);
}

} // namespace Detail

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI BgsRpcConnect(const char* url) {
	PRINT_NAME();
	// No guest deref without null check.
	const std::string urlStr = Detail::BoundedStr(url, Detail::kMaxUrlChars);
	LOGF("\t url = %s\n", url == nullptr ? "<null>" : urlStr.c_str());
	std::lock_guard<std::mutex> lock(Detail::g_mutex);
	const uint64_t              handle = Detail::g_nextHandle++;
	Detail::Conn                conn;
	conn.connected = true;
	conn.url       = urlStr;
	// Seed so the first poll observes login + show (control-center).
	conn.queue.emplace("rnps.login.done uid=1000");
	conn.queue.emplace("rnps.show.control-center");
	Detail::g_conns.emplace(handle, std::move(conn));
	LOGF("\t handle = 0x%016" PRIx64 " (connected, 2 msgs seeded)\n", handle);
	return handle;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI BgsRpcClose(uint64_t handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", handle);
	std::lock_guard<std::mutex> lock(Detail::g_mutex);
	const auto                  it = Detail::g_conns.find(handle);
	if (it != Detail::g_conns.end()) {
		it->second.connected = false;
		while (!it->second.queue.empty()) {
			it->second.queue.pop();
		}
		Detail::g_conns.erase(it);
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI BgsRpcSend(uint64_t handle, const char* data) {
	PRINT_NAME();
	// No guest deref without null check; log capped at 200 chars.
	if (data == nullptr) {
		LOGF("\t handle = 0x%016" PRIx64 " data = <null>\n", handle);
		(void)handle;
		return 0;
	}
	const std::string full    = Detail::BoundedStr(data, Detail::kMaxDataChars);
	const std::string preview = full.substr(0, Detail::kLogCap);
	LOGF("\t handle = 0x%016" PRIx64 " data = %s%s\n", handle, preview.c_str(),
	     full.size() > Detail::kLogCap ? "...(truncated)" : "");
	(void)handle;
	return 0;
}

static std::string __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI BgsRpcPoll(uint64_t handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", handle);
	std::lock_guard<std::mutex> lock(Detail::g_mutex);
	const auto                  it = Detail::g_conns.find(handle);
	if (it == Detail::g_conns.end() || !it->second.connected || it->second.queue.empty()) {
		return {};
	}
	std::string msg = std::move(it->second.queue.front());
	it->second.queue.pop();
	LOGF("\t handle = 0x%016" PRIx64 " msg = %s\n", handle, msg.c_str());
	return msg;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI BgsRpcPollInto(uint64_t handle, char* out, uint64_t size) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 " out = %p size = %llu\n", handle,
	     static_cast<void*>(out), static_cast<unsigned long long>(size));
	// Simple guest-pointer checks: no deref without null check.
	if (out == nullptr) {
		return -1;
	}
	if (size == 0) {
		return -1;
	}
	std::string msg;
	{
		std::lock_guard<std::mutex> lock(Detail::g_mutex);
		const auto                  it = Detail::g_conns.find(handle);
		if (it == Detail::g_conns.end() || !it->second.connected || it->second.queue.empty()) {
			out[0] = '\0';
			return -1;
		}
		msg = std::move(it->second.queue.front());
		it->second.queue.pop();
	}
	const size_t copy = msg.size() < static_cast<size_t>(size - 1)
	                        ? msg.size()
	                        : static_cast<size_t>(size - 1);
	std::memcpy(out, msg.data(), copy);
	out[copy] = '\0';
	LOGF("\t handle = 0x%016" PRIx64 " msg = %s\n", handle, msg.c_str());
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI BgsRpcLoopback(uint64_t handle) {
	PRINT_NAME();
	LOGF("\t handle = 0x%016" PRIx64 "\n", handle);
	std::lock_guard<std::mutex> lock(Detail::g_mutex);
	const auto                  it = Detail::g_conns.find(handle);
	if (it == Detail::g_conns.end() || !it->second.connected) {
		return 0;
	}
	return 1;
}

LIB_DEFINE(InitBgsRpc_1) {
	// Real BGS NIDs come from a future export dump; LOCAL_ keys are placeholders.
	LIB_FUNC("LOCAL_BgsRpcConnect", BgsRpcConnect);   // LOCAL
	LIB_FUNC("LOCAL_BgsRpcClose", BgsRpcClose);       // LOCAL
	LIB_FUNC("LOCAL_BgsRpcSend", BgsRpcSend);         // LOCAL
	LIB_FUNC("LOCAL_BgsRpcPoll", BgsRpcPoll);         // LOCAL
	LIB_FUNC("LOCAL_BgsRpcPollInto", BgsRpcPollInto); // LOCAL
	LIB_FUNC("LOCAL_BgsRpcLoopback", BgsRpcLoopback); // LOCAL
}

namespace BgsRpcHost {

// Host-side channel API for the Hermes live pump (see libBgsRpc.h). Same
// table the guest NIDs above use; host strings need no guest validation.
uint64_t Connect(const char* url) {
	return BgsRpcConnect(url);
}

std::string Poll(uint64_t handle) {
	return BgsRpcPoll(handle);
}

void Close(uint64_t handle) {
	BgsRpcClose(handle);
}

} // namespace BgsRpcHost

} // namespace BgsRpc

} // namespace Libs

// Orchestrator registration snippet for src/libs/libs.cpp (add exactly this):
//
//   namespace BgsRpc {
//   LIB_DEFINE(InitBgsRpc_1);
//   } // namespace BgsRpc
//
// ... and inside InitAll(Loader::SymbolDatabase* s), alongside the other
// ps5-vsh entries:
//
//   BgsRpc::InitBgsRpc_1(s);
