#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "libs/network.h"
#include "libs/netIfList.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Libs::LibHttp2 { void InitNet_1_Http2(Loader::SymbolDatabase*); }
namespace Libs::JscdSmall::NetCtl { void InitJscdSmallNetCtl_1(Loader::SymbolDatabase*); }
namespace Libs::LibNetConfig { void InitNetConfig_1(Loader::SymbolDatabase*); }

static void Check(bool value, const char* message) {
	if (!value) { std::fprintf(stderr, "NetworkHttpTests: %s\n", message); std::exit(1); }
}
template<typename F> F Export(Loader::SymbolDatabase& symbols, const char* nid) {
	const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
	Check(symbol != nullptr, "missing export");
	return reinterpret_cast<F>(symbol->vaddr);
}

int main(int argc, char** argv) {
	using namespace Libs;
	using namespace Libs::Network;
	Check(argc == 2, "expected loopback fixture URL");
	Common::InitializeThreads();
	Common::Subsystems systems;
	systems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	systems.Initialize<Log::Lifecycle>();
	Initialize();
	_putenv_s("KYTY_NETWORK_HOST", "1");
	Loader::SymbolDatabase interface_symbols;
	Libs::LibNetConfig::InitNetConfig_1(&interface_symbols);
	using IfList = int (KYTY_SYSV_ABI *)(int, NetIfaddr::NetIfList*, int);
	const auto interfaces = Export<IfList>(interface_symbols, "HoV-GJyx7YY");
	struct { uint32_t before; NetIfaddr::NetIfList entries[2]; uint32_t after; } list;
	std::memset(&list, 0xa5, sizeof(list));
	Check(interfaces(0, list.entries, 2) == 1, "host loopback interface not enumerated");
	Check(list.before == 0xa5a5a5a5 && list.after == 0xa5a5a5a5 &&
	      reinterpret_cast<const uint8_t*>(&list.entries[1])[0] == 0xa5 &&
	      reinterpret_cast<const uint8_t*>(&list.entries[1])[sizeof(NetIfaddr::NetIfList) - 1] == 0xa5,
	      "interface enumeration exceeded returned entry count");
	const auto& loopback = list.entries[0];
	const uint8_t loopback_ip[] {127, 0, 0, 1};
	Check(std::strcmp(loopback.name, "lo0") == 0 && loopback.index > 0 &&
	      (loopback.flags & 0x49) == 0x49 && loopback.mtu > 0 &&
	      std::memcmp(&loopback.ipv4[0].address, loopback_ip, 4) == 0,
	      "host interface record layout/address/flags");
	const auto snapshot = list.entries[0];
	Check(interfaces(0, list.entries, 0) == 0 && interfaces(3, list.entries, 1) == 0 &&
	      std::memcmp(&snapshot, list.entries, sizeof(snapshot)) == 0,
	      "empty enumeration modified output or fabricated debug interface");
	Check(interfaces(10, list.entries, 1) == NET_ERROR_EINVAL &&
	      interfaces(0, list.entries, -1) == NET_ERROR_EINVAL &&
	      interfaces(0, nullptr, 1) == NET_ERROR_EFAULT &&
	      interfaces(0, reinterpret_cast<NetIfaddr::NetIfList*>(1), 1) == NET_ERROR_EFAULT,
	      "interface enumeration invalid argument/address handling");
	_putenv_s("KYTY_NETWORK_HOST", "0");
	Check(interfaces(0, list.entries, 1) == 0 && std::memcmp(&snapshot, list.entries, sizeof(snapshot)) == 0,
	      "disabled host network exposed an interface");
	_putenv_s("KYTY_NETWORK_HOST", "1");
	Loader::SymbolDatabase netctl_symbols;
	Libs::JscdSmall::NetCtl::InitJscdSmallNetCtl_1(&netctl_symbols);
	using State = int (KYTY_SYSV_ABI *)(int*);
	const auto private_state = Export<State>(netctl_symbols, "JXlI9EZVjf4");
	struct { int before = 123, state = -987, after = 456; } net_state;
	int public_state = -1;
	Check(NetCtl::NetCtlGetState(&public_state) == 0 && private_state(&net_state.state) == 0 &&
	      net_state.state == public_state && net_state.before == 123 && net_state.after == 456,
	      "native private NetCtl output ABI or host state mismatch");
	Check(private_state(nullptr) == static_cast<int>(0x80412107u), "private NetCtl null output");
	_putenv_s("KYTY_NETWORK_HOST", "0");
	net_state.state = -987;
	Check(private_state(&net_state.state) == 0 && net_state.state == 0, "private NetCtl disabled host state");
	_putenv_s("KYTY_NETWORK_HOST", "1");
	const auto pool = Net::NetPoolCreate("HTTP test", 1024 * 1024, 0);
	const auto ssl = Ssl::SslInit(1024 * 1024);
	const auto http = Http::HttpInit(pool, ssl, 1024 * 1024);
	Check(pool >= 0 && ssl >= 0 && http >= 0, "HTTP contexts");
	const auto tmpl = Http::HttpCreateTemplate(http, "Kyty test", 2, 1);
	const auto conn = Http::HttpCreateConnectionWithURL(tmpl, argv[1], 1);
	const std::string body("hello\0world", 11);
	for (bool nonblock: {false, true}) {
		const auto req = Http::HttpCreateRequest(conn, 1, "/", body.size());
		Check(req >= 0, "request creation");
		Check(Http::HttpSetNonblock(req, nonblock) == 0, "nonblocking setting");
		const auto sent = Http::HttpSendRequest(req, body.data(), body.size());
		Check(sent == (nonblock ? HTTP_ERROR_EAGAIN : 0), "send result");
		if (nonblock) {
			Http::HttpEpollHandle epoll = nullptr;
			Check(Http::HttpCreateEpoll(http, &epoll) == 0, "epoll creation");
			Check(Http::HttpSetEpoll(req, epoll, nullptr) == 0, "epoll association");
			alignas(8) std::array<unsigned char, 24> event {};
			Check(Http::HttpWaitRequest(epoll, reinterpret_cast<Http::HttpNBEvent*>(event.data()), 1, 5000) == 1,
			      "nonblocking completion");
			Http::HttpDestroyEpoll(http, epoll);
		}
		int status = 0;
		Check(Http::HttpGetStatusCode(req, &status) == 0 && status == 201, "HTTP status");
		std::string received;
		char bytes[4];
		int count;
		while ((count = Http::HttpReadData(req, bytes, sizeof(bytes))) > 0) received.append(bytes, count);
		Check(count == 0 && received == body, "partial reads and EOF");
		Check(Http::HttpDeleteRequest(req) == 0 && Http::HttpReadData(req, bytes, sizeof(bytes)) == HTTP_ERROR_INVALID_ID,
		      "deleted request");
	}
	Check(Http::HttpSetCookieEnabled(tmpl, 1) == 0, "SNS cookie-enable prerequisite");
	const auto cookie_conn = Http::HttpCreateConnectionWithURL(tmpl, argv[1], 1);
	auto cookie_get = [&](int connection, const char* path, int enabled = -1) {
		const int req = Http::HttpCreateRequest(connection, 0, path, 0);
		Check(req >= 0, "cookie request creation");
		if (enabled >= 0) Check(Http::HttpSetCookieEnabled(req, enabled) == 0, "request cookie override");
		Check(Http::HttpSendRequest(req, nullptr, 0) == 0, "cookie request send");
		char bytes[256];
		const int count = Http::HttpReadData(req, bytes, sizeof(bytes));
		Check(count >= 0, "cookie response read");
		Http::HttpDeleteRequest(req);
		return std::string(bytes, count);
	};
	cookie_get(cookie_conn, "/cookie-set");
	Check(cookie_get(cookie_conn, "/cookie-echo") == "retail=owned", "template-to-connection-to-request cookie inheritance");
	Check(cookie_get(conn, "/cookie-echo").empty(), "template mutation changed existing connection policy");
	Check(cookie_get(cookie_conn, "/cookie-echo", 0).empty(), "request cookie-disable override");
	Check(cookie_get(conn, "/cookie-echo", 1) == "retail=owned", "same HTTP context did not share jar");
	const auto isolated_http = Http::HttpInit(pool, ssl, 1024 * 1024);
	const auto isolated_template = Http::HttpCreateTemplate(isolated_http, "isolated", 2, 1);
	Check(Http::HttpSetCookieEnabled(isolated_template, 1) == 0, "isolated enable");
	const auto isolated_conn = Http::HttpCreateConnectionWithURL(isolated_template, argv[1], 1);
	Check(cookie_get(isolated_conn, "/cookie-echo").empty(), "HTTP contexts shared cookies");
	Http::HttpDeleteConnection(isolated_conn);
	Http::HttpDeleteTemplate(isolated_template);
	Http::HttpTerm(isolated_http);
	Http::HttpDeleteConnection(cookie_conn);
	_putenv_s("KYTY_NETWORK_HOST", "0");
	const auto offline = Http::HttpCreateRequest(conn, 0, "/", 0);
	Check(Http::HttpSendRequest(offline, nullptr, 0) == HTTP_ERROR_TIMEOUT, "offline compatibility");
	Http::HttpDeleteRequest(offline);
	Http::HttpDeleteConnection(conn);
	Http::HttpDeleteTemplate(tmpl);
	Http::HttpTerm(http);
	Ssl::SslTerm(ssl);
	Net::NetPoolDestroy(pool);
	_putenv_s("KYTY_NETWORK_HOST", "1");
	Loader::SymbolDatabase symbols;
	LibHttp2::InitNet_1_Http2(&symbols);
	auto init2 = Export<int(KYTY_SYSV_ABI*)(int,int,size_t,int)>(symbols,"3JCe3lCbQ8A");
	auto template2 = Export<int(KYTY_SYSV_ABI*)(int,const char*,int,int)>(symbols,"+wCt7fCijgk");
	auto request2 = Export<int(KYTY_SYSV_ABI*)(int,const char*,const char*,uint64_t)>(symbols,"mmyOCxQMVYQ");
	auto send2 = Export<int(KYTY_SYSV_ABI*)(int,const void*,size_t)>(symbols,"rbqZig38AT8");
	auto async2 = Export<int(KYTY_SYSV_ABI*)(int,const void*,size_t,void*,void*)>(symbols,"A+NVAFu4eCg");
	auto wait2 = Export<int(KYTY_SYSV_ABI*)(int,void*,uint32_t*,void*)>(symbols,"MOp-AUhdfi8");
	auto status2 = Export<int(KYTY_SYSV_ABI*)(int,int*)>(symbols,"9XYJwCf3lEA");
	auto read2 = Export<int(KYTY_SYSV_ABI*)(int,void*,size_t)>(symbols,"QygCNNmbGss");
	auto term2 = Export<int(KYTY_SYSV_ABI*)(int)>(symbols,"YiBUtz-pGkc");
	const auto ctx2 = init2(0,0,1024*1024,4);
	const auto tmpl2 = template2(ctx2,"Kyty test",2,1);
	for (bool async: {false,true}) {
		const auto req = request2(tmpl2,"POST",argv[1],body.size());
		Check(req != tmpl2 && req != ctx2, "HTTP2 handles overlap");
		if (async) {
			Check(async2(req,body.data(),body.size(),nullptr,nullptr)==0,"HTTP2 async send");
			alignas(8) std::array<int,6> result {};
			uint32_t timeout = 5000000;
			Check(wait2(req,result.data(),&timeout,nullptr)==0 && result[2]==0,"HTTP2 async completion");
		} else Check(send2(req,body.data(),body.size())==0,"HTTP2 send");
		int status=0;
		Check(status2(req,&status)==0 && status==201,"HTTP2 status");
		char bytes[32] {};
		Check(read2(req,bytes,sizeof(bytes))==body.size() && std::string(bytes,body.size())==body &&
		      read2(req,bytes,sizeof(bytes))==0,"HTTP2 body and EOF");
	}
	Check(term2(ctx2)==0,"HTTP2 cleanup");
	Shutdown();
	systems.Destroy();
	std::puts("NetworkHttpTests: HTTP/HTTP2 sync, async, partial reads, EOF and offline cases passed");
}
