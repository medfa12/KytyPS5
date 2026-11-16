// Exercise the registered factories, not only a routing-policy helper.
void TestAppDbBootRouting() {
	constexpr const char* test = "AppDbBootRouting";
	struct Environment {
		std::array<const char*, 2> names {"KYTY_APPDB_CORE_AUTH_RECORD", "KYTY_APPDB_UI_AUTH_RECORD"};
		std::array<std::optional<std::string>, 2> saved;
		static void Set(const char* name, const char* value) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			_putenv_s(name, value ? value : "");
#else
			if (value) { setenv(name, value, 1); } else { unsetenv(name); }
#endif
		}
		Environment() {
			for (size_t i = 0; i < names.size(); ++i) {
				if (const auto* value = std::getenv(names[i])) { saved[i] = value; }
				Set(names[i], nullptr);
			}
		}
		~Environment() {
			for (size_t i = 0; i < names.size(); ++i) { Set(names[i], saved[i] ? saved[i]->c_str() : nullptr); }
		}
	} environment;
	Check(test, !Libs::AppDbNativeTransport::Installed(), "isolated fixture must start without a native adapter");
	Loader::SymbolDatabase symbols;
	Libs::LibcInternal::InitLibcInternal_1(&symbols);
	using Factory = int (KYTY_SYSV_ABI *)(void**, const void*, void*, void*);
	auto resolve = [&](const char* nid) {
		const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(test, symbol && symbol->vaddr, "registered IPMI factory missing");
		return reinterpret_cast<Factory>(symbol->vaddr);
	};
	const auto client = resolve("0zsTiDhM0nU");
	const auto server = resolve("hoWSsRT0ktA");
	std::array<uint8_t, Libs::IpmiClientAbi::ConfigSize> config {};
	Libs::IpmiClientAbi::Construct(config.data());
	std::memcpy(config.data(), Libs::AppDbNativeTransport::ServiceName, sizeof(Libs::AppDbNativeTransport::ServiceName));
	Libs::AppDbNativeTransport::ServerConfig server_config {};
	std::memcpy(server_config.name, Libs::AppDbNativeTransport::ServiceName, sizeof(Libs::AppDbNativeTransport::ServiceName));
	auto compatibility = [&] {
		void* c = nullptr;
		CheckOk(test, client(&c, config.data(), nullptr, nullptr), "regular boot client factory");
		Check(test, c != nullptr, "regular client object");
		const auto destroy = reinterpret_cast<int64_t (KYTY_SYSV_ABI *)(void*)>((*static_cast<void***>(c))[0]);
		destroy(c);
		void* s = nullptr;
		CheckOk(test, server(&s, &server_config, nullptr, nullptr), "regular boot server factory");
		Check(test, s != nullptr, "regular server object");
		std::free(s);
	};
	compatibility();
	for (const auto* name: environment.names) {
		Environment::Set(name, "fixture-requested-record");
		std::array<uint8_t, Libs::IpmiClientAbi::ClientMemorySize> backing {};
		void* c = nullptr;
		void* s = nullptr;
		Check(test, client(&c, config.data(), nullptr, backing.data()) == static_cast<int>(0x8002004eu) && c == nullptr,
		      "requested native client must fail without provider, never fall back");
		Check(test, server(&s, &server_config, nullptr, backing.data()) == static_cast<int>(0x8002004eu) && s == nullptr,
		      "requested native server must fail without provider, never fall back");
		Environment::Set(name, nullptr);
	}
	compatibility();
}
