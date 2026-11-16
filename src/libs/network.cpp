#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <afunix.h> // AF_UNIX / sockaddr_un (AgcCompositor <-> libSceComposite local transport)
#include <bcrypt.h>
#ifdef s_addr
#undef s_addr
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/uio.h>
#endif
#endif

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "kernel/guestDescriptorRange.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/ipmiObserve.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "libs/hostHttp.h"
#include "loader/nativeSslSmoke.h"
#include "loader/runtimeProcessIdentity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Libs::Network {

class Network {
public:
	class Id {
	public:
		static constexpr int MAX_ID = 65536;

		enum class Type : uint32_t {
			Invalid    = 0,
			Http       = 1,
			Ssl        = 2,
			Template   = 3,
			Connection = 4,
			Request    = 5,
		};

		explicit Id(int id)
		    : m_id(static_cast<uint32_t>(id) & 0xffffu), m_type(static_cast<uint32_t>(id) >> 16u) {}
		[[nodiscard]] int ToInt() const {
			return static_cast<int>(m_id + (static_cast<uint32_t>(m_type) << 16u));
		}
		[[nodiscard]] bool IsValid() const { return GetType() != Type::Invalid; }
		[[nodiscard]] Type GetType() const {
			switch (m_type) {
				case static_cast<uint32_t>(Type::Http): return Type::Http;
				case static_cast<uint32_t>(Type::Ssl): return Type::Ssl;
				case static_cast<uint32_t>(Type::Template): return Type::Template;
				case static_cast<uint32_t>(Type::Connection): return Type::Connection;
				case static_cast<uint32_t>(Type::Request): return Type::Request;
				default: return Type::Invalid;
			}
		}

		friend class Network;

	private:
		Id() = default;
		static Id Invalid() { return {}; }
		static Id Create(int net_id, Type type) {
			Id r;
			r.m_id   = net_id;
			r.m_type = static_cast<uint32_t>(type);
			return r;
		}
		[[nodiscard]] int GetId() const { return static_cast<int>(m_id); }

		uint32_t m_id   = 0;
		uint32_t m_type = static_cast<uint32_t>(Type::Invalid);
	};

	using HttpsCallback = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int (*)(int, unsigned int, void* const*, int, void*);

	Network(): m_native_ssl(Loader::NativeSslSmoke::ProductionRequested()) {}
	virtual ~Network() = default;
	bool UsesNativeSsl() const { return m_native_ssl; }

	KYTY_CLASS_NO_COPY(Network);

	int  PoolCreate(const char* name, int size);
	int  SetInitParam(const void* param);
	bool PoolDestroy(int memid);
	int  ResolverCreate(const char* name, int memid);
	bool ResolverDestroy(int rid);
	bool ResolverValid(int rid);
	bool ResolverRetain(int rid, int rid6, std::array<uint64_t, 2>* generations);
	bool ResolverPublish(int rid, int rid6, const std::array<uint64_t, 2>& generations,
	                     Net::NetResolverInfoEx* output, const Net::NetResolverInfoEx& result);
	bool ResolverPublishAddress6(int rid, uint64_t generation, void* output, const void* address);

	Id   SslInit(uint64_t pool_size);
	bool SslTerm(Id ssl_ctx_id);
	bool SslValid(Id ssl_ctx_id);

	Id   HttpInit(int memid, int ssl_ctx_id, uint64_t pool_size);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	bool HttpSslIdForTest(Id http, int* ssl);
#endif
	bool HttpTerm(Id http_ctx_id);
	Id   HttpCreateTemplate(Id http_ctx_id, const char* user_agent, int http_ver,
	                        bool is_auto_proxy_conf);
	bool HttpDeleteTemplate(Id tmpl_id);
	bool HttpSetNonblock(Id id, bool enable);
	bool HttpsSetSslCallback(Id id, HttpsCallback cbfunc, void* user_arg);
	bool HttpsSetMinSslVersion(Id id, uint32_t ssl_version);
	bool HttpsDisableOption(Id id, uint32_t ssl_flags);
	bool HttpAddRequestHeader(Id id, const char* name, const char* value, bool add);
	bool HttpValid(Id http_ctx_id);
	bool HttpValidTemplate(Id tmpl_id);
	bool HttpValidConnection(Id conn_id);
	bool HttpValidRequest(Id req_id);
	bool HttpValidBase(Id id);
	Id HttpCreateConnection(Id tmpl_id, const char* server_name, const char* scheme, uint16_t port,
	                        bool enable_keep_alive);
	Id HttpCreateConnectionWithURL(Id tmpl_id, const char* url, bool enable_keep_alive);
	bool HttpDeleteConnection(Id conn_id);
	Id   HttpCreateRequestWithURL2(Id conn_id, const char* method, const char* url,
	                               uint64_t content_length);
	bool HttpSetRequestContentLength(Id req_id, uint64_t content_length);
	bool HttpDeleteRequest(Id req_id);
	bool HttpSetResolveTimeOut(Id id, uint32_t usec);
	bool HttpSetResolveRetry(Id id, int32_t retry);
	bool HttpSetConnectTimeOut(Id id, uint32_t usec);
	bool HttpSetSendTimeOut(Id id, uint32_t usec);
	bool HttpSetRecvTimeOut(Id id, uint32_t usec);
	bool HttpSetAutoRedirect(Id id, int enable);
	bool HttpSetAuthEnabled(Id id, int enable);
	bool HttpSetCookieEnabled(Id id, int enable);
	bool HttpMarkRequestSent(Id req_id, int result);
	int HttpSend(Id req_id, const void* data, size_t size);
	int HttpRead(Id req_id, void* data, size_t size);
	int HttpWait(Id req_id, int timeout_ms);
	bool HttpGetRequestResponse(Id req_id, int* send_result, int* status_code, const char** headers,
	                            size_t* headers_size, uint64_t* content_length);

private:
	struct Pool {
		bool        used = false;
		std::string name;
		int         size = 0;
	};

	struct Ssl {
		bool     used = false;
		uint64_t size = 0;
	};

	struct Resolver {
		bool        used = false;
		std::string name;
		int         memid = 0;
	};

	struct Http {
		std::shared_ptr<HostHttp::Session> session;
		bool     used       = false;
		uint64_t size       = 0;
		int      memid      = 0;
		int      ssl_ctx_id = 0;
	};

	struct HttpHeader {
		std::string name;
		std::string value;
	};

	struct HttpBase {
		std::shared_ptr<HostHttp::Session> session;
		bool                    cookies_enabled = false;
		std::vector<HttpHeader> headers;
		bool                    used            = false;
		bool                    nonblock        = false;
		bool                    auto_redirect   = true;
		bool                    auth_enabled    = true;
		HttpsCallback           ssl_cbfunc      = nullptr;
		void*                   ssl_user_arg    = nullptr;
		uint32_t                ssl_flags       = 0xA7;
		uint32_t                min_ssl_version = 0;
		int                     http_ctx_id     = 0;
		uint32_t                resolve_timeout = 1'000000;
		int32_t                 resolve_retry   = 4;
		uint32_t                connect_timeout = 30'000000;
		uint32_t                send_timeout    = 120'000000;
		uint32_t                recv_timeout    = 120'000000;
	};

	struct HttpTemplate: public HttpBase {
		std::string user_agent;
		int         http_ver           = 0;
		bool        is_auto_proxy_conf = true;
	};

	struct HttpConnection: public HttpTemplate {
		explicit HttpConnection(const HttpTemplate& tmpl): HttpTemplate(tmpl) {}
		// int    tmpl_id = 0;
		std::string url;
		bool        enable_keep_alive = false;
	};

	struct HttpRequest: public HttpConnection {
		explicit HttpRequest(HttpConnection& conn): HttpConnection(conn) {}
		// int      conn_id = 0;
		std::string method;
		std::string url;
		uint64_t    content_length = 0;
		bool        send_attempted = false;
		int         send_result    = HTTP_ERROR_BEFORE_SEND;
		int         status_code    = 0;
		std::string response_headers;
		std::string response_body;
		size_t read_offset = 0;
		std::shared_ptr<std::shared_future<HostHttp::Response>> transfer;
	};
	static void HttpRefresh(HttpRequest& request);

	HttpBase* FindHttpBase(Id id, bool include_request);

	static constexpr int POOLS_MAX     = 128;
	static constexpr int POOL_ID_BASE  = 3000;
	static int PoolIndex(int memid) {
		// Check before subtraction, including INT_MIN; public IDs have no generation.
		return memid >= POOL_ID_BASE && memid < POOL_ID_BASE + POOLS_MAX ? memid - POOL_ID_BASE : -1;
	}
	static constexpr int RESOLVERS_MAX = 32;
	static constexpr int SSL_MAX       = 32;
	static constexpr int HTTP_MAX      = 32;

	Common::Mutex               m_mutex;
	const bool                  m_native_ssl;
	Pool                        m_pools[POOLS_MAX];
	std::array<uint8_t, 32>      m_init_param {};
	bool                        m_init_param_set = false;
	int                         m_pool_limit = 32;
	Resolver                    m_resolvers[RESOLVERS_MAX];
	uint64_t                    m_resolver_generations[RESOLVERS_MAX] {};
	Ssl                         m_ssl[SSL_MAX];
	Http                        m_http[HTTP_MAX];
	std::vector<HttpTemplate>   m_templates;
	std::vector<HttpConnection> m_connections;
	std::vector<HttpRequest>    m_requests;
};

static Network* g_net = nullptr;

bool NativeSslEnabled() { return g_net && g_net->UsesNativeSsl(); }

void Initialize() {
	EXIT_IF(g_net != nullptr);

	HostHttp::SetConfiguredEnabled(Config::IsNetworkConnected());
	g_net = new Network;
}

void Shutdown() {
	delete g_net;
	g_net = nullptr;
	HostHttp::SetConfiguredEnabled(false);
}

int Network::SetInitParam(const void* param) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	if (m_init_param_set) { return NET_ERROR_EBUSY; }
	if (param == nullptr) { return NET_ERROR_EINVAL; }
	std::memcpy(m_init_param.data(), param, m_init_param.size());
	uint32_t flags = 0;
	std::memcpy(&flags, m_init_param.data(), sizeof(flags));
	m_pool_limit = (flags & 1) != 0 ? POOLS_MAX : 32;
	m_init_param_set = true;
	return OK;
}

int Network::PoolCreate(const char* name, int size) {
	static std::atomic<bool> full_observed {false}; // Once per host process, even across Network recreation.
	std::array<IpmiObserve::NetPoolEntry, IpmiObserve::NetPoolOccupiedLimit> occupied {};
	bool publish = false;
	const bool observe = IpmiObserve::Enabled();
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		for (int id = 0; id < m_pool_limit; id++) {
			if (!m_pools[id].used) {
				m_pools[id].used = true;
				m_pools[id].size = size;
				m_pools[id].name = std::string(name);

				return POOL_ID_BASE + id;
			}
		}
		bool expected = false;
		if (observe && full_observed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
			// Every configured slot is occupied here. Only fixed host bytes copied
			// while holding the same mutex as create/destroy; no observer reads,
			// allocations, logging or callbacks in this snapshot block.
			// Bound the copy to observer capacity; zero tail IDs are skipped by
			// publication when the configured quota is smaller than the array.
			const auto count = std::min(static_cast<size_t>(m_pool_limit), occupied.size());
			for (size_t id = 0; id < count; ++id) {
				auto& entry = occupied[id];
				entry.id = POOL_ID_BASE + static_cast<int>(id); entry.size = m_pools[id].size;
				std::copy_n(m_pools[id].name.data(), std::min(m_pools[id].name.size(), entry.name.size()), entry.name.begin());
			}
			publish = true;
		}
	}
	if (publish) {
		IpmiObserve::NetPoolFullSnapshot(occupied);
	}
	return -1;
}

bool Network::PoolDestroy(int memid) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	const int index = PoolIndex(memid);
	if (index >= 0 && m_pools[index].used) {
		m_pools[index].used = false;

		return true;
	}

	return false;
}

int Network::ResolverCreate(const char* name, int memid) {
	Common::LockGuard lock(m_mutex);

	const int index = PoolIndex(memid);
	if (index < 0 || !m_pools[index].used) {
		return -1;
	}

	for (int id = 0; id < RESOLVERS_MAX; id++) {
		if (!m_resolvers[id].used) {
			m_resolvers[id].used  = true;
			m_resolvers[id].name  = std::string(name);
			m_resolvers[id].memid = memid;
			// Destroy/recreate of the same public ID must not accept an old DNS result.
			if (++m_resolver_generations[id] == 0) { ++m_resolver_generations[id]; }

			return id;
		}
	}

	return -1;
}

bool Network::ResolverDestroy(int rid) {
	Common::LockGuard lock(m_mutex);

	if (rid >= 0 && rid < RESOLVERS_MAX && m_resolvers[rid].used) {
		m_resolvers[rid] = {};
		return true;
	}

	return false;
}

bool Network::ResolverValid(int rid) {
	Common::LockGuard lock(m_mutex);

	return rid >= 0 && rid < RESOLVERS_MAX && m_resolvers[rid].used;
}

bool Network::ResolverRetain(int rid, int rid6, std::array<uint64_t, 2>* generations) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	const auto valid = [this](int id) { return id >= 0 && id < RESOLVERS_MAX && m_resolvers[id].used; };
	if (!valid(rid) || (rid6 != -1 && !valid(rid6))) { return false; }
	(*generations)[0] = m_resolver_generations[rid];
	(*generations)[1] = rid6 != -1 ? m_resolver_generations[rid6] : 0;
	return true;
}

bool Network::ResolverPublish(int rid, int rid6, const std::array<uint64_t, 2>& generations,
                              Net::NetResolverInfoEx* output, const Net::NetResolverInfoEx& result) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	const auto valid = [this](int id, uint64_t generation) {
		return id >= 0 && id < RESOLVERS_MAX && m_resolvers[id].used &&
		       m_resolver_generations[id] == generation;
	};
	if (!valid(rid, generations[0]) || (rid6 != -1 && !valid(rid6, generations[1]))) { return false; }
	// Publication linearizes with destroy/reuse under the registry mutex. No
	// registry pointer or lock is retained across the blocking host resolver.
	*output = result;
	return true;
}

bool Network::ResolverPublishAddress6(int rid, uint64_t generation, void* output, const void* address) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	if (rid < 0 || rid >= RESOLVERS_MAX || !m_resolvers[rid].used ||
	    m_resolver_generations[rid] != generation) { return false; }
	// Unlike the records API, single-address failures preserve all caller bytes.
	// Publish exactly one initialized IPv6 address, linearized with destroy/reuse.
	std::memcpy(output, address, 16);
	return true;
}

Network::Id Network::SslInit(uint64_t pool_size) {
	static_assert(SSL_MAX == IpmiObserve::SslOccupiedLimit);
	static std::atomic<bool> full_observed {false}; // Once per host process, across Network recreation.
	std::array<IpmiObserve::SslEntry, IpmiObserve::SslOccupiedLimit> occupied {};
	bool publish = false;
	const bool observe = IpmiObserve::Enabled();
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		for (int id = 0; id < SSL_MAX; id++) {
			if (!m_ssl[id].used) {
				m_ssl[id].used = true;
				m_ssl[id].size = pool_size;

				return Id::Create(id, Id::Type::Ssl);
			}
		}
		bool expected = false;
		if (observe && full_observed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
			// All32 slots are occupied under the same mutex as init/term. Copy
			// fixed host values only: no allocation, guest reads or publication.
			for (int id = 0; id < SSL_MAX; ++id) {
				occupied[id].id = Id::Create(id, Id::Type::Ssl).ToInt();
				occupied[id].size = m_ssl[id].size;
			}
			publish = true;
		}
	}
	if (publish) { IpmiObserve::SslFullSnapshot(occupied); }
	return Id::Invalid();
}

bool Network::SslTerm(Id ssl_ctx_id) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	if (ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 &&
	    ssl_ctx_id.GetId() < SSL_MAX && m_ssl[ssl_ctx_id.GetId()].used) {
		m_ssl[ssl_ctx_id.GetId()].used = false;

		return true;
	}

	return false;
}

bool Network::SslValid(Id ssl_ctx_id) {
	Common::LockGuard lock(m_mutex);

	return ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 &&
	       ssl_ctx_id.GetId() < SSL_MAX && m_ssl[ssl_ctx_id.GetId()].used;
}

Network::Id Network::HttpInit(int memid, int ssl_ctx_id, uint64_t pool_size) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);

	const int index = PoolIndex(memid);
	const Id legacy_ssl(ssl_ctx_id);
	const bool ssl_admitted = m_native_ssl ||
	    (legacy_ssl.GetType() == Id::Type::Ssl && legacy_ssl.GetId() >= 0 &&
	     legacy_ssl.GetId() < SSL_MAX && m_ssl[legacy_ssl.GetId()].used);
	if (ssl_admitted && index >= 0 && m_pools[index].used) {
		for (int id = 0; id < HTTP_MAX; id++) {
			if (!m_http[id].used) {
				m_http[id].used       = true;
				m_http[id].size       = pool_size;
				// Native HTTP retains the supplied signed ID without an SSL lookup
				// or a permanent context reference. Never decode native opaque IDs.
				m_http[id].ssl_ctx_id = m_native_ssl ? ssl_ctx_id : legacy_ssl.GetId();
				m_http[id].memid      = memid;
				m_http[id].session    = HostHttp::CreateSession();

				return Id::Create(id, Id::Type::Http);
			}
		}
	}

	return Id::Invalid();
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool Network::HttpSslIdForTest(Id http, int* ssl) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	if (!ssl || http.GetType() != Id::Type::Http || http.GetId() < 0 ||
	    http.GetId() >= HTTP_MAX || !m_http[http.GetId()].used) { return false; }
	*ssl = m_http[http.GetId()].ssl_ctx_id;
	return true;
}
bool HttpSslIdForTest(int http, int* ssl) {
	return g_net && g_net->HttpSslIdForTest(Network::Id(http), ssl);
}
#endif

bool Network::HttpValid(Id http_ctx_id) {
	Common::LockGuard lock(m_mutex);

	return (http_ctx_id.GetType() == Id::Type::Http && http_ctx_id.GetId() >= 0 &&
	        http_ctx_id.GetId() < HTTP_MAX && m_http[http_ctx_id.GetId()].used);
}

bool Network::HttpValidTemplate(Id tmpl_id) {
	Common::LockGuard lock(m_mutex);

	return (tmpl_id.GetType() == Id::Type::Template && tmpl_id.GetId() >= 0 &&
	        static_cast<size_t>(tmpl_id.GetId()) < m_templates.size() &&
	        m_templates[tmpl_id.GetId()].used);
}

bool Network::HttpValidConnection(Id conn_id) {
	Common::LockGuard lock(m_mutex);

	return (conn_id.GetType() == Id::Type::Connection && conn_id.GetId() >= 0 &&
	        static_cast<size_t>(conn_id.GetId()) < m_connections.size() &&
	        m_connections[conn_id.GetId()].used);
}

bool Network::HttpValidRequest(Id req_id) {
	Common::LockGuard lock(m_mutex);

	return (req_id.GetType() == Id::Type::Request && req_id.GetId() >= 0 &&
	        static_cast<size_t>(req_id.GetId()) < m_requests.size() &&
	        m_requests[req_id.GetId()].used);
}

bool Network::HttpValidBase(Id id) {
	Common::LockGuard lock(m_mutex);

	return FindHttpBase(id, true) != nullptr;
}

Network::HttpBase* Network::FindHttpBase(Id id, bool include_request) {
	const auto index = id.GetId();

	switch (id.GetType()) {
		case Id::Type::Template:
			if (index >= 0 && static_cast<size_t>(index) < m_templates.size() &&
			    m_templates[index].used) {
				return &m_templates[index];
			}
			break;
		case Id::Type::Connection:
			if (index >= 0 && static_cast<size_t>(index) < m_connections.size() &&
			    m_connections[index].used) {
				return &m_connections[index];
			}
			break;
		case Id::Type::Request:
			if (include_request && index >= 0 && static_cast<size_t>(index) < m_requests.size() &&
			    m_requests[index].used) {
				return &m_requests[index];
			}
			break;
		default: break;
	}

	return nullptr;
}

bool Network::HttpTerm(Id http_ctx_id) {
	std::shared_ptr<HostHttp::Session> retired;
	Common::LockGuard lock(m_mutex);

	if (HttpValid(http_ctx_id)) {
		m_http[http_ctx_id.GetId()].used = false;
		retired = std::move(m_http[http_ctx_id.GetId()].session);

		return true;
	}

	return false;
}

Network::Id Network::HttpCreateTemplate(Id http_ctx_id, const char* user_agent, int http_ver,
                                        bool is_auto_proxy_conf) {
	Common::LockGuard lock(m_mutex);

	if (HttpValid(http_ctx_id)) {
		HttpTemplate tn {};
		tn.session            = m_http[http_ctx_id.GetId()].session;
		tn.used               = true;
		tn.http_ver           = http_ver;
		tn.user_agent         = user_agent != nullptr ? user_agent : "";
		tn.is_auto_proxy_conf = is_auto_proxy_conf;
		tn.http_ctx_id        = http_ctx_id.GetId();
		tn.nonblock           = false;

		int index = 0;
		for (auto& t: m_templates) {
			if (!t.used) {
				t = tn;
				return Id::Create(index, Id::Type::Template);
			}
			index++;
		}

		if (index < Id::MAX_ID) {
			m_templates.push_back(tn);
			return Id::Create(index, Id::Type::Template);
		}
	}

	return Id::Invalid();
}

Network::Id Network::HttpCreateConnectionWithURL(Id tmpl_id, const char* url,
                                                 bool enable_keep_alive) {
	Common::LockGuard lock(m_mutex);

	if (url != nullptr && url[0] != '\0' && HttpValidTemplate(tmpl_id)) {
		HttpConnection cn(m_templates[tmpl_id.GetId()]);
		cn.used              = true;
		cn.enable_keep_alive = enable_keep_alive;
		cn.url               = std::string(url);
		// cn.tmpl_id           = tmpl_id.ToInt();

		int index = 0;
		for (auto& t: m_connections) {
			if (!t.used) {
				t = cn;
				return Id::Create(index, Id::Type::Connection);
			}
			index++;
		}

		if (index < Id::MAX_ID) {
			m_connections.push_back(cn);
			return Id::Create(index, Id::Type::Connection);
		}
	}

	return Id::Invalid();
}

Network::Id Network::HttpCreateConnection(Id tmpl_id, const char* server_name, const char* scheme,
                                          uint16_t port, bool enable_keep_alive) {
	Common::LockGuard lock(m_mutex);

	if (server_name != nullptr && scheme != nullptr && HttpValidTemplate(tmpl_id)) {
		HttpConnection cn(m_templates[tmpl_id.GetId()]);
		cn.used              = true;
		cn.enable_keep_alive = enable_keep_alive;
		cn.url               = std::string(scheme) + "://" + std::string(server_name);
		if (port != 0) {
			cn.url = cn.url + fmt::format(":{}", static_cast<unsigned>(port));
		}

		int index = 0;
		for (auto& t: m_connections) {
			if (!t.used) {
				t = cn;
				return Id::Create(index, Id::Type::Connection);
			}
			index++;
		}

		if (index < Id::MAX_ID) {
			m_connections.push_back(cn);
			return Id::Create(index, Id::Type::Connection);
		}
	}

	return Id::Invalid();
}

bool Network::HttpDeleteConnection(Id conn_id) {
	std::shared_ptr<HostHttp::Session> retired;
	Common::LockGuard lock(m_mutex);

	if (HttpValidConnection(conn_id)) {
		m_connections[conn_id.GetId()].used = false;
		retired = std::move(m_connections[conn_id.GetId()].session);

		return true;
	}

	return false;
}

