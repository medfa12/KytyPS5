// Uses the production registered HTTP SysV entry and real net-pool lifetime.
// No native provider or fabricated SSL context is needed: native HTTP init
// retains an opaque signed ID without looking up/refcounting that context.
namespace NativeSslHttpTest {
constexpr const char* Name = "NativeSslHttp";
struct Environment {
	bool present;
	std::string old;
	Environment() {
		const char* value = std::getenv("KYTY_NATIVE_SSL");
		present = value != nullptr;
		if (value) { old = value; }
	}
	static void Set(const char* value) {
#ifdef _WIN32
		_putenv_s("KYTY_NATIVE_SSL", value ? value : "");
#else
		if (value) { setenv("KYTY_NATIVE_SSL", value, 1); } else { unsetenv("KYTY_NATIVE_SSL"); }
#endif
	}
	~Environment() { Set(present ? old.c_str() : nullptr); }
};
void Retention(NetPoolConfigTest::Api& api) {
	Environment::Set("1");
	NetPoolConfigTest::Instance instance;
	// A process-lifetime mode is captured at Network creation, not reevaluated
	// under a guest-held HTTP lock or switched by subsequent environment changes.
	Environment::Set("0");
	const int pool = api.create("native-ssl-http", 0x4000, 0);
	Check(Name, pool >= 0, "net pool creation");
	using Socket = int (KYTY_SYSV_ABI *)(const char*, int, int, int);
	using Close = int (KYTY_SYSV_ABI *)(int);
	using Abort = int (KYTY_SYSV_ABI *)(int, int);
	const auto socket = api.Resolve<Socket>("Q4qBuN-c0ZM", "Net", "LibNet::NetSocket");
	const auto close = api.Resolve<Close>("45ggEzakPJQ", "Net", "LibNet::NetSocketClose");
	const auto abort = api.Resolve<Abort>("zJGf8xjFnQE", "Net", "LibNet::NetSocketAbort");
	const int fd = socket("native-ssl-real-transport", 2, 2, 0);
	struct SocketOwner { Close close; int fd; ~SocketOwner() { if (fd >= 0) { close(fd); } } } owner {close, fd};
	Check(Name, fd >= 0 && Libs::Network::Net::IsSocket(fd), "native SSL received a synthetic socket");
	std::array<uint8_t, 16> address {16, 2, 0, 0, 127, 0, 0, 1};
	CheckOk(Name, Libs::Network::Net::Bind(fd, address.data(), address.size()), "loopback bind");
	uint32_t address_size = address.size();
	CheckOk(Name, Libs::Network::Net::Getsockname(fd, address.data(), &address_size), "loopback address");
	CheckOk(Name, Libs::Network::Net::Connect(fd, address.data(), address_size), "loopback connect");
	const int nonblocking = 1;
	CheckOk(Name, Libs::Network::Net::Setsockopt(fd, 0xffff, 0x1200, &nonblocking, sizeof(nonblocking)), "bounded socket I/O");
	Check(Name, abort(fd, 7) == Libs::Network::NET_ERROR_EOPNOTSUPP &&
	            *api.error_addr() == Libs::Posix::POSIX_EOPNOTSUPP && Libs::Network::Net::IsSocket(fd),
	      "unsupported abort fabricated success or consumed the transport");
	const uint8_t sent = 0xa7;
	Check(Name, Libs::Network::Net::Send(fd, &sent, 1, 0) == 1, "unsupported abort shut down sending");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	uint8_t received = 0;
	int64_t count = -1;
	do {
		count = Libs::Network::Net::Recv(fd, &received, 1, 0);
		if (count >= 0 || *Libs::Posix::GetErrorAddr() != Libs::Posix::POSIX_EWOULDBLOCK) { break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < deadline);
	Check(Name, count == 1 && received == sent, "unsupported abort shut down or changed receiving");
	CheckOk(Name, close(fd), "real socket cleanup");
	owner.fd = -1;
	Check(Name, !Libs::Network::Net::IsSocket(fd), "closed socket remained registered");
	Check(Name, abort(fd, 7) == Libs::Network::NET_ERROR_EBADF, "abort accepted closed socket");
	for (int ssl: {1, 0x12345678, INT32_MAX, 0, -1, INT32_MIN}) {
		const int http = api.http_init(pool, ssl, 0x4000);
		int retained = 17;
		Check(Name, http >= 0 && Libs::Network::HttpSslIdForTest(http, &retained) && retained == ssl,
		      "native HTTP changed or validated an opaque signed SSL ID");
		CheckOk(Name, api.http_term(http), "HTTP cleanup");
		Check(Name, !Libs::Network::HttpSslIdForTest(http, &retained), "dead HTTP context retained");
	}
	CheckOk(Name, api.destroy(pool), "pool cleanup");
	Check(Name, api.http_init(pool, 1, 0x4000) == Libs::Network::HTTP_ERROR_OUT_OF_MEMORY,
	      "native ID mode bypassed net-pool lifetime");
}
void Legacy(NetPoolConfigTest::Api& api, const char* option) {
		Environment::Set(option);
		NetPoolConfigTest::Instance instance;
		const int pool = api.create("legacy-ssl-http", 0x4000, 0);
		Check(Name, api.http_init(pool, 1, 0x4000) == Libs::Network::HTTP_ERROR_OUT_OF_MEMORY,
		      "non-exact option enabled native SSL mode");
		const int ssl = api.ssl_init(0x4000);
		const int http = api.http_init(pool, ssl, 0x4000);
		Check(Name, http >= 0, "legacy live SSL rejected");
		CheckOk(Name, api.http_term(http), "legacy HTTP cleanup");
		CheckOk(Name, api.ssl_term(ssl), "legacy SSL cleanup");
		Check(Name, api.http_init(pool, ssl, 0x4000) == Libs::Network::HTTP_ERROR_OUT_OF_MEMORY,
		      "legacy dead SSL accepted");
		CheckOk(Name, api.destroy(pool), "legacy pool cleanup");
}
}
int NativeSslHttpChild(const char* option) {
	NativeSslHttpTest::Environment environment;
	NativeSslHttpTest::Environment::Set(option);
#ifdef _WIN32
	_putenv_s("KYTY_NATIVE_SSL_SMOKE", "");
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
	unsetenv("KYTY_NATIVE_SSL_SMOKE");
#endif
	try {
		InitSubsystems();
		NetPoolConfigTest::Api api;
		if (std::strcmp(option, "1") == 0) { NativeSslHttpTest::Retention(api); }
		else { NativeSslHttpTest::Legacy(api, option); }
	} catch (const TestFailure&) { return 1; }
	return 0;
}
void TestNativeSslHttp() {
#ifdef _WIN32
	constexpr const char* test = NativeSslHttpTest::Name;
	wchar_t exe[32768] {};
	Check(test, GetModuleFileNameW(nullptr, exe, 32768) != 0, "resolve executable");
	for (const wchar_t* option: {L"1", L"0", L"true", L"01"}) {
		STARTUPINFOW startup {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION child {};
		std::wstring command = L"\"" + std::wstring(exe) + L"\" --native-ssl-http-child " + option;
		Check(test, CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
		                          nullptr, nullptr, &startup, &child) != 0, "launch fresh mode child");
		const auto waited = WaitForSingleObject(child.hProcess, 4000);
		if (waited != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 75); WaitForSingleObject(child.hProcess, 1000); }
		DWORD exit = 0;
		const bool queried = GetExitCodeProcess(child.hProcess, &exit) != 0;
		CloseHandle(child.hThread); CloseHandle(child.hProcess);
		Check(test, waited == WAIT_OBJECT_0 && queried && exit == 0, "fresh-mode HTTP/socket fixture failed or hung");
	}
#else
	// The loader's selected native SSL path is currently Windows-only.
	Check(NativeSslHttpTest::Name, NativeSslHttpChild("0") == 0, "legacy HTTP validation");
#endif
}
