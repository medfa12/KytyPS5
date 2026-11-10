#include "libs/hostHttp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace Libs::HostHttp {
class Session {
public:
#if defined(_WIN32)
	~Session() { if (handle) WinHttpCloseHandle(handle); }
	HINTERNET Get() {
		std::lock_guard<std::mutex> lock(mutex);
		if (!handle) handle = WinHttpOpen(L"Kyty", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		return handle;
	}
private:
	std::mutex mutex;
	HINTERNET handle = nullptr;
#endif
};

std::shared_ptr<Session> CreateSession() { return std::make_shared<Session>(); }

static std::atomic_bool g_configured_enabled {false};

void SetConfiguredEnabled(bool enabled) {
	g_configured_enabled.store(enabled, std::memory_order_relaxed);
}

int GuestError(uint32_t error, bool http2) {
	if (error == 0) return 0;
	uint32_t code = 0x1063; // network error
#if defined(_WIN32)
	switch (error) {
		case ERROR_WINHTTP_TIMEOUT: code = 0x1068; break;
		case ERROR_WINHTTP_SECURE_FAILURE:
			if (http2) return static_cast<int>(0x8095f00au); // SSL invalid certificate
			code = 0x1075; break;
		case ERROR_INVALID_PARAMETER: code = 0x11fe; break;
		case ERROR_NOT_ENOUGH_MEMORY: code = 0x1022; break;
		case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: code = 0x1064; break;
		default: break;
	}
#endif
	if (http2 && code == 0x1063) return static_cast<int>(0x80410133u); // Net ENETUNREACH
	return static_cast<int>((http2 ? 0x817b0000u : 0x80430000u) | code);
}

std::shared_future<Response> Start(Request request) {
	std::packaged_task<Response()> task([request = std::move(request)] { return Send(request); });
	auto result = task.get_future().share();
	std::thread(std::move(task)).detach();
	return result;
}

bool Enabled() {
	const char* value = std::getenv("KYTY_NETWORK_HOST");
	return g_configured_enabled.load(std::memory_order_relaxed) ||
	       (value != nullptr && std::strcmp(value, "1") == 0);
}

#if defined(_WIN32)
namespace {
struct Handle {
	HINTERNET value;
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
	explicit Handle(HINTERNET handle): value(handle) {}
	~Handle() { if (value) WinHttpCloseHandle(value); }
};
std::wstring Wide(const std::string& text) {
	if (text.empty() || text.size() > INT_MAX) return {};
	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring out(size, L'\0');
	if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size);
	return out;
}
}
#endif

