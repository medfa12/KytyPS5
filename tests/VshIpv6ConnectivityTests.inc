// Include after NetPoolConfigTests.inc. Run in a dedicated process after
// InitSubsystems(), before InitAll(). Real IPv6 loopback, no external services.
namespace VshIpv6ConnectivityTest {
namespace Net = Libs::Network::Net;
namespace Network = Libs::Network;
constexpr const char* Name = "VshIpv6Connectivity";
using Address = std::array<uint8_t, 28>;

struct HostMode {
	bool present;
	std::string previous;
	static void Set(const char* value) {
#ifdef _WIN32
		_putenv_s("KYTY_NETWORK_HOST", value ? value : "");
#else
		if (value) { setenv("KYTY_NETWORK_HOST", value, 1); }
		else { unsetenv("KYTY_NETWORK_HOST"); }
#endif
	}
	HostMode() {
		const auto* value = std::getenv("KYTY_NETWORK_HOST");
		present = value != nullptr;
		if (value) { previous = value; }
		Set("1");
	}
	~HostMode() { Set(present ? previous.c_str() : nullptr); }
};

struct Api: NetPoolConfigTest::Api {
	using Socket = int (KYTY_SYSV_ABI *)(const char*, int, int, int);
	using SendTo = int (KYTY_SYSV_ABI *)(int, const void*, size_t, int, const void*, uint32_t);
	using RecvFrom = int (KYTY_SYSV_ABI *)(int, void*, size_t, int, void*, uint32_t*);
	Socket socket = Resolve<Socket>("Q4qBuN-c0ZM", "Net", "LibNet::NetSocket");
	decltype(&Net::SocketClose) close = Resolve<decltype(close)>("45ggEzakPJQ", "Net", "LibNet::NetSocketClose");
	decltype(&Net::Bind) bind = Resolve<decltype(bind)>("bErx49PgxyY", "Net", "LibNet::NetBind");
	decltype(&Net::Connect) connect = Resolve<decltype(connect)>("OXXX4mUk3uk", "Net", "LibNet::NetConnect");
	decltype(&Net::Listen) listen = Resolve<decltype(listen)>("kOj1HiAGE54", "Net", "LibNet::NetListen");
	decltype(&Net::Accept) accept = Resolve<decltype(accept)>("PIWqhn9oSxc", "Net", "LibNet::NetAccept");
	decltype(&Net::Getsockname) name = Resolve<decltype(name)>("hoOAofhhRvE", "Net", "LibNet::NetGetsockname");
	decltype(&Net::Getsockopt) get = Resolve<decltype(get)>("xphrZusl78E", "Net", "LibNet::NetGetsockopt");
	decltype(&Net::Setsockopt) set = Resolve<decltype(set)>("2mKX2Spso7I", "Net", "LibNet::NetSetsockopt");
	SendTo send = Resolve<SendTo>("gvD1greCu0A", "Net", "LibNet::NetSendto");
	RecvFrom recv = Resolve<RecvFrom>("304ooNZxWDY", "Net", "LibNet::NetRecvfrom");
};

struct Socket {
	Api& api;
	int fd;
	Socket(Api& a, int type): api(a), fd(a.socket("vsh-ipv6-loopback", 28, type, 0)) {
		Check(Name, fd >= 0 && Net::IsSocket(fd), "IPv6 requires a real host socket");
	}
	~Socket() { if (fd >= 0) { api.close(fd); } }
	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;
};

void Nonblocking(Api& api, int fd) {
	const int one = 1;
	CheckOk(Name, api.set(fd, 0xffff, 0x1200, &one, sizeof(one)), "nonblocking socket");
}

Address Bound(Api& api, int fd, Address address) {
	CheckOk(Name, api.bind(fd, address.data(), address.size()), "bind ::1");
	uint32_t size = address.size();
	CheckOk(Name, api.name(fd, address.data(), &size), "local IPv6 address");
	Check(Name, size == 28 && address[0] == 28 && address[1] == 28 &&
	            (address[2] != 0 || address[3] != 0) && address[23] == 1,
	      "guest IPv6 layout or ephemeral port incorrect");
	return address;
}

template<class F> int Retry(F call) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	int result;
	do {
		result = call();
		if (result != Network::NET_ERROR_EWOULDBLOCK) { return result; }
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < deadline);
	return result;
}

