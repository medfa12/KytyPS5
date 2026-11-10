// Include after NetPoolConfigTests.inc. Parent wires TestNpNativeWorkerHttp in
// a dedicated mode after InitSubsystems, before InitAll. No network requests.
void TestNpNativeWorkerHttp() {
	constexpr const char* name = "NpNativeWorkerHttp";
	namespace Http = Libs::Network::Http;
	namespace Network = Libs::Network;
	NetPoolConfigTest::Api api;
	const auto cookie = api.Resolve<decltype(&Http::HttpSetCookieEnabled)>(
	    "XNUoD2B9a6A", "Http", "Http::HttpSetCookieEnabled");
	Check(name, cookie(-1, 0) == Network::HTTP_ERROR_BEFORE_INIT, "uninitialized HTTP accepted");
	NetPoolConfigTest::Instance instance;
	const int pool = api.create("SceNpMgrWorkerThread", 0x4000, 0);
	Check(name, pool >= 0, "worker pool creation");
	struct Worker {
		int ssl = -1, http = -1, tmpl = -1;
	};
	std::array<Worker, 5> workers;
	struct Cleanup {
		NetPoolConfigTest::Api& api;
		std::array<Worker, 5>& workers;
		int pool;
		~Cleanup() {
			for (auto& worker: workers) {
				if (worker.tmpl >= 0) { Http::HttpDeleteTemplate(worker.tmpl); }
				if (worker.http >= 0) { api.http_term(worker.http); }
				if (worker.ssl >= 0) { api.ssl_term(worker.ssl); }
			}
			api.destroy(pool);
		}
	} cleanup {api, workers, pool};
	for (auto& worker: workers) {
		worker.ssl = api.ssl_init(0x30000);
		Check(name, worker.ssl >= 0, "worker SSL context");
		worker.http = api.http_init(pool, worker.ssl, 0x10000);
		Check(name, worker.http >= 0, "worker HTTP context");
		worker.tmpl = Http::HttpCreateTemplate(worker.http, "NpManager-agent/1.0.0", 2, 0);
		Check(name, worker.tmpl >= 0, "worker template");
		CheckOk(name, cookie(worker.tmpl, 0), "native NP cookie-disable prerequisite");
#if defined(_WIN32)
		CheckOk(name, cookie(worker.tmpl, 1), "native SNS cookie-enable prerequisite");
#else
		Check(name, cookie(worker.tmpl, 1) == Network::HTTP_ERROR_UNKNOWN, "unsupported backend accepted cookies");
#endif
		CheckOk(name, cookie(worker.tmpl, 0), "cookie disable after enable");
		Check(name, cookie(worker.http, 0) == Network::HTTP_ERROR_INVALID_ID,
		      "context incorrectly accepted as template/connection/request");
	}
	const int retired = workers[0].tmpl;
	CheckOk(name, Http::HttpDeleteTemplate(retired), "worker template rollback");
	workers[0].tmpl = -1;
	Check(name, cookie(retired, 0) == Network::HTTP_ERROR_INVALID_ID, "deleted worker accepted");
	for (int invalid: {-1, INT32_MIN, INT32_MAX}) {
		Check(name, cookie(invalid, 0) == Network::HTTP_ERROR_INVALID_ID, "invalid cookie target accepted");
	}
}
