// Dedicated --net-pool-config-only mode, after InitSubsystems and before InitAll.
// Parent includes libs/network.h, libs/errno.h, loader/symbolDatabase.h and
// declares namespace Libs { void InitNet_1(Loader::SymbolDatabase*); } globally.
namespace NetPoolConfigTest {
namespace Network = Libs::Network;
namespace Net = Network::Net;
constexpr const char* Name = "NetPoolConfig";

struct Instance {
	Instance() { Network::Initialize(); }
	~Instance() { Network::Shutdown(); }
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;
};

struct Api {
	using ErrorAddr = int* (KYTY_SYSV_ABI *)();
	Loader::SymbolDatabase symbols;
	ErrorAddr error_addr;
	decltype(&Net::NetSetInitParam) configure;
	decltype(&Net::NetInit) init;
	decltype(&Net::NetPoolCreate) create;
	decltype(&Net::NetPoolDestroy) destroy;
	decltype(&Net::NetResolverCreate) resolver_create;
	decltype(&Net::NetResolverDestroy) resolver_destroy;
	decltype(&Network::Ssl::SslInit) ssl_init;
	decltype(&Network::Ssl::SslTerm) ssl_term;
	decltype(&Network::Http::HttpInit) http_init;
	decltype(&Network::Http::HttpTerm) http_term;