Network::Id Network::HttpCreateRequestWithURL2(Id conn_id, const char* method, const char* url,
                                               uint64_t content_length) {
	Common::LockGuard lock(m_mutex);

	if (method != nullptr && url != nullptr && url[0] != '\0' && HttpValidConnection(conn_id)) {
		HttpRequest cn(m_connections[conn_id.GetId()]);
		cn.used   = true;
		cn.method = std::string(method);
		cn.url    = std::string(url);
		// cn.conn_id        = conn_id.ToInt();
		cn.content_length = content_length;

		int index = 0;
		for (auto& t: m_requests) {
			if (!t.used) {
				t = cn;
				return Id::Create(index, Id::Type::Request);
			}
			index++;
		}

		if (index < Id::MAX_ID) {
			m_requests.push_back(cn);
			return Id::Create(index, Id::Type::Request);
		}
	}

	return Id::Invalid();
}

bool Network::HttpDeleteRequest(Id req_id) {
	std::shared_ptr<HostHttp::Session> retired;
	Common::LockGuard lock(m_mutex);

	if (HttpValidRequest(req_id)) {
		m_requests[req_id.GetId()].used = false;
		retired = std::move(m_requests[req_id.GetId()].session);

		return true;
	}

	return false;
}

bool Network::HttpSetRequestContentLength(Id req_id, uint64_t content_length) {
	Common::LockGuard lock(m_mutex);

	if (HttpValidRequest(req_id)) {
		m_requests[req_id.GetId()].content_length = content_length;
		return true;
	}

	return false;
}

bool Network::HttpMarkRequestSent(Id req_id, int result) {
	Common::LockGuard lock(m_mutex);

	if (req_id.GetType() == Id::Type::Request && req_id.GetId() >= 0 &&
	    static_cast<size_t>(req_id.GetId()) < m_requests.size() &&
	    m_requests[req_id.GetId()].used) {
		auto& request          = m_requests[req_id.GetId()];
		request.send_attempted = true;
		request.send_result    = result;
		request.status_code    = 0;
		request.response_headers.clear();
		request.response_body.clear();
		request.read_offset = 0;
		request.transfer.reset();
		return true;
	}

	return false;
}

void Network::HttpRefresh(HttpRequest& request) {
	if (!request.transfer || request.send_result != HTTP_ERROR_EAGAIN ||
	    request.transfer->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	const auto& response = request.transfer->get();
	request.send_result = HostHttp::GuestError(response.error);
	request.status_code = response.status;
	request.response_headers = response.headers;
	request.response_body = response.body;
}

int Network::HttpSend(Id req_id, const void* data, size_t size) {
	if (data == nullptr && size != 0) return HTTP_ERROR_INVALID_VALUE;
	std::shared_ptr<std::shared_future<HostHttp::Response>> transfer;
	bool nonblock = false;
	{
		Common::LockGuard lock(m_mutex);
		if (!HttpValidRequest(req_id)) return HTTP_ERROR_INVALID_ID;
		auto& request = m_requests[req_id.GetId()];
		if (request.send_attempted) {
			HttpRefresh(request);
			return request.send_result;
		}
		request.send_attempted = true;
		if (!HostHttp::Enabled()) return request.send_result = HTTP_ERROR_TIMEOUT;
		HostHttp::Request host;
		host.session = request.session;
		host.cookies = request.cookies_enabled;
		host.url = request.url;
		if (host.url.find("://") == std::string::npos) {
			const auto& base = request.HttpConnection::url;
			const auto scheme = base.find("://");
			const auto end = scheme == std::string::npos ? std::string::npos : base.find('/', scheme + 3);
			host.url = base.substr(0, end) + (host.url.starts_with('/') ? "" : "/") + host.url;
		}
		host.method = request.method;
		host.user_agent = request.user_agent;
		host.redirect = request.auto_redirect;
		host.resolve_timeout_ms = std::max(1u, request.resolve_timeout / 1000u);
		host.connect_timeout_ms = std::max(1u, request.connect_timeout / 1000u);
		host.send_timeout_ms = std::max(1u, request.send_timeout / 1000u);
		host.recv_timeout_ms = std::max(1u, request.recv_timeout / 1000u);
		for (const auto& header: request.headers) host.headers.emplace_back(header.name, header.value);
		if (size) host.body.assign(static_cast<const char*>(data), size);
		transfer = std::make_shared<std::shared_future<HostHttp::Response>>(HostHttp::Start(std::move(host)));
		request.transfer = transfer;
		request.send_result = HTTP_ERROR_EAGAIN;
		nonblock = request.nonblock;
	}
	if (nonblock) return HTTP_ERROR_EAGAIN;
	transfer->wait();
	Common::LockGuard lock(m_mutex);
	if (!HttpValidRequest(req_id)) return HTTP_ERROR_INVALID_ID;
	auto& request = m_requests[req_id.GetId()];
	if (request.transfer != transfer) return HTTP_ERROR_ABORTED;
	HttpRefresh(request);
	LOGF("Host HTTP: request=%d status=%d bytes=%zu result=0x%08x\n",
	     req_id.ToInt(), request.status_code, request.response_body.size(), request.send_result);
	return request.send_result;
}

int Network::HttpRead(Id req_id, void* data, size_t size) {
	if (!data && size) return HTTP_ERROR_INVALID_VALUE;
	Common::LockGuard lock(m_mutex);
	if (!HttpValidRequest(req_id)) return HTTP_ERROR_INVALID_ID;
	auto& request = m_requests[req_id.GetId()];
	HttpRefresh(request);
	if (request.send_result != 0) return request.send_result;
	const auto count = std::min({size, request.response_body.size() - request.read_offset, static_cast<size_t>(INT_MAX)});
	if (count) std::memcpy(data, request.response_body.data() + request.read_offset, count);
	request.read_offset += count;
	return static_cast<int>(count);
}

int Network::HttpWait(Id req_id, int timeout_ms) {
	std::shared_ptr<std::shared_future<HostHttp::Response>> transfer;
	{
		Common::LockGuard lock(m_mutex);
		if (!HttpValidRequest(req_id)) return HTTP_ERROR_INVALID_ID;
		auto& request = m_requests[req_id.GetId()];
		HttpRefresh(request);
		if (request.send_result != HTTP_ERROR_EAGAIN) return request.send_result;
		transfer = request.transfer;
	}
	if (!transfer) return HTTP_ERROR_BEFORE_SEND;
	if (timeout_ms < 0) transfer->wait();
	else if (transfer->wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
		return HTTP_ERROR_EAGAIN;
	Common::LockGuard lock(m_mutex);
	if (!HttpValidRequest(req_id)) return HTTP_ERROR_INVALID_ID;
	auto& request = m_requests[req_id.GetId()];
	HttpRefresh(request);
	return request.send_result;
}

bool Network::HttpGetRequestResponse(Id req_id, int* send_result, int* status_code,
                                     const char** headers, size_t* headers_size,
                                     uint64_t* content_length) {
	Common::LockGuard lock(m_mutex);

	if (req_id.GetType() != Id::Type::Request || req_id.GetId() < 0 ||
	    static_cast<size_t>(req_id.GetId()) >= m_requests.size() ||
	    !m_requests[req_id.GetId()].used) {
		return false;
	}

	auto& request = m_requests[req_id.GetId()];
	HttpRefresh(request);

	if (send_result != nullptr) {
		*send_result = (request.send_attempted ? request.send_result : HTTP_ERROR_BEFORE_SEND);
	}
	if (status_code != nullptr) {
		*status_code = request.status_code;
	}
	if (headers != nullptr) {
		*headers = request.response_headers.c_str();
	}
	if (headers_size != nullptr) {
		*headers_size = request.response_headers.size();
	}
	if (content_length != nullptr) {
		*content_length = request.response_body.size();
	}

	return true;
}

bool Network::HttpDeleteTemplate(Id tmpl_id) {
	std::shared_ptr<HostHttp::Session> retired;
	Common::LockGuard lock(m_mutex);

	if (HttpValidTemplate(tmpl_id)) {
		m_templates[tmpl_id.GetId()].used = false;
		retired = std::move(m_templates[tmpl_id.GetId()].session);

		return true;
	}

	return false;
}

bool Network::HttpSetNonblock(Id id, bool enable) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->nonblock = enable;
		return true;
	}

	return false;
}

bool Network::HttpsSetSslCallback(Id id, HttpsCallback cbfunc, void* user_arg) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->ssl_cbfunc   = cbfunc;
		base->ssl_user_arg = user_arg;
		return true;
	}

	return false;
}

bool Network::HttpsSetMinSslVersion(Id id, uint32_t ssl_version) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->min_ssl_version = ssl_version;
		return true;
	}

	return false;
}

bool Network::HttpsDisableOption(Id id, uint32_t ssl_flags) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->ssl_flags &= ~ssl_flags;
		return true;
	}

	return false;
}

bool Network::HttpAddRequestHeader(Id id, const char* name, const char* value, bool add) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		HttpHeader nh({std::string(name), std::string(value)});
		if (add) {
			base->headers.push_back(nh);
		} else {
			for (auto& h: base->headers) {
				if (h.name == nh.name) {
					h.value = nh.value;
				}
			}
		}
		return true;
	}

	return false;
}

bool Network::HttpSetResolveTimeOut(Id id, uint32_t usec) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, false);

	if (base != nullptr) {
		base->resolve_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetResolveRetry(Id id, int32_t retry) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, false);

	if (base != nullptr) {
		base->resolve_retry = retry;
		return true;
	}

	return false;
}

bool Network::HttpSetConnectTimeOut(Id id, uint32_t usec) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->connect_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetSendTimeOut(Id id, uint32_t usec) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->send_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetRecvTimeOut(Id id, uint32_t usec) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->recv_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetAutoRedirect(Id id, int enable) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->auto_redirect = (enable != 0);
		return true;
	}

	return false;
}

bool Network::HttpSetAuthEnabled(Id id, int enable) {
	Common::LockGuard lock(m_mutex);

	HttpBase* base = FindHttpBase(id, true);

	if (base != nullptr) {
		base->auth_enabled = (enable != 0);
		return true;
	}

	return false;
}

bool Network::HttpSetCookieEnabled(Id id, int enable) {
	Common::LockGuard lock(m_mutex);
	auto* base = FindHttpBase(id, true);
	if (!base) { return false; }
	base->cookies_enabled = enable != 0;
	return true;
}

namespace Net {

LIB_NAME("Net", "Net");

#if defined(_WIN32)
using NativeSocket                                  = SOCKET;
static constexpr NativeSocket INVALID_NATIVE_SOCKET = INVALID_SOCKET;
using SocketLength                                  = int;
using SocketIoLength                                = int;
#else
using NativeSocket                                  = int;
static constexpr NativeSocket INVALID_NATIVE_SOCKET = -1;
using SocketLength                                  = socklen_t;
using SocketIoLength                                = size_t;
#endif

struct SocketPairState {
	std::atomic_bool closed[2] {};
	std::atomic_bool read_shutdown[2] {};
	std::atomic_bool write_shutdown[2] {};
};
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static std::atomic_int g_pair_host_sockets {0};
static std::atomic_int g_pair_paths {0};
#endif

struct SocketTransport {
	SocketTransport(NativeSocket value, int socket_type = SOCK_STREAM): socket(value), type(socket_type) {}
	~SocketTransport() {
		Close();
		// Only auto-bound paths belong to this transport. Guest bind paths retain
		// their existing unlink semantics. Final release is outside table locks.
		if (!unix_private_path.empty()) {
			std::error_code error;
			std::filesystem::remove(unix_private_path, error);
		}
	}
	bool HasPendingReceive() const {
#if defined(_WIN32)
		return buffered_bytes.load(std::memory_order_relaxed) != 0 ||
		       receive_shutdown.load(std::memory_order_relaxed);
#else
		return false;
#endif
	}
	int Close() {
#if defined(_WIN32)
		receive_shutdown.store(true, std::memory_order_relaxed);
#endif
		const auto value = socket.exchange(INVALID_NATIVE_SOCKET);
		if (value == INVALID_NATIVE_SOCKET) {
			return 0;
		}
#if defined(_WIN32)
		const int result = closesocket(value);
#else
		const int result = ::close(value);
#endif
		if (pair && result == 0) { pair->closed[pair_index].store(true); }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		// A failed close must remain visible to the leak assertions.
		if (pair_owned && result == 0) { --g_pair_host_sockets; }
#endif
		return result;
	}
	std::shared_ptr<SocketPairState> pair;
	unsigned pair_index = 0;
	bool pair_owned = false; // includes the unpublished construction listener
	// Mode changes serialize per retained transport, never across the table lock.
	Common::Mutex mode_mutex;
	std::atomic_bool pair_nonblocking {false};
	// Immutable guest name; pointer snapshots/replacement use g_socket_mutex.
	// Allocation and last-owner destruction take place outside the table lock.
	std::shared_ptr<const std::string> unix_guest_path;
	std::shared_ptr<const std::string> unix_bound_host_path; // guarded by g_socket_mutex
	std::string unix_private_path; // set once under unix_bind_mutex, freed on final release
	Common::Mutex unix_bind_mutex;
	int owner_pid = -1; // assigned from creation context, never from accept's thread
	std::atomic_int peer_pid {-1}; // no unmatched-peer ShellUI fallback
	std::atomic<NativeSocket> socket;
	int                      type;
#if defined(_WIN32)
	std::mutex         receive_mutex;
	std::vector<char>  receive_buffer;
	std::atomic_size_t buffered_bytes   = 0;
	std::atomic_bool   receive_shutdown = false;
#endif
};

struct P2pEndpoint {
	bool     bound          = false;
	uint32_t address        = 0;
	uint16_t port           = 0;
	uint16_t virtual_port   = 0;
	int      nonblocking    = 0;
	int      broadcast      = 0;
	int      use_crypto     = 0;
	int      send_buffer    = 9216;
	int      receive_buffer = 40 * 1024;
};

struct SocketSlot {
	std::shared_ptr<SocketTransport> transport;
	std::optional<P2pEndpoint>       p2p;
	bool cloexec = false;
	uint64_t generation = 0;
};

struct NetTimeval {
	int64_t tv_sec;
	int64_t tv_usec;
};

static constexpr int                         SOCKET_FD_MIN = GuestDescriptors::SocketBegin;
static constexpr int                         SOCKET_FD_MAX = GuestDescriptors::SocketEnd;
static Common::Mutex                         g_socket_mutex;
static std::array<SocketSlot, SOCKET_FD_MAX> g_sockets;
static uint64_t g_socket_generation = 0; // guarded by g_socket_mutex
struct P2pBindAttempt {
	uint32_t address = 0;
	uint16_t port = 0;
	std::atomic_bool complete {false};
};
// Reservations survive descriptor close while host bind is in flight. Waiting
// binders retain the attempt and wait outside all table locks/critical guards.
static std::array<std::shared_ptr<P2pBindAttempt>, SOCKET_FD_MAX> g_p2p_bind_attempts;
static std::atomic_bool                      g_winsock_initialized = false;

constexpr int      EPOLL_ID_BASE = SOCKET_FD_MAX;
constexpr uint32_t EPOLL_IN      = 0x00000001;
constexpr uint32_t EPOLL_OUT     = 0x00000002;
constexpr uint32_t EPOLL_ERR     = 0x00000008;

struct EpollRegistration {
	int           id = -1;
	NetEpollEvent event {};
};

struct EpollSlot {
	bool                           used = false;
	std::string                    name;
	std::vector<EpollRegistration> registrations;
};

static std::mutex                g_epoll_mutex;
static std::condition_variable   g_epoll_changed;
static std::array<EpollSlot, 32> g_epolls;

static EpollSlot* GetEpollSlot(int eid) {
	const auto index = eid - EPOLL_ID_BASE;
	if (index < 0 || index >= static_cast<int>(g_epolls.size())) {
		return nullptr;
	}
	auto& slot = g_epolls[static_cast<size_t>(index)];
	return slot.used ? &slot : nullptr;
}

static void RemoveSocketFromEpolls(int id) {
	std::lock_guard lock(g_epoll_mutex);
	for (auto& epoll: g_epolls) {
		if (!epoll.used) {
			continue;
		}
		std::erase_if(epoll.registrations,
		              [id](const auto& registration) { return registration.id == id; });
	}
	g_epoll_changed.notify_all();
}

static bool EnsureSocketBackend() {
#if defined(_WIN32)
	if (!g_winsock_initialized.load()) {
		WSADATA data {};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
			return false;
		}
		g_winsock_initialized = true;
	}
#endif
	return true;
}

static int SetGuestSocketError(int error) {
	*Posix::GetErrorAddr() = error;
	return -1;
}

static int ConvertHostSocketError(int error) {
	if (error == 0) {
		return 0;
	}
	int posix_error = Posix::POSIX_EIO;
#if defined(_WIN32)
	switch (error) {
		case WSAEACCES: posix_error = Posix::POSIX_EACCES; break;
		case WSAEADDRINUSE: posix_error = Posix::POSIX_EADDRINUSE; break;
		case WSAEADDRNOTAVAIL: posix_error = Posix::POSIX_EADDRNOTAVAIL; break;
		case WSAEAFNOSUPPORT: posix_error = Posix::POSIX_EAFNOSUPPORT; break;
		case WSAEFAULT: posix_error = Posix::POSIX_EFAULT; break;
		case WSAEINTR: posix_error = Posix::POSIX_EINTR; break;
		case WSAEINVAL: posix_error = Posix::POSIX_EINVAL; break;
		case WSAEISCONN: posix_error = Posix::POSIX_EISCONN; break;
		case WSAEMFILE: posix_error = Posix::POSIX_EMFILE; break;
		case WSAEMSGSIZE: posix_error = Posix::POSIX_EMSGSIZE; break;
		case WSAENOBUFS: posix_error = Posix::POSIX_ENOBUFS; break;
		case WSAENETDOWN: posix_error = Posix::POSIX_ENETDOWN; break;
		case WSAENETRESET: posix_error = Posix::POSIX_ENETRESET; break;
		case WSAENETUNREACH: posix_error = Posix::POSIX_ENETUNREACH; break;
		case WSAENOTCONN: posix_error = Posix::POSIX_ENOTCONN; break;
		case WSAENOTSOCK: posix_error = Posix::POSIX_ENOTSOCK; break;
		case WSAENOPROTOOPT: posix_error = Posix::POSIX_ENOPROTOOPT; break;
		case WSAEOPNOTSUPP: posix_error = Posix::POSIX_EOPNOTSUPP; break;
		case WSAEPROTONOSUPPORT: posix_error = Posix::POSIX_EPROTONOSUPPORT; break;
		case WSAESHUTDOWN: posix_error = Posix::POSIX_ESHUTDOWN; break;
		case WSAETIMEDOUT: posix_error = Posix::POSIX_ETIMEDOUT; break;
		case WSAEWOULDBLOCK: posix_error = Posix::POSIX_EWOULDBLOCK; break;
		case WSAECONNABORTED: posix_error = Posix::POSIX_ECONNABORTED; break;
		case WSAECONNREFUSED: posix_error = Posix::POSIX_ECONNREFUSED; break;
		case WSAECONNRESET: posix_error = Posix::POSIX_ECONNRESET; break;
		case WSAEDESTADDRREQ: posix_error = Posix::POSIX_EDESTADDRREQ; break;
		case WSAEHOSTUNREACH: posix_error = Posix::POSIX_EHOSTUNREACH; break;
		case WSAEINPROGRESS: posix_error = Posix::POSIX_EINPROGRESS; break;
		case WSAEALREADY: posix_error = Posix::POSIX_EALREADY; break;
		case WSAENAMETOOLONG: posix_error = Posix::POSIX_ENAMETOOLONG; break;
		default: break;
	}
#else
	switch (error) {
		case EACCES: posix_error = Posix::POSIX_EACCES; break;
		case EADDRINUSE: posix_error = Posix::POSIX_EADDRINUSE; break;
		case EADDRNOTAVAIL: posix_error = Posix::POSIX_EADDRNOTAVAIL; break;
		case EAFNOSUPPORT: posix_error = Posix::POSIX_EAFNOSUPPORT; break;
		case EAGAIN: posix_error = Posix::POSIX_EWOULDBLOCK; break;
		case EBADF: posix_error = Posix::POSIX_EBADF; break;
		case EFAULT: posix_error = Posix::POSIX_EFAULT; break;
		case EINTR: posix_error = Posix::POSIX_EINTR; break;
		case EINVAL: posix_error = Posix::POSIX_EINVAL; break;
		case EISCONN: posix_error = Posix::POSIX_EISCONN; break;
		case EMFILE: posix_error = Posix::POSIX_EMFILE; break;
		case EMSGSIZE: posix_error = Posix::POSIX_EMSGSIZE; break;
		case ENFILE: posix_error = Posix::POSIX_ENFILE; break;
		case ENOBUFS: posix_error = Posix::POSIX_ENOBUFS; break;
		case ENOMEM: posix_error = Posix::POSIX_ENOMEM; break;
		case ENETDOWN: posix_error = Posix::POSIX_ENETDOWN; break;
		case ENETRESET: posix_error = Posix::POSIX_ENETRESET; break;
		case ENETUNREACH: posix_error = Posix::POSIX_ENETUNREACH; break;
		case ENOTCONN: posix_error = Posix::POSIX_ENOTCONN; break;
		case ENOTSOCK: posix_error = Posix::POSIX_ENOTSOCK; break;
		case ENOPROTOOPT: posix_error = Posix::POSIX_ENOPROTOOPT; break;
		case EOPNOTSUPP: posix_error = Posix::POSIX_EOPNOTSUPP; break;
		case EPIPE: posix_error = Posix::POSIX_EPIPE; break;
		case EPROTONOSUPPORT: posix_error = Posix::POSIX_EPROTONOSUPPORT; break;
		case ESHUTDOWN: posix_error = Posix::POSIX_ESHUTDOWN; break;
		case ETIMEDOUT: posix_error = Posix::POSIX_ETIMEDOUT; break;
		case ECONNABORTED: posix_error = Posix::POSIX_ECONNABORTED; break;
		case ECONNREFUSED: posix_error = Posix::POSIX_ECONNREFUSED; break;
		case ECONNRESET: posix_error = Posix::POSIX_ECONNRESET; break;
		case EDESTADDRREQ: posix_error = Posix::POSIX_EDESTADDRREQ; break;
		case EHOSTUNREACH: posix_error = Posix::POSIX_EHOSTUNREACH; break;
		case EINPROGRESS: posix_error = Posix::POSIX_EINPROGRESS; break;
		case EALREADY: posix_error = Posix::POSIX_EALREADY; break;
		case ENAMETOOLONG: posix_error = Posix::POSIX_ENAMETOOLONG; break;
		case ENOENT: posix_error = Posix::POSIX_ENOENT; break;
		default: break;
	}
#endif
	return posix_error;
}

static int SetHostSocketError() {
#if defined(_WIN32)
	return SetGuestSocketError(ConvertHostSocketError(WSAGetLastError()));
#else
	return SetGuestSocketError(ConvertHostSocketError(errno));
#endif
}

static int ConvertFamily(int family) {
	switch (family) {
		case 1: return AF_UNIX; // PF_LOCAL: AgcCompositor <-> libSceComposite transport
		case 2: return AF_INET;
		case 28: return AF_INET6;
		default: return -1;
	}
}

static int ConvertSocketOptionLevel(int level) {
	if (level == 41) { return IPPROTO_IPV6; }
	return (level == 0xffff ? SOL_SOCKET : level);
}