void Datagram(Api& api, const Address& loopback) {
	Socket server(api, 2), client(api, 2);
	Nonblocking(api, server.fd);
	Nonblocking(api, client.fd);
	for (uint32_t size: {0u, 1u, 2u, 8u, 27u}) {
		Check(Name, api.bind(server.fd, loopback.data(), size) == Network::NET_ERROR_EINVAL &&
		            *api.error_addr() == Libs::Posix::POSIX_EINVAL, "short IPv6 input accepted");
	}
	Check(Name, api.bind(server.fd, nullptr, 28) == Network::NET_ERROR_EFAULT,
	      "null IPv6 input accepted");
	const int one = 1;
	CheckOk(Name, api.set(server.fd, 41, 27, &one, sizeof(one)), "guest IPV6_V6ONLY translation");
	int value = -1;
	uint32_t size = sizeof(value);
	CheckOk(Name, api.get(server.fd, 41, 27, &value, &size), "read IPV6_V6ONLY");
	Check(Name, value == 1 && size == sizeof(value), "IPv6 option readback value=" +
	      std::to_string(value) + " size=" + std::to_string(size));
	for (int enabled: {0, 1}) {
		CheckOk(Name, api.set(server.fd, 41, 27, &enabled, sizeof(enabled)), "change IPV6_V6ONLY");
		std::array<uint8_t, 6> option;
		option.fill(0xa5);
		size = sizeof(int);
		CheckOk(Name, api.get(server.fd, 41, 27, option.data() + 1, &size), "integer IPv6 option copyout");
		std::memcpy(&value, option.data() + 1, sizeof(value));
		Check(Name, value == enabled && size == sizeof(int) && option.front() == 0xa5 && option.back() == 0xa5,
		      "IPv6 boolean must replace all four guest bytes and preserve canaries");
	}
	Check(Name, api.set(server.fd, 41, 0x7fff, &one, sizeof(one)) == Network::NET_ERROR_ENOPROTOOPT &&
	            *api.error_addr() == Libs::Posix::POSIX_ENOPROTOOPT, "unknown IPv6 option passed through");
	const auto destination = Bound(api, server.fd, loopback);
	const auto source = Bound(api, client.fd, loopback);
	for (uint32_t capacity: {0u, 1u, 7u, 27u, 28u, 32u}) {
		std::array<uint8_t, 34> output;
		output.fill(0xa5);
		size = capacity;
		CheckOk(Name, api.name(server.fd, output.data() + 1, &size), "short getsockname copyout");
		const auto copied = std::min(capacity, 28u);
		Check(Name, size == 28 && output[0] == 0xa5 &&
		            std::equal(destination.begin(), destination.begin() + copied, output.begin() + 1) &&
		            std::all_of(output.begin() + 1 + copied, output.end(), [](uint8_t b) { return b == 0xa5; }),
		      "IPv6 copyout corrupted canaries or failed to report full size");
	}
	const std::array<uint8_t, 5> payload {0x56, 0x36, 0x00, 0xfe, 0x71};
	std::array<uint8_t, 5> received {};
	Check(Name, api.recv(server.fd, received.data(), received.size(), 0, nullptr, nullptr) ==
	            Network::NET_ERROR_EWOULDBLOCK, "empty UDP socket reported fabricated data");
	for (uint32_t capacity: {3u, 28u}) {
		Check(Name, api.send(client.fd, payload.data(), payload.size(), 0, destination.data(), 28) == 5,
		      "real IPv6 datagram send");
		std::array<uint8_t, 30> from;
		from.fill(0xa5);
		size = capacity;
		const int count = Retry([&] { return api.recv(server.fd, received.data(), received.size(), 0,
		                                           from.data() + 1, &size); });
		Check(Name, count == 5 && received == payload && size == 28 && from[0] == 0xa5 &&
		            std::equal(source.begin(), source.begin() + capacity, from.begin() + 1) &&
		            std::all_of(from.begin() + 1 + capacity, from.end(), [](uint8_t b) { return b == 0xa5; }),
		      "UDP payload/source address lost or truncated copyout consumed packet as error");
	}
}

