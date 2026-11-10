#include "libs/hostHttp.h"

#include <cstdio>
#include <cstdlib>

static void Check(bool value, const char* message) {
	if (!value) { std::fprintf(stderr, "HostHttpTests: %s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
	using namespace Libs::HostHttp;
	Check(argc == 2, "expected local fixture URL");
	Request request;
	request.url = argv[1];
	_putenv_s("KYTY_NETWORK_HOST", "0");
	Check(!Enabled() && Send(request).error != 0, "disabled transport sent a request");
	_putenv_s("KYTY_NETWORK_HOST", "1");
	request.method = "POST";
	request.body = std::string("hello\0world", 11);
	request.headers.emplace_back("X-Kyty-Test", "echo");
	auto response = Send(request);
	Check(response.error == 0 && response.status == 201 && response.body == request.body,
	      "POST status or binary body mismatch");
	Check(response.headers.find("X-Echo: echo") != std::string::npos, "response headers missing");
	request.method = "GET";
	request.body.clear();
	request.url = std::string(argv[1]) + "missing";
	response = Send(request);
	Check(response.error == 0 && response.status == 404 && response.body == "missing", "404 is not an HTTP response");
	request.url = std::string(argv[1]) + "redirect";
	request.redirect = false;
	response = Send(request);
	Check(response.error == 0 && response.status == 302, "redirect suppression failed");
	request.redirect = true;
	response = Send(request);
	Check(response.error == 0 && response.status == 200 && response.body == "ok", "redirect following failed");
	request.session = CreateSession();
	request.cookies = true;
	const std::string root = argv[1];
	auto get = [&](const char* path) {
		request.url = root + path;
		auto response = Send(request);
		Check(response.error == 0 && response.status == 200, "cookie fixture transport failed");
		return response.body;
	};
	Check(get("cookie-echo").empty(), "new context inherited cookies");
	get("cookie-set");
	Check(get("cookie-echo") == "retail=owned", "cookie not retained across requests or path scope lost");
	Check(get("scoped/cookie-echo").find("limited=yes") != std::string::npos, "cookie path match lost");
	request.cookies = false;
	Check(get("cookie-echo").empty(), "disabled automatic cookies leaked");
	request.cookies = true;
	Check(get("cookie-echo") == "retail=owned", "disable erased session jar");
	Check(get("cookie-redirect").find("redirected=yes") != std::string::npos, "redirect cookie not consumed");
	auto retained = request.session;
	request.session = CreateSession();
	Check(get("cookie-echo").empty(), "separate context inherited old cookie jar");
	request.cookies = false;
	get("cookie-set");
	request.cookies = true;
	Check(get("cookie-echo").empty(), "disabled cookie receive stored cookies");
	request.session = retained;
	retained.reset();
	Check(get("cookie-echo").find("retail=owned") != std::string::npos, "retained transfer namespace retired early");
	request.headers.emplace_back("bad\r\nname", "value");
	Check(Send(request).error != 0, "header injection accepted");
	request.headers.clear();
	request.url = "file:///C:/Windows/win.ini";
	Check(Send(request).error != 0, "non-HTTP URL accepted");
	std::puts("HostHttpTests: all cases passed");
}
