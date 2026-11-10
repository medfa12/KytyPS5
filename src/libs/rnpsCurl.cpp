#include "libs/rnpsCurl.h"
#include "libs/hostHttp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::RnpsCurl {
namespace {
using Clock = std::chrono::steady_clock;
using Write = size_t (KYTY_SYSV_ABI *)(char*, size_t, size_t, void*);
using Progress = int (KYTY_SYSV_ABI *)(void*, int64_t, int64_t, int64_t, int64_t);
constexpr int Ok = 0, Unsupported = 4, BadArgument = 43, UnknownOption = 48;
constexpr size_t BodyLimit = 64 * 1024 * 1024;
struct Share { std::atomic_uint users {0}; };
struct Easy {
	Easy() { request.redirect = false; request.max_redirects = 50; request.user_agent.clear(); }
	HostHttp::Request request;
	HostHttp::Response response;
	std::shared_future<HostHttp::Response> future;
	std::string custom_method, content_type, copied_body, cookie, referer;
	const char* post = nullptr;
	Slist* headers = nullptr;
	int64_t post_size = -1;
	int64_t upload_size = -1;
	Write read = nullptr;
	void* read_data = nullptr;
	uint64_t uploaded = 0;
	uint64_t timeout_ms = 0;
	Write write = nullptr, header = nullptr;
	Progress progress = nullptr;
	void *write_data = nullptr, *header_data = nullptr, *progress_data = nullptr, *private_data = nullptr;
	void* multi = nullptr;
	std::shared_ptr<Share> share;
	char* error_buffer = nullptr;
	bool posting = false, head = false, include_header = false, fail_http = false, no_progress = true;
	bool upload = false;
	bool copied_post = false;
	bool done = false, delivering = false;
	int result = 0;
	Clock::time_point started;
	double elapsed = 0;
	~Easy() { if (share) --share->users; }
};
struct Multi {
	std::vector<void*> handles;
	std::deque<Message> messages;
	Message last {};
	bool performing = false;
	std::mutex wait_mutex;
	std::condition_variable wake;
	bool awakened = false;
};
std::mutex registry_mutex;
uintptr_t next_handle = 0xc000;
std::unordered_map<void*, std::shared_ptr<Easy>> easies;
std::unordered_map<void*, std::shared_ptr<Multi>> multis;
std::unordered_map<void*, std::shared_ptr<Share>> shares;
template<class T> std::shared_ptr<T> Find(const std::unordered_map<void*, std::shared_ptr<T>>& map, void* handle) {
	std::lock_guard lock(registry_mutex);
	auto it = map.find(handle);
	return it == map.end() ? nullptr : it->second;
}
template<class T> void* Create(std::unordered_map<void*, std::shared_ptr<T>>& map) {
	try {
		auto object = std::make_shared<T>();
		std::lock_guard lock(registry_mutex);
		auto* handle = reinterpret_cast<void*>(next_handle += 16);
		map.emplace(handle, std::move(object));
		return handle;
	} catch (...) { return nullptr; }
}
bool Ready(const Easy& e) {
	return e.future.valid() && e.future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}
bool TimedOut(const Easy& e) {
	return e.timeout_ms && Clock::now() - e.started >= std::chrono::milliseconds(e.timeout_ms);
}
int CurlError(uint32_t error) {
	// WinHTTP errors; kept numeric so the guest adapter also builds off Windows.
	switch (error) {
		case 0: return 0;
		case 12002: return 28; // operation timed out
		case 12007: return 6; // could not resolve host
		case 12044: return 98; // client certificate required
		case 12156: return 47; // too many redirects
		case 12175: return 60; // TLS certificate verification
		case 12005: case 87: return 3; // malformed URL
		case 8: return 27; // out of memory
		default: return 7; // could not connect
	}
}
void Finish(Easy& e, int result) {
	e.future = {};
	e.result = result;
	e.done = true;
	e.elapsed = std::chrono::duration<double>(Clock::now() - e.started).count();
	if (e.error_buffer && result) std::snprintf(e.error_buffer, 256, "%s", EasyStrerror(result));
	// Do not log URL paths, queries, headers, or account tokens.
	std::fprintf(stderr, "RNPS curl: completed status=%d curl=%d host=%u bytes=%zu\n",
	             e.response.status, result, e.response.error, e.response.body.size());
}
int Start(Easy& e) {
	e.started = Clock::now();
	e.response = {};
	e.content_type.clear();
	e.result = 0;
	e.elapsed = 0;
	e.uploaded = 0;
	if (e.error_buffer) e.error_buffer[0] = 0;
	if (!e.no_progress && e.progress && e.progress(e.progress_data, 0, 0, 0, 0)) return 42;
	auto request = e.request; // HostHttp owns everything touched by its worker.
	request.method = !e.custom_method.empty() ? e.custom_method : e.head ? "HEAD" : e.upload ? "PUT" : e.posting ? "POST" : "GET";
	if (!e.head && (e.upload || (e.posting && !e.post))) {
		const auto expected = e.upload ? e.upload_size : e.post_size;
		if (expected > int64_t(BodyLimit)) return BadArgument;
		if (!e.read && expected != 0) return 26;
		char buffer[16384];
		while (expected < 0 || request.body.size() < uint64_t(expected)) {
			const auto capacity = expected < 0 ? sizeof(buffer) : std::min<uint64_t>(sizeof(buffer), uint64_t(expected) - request.body.size());
			const auto count = e.read(buffer, 1, capacity, e.read_data);
			if (count == 0x10000000) return 42;
			if (count > capacity || request.body.size() + count > BodyLimit) return 26;
			if (!count) {
				if (expected >= 0 && request.body.size() != uint64_t(expected)) return 26;
				break;
			}
			request.body.append(buffer, count);
			if (TimedOut(e)) return 28;
		}
	} else if (e.posting && !e.head) {
		const auto size = e.post_size < 0 ? (e.post ? std::strlen(e.post) : 0) : uint64_t(e.post_size);
		if (size > BodyLimit || (size && !e.post)) return BadArgument;
		if (size) request.body.assign(e.post, size);
	}
	e.uploaded = request.body.size();
	for (auto* node = e.headers; node; node = node->next) {
		if (!node->data || request.headers.size() >= 4096) return BadArgument;
		std::string line(node->data);
		const auto colon = line.find(':');
		if (colon == std::string::npos) return BadArgument;
		auto value = line.substr(colon + 1);
		if (!value.empty() && value[0] == ' ') value.erase(0, 1);
		request.headers.emplace_back(line.substr(0, colon), std::move(value));
	}
	if (!e.cookie.empty()) request.headers.emplace_back("Cookie", e.cookie);
	if (!e.referer.empty()) request.headers.emplace_back("Referer", e.referer);
	if (e.posting && std::none_of(request.headers.begin(), request.headers.end(), [](const auto& h) {
		std::string key = h.first;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		return key == "content-type";
	})) request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
	try { e.future = HostHttp::Start(std::move(request)); }
	catch (...) { return 27; }
	return Ok;
}
bool Deliver(Write callback, void* data, const char* bytes, size_t size) {
	return callback && callback(const_cast<char*>(bytes), 1, size, data) == size;
}
int Complete(Easy& e) {
	try { e.response = e.future.get(); } catch (...) { return 7; }
	e.future = {};
	if (e.response.error) return CurlError(e.response.error);
	const auto& headers = e.response.headers;
	for (size_t begin = 0; begin < headers.size();) {
		const auto end = headers.find('\n', begin);
		const auto size = (end == std::string::npos ? headers.size() : end + 1) - begin;
		std::string line = headers.substr(begin, size);
		if (line.size() >= 13) {
			auto key = line.substr(0, 13);
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			if (key == "content-type:") {
				auto first = line.find_first_not_of(" \t", 13);
				if (first != std::string::npos) e.content_type = line.substr(first, line.find_last_not_of("\r\n") - first + 1);
			}
		}
		if (e.header && !Deliver(e.header, e.header_data, headers.data() + begin, size)) return 23;
		if (!e.header && e.header_data && !Deliver(e.write, e.header_data, headers.data() + begin, size)) return 23;
		if (e.include_header && !Deliver(e.write, e.write_data, headers.data() + begin, size)) return 23;
		begin += size;
	}
	if (e.fail_http && e.response.status >= 400) return 22;
	if (!e.no_progress && e.progress && e.progress(e.progress_data, e.response.body.size(), e.response.body.size(), e.uploaded, e.uploaded)) return 42;
	if (!e.head) {
		for (size_t offset = 0; offset < e.response.body.size(); offset += 16384) {
			auto size = std::min<size_t>(16384, e.response.body.size() - offset);
			if (!Deliver(e.write, e.write_data, e.response.body.data() + offset, size)) return 23;
		}
	}
	return Ok;
}
} // namespace