void Stream(Api& api, const Address& loopback) {
	Socket server(api, 1), client(api, 1);
	Nonblocking(api, server.fd);
	Nonblocking(api, client.fd);
	const auto destination = Bound(api, server.fd, loopback);
	CheckOk(Name, api.listen(server.fd, 1), "IPv6 listen");
	const int connected = api.connect(client.fd, destination.data(), destination.size());
	Check(Name, connected == 0 || (connected == Network::NET_ERROR_EINPROGRESS &&
	            *api.error_addr() == Libs::Posix::POSIX_EINPROGRESS), "IPv6 connect pending errno");
	Address remote {};
	uint32_t size = remote.size();
	const int accepted = Retry([&] { return api.accept(server.fd, remote.data(), &size); });
	struct Accepted { Api& api; int fd; ~Accepted() { if (fd >= 0) { api.close(fd); } } } owner {api, accepted};
	Check(Name, accepted >= 0 && size == 28 && remote[1] == 28 && remote[23] == 1,
	      "real IPv6 TCP accept/address");
	Nonblocking(api, accepted);
	int error = -1;
	size = sizeof(error);
	CheckOk(Name, api.get(client.fd, 0xffff, 0x1007, &error, &size), "IPv6 SO_ERROR");
	Check(Name, error == 0, "IPv6 connect did not complete");
	Address peer {};
	size = peer.size();
	CheckOk(Name, Net::Getpeername(client.fd, peer.data(), &size), "IPv6 POSIX peer address");
	Check(Name, size == 28 && peer == destination, "peer address port/order mismatch");
	const uint8_t sent = 0xc7;
	uint8_t received = 0;
	Check(Name, api.send(client.fd, &sent, 1, 0, nullptr, 0) == 1, "IPv6 TCP send");
	Check(Name, Retry([&] { return api.recv(accepted, &received, 1, 0, nullptr, nullptr); }) == 1 &&
	            received == sent, "IPv6 TCP bytes did not reach the accepted endpoint");
	CheckOk(Name, api.close(client.fd), "close IPv6 TCP client");
	client.fd = -1;
	Check(Name, Retry([&] { return api.recv(accepted, &received, 1, 0, nullptr, nullptr); }) == 0,
	      "TCP peer close did not produce real EOF");
}
} // namespace VshIpv6ConnectivityTest

void TestVshIpv6Connectivity() {
	using namespace VshIpv6ConnectivityTest;
	HostMode mode;
	NetPoolConfigTest::Instance instance;
	Api api;
	CheckOk(Name, api.init(), "network initialization");
	const int pool = api.create("ipv6-connectivity", 0x4000, 0);
	Check(Name, pool >= 0, "resolver pool");
	const int resolver = api.resolver_create("ipv6-connectivity", pool, 0);
	Check(Name, resolver >= 0, "IPv6 resolver");
	Address loopback {28, 28};
	const auto resolve = api.Resolve<decltype(&Net::NetResolverStartNtoa6)>(
	    "zl35YNs9jnI", "Net", "LibNet::NetResolverStartNtoa6");
	CheckOk(Name, resolve(resolver, "::1", loopback.data() + 8, 0, 0, 0), "resolve IPv6 literal");
	Check(Name, loopback[23] == 1, "resolver did not yield IPv6 loopback");
	CheckOk(Name, api.resolver_destroy(resolver), "resolver cleanup");
	CheckOk(Name, api.destroy(pool), "pool cleanup");
	Datagram(api, loopback);
	Stream(api, loopback);
}