Response Send(const Request& request) {
	Response result;
#if defined(_WIN32)
	if (!Enabled()) { result.error = ERROR_NETWORK_UNREACHABLE; return result; }
	const auto url = Wide(request.url);
	URL_COMPONENTS parts {sizeof(parts)};
	parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
	    (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
	    request.body.size() > MAXDWORD) {
		result.error = ERROR_INVALID_PARAMETER; return result;
	}
	const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
	std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
	if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
	if (path.empty()) path = L"/";
	const auto session_owner = request.session ? request.session : CreateSession();
	const auto session = session_owner->Get();
	if (!session) { result.error = GetLastError(); return result; }
	const auto timeout = [](uint32_t n) { return static_cast<int>(std::clamp(n, 1u, static_cast<uint32_t>(INT_MAX))); };
	Handle connection {WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
	if (!connection.value) { result.error = GetLastError(); return result; }
	const auto method = Wide(request.method);
	const wchar_t* version = request.http_version == 1 ? L"HTTP/1.0" : nullptr;
	Handle call {WinHttpOpenRequest(connection.value, method.c_str(), path.c_str(), version,
	                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	                               parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
	if (!call.value) { result.error = GetLastError(); return result; }
	// Session state is shared, request options are not: concurrent templates may
	// use different agents/timeouts without racing session-wide configuration.
	if (!WinHttpSetTimeouts(call.value, timeout(request.resolve_timeout_ms), timeout(request.connect_timeout_ms),
	                       timeout(request.send_timeout_ms), timeout(request.recv_timeout_ms))) {
		result.error = GetLastError(); return result;
	}
	if (request.http_version == 3) {
		DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
		if (!WinHttpSetOption(call.value, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols))) {
			result.error = GetLastError(); return result;
		}
	}
	DWORD redirects = request.max_redirects;
	if (!WinHttpSetOption(call.value, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects))) {
		result.error = GetLastError(); return result;
	}
	if (request.decompress) {
		DWORD encoding = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
		if (!WinHttpSetOption(call.value, WINHTTP_OPTION_DECOMPRESSION, &encoding, sizeof(encoding))) {
			result.error = GetLastError(); return result;
		}
	}
	DWORD disable = request.cookies ? 0 : WINHTTP_DISABLE_COOKIES;
	if (!request.redirect) disable |= WINHTTP_DISABLE_REDIRECTS;
	if (disable && !WinHttpSetOption(call.value, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable))) {
		result.error = GetLastError(); return result;
	}
	DWORD auth = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
	if (!WinHttpSetOption(call.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &auth, sizeof(auth))) {
		result.error = GetLastError(); return result;
	}
	if (request.user_agent.find_first_of("\r\n") != std::string::npos) {
		result.error = ERROR_INVALID_PARAMETER; return result;
	}
	std::string header_text = "User-Agent: " + request.user_agent + "\r\n";
	for (const auto& [name, value]: request.headers) {
		if (name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos) {
			result.error = ERROR_INVALID_PARAMETER; return result;
		}
		header_text += name + ": " + value + "\r\n";
	}
	const auto headers = Wide(header_text);
	if (!WinHttpSendRequest(call.value, headers.c_str(), static_cast<DWORD>(headers.size()),
	                        request.body.empty() ? nullptr : const_cast<char*>(request.body.data()),
	                        static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0) ||
	    !WinHttpReceiveResponse(call.value, nullptr)) {
		result.error = GetLastError(); return result;
	}
	DWORD status = 0, size = sizeof(status);
	if (!WinHttpQueryHeaders(call.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
		result.error = GetLastError(); return result;
	}
	result.status = static_cast<int>(status);
	size = 0;
	WinHttpQueryHeaders(call.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
	                    nullptr, &size, WINHTTP_NO_HEADER_INDEX);
	std::wstring response_headers(size / sizeof(wchar_t), L'\0');
	if (size && WinHttpQueryHeaders(call.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
	                                response_headers.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
		const int count = static_cast<int>(std::wcslen(response_headers.c_str()));
		const int bytes = WideCharToMultiByte(CP_UTF8, 0, response_headers.data(), count, nullptr, 0, nullptr, nullptr);
		result.headers.resize(bytes);
		WideCharToMultiByte(CP_UTF8, 0, response_headers.data(), count, result.headers.data(), bytes, nullptr, nullptr);
	}
	size = 0;
	WinHttpQueryOption(call.value, WINHTTP_OPTION_URL, nullptr, &size);
	std::wstring final_url(size / sizeof(wchar_t), L'\0');
	if (size && WinHttpQueryOption(call.value, WINHTTP_OPTION_URL, final_url.data(), &size)) {
		const int count = static_cast<int>(std::wcslen(final_url.c_str()));
		const int bytes = WideCharToMultiByte(CP_UTF8, 0, final_url.data(), count, nullptr, 0, nullptr, nullptr);
		result.effective_url.resize(bytes);
		WideCharToMultiByte(CP_UTF8, 0, final_url.data(), count, result.effective_url.data(), bytes, nullptr, nullptr);
	}
	std::array<char, 16384> buffer;
	for (;;) {
		DWORD count = 0;
		if (!WinHttpReadData(call.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count)) {
			result.error = GetLastError(); break;
		}
		if (!count) break;
		if (result.body.size() + count > 64u * 1024u * 1024u) { result.error = ERROR_NOT_ENOUGH_MEMORY; break; }
		result.body.append(buffer.data(), count);
	}
#else
	result.error = 1;
#endif
	return result;
}
} // namespace Libs::HostHttp