static int ConvertSocketOptionName(int level, int option) {
	if (level == 41) {
		// BSD/Prospero IPV6_V6ONLY is 27, unlike Winsock (and Linux).
		// Never pass unknown IPv6 option numbers through to a different host option.
		return option == 27 ? IPV6_V6ONLY : -1;
	}
	if (level == 0xffff) {
		switch (option) {
			case 0x0020: return SO_BROADCAST;
			case 0x1001: return SO_SNDBUF;
			case 0x1002: return SO_RCVBUF;
			case 0x1007: return SO_ERROR;
			case 0x1105: return SO_SNDTIMEO;
#if defined(_WIN32)
			// Preserve the existing Winsock-compatible BSD options explicitly.
			// Unknown guest names must not leak Winsock's generic WSAEINVAL.
			case 0x0001: return SO_DEBUG;
			case 0x0002: return SO_ACCEPTCONN;
			case 0x0004: return SO_REUSEADDR;
			case 0x0008: return SO_KEEPALIVE;
			case 0x0010: return SO_DONTROUTE;
			case 0x0080: return SO_LINGER;
			case 0x0100: return SO_OOBINLINE;
			case 0x1003: return SO_SNDLOWAT;
			case 0x1004: return SO_RCVLOWAT;
			case 0x1005: return SO_SNDTIMEO;
			case 0x1006: return SO_RCVTIMEO;
			case 0x1008: return SO_TYPE;
#endif
			default: return -1;
		}
	} else if (level == 6 && option == 1) {
		return TCP_NODELAY;
	}
#if defined(_WIN32)
	return option;
#else
	return -1;
#endif
}

static int* P2pSocketOption(P2pEndpoint& endpoint, int option) {
	switch (option) {
		case 0x0020: return &endpoint.broadcast;
		case 0x1001: return &endpoint.send_buffer;
		case 0x1002: return &endpoint.receive_buffer;
		case 0x1200: return &endpoint.nonblocking;
		case 0x20000: return &endpoint.use_crypto;
		default: return nullptr;
	}
}

static int ConvertMessageFlags(int flags) {
	constexpr int guest_msg_peek      = 0x00000002;
	constexpr int guest_msg_dontroute = 0x00000004;
	constexpr int guest_msg_waitall   = 0x00000040;
	constexpr int guest_msg_dontwait  = 0x00000080;
	constexpr int guest_msg_nosignal  = 0x00020000;

	int host_flags = 0;
	if ((flags & guest_msg_peek) != 0) {
		host_flags |= MSG_PEEK;
	}
	if ((flags & guest_msg_dontroute) != 0) {
		host_flags |= MSG_DONTROUTE;
	}
	if ((flags & guest_msg_waitall) != 0) {
		host_flags |= MSG_WAITALL;
	}
#if !defined(_WIN32)
	if ((flags & guest_msg_dontwait) != 0) {
		host_flags |= MSG_DONTWAIT;
	}
	if ((flags & guest_msg_nosignal) != 0) {
		host_flags |= MSG_NOSIGNAL;
	}
#endif

	flags &= ~(guest_msg_peek | guest_msg_dontroute | guest_msg_waitall | guest_msg_dontwait |
	           guest_msg_nosignal);
	if (flags != 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP;
		return -1;
	}

	return host_flags;
}

// Guest (Orbis/BSD) sockaddr_un: { u8 sun_len; u8 sun_family = 1; char sun_path[104]; }.
static constexpr uint32_t GUEST_SOCKADDR_UN_PATH_OFFSET = 2;
static constexpr uint32_t GUEST_SOCKADDR_UN_SIZE        = 106;
// BSD struct sockaddr (16 bytes): what an unnamed AF_LOCAL endpoint reports.
static constexpr uint32_t GUEST_SOCKADDR_NONAME_SIZE = 16;

static int ConvertGuestUnixSockaddr(const uint8_t* bytes, uint32_t addrlen, sockaddr_storage* out,
                                    SocketLength* out_len, std::string* guest_path_out) {
	// BSD rejects an address larger than sockaddr_un; the path is never truncated.
	if (addrlen > GUEST_SOCKADDR_UN_SIZE) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	// The path is NUL-terminated inside the supplied length, or fills it completely.
	const uint32_t capacity = (addrlen > GUEST_SOCKADDR_UN_PATH_OFFSET
	                               ? addrlen - GUEST_SOCKADDR_UN_PATH_OFFSET
	                               : 0);
	const auto*    path     = bytes + GUEST_SOCKADDR_UN_PATH_OFFSET;
	const auto*    nul      = capacity != 0 ? static_cast<const uint8_t*>(std::memchr(path, 0, capacity)) : nullptr;
	const size_t   length   = (nul != nullptr ? static_cast<size_t>(nul - path) : capacity);
	if (length == 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOENT;
		return -1;
	}

	const std::string guest_path(reinterpret_cast<const char*>(path), length);
	const auto        host_path = LibKernel::FileSystem::GetRealFilename(guest_path);
	if (host_path.empty()) {
		// Unmounted guest paths must not reach the host filesystem.
		LOGF("\t AF_UNIX path = %s is not mounted\n", guest_path.c_str());
		*Posix::GetErrorAddr() = Posix::POSIX_ENOENT;
		return -1;
	}
	const auto host_str = Common::PathToString(host_path);

	std::memset(out, 0, sizeof(*out));
	auto* un = reinterpret_cast<sockaddr_un*>(out);
	if (host_str.empty() || host_str.size() >= sizeof(un->sun_path)) {
		*Posix::GetErrorAddr() = host_str.empty() ? Posix::POSIX_ENOENT : Posix::POSIX_ENAMETOOLONG;
		return -1;
	}
	un->sun_family = AF_UNIX;
	std::memcpy(un->sun_path, host_str.c_str(), host_str.size() + 1);
	*out_len = static_cast<SocketLength>(offsetof(sockaddr_un, sun_path) + host_str.size() + 1);
	if (guest_path_out != nullptr) {
		*guest_path_out = guest_path;
	}
	LOGF("\t AF_UNIX path = %s -> %s\n", guest_path.c_str(), un->sun_path);
	return 0;
}

static int ConvertGuestSockaddr(const void* addr, uint32_t addrlen, sockaddr_storage* out,
                                SocketLength* out_len, std::string* unix_guest_path = nullptr) {
	EXIT_IF(out == nullptr);
	EXIT_IF(out_len == nullptr);

	if (addr == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}

	const auto* bytes = static_cast<const uint8_t*>(addr);
	if (addrlen < 2) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	if (bytes[1] == 1) {
		return ConvertGuestUnixSockaddr(bytes, addrlen, out, out_len, unix_guest_path);
	}

	const int family = ConvertFamily(bytes[1]);
	if (family == AF_INET6) {
		// SceNetSockaddrIn6 / BSD sockaddr_in6: len/family, port, flowinfo,
		// 16 address bytes, scope ID. Port and flowinfo retain network byte order;
		// scope ID is host order. Do not reinterpret a guest structure as Winsock.
		if (addrlen < 28) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		std::memset(out, 0, sizeof(*out));
		auto* in6 = reinterpret_cast<sockaddr_in6*>(out);
		in6->sin6_family = AF_INET6;
		std::memcpy(&in6->sin6_port, bytes + 2, 2);
		std::memcpy(&in6->sin6_flowinfo, bytes + 4, 4);
		std::memcpy(&in6->sin6_addr, bytes + 8, 16);
		std::memcpy(&in6->sin6_scope_id, bytes + 24, 4);
		*out_len = sizeof(sockaddr_in6);
		return 0;
	}
	if (family != AF_INET) {
		*Posix::GetErrorAddr() = (family < 0 ? Posix::POSIX_EAFNOSUPPORT : Posix::POSIX_EOPNOTSUPP);
		return -1;
	}

	if (addrlen < 8) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	std::memset(out, 0, sizeof(*out));
	auto* in       = reinterpret_cast<sockaddr_in*>(out);
	in->sin_family = AF_INET;
	std::memcpy(&in->sin_port, bytes + 2, sizeof(in->sin_port));
	std::memcpy(&in->sin_addr, bytes + 4, sizeof(in->sin_addr));
	*out_len = sizeof(sockaddr_in);
	return 0;
}

static std::string UnixHostPath(const sockaddr_storage& address, SocketLength length) {
	if (address.ss_family != AF_UNIX || length <= static_cast<SocketLength>(offsetof(sockaddr_un, sun_path))) { return {}; }
	const auto& un = reinterpret_cast<const sockaddr_un&>(address);
	const auto capacity = std::min<size_t>(sizeof(un.sun_path), length - offsetof(sockaddr_un, sun_path));
	const auto* end = static_cast<const char*>(std::memchr(un.sun_path, 0, capacity));
	return std::string(un.sun_path, end != nullptr ? static_cast<size_t>(end - un.sun_path) : capacity);
}

static int UnixEndpointOwner(const std::string& path) {
	if (path.empty()) { return -1; }
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_socket_mutex);
	const SocketTransport* found = nullptr;
	for (const auto& slot: g_sockets) {
		if (slot.transport && slot.transport->socket != INVALID_NATIVE_SOCKET &&
		    slot.transport->unix_bound_host_path && *slot.transport->unix_bound_host_path == path) {
			// Unlink/rebind can leave two live sockets with the same textual name.
			// Descriptor order must never resolve an ambiguous path to ShellUI.
			if (found != nullptr && found != slot.transport.get()) { return -1; }
			found = slot.transport.get();
		}
	}
	return found != nullptr ? found->owner_pid : -1;
}

// Every HLE Unix client, including the boot session, receives a distinguishable
// endpoint. Unmatched/closed-before-accept clients fail identity queries; they
// never acquire the privileged boot identity. Repeated connect uses the same
// bound endpoint, including after ECONNREFUSED or EINPROGRESS.
static int EnsureUnixClientEndpoint(int fd, const SocketSlot& state, NativeSocket socket) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard bind_lock(state.transport->unix_bind_mutex);
	{
		Common::LockGuard lock(g_socket_mutex);
		const auto& slot = g_sockets[static_cast<size_t>(fd)];
		if (slot.generation != state.generation || slot.transport != state.transport) { return SetGuestSocketError(Posix::POSIX_EBADF); }
		if (state.transport->unix_bound_host_path) { return 0; }
	}
	static std::atomic_uint64_t sequence {0};
#if defined(_WIN32)
	const auto host_pid = GetCurrentProcessId();
#else
	const auto host_pid = ::getpid();
#endif
	std::shared_ptr<const std::string> path;
	try {
		path = std::make_shared<const std::string>(Common::PathToString(std::filesystem::temp_directory_path() /
		    fmt::format("kyipc_{}_{}_{}", host_pid, state.transport->owner_pid, sequence.fetch_add(1))));
	} catch (...) { return SetGuestSocketError(Posix::POSIX_ENOMEM); }
	sockaddr_un local {};
	if (path->size() >= sizeof(local.sun_path)) { return SetGuestSocketError(Posix::POSIX_ENAMETOOLONG); }
	local.sun_family = AF_UNIX;
	std::memcpy(local.sun_path, path->c_str(), path->size() + 1);
	// Allocate cleanup storage before bind so allocation failure cannot leak a path.
	try { state.transport->unix_private_path = *path; }
	catch (const std::bad_alloc&) { return SetGuestSocketError(Posix::POSIX_ENOMEM); }
	if (::bind(socket, reinterpret_cast<const sockaddr*>(&local),
	           static_cast<SocketLength>(offsetof(sockaddr_un, sun_path) + path->size() + 1)) != 0) {
		state.transport->unix_private_path.clear(); // never unlink a path we did not bind
		return SetHostSocketError();
	}
	{
		Common::LockGuard lock(g_socket_mutex);
		const auto& slot = g_sockets[static_cast<size_t>(fd)];
		if (slot.generation != state.generation || slot.transport != state.transport) { return SetGuestSocketError(Posix::POSIX_EBADF); }
		state.transport->unix_bound_host_path = path;
	}
	return 0;
}

static int ConvertHostSockaddr(const sockaddr_storage* addr, SocketLength addrlen, void* out,
                               uint32_t* out_len, const std::string* unix_guest_path = nullptr) {
	EXIT_IF(addr == nullptr);
	EXIT_IF(out_len == nullptr);

	if (out == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}

	if (addr->ss_family == AF_UNIX) {
		// Host paths are not mapped back; a named endpoint reports the guest name it bound,
		// anything else (accepted peers, connected clients) is unnamed. BSD copies at most the
		// supplied capacity (zero is valid) and reports the complete address length.
		std::vector<uint8_t> guest;
		if (unix_guest_path != nullptr && !unix_guest_path->empty()) {
			guest.assign(GUEST_SOCKADDR_UN_PATH_OFFSET + unix_guest_path->size() + 1, 0);
			guest[0] = static_cast<uint8_t>(guest.size());
			guest[1] = 1;
			std::memcpy(guest.data() + GUEST_SOCKADDR_UN_PATH_OFFSET, unix_guest_path->data(),
			            unix_guest_path->size());
		} else {
			guest.assign(GUEST_SOCKADDR_NONAME_SIZE, 0);
			guest[0] = static_cast<uint8_t>(GUEST_SOCKADDR_NONAME_SIZE);
			guest[1] = 1;
		}
		std::memcpy(out, guest.data(), std::min<size_t>(*out_len, guest.size()));
		*out_len = static_cast<uint32_t>(guest.size());
		return 0;
	}

	if (addr->ss_family == AF_INET6) {
		if (addrlen < static_cast<SocketLength>(sizeof(sockaddr_in6))) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		std::array<uint8_t, 28> guest {};
		const auto* in6 = reinterpret_cast<const sockaddr_in6*>(addr);
		guest[0] = static_cast<uint8_t>(guest.size());
		guest[1] = 28;
		std::memcpy(guest.data() + 2, &in6->sin6_port, 2);
		std::memcpy(guest.data() + 4, &in6->sin6_flowinfo, 4);
		std::memcpy(guest.data() + 8, &in6->sin6_addr, 16);
		std::memcpy(guest.data() + 24, &in6->sin6_scope_id, 4);
		// BSD permits a short (including zero-capacity) output buffer, copies
		// its prefix, and reports the full address size without discarding I/O.
		std::memcpy(out, guest.data(), std::min<size_t>(*out_len, guest.size()));
		*out_len = static_cast<uint32_t>(guest.size());
		return 0;
	}

	if (addr->ss_family != AF_INET ||
	    addrlen < static_cast<SocketLength>(sizeof(sockaddr_in))) {
		*Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP;
		return -1;
	}

	static constexpr uint32_t guest_addrlen = 16;
	if (*out_len < guest_addrlen) {
		*out_len               = guest_addrlen;
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	auto*       bytes = static_cast<uint8_t*>(out);
	const auto* in    = reinterpret_cast<const sockaddr_in*>(addr);
	std::memset(bytes, 0, guest_addrlen);
	bytes[0] = static_cast<uint8_t>(guest_addrlen);
	bytes[1] = 2;
	std::memcpy(bytes + 2, &in->sin_port, sizeof(in->sin_port));
	std::memcpy(bytes + 4, &in->sin_addr, sizeof(in->sin_addr));
	*out_len = guest_addrlen;
	return 0;
}

static bool GetSocketBackend(int guest_fd, NativeSocket* out, SocketSlot* state = nullptr,
                             std::shared_ptr<SocketTransport>* transport = nullptr) {
	EXIT_IF(out == nullptr);

	if (guest_fd < 0 || guest_fd >= SOCKET_FD_MAX) {
		SetGuestSocketError(Posix::POSIX_EBADF);
		return false;
	}

	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_socket_mutex);
	const auto&       slot = g_sockets[static_cast<size_t>(guest_fd)];
	if (!slot.transport) {
		SetGuestSocketError(Posix::POSIX_EBADF);
		return false;
	}
	// Pair operations must retain ownership too; raw-handle-only operations
	// (connect/listen) are outside the already-connected pair subset.
	if ((slot.p2p || slot.transport->pair) && state == nullptr) {
		SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
		return false;
	}

	*out = slot.transport->socket;
	if (state != nullptr) {
		*state = slot;
	}
	if (transport != nullptr) {
		*transport = slot.transport;
	}
	return true;
}

bool __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IsSocket(int s) {
	if (s < 0 || s >= SOCKET_FD_MAX) {
		return false;
	}

	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_socket_mutex);
	return g_sockets[static_cast<size_t>(s)].transport != nullptr;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static thread_local void (*g_retained_stat_hook)(void*) = nullptr;
static thread_local void* g_retained_stat_context = nullptr;
void SocketRetainedStatHookForTest(void (*hook)(void*), void* context) {
	g_retained_stat_hook = hook;
	g_retained_stat_context = context;
}
#endif

bool TrySocketFstat(int fd, LibKernel::FileSystem::FileStat* output, int* result) {
	if (!GuestDescriptors::IsSocketRange(fd)) { return false; }
	*result = LibKernel::KERNEL_ERROR_EBADF;
	if (output == nullptr) { *result = LibKernel::KERNEL_ERROR_EFAULT; return true; }
	SocketSlot retained;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_socket_mutex);
		retained = g_sockets[static_cast<size_t>(fd)];
	}
	if (!retained.transport) { return true; }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (auto hook = std::exchange(g_retained_stat_hook, nullptr)) {
		hook(std::exchange(g_retained_stat_context, nullptr));
	}
#endif
	LibKernel::FileSystem::FileStat attributes {};
	std::memset(&attributes, 0, sizeof(attributes)); // Include ABI padding in the minimal snapshot.
	// A Net descriptor names an actual socket, not a disk inode. No timestamps,
	// permissions, byte counts, file links or host handles are invented here.
	attributes.st_mode = 0140000u; // S_IFSOCK, same guest type as the stat ABI.
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_socket_mutex);
		const auto& current = g_sockets[static_cast<size_t>(fd)];
		if (current.generation != retained.generation || current.transport != retained.transport ||
		    retained.transport->socket.load() == INVALID_NATIVE_SOCKET) { return true; }
		// No host query/callback under the table lock. Close/reuse cannot turn this
		// publication into a stat of a different object, or fall through to files.
		std::memcpy(output, &attributes, sizeof(attributes));
		*result = OK;
	}
	return true; // retained transport's final release occurs outside the lock.
}

// Non-blocking readiness probe for EVFILT_READ registrations on native sockets
// (pending data, a pending connection on a listener, or peer shutdown).
bool IsSocketReadReady(int s) {
	const int    saved_errno = *Posix::GetErrorAddr();
	NativeSocket socket      = INVALID_NATIVE_SOCKET;
	SocketSlot   state;
	if (!GetSocketBackend(s, &socket, &state) || state.p2p) {
		*Posix::GetErrorAddr() = saved_errno;
		return false;
	}
	// Windows PEEK consumes host bytes into the retained transport buffer.
	// EVFILT_READ must observe those bytes just like select(), even when the
	// underlying Winsock receive queue is now empty.
	if (state.transport->HasPendingReceive()) {
		*Posix::GetErrorAddr() = saved_errno;
		return true;
	}
#if !defined(_WIN32)
	if (socket >= FD_SETSIZE) {
		*Posix::GetErrorAddr() = saved_errno;
		return false;
	}
#endif
	fd_set readable {};
	FD_ZERO(&readable);
	FD_SET(socket, &readable);
	timeval timeout {};
#if defined(_WIN32)
	const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
	const int ready = ::select(socket + 1, &readable, nullptr, nullptr, &timeout);
#endif
	*Posix::GetErrorAddr() = saved_errno;
	return ready > 0;
}

int GetUnixPeerProcessId(int s, int* guest_pid) {
	if (guest_pid == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EFAULT);
	}
	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot   state;
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}
	if (state.p2p || state.transport->pair) {
		return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
	}
	sockaddr_storage address {};
	SocketLength     length = sizeof(address);
	if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
		return SetHostSocketError();
	}
	if (address.ss_family != AF_UNIX) {
		return SetGuestSocketError(Posix::POSIX_ENOTTY);
	}
	length = sizeof(address);
	if (::getpeername(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
		return SetHostSocketError();
	}
#if defined(_WIN32)
	ULONG peer     = 0;
	DWORD returned = 0;
	if (WSAIoctl(socket, SIO_AF_UNIX_GETPEERPID, nullptr, 0, &peer, sizeof(peer), &returned,
	             nullptr, nullptr) != 0) {
		return SetHostSocketError();
	}
	const auto local = GetCurrentProcessId();
#elif defined(__linux__)
	ucred        credentials {};
	SocketLength credentials_length = sizeof(credentials);
	if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0) {
		return SetHostSocketError();
	}
	const auto peer  = credentials.pid;
	const auto local = ::getpid();
#else
	return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
#endif
#if defined(_WIN32) || defined(__linux__)
	// A peer outside this emulator process has no guest identity.
	if (static_cast<uint64_t>(peer) != static_cast<uint64_t>(local)) {
		return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
	}
	const int pid = state.transport->peer_pid.load();
	if (!Loader::IsGuestProcessIdentityActive(pid)) { return SetGuestSocketError(Posix::POSIX_ESRCH); }
	*guest_pid = pid;
	return 0;
#endif
}

static bool TakeSocketBackend(int guest_fd, SocketSlot* out) {
	EXIT_IF(out == nullptr);

	if (guest_fd < 0 || guest_fd >= SOCKET_FD_MAX) {
		return false;
	}

	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_socket_mutex);
	auto&             slot = g_sockets[static_cast<size_t>(guest_fd)];
	if (!slot.transport) {
		return false;
	}

	*out = std::move(slot);
	slot = {};
	return true;
}

static int AllocSocketFd(const std::shared_ptr<SocketTransport>& transport, bool p2p = false) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_socket_mutex);
	for (int fd = SOCKET_FD_MIN; fd < SOCKET_FD_MAX; fd++) {
		auto& slot = g_sockets[static_cast<size_t>(fd)];
		if (!slot.transport) {
			slot.transport = transport;
			slot.generation = ++g_socket_generation;
			if (p2p) {
				slot.p2p.emplace();
			}
			return fd;
		}
	}

	return -1;
}

#include "libs/networkSocketpair.inc"
static int SelectSockets(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds,
                         timeval* timeout,
                         const std::vector<std::shared_ptr<SocketTransport>>& stream_reads) {
#if defined(_WIN32)
	if (!stream_reads.empty()) {
		const auto requested_read   = *readfds;
		const auto requested_write  = writefds != nullptr ? *writefds : fd_set {};
		const auto requested_except = exceptfds != nullptr ? *exceptfds : fd_set {};
		std::optional<std::chrono::steady_clock::time_point> deadline;
		if (timeout != nullptr) {
			deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout->tv_sec) +
			           std::chrono::microseconds(timeout->tv_usec);
		}
		for (;;) {
			*readfds = requested_read;
			if (writefds != nullptr) {
				*writefds = requested_write;
			}
			if (exceptfds != nullptr) {
				*exceptfds = requested_except;
			}
			const bool ready = std::any_of(stream_reads.begin(), stream_reads.end(),
			    [](const auto& transport) { return transport->HasPendingReceive(); });
			// A concurrent peek can move native bytes into the guest buffer after this check.
			// Bound native waits so both readiness sources are rechecked without holding receive locks.
			auto wait = std::chrono::microseconds(ready ? 0 : 10'000);
			if (deadline) {
				const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
				    *deadline - std::chrono::steady_clock::now());
				wait = std::min(wait, std::max(remaining, std::chrono::microseconds::zero()));
			}
			timeval slice {};
			slice.tv_usec = static_cast<long>(wait.count());
			int result = ::select(nfds, readfds, writefds, exceptfds, &slice);
			if (result < 0) {
				return result;
			}
			for (const auto& transport: stream_reads) {
				const auto socket = transport->socket.load();
				if (socket == INVALID_NATIVE_SOCKET) {
					WSASetLastError(WSAENOTSOCK);
					return SOCKET_ERROR;
				}
				if (transport->HasPendingReceive() && !FD_ISSET(socket, readfds)) {
					FD_SET(socket, readfds);
					result++;
				}
			}
			if (result != 0 || (deadline && std::chrono::steady_clock::now() >= *deadline)) {
				return result;
			}
		}
	}
