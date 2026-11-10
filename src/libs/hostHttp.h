#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace Libs::HostHttp {
// A guest HTTP context owns one cookie/session namespace. Transfers retain it
// independently of guest IDs, so ID reuse cannot inherit a retired cookie jar.
class Session;
std::shared_ptr<Session> CreateSession();
void SetConfiguredEnabled(bool enabled);
bool Enabled();
struct Request {
	std::shared_ptr<Session> session;
	bool cookies = false;
	std::string url, method = "GET", user_agent = "Kyty";
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
	uint32_t resolve_timeout_ms = 30000, connect_timeout_ms = 30000, send_timeout_ms = 30000, recv_timeout_ms = 30000;
	bool redirect = true;
	bool decompress = false;
	uint32_t max_redirects = 10;
	int http_version = 0; // 0=host default, 1=HTTP/1.0, 2=HTTP/1.1, 3=negotiate HTTP/2
};
struct Response {
	uint32_t error = 0;
	int status = 0;
	std::string headers, body, effective_url;
};
Response Send(const Request& request);
// Owns all request bytes; the worker never touches guest memory or HLE tables.
std::shared_future<Response> Start(Request request);
int GuestError(uint32_t error, bool http2 = false);
} // namespace Libs::HostHttp
