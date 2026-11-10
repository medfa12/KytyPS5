#include "libs/rnpsCurl.h"
#include "libs/hostHttp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <thread>

using namespace Libs::RnpsCurl;
static void Check(bool ok, const char* message) {
	if (!ok) { std::fprintf(stderr, "RnpsCurlTests: %s\n", message); std::exit(1); }
}
struct Sink {
	std::string bytes;
	std::thread::id owner = std::this_thread::get_id();
	bool reject = false;
};
static size_t KYTY_SYSV_ABI Write(char* data, size_t size, size_t count, void* userdata) {
	auto& sink = *static_cast<Sink*>(userdata);
	Check(std::this_thread::get_id() == sink.owner, "guest callback ran on worker thread");
	Check(size == 1, "unexpected curl callback element size");
	if (sink.reject) return 0;
	sink.bytes.append(data, size * count);
	return size * count;
}
static void Set(void* e, int option, uint64_t value) { Check(EasySetopt(e, option, value) == 0, "setopt failed"); }
static void SetUrl(void* e, const std::string& url) { Set(e, 10002, reinterpret_cast<uint64_t>(url.c_str())); }
struct Source { std::string bytes; size_t offset = 0; };
static size_t KYTY_SYSV_ABI Read(char* data, size_t size, size_t count, void* userdata) {
	auto& source = *static_cast<Source*>(userdata);
	auto bytes = std::min(size * count, source.bytes.size() - source.offset);
	std::memcpy(data, source.bytes.data() + source.offset, bytes);
	source.offset += bytes;
	return bytes;
}
static int KYTY_SYSV_ABI Abort(void*, int64_t, int64_t, int64_t, int64_t) { return 1; }
static void* New(const std::string& url, Sink& body, Sink& headers) {
	auto* e = EasyInit();
	Check(e != nullptr, "easy init returned null");
	SetUrl(e, url);
	Set(e, 20011, reinterpret_cast<uint64_t>(&Write));
	Set(e, 10001, reinterpret_cast<uint64_t>(&body));
	Set(e, 20079, reinterpret_cast<uint64_t>(&Write));
	Set(e, 10029, reinterpret_cast<uint64_t>(&headers));
	return e;
}
static Message Drain(void* m) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;) {
		int running = -1;
		Check(MultiPerform(m, &running) == 0, "multi perform failed");
		int pending = -1;
		if (auto* message = MultiInfoRead(m, &pending)) return *message;
		Check(running > 0 && std::chrono::steady_clock::now() < deadline, "missing transfer completion");
		Check(MultiPoll(m, nullptr, 0, 100, nullptr) == 0, "multi poll failed");
	}
}
static void Signal(const std::string& url) {
	Libs::HostHttp::Request request;
	request.url = url;
	Check(Libs::HostHttp::Send(request).status == 200, "fixture coordination failed");
}
int main(int argc, char** argv) {
	if (argc == 3 && std::strcmp(argv[1], "--probe") == 0) {
		_putenv_s("KYTY_NETWORK_HOST", "1");
		Sink body, headers;
		auto* e = New(argv[2], body, headers);
		Set(e, 155, 20000);
		Set(e, 52, 1);
		Set(e, 10102, reinterpret_cast<uint64_t>(""));
		const auto result = EasyPerform(e);
		int64_t status = 0;
		EasyGetinfo(e, 0x200002, &status);
		std::printf("RNPS HTTPS probe: curl=%d status=%lld bytes=%zu\n", result, static_cast<long long>(status), body.bytes.size());
		EasyCleanup(e);
		return result || !status ? 1 : 0;
	}
	Check(argc == 2, "expected loopback fixture URL");
	const std::string base = argv[1];
	_putenv_s("KYTY_NETWORK_HOST", "1");
	Check(GlobalInit(3) == 0, "global init");
	Sink body, headers;
	auto* e = New(base, body, headers);
	const std::string post("hello\0guest", 11);
	Set(e, 60, post.size());
	Set(e, 10165, reinterpret_cast<uint64_t>(post.data()));
	auto* list = SlistAppend(nullptr, "X-Kyty-Test: curl-guest");
	Check(list && SlistAppend(list, "X-Second: header") == list && list->next, "slist append ABI");
	Set(e, 10023, reinterpret_cast<uint64_t>(list));
	Check(EasyPerform(e) == 0 && body.bytes == post, "binary POST/callback delivery");
	Check(headers.bytes.find("X-Echo: curl-guest") != std::string::npos, "header propagation");
	Check(headers.bytes.find('\0') == std::string::npos, "spurious NUL in headers");
	int64_t status = -1;
	Check(EasyGetinfo(e, 0x200002, &status) == 0 && status == 201, "LP64 response code");
	Set(e, 10103, 0x1234567887654321ULL);
	void* private_data = nullptr;
	Check(EasyGetinfo(e, 0x100015, &private_data) == 0 && uintptr_t(private_data) == 0x1234567887654321ULL, "private pointer truncated");
	Set(e, 10023, 0);
	SlistFreeAll(list);
	Set(e, 80, 1);
	body.bytes.clear(); headers.bytes.clear();
	SetUrl(e, base + "redirect");
	Set(e, 52, 1);
	Check(EasyPerform(e) == 0 && body.bytes == "ok", "redirect request failed");
	const char* final_url = nullptr;
	Check(EasyGetinfo(e, 0x100001, &final_url) == 0 && std::string(final_url) == base + "ok", "effective redirect URL");
	const char* type = nullptr;
	Check(EasyGetinfo(e, 0x100012, &type) == 0 && type && std::string(type) == "text/plain", "content type");
	SetUrl(e, base + "gzip");
	Set(e, 10102, reinterpret_cast<uint64_t>(""));
	body.bytes.clear();
	Check(EasyPerform(e) == 0 && body.bytes == "compressed response", "gzip response not decoded");
	Set(e, 10102, 0);
	Check(EasySetopt(e, 999999, 0) == 48, "unsupported option silently succeeded");
	SetUrl(e, base + "missing");
	body.bytes.clear();
	Check(EasyPerform(e) == 0 && body.bytes == "missing", "HTTP 404 became transport failure");
	Set(e, 45, 1);
	char error[256] {};
	Set(e, 10010, reinterpret_cast<uint64_t>(error));
	Check(EasyPerform(e) == 22 && error[0], "FAILONERROR/error buffer");
	Set(e, 45, 0);
	SetUrl(e, base);
	body.reject = true;
	Check(EasyPerform(e) == 23, "short write callback incorrectly succeeded");
	body.reject = false;
	Source source {post};
	Set(e, 46, 1);
	Set(e, 10036, reinterpret_cast<uint64_t>("POST"));
	Set(e, 30115, post.size());
	Set(e, 10009, reinterpret_cast<uint64_t>(&source));
	Set(e, 20012, reinterpret_cast<uint64_t>(&Read));
	body.bytes.clear();
	Check(EasyPerform(e) == 0 && body.bytes == post && source.offset == post.size(), "native read-callback upload");
	Set(e, 80, 1);
	Set(e, 10036, 0);
	Set(e, 43, 0);
	Set(e, 20219, reinterpret_cast<uint64_t>(&Abort));
	Check(EasyPerform(e) == 42, "progress callback did not abort");
	Set(e, 43, 1);
	Set(e, 20219, 0);

	auto* share = ShareInit();
	Set(e, 10100, reinterpret_cast<uint64_t>(share));
	Check(ShareCleanup(share) == 2, "in-use share was destroyed");
	Set(e, 10100, 0);
	Check(ShareCleanup(share) == 0, "share cleanup");

	auto* m = MultiInit();
	Check(m != nullptr && MultiAddHandle(m, e) == 0 && MultiAddHandle(m, e) == 7, "multi membership");
	int64_t timeout = -999;
	Check(MultiTimeout(m, &timeout) == 0 && timeout == 0, "LP64 immediate timeout");
	auto message = Drain(m);
	Check(message.message == 1 && message.easy == e && message.data.result == 0, "CURLMsg ABI/completion");
	int remaining = -1;
	Check(!MultiInfoRead(m, &remaining) && remaining == 0, "completion delivered twice");
	Check(MultiRemoveHandle(m, e) == 0, "remove completed handle");
	Check(MultiTimeout(m, &timeout) == 0 && timeout == -1, "LP64 idle timeout");

	// The server holds the response until after removal and cleanup. A worker
	// retaining a guest callback or userdata would now violate this assertion.
	Sink held_body, held_headers;
	auto* held = New(base + "held", held_body, held_headers);
	Check(MultiAddHandle(m, held) == 0, "add held transfer");
	int running = -1;
	Check(MultiPerform(m, &running) == 0 && running == 1, "held transfer not pending");
	Signal(base + "arrived");
	Check(MultiRemoveHandle(m, held) == 0, "cancel pending transfer");
	EasyCleanup(held);
	Signal(base + "release");
	Check(MultiPerform(m, &running) == 0 && running == 0 && held_body.bytes.empty(), "cancelled callback delivered");
	Check(!MultiInfoRead(m, &remaining), "cancelled completion queued");
	Check(EasySetopt(held, 52, 1) == 43, "stale handle accepted");
	SetUrl(e, base + "timeout");
	Set(e, 155, 30);
	Check(MultiAddHandle(m, e) == 0 && Drain(m).data.result == 28, "total timeout did not complete");
	Check(MultiRemoveHandle(m, e) == 0, "remove timed out handle");
	Set(e, 155, 0);
	SetUrl(e, base);

	// Cross-thread wakeup may interrupt poll without invoking any guest callback.
	MultiPoll(m, nullptr, 0, 0, nullptr); // consume prior add wakeup
	std::promise<void> started;
	auto ready = started.get_future();
	auto polling = std::async(std::launch::async, [&] { started.set_value(); return MultiPoll(m, nullptr, 0, 5000, nullptr); });
	ready.wait();
	Check(MultiWakeup(m) == 0 && polling.wait_for(std::chrono::seconds(1)) == std::future_status::ready && polling.get() == 0, "cross-thread wakeup failed");

	_putenv_s("KYTY_NETWORK_HOST", "0");
	Check(MultiAddHandle(m, e) == 0, "add disabled request");
	message = Drain(m);
	Check(message.data.result != 0, "disabled host network fabricated success");
	Check(MultiCleanup(m) == 0, "cleanup attached multi");
	EasyCleanup(e);
	Check(MultiWakeup(m) == 1, "destroyed multi remained live");
	std::puts("RnpsCurlTests: real loopback HTTP, guest ABI, callbacks, completion, cancellation and wakeup passed");
}