#endif
	return ::select(nfds, readfds, writefds, exceptfds, timeout);
}

static bool GuestFdIsSet(const void* fds, int fd) {
	if (fds == nullptr || fd < 0 || fd >= SOCKET_FD_MAX) {
		return false;
	}

	const auto* words = static_cast<const uint64_t*>(fds);
	return (words[fd / 64] & (uint64_t {1} << (fd % 64))) != 0;
}

static void GuestFdZero(void* fds, int nfds) {
	if (fds == nullptr || nfds <= 0) {
		return;
	}

	std::memset(fds, 0, static_cast<size_t>((nfds + 63) / 64) * sizeof(uint64_t));
}

static void GuestFdSet(void* fds, int fd) {
	if (fds == nullptr || fd < 0 || fd >= SOCKET_FD_MAX) {
		return;
	}

	auto* words = static_cast<uint64_t*>(fds);
	words[fd / 64] |= (uint64_t {1} << (fd % 64));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetInit() {
	PRINT_NAME();

	EnsureSocketBackend();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetTerm() {
	PRINT_NAME();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetSetInitParam(const void* param) {
	if (g_net == nullptr) { return NET_ERROR_ENOTINIT; }
	return g_net->SetInitParam(param);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetPoolCreate(const char* name, int size, int flags) {
	PRINT_NAME();

	if (g_net == nullptr) { return NET_ERROR_ENOTINIT; }
	if (name == nullptr || size < 0x1000 || flags != 0) { return NET_ERROR_EINVAL; }

	LOGF("\t name = %s\n"
	     "\t size = %d\n"
	     "\t flags = %d\n",
	     name, size, flags);

	EXIT_IF(g_net == nullptr);

	int id = g_net->PoolCreate(name, size);

	if (id < 0) {
		return NET_ERROR_ENFILE;
	}

	return id;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetPoolDestroy(int memid) {
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->PoolDestroy(memid)) {
		return NET_ERROR_EBADF;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetResolverCreate(const char* name, int memid, int flags) {
	PRINT_NAME();

	LOGF("\t name  = %s\n"
	     "\t memid = %d\n"
	     "\t flags = %d\n",
	     name != nullptr ? name : "<null>", memid, flags);

	if (name == nullptr || flags != 0) {
		return NET_ERROR_EINVAL;
	}

	EXIT_IF(g_net == nullptr);

	const int id = g_net->ResolverCreate(name, memid);
	if (id < 0) {
		return NET_ERROR_RESOLVER_ENOSPACE;
	}

	return id;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetResolverDestroy(int rid) {
	PRINT_NAME();

	LOGF("\t rid = %d\n", rid);

	EXIT_IF(g_net == nullptr);
	if (!g_net->ResolverDestroy(rid)) {
		return NET_ERROR_EBADF;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetResolverAbort(int rid, int flags) {
	PRINT_NAME();
	LOGF("\t rid = %d\n\t flags = 0x%08x\n", rid, flags);
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetResolverStartNtoaMultipleRecords(int rid, const char* hostname,
                                                      NetResolverInfo* info, int timeout, int retry,
                                                      int flags) {
	PRINT_NAME();

	LOGF("\t rid      = %d\n"
	     "\t hostname = %s\n"
	     "\t timeout  = %d\n"
	     "\t retry    = %d\n"
	     "\t flags    = 0x%08x\n",
	     rid, hostname != nullptr ? hostname : "<null>", timeout, retry, flags);

	if (hostname == nullptr || info == nullptr || (flags & ~0x00010000) != 0) {
		return NET_ERROR_EINVAL;
	}

	EXIT_IF(g_net == nullptr);
	if (!g_net->ResolverValid(rid)) {
		return NET_ERROR_EBADF;
	}

	*info = {};
	if ((flags & 0x00010000) == 0 && NetInetPton(2, hostname, &info->addrs[0].addr) == 1) {
		info->addrs[0].af = 2;
		info->records     = 1;
		info->dns4records = 1;
		return OK;
	}

	if (!EnsureSocketBackend()) {
		return NET_ERROR_ENETDOWN;
	}

	addrinfo hints {};
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo* result = nullptr;
	const int ret    = getaddrinfo(hostname, nullptr, &hints, &result);
	if (ret != 0 || result == nullptr) {
		if (result != nullptr) {
			freeaddrinfo(result);
		}
		if (ret == EAI_NONAME) {
			return NET_ERROR_RESOLVER_ENOHOST;
		}
		if (ret == EAI_NODATA) {
			return NET_ERROR_RESOLVER_ENORECORD;
		}
		return NET_ERROR_RESOLVER_EINTERNAL;
	}

	for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
		if (ai->ai_family == AF_INET && ai->ai_addr != nullptr &&
		    ai->ai_addrlen >= sizeof(sockaddr_in)) {
			if (info->records < static_cast<int>(std::size(info->addrs))) {
				NetResolverAddr& out = info->addrs[info->records++];
				const auto*      in  = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
				std::memcpy(&out.addr, &in->sin_addr, sizeof(out.addr));
				out.af = 2;
			}
			info->dns4records++;
		}
	}

	freeaddrinfo(result);
	return info->records > 0 ? OK : NET_ERROR_RESOLVER_ENORECORD;
}

static void AppendResolverAddress(NetResolverInfoEx& info, const NetResolverAddrEx& address) {
	// 13.60 e655/e67b/e681, e6b4/e73d/e743 and DNS parser f5b6/f611:
	// append in arrival order, clip storage (not DNS counters) at ten records.
	if (address.af != 2 && address.af != 28) { return; }
	if (info.records < static_cast<int>(std::size(info.addrs))) {
		auto& out = info.addrs[info.records++];
		std::memcpy(out.addr, address.addr, address.af == 2 ? 4 : 16);
		out.af = address.af;
	}
	auto& count = address.af == 2 ? info.dns4records : info.dns6records;
	if (count < std::numeric_limits<int>::max()) { ++count; }
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
static thread_local void (*g_resolver_retained_hook)(void*) = nullptr;
static thread_local void* g_resolver_retained_context = nullptr;

void NetResolverRetainedHookForTest(void (*hook)(void*), void* context) {
	g_resolver_retained_hook = hook;
	g_resolver_retained_context = context;
}

void NetResolverAppendForTest(NetResolverInfoEx* info, const NetResolverAddrEx* records, size_t count) {
	for (size_t i = 0; i < count; ++i) { AppendResolverAddress(*info, records[i]); }
}
#endif

static bool NetworkTraceEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_NETWORK_TRACE");
		return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

static void TraceResolverEx(int rid, int rid6, int flags, const char* hostname,
                             const NetResolverInfoEx& info, int status) {
	if (!NetworkTraceEnabled()) { return; }
	static std::atomic<unsigned> calls {0};
	if (calls.fetch_add(1, std::memory_order_relaxed) >= 128) { return; }
	// Only validated hostname text is provided here, never a URL, headers or data.
	std::array<char, 11> families {};
	for (int i = 0; i < info.records && i < 10; ++i) { families[i] = info.addrs[i].af == 2 ? '4' : '6'; }
	LOGF("network-dns-ex: host=%.127s flags=0x%08x rid=%d rid6=%d status=0x%08x records=%d dns4=%d dns6=%d families=%s\n",
	     hostname, static_cast<unsigned>(flags), rid, rid6, static_cast<unsigned>(status),
	     info.records, info.dns4records, info.dns6records, families.data());
}

static int ExtendedResolverHostError(int result) {
	if (result == EAI_NONAME) { return NET_ERROR_RESOLVER_ENOHOST; }
#if defined(EAI_NODATA)
	if (result == EAI_NODATA) { return NET_ERROR_RESOLVER_ENORECORD; }
#endif
#if defined(EAI_ADDRFAMILY)
	if (result == EAI_ADDRFAMILY) { return NET_ERROR_RESOLVER_ENORECORD; }
#endif
	if (result == EAI_AGAIN || result == EAI_FAIL) { return NET_ERROR_RESOLVER_ESERVERFAILURE; }
	if (result == EAI_MEMORY) { return NET_ERROR_RESOLVER_ENOSPACE; }
	if (result == EAI_FAMILY) { return NET_ERROR_RESOLVER_ENOSUPPORT; }
	return NET_ERROR_RESOLVER_EINTERNAL;
}

static int ResolveHostFamily(const char* hostname, int family, NetResolverInfoEx& info) {
	addrinfo hints {};
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM; // Avoid protocol-only duplicates, not address duplicates.
	// No AI_ADDRCONFIG or AI_V4MAPPED: query real AAAA even without an IPv6
	// interface, and never synthesize IPv6 from an A answer.
	addrinfo* addresses = nullptr;
	const int status = ::getaddrinfo(hostname, nullptr, &hints, &addresses);
	if (status != 0) {
		if (addresses != nullptr) { ::freeaddrinfo(addresses); }
		return ExtendedResolverHostError(status);
	}
	bool found = false;
	for (auto* ai = addresses; ai != nullptr; ai = ai->ai_next) {
		if (ai->ai_family != family || ai->ai_addr == nullptr) { continue; }
		NetResolverAddrEx out {};
		if (family == AF_INET && ai->ai_addrlen >= sizeof(sockaddr_in)) {
			std::memcpy(out.addr, &reinterpret_cast<const sockaddr_in*>(ai->ai_addr)->sin_addr, 4);
			out.af = 2;
		} else if (family == AF_INET6 && ai->ai_addrlen >= sizeof(sockaddr_in6)) {
			std::memcpy(out.addr, &reinterpret_cast<const sockaddr_in6*>(ai->ai_addr)->sin6_addr, 16);
			out.af = 28;
		} else { continue; }
		AppendResolverAddress(info, out);
		found = true;
	}
	if (addresses != nullptr) { ::freeaddrinfo(addresses); }
	return found ? OK : NET_ERROR_RESOLVER_ENORECORD;
}

static bool HostParsesNumericAddress(const char* hostname) {
	addrinfo hints {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST; // Probe only: never DNS or a service connection.
	addrinfo* addresses = nullptr;
	const int status = ::getaddrinfo(hostname, nullptr, &hints, &addresses);
	if (addresses != nullptr) { ::freeaddrinfo(addresses); }
	return status == 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetResolverStartNtoaMultipleRecordsEx(
    int rid, const char* hostname, NetResolverInfoEx* info, int timeout, int retry, int flags) {
	NetResolverInfoEx result {};
	result.rid6 = -1;
	std::array<char, 128> trace_hostname {};
	std::memcpy(trace_hostname.data(), "<unchecked>", sizeof("<unchecked>"));
	const auto finish = [&](int status) {
		TraceResolverEx(rid, result.rid6, flags, trace_hostname.data(), result, status);
		return status;
	};
	if (g_net == nullptr) { return finish(NET_ERROR_ENOTINIT); }
	constexpr int inet6 = 0x01000000, inet46 = 0x02000000, disable_ip = 0x00010000;
	constexpr int dns_transport = 0x00300000;
	// 13.60 3b6e/c321 and 4.03 3b1b/bf00: unknown bits, conflicting
	// families, DNS-transport selectors without INET6, and negative budgets fail.
	if ((flags & ~0x03310001) != 0 || (flags & (inet6 | inet46)) == (inet6 | inet46) ||
	    ((flags & inet6) == 0 && (flags & dns_transport) != 0) || timeout < 0 || retry < 0 ||
	    (timeout != 0 && timeout < 1'000000)) { return finish(NET_ERROR_EINVAL); }
	// Host getaddrinfo provides synchronous/default host policy, not the guest
	// asynchronous engine, selectable DNS transport, or cancellable custom budget.
	if ((flags & (1 | dns_transport)) != 0 || timeout != 0 || retry > 1) {
		return finish(NET_ERROR_EOPNOTSUPP);
	}
	// Native c3b6/c3de rejects a dead primary ID before inspecting caller buffers.
	// Retain both generations together below once the rid6 input can be read.
	if (!g_net->ResolverValid(rid)) { return finish(NET_ERROR_EBADF); }
	if (hostname == nullptr || info == nullptr) { return finish(NET_ERROR_EINVAL); }
	result.rid6 = info->rid6; // Preserve even for single-family calls, as firmware does.
	const bool dual = (flags & inet46) != 0;
	// -1 denotes "unused" only internally, never a valid dual-stack resolver ID.
	if (dual && result.rid6 < 0) { return finish(NET_ERROR_EBADF); }
	std::array<uint64_t, 2> generations {};
	const int active_rid6 = dual ? result.rid6 : -1;
	if (!g_net->ResolverRetain(rid, active_rid6, &generations)) { return finish(NET_ERROR_EBADF); }
	if (dual && rid == result.rid6) { return finish(NET_ERROR_RESOLVER_EBUSY); }
	size_t length = 0;
	while (length < 256 && hostname[length] != '\0') { ++length; }
	if (length == 0 || length == 256) { return finish(NET_ERROR_EINVAL); }
	// Snapshot caller text before a blocking host call. Trace only hostname-like
	// ASCII; an accidentally supplied credential-bearing URL is never logged.
	const std::string name(hostname, length);
	const bool safe_trace = std::all_of(name.begin(), name.end(), [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		       c == '-' || c == '_' || c == '.' || c == ':' || c == '%';
	});
	trace_hostname.fill(0);
	if (safe_trace) { std::memcpy(trace_hostname.data(), name.data(), std::min(name.size(), trace_hostname.size() - 1)); }
	else { std::memcpy(trace_hostname.data(), "<non-hostname>", sizeof("<non-hostname>")); }
	// Firmware clears all 384 bytes only after validation, retaining input rid6.
	if (!g_net->ResolverPublish(rid, active_rid6, generations, info, result)) { return finish(NET_ERROR_EBADF); }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (auto hook = std::exchange(g_resolver_retained_hook, nullptr)) {
		hook(std::exchange(g_resolver_retained_context, nullptr));
	}
#endif
	int status = NET_ERROR_ENETDOWN;
	if (EnsureSocketBackend()) {
		NetResolverAddrEx literal {};
#if defined(_WIN32)
		if (::InetPtonA(AF_INET, name.c_str(), literal.addr) == 1) { literal.af = 2; }
		else if (::InetPtonA(AF_INET6, name.c_str(), literal.addr) == 1) { literal.af = 28; }
#else
		if (::inet_pton(AF_INET, name.c_str(), literal.addr) == 1) { literal.af = 2; }
		else if (::inet_pton(AF_INET6, name.c_str(), literal.addr) == 1) { literal.af = 28; }
#endif
		if ((flags & disable_ip) != 0 && (literal.af != 0 || HostParsesNumericAddress(name.c_str()))) {
			// getaddrinfo cannot force DNS for numeric text, including alternate
			// forms (e.g. 2130706433) and scoped IPv6 it accepts beyond inet_pton.
			status = NET_ERROR_EOPNOTSUPP;
		} else if (literal.af != 0) {
			if (dual || literal.af == ((flags & inet6) != 0 ? 28 : 2)) {
				AppendResolverAddress(result, literal);
				status = OK;
			} else { status = NET_ERROR_RESOLVER_ENORECORD; }
		} else if (dual) {
			// Firmware c66a/c6b1 starts AAAA before A and appends each completion
			// without sorting/deduplication. The synchronous host adapter serializes
			// those phases; within each phase preserve host answer order. It does
			// not emulate the native engine's 50ms partial-answer grace interval.
			const int status6 = ResolveHostFamily(name.c_str(), AF_INET6, result);
			const int status4 = ResolveHostFamily(name.c_str(), AF_INET, result);
			status = result.records != 0 ? OK : (status4 != NET_ERROR_RESOLVER_ENORECORD ? status4 : status6);
		} else {
			status = ResolveHostFamily(name.c_str(), (flags & inet6) != 0 ? AF_INET6 : AF_INET, result);
		}
	}
	if (!g_net->ResolverPublish(rid, active_rid6, generations, info, result)) {
		// A destroyed/reused public ID cannot publish answers for its replacement.
		result = {};
		result.rid6 = info->rid6;
		return finish(NET_ERROR_EBADF);
	}
	return finish(status);
}

int KYTY_SYSV_ABI NetResolverStartNtoa6(int rid, const char* hostname, void* addr, int timeout,
                                       int retry, int flags) {
	if (g_net == nullptr) { return NET_ERROR_ENOTINIT; }
	// 13.60 3920/cbd0 (local4.03 38d0/c760): native validation precedes
	// resolver lookup, then null/hostname checks; zero budgets select defaults.
	constexpr int disable_ip = 0x10000, dns_transport = 0x300000;
	if ((flags & ~0x310001) != 0 || timeout < 0 || retry < 0 ||
	    (timeout != 0 && timeout < 1'000000)) { return NET_ERROR_EINVAL; }
	std::array<uint64_t, 2> generations {};
	if (!g_net->ResolverRetain(rid, -1, &generations)) { return NET_ERROR_EBADF; }
	if (hostname == nullptr || addr == nullptr) { return NET_ERROR_EINVAL; }
	size_t length = 0;
	while (length < 256 && hostname[length] != '\0') { ++length; }
	if (length == 0 || length == 256) { return NET_ERROR_EINVAL; }
	// Same synchronous host-policy subset as MultipleRecordsEx, not a pretend
	// native async engine, selectable DNS transport, retry loop or custom budget.
	if ((flags & (1 | dns_transport)) != 0 || timeout != 0 || retry > 1) {
		return NET_ERROR_EOPNOTSUPP;
	}
	try {
		const std::string name(hostname, length);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (auto hook = std::exchange(g_resolver_retained_hook, nullptr)) {
			hook(std::exchange(g_resolver_retained_context, nullptr));
		}
#endif
		NetResolverInfoEx result {};
		int status = NET_ERROR_ENETDOWN;
		if (EnsureSocketBackend()) {
			NetResolverAddrEx literal {};
#if defined(_WIN32)
			if (::InetPtonA(AF_INET6, name.c_str(), literal.addr) == 1) { literal.af = 28; }
			else if (::InetPtonA(AF_INET, name.c_str(), literal.addr) == 1) { literal.af = 2; }
#else
			if (::inet_pton(AF_INET6, name.c_str(), literal.addr) == 1) { literal.af = 28; }
			else if (::inet_pton(AF_INET, name.c_str(), literal.addr) == 1) { literal.af = 2; }
#endif
			if ((flags & disable_ip) != 0 && (literal.af != 0 || HostParsesNumericAddress(name.c_str()))) {
				status = NET_ERROR_EOPNOTSUPP; // Host lookup cannot force DNS for numeric text.
			} else if (literal.af == 28) {
				AppendResolverAddress(result, literal);
				status = OK;
			} else if (literal.af == 2) {
				status = NET_ERROR_RESOLVER_ENORECORD; // Never invent mapped AAAA from an A address.
			} else {
				status = ResolveHostFamily(name.c_str(), AF_INET6, result);
			}
		}
		if (status == OK && !g_net->ResolverPublishAddress6(rid, generations[0], addr, result.addrs[0].addr)) {
			status = NET_ERROR_EBADF;
		}
		return status;
	} catch (const std::bad_alloc&) { return NET_ERROR_RESOLVER_ENOSPACE; }
}

int KYTY_SYSV_ABI NetResolverStartNtoa(int rid, const char* hostname, void* addr, int timeout,
                                       int retry, int flags) {
	PRINT_NAME();

	if (addr == nullptr) {
		return NET_ERROR_EINVAL;
	}

	NetResolverInfo info;
	const int       ret =
	    NetResolverStartNtoaMultipleRecords(rid, hostname, &info, timeout, retry, flags);
	if (ret == OK) {
		std::memcpy(addr, &info.addrs[0].addr, sizeof(info.addrs[0].addr));
	}
	return ret;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetInetPton(int af, const char* src, void* dst) {
	PRINT_NAME();

	if (src == nullptr || dst == nullptr) {
		return NET_ERROR_EINVAL;
	}

	const int host_family = ConvertFamily(af);
	if (host_family < 0) {
		return NET_ERROR_EAFNOSUPPORT;
	}

	LOGF("\t src = %.46s\n", src);

#if defined(_WIN32)
	const int result = ::InetPtonA(host_family, src, dst);
#else
	const int result = ::inet_pton(host_family, src, dst);
#endif
	return result < 0 ? NET_ERROR_EINVAL : result;
}

const char* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetInetNtop(int af, const void* src, char* dst, uint32_t size) {
	PRINT_NAME();

	if (src == nullptr || dst == nullptr || size == 0) {
		return nullptr;
	}

	if (af != 2 && af != 28) {
		return nullptr;
	}

#if defined(_WIN32)
	const int win_af = (af == 28 ? AF_INET6 : AF_INET);
	if (::InetNtopA(win_af, const_cast<void*>(src), dst, size) == nullptr) {
		return nullptr;
	}
#else
	if (::inet_ntop(af, src, dst, size) == nullptr) {
		return nullptr;
	}
#endif

	return dst;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetEtherNtostr(const NetEtherAddr* n, char* str, size_t len) {
	PRINT_NAME();

	if (n == nullptr || str == nullptr || len < 18) {
		return NET_ERROR_EINVAL;
	}

	std::snprintf(str, len, "%02x:%02x:%02x:%02x:%02x:%02x", n->data[0], n->data[1],
	              n->data[2], n->data[3], n->data[4], n->data[5]);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetGetMacAddress(NetEtherAddr* addr, int flags) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	EXIT_NOT_IMPLEMENTED(flags != 0);

	memset(addr->data, 0, sizeof(addr->data));

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetGetSockInfo(int s, void* info, int n, int flags) {
	PRINT_NAME();

	LOGF("\t s     = %d\n"
	     "\t info  = 0x%016" PRIx64 "\n"
	     "\t n     = %d\n"
	     "\t flags = %d\n",
	     s, reinterpret_cast<uint64_t>(info), n, flags);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EpollCreate(const char* name, int flags) {
	if (name == nullptr || flags != 0) {
		return SetGuestSocketError(Posix::POSIX_EINVAL);
	}

	std::lock_guard lock(g_epoll_mutex);
	for (size_t index = 0; index < g_epolls.size(); index++) {
		auto& slot = g_epolls[index];
		if (!slot.used) {
			slot.used = true;
			slot.name = name;
			slot.registrations.clear();
			return EPOLL_ID_BASE + static_cast<int>(index);
		}
	}

	return SetGuestSocketError(Posix::POSIX_EMFILE);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EpollControl(int eid, int op, int id, const NetEpollEvent* event) {
	constexpr int EPOLL_CTL_ADD = 1;
	constexpr int EPOLL_CTL_MOD = 2;
	constexpr int EPOLL_CTL_DEL = 3;

	if ((op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) && event == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EINVAL);
	}
	if (op == EPOLL_CTL_DEL && event != nullptr) {
		return SetGuestSocketError(Posix::POSIX_EINVAL);
	}
	if (op < EPOLL_CTL_ADD || op > EPOLL_CTL_DEL) {
		return SetGuestSocketError(Posix::POSIX_EINVAL);
	}
	if (!IsSocket(id)) {
		return SetGuestSocketError(Posix::POSIX_EBADF);
	}

	std::lock_guard lock(g_epoll_mutex);
	auto*           slot = GetEpollSlot(eid);
	if (slot == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EBADF);
	}

	auto registration = std::find_if(slot->registrations.begin(), slot->registrations.end(),
	                                 [id](const auto& value) { return value.id == id; });
	switch (op) {
		case EPOLL_CTL_ADD:
			if (registration != slot->registrations.end()) {
				return SetGuestSocketError(Posix::POSIX_EEXIST);
			}
			slot->registrations.push_back({id, *event});
			break;
		case EPOLL_CTL_MOD:
			if (registration == slot->registrations.end()) {
				return SetGuestSocketError(Posix::POSIX_ENOENT);
			}
			registration->event = *event;
			break;
		case EPOLL_CTL_DEL:
			if (registration == slot->registrations.end()) {
				return SetGuestSocketError(Posix::POSIX_ENOENT);
			}
			slot->registrations.erase(registration);
			break;
		default: break;
	}

	g_epoll_changed.notify_all();
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EpollWait(int eid, NetEpollEvent* events, int maxevents, int timeout) {
	if (events == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EFAULT);
	}
	if (maxevents <= 0 || timeout < -1) {
		return SetGuestSocketError(Posix::POSIX_EINVAL);
	}

	std::vector<EpollRegistration> registrations;
	{
		std::unique_lock lock(g_epoll_mutex);
		auto*            slot = GetEpollSlot(eid);
		if (slot == nullptr) {
			return SetGuestSocketError(Posix::POSIX_EBADF);
		}

		if (slot->registrations.empty() && timeout != 0) {
			auto ready = [eid] {
				auto* current = GetEpollSlot(eid);
				return current == nullptr || !current->registrations.empty();
			};
			if (timeout < 0) {
				g_epoll_changed.wait(lock, ready);
			} else {
				g_epoll_changed.wait_for(lock, std::chrono::microseconds(timeout), ready);
			}
			slot = GetEpollSlot(eid);
			if (slot == nullptr) {
				return SetGuestSocketError(Posix::POSIX_EBADF);
			}
		}
		registrations = slot->registrations;
	}

	if (registrations.empty()) {
		return 0;
	}

	struct HostRegistration {
		EpollRegistration guest;
		NativeSocket      socket = INVALID_NATIVE_SOCKET;
		SocketSlot        state;
	};

	fd_set host_read {};
	fd_set host_write {};
	fd_set host_except {};
	FD_ZERO(&host_read);
	FD_ZERO(&host_write);
	FD_ZERO(&host_except);

	std::vector<HostRegistration> host_registrations;
	std::vector<std::shared_ptr<SocketTransport>> stream_reads;
	host_registrations.reserve(registrations.size());
	int host_nfds = 0;
	for (const auto& registration: registrations) {
		NativeSocket socket = INVALID_NATIVE_SOCKET;
		SocketSlot   state;
		if (!GetSocketBackend(registration.id, &socket, &state)) {
			continue;
		}
#if defined(_WIN32)
		if (host_registrations.size() >= FD_SETSIZE) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
#else
		if (socket >= FD_SETSIZE) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		host_nfds = std::max(host_nfds, socket + 1);
#endif
		if ((registration.event.events & EPOLL_IN) != 0) {
			FD_SET(socket, &host_read);
#if defined(_WIN32)
			if (state.transport->type == SOCK_STREAM) {
				stream_reads.push_back(state.transport);
			}
#endif
		}
		if ((registration.event.events & EPOLL_OUT) != 0) {
			FD_SET(socket, &host_write);
		}
		FD_SET(socket, &host_except);
		host_registrations.push_back({registration, socket, std::move(state)});
	}

	if (host_registrations.empty()) {
		return 0;
	}

	timeval  host_timeout {};
	timeval* host_timeout_ptr = nullptr;
	if (timeout >= 0) {
		host_timeout.tv_sec  = timeout / 1000000;
		host_timeout.tv_usec = timeout % 1000000;
		host_timeout_ptr     = &host_timeout;
	}
	const int result = SelectSockets(host_nfds, &host_read, &host_write, &host_except,
	                                 host_timeout_ptr, stream_reads);
	if (result < 0) {
		return SetHostSocketError();
	}
	if (result == 0) {
		return 0;
	}

	int count = 0;
	for (const auto& registration: host_registrations) {
		uint32_t ready = 0;
		if ((registration.guest.event.events & EPOLL_IN) != 0 &&
		    FD_ISSET(registration.socket, &host_read)) {
			if (registration.state.p2p) {
				return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
			}
			ready |= EPOLL_IN;
		}
		if ((registration.guest.event.events & EPOLL_OUT) != 0 &&
		    FD_ISSET(registration.socket, &host_write)) {
			ready |= EPOLL_OUT;
		}
		if (FD_ISSET(registration.socket, &host_except)) {
			ready |= EPOLL_ERR;
		}
		if (ready == 0) {
			continue;
		}

		auto& output    = events[count++];
		output          = registration.guest.event;
		output.events   = ready;
		output.reserved = 0;
		output.ident    = static_cast<uint64_t>(registration.guest.id);
		if (count == maxevents) {
			break;
		}
	}
	return count;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI EpollDestroy(int eid) {
	std::lock_guard lock(g_epoll_mutex);
	auto*           slot = GetEpollSlot(eid);
	if (slot == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EBADF);
	}

	slot->used = false;
	slot->name.clear();
	slot->registrations.clear();
	g_epoll_changed.notify_all();
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SocketClose(int s) {
	PRINT_NAME();

	LOGF("\t s = %d\n", s);

	SocketSlot slot;
	if (!TakeSocketBackend(s, &slot)) {
		return NET_ERROR_EBADF;
	}
	RemoveSocketFromEpolls(s);

	bool close_transport = slot.transport.use_count() == 1;
#if defined(_WIN32)
	// Retained receive/readiness state must not defer native TCP close cancellation.
	close_transport |= !slot.p2p && !slot.transport->pair && slot.transport->type == SOCK_STREAM;
#endif
	if (close_transport && slot.transport->Close() != 0) {
		return NET_ERROR_EBADF;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Socket(int family, int type, int protocol) {
	PRINT_NAME();

	LOGF("\t family   = %d\n"
	     "\t type     = %d\n"
	     "\t protocol = %d\n",
	     family, type, protocol);

	if (!EnsureSocketBackend()) {
		return SetGuestSocketError(Posix::POSIX_ENETDOWN);
	}

	const int host_family = ConvertFamily(family);
	if (host_family < 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EAFNOSUPPORT;
		return -1;
	}

	constexpr int socket_dgram_p2p = 6;
	const bool    p2p              = type == socket_dgram_p2p;
	if (p2p && (family != 2 || protocol != 0)) {
		return SetGuestSocketError(Posix::POSIX_EPROTONOSUPPORT);
	}
	std::shared_ptr<SocketTransport> transport;
	try { transport = std::make_shared<SocketTransport>(INVALID_NATIVE_SOCKET); }
	catch (const std::bad_alloc&) { return SetGuestSocketError(Posix::POSIX_ENOMEM); }
	NativeSocket socket = ::socket(host_family, p2p ? SOCK_DGRAM : type, protocol);
	if (socket == INVALID_NATIVE_SOCKET) {
		return SetHostSocketError();
	}

	transport->socket = socket;
	transport->type = p2p ? SOCK_DGRAM : type;
	transport->owner_pid = Loader::GetCurrentGuestProcessId();
	if (p2p) {
		// Logical sockets keep their own blocking mode; the shared transport never blocks.
#if defined(_WIN32)
		u_long    enabled = 1;
		const int result  = ioctlsocket(socket, FIONBIO, &enabled);
#else
		const int result = ::fcntl(socket, F_SETFL, O_NONBLOCK);
#endif
		if (result != 0) {
			return SetHostSocketError();
		}
	}
	const int fd = AllocSocketFd(transport, p2p);
	if (fd < 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EMFILE;
		return -1;
	}

	LOGF("\t fd = %d\n", fd);
	return fd;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Bind(int s, const void* addr, uint32_t addrlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t addr    = 0x%016" PRIx64 "\n"
	     "\t addrlen = %" PRIu32 "\n",
	     s, reinterpret_cast<uint64_t>(addr), addrlen);

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot   state;
	if (addr == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EFAULT);
	}
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}

	sockaddr_storage host_addr {};
	SocketLength     host_addrlen = 0;
	std::string      unix_guest_path;
	if (ConvertGuestSockaddr(addr, addrlen, &host_addr, &host_addrlen, &unix_guest_path) != 0) {
		return -1;
	}

	if (state.p2p) {
		if (host_addr.ss_family != AF_INET) {
			return SetGuestSocketError(Posix::POSIX_EAFNOSUPPORT);
		}
		if (addrlen < 16) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		const auto& in      = *reinterpret_cast<const sockaddr_in*>(&host_addr);
		uint32_t    address = 0;
		std::memcpy(&address, &in.sin_addr, sizeof(address));
		uint16_t virtual_port = 0;
		std::memcpy(&virtual_port, static_cast<const uint8_t*>(addr) + 8, sizeof(virtual_port));
		const auto requested_port = ntohs(virtual_port);
		if (requested_port == 0) {
			return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
		}
		if (ntohs(in.sin_port) != 3658) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		if (requested_port == 5353 || requested_port >= 32768) {
			return SetGuestSocketError(Posix::POSIX_EACCES);
		}

		std::shared_ptr<P2pBindAttempt> attempt;
		try { attempt = std::make_shared<P2pBindAttempt>(); }
		catch (const std::bad_alloc&) { return SetGuestSocketError(Posix::POSIX_ENOMEM); }
		attempt->address = address;
		attempt->port = in.sin_port;
		int reservation = -1;
		for (;;) {
			std::shared_ptr<P2pBindAttempt> pending;
			std::shared_ptr<SocketTransport> shared;
			{
				Common::HostLockCriticalGuard critical;
				Common::LockGuard lock(g_socket_mutex);
				auto& slot = g_sockets[static_cast<size_t>(s)];
				if (slot.generation != state.generation || slot.transport != state.transport) {
					return SetGuestSocketError(Posix::POSIX_EBADF);
				}
				if (slot.p2p->bound) { return SetGuestSocketError(Posix::POSIX_EINVAL); }
				for (const auto& other: g_sockets) {
					if (other.p2p && other.p2p->bound && other.p2p->port == in.sin_port &&
					    other.p2p->address == address) {
						if (other.p2p->virtual_port == virtual_port) {
							return SetGuestSocketError(Posix::POSIX_EADDRINUSE);
						}
						shared = other.transport;
					}
				}
				if (shared) {
					// state retains the displaced unbound socket until after unlock.
					slot.transport = shared;
					slot.p2p->bound = true;
					slot.p2p->address = address;
					slot.p2p->port = in.sin_port;
					slot.p2p->virtual_port = virtual_port;
				} else {
					for (const auto& other: g_p2p_bind_attempts) {
						if (other && other->address == address && other->port == in.sin_port) {
							pending = other; break;
						}
					}
					if (!pending) {
						for (int index = 0; index < SOCKET_FD_MAX; ++index) {
							if (!g_p2p_bind_attempts[index]) {
								reservation = index;
								g_p2p_bind_attempts[index] = attempt;
								break;
							}
						}
						if (reservation < 0) { return SetGuestSocketError(Posix::POSIX_ENOBUFS); }
					}
				}
			}
			if (shared) { break; }
			if (pending) {
				pending->complete.wait(false); // no table ownership or critical guard
				continue; // revalidate slot generation and conflicts after wakeup
			}
			// One host bind per physical address/port, outside g_socket_mutex.
			const int bound = ::bind(socket, reinterpret_cast<const sockaddr*>(&host_addr), host_addrlen);
			int error = bound == 0 ? 0 : (SetHostSocketError(), *Posix::GetErrorAddr());
			{
				Common::HostLockCriticalGuard critical;
				Common::LockGuard lock(g_socket_mutex);
				auto& slot = g_sockets[static_cast<size_t>(s)];
				if (slot.generation != state.generation || slot.transport != state.transport) {
					error = Posix::POSIX_EBADF;
				} else if (error == 0) {
					slot.p2p->bound = true;
					slot.p2p->address = address;
					slot.p2p->port = in.sin_port;
					slot.p2p->virtual_port = virtual_port;
				}
				g_p2p_bind_attempts[reservation].reset(); // attempt retained above
			}
			attempt->complete.store(true);
			attempt->complete.notify_all();
			if (error != 0) { return SetGuestSocketError(error); }
			break;
		}
		LOGF("\t UDPP2P bound UDP port=%u virtual port=%u\n", ntohs(in.sin_port), ntohs(virtual_port));
		return 0;
	}

	std::shared_ptr<const std::string> name;
	std::shared_ptr<const std::string> host_name;
	if (host_addr.ss_family == AF_UNIX) {
		try {
			name = std::make_shared<const std::string>(std::move(unix_guest_path));
			host_name = std::make_shared<const std::string>(UnixHostPath(host_addr, host_addrlen));
		}
		catch (const std::bad_alloc&) { return SetGuestSocketError(Posix::POSIX_ENOMEM); }
	}
	Common::HostLockCriticalGuard bind_critical;
	Common::LockGuard bind_lock(state.transport->unix_bind_mutex);
	if (::bind(socket, reinterpret_cast<const sockaddr*>(&host_addr), host_addrlen) != 0) {
		return SetHostSocketError();
	}

	if (host_addr.ss_family == AF_UNIX) {
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_socket_mutex);
		auto&             slot = g_sockets[static_cast<size_t>(s)];
		if (slot.generation == state.generation && slot.transport == state.transport) {
			slot.transport->unix_guest_path.swap(name); // old name retires after unlock
			slot.transport->unix_bound_host_path.swap(host_name);
		}
	}

	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Connect(int s, const void* addr, uint32_t addrlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t addr    = 0x%016" PRIx64 "\n"
	     "\t addrlen = %" PRIu32 "\n",
	     s, reinterpret_cast<uint64_t>(addr), addrlen);

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot state;
	int trace_family = -1;
	unsigned trace_port = 0;
	std::array<uint8_t, 4> trace_address {};
	const auto finish = [&](int result) {
		if (NetworkTraceEnabled()) {
			static std::atomic_uint calls {0};
			if (calls.fetch_add(1, std::memory_order_relaxed) < 128) {
				if (trace_family == 2) {
					LOGF("network-connect: fd=%d family=%d remote=%u.%u.%u.%u:%u result=%d errno=%d\n",
					     s, trace_family, trace_address[0], trace_address[1], trace_address[2], trace_address[3], trace_port,
					     result, result < 0 ? *Posix::GetErrorAddr() : 0);
				} else {
					LOGF("network-connect: fd=%d family=%d length=%u result=%d errno=%d\n",
					     s, trace_family, addrlen, result, result < 0 ? *Posix::GetErrorAddr() : 0);
				}
			}
		}
		return result;
	};
	if (addr == nullptr) {
		return finish(SetGuestSocketError(Posix::POSIX_EFAULT));
	}
	if (!GetSocketBackend(s, &socket, &state)) {
		return finish(-1);
	}
	if (state.p2p || state.transport->pair) { return finish(SetGuestSocketError(Posix::POSIX_EOPNOTSUPP)); }

	sockaddr_storage host_addr {};
	SocketLength     host_addrlen = 0;
	if (ConvertGuestSockaddr(addr, addrlen, &host_addr, &host_addrlen) != 0) {
		return finish(-1);
	}
	if (NetworkTraceEnabled()) {
		trace_family = host_addr.ss_family == AF_INET ? 2 : host_addr.ss_family == AF_INET6 ? 28 :
		               host_addr.ss_family == AF_UNIX ? 1 : -1;
		if (trace_family == 2) {
			const auto& address = reinterpret_cast<const sockaddr_in&>(host_addr);
			std::memcpy(trace_address.data(), &address.sin_addr, trace_address.size());
			trace_port = ntohs(address.sin_port);
		}
	}

	int peer_pid = -1;
	if (host_addr.ss_family == AF_UNIX) {
		if (EnsureUnixClientEndpoint(s, state, socket) != 0) { return finish(-1); }
		peer_pid = UnixEndpointOwner(UnixHostPath(host_addr, host_addrlen));
	}
	if (::connect(socket, reinterpret_cast<const sockaddr*>(&host_addr), host_addrlen) != 0) {
		SetHostSocketError();
#if defined(_WIN32)
		// Winsock starts a nonblocking connection with WSAEWOULDBLOCK;
		// BSD connect reports EINPROGRESS. Receive/send keep EWOULDBLOCK.
		if (*Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK) {
			*Posix::GetErrorAddr() = Posix::POSIX_EINPROGRESS;
		}
#endif
		const auto error = *Posix::GetErrorAddr();
		if (host_addr.ss_family == AF_UNIX &&
		    (error == Posix::POSIX_EINPROGRESS || error == Posix::POSIX_EWOULDBLOCK)) {
			state.transport->peer_pid.store(peer_pid);
		}
		return finish(-1);
	}
	if (host_addr.ss_family == AF_UNIX) { state.transport->peer_pid.store(peer_pid); }

	return finish(0);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Listen(int s, int backlog) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t backlog = %d\n",
	     s, backlog);

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot state;
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}
	if (state.p2p || state.transport->pair) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }

	if (::listen(socket, backlog) != 0) {
		return SetHostSocketError();
	}

	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Accept(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t addr    = 0x%016" PRIx64 "\n"
	     "\t addrlen = 0x%016" PRIx64 "\n",
	     s, reinterpret_cast<uint64_t>(addr), reinterpret_cast<uint64_t>(addrlen));

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot   listener;
	if (!GetSocketBackend(s, &socket, &listener)) {
		return -1;
	}
	if (listener.p2p || listener.transport->pair) {
		return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
	}

	std::shared_ptr<SocketTransport> transport;
	try { transport = std::make_shared<SocketTransport>(INVALID_NATIVE_SOCKET); }
	catch (const std::bad_alloc&) { return SetGuestSocketError(Posix::POSIX_ENOMEM); }
	bool nonblocking = false;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_socket_mutex);
		// Snapshot the retained listener, not a possibly reused numeric fd. Shared
		// immutable name copies cannot allocate or destroy its final owner here.
		transport->unix_guest_path = listener.transport->unix_guest_path;
#if defined(_WIN32)
		nonblocking = listener.transport->pair_nonblocking.load();
#endif
	}
	sockaddr_storage host_addr {};
	SocketLength     host_addrlen = sizeof(host_addr);
	NativeSocket     accepted =
	    ::accept(socket, reinterpret_cast<sockaddr*>(&host_addr), &host_addrlen);
	if (accepted == INVALID_NATIVE_SOCKET) {
		return SetHostSocketError();
	}

	transport->socket = accepted;
	transport->owner_pid = listener.transport->owner_pid;
	if (host_addr.ss_family == AF_UNIX) {
		transport->peer_pid.store(UnixEndpointOwner(UnixHostPath(host_addr, host_addrlen)));
	}
	// Preserve Windows listener-mode inheritance and Linux's blocking accept
	// default. Set the unpublished host mode explicitly before publishing its
	// metadata; a racing Windows listener SETFL cannot make the two disagree.
	if (PairSetNonblocking(*transport, nonblocking) != 0) { return -1; }
	const int fd = AllocSocketFd(transport);
	if (fd < 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EMFILE;
		return -1;
	}

	// BSD ignores the length pointer when no address buffer is supplied.
	if (addr != nullptr) {
		if (addrlen == nullptr) {
			SocketClose(fd);
			*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
			return -1;
		}
		if (ConvertHostSockaddr(&host_addr, host_addrlen, addr, addrlen) != 0) {
			SocketClose(fd);
			return -1;
		}
	}

	return fd;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Shutdown(int s, int how) {
	PRINT_NAME();

	LOGF("\t s   = %d\n"
	     "\t how = %d\n",
	     s, how);

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot state;
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}
	if (state.p2p) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }

	if (how < 0 || how > 2) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	if (::shutdown(socket, how) != 0) {
		return SetHostSocketError();
	}
	if (state.transport->pair) {
		const auto index = state.transport->pair_index;
		if (how == 0 || how == 2) { state.transport->pair->read_shutdown[index].store(true); }
		if (how == 1 || how == 2) { state.transport->pair->write_shutdown[index].store(true); }
	}

#if defined(_WIN32)
	if (how == SD_RECEIVE || how == SD_BOTH) {
		state.transport->receive_shutdown.store(true, std::memory_order_relaxed);
	}
#endif
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Getsockname(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t addr    = 0x%016" PRIx64 "\n"
	     "\t addrlen = 0x%016" PRIx64 "\n",
	     s, reinterpret_cast<uint64_t>(addr), reinterpret_cast<uint64_t>(addrlen));

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot   state;
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}
	if (addr == nullptr || addrlen == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EFAULT);
	}

	sockaddr_storage host_addr {};
	SocketLength     host_addrlen = sizeof(host_addr);
	if (::getsockname(socket, reinterpret_cast<sockaddr*>(&host_addr), &host_addrlen) != 0) {
		return SetHostSocketError();
	}

	std::shared_ptr<const std::string> unix_name;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_socket_mutex);
		unix_name = state.transport->unix_guest_path;
	}
	if (state.transport->pair) {
		// Neither construction-side host name is guest-visible. Both ends are unnamed.
		sockaddr_un unnamed {};
		unnamed.sun_family = AF_UNIX;
		std::memcpy(&host_addr, &unnamed, sizeof(unnamed));
		host_addrlen = static_cast<SocketLength>(offsetof(sockaddr_un, sun_path));
	}
	const int result = ConvertHostSockaddr(&host_addr, host_addrlen, addr, addrlen, unix_name.get());
	if (result == 0 && state.p2p) {
		std::memcpy(static_cast<uint8_t*>(addr) + 8, &state.p2p->virtual_port, sizeof(uint16_t));
	}
	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Getpeername(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();
	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot state;
	if (!GetSocketBackend(s, &socket, &state)) { return -1; }
	if (addr == nullptr || addrlen == nullptr) { return SetGuestSocketError(Posix::POSIX_EFAULT); }
	if (state.p2p) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }

	sockaddr_storage host_addr {};
	SocketLength host_length = sizeof(host_addr);
	if (::getpeername(socket, reinterpret_cast<sockaddr*>(&host_addr), &host_length) != 0) {
		return SetHostSocketError();
	}
	if (state.transport->pair) {
		// Socketpair may use a host TCP transport; its guest endpoints are unnamed AF_LOCAL.
		sockaddr_un unnamed {};
		unnamed.sun_family = AF_UNIX;
		std::memcpy(&host_addr, &unnamed, sizeof(unnamed));
		host_length = static_cast<SocketLength>(offsetof(sockaddr_un, sun_path));
	} else if (host_addr.ss_family == AF_UNIX) {
		// Named Unix peers need retained guest-name metadata, not host paths or an
		// invented unnamed address. This operation is not implemented for them yet.
		return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
	}

	// BSD reports the complete address length and copies only the caller's capacity.
	// Convert into local storage first: the existing IPv4 converter requires 16 bytes.
	std::array<uint8_t, GUEST_SOCKADDR_UN_SIZE> guest {};
	uint32_t length = static_cast<uint32_t>(guest.size());
	if (ConvertHostSockaddr(&host_addr, host_length, guest.data(), &length) != 0) { return -1; }
	std::memcpy(addr, guest.data(), std::min(*addrlen, length));
	*addrlen = length;
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Getsockopt(int s, int level, int optname, void* optval, uint32_t* optlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t level   = 0x%08" PRIx32 "\n"
	     "\t optname = 0x%08" PRIx32 "\n",
	     s, static_cast<uint32_t>(level), static_cast<uint32_t>(optname));

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot   state;
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}
	if (optval == nullptr || optlen == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EFAULT);
	}
	if (state.p2p) {
		const auto* value = level == 0xffff ? P2pSocketOption(*state.p2p, optname) : nullptr;
		if (value == nullptr) {
			return SetGuestSocketError(Posix::POSIX_ENOPROTOOPT);
		}
		if (*optlen < sizeof(int)) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		std::memcpy(optval, value, sizeof(int));
		*optlen = sizeof(int);
		return 0;
	}

	const bool socket_error = (level == 0xffff && optname == 0x1007);
	if (state.transport->pair && !socket_error) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
	const bool send_timeout = (level == 0xffff && optname == 0x1105);
	const bool ipv6_only = (level == 41 && optname == 27);
	optname                = ConvertSocketOptionName(level, optname);
	if (optname < 0) {
		return SetGuestSocketError(Posix::POSIX_ENOPROTOOPT);
	}
#if defined(_WIN32)
	DWORD timeout {};
#else
	timeval timeout {};
#endif
	SocketLength len = static_cast<SocketLength>(*optlen);
	void*        value = optval;
	uint32_t ipv6_value = 0;
	if (ipv6_only) {
		if (*optlen < sizeof(int)) { return SetGuestSocketError(Posix::POSIX_EINVAL); }
		// Winsock may return a one-byte BOOLEAN. The guest option ABI is an
		// int; never leave its upper bytes unchanged or expose the host length.
		value = &ipv6_value;
		len = sizeof(ipv6_value);
	}
	if (send_timeout) {
		if (*optlen < sizeof(int)) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		value = &timeout;
		len   = sizeof(timeout);
	}
	if (::getsockopt(socket, ConvertSocketOptionLevel(level), optname, static_cast<char*>(value),
	                 &len) != 0) {
		return SetHostSocketError();
	}
	if (send_timeout) {
#if defined(_WIN32)
		const int64_t usec = static_cast<int64_t>(timeout) * 1000;
#else
		const int64_t usec = static_cast<int64_t>(timeout.tv_sec) * 1'000'000 + timeout.tv_usec;
#endif
		const int guest_timeout =
		    static_cast<int>(std::min<int64_t>(usec, std::numeric_limits<int>::max()));
		std::memcpy(optval, &guest_timeout, sizeof(guest_timeout));
		len = sizeof(guest_timeout);
	}
	if (ipv6_only) {
		const int guest_value = ipv6_value != 0;
		std::memcpy(optval, &guest_value, sizeof(guest_value));
		len = sizeof(guest_value);
	}
	if (socket_error && len >= static_cast<SocketLength>(sizeof(int))) {
		auto* error = static_cast<int*>(optval);
		*error = ConvertHostSocketError(*error);
	}
	*optlen = static_cast<uint32_t>(len);
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Setsockopt(int s, int level, int optname, const void* optval, uint32_t optlen) {
	PRINT_NAME();

	LOGF("\t s       = %d\n"
	     "\t level   = 0x%08" PRIx32 "\n"
	     "\t optname = 0x%08" PRIx32 "\n"
	     "\t optlen  = %" PRIu32 "\n",
	     s, static_cast<uint32_t>(level), static_cast<uint32_t>(optname), optlen);

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot   state;
	if (optval == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EFAULT);
	}
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}
	if (state.p2p) {
		// Read guest input before locking; only fixed-size endpoint metadata is
		// touched in the short publication scope below.
		if (level != 0xffff || P2pSocketOption(*state.p2p, optname) == nullptr) {
			return SetGuestSocketError(Posix::POSIX_ENOPROTOOPT);
		}
		if (optlen != sizeof(int)) { return SetGuestSocketError(Posix::POSIX_EINVAL); }
		int value = 0;
		std::memcpy(&value, optval, sizeof(value));
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_socket_mutex);
		auto&             slot = g_sockets[static_cast<size_t>(s)];
		if (slot.generation != state.generation || slot.transport != state.transport) {
			return SetGuestSocketError(Posix::POSIX_EBADF);
		}
		auto* option = level == 0xffff ? P2pSocketOption(*slot.p2p, optname) : nullptr;
		if (option == nullptr) {
			return SetGuestSocketError(Posix::POSIX_ENOPROTOOPT);
		}
		if (optname == 0x1001 || optname == 0x1002) {
			if (value <= 0) {
				return SetGuestSocketError(Posix::POSIX_EINVAL);
			}
			*option = optname == 0x1002 ? std::min(value, 512 * 1024) : value;
		} else {
			*option = value != 0;
		}
		return 0;
	}

#if defined(__linux__)
	if (level == 0xffff && optname == 0x10000) {
		if (optlen != sizeof(int)) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		int enabled = 0;
		std::memcpy(&enabled, optval, sizeof(enabled));
		// Native sends preserve 255.255.255.255 without changing broadcast permission.
		return enabled != 0 ? 0 : SetGuestSocketError(Posix::POSIX_ENOPROTOOPT);
	}
#endif

	constexpr int ORBIS_SO_NBIO = 0x1200;
	if (state.transport->pair && !(level == 0xffff &&
	    (optname == ORBIS_SO_NBIO || optname == 0x0800))) {
		// In particular, do not let unsupported linger change pair close into
		// an unbounded operation or an untracked deferred closesocket failure.
		return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
	}
#if defined(_WIN32)
	// Winsock never raises SIGPIPE (also not for AF_UNIX), so BSD SO_NOSIGPIPE is already the
	// host behavior. libSceComposite requires the option to succeed before connect().
	constexpr int ORBIS_SO_NOSIGPIPE = 0x0800;
	if (level == 0xffff && optname == ORBIS_SO_NOSIGPIPE) {
		return optlen == sizeof(int) ? 0 : SetGuestSocketError(Posix::POSIX_EINVAL);
	}
#endif
	if (ConvertSocketOptionLevel(level) == SOL_SOCKET && optname == ORBIS_SO_NBIO &&
	    optlen >= sizeof(int)) {
		const bool enabled = *static_cast<const int*>(optval) != 0;
		return SetSocketNonblocking(s, state, enabled);
	}
	const bool send_timeout = (level == 0xffff && optname == 0x1105);
	optname                = ConvertSocketOptionName(level, optname);
	if (optname < 0) {
		return SetGuestSocketError(Posix::POSIX_ENOPROTOOPT);
	}
#if defined(_WIN32)
	DWORD timeout {};
#else
	timeval timeout {};
#endif
	if (send_timeout) {
		if (optlen != sizeof(int)) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		int usec = 0;
		std::memcpy(&usec, optval, sizeof(usec));
		usec = std::max(usec, 0);
#if defined(_WIN32)
		timeout = static_cast<DWORD>((static_cast<int64_t>(usec) + 999) / 1000);
#else
		timeout.tv_sec  = usec / 1'000'000;
		timeout.tv_usec = usec % 1'000'000;
#endif
		optval = &timeout;
		optlen = sizeof(timeout);
	}

	if (::setsockopt(socket, ConvertSocketOptionLevel(level), optname,
	                 static_cast<const char*>(optval), static_cast<SocketLength>(optlen)) != 0) {
		return SetHostSocketError();
	}

	return 0;
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Send(int s, const void* buf, uint64_t len, int flags) {
	return Sendto(s, buf, len, flags, nullptr, 0);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Sendto(int s, const void* buf, uint64_t len, int flags, const void* addr,
                             uint32_t addrlen) {
	PRINT_NAME();

	LOGF("\t s     = %d\n"
	     "\t buf   = 0x%016" PRIx64 "\n"
	     "\t len   = %" PRIu64 "\n"
	     "\t flags = 0x%08" PRIx32 "\n"
	     "\t addr  = 0x%016" PRIx64 "\n"
	     "\t addrlen = %" PRIu32 "\n",
	     s, reinterpret_cast<uint64_t>(buf), len, static_cast<uint32_t>(flags),
	     reinterpret_cast<uint64_t>(addr), addrlen);

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	if (buf == nullptr) {
		return SetGuestSocketError(Posix::POSIX_EFAULT);
	}
	// UDPP2P requires protocol framing; the native-only lookup rejects raw sends.
	SocketSlot state;
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}
	if (state.p2p) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }

	int host_flags = ConvertMessageFlags(flags);
	if (host_flags < 0) {
		return -1;
	}
	if (state.transport->pair) {
		if (addr != nullptr) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
#if defined(_WIN32)
		// Winsock has no per-call MSG_DONTWAIT. Never silently block instead.
		if ((flags & 0x80) != 0) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
#else
		host_flags |= MSG_NOSIGNAL; // guest SIGPIPE delivery is explicitly unimplemented
#endif
		if (PairSendClosed(*state.transport)) { return SetGuestSocketError(Posix::POSIX_EPIPE); }
	}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	// One-shot calling-thread hook: ownership is retained, but no table/mode
	// lock or critical guard spans this callback or the actual host send.
	if (auto hook = std::exchange(g_retained_send_hook, nullptr)) {
		hook(std::exchange(g_retained_send_context, nullptr));
	}
#endif

	const auto host_len = static_cast<SocketIoLength>(
	    std::min<uint64_t>(len, std::numeric_limits<SocketIoLength>::max()));
	int64_t result = 0;
	if (addr == nullptr) {
		result = ::send(socket, static_cast<const char*>(buf), host_len, host_flags);
	} else {
		sockaddr_storage host_addr {};
		SocketLength     host_addrlen = 0;
		if (ConvertGuestSockaddr(addr, addrlen, &host_addr, &host_addrlen) != 0) {
			return -1;
		}
		result = ::sendto(socket, static_cast<const char*>(buf), host_len, host_flags,
		                  reinterpret_cast<const sockaddr*>(&host_addr), host_addrlen);
	}
	if (result < 0) {
#if defined(_WIN32)
		const int error = WSAGetLastError();
		if (state.transport->pair && (error == WSAESHUTDOWN || error == WSAECONNRESET ||
		    (error == WSAENOTCONN && PairSendClosed(*state.transport)))) {
			return SetGuestSocketError(Posix::POSIX_EPIPE);
		}
#endif
		return SetHostSocketError();
	}

	return result;
}

#if defined(_WIN32)
static int ReceiveStream(SocketTransport& transport, char* output, int length, int flags,
                         bool dontwait) {
	std::unique_lock lock(transport.receive_mutex, std::defer_lock);
	if (dontwait) {
		if (!lock.try_lock()) {
			return SetGuestSocketError(Posix::POSIX_EWOULDBLOCK);
		}
	} else {
		lock.lock();
	}
	if (transport.receive_shutdown.load(std::memory_order_relaxed)) {
		return SetGuestSocketError(Posix::POSIX_ESHUTDOWN);
	}
	auto&           buffered = transport.receive_buffer;
	const bool      peek     = (flags & MSG_PEEK) != 0;
	const bool      waitall  = (flags & MSG_WAITALL) != 0;
	int             copied   = static_cast<int>(std::min<size_t>(buffered.size(), length));
	if (copied != 0) {
		std::memcpy(output, buffered.data(), copied);
		if (!peek) {
			buffered.erase(buffered.begin(), buffered.begin() + copied);
			transport.buffered_bytes.store(buffered.size(), std::memory_order_relaxed);
		}
	}
	if (copied == length || (copied != 0 && !waitall)) {
		return copied;
	}

	// Winsock rejects PEEK | WAITALL. Consume into transport-owned storage when peeking,
	// and let ordinary recv report EOF, timeouts, or nonblocking partial progress.
	flags &= ~(MSG_PEEK | MSG_WAITALL);
	do {
		if (transport.receive_shutdown.load(std::memory_order_relaxed)) {
			return copied != 0 ? copied : SetGuestSocketError(Posix::POSIX_ESHUTDOWN);
		}
		const auto socket = transport.socket.load();
		if (dontwait) {
			fd_set readable {};
			FD_SET(socket, &readable);
			timeval immediate {};
			const int ready = ::select(0, &readable, nullptr, nullptr, &immediate);
			if (ready <= 0) {
				return copied != 0 ? copied : (ready < 0 ? SetHostSocketError() :
				    SetGuestSocketError(Posix::POSIX_EWOULDBLOCK));
			}
		}
		const auto previous_size = buffered.size();
		const int  requested     = peek ? std::min(length - copied, 64 * 1024) : length - copied;
		char*      destination   = output + copied;
		if (peek) {
			buffered.resize(previous_size + requested);
			destination = buffered.data() + previous_size;
		}
		const int received = ::recv(socket, destination, requested, flags);
		if (peek) {
			if (received > 0) {
				std::memcpy(output + copied, destination, received);
			}
			buffered.resize(previous_size + std::max(received, 0));
			transport.buffered_bytes.store(buffered.size(), std::memory_order_relaxed);
		}
		if (received <= 0) {
			return copied != 0 || received == 0 ? copied : SetHostSocketError();
		}
		copied += received;
	} while (waitall && copied < length);
	return copied;
}
#endif

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Recv(int s, void* buf, uint64_t len, int flags) {
	return Recvfrom(s, buf, len, flags, nullptr, nullptr);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Recvfrom(int s, void* buf, uint64_t len, int flags, void* addr,
                               uint32_t* addrlen) {
	PRINT_NAME();

	LOGF("\t s     = %d\n"
	     "\t buf   = 0x%016" PRIx64 "\n"
	     "\t len   = %" PRIu64 "\n"
	     "\t flags = 0x%08" PRIx32 "\n"
	     "\t addr  = 0x%016" PRIx64 "\n"
	     "\t addrlen = 0x%016" PRIx64 "\n",
	     s, reinterpret_cast<uint64_t>(buf), len, static_cast<uint32_t>(flags),
	     reinterpret_cast<uint64_t>(addr), reinterpret_cast<uint64_t>(addrlen));

	if (buf == nullptr || (addr != nullptr && addrlen == nullptr)) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}

	NativeSocket socket = INVALID_NATIVE_SOCKET;
	SocketSlot   state;
	if (!GetSocketBackend(s, &socket, &state)) {
		return -1;
	}
#if defined(_WIN32)
	if (state.transport->pair && (flags & 0x80) != 0) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
#endif

	const int host_flags = ConvertMessageFlags(flags);
	if (host_flags < 0) {
		return -1;
	}
	if (state.p2p) {
		if (!state.p2p->bound || (!state.p2p->nonblocking && (flags & 0x80) == 0)) {
			return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
		}
		char pending;
		if (::recv(socket, &pending, sizeof(pending), MSG_PEEK) < 0) {
			SetHostSocketError();
			// Winsock reports a peeked datagram's truncation as EMSGSIZE.
			if (*Posix::GetErrorAddr() != Posix::POSIX_EMSGSIZE) {
				return -1;
			}
		}
		// Pending UDPP2P packets still require framing and optional decryption.
		return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP);
	}

	const auto host_len = static_cast<SocketIoLength>(
	    std::min<uint64_t>(len, std::numeric_limits<SocketIoLength>::max()));
	sockaddr_storage host_addr {};
	SocketLength     host_addrlen = sizeof(host_addr);
	int64_t          result       = 0;
#if defined(_WIN32)
	if (state.transport->type == SOCK_STREAM) {
		if (addr != nullptr &&
		    ::getpeername(socket, reinterpret_cast<sockaddr*>(&host_addr), &host_addrlen) != 0) {
			return SetHostSocketError();
		}
		result = ReceiveStream(*state.transport, static_cast<char*>(buf), host_len, host_flags,
		                       (flags & 0x80) != 0);
		if (result < 0) {
			return result;
		}
	} else
#endif
	{
		state.transport.reset();
		if (addr == nullptr) {
			result = ::recv(socket, static_cast<char*>(buf), host_len, host_flags);
		} else {
			result = ::recvfrom(socket, static_cast<char*>(buf), host_len, host_flags,
			                    reinterpret_cast<sockaddr*>(&host_addr), &host_addrlen);
		}
	}
#if defined(_WIN32)
	if (result < 0 && WSAGetLastError() == WSAEMSGSIZE) {
		// Winsock copied the datagram prefix; POSIX reports its length as success.
		result = host_len;
	}
#endif
	if (result < 0) {
		return SetHostSocketError();
	}
	if (addr != nullptr && ConvertHostSockaddr(&host_addr, host_addrlen, addr, addrlen) != 0) {
		return -1;
	}

	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Select(int nfds, void* readfds, void* writefds, void* exceptfds,
                         const void* timeout) {
	PRINT_NAME();

	if (nfds < 0 || nfds > SOCKET_FD_MAX) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	static std::atomic_uint32_t select_log_count = 0;
	const bool log_select = select_log_count.fetch_add(1, std::memory_order_relaxed) < 64;

	if (log_select) {
		const auto  read0   = (readfds != nullptr ? *static_cast<const uint64_t*>(readfds) : 0);
		const auto  write0  = (writefds != nullptr ? *static_cast<const uint64_t*>(writefds) : 0);
		const auto  except0 = (exceptfds != nullptr ? *static_cast<const uint64_t*>(exceptfds) : 0);
		const auto* guest_timeout = static_cast<const NetTimeval*>(timeout);
		LOGF("\t nfds       = %d\n"
		     "\t read       = 0x%016" PRIx64 " bits0=0x%016" PRIx64 "\n"
		     "\t write      = 0x%016" PRIx64 " bits0=0x%016" PRIx64 "\n"
		     "\t except     = 0x%016" PRIx64 " bits0=0x%016" PRIx64 "\n"
		     "\t timeout    = 0x%016" PRIx64,
		     nfds, reinterpret_cast<uint64_t>(readfds), read0, reinterpret_cast<uint64_t>(writefds),
		     write0, reinterpret_cast<uint64_t>(exceptfds), except0,
		     reinterpret_cast<uint64_t>(timeout));
		if (guest_timeout != nullptr) {
			LOGF(" sec=%" PRId64 " usec=%" PRId64, guest_timeout->tv_sec, guest_timeout->tv_usec);
		}
		LOGF("\n");
	}

	fd_set host_read {};
	fd_set host_write {};
	fd_set host_except {};
	FD_ZERO(&host_read);
	FD_ZERO(&host_write);
	FD_ZERO(&host_except);

	std::vector<std::pair<int, NativeSocket>> descriptors;
	std::array<std::shared_ptr<SocketTransport>, SOCKET_FD_MAX> retained {};
	std::vector<std::shared_ptr<SocketTransport>> stream_reads;
	std::vector<std::pair<void*, int>> ready_files;
	int host_nfds = 0;
	for (int fd = 0; fd < nfds; fd++) {
		const bool read = GuestFdIsSet(readfds, fd);
		const bool write = GuestFdIsSet(writefds, fd);
		const bool except = GuestFdIsSet(exceptfds, fd);
		if (!read && !write && !except) {
			continue;
		}
		if (!IsSocket(fd) && LibKernel::FileSystem::IsOpenDescriptor(fd)) {
			if (read) { ready_files.emplace_back(readfds, fd); }
			if (write) { ready_files.emplace_back(writefds, fd); }
			continue;
		}
		NativeSocket socket = INVALID_NATIVE_SOCKET;
		SocketSlot state;
#if !defined(_WIN32)
		if (fd < 3) {
			socket = fd;
		} else
#endif
		if (!GetSocketBackend(fd, &socket, &state)) {
			return -1;
		}
		if (state.p2p) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
		retained[fd] = state.transport;
#if defined(_WIN32)
		if ((read && host_read.fd_count == FD_SETSIZE) ||
		    (write && host_write.fd_count == FD_SETSIZE) ||
		    (except && host_except.fd_count == FD_SETSIZE)) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
#else
		if (socket >= FD_SETSIZE) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		host_nfds = std::max(host_nfds, socket + 1);
#endif
		descriptors.emplace_back(fd, socket);
		if (read) {
			FD_SET(socket, &host_read);
#if defined(_WIN32)
			if (state.transport->type == SOCK_STREAM) {
				stream_reads.push_back(state.transport);
			}
#endif
		}
		if (write) {
			FD_SET(socket, &host_write);
		}
		if (except) {
			FD_SET(socket, &host_except);
		}
	}

	timeval host_timeout {};
	timeval* host_timeout_ptr = nullptr;
	if (timeout != nullptr) {
		const auto* guest_timeout = static_cast<const NetTimeval*>(timeout);
		if (guest_timeout->tv_sec < 0 || guest_timeout->tv_usec < 0 ||
		    guest_timeout->tv_usec >= 1'000'000) {
			return SetGuestSocketError(Posix::POSIX_EINVAL);
		}
		host_timeout.tv_sec = static_cast<decltype(host_timeout.tv_sec)>(guest_timeout->tv_sec);
		host_timeout.tv_usec = static_cast<decltype(host_timeout.tv_usec)>(guest_timeout->tv_usec);
		host_timeout_ptr = &host_timeout;
	}
	if (!ready_files.empty()) {
		host_timeout     = {};
		host_timeout_ptr = &host_timeout;
	}

	int result = 0;
#if defined(_WIN32)
	if (descriptors.empty()) {
		Sleep(host_timeout_ptr == nullptr ? INFINITE :
		      static_cast<DWORD>(host_timeout.tv_sec * 1000 + host_timeout.tv_usec / 1000));
	} else
#endif
	{
		result = SelectSockets(host_nfds, readfds != nullptr ? &host_read : nullptr,
		                       writefds != nullptr ? &host_write : nullptr,
		                       exceptfds != nullptr ? &host_except : nullptr, host_timeout_ptr,
		                       stream_reads);
		if (result < 0) {
			return SetHostSocketError();
		}
	}

	GuestFdZero(readfds, nfds);
	GuestFdZero(writefds, nfds);
	GuestFdZero(exceptfds, nfds);
	result = 0;
	for (const auto& [fd, socket]: descriptors) {
		if (FD_ISSET(socket, &host_read)) {
			GuestFdSet(readfds, fd);
			result++;
		}
		if (FD_ISSET(socket, &host_write)) {
			GuestFdSet(writefds, fd);
			result++;
		}
		if (FD_ISSET(socket, &host_except)) {
			GuestFdSet(exceptfds, fd);
			result++;
		}
	}
	for (const auto& [fds, fd]: ready_files) {
		GuestFdSet(fds, fd);
		result++;
	}
	return result;
}

} // namespace Net

namespace Ssl {

LIB_NAME("Ssl", "Ssl");

struct SslData {
	char*  ptr  = nullptr;
	size_t size = 0;
};

struct SslCaCerts {
	SslData* cert_data     = nullptr;
	size_t   cert_data_num = 0;
	void*    pool          = nullptr;
};

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SslInit(uint64_t pool_size) {
	const auto caller = KYTY_IPMI_CALLER; // Directly registered export: capture the guest return address here.
	PRINT_NAME();

	LOGF("\t size = %" PRIu64 "\n", pool_size);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(pool_size == 0);

	auto id = g_net->SslInit(pool_size);
	const int result = id.IsValid() ? id.ToInt() : SSL_ERROR_OUT_OF_SIZE;
	IpmiObserve::SslInitialized(caller, pool_size, result);
	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SslTerm(int ssl_ctx_id) {
	const auto caller = KYTY_IPMI_CALLER;
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	const int result = g_net->SslTerm(Network::Id(ssl_ctx_id)) ? OK : SSL_ERROR_INVALID_ID;
	IpmiObserve::SslTerminated(caller, ssl_ctx_id, result);
	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SslGetCaCerts(int ssl_ctx_id, void* ca_certs) {
	PRINT_NAME();

	LOGF("\t ssl_ctx_id = %d\n", ssl_ctx_id);

	if (ca_certs == nullptr) {
		return SSL_ERROR_INVALID_ARG;
	}

	EXIT_IF(g_net == nullptr);

	if (!g_net->SslValid(Network::Id(ssl_ctx_id))) {
		return SSL_ERROR_INVALID_ID;
	}

	auto* certs          = static_cast<SslCaCerts*>(ca_certs);
	certs->cert_data     = nullptr;
	certs->cert_data_num = 0;
	certs->pool          = nullptr;

	return SSL_ERROR_NOT_FOUND;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SslFreeCaCerts(int ssl_ctx_id, void* ca_certs) {
	PRINT_NAME();

	LOGF("\t ssl_ctx_id = %d\n", ssl_ctx_id);

	if (ca_certs == nullptr) {
		return SSL_ERROR_INVALID_ARG;
	}

	EXIT_IF(g_net == nullptr);

	if (!g_net->SslValid(Network::Id(ssl_ctx_id))) {
		return SSL_ERROR_INVALID_ID;
	}

	auto* certs          = static_cast<SslCaCerts*>(ca_certs);
	certs->cert_data     = nullptr;
	certs->cert_data_num = 0;
	certs->pool          = nullptr;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SslLoadRootCACert(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
	PRINT_NAME();
	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
	return OK;
}

} // namespace Ssl

namespace Http {

struct HttpEpoll {
	Network::Id http_ctx_id = Network::Id(0);
	Network::Id request_id  = Network::Id(0);
	void*       user_arg    = nullptr;
};

struct HttpNBEvent {
	uint32_t events       = 0;
	uint32_t event_detail = 0;
	int      id           = 0;
	void*    user_arg     = nullptr;
};

LIB_NAME("Http", "Http");

static const char* HttpMethodToString(int method) {
	switch (method) {
		case 0: return "GET";
		case 1: return "POST";
		case 2: return "HEAD";
		case 3: return "OPTIONS";
		case 4: return "PUT";
		case 5: return "DELETE";
		case 6: return "TRACE";
		case 7: return "CONNECT";
		default: return nullptr;
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpInit(int memid, int ssl_ctx_id, uint64_t pool_size) {
	PRINT_NAME();

	LOGF("\t memid      = %d\n"
	     "\t ssl_ctx_id = %d\n"
	     "\t size       = %" PRIu64 "\n",
	     memid, ssl_ctx_id, pool_size);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(pool_size == 0);

	auto id = g_net->HttpInit(memid, ssl_ctx_id, pool_size);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpTerm(int http_ctx_id) {
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpTerm(Network::Id(http_ctx_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCreateTemplate(int http_ctx_id, const char* user_agent, int http_ver,
                                     int is_auto_proxy_conf) {
	PRINT_NAME();

	LOGF("\t http_ctx_id        = %d\n"
	     "\t user_agent         = %s\n"
	     "\t http_ver           = %d\n"
	     "\t is_auto_proxy_conf = %d\n",
	     http_ctx_id, user_agent, http_ver, is_auto_proxy_conf);

	EXIT_IF(g_net == nullptr);

	auto id = g_net->HttpCreateTemplate(Network::Id(http_ctx_id), user_agent, http_ver,
	                                    is_auto_proxy_conf != 0);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpDeleteTemplate(int tmpl_id) {
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteTemplate(Network::Id(tmpl_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetNonblock(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n"
	     "\t enable = %d\n",
	     id, enable);

	if (!g_net->HttpSetNonblock(Network::Id(id), enable != 0)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpsSetSslCallback(int id, HttpsCallback cbfunc, void* user_arg) {
	PRINT_NAME();

	LOGF("\t id     = %d\n", id);

	if (!g_net->HttpsSetSslCallback(Network::Id(id), cbfunc, user_arg)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpsSetMinSslVersion(int id, uint32_t ssl_version) {
	PRINT_NAME();

	LOGF("\t id          = %d\n"
	     "\t ssl_version = %u\n",
	     id, ssl_version);

	if (!g_net->HttpsSetMinSslVersion(Network::Id(id), ssl_version)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpsDisableOption(int id, uint32_t ssl_flags) {
	PRINT_NAME();

	LOGF("\t id        = %d\n"
	     "\t ssl_flags = %u\n",
	     id, ssl_flags);

	if (!g_net->HttpsDisableOption(Network::Id(id), ssl_flags)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetResolveTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n"
	     "\t usec = %u\n",
	     id, usec);

	if (!g_net->HttpSetResolveTimeOut(Network::Id(id), usec)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetResolveRetry(int id, int32_t retry) {
	PRINT_NAME();

	LOGF("\t id    = %d\n"
	     "\t retry = %d\n",
	     id, retry);

	if (!g_net->HttpSetResolveRetry(Network::Id(id), retry)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetConnectTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n"
	     "\t usec = %u\n",
	     id, usec);

	if (!g_net->HttpSetConnectTimeOut(Network::Id(id), usec)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetSendTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n"
	     "\t usec = %u\n",
	     id, usec);

	if (!g_net->HttpSetSendTimeOut(Network::Id(id), usec)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetRecvTimeOut(int id, uint32_t usec) {
	PRINT_NAME();

	LOGF("\t id   = %d\n"
	     "\t usec = %u\n",
	     id, usec);

	if (!g_net->HttpSetRecvTimeOut(Network::Id(id), usec)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetAutoRedirect(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n"
	     "\t enable = %d\n",
	     id, enable);

	if (!g_net->HttpSetAutoRedirect(Network::Id(id), enable)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetAuthEnabled(int id, int enable) {
	PRINT_NAME();

	LOGF("\t id     = %d\n"
	     "\t enable = %d\n",
	     id, enable);

	if (!g_net->HttpSetAuthEnabled(Network::Id(id), enable)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetCookieEnabled(int id, int enable) {
	// Original 13.60 libSceHttp +14b60, XNUoD2B9a6A; ShellCore +cc5c36
	// disables cookies on each NP worker template before starting its queue.
	// Retail SNS also enables cookies. Each HTTP context now owns a WinHTTP
	// session; the inherited per-object switch controls both send and receive.
	if (g_net == nullptr) { return HTTP_ERROR_BEFORE_INIT; }
#if !defined(_WIN32)
	if (!g_net->HttpValidBase(Network::Id(id))) { return HTTP_ERROR_INVALID_ID; }
	if (enable != 0) { return HTTP_ERROR_UNKNOWN; }
#endif
	return g_net->HttpSetCookieEnabled(Network::Id(id), enable) ? OK : HTTP_ERROR_INVALID_ID;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpAddRequestHeader(int id, const char* name, const char* value, uint32_t mode) {
	PRINT_NAME();

	LOGF("\t id    = %d\n"
	     "\t name  = %s\n"
	     "\t value = %s\n"
	     "\t mode  = %u\n",
	     id, name != nullptr ? name : "(null)", value != nullptr ? "<omitted>" : "(null)", mode);

	EXIT_NOT_IMPLEMENTED(mode != 0 && mode != 1);

	if (!g_net->HttpAddRequestHeader(Network::Id(id), name, value, mode == 1)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

static int HttpCheckBase(int id) {
	EXIT_IF(g_net == nullptr);
	return g_net->HttpValidBase(Network::Id(id)) ? OK : HTTP_ERROR_INVALID_ID;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetRedirectCallback(int id, void*, void*) {
	return HttpCheckBase(id);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetCookieRecvCallback(int id, void*, void*) {
	return HttpCheckBase(id);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetRequestStatusCallback(int id, void*, void*) {
	return HttpCheckBase(id);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetInflateGZIPEnabled(int id, int) {
	return HttpCheckBase(id);
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetChunkedTransferEnabled(int id, int enabled) {
	EXIT_IF(g_net == nullptr);
	if (enabled != 0 && enabled != 1) { return HTTP_ERROR_INVALID_VALUE; }
	return g_net->HttpValidRequest(Network::Id(id)) ? OK : HTTP_ERROR_INVALID_ID;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpsLoadCert(int id, int, const void**, const void*, const void*) {
	EXIT_IF(g_net == nullptr);
	return g_net->HttpValid(Network::Id(id)) ? OK : HTTP_ERROR_INVALID_ID;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpsUnloadCert(int id) {
	EXIT_IF(g_net == nullptr);
	return g_net->HttpValid(Network::Id(id)) ? OK : HTTP_ERROR_INVALID_ID;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpsGetSslError(int id, int32_t* error, uint32_t* detail) {
	if (error == nullptr || detail == nullptr) { return HTTP_ERROR_INVALID_VALUE; }
	*error = 0;
	*detail = 0;
	EXIT_IF(g_net == nullptr);
	return g_net->HttpValidRequest(Network::Id(id)) ? OK : HTTP_ERROR_INVALID_ID;
}
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpGetLastErrno(int id, int* error) {
	if (error == nullptr) { return HTTP_ERROR_INVALID_VALUE; }
	*error = 0;
	EXIT_IF(g_net == nullptr);
	return g_net->HttpValidRequest(Network::Id(id)) ? OK : HTTP_ERROR_INVALID_ID;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCreateEpoll(int http_ctx_id, HttpEpollHandle* eh) {
	PRINT_NAME();

	LOGF("\t http_ctx_id = %d\n", http_ctx_id);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValid(Network::Id(http_ctx_id)));

	*eh = new HttpEpoll;

	(*eh)->http_ctx_id = Network::Id(http_ctx_id);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpDestroyEpoll(int http_ctx_id, HttpEpollHandle eh) {
	PRINT_NAME();

	LOGF("\t http_ctx_id = %d\n", http_ctx_id);

	EXIT_IF(g_net == nullptr);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValid(Network::Id(http_ctx_id)));

	delete eh;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetEpoll(int id, HttpEpollHandle eh, void* user_arg) {
	PRINT_NAME();

	LOGF("\t id = %d\n", id);

	EXIT_NOT_IMPLEMENTED(eh == nullptr);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValidRequest(Network::Id(id)));

	eh->request_id = Network::Id(id);
	eh->user_arg   = user_arg;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpUnsetEpoll(int id) {
	PRINT_NAME();

	LOGF("\t id = %d\n", id);

	EXIT_NOT_IMPLEMENTED(!g_net->HttpValidRequest(Network::Id(id)));

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSendRequest(int request_id, const void* post_data, size_t size) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n", request_id);

	EXIT_IF(g_net == nullptr);

	return g_net->HttpSend(Network::Id(request_id), post_data, size);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpReadData(int request_id, void* data, size_t size) {
	return g_net ? g_net->HttpRead(Network::Id(request_id), data, size) : HTTP_ERROR_BEFORE_INIT;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpAbortRequest(int request_id) {
	PRINT_NAME();

	LOGF("\t request_id = %d\n", request_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpMarkRequestSent(Network::Id(request_id), HTTP_ERROR_ABORTED)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpWaitRequest(HttpEpollHandle eh, HttpNBEvent* nbev, int maxevents,
                                  int timeout) {
	PRINT_NAME();

	LOGF("\t eh        = 0x%016" PRIx64 "\n"
	     "\t nbev      = 0x%016" PRIx64 "\n"
	     "\t maxevents = %d\n"
	     "\t timeout   = %d\n",
	     reinterpret_cast<uint64_t>(eh), reinterpret_cast<uint64_t>(nbev), maxevents, timeout);

	EXIT_IF(g_net == nullptr);

	if (eh == nullptr || maxevents < 0 || (maxevents > 0 && nbev == nullptr)) {
		return HTTP_ERROR_INVALID_VALUE;
	}
	if (maxevents == 0) return 0;
	const int result = g_net->HttpWait(eh->request_id, timeout);
	if (result == HTTP_ERROR_EAGAIN || result == HTTP_ERROR_BEFORE_SEND) return 0;
	if (result == HTTP_ERROR_INVALID_ID) return result;
	nbev[0] = {result == 0 ? 3u : 8u, static_cast<uint32_t>(result), eh->request_id.ToInt(), eh->user_arg};
	return 1;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpGetStatusCode(int request_id, int* status_code) {
	PRINT_NAME();

	LOGF("\t request_id  = %d\n"
	     "\t status_code = 0x%016" PRIx64 "\n",
	     request_id, reinterpret_cast<uint64_t>(status_code));

	if (status_code == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	*status_code = 0;

	EXIT_IF(g_net == nullptr);

	int send_result = HTTP_ERROR_BEFORE_SEND;
	if (!g_net->HttpGetRequestResponse(Network::Id(request_id), &send_result, status_code, nullptr,
	                                   nullptr, nullptr)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return send_result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpGetAllResponseHeaders(int request_id, char** header, size_t* header_size) {
	PRINT_NAME();

	LOGF("\t request_id  = %d\n"
	     "\t header      = 0x%016" PRIx64 "\n"
	     "\t header_size = 0x%016" PRIx64 "\n",
	     request_id, reinterpret_cast<uint64_t>(header), reinterpret_cast<uint64_t>(header_size));

	if (header == nullptr || header_size == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	*header      = nullptr;
	*header_size = 0;

	EXIT_IF(g_net == nullptr);

	int         send_result  = HTTP_ERROR_BEFORE_SEND;
	const char* headers      = nullptr;
	size_t      headers_size = 0;
	if (!g_net->HttpGetRequestResponse(Network::Id(request_id), &send_result, nullptr, &headers,
	                                   &headers_size, nullptr)) {
		return HTTP_ERROR_INVALID_ID;
	}

	*header      = const_cast<char*>(headers);
	*header_size = headers_size;

	return send_result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpGetResponseContentLength(int request_id, int* result,
                                               uint64_t* content_length) {
	PRINT_NAME();

	LOGF("\t request_id     = %d\n"
	     "\t result         = 0x%016" PRIx64 "\n"
	     "\t content_length = 0x%016" PRIx64 "\n",
	     request_id, reinterpret_cast<uint64_t>(result),
	     reinterpret_cast<uint64_t>(content_length));

	if (result == nullptr || content_length == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	*result         = 0;
	*content_length = 0;

	EXIT_IF(g_net == nullptr);

	int send_result = HTTP_ERROR_BEFORE_SEND;
	if (!g_net->HttpGetRequestResponse(Network::Id(request_id), &send_result, nullptr, nullptr,
	                                   nullptr, content_length)) {
		return HTTP_ERROR_INVALID_ID;
	}

	*result = (send_result == OK ? 0 : -1);
	return send_result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCreateConnection(int tmpl_id, const char* server_name, const char* scheme,
                                       uint16_t port, int enable_keep_alive) {
	PRINT_NAME();

	LOGF("\t tmpl_id           = %d\n"
	     "\t server_name       = %s\n"
	     "\t scheme            = %s\n"
	     "\t port              = %u\n"
	     "\t enable_keep_alive = %d\n",
	     tmpl_id, server_name, scheme, static_cast<unsigned>(port), enable_keep_alive);

	EXIT_IF(g_net == nullptr);

	if (server_name == nullptr || scheme == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	auto id = g_net->HttpCreateConnection(Network::Id(tmpl_id), server_name, scheme, port,
	                                      enable_keep_alive != 0);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCreateConnectionWithURL(int tmpl_id, const char* url, int enable_keep_alive) {
	PRINT_NAME();

	LOGF("\t tmpl_id           = %d\n"
	     "\t url               = %s\n"
	     "\t enable_keep_alive = %d\n",
	     tmpl_id, url != nullptr ? "<omitted>" : "(null)", enable_keep_alive);

	EXIT_IF(g_net == nullptr);

	if (url == nullptr || url[0] == '\0') {
		return HTTP_ERROR_INVALID_URL;
	}

	auto id = g_net->HttpCreateConnectionWithURL(Network::Id(tmpl_id), url, enable_keep_alive != 0);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpDeleteConnection(int conn_id) {
	PRINT_NAME();

	LOGF("\t conn_id = %d\n", conn_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteConnection(Network::Id(conn_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCreateRequest(int conn_id, int method, const char* path,
                                    uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t conn_id        = %d\n"
	     "\t method         = %d\n"
	     "\t path           = %s\n"
	     "\t content_length = %" PRIu64 "\n",
	     conn_id, method, path != nullptr ? "<omitted>" : "(null)", content_length);

	EXIT_IF(g_net == nullptr);

	auto method_name = HttpMethodToString(method);
	if (method_name == nullptr) {
		return HTTP_ERROR_UNKNOWN_METHOD;
	}
	if (path == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	auto id =
	    g_net->HttpCreateRequestWithURL2(Network::Id(conn_id), method_name, path, content_length);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCreateRequest2(int conn_id, const char* method, const char* path,
                                     uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t conn_id        = %d\n"
	     "\t method         = %s\n"
	     "\t path           = %s\n"
	     "\t content_length = %" PRIu64 "\n",
	     conn_id, method, path != nullptr ? "<omitted>" : "(null)", content_length);

	EXIT_IF(g_net == nullptr);

	if (method == nullptr || path == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}

	auto id =
	    g_net->HttpCreateRequestWithURL2(Network::Id(conn_id), method, path, content_length);
	return id.IsValid() ? id.ToInt() : HTTP_ERROR_OUT_OF_MEMORY;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCreateRequestWithURL2(int conn_id, const char* method, const char* url,
                                            uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t conn_id        = %d\n"
	     "\t url            = %s\n"
	     "\t method         = %s\n"
	     "\t content_length = %" PRIu64 "\n",
	     conn_id, url != nullptr ? "<omitted>" : "(null)", method, content_length);

	EXIT_IF(g_net == nullptr);

	if (method == nullptr) {
		return HTTP_ERROR_INVALID_VALUE;
	}
	if (url == nullptr || url[0] == '\0') {
		return HTTP_ERROR_INVALID_URL;
	}

	auto id = g_net->HttpCreateRequestWithURL2(Network::Id(conn_id), method, url, content_length);

	if (!id.IsValid()) {
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpDeleteRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteRequest(Network::Id(req_id))) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetRequestContentLength(int request_id, uint64_t content_length) {
	PRINT_NAME();

	LOGF("\t request_id     = %d\n"
	     "\t content_length = %" PRIu64 "\n",
	     request_id, content_length);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpSetRequestContentLength(Network::Id(request_id), content_length)) {
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

} // namespace Http

namespace NetCtl {

LIB_NAME("NetCtl", "NetCtl");

constexpr int NET_CTL_STATE_DISCONNECTED  = 0;
constexpr int NET_CTL_STATE_IPOBTAINED    = 3;
constexpr int NET_CTL_DEVICE_WIRED        = 0;
constexpr int NET_CTL_DEVICE_WIRELESS     = 1;
constexpr int NET_CTL_LINK_CONNECTED      = 1;
constexpr int NET_CTL_EVENT_DISCONNECTED  = 1;
constexpr int NET_CTL_EVENT_IPOBTAINED    = 3;
constexpr int NET_CTL_IP_DHCP             = 0;
constexpr int NET_CTL_HTTP_PROXY_OFF      = 0;
constexpr int NET_CTL_CALLBACK_MAX        = 8;
constexpr int NET_CTL_ERROR_CALLBACK_MAX  = -2143215357; /* 0x80412103 */
constexpr int NET_CTL_ERROR_ID_NOT_FOUND  = -2143215356; /* 0x80412104 */
constexpr int NET_CTL_ERROR_INVALID_ID    = -2143215355; /* 0x80412105 */
constexpr int NET_CTL_ERROR_INVALID_CODE  = -2143215354; /* 0x80412106 */
constexpr int NET_CTL_ERROR_INVALID_ADDR  = -2143215353; /* 0x80412107 */
constexpr int NET_CTL_ERROR_NOT_CONNECTED = -2143215352; /* 0x80412108 */
constexpr int NET_CTL_ERROR_INVALID_TYPE  = -2143215345; /* 0x8041210f */

struct NetInAddr {
	uint32_t s_addr = 0;
};

struct NetEtherAddr {
	uint8_t data[6];
};

struct NetCtlNatInfo {
	unsigned int size       = sizeof(NetCtlNatInfo);
	int          stunStatus = 0;
	int          natType    = 0;
	NetInAddr    mappedAddr;
};

union NetCtlInfo {
	uint32_t     device;
	NetEtherAddr ether_addr;
	uint32_t     mtu;
	uint32_t     link;
	NetEtherAddr bssid;
	char         ssid[32 + 1];
	uint32_t     wifi_security;
	int32_t      rssi_dbm;
	uint8_t      rssi_percentage;
	uint8_t      channel;
	uint32_t     ip_config;
	char         dhcp_hostname[255 + 1];
	char         pppoe_auth_name[127 + 1];
	char         ip_address[16];
	char         netmask[16];
	char         default_route[16];
	char         primary_dns[16];
	char         secondary_dns[16];
	uint32_t     http_proxy_config;
	char         http_proxy_server[255 + 1];
	uint16_t     http_proxy_port;
};

struct NetCtlCallbackSlot {
	NetCtlCallback func       = nullptr;
	void*          arg        = nullptr;
	int            last_event = 0;
};

static Common::Mutex                     g_net_ctl_callbacks_mutex;
static std::array<NetCtlCallbackSlot, 8> g_net_ctl_callbacks;
static std::atomic_bool g_net_ctl_status_initialized {false};
static std::atomic_bool g_net_ctl_connected {false};

struct HostNetworkInfo {
	bool         connected = false;
	bool         wireless  = false;
	NetEtherAddr ether_addr {};
	char         ip_address[16] {};
	char         netmask[16] {};
	char         default_route[16] {};
	char         primary_dns[16] {};
	char         secondary_dns[16] {};
};

static void CopyIpv4String(char* dst, size_t dst_size, const sockaddr* addr) {
	if (dst == nullptr || dst_size == 0 || addr == nullptr || addr->sa_family != AF_INET) {
		return;
	}

	const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
	inet_ntop(AF_INET, &in->sin_addr, dst, static_cast<socklen_t>(dst_size));
}

#if defined(_WIN32)
static void CopyIpv4Netmask(char* dst, size_t dst_size, uint8_t prefix_len) {
	if (dst == nullptr || dst_size == 0 || prefix_len > 32) {
		return;
	}

	const uint32_t mask = (prefix_len == 0 ? 0 : (0xffffffffu << (32u - prefix_len)));
	in_addr        in {};
	in.S_un.S_addr = htonl(mask);
	inet_ntop(AF_INET, &in, dst, static_cast<socklen_t>(dst_size));
}
#endif

#ifdef KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS
static thread_local void (*g_host_network_query_callback)() = nullptr;
#endif

static HostNetworkInfo QueryHostNetworkInfo() {
	// Adapter discovery enters host DLLs that acquire the process heap lock.
	// A guest GC suspension there would also block the UI and unrelated host
	// allocations. Protect the entire query, including temporary-buffer cleanup.
	Common::HostLockCriticalGuard critical;
#ifdef KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS
	if (g_host_network_query_callback != nullptr) {
		g_host_network_query_callback();
	}
#endif
	HostNetworkInfo info;
#if defined(_WIN32)
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS;
	ULONG size  = 0;
	auto  ret   = GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
	if (ret != ERROR_BUFFER_OVERFLOW || size == 0) {
		return info;
	}

	std::vector<uint8_t> buffer(size);
	auto*                adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
	ret = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
	if (ret != NO_ERROR) {
		return info;
	}

	for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
		if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
		    adapter->IfType == IF_TYPE_TUNNEL) {
			continue;
		}

		for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
		     unicast       = unicast->Next) {
			const auto* addr = unicast->Address.lpSockaddr;
			if (addr == nullptr) {
				continue;
			}
			if (addr->sa_family == AF_INET) {
				const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
				const auto  ip = ntohl(in->sin_addr.S_un.S_addr);
				if ((ip & 0xff000000u) != 0x7f000000u && ip != 0) {
					info.connected = true;
					info.wireless  = (adapter->IfType == IF_TYPE_IEEE80211);
					if (adapter->PhysicalAddressLength >= sizeof(info.ether_addr.data)) {
						std::memcpy(info.ether_addr.data, adapter->PhysicalAddress,
						            sizeof(info.ether_addr.data));
					}
					CopyIpv4String(info.ip_address, sizeof(info.ip_address), addr);
					CopyIpv4Netmask(info.netmask, sizeof(info.netmask),
					                unicast->OnLinkPrefixLength);
					if (adapter->FirstGatewayAddress != nullptr) {
						CopyIpv4String(info.default_route, sizeof(info.default_route),
						               adapter->FirstGatewayAddress->Address.lpSockaddr);
					}
					int dns_index = 0;
					for (auto* dns = adapter->FirstDnsServerAddress; dns != nullptr;
					     dns       = dns->Next) {
						if (dns->Address.lpSockaddr != nullptr &&
						    dns->Address.lpSockaddr->sa_family == AF_INET) {
							CopyIpv4String(dns_index == 0 ? info.primary_dns : info.secondary_dns,
							               dns_index == 0 ? sizeof(info.primary_dns)
							                              : sizeof(info.secondary_dns),
							               dns->Address.lpSockaddr);
							dns_index++;
							if (dns_index >= 2) {
								break;
							}
						}
					}
					return info;
				}
			}
		}
	}

	return info;
#else
	ifaddrs* addresses = nullptr;
	if (getifaddrs(&addresses) != 0) {
		return info;
	}
	const auto usable = [](const ifaddrs* address) {
		return address->ifa_addr != nullptr && address->ifa_addr->sa_family == AF_INET &&
		       (address->ifa_flags & (IFF_UP | IFF_RUNNING | IFF_LOOPBACK)) ==
		           (IFF_UP | IFF_RUNNING) &&
		       reinterpret_cast<const sockaddr_in*>(address->ifa_addr)->sin_addr.s_addr != 0;
	};
	const ifaddrs* selected = nullptr;
	for (auto* address = addresses; address != nullptr; address = address->ifa_next) {
		if (usable(address)) {
			selected = address;
			break;
		}
	}
	std::ifstream routes("/proc/net/route");
	std::string line;
	uint32_t best_metric = std::numeric_limits<uint32_t>::max();
	uint32_t gateway = 0;
	bool has_default_route = false;
	while (std::getline(routes, line)) {
		std::istringstream fields(line);
		std::string name;
		uint32_t destination = 0, candidate_gateway = 0, flags = 0, refs = 0, uses = 0;
		uint32_t metric = 0, mask = 0;
		if (!(fields >> name >> std::hex >> destination >> candidate_gateway >> flags >>
		      std::dec >> refs >> uses >> metric >> std::hex >> mask) ||
		    destination != 0 || mask != 0 || (flags & (RTF_UP | RTF_REJECT)) != RTF_UP ||
		    (has_default_route && metric >= best_metric)) {
			continue;
		}
		for (auto* address = addresses; address != nullptr; address = address->ifa_next) {
			if (usable(address) && name == address->ifa_name) {
				selected = address;
				gateway = candidate_gateway;
				best_metric = metric;
				has_default_route = true;
				break;
			}
		}
	}
	if (selected != nullptr) {
		CopyIpv4String(info.ip_address, sizeof(info.ip_address), selected->ifa_addr);
		CopyIpv4String(info.netmask, sizeof(info.netmask), selected->ifa_netmask);
		info.connected = info.ip_address[0] != '\0';
		if (has_default_route) {
			in_addr address {gateway};
			inet_ntop(AF_INET, &address, info.default_route, sizeof(info.default_route));
		}
	}
	freeifaddrs(addresses);
	return info;
#endif
}

#ifdef KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS
void TestQueryHostNetworkInfoWithCallback(void (*callback)()) {
	g_host_network_query_callback = callback;
	(void)QueryHostNetworkInfo();
	g_host_network_query_callback = nullptr;
}
#endif

static bool NetCtlConnected() {
	const bool connected = HostHttp::Enabled() && QueryHostNetworkInfo().connected;
	if (!g_net_ctl_status_initialized.exchange(true) || g_net_ctl_connected.exchange(connected) != connected) {
		g_net_ctl_connected = connected;
		LOGF("\t host network connected = %s\n", connected ? "true" : "false");
	}
	return connected;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlInit() {
	PRINT_NAME();

	HostHttp::SetConfiguredEnabled(Config::IsNetworkConnected());
	g_net_ctl_status_initialized = false;
	(void)NetCtlConnected();

	return OK;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlTerm() {
	PRINT_NAME();

	Common::LockGuard lock(g_net_ctl_callbacks_mutex);

	for (auto& cb: g_net_ctl_callbacks) {
		cb = {};
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlGetNatInfo(NetCtlNatInfo* nat_info) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(nat_info == nullptr);
	EXIT_NOT_IMPLEMENTED(nat_info->size != sizeof(NetCtlNatInfo));

	nat_info->stunStatus        = 0;
	nat_info->natType           = 0;
	nat_info->mappedAddr.s_addr = 0;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlCheckCallback() {
	PRINT_NAME();

	Common::LockGuard lock(g_net_ctl_callbacks_mutex);
	const int event = (NetCtlConnected() ? NET_CTL_EVENT_IPOBTAINED : NET_CTL_EVENT_DISCONNECTED);
	for (auto& cb: g_net_ctl_callbacks) {
		if (cb.func != nullptr && cb.last_event != event) {
			cb.func(event, cb.arg);
			cb.last_event = event;
		}
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlGetState(int* state) {
	PRINT_NAME();

	if (state == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}

	*state = (NetCtlConnected() ? NET_CTL_STATE_IPOBTAINED : NET_CTL_STATE_DISCONNECTED);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlGetStateV6(int* state) {
	PRINT_NAME();

	if (state == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}

	*state = NET_CTL_STATE_DISCONNECTED;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlRegisterCallback(NetCtlCallback func, void* arg, int* cid) {
	PRINT_NAME();

	if (func == nullptr || cid == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}

	Common::LockGuard lock(g_net_ctl_callbacks_mutex);

	for (int i = 0; i < NET_CTL_CALLBACK_MAX; i++) {
		if (g_net_ctl_callbacks[static_cast<size_t>(i)].func == nullptr) {
			g_net_ctl_callbacks[static_cast<size_t>(i)].func       = func;
			g_net_ctl_callbacks[static_cast<size_t>(i)].arg        = arg;
			g_net_ctl_callbacks[static_cast<size_t>(i)].last_event = 0;
			*cid                                                   = i;
			return OK;
		}
	}

	return NET_CTL_ERROR_CALLBACK_MAX;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlUnregisterCallback(int cid) {
	PRINT_NAME();

	if (cid < 0 || cid >= NET_CTL_CALLBACK_MAX) {
		return NET_CTL_ERROR_INVALID_ID;
	}

	Common::LockGuard lock(g_net_ctl_callbacks_mutex);

	auto& cb = g_net_ctl_callbacks[static_cast<size_t>(cid)];

	if (cb.func == nullptr) {
		return NET_CTL_ERROR_ID_NOT_FOUND;
	}

	cb = {};

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlGetResult(int event_type, int* error_code) {
	PRINT_NAME();

	if (error_code == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}

	LOGF("\t event_type = %d\n", event_type);

	switch (event_type) {
		case NET_CTL_EVENT_DISCONNECTED:
		case 2:
		case NET_CTL_EVENT_IPOBTAINED: *error_code = OK; break;
		default: return NET_CTL_ERROR_INVALID_TYPE;
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetCtlGetInfo(int code, NetCtlInfo* info) {
	PRINT_NAME();

	if (info == nullptr) {
		return NET_CTL_ERROR_INVALID_ADDR;
	}
	if (code < 1 || code > 21) {
		return NET_CTL_ERROR_INVALID_CODE;
	}
	if (!HostHttp::Enabled()) {
		return NET_CTL_ERROR_NOT_CONNECTED;
	}
	const auto host = QueryHostNetworkInfo();
	if (!host.connected || (code == 16 && host.default_route[0] == '\0')) {
		return NET_CTL_ERROR_NOT_CONNECTED;
	}
	memset(info, 0, sizeof(NetCtlInfo));

	if (!NetCtlConnected()) return NET_CTL_ERROR_NOT_CONNECTED;

	switch (code) {
		case 1:
			info->device = (host.wireless ? NET_CTL_DEVICE_WIRELESS : NET_CTL_DEVICE_WIRED);
			break;
		case 2: info->ether_addr = host.ether_addr; break;
		case 3: info->mtu = 1500; break;
		case 4: info->link = NET_CTL_LINK_CONNECTED; break;
		case 5: break;
		case 6: break;
		case 7: info->wifi_security = 0; break;
		case 8: info->rssi_dbm = 0; break;
		case 9: info->rssi_percentage = 0; break;
		case 10: info->channel = 0; break;
		case 11: info->ip_config = NET_CTL_IP_DHCP; break;
		case 12: break;
		case 13: break;
		case 14:
			std::strncpy(info->ip_address, host.ip_address,
			             sizeof(info->ip_address) - 1);
			break;
		case 15:
			std::strncpy(info->netmask, host.netmask, sizeof(info->netmask) - 1);
			break;
		case 16:
			std::strncpy(info->default_route, host.default_route,
			             sizeof(info->default_route) - 1);
			break;
		case 17:
			std::strncpy(info->primary_dns, host.primary_dns, sizeof(info->primary_dns) - 1);
			break;
		case 18:
			std::strncpy(info->secondary_dns, host.secondary_dns, sizeof(info->secondary_dns) - 1);
			break;
		case 19: info->http_proxy_config = NET_CTL_HTTP_PROXY_OFF; break;
		case 20: break;
		case 21: info->http_proxy_port = 0; break;
	}

	return OK;
}

} // namespace NetCtl

namespace NpManager {

LIB_NAME("NpManager", "NpManager");

struct NpTitleId {
	char    id[12 + 1];
	uint8_t padding[3];
};

struct NpTitleSecret {
	uint8_t data[128];
};

struct NpCountryCode {
	char data[2];
	char term;
	char padding[1];
};

struct NpLanguageCode {
	char code[36];
	char padding[12];
};

struct NpAgeRestriction {
	NpCountryCode country_code;
	int8_t        age;
	uint8_t       padding[3];
};

struct NpContentRestriction {
	size_t                  size;
	int8_t                  default_age_restriction;
	char                    padding[3];
	int32_t                 age_restriction_count;
	const NpAgeRestriction* age_restriction;
};

struct NpOnlineId {
	char data[16];
	char term;
	char dummy[3];
};

struct NpId {
	NpOnlineId handle;
	uint8_t    opt[8];
	uint8_t    reserved[8];
};

struct NpCreateAsyncRequestParameter {
	size_t                   size;
	LibKernel::KernelCpumask cpu_affinity_mask;
	int                      thread_priority;
	uint8_t                  padding[4];
};

struct NpCheckPremiumParameter {
	size_t   size;
	int      user_id;
	char     padding[4];
	uint64_t features;
	uint8_t  reserved[32];
};

struct NpCheckPremiumResult {
	bool    authorized;
	uint8_t reserved[32];
};

constexpr int      np_error_invalid_argument            = -2141913085; /* 0x80550003 */
constexpr int      np_error_signed_out                  = -2141913082; /* 0x80550006 */
constexpr int      np_error_user_not_found              = -2141913081; /* 0x80550007 */
constexpr int      np_error_callback_already_registered = -2141913080; /* 0x80550008 */
constexpr int      np_error_invalid_size                = -2141913071; /* 0x80550011 */
constexpr int      np_error_aborted                     = -2141913070; /* 0x80550012 */
constexpr int      np_error_request_max                 = -2141913069; /* 0x80550013 */
constexpr int      np_error_request_not_found           = -2141913068; /* 0x80550014 */
constexpr int      np_error_invalid_id                  = -2141913067; /* 0x80550015 */
constexpr int      np_error_network_not_available       = -2141872086; /* 0x8055a02a */
constexpr int      np_request_max                       = 128;
constexpr uint32_t np_state_signed_out                  = 1;
constexpr uint32_t np_state_signed_in                   = 2;
constexpr uint64_t np_account_id                        = 0x00000000feedfaceull;

// A simulated account belongs only to the primary user and survives network loss.
bool IsSignedIn(int user_id) {
	return Config::PsnEmulationEnabled() && user_id == Config::GetUserId();
}

static int np_reachability_result(int user_id) {
	if (!IsSignedIn(user_id)) {
		return np_error_signed_out;
	}
	// This is an explicit compatibility simulation, not a connection to PSN.
	return NetCtl::NetCtlConnected() ? OK : np_error_network_not_available;
}

struct NpStateCallback {
	NpStateCallbackA callback = nullptr;
	void*            userdata = nullptr;
	bool             pending  = false;
};

static std::vector<NpStateCallback> g_np_state_callbacks;

enum class NpRequestState {
	Free,
	Ready,
	Aborted,
	Complete,
};

struct NpRequest {
	NpRequestState state  = NpRequestState::Free;
	bool           async  = false;
	int            result = OK;
};

static std::mutex             g_np_request_mutex;
static std::vector<NpRequest> g_np_requests;

static int np_create_request(bool async) {
	std::lock_guard lock(g_np_request_mutex);

	for (size_t i = 0; i < g_np_requests.size(); i++) {
		auto& request = g_np_requests[i];
		if (request.state == NpRequestState::Free) {
			request.state  = NpRequestState::Ready;
			request.async  = async;
			request.result = OK;
			return static_cast<int>(i + 1);
		}
	}

	if (g_np_requests.size() >= np_request_max) {
		return np_error_request_max;
	}

	g_np_requests.push_back({NpRequestState::Ready, async, OK});
	return static_cast<int>(g_np_requests.size());
}

static NpRequest* np_get_request_locked(int req_id) {
	if (req_id <= 0) {
		return nullptr;
	}

	const auto index = static_cast<size_t>(req_id - 1);
	if (index >= g_np_requests.size() || g_np_requests[index].state == NpRequestState::Free) {
		return nullptr;
	}

	return &g_np_requests[index];
}

static int np_complete_locked(NpRequest* request, int result) {
	if (request->state == NpRequestState::Complete) {
		request->result = np_error_invalid_argument;
		return np_error_invalid_argument;
	}
	if (request->state == NpRequestState::Aborted) {
		request->result = np_error_aborted;
		return np_error_aborted;
	}

	request->state  = NpRequestState::Complete;
	request->result = result;

	return (request->async ? OK : result);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpCheckCallback() {
	PRINT_NAME();

	std::vector<NpStateCallback> pending;
	for (auto& entry: g_np_state_callbacks) {
		if (entry.pending) {
			pending.push_back(entry);
			entry.pending = false;
		}
	}
	for (const auto& entry: pending) {
		entry.callback(Config::GetUserId(),
		               IsSignedIn(Config::GetUserId()) ? np_state_signed_in : np_state_signed_out,
		               entry.userdata);
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpSetNpTitleId(const NpTitleId* title_id, const NpTitleSecret* title_secret) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(title_id == nullptr);
	EXIT_NOT_IMPLEMENTED(title_secret == nullptr);

	LOGF("\t title_id = %.12s (title secret omitted)\n", title_id->id);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpSetContentRestriction(const NpContentRestriction* restriction) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(restriction == nullptr);
	EXIT_NOT_IMPLEMENTED(restriction->size != sizeof(NpContentRestriction));

	LOGF("\t default_age_restriction = %" PRIi8 "\n"
	     "\t age_restriction_count   = %" PRIi32 "\n",
	     restriction->default_age_restriction, restriction->age_restriction_count);

	for (int i = 0; i < restriction->age_restriction_count; i++) {
		LOGF("\t age_restriction[%d].age = %" PRIi8 "\n"
		     "\t age_restriction[%d].country_code.data = %.2s\n",
		     i, restriction->age_restriction[i].age, i,
		     restriction->age_restriction[i].country_code.data);
	}

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpRegisterStateCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpUnregisterStateCallback() {
	PRINT_NAME();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpRegisterStateCallbackA(NpStateCallbackA callback, void* userdata) {
	PRINT_NAME();

	if (callback == nullptr) {
		return np_error_invalid_argument;
	}
	for (const auto& entry: g_np_state_callbacks) {
		if (entry.callback == callback) {
			return np_error_callback_already_registered;
		}
	}
	g_np_state_callbacks.push_back({callback, userdata, true});
	return static_cast<int>(g_np_state_callbacks.size());
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpRegisterGamePresenceCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpRegisterPlusEventCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpRegisterPremiumEventCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpRegisterNpReachabilityStateCallback(void* /*callback*/, void* /*userdata*/) {
	PRINT_NAME();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetNpId(int user_id, NpId* np_id) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(np_id == nullptr);

	std::memset(np_id, 0, sizeof(*np_id));

	if (IsSignedIn(user_id)) {
		std::memcpy(np_id->handle.data, "Kyty", 4);
		return OK;
	}

	return np_error_signed_out;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetOnlineId(int user_id, NpOnlineId* online_id) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(online_id == nullptr);

	std::memset(online_id, 0, sizeof(*online_id));

	if (IsSignedIn(user_id)) {
		std::memcpy(online_id->data, "Kyty", 4);
		return OK;
	}

	return np_error_signed_out;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetAccountIdA(int user_id, uint64_t* account_id) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(account_id == nullptr);

	*account_id = 0;

	if (IsSignedIn(user_id)) {
		*account_id = np_account_id;
		return OK;
	}

	return np_error_signed_out;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetUserIdByAccountId(uint64_t account_id, int* user_id) {
	PRINT_NAME();

	LOGF("\t account_id = 0x%016" PRIx64 "\n", account_id);

	if (account_id == 0 || user_id == nullptr) {
		return np_error_invalid_argument;
	}
	if (!Config::PsnEmulationEnabled()) {
		return np_error_signed_out;
	}
	if (account_id != np_account_id) {
		return np_error_user_not_found;
	}

	*user_id = Config::GetUserId();
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetAccountCountryA(int user_id, void* country_code) {
	PRINT_NAME();

	LOGF("\t user_id = %d\n", user_id);

	EXIT_NOT_IMPLEMENTED(country_code == nullptr);

	auto* code = static_cast<NpCountryCode*>(country_code);
	std::memset(code, 0, sizeof(*code));

	if (IsSignedIn(user_id)) {
		std::memcpy(code->data, "US", 2);
		return OK;
	}

	return np_error_signed_out;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetAccountAge(int req_id, int user_id, uint8_t* age) {
	PRINT_NAME();

	LOGF("\t req_id  = %d\n", req_id);
	LOGF("\t user_id = %d\n", user_id);
	LOGF("\t age     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(age));

	if (req_id <= 0 || age == nullptr) {
		return np_error_invalid_argument;
	}

	std::lock_guard lock(g_np_request_mutex);
	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	*age = IsSignedIn(user_id) ? 21 : 0;
	return np_complete_locked(request, IsSignedIn(user_id) ? OK : np_error_signed_out);
}

int KYTY_SYSV_ABI NpGetAccountLanguage2(int req_id, int user_id, void* language_code) {
	PRINT_NAME();

	LOGF("\t req_id        = %d\n", req_id);
	LOGF("\t user_id       = %d\n", user_id);
	LOGF("\t language_code = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(language_code));

	if (req_id <= 0 || language_code == nullptr) {
		return np_error_invalid_argument;
	}

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	auto* code = static_cast<NpLanguageCode*>(language_code);
	std::memset(code, 0, sizeof(*code));
	if (IsSignedIn(user_id)) {
		std::memcpy(code->code, "en-US", 5);
	}
	return np_complete_locked(request, IsSignedIn(user_id) ? OK : np_error_signed_out);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpCreateRequest() {
	PRINT_NAME();

	const auto req_id = np_create_request(false);

	LOGF("\t req_id = %d\n", req_id);

	return req_id;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpCreateAsyncRequest(const NpCreateAsyncRequestParameter* param) {
	PRINT_NAME();

	if (param == nullptr) {
		return np_error_invalid_argument;
	}

	if (param->size < sizeof(NpCreateAsyncRequestParameter)) {
		return np_error_invalid_size;
	}

	LOGF("\t size              = %" PRIu64 "\n"
	     "\t cpu_affinity_mask = %" PRIu64 "\n"
	     "\t thread_priority   = %d\n",
	     param->size, param->cpu_affinity_mask, param->thread_priority);

	const auto req_id = np_create_request(true);

	LOGF("\t req_id            = %d\n", req_id);

	return req_id;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpDeleteRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	request->state  = NpRequestState::Free;
	request->async  = false;
	request->result = OK;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAbortRequest(int req_id) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	request->state  = NpRequestState::Aborted;
	request->result = np_error_aborted;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpCheckNpAvailability(int req_id, const char* user, void* result) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(req_id <= 0);
	// EXIT_NOT_IMPLEMENTED(user == nullptr);
	// EXIT_NOT_IMPLEMENTED(result != nullptr);

	LOGF("\t req_id = %d\n"
	     "\t user   = %s\n",
	     req_id, user != nullptr ? user : "(null)");

	if (req_id <= 0 || user == nullptr) {
		return np_error_invalid_argument;
	}

	if (result != nullptr) {
		std::memset(result, 0, sizeof(int));
	}

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	return np_complete_locked(request, std::strcmp(user, "Kyty") == 0
	                                       ? np_reachability_result(Config::GetUserId())
	                                       : np_error_signed_out);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpCheckNpReachability(int req_id, int user_id) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(req_id <= 0);

	LOGF("\t req_id  = %d\n", req_id);
	LOGF("\t user_id = %d\n", user_id);

	if (req_id <= 0) {
		return np_error_invalid_argument;
	}

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	return np_complete_locked(request, np_reachability_result(user_id));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpPollAsync(int req_id, int* result) {
	PRINT_NAME();

	if (result == nullptr) {
		return np_error_invalid_argument;
	}

	LOGF("\t req_id = %d\n", req_id);

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	if (!request->async || request->state == NpRequestState::Ready) {
		return np_error_invalid_id;
	}

	*result = request->result;

	// return request->state == NpRequestState::Aborted ? np_error_aborted : OK;
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpCheckPremium(int req_id, const NpCheckPremiumParameter* param,
                                 NpCheckPremiumResult* result) {
	PRINT_NAME();

	LOGF("\t req_id = %d\n", req_id);
	LOGF("\t param  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
	LOGF("\t result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(result));

	if (param == nullptr || result == nullptr) {
		return np_error_invalid_argument;
	}

	if (param->size < sizeof(NpCheckPremiumParameter)) {
		return np_error_invalid_size;
	}

	LOGF("\t size    = %" PRIu64 "\n"
	     "\t user_id = %d\n"
	     "\t features = 0x%016" PRIx64 "\n",
	     param->size, param->user_id, param->features);

	std::lock_guard lock(g_np_request_mutex);

	auto* request = np_get_request_locked(req_id);
	if (request == nullptr) {
		return np_error_request_not_found;
	}

	std::memset(result, 0, sizeof(*result));

	// The simulated account has no premium eligibility.
	return np_complete_locked(request, np_reachability_result(param->user_id));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetState(int user_id, uint32_t* state) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(state == nullptr);

	LOGF("\t user_id = %d\n", user_id);

	*state = IsSignedIn(user_id) ? np_state_signed_in : np_state_signed_out;

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetNpReachabilityState(int user_id, uint32_t* state) {
	PRINT_NAME();

	if (state == nullptr) {
		return np_error_invalid_argument;
	}

	LOGF("\t user_id = %d\n", user_id);

	*state = np_reachability_result(user_id) == OK ? 2 : 0; // REACHABLE / UNAVAILABLE

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpHasSignedUp(int user_id, bool* has_signed_up) {
	PRINT_NAME();

	if (has_signed_up == nullptr) {
		return np_error_invalid_argument;
	}

	LOGF("\t user_id = %d\n", user_id);

	*has_signed_up = IsSignedIn(user_id);

	return OK;
}

} // namespace NpManager

} // namespace Libs::Network
