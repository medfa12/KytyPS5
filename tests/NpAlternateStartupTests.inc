// Real-input, mapping-only test; dispatched in its own process.
void TestNpAlternateStartup() {
	constexpr const char* test = "NpAlternateStartup";
	namespace N = Loader::NpAlternateStartup;
	const char* alternate_env = std::getenv("KYTY_TEST_NP_ALTERNATE_IMAGE");
	const char* original_env = std::getenv("KYTY_TEST_NP_ORIGINAL_IMAGE");
	Check(test, alternate_env && original_env, "both reviewed image paths required");
	if (!alternate_env || !original_env) { return; }
	const std::filesystem::path alternate(alternate_env), original(original_env);
	struct Environment {
		std::array<const char*, 6> keys {"KYTY_NATIVE_NP_ALTERNATE_STARTUP", "KYTY_NATIVE_NP_OWNERSHIP",
		    "KYTY_NATIVE_NP_WORKQUEUE", "KYTY_NATIVE_NP_CALLOUT", "KYTY_NATIVE_NP_HTTP_POOL", "KYTY_NATIVE_NP_DISPATCHER"};
		std::array<std::string, 6> saved;
		static void Set(const char* key, const char* value) {
#if defined(_WIN32)
			_putenv_s(key, value);
#else
			setenv(key, value, 1);
#endif
		}
		Environment() {
			for (size_t i = 0; i < keys.size(); ++i) { saved[i] = std::string(N::Env(keys[i])); Set(keys[i], ""); }
		}
		~Environment() { for (size_t i = 0; i < keys.size(); ++i) { Set(keys[i], saved[i].c_str()); } }
	} environment;
	const auto root = std::filesystem::temp_directory_path() /
	    ("kyty-np-alternate-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
	} cleanup {root};
	const auto copy = root / "libSceNpCommon.sprx";
	std::filesystem::copy_file(alternate, copy);
	std::vector<uint8_t> expected;
	{
		Loader::Elf64 image;
		image.Open(copy);
		Check(test, image.SnapshotPlainFile(N::ImageSize, &expected), "real image snapshot");
		Check(test, !image.FreezePlainFile(N::OriginalHash, N::ImageSize), "alternate rejected by original pin");
		Check(test, image.FreezePlainFile(N::ImageHash, N::ImageSize), "exact alternate frozen");
		// Change mapped executable backing bytes after admission. LoadSegment must
		// still read retained bytes, not the open file or a reopened pathname.
		const auto offset = image.GetPhdr()[0].p_offset + 0x100;
		{
			std::fstream out(copy, std::ios::binary | std::ios::in | std::ios::out);
			out.seekp(static_cast<std::streamoff>(offset));
			const char changed = static_cast<char>(expected[offset] ^ 1);
			out.write(&changed, 1); out.flush();
			Check(test, out.good(), "mutate temporary backing only");
		}
		std::array<uint8_t, 32> mapped {};
		image.LoadSegment(reinterpret_cast<uint64_t>(mapped.data()), offset, mapped.size());
		Check(test, std::memcmp(mapped.data(), expected.data() + offset, mapped.size()) == 0,
		      "frozen segment immune to post-admission backing mutation");
		Loader::Elf64 changed;
		changed.Open(copy);
		Check(test, !changed.FreezePlainFile(N::ImageHash, N::ImageSize), "same-size changed executable rejected");
	}
	Libs::LibKernel::Initialize();
	Libs::LibKernel::PthreadInitSelfForMainThread();
	auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Environment::Set(environment.keys[0], "1");
	Check(test, linker->LoadProgram(copy) == nullptr, "production loader wrong hash rejection");
	Check(test, linker->LoadProgram(original) == nullptr, "original cannot substitute for alternate");
	Environment::Set(environment.keys[0], "true");
	Check(test, linker->LoadProgram(alternate) == nullptr, "malformed alternate rejected by loader");
	Environment::Set(environment.keys[0], "1");
	Check(test, linker->LoadProgram(alternate, true) == nullptr, "application-owned NP provider rejected");
	Environment::Set(environment.keys[1], "1");
	Check(test, linker->LoadProgram(alternate) == nullptr, "partial family rejected by loader");
	Environment::Set(environment.keys[0], "");
	Check(test, linker->LoadProgram(alternate) == nullptr, "ownership-only cannot bypass alternate gate");
	Environment::Set(environment.keys[1], "");
	Environment::Set(environment.keys[0], "1");
	Environment::Set(environment.keys[5], "1");
	Check(test, linker->LoadProgram(alternate) == nullptr, "dispatcher conflict rejected by loader");
	Environment::Set(environment.keys[5], "");
	const auto renamed = root / "renamed.sprx";
	std::filesystem::copy_file(alternate, renamed);
	Check(test, linker->LoadProgram(renamed) == nullptr, "export metadata prevents renamed provider bypass");
	auto* provider = linker->LoadProgram(alternate);
	Check(test, provider && provider->np_alternate54 && !provider->boot_credential_admission,
	      "verified alternate provider grants no credentials");
	if (provider) {
		Check(test, provider->export_symbols->GetExportPolicy() == Loader::SymbolDatabase::ExportPolicy::NpHttpPool,
		      "production provider selects exact54 policy");
		auto visible = [&](std::string_view nid) {
			return provider->export_symbols->FindByNid(std::string(nid), Loader::SymbolType::Func) != nullptr;
		};
		for (const auto nid: Loader::NpOwnershipExports) { Check(test, visible(nid), "ownership export present"); }
		for (const auto nid: Loader::NpWorkQueueExports) { Check(test, visible(nid), "queue export present"); }
		for (const auto nid: Loader::NpCalloutExports) { Check(test, visible(nid), "callout export present"); }
		for (const auto nid: Loader::NpHttpPoolExports) { Check(test, visible(nid), "pool export present"); }
		Check(test, !visible("fhJ5uKzcn0w") && !visible("EjMsfO3GCIA"), "dispatcher56 excluded");
		Check(test, linker->LoadProgram(alternate) == nullptr, "duplicate provider rejected");
	}
	linker->Clear();
	// Exercise the production original-provider path, not just the hash helper.
	// No alternate selection: both ordinary and legacy54 loads must still map.
	Environment::Set(environment.keys[0], "");
	std::vector<uint8_t> original_bytes;
	{
		Loader::Elf64 image;
		image.Open(original);
		Check(test, image.SnapshotPlainFile(N::ImageSize, &original_bytes), "original reference bytes");
	}
	for (const bool legacy54: {false, true}) {
		for (size_t i = 1; i < 5; ++i) { Environment::Set(environment.keys[i], legacy54 ? "1" : ""); }
		provider = linker->LoadProgram(original);
		Check(test, provider && provider->base_vaddr && provider->base_size && !provider->np_alternate54 &&
		      !provider->application_owner && !provider->boot_credential_admission,
		      "alternate OFF: original production provider admitted and mapped without credentials");
		if (provider) {
			Check(test, provider->export_symbols && provider->export_symbols->GetExportPolicy() ==
			      (legacy54 ? Loader::SymbolDatabase::ExportPolicy::NpHttpPool : Loader::SymbolDatabase::ExportPolicy::All),
			      "alternate OFF: original ordinary/legacy54 policy preserved");
			bool checked_bytes = false;
			const auto* ph = provider->elf->GetPhdr();
			for (size_t i = 0; i < provider->elf->GetEhdr()->e_phnum; ++i) {
				const auto& segment = ph[i];
				// Read-only data has no host instruction adaptation. LoadProgram
				// has not relocated or executed the provider at this point.
				if (segment.p_type != Loader::PT_LOAD || segment.p_flags != Loader::PF_R || segment.p_filesz < 32) { continue; }
				if (segment.p_offset <= original_bytes.size() && original_bytes.size() - segment.p_offset >= 32) {
					Check(test, std::memcmp(reinterpret_cast<const void*>(provider->base_vaddr + segment.p_vaddr),
					                        original_bytes.data() + segment.p_offset, 32) == 0,
					      "original mapped read-only bytes match reviewed source");
					checked_bytes = true;
				}
				break;
			}
			Check(test, checked_bytes, "original mapped-byte assertion exercised");
			if (provider->export_symbols) {
				Check(test, provider->export_symbols->FindByNid("V75N47uYdQc", Loader::SymbolType::Func) != nullptr,
				      "original allocator export published");
				Check(test, (provider->export_symbols->FindByNid("fhJ5uKzcn0w", Loader::SymbolType::Func) != nullptr) == !legacy54,
				      "original ordinary exports remain unfiltered; legacy54 excludes dispatcher");
			}
			Check(test, linker->LoadProgram(original) == nullptr, "original duplicate provider intentionally rejected");
		}
		linker->Clear();
	}
	// A co-module inherits its application's ownership even when loaded with
	// new_application=false. It must not become another session NP provider.
	Loader::Program private_owner;
	private_owner.application_owner = &private_owner;
	auto* previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::RuntimeLinker::SetCurrentProgram(&private_owner);
	Check(test, linker->LoadProgram(original) == nullptr, "original private co-provider intentionally rejected");
	Environment::Set(environment.keys[0], "1");
	Check(test, linker->LoadProgram(alternate) == nullptr, "alternate private co-provider rejected");
	Loader::RuntimeLinker::SetCurrentProgram(previous);
}
