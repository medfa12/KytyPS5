void TestNpAtomicIncrement() {
	constexpr const char* name = "NpAtomicIncrement";
	Loader::SymbolDatabase symbols;
	Libs::JscdSmall::InitJscdSmall_1(&symbols);
	Libs::OfflineLogin::InitOfflineLogin_1(&symbols);
	Libs::LibNpCommon::InitNpCommon_1(&symbols);
	Libs::ShellHits::InitShellHits_1(&symbols);
	const auto* exact = symbols.Find({"pfJgSA4jO3M", "NpCommon", 1, "NpCommon", 1, 1, Loader::SymbolType::Func});
	const auto* fallback = symbols.FindByNid("pfJgSA4jO3M", Loader::SymbolType::Func);
	Check(name, exact != nullptr && fallback != nullptr && exact->vaddr == fallback->vaddr,
	      "atomic increment exact/fallback binding");
	using Increment = uint32_t (KYTY_SYSV_ABI *)(uint32_t*);
	auto increment = reinterpret_cast<Increment>(exact->vaddr);
	struct { uint32_t before = 0x12345678, count = UINT32_MAX, after = 0x87654321; } slot;
	Check(name, increment(&slot.count) == UINT32_MAX && slot.count == 0,
	      "first trophy schedule must observe old negative value and wrap the stored count");
	Check(name, increment(&slot.count) == 0 && slot.count == 1, "increment must return old value");
	Check(name, slot.before == 0x12345678 && slot.after == 0x87654321, "increment writes exactly four bytes");
	slot.count = 0;
	std::array<std::array<uint32_t, 64>, 4> observed {};
	std::array<std::thread, 4> workers;
	for (size_t worker = 0; worker < workers.size(); ++worker) {
		workers[worker] = std::thread([&, worker] {
			for (auto& value: observed[worker]) { value = increment(&slot.count); }
		});
	}
	for (auto& worker: workers) { worker.join(); }
	std::array<bool, 256> seen {};
	for (const auto& worker: observed) {
		for (const auto value: worker) {
			Check(name, value < seen.size() && !seen[value], "concurrent increments must return unique old counts");
			seen[value] = true;
		}
	}
	Check(name, slot.count == 256 && slot.before == 0x12345678 && slot.after == 0x87654321,
	      "atomic increments lost updates or changed adjacent fields");
}
