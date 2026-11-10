namespace Libs::Composite {
void InitComposite_1(Loader::SymbolDatabase* symbols);
}

void TestCompositeMemoryPools() {
	constexpr const char* test = "CompositeMemoryPools";
	Loader::SymbolDatabase symbols;
	Libs::Composite::InitComposite_1(&symbols);
	auto address = [&](const char* nid) {
		const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(test, symbol != nullptr, "native memory-pool export registered");
		return symbol ? symbol->vaddr : uint64_t(0);
	};
	using Init = int(KYTY_SYSV_ABI *)(uint32_t, uint32_t, uint32_t, uint8_t);
	using Get = uint64_t(KYTY_SYSV_ABI *)();
	using Quit = int(KYTY_SYSV_ABI *)(uint64_t, uint64_t, uint64_t, uint64_t);
	const auto init = reinterpret_cast<Init>(address("IUlpGnuoR1c"));
	const auto quit = reinterpret_cast<Quit>(address("y5IJdPxdBrE"));
	const Get bases[] = {reinterpret_cast<Get>(address("T6CVkdCDO7o")),
	                     reinterpret_cast<Get>(address("bxt+muwit0w")),
	                     reinterpret_cast<Get>(address("ReHQrFnGVxA"))};
	const Get sizes[] = {reinterpret_cast<Get>(address("N6ID0KNnzY8")),
	                     reinterpret_cast<Get>(address("FTQCTDU0b4g")),
	                     reinterpret_cast<Get>(address("5x5tC7zLwq4"))};
	if (!init || !quit || !bases[0] || !bases[1] || !bases[2] || !sizes[0] || !sizes[1] || !sizes[2]) { return; }
	for (size_t i = 0; i < 3; ++i) { Check(test, bases[i]() == 0 && sizes[i]() == 0, "no fabricated arena before init"); }
	Check(test, init(0x400000, 0xe00000, 0x4000, 4) == static_cast<int>(0x80d40006u), "native invalid order error");
	Check(test, init(0x20000001, 0, 0, 2) == static_cast<int>(0x80d40005u), "native arena-size limit");
	Check(test, init(0x400000, 0xe00000, 0x4000, 2) == 0, "observed PSM sizes allocate actual arenas");
	const uint64_t expected[] = {0x400000, 0xe00000, 0x4000};
	uint64_t original[3] {};
	for (size_t i = 0; i < 3; ++i) {
		original[i] = bases[i]();
		Check(test, original[i] != 0 && (original[i] & 0x3fff) == 0 && sizes[i]() == expected[i],
		      "no-argument 64-bit getters return distinct bounded page-aligned backing");
		if (original[i] && sizes[i]() == expected[i]) {
			auto* bytes = reinterpret_cast<uint8_t*>(original[i]);
			Check(test, bytes[0] == 0 && bytes[expected[i] - 1] == 0, "actual backing spans reported extent");
			bytes[0] = static_cast<uint8_t>(i + 1); bytes[expected[i] - 1] = 0xa5;
		}
	}
	Check(test, original[0] != original[1] && original[1] != original[2] && original[0] != original[2], "arenas do not alias");
	Check(test, init(0x4000, 0x4000, 0, 0) == static_cast<int>(0x80d40003u), "duplicate init preserves live backing");
	for (size_t i = 0; i < 3; ++i) { Check(test, bases[i]() == original[i] && sizes[i]() == expected[i], "rejected init preserves getters"); }
	Check(test, quit(0, 0, 0, 0) == 0, "quit retires backing");
	for (size_t i = 0; i < 3; ++i) { Check(test, bases[i]() == 0 && sizes[i]() == 0, "quit clears exported arenas"); }
	Check(test, init(0x4000, 0, 0, 1) == 0 && bases[0]() != 0 && bases[1]() == 0 && sizes[1]() == 0,
	      "reinitialization supports absent optional arena without inventing pixels");
	quit(0, 0, 0, 0);
}
