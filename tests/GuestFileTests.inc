void TestGuestFileFamilies() {
	constexpr const char* test = "GuestFileFamilies";
	namespace GF = Libs::GuestFile;
	namespace C = Libs::LibcInternal;
	namespace FS = Libs::LibKernel::FileSystem;
	const bool previous_mode = Libs::IsDirectTitleBoot();
	auto* previous_program = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::Program program;
	program.unique_id = 740002;
	program.file_name = "guest-file-fixture.elf";
	Loader::RuntimeLinker::SetCurrentProgram(&program);
	const auto root = std::filesystem::temp_directory_path() /
	                  ("kyty_guest_file_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root);
	FS::Initialize();
	FS::Mount(root, "/guest-file-test");
	const std::array<uint8_t, 8> payload {0, 0xff, '\r', '\n', '%', 1, 2, 3};
	for (bool direct : {false, true}) {
		Libs::SetDirectTitleBoot(direct);
		for (bool magic_family : {true, false}) {
			const std::string name = std::string(direct ? "direct-" : "chain-") + (magic_family ? "magic" : "libc");
			const auto guest = "/guest-file-test/" + name;
			void* stream = magic_family ? GF::Open(guest.c_str(), 0x102) : C::GuestFopen(guest.c_str(), "w+b");
			Check(test, stream != nullptr, "open test stream");
			if (!stream) { continue; }
			Check(test, C::GuestFwrite(payload.data(), 2, 4, stream) == 4, "cross-family binary write item count");
			Check(test, C::GuestFtell(stream) == 8, "cross-family position after write");
			Check(test, C::fflush(static_cast<FILE*>(stream)) == 0, "cross-family flush");
			Check(test, C::GuestFseek(stream, 0, SEEK_SET) == 0, "cross-family seek and diagnostic layout");
			std::array<uint8_t, 8> actual {};
			Check(test, C::GuestFread(actual.data(), 2, 4, stream) == 4 && actual == payload, "cross-family read bytes");
			Check(test, C::GuestFclose(stream) == 0, "close through correct owner");
			Check(test, GF::OwnShimHost(stream) == nullptr && GF::RegisteredForeignShimHost(stream) == nullptr,
			      "closed stream removed from registries");
			Check(test, C::GuestFclose(stream) == -1, "closed pointer cannot be deleted twice");
			std::ifstream saved(root / name, std::ios::binary);
			actual.fill(0);
			saved.read(reinterpret_cast<char*>(actual.data()), actual.size());
			Check(test, saved.gcount() == payload.size() && actual == payload && saved.peek() == EOF,
			      "actual persisted file contains exact payload");
		}
	}
	// A committed pointer or forged magic is not proof of ownership. Even an
	// otherwise valid host FILE in its first word must not be unwrapped/deleted.
	FILE* host = std::tmpfile();
	Check(test, host != nullptr, "create foreign-stream sentinel");
	if (host) {
		std::array<uint64_t, 16> forged {};
		for (uint64_t word : {uint64_t(0x4746494C4553484Dull), reinterpret_cast<uint64_t>(host)}) {
			forged[0] = word;
			forged[1] = reinterpret_cast<uint64_t>(host);
			Check(test, GF::OwnShimHost(forged.data()) == nullptr && GF::ShimPath(forged.data()) == nullptr,
			      "unregistered magic/FILE pointer rejected");
			uint8_t byte = 0x7f;
			Check(test, C::GuestFread(&byte, 1, 1, forged.data()) == 0 && byte == 0x7f,
			      "foreign read does not touch destination");
			Check(test, C::GuestFclose(forged.data()) == -1, "foreign close rejected");
		}
		Check(test, std::fputc('x', host) == 'x', "foreign host stream remains usable");
		std::fclose(host);
	}
	Check(test, GF::OwnShimHost(reinterpret_cast<void*>(1)) == nullptr && C::GuestFclose(reinterpret_cast<void*>(1)) == -1,
	      "unmapped pointer rejected without probing");
	FS::Shutdown();
	Loader::RuntimeLinker::SetCurrentProgram(previous_program);
	Libs::SetDirectTitleBoot(previous_mode);
	std::filesystem::remove_all(root);
}
