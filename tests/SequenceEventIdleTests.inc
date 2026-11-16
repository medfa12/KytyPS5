// Include in VirtualMemoryAllocationTests.cpp's fixture namespace and dispatch
// TestSequenceEventIdle() in a fresh dedicated process after InitSubsystems().
// Uses its existing Libs::JscdNet::InitJscdNet_1 declaration and symbol fixture.
void TestSequenceEventIdle() {
	constexpr const char* test = "SequenceEventIdle";
	constexpr int empty = static_cast<int>(0x80020023u);
	constexpr int missing = static_cast<int>(0x81378012u);
	constexpr int invalid = static_cast<int>(0x81378000u);
	Loader::SymbolDatabase symbols;
	Libs::JscdNet::InitJscdNet_1(&symbols);
	auto resolve = [&](const char* nid) {
		const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(test, symbol != nullptr && symbol->vaddr != 0, "missing sequence-handler export");
		return symbol->vaddr;
	};
	using Init = int(KYTY_SYSV_ABI *)();
	using Receive = int(KYTY_SYSV_ABI *)();
	using Get = int(KYTY_SYSV_ABI *)(void*);
	const auto core_init = reinterpret_cast<Init>(resolve("Fn9dTNR6wMM"));
	const auto core_receive = reinterpret_cast<Receive>(resolve("gQ-7OfXu59I"));
	const auto core_get = reinterpret_cast<Get>(resolve("jLUxdy7mfas"));
	const auto report_init = reinterpret_cast<Init>(resolve("7P8xYWW8YaY"));
	const auto report_receive = reinterpret_cast<Receive>(resolve("Hdtr0jFNOYI"));
	const auto report_get = reinterpret_cast<Get>(resolve("9QRrQtmkp10"));
	Check(test, core_receive() == missing && report_receive() == missing,
	      "uninitialized receive must not manufacture an event");
	Check(test, core_get(nullptr) == invalid && report_get(nullptr) == invalid, "native null getter error");
	Check(test, core_init() == 0 && core_receive() == empty && report_receive() == missing,
	      "independent sequence-handler initialization");
	Check(test, report_init() == 0 && report_receive() == empty, "report queue idle status");
	Check(test, core_init() == missing && report_init() == missing, "native duplicate initialization error");
	std::array<uint8_t, 0x6ac + 2> core;
	std::array<uint8_t, 0x6c0 + 2> report;
	core.fill(0xa5);
	report.fill(0x5a);
	Check(test, core_get(core.data() + 1) == empty && report_get(report.data() + 1) == empty,
	      "cache getters retain failed receive status");
	Check(test, core.front() == 0xa5 && core.back() == 0xa5 && report.front() == 0x5a && report.back() == 0x5a,
	      "native 0x6ac/0x6c0 cache copy sizes preserve adjacent canaries");
	Check(test, std::all_of(core.begin() + 1, core.end() - 1, [](uint8_t b) { return b == 0; }) &&
	            std::all_of(report.begin() + 1, report.end() - 1, [](uint8_t b) { return b == 0; }),
	      "native getter copies the cache even on failed receive");
	unsigned dispatches = 0;
	for (unsigned frame = 0; frame < 1024; ++frame) {
		// Same success gate as both native managed EventMonitor.Update methods.
		if (core_receive() == 0) { ++dispatches; core_get(core.data() + 1); }
		if (report_receive() == 0) { ++dispatches; report_get(report.data() + 1); }
	}
	Check(test, dispatches == 0, "idle frames must not dispatch bogus all-zero crash/report events");
}