static_assert(sizeof(Message) == 24 && offsetof(Message, easy) == 8 && offsetof(Message, data) == 16);
int RNPS_CURL_ABI GlobalInit(int64_t) {
	std::fprintf(stderr, "RNPS curl: host transport initialized enabled=%d\n", HostHttp::Enabled());
	return Ok;
}
void* RNPS_CURL_ABI EasyInit() { return Create(easies); }
void RNPS_CURL_ABI EasyCleanup(void* handle) {
	auto e = Find(easies, handle);
	if (!e || e->delivering) return;
	if (e->multi && MultiRemoveHandle(e->multi, handle) != Ok) return;
	std::lock_guard lock(registry_mutex);
	easies.erase(handle);
}
int RNPS_CURL_ABI EasySetopt(void* handle, int option, uint64_t value) {
	auto e = Find(easies, handle);
	if (!e || e->future.valid() || e->delivering) return BadArgument;
	auto text = [value] { return value ? std::string(reinterpret_cast<const char*>(value)) : std::string(); };
	switch (option) {
		case 10002: e->request.url = text(); break;
		case 10018: e->request.user_agent = text(); break;
		case 10036: e->custom_method = text(); break;
		case 10001: e->write_data = reinterpret_cast<void*>(value); break;
		case 20011: e->write = reinterpret_cast<Write>(value); break;
		case 10009: e->read_data = reinterpret_cast<void*>(value); break;
		case 20012: e->read = reinterpret_cast<Write>(value); break;
		case 14: case 30115:
			if (int64_t(value) < -1 || int64_t(value) > int64_t(BodyLimit)) return BadArgument;
			e->upload_size = int64_t(value); break;
		// Upload bytes are copied before transport; WinHTTP can replay that copy
		// without seeking the guest input on a redirect/authentication retry.
		case 20167: case 10168: break;
		case 10029: e->header_data = reinterpret_cast<void*>(value); break;
		case 20079: e->header = reinterpret_cast<Write>(value); break;
		case 10057: e->progress_data = reinterpret_cast<void*>(value); break;
		case 20219: e->progress = reinterpret_cast<Progress>(value); break;
		case 10023: e->headers = reinterpret_cast<Slist*>(value); break;
		case 10103: e->private_data = reinterpret_cast<void*>(value); break;
		case 10010: e->error_buffer = reinterpret_cast<char*>(value); break;
		case 10015: e->post = reinterpret_cast<const char*>(value); e->copied_body.clear(); e->copied_post = false; e->posting = true; break;
		case 10165:
			if (e->post_size < -1 || e->post_size > int64_t(BodyLimit)) return BadArgument;
			e->copied_body = value ? (e->post_size < 0 ? text() : std::string(reinterpret_cast<const char*>(value), size_t(e->post_size))) : std::string();
			e->post = e->copied_body.data(); e->post_size = e->copied_body.size(); e->copied_post = true; e->posting = true; break;
		case 60: case 30120:
			if (int64_t(value) < -1 || int64_t(value) > int64_t(BodyLimit)) return BadArgument;
			if (e->copied_post && int64_t(value) > int64_t(e->copied_body.size())) return BadArgument;
			e->post_size = int64_t(value); break;
		case 47: e->posting = value != 0; e->head = false; e->upload = false; break;
		case 46: e->upload = value != 0; e->head = false; e->posting = false; break;
		case 80: if (value) { e->posting = false; e->head = false; e->upload = false; } break;
		case 44: e->head = value != 0; break;
		case 52: e->request.redirect = value != 0; break;
		case 68:
			if (int64_t(value) != -1 && value > UINT32_MAX) return BadArgument;
			e->request.max_redirects = uint32_t(value); break;
		case 42: e->include_header = value != 0; break;
		case 45: e->fail_http = value != 0; break;
		case 43: e->no_progress = value != 0; break;
		case 13: case 155: case 78: case 156: {
			if (int64_t(value) < 0) return BadArgument;
			uint64_t milliseconds = std::min<uint64_t>(value, 0x7fffffff);
			if (option == 13 || option == 78) milliseconds = std::min<uint64_t>(milliseconds * 1000, 0x7fffffff);
			const auto timeout = milliseconds ? uint32_t(milliseconds) : 30000;
			if (option == 78 || option == 156) e->request.connect_timeout_ms = timeout;
			else { e->timeout_ms = milliseconds; e->request.send_timeout_ms = e->request.recv_timeout_ms = timeout; }
			break;
		}
		case 10102:
			if (value && !text().empty() && text() != "gzip" && text() != "deflate" && text() != "gzip, deflate") return Unsupported;
			e->request.decompress = value != 0; break;
		case 10022: e->cookie = text(); break;
		case 10016: e->referer = text(); break;
		case 10004: if (value && !text().empty()) return Unsupported; break;
		case 10100: {
			auto share = value ? Find(shares, reinterpret_cast<void*>(value)) : nullptr;
			if (value && !share) return BadArgument;
			if (e->share) --e->share->users;
			e->share = std::move(share);
			if (e->share) ++e->share->users;
			break;
		}
		// Scheduling/cache hints do not require host/guest callback bridging.
		case 41: case 99: case 71: case 74: case 75: case 91: case 92: case 121: case 150: case 213: case 214: case 215: case 237: break;
		case 113: if (value) return Unsupported; break;
		case 64: if (value != 1) return Unsupported; break; // preserve TLS verification
		case 81: if (value != 2) return Unsupported; break;
		case 84:
			if (value > 4) return Unsupported;
			e->request.http_version = value > 2 ? 3 : int(value); break;
		default:
			std::fprintf(stderr, "RNPS curl: unsupported easy option=%d\n", option);
			return UnknownOption;
	}
	return Ok;
}
int RNPS_CURL_ABI EasyGetinfo(void* handle, int info, void* output) {
	auto e = Find(easies, handle);
	if (!e || !output) return BadArgument;
	switch (info) {
		case 0x100001: *static_cast<const char**>(output) = e->response.effective_url.empty() ? e->request.url.c_str() : e->response.effective_url.c_str(); break;
		case 0x200002: *static_cast<int64_t*>(output) = e->response.status; break;
		case 0x100012: *static_cast<const char**>(output) = e->content_type.empty() ? nullptr : e->content_type.c_str(); break;
		case 0x100015: *static_cast<void**>(output) = e->private_data; break;
		case 0x300003: *static_cast<double*>(output) = e->elapsed; break;
		case 0x300008: *static_cast<double*>(output) = double(e->response.body.size()); break;
		case 0x600008: *static_cast<int64_t*>(output) = e->response.body.size(); break;
		case 0x20000b: *static_cast<int64_t*>(output) = e->response.headers.size(); break;
		default: return UnknownOption;
	}
	return Ok;
}
int RNPS_CURL_ABI EasyPerform(void* handle) {
	auto e = Find(easies, handle);
	if (!e || e->multi || e->delivering) return BadArgument;
	e->done = false;
	e->delivering = true;
	int result = Start(*e);
	if (!result) {
		if (e->timeout_ms && e->future.wait_for(std::chrono::milliseconds(e->timeout_ms)) != std::future_status::ready) result = 28;
		else {
			e->future.wait();
			result = Complete(*e);
		}
	}
	e->delivering = false;
	Finish(*e, result);
	return result;
}
const char* RNPS_CURL_ABI EasyStrerror(int error) {
	switch (error) {
		case 0: return "No error";
		case 3: return "Malformed URL";
		case 4: return "Feature not supported";
		case 6: return "Could not resolve host";
		case 7: return "Could not connect";
		case 22: return "HTTP response indicated failure";
		case 23: return "Write callback failed";
		case 27: return "Out of memory";
		case 28: return "Operation timed out";
		case 42: return "Aborted by callback";
		case 43: return "Bad function argument";
		case 47: return "Too many redirects";
		case 48: return "Unknown option";
		case 60: return "TLS certificate verification failed";
		case 98: return "Client certificate required";
		default: return "Transfer failed";
	}
}
Slist* RNPS_CURL_ABI SlistAppend(Slist* list, const char* text) {
	if (!text) return nullptr;
	auto node = std::unique_ptr<Slist>(new (std::nothrow) Slist {});
	if (!node) return nullptr;
	auto data = std::unique_ptr<char[]>(new (std::nothrow) char[std::strlen(text) + 1]);
	if (!data) return nullptr;
	std::strcpy(data.get(), text);
	node->data = data.release();
	if (!list) return node.release();
	auto* tail = list;
	while (tail->next) tail = tail->next;
	tail->next = node.release();
	return list;
}
void RNPS_CURL_ABI SlistFreeAll(Slist* list) {
	while (list) { auto* next = list->next; delete[] list->data; delete list; list = next; }
}
void* RNPS_CURL_ABI MultiInit() { return Create(multis); }
int RNPS_CURL_ABI MultiCleanup(void* handle) {
	auto m = Find(multis, handle);
	if (!m) return 1;
	if (m->performing) return 8;
	for (auto* h : m->handles) if (auto e = Find(easies, h)) { e->multi = nullptr; e->future = {}; e->done = false; }
	std::lock_guard lock(registry_mutex);
	multis.erase(handle);
	return Ok;
}
int RNPS_CURL_ABI MultiSetopt(void* handle, int option, uint64_t) {
	if (!Find(multis, handle)) return 1;
	// Optional connection scheduling hints and HTTP/2 push notification. This
	// backend does not advertise server push, so no push callback is invoked.
	switch (option) {
		case 3: case 6: case 7: case 13: case 16: case 20014: case 10015: return Ok;
		default: return 6;
	}
}
int RNPS_CURL_ABI MultiAddHandle(void* handle, void* easy) {
	auto m = Find(multis, handle);
	auto e = Find(easies, easy);
	if (!m) return 1;
	if (!e) return 2;
	if (m->performing) return 8;
	if (e->multi) return 7;
	if (e->delivering) return 8;
	e->done = false;
	e->multi = handle;
	m->handles.push_back(easy);
	MultiWakeup(handle);
	return Ok;
}
int RNPS_CURL_ABI MultiRemoveHandle(void* handle, void* easy) {
	auto m = Find(multis, handle);
	auto e = Find(easies, easy);
	if (!m) return 1;
	if (!e) return 2;
	if (m->performing) return 8;
	if (e->delivering) return 8;
	if (e->multi != handle) return e->multi ? 2 : Ok;
	e->multi = nullptr;
	e->future = {}; // worker retains request bytes; it cannot call the guest
	e->done = false;
	m->handles.erase(std::remove(m->handles.begin(), m->handles.end(), easy), m->handles.end());
	m->messages.erase(std::remove_if(m->messages.begin(), m->messages.end(), [easy](const auto& msg) { return msg.easy == easy; }), m->messages.end());
	return Ok;
}
int RNPS_CURL_ABI MultiPerform(void* handle, int* running) {
	auto m = Find(multis, handle);
	if (!m) return 1;
	if (!running) return 10;
	if (m->performing) return 8;
	m->performing = true;
	*running = 0;
	const auto handles = m->handles;
	for (auto* h : handles) {
		auto e = Find(easies, h);
		if (!e || e->multi != handle || e->done) continue;
		int result = 0;
		e->delivering = true;
		if (!e->future.valid()) result = Start(*e);
		if (!result && TimedOut(*e)) result = 28;
		if (!result && !e->no_progress && e->progress && e->progress(e->progress_data, 0, 0, e->uploaded, 0)) result = 42;
		if (!result && !Ready(*e)) { e->delivering = false; ++*running; continue; }
		if (!result) result = Complete(*e);
		Finish(*e, result);
		e->delivering = false;
		Message message {1, h, {nullptr}};
		message.data.result = result;
		m->messages.push_back(message);
	}
	m->performing = false;
	return Ok;
}
Message* RNPS_CURL_ABI MultiInfoRead(void* handle, int* remaining) {
	if (remaining) *remaining = 0;
	auto m = Find(multis, handle);
	if (!m || m->messages.empty()) return nullptr;
	m->last = m->messages.front();
	m->messages.pop_front();
	if (remaining) *remaining = int(m->messages.size());
	return &m->last;
}
int RNPS_CURL_ABI MultiPoll(void* handle, void*, unsigned count, int timeout, int* ready) {
	if (ready) *ready = 0;
	auto m = Find(multis, handle);
	if (!m) return 1;
	if (count || timeout < 0) return 10;
	const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
	std::unique_lock lock(m->wait_mutex);
	for (;;) {
		if (m->awakened) { m->awakened = false; break; }
		bool complete = false;
		for (auto* h : m->handles) if (auto e = Find(easies, h); e && Ready(*e)) { complete = true; break; }
		if (complete || Clock::now() >= deadline) break;
		m->wake.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(10)));
	}
	return Ok;
}
int RNPS_CURL_ABI MultiWakeup(void* handle) {
	auto m = Find(multis, handle);
	if (!m) return 1;
	std::lock_guard lock(m->wait_mutex);
	m->awakened = true;
	m->wake.notify_all();
	return Ok;
}
int RNPS_CURL_ABI MultiTimeout(void* handle, int64_t* timeout) {
	auto m = Find(multis, handle);
	if (!m) return 1;
	if (!timeout) return 10;
	*timeout = -1;
	for (auto* h : m->handles) if (auto e = Find(easies, h); e && !e->done) {
		*timeout = !e->future.valid() || Ready(*e) ? 0 : 10;
		if (!*timeout) break;
	}
	return Ok;
}
int RNPS_CURL_ABI MultiFdset(void* handle, void*, void*, void*, int* maxfd) {
	if (!Find(multis, handle)) return 1;
	if (!maxfd) return 10;
	// There are no guest descriptors. curl adds to (does not clear) caller sets.
	*maxfd = -1;
	return Ok;
}
const char* RNPS_CURL_ABI MultiStrerror(int error) {
	switch (error) {
		case 0: return "No error";
		case 1: return "Invalid multi handle";
		case 2: return "Invalid easy handle";
		case 6: return "Unknown option";
		case 7: return "Easy handle already added";
		case 8: return "Recursive API call";
		case 10: return "Bad function argument";
		default: return "Multi operation failed";
	}
}
void* RNPS_CURL_ABI ShareInit() { return Create(shares); }
int RNPS_CURL_ABI ShareCleanup(void* handle) {
	auto share = Find(shares, handle);
	if (!share) return 3;
	if (share->users) return 2;
	std::lock_guard lock(registry_mutex);
	shares.erase(handle);
	return Ok;
}
int RNPS_CURL_ABI ShareSetopt(void* handle, int option, uint64_t value) {
	auto share = Find(shares, handle);
	if (!share) return 3;
	if (share->users) return 2;
	// DNS/TLS/session/connection caches are owned by WinHTTP; no guest memory is
	// shared between workers. Cookie-engine sharing is not implemented.
	if (option == 1 || option == 2) return value >= 3 && value <= 5 ? 0 : 5;
	if (option >= 3 && option <= 5) return 0;
	return 1;
}
const char* RNPS_CURL_ABI PushheaderBynum(void*, size_t) { return nullptr; }
const char* RNPS_CURL_ABI PushheaderByname(void*, const char*) { return nullptr; }
} // namespace Libs::RnpsCurl