	template<class F> F Resolve(const char* nid, const char* library, const char* implementation) {
		const auto* exact = symbols.Find({nid, library, 1, library, 1, 1, Loader::SymbolType::Func});
		const auto* fallback = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(Name, exact != nullptr && fallback != nullptr && exact->vaddr != 0 &&
		            exact->vaddr == fallback->vaddr && exact->dbg_name == implementation,
		      std::string("exact/NID binding: ") + nid);
		return reinterpret_cast<F>(exact->vaddr);
	}
	Api() {
		Libs::InitNet_1(&symbols); // Production orchestrator; calls below use the registered SysV ABI.
		error_addr = Resolve<ErrorAddr>("HQOwnfMGipQ", "Net", "LibNet::GetNetErrorAddr");
		configure = Resolve<decltype(configure)>("6MojQ8uFHEI", "Net", "LibNet::SceNetInitParam");
		init = Resolve<decltype(init)>("Nlev7Lg8k3A", "Net", "LibNet::NetInit");
		create = Resolve<decltype(create)>("dgJBaeJnGpo", "Net", "LibNet::NetPoolCreate");
		destroy = Resolve<decltype(destroy)>("K7RlrTkI-mw", "Net", "LibNet::NetPoolDestroy");
		resolver_create = Resolve<decltype(resolver_create)>("C4UgDHHPvdw", "Net", "LibNet::NetResolverCreate");
		resolver_destroy = Resolve<decltype(resolver_destroy)>("kJlYH5uMAWI", "Net", "LibNet::NetResolverDestroy");
		ssl_init = Resolve<decltype(ssl_init)>("hdpVEUDFW3s", "Ssl", "Ssl::SslInit");
		ssl_term = Resolve<decltype(ssl_term)>("0K1yQ6Lv-Yc", "Ssl", "Ssl::SslTerm");
		http_init = Resolve<decltype(http_init)>("A9cVMUtEp4Y", "Http", "Http::HttpInit");
		http_term = Resolve<decltype(http_term)>("Ik-KpLTlf7Q", "Http", "Http::HttpTerm");
	}
};

struct Input {
	uint64_t before = 0x123456789abcdef0ull;
	std::array<uint8_t, 32> bytes {};
	uint64_t after = 0xfedcba9876543210ull;
	explicit Input(uint32_t flags) {
		bytes.fill(0xa5); // Nonzero remaining bytes must not acquire invented validation.
		std::memcpy(bytes.data(), &flags, sizeof(flags));
	}
};

void Configure(Api& api, Input& input, int expected) {
	const auto saved = input;
	*api.error_addr() = 0x1234;
	Check(Name, api.configure(input.bytes.data()) == expected, "setter status/ABI");
	// The only failure exercised by this helper is native EBUSY=0x10;
	// it is not the low16 bits (0x0110) of the full SCE network status.
	const int expected_errno = expected == 0 ? 0x1234 : 0x10;
	Check(Name, *api.error_addr() == expected_errno,
	      "setter must preserve errno on success and store native EBUSY on repeat");
	Check(Name, input.before == saved.before && input.bytes == saved.bytes && input.after == saved.after,
	      "setter mutated input or adjacent canaries");
}

void BusyAndThreadErrno(Api& api) {
	Check(Name, api.configure(nullptr) == Network::NET_ERROR_EBUSY && *api.error_addr() == 0x10,
	      "already-set check must precede null");
	Check(Name, api.configure(reinterpret_cast<const void*>(1)) == Network::NET_ERROR_EBUSY &&
	            *api.error_addr() == 0x10, "repeated unreadable input must not be loaded by setter");
	int* main_errno = api.error_addr();
	*main_errno = 0x3456;
	bool worker_ok = false;
	std::thread worker([&] {
		int* local_errno = api.error_addr();
		*local_errno = 0x5678;
		const bool independent = local_errno != main_errno;
		const int busy = api.configure(nullptr);
		worker_ok = independent && busy == Network::NET_ERROR_EBUSY && *local_errno == 0x10;
	});
	worker.join();
	Check(Name, worker_ok && *main_errno == 0x3456, "net errno must remain thread-local");
}

void Fill(Api& api, int first, int limit) {
	for (int slot = first; slot < limit; ++slot) {
		Check(Name, api.create("net-pool-config", 0x1000, 0) == 3000 + slot, "lowest free public pool ID");
	}
}

void Full(Api& api) {
	Check(Name, api.create("full", 0x4000, 0) == Network::NET_ERROR_ENFILE, "configured quota not enforced");
}

void InvalidCreate(Api& api) {
	Check(Name, api.create(nullptr, 0x1000, 0) == Network::NET_ERROR_EINVAL, "null pool name");
	for (int size: {INT32_MIN, -1, 0, 0xfff}) {
		Check(Name, api.create("invalid", size, 0) == Network::NET_ERROR_EINVAL, "small/negative pool size");
	}
	for (int flags: {-1, 1, 2}) {
		Check(Name, api.create("invalid", 0x1000, flags) == Network::NET_ERROR_EINVAL, "nonzero pool flags");
	}
}

void InvalidPool(Api& api, int ssl, int id) {
	Check(Name, api.destroy(id) == Network::NET_ERROR_EBADF, "invalid/dead destroy ID");
	Check(Name, api.resolver_create("resolver", id, 0) == Network::NET_ERROR_RESOLVER_ENOSPACE,
	      "invalid/dead resolver pool ID");
	Check(Name, api.http_init(id, ssl, 0x4000) == Network::HTTP_ERROR_OUT_OF_MEMORY,
	      "invalid/dead HTTP pool ID");
}

void LivePool(Api& api, int ssl, int id) {
	const int resolver = api.resolver_create("resolver", id, 0);
	Check(Name, resolver >= 0, "resolver rejected live public pool ID");
	const int http = api.http_init(id, ssl, 0x4000);
	Check(Name, http >= 0, "HTTP rejected live public pool ID");
	CheckOk(Name, api.http_term(http), "HTTP cleanup");
	CheckOk(Name, api.resolver_destroy(resolver), "resolver cleanup");
}

void DestroyAll(Api& api, int count) {
	for (int slot = 0; slot < count; ++slot) {
		CheckOk(Name, api.destroy(3000 + slot), "pool cleanup (live IDs preserved)");
	}
}

void DefaultThenExtended(Api& api) {
	Instance instance;
	Check(Name, api.configure(nullptr) == Network::NET_ERROR_EINVAL && *api.error_addr() == 0x16,
	      "null setter before configuration / errno");
	InvalidCreate(api);
	Fill(api, 0, 32); // Failed calls above consume neither a slot nor the setter latch.
	Full(api);
	InvalidCreate(api); // Public validation precedes quota failure as well.
	const int ssl = api.ssl_init(0x4000);
	Check(Name, ssl >= 0, "SSL fixture init");
	for (int id: {INT32_MIN, -1, 0, 1, 31, 127, 2999, 3032, 3127, 3128, INT32_MAX}) {
		InvalidPool(api, ssl, id);
	}
	LivePool(api, ssl, 3000);
	LivePool(api, ssl, 3031);
	CheckOk(Name, api.destroy(3000), "release slot zero");
	InvalidPool(api, ssl, 3000);
	Check(Name, api.create("reuse-zero", 0x1000, 0) == 3000, "slot zero immediate reuse");
	CheckOk(Name, api.destroy(3017), "release middle slot");
	Check(Name, api.create("reuse-middle", 0x1000, 0) == 3017, "middle slot immediate reuse");
	Full(api);
	CheckOk(Name, Net::NetTerm(), "default NetTerm"); // No NetTerm NID is registered in this module.
	CheckOk(Name, api.init(), "default NetInit");
	Full(api);
	LivePool(api, ssl, 3000);

	Input extended(1);
	Configure(api, extended, 0); // Existing 32 pools survive expansion.
	extended.bytes.fill(0); // Configuration is copied, not retained as a caller pointer.
	Fill(api, 32, 128);
	Full(api);
	LivePool(api, ssl, 3032);
	LivePool(api, ssl, 3127);
	Input shrink(0);
	Configure(api, shrink, Network::NET_ERROR_EBUSY);
	BusyAndThreadErrno(api);
	CheckOk(Name, Net::NetTerm(), "extended NetTerm");
	CheckOk(Name, api.init(), "extended NetInit");
	Configure(api, shrink, Network::NET_ERROR_EBUSY);
	Full(api);
	LivePool(api, ssl, 3000);
	LivePool(api, ssl, 3127);
	CheckOk(Name, api.destroy(3127), "release extended final slot");
	InvalidPool(api, ssl, 3127);
	Check(Name, api.create("reuse-last", 0x1000, 0) == 3127, "busy/null/init/term changed extended quota");
	Full(api);
	CheckOk(Name, api.ssl_term(ssl), "SSL cleanup");
	DestroyAll(api, 128);
}

void BitZeroClear(Api& api, uint32_t flags) {
	Instance instance; // Fresh instance resets both configuration and occupancy.
	Input input(flags);
	if (flags == 0) { input.bytes.fill(0); } // Exact all-zero native input still latches.
	Configure(api, input, 0);
	input.bytes.fill(0xff); // Later bit0 changes in caller storage must not expand the quota.
	Fill(api, 0, 32);
	Full(api);
	Input extended(1);
	Configure(api, extended, Network::NET_ERROR_EBUSY);
	BusyAndThreadErrno(api);
	CheckOk(Name, Net::NetTerm(), "bit0-clear NetTerm");
	CheckOk(Name, api.init(), "bit0-clear NetInit");
	Configure(api, extended, Network::NET_ERROR_EBUSY);
	Full(api);
	CheckOk(Name, api.destroy(3031), "bit0-clear release final slot");
	Check(Name, api.create("reuse-default-last", 0x1000, 0) == 3031, "bit0-clear final slot reuse");
	Full(api);
	DestroyAll(api, 32);
}
} // namespace NetPoolConfigTest

void TestNetPoolConfig() {
	using namespace NetPoolConfigTest;
	Api api;
	Check(Name, api.create(nullptr, 0, 1) == Network::NET_ERROR_ENOTINIT && *api.error_addr() == 0xc8,
	      "pool readiness must precede argument validation");
	Check(Name, api.configure(nullptr) == Network::NET_ERROR_ENOTINIT && *api.error_addr() == 0xc8,
	      "readiness must precede null / errno");
	Check(Name, api.configure(reinterpret_cast<const void*>(1)) == Network::NET_ERROR_ENOTINIT &&
	            *api.error_addr() == 0xc8, "not-ready setter must not load input");
	DefaultThenExtended(api);
	BitZeroClear(api, 0);
	BitZeroClear(api, 0xfffffffeu); // Only bit0 controls quota; all other bits are accepted.
	Check(Name, api.configure(nullptr) == Network::NET_ERROR_ENOTINIT && *api.error_addr() == 0xc8,
	      "Network shutdown owns setter readiness");
}
