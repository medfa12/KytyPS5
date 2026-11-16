void TestGuestVsnprintf() {
	constexpr const char* test = "GuestVsnprintf";
	Loader::SymbolDatabase symbols;
	Libs::LibcInternal::InitLibcInternal_1(&symbols);
	const auto* symbol = symbols.Find({"Q2V+iqvjgC0", "LibcInternal", 1, "LibcInternal", 1, 1, Loader::SymbolType::Func});
	Check(test, symbol != nullptr, "resolve actual vsnprintf export");
	const auto print = reinterpret_cast<int(KYTY_SYSV_ABI *)(char*, size_t, const char*, Libs::VaList*)>(symbol->vaddr);
	alignas(16) Libs::VaRegSave registers {};
	registers.gp[2] = reinterpret_cast<uint64_t>("native");
	registers.gp[3] = UINT64_C(1234567890123);
	registers.gp[4] = 11;
	registers.gp[5] = 22;
	const double decimal = 1.5;
	std::memcpy(&registers.fp[0], &decimal, sizeof(decimal));
	std::array<uint64_t, 2> overflow {33, reinterpret_cast<uint64_t>("tail")};
	Libs::VaList args {16, 48, overflow.data(), &registers};
	const auto original = args;
	const auto original_registers = registers;
	const auto original_overflow = overflow;
	constexpr char format[] = "[%s] %llu %.1f %d %d %d %s";
	constexpr char expected[] = "[native] 1234567890123 1.5 11 22 33 tail";
	auto unchanged = [&] {
		Check(test, std::memcmp(&args, &original, sizeof(args)) == 0 &&
		            std::memcmp(&registers, &original_registers, sizeof(registers)) == 0 && overflow == original_overflow,
		      "query/write preserve GP, FP and overflow cursor and backing arguments");
	};
	Check(test, print(nullptr, 0, format, &args) == sizeof(expected) - 1, "Esvm size-query pass");
	unchanged();
	std::array<char, 96> output;
	output.fill('!');
	Check(test, print(output.data() + 1, sizeof(expected), format, &args) == sizeof(expected) - 1 &&
	            std::strcmp(output.data() + 1, expected) == 0 && output.front() == '!' && output[sizeof(expected) + 1] == '!',
	      "reuse same cursor for full write after size query");
	unchanged();
	output.fill('!');
	Check(test, print(output.data() + 1, 8, format, &args) == sizeof(expected) - 1 &&
	            std::memcmp(output.data() + 1, expected, 7) == 0 && output[8] == 0 && output.front() == '!' && output[9] == '!',
	      "truncation still returns full length and preserves canaries");
	unchanged();
}

void TestGuestFloatingPointFormatting() {
	constexpr const char* test = "GuestFloatingPointFormatting";
	Loader::SymbolDatabase symbols;
	Libs::LibcInternal::InitLibcInternal_1(&symbols);
	const auto* symbol = symbols.Find({"Q2V+iqvjgC0", "LibcInternal", 1, "LibcInternal", 1, 1, Loader::SymbolType::Func});
	Check(test, symbol != nullptr, "resolve actual vsnprintf export");
	const auto print = reinterpret_cast<int(KYTY_SYSV_ABI *)(char*, size_t, const char*, Libs::VaList*)>(symbol->vaddr);
	struct Case { const char* format; double value; const char* expected; };
	const auto* direct_symbol = symbols.Find({"eLdDw6l0-bU", "LibcInternal", 1, "LibcInternal", 1, 1, Loader::SymbolType::Func});
	Check(test, direct_symbol != nullptr, "resolve snprintf used by monosgen DoubleToString");
	const auto direct_print = reinterpret_cast<int(KYTY_SYSV_ABI *)(char*, size_t, const char*, ...)>(direct_symbol->vaddr);
	// Regular 13.60 mscorlib Number.DoubleToNumber calls Sys.DoubleToString
	// with %.40e and a 50-byte buffer. The old reverse 32-byte mantissa
	// buffer dropped its leading digits, turning popup dimensions into zero.
	const Case cases[] = {
	    {"%.40e", 928.0, "9.2800000000000000000000000000000000000000e+02"},
	    {"%.40e", 1456.0, "1.4560000000000000000000000000000000000000e+03"},
	    {"%.40e", -0.125, "-1.2500000000000000000000000000000000000000e-01"},
	    {"%.40e", 0.0, "0.0000000000000000000000000000000000000000e+00"},
	    {"%.40e", -0.0, "-0.0000000000000000000000000000000000000000e+00"},
	    {"%.40f", 1.25, "1.2500000000000000000000000000000000000000"},
	    {"%.17g", 1.2345678901234567, "1.2345678901234567"},
	    {"%#.4g", 1.0, "1.000"},
	    {"%+012.2f", 928.0, "+00000928.00"},
	    {"%-12.2f", 928.0, "928.00      "},
	    {"%.0f", 2.5, "2"},
	    {"%.0f", 3.5, "4"},
	    {"%.2E", 1456.0, "1.46E+03"},
	};
	for (const auto& item: cases) {
		alignas(16) Libs::VaRegSave registers {};
		std::memcpy(&registers.fp[0], &item.value, sizeof(item.value));
		Libs::VaList args {0, 48, nullptr, &registers};
		const auto original = args;
		const auto length = static_cast<int>(std::strlen(item.expected));
		std::array<char, 64> output;
		output.fill('!');
		Check(test, print(nullptr, 0, item.format, &args) == length, "floating-point size query");
		Check(test, print(output.data() + 1, 50, item.format, &args) == length &&
		            std::strcmp(output.data() + 1, item.expected) == 0 && output.front() == '!' && output[51] == '!',
		      "native DoubleToNumber precision and ordinary formatting");
		output.fill('!');
		Check(test, print(output.data() + 1, 8, item.format, &args) == length &&
		            std::memcmp(output.data() + 1, item.expected, (std::min)(size_t {7}, std::strlen(item.expected))) == 0 &&
		            output[(std::min)(size_t {7}, std::strlen(item.expected)) + 1] == 0 && output[9] == '!',
		      "floating-point truncation preserves full length and canaries");
		Check(test, std::memcmp(&args, &original, sizeof(args)) == 0, "floating-point cursor preserved");
		output.fill('!');
		Check(test, direct_print(output.data() + 1, 50, item.format, item.value) == length &&
		            std::strcmp(output.data() + 1, item.expected) == 0 && output.front() == '!' && output[51] == '!',
		      "native SysV variadic snprintf preserves double in XMM0");
	}
}
