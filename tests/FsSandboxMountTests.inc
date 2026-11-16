// Dedicated fresh-process test mode: run before any InitAll/InitFsSandbox call.
// Parent declares Libs::FsSandbox::InitFsSandbox_1(Loader::SymbolDatabase*) and
// supplies the usual filesystem/Program fixture headers. No firmware is used.
namespace FsSandboxMountTest {
namespace Fs = Libs::LibKernel::FileSystem;
constexpr const char* Name = "FsSandboxMounts";
constexpr std::array<uint8_t, 8> Sentinel {0x61, 0x63, 0x74, 0xff, 0, 0x12, 0x34, 0x56};

struct Fixture {
	Loader::Program* previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::Program program;
	std::filesystem::path root;
	Fixture() {
		program.unique_id = 740001;
		program.file_name = "sandbox-mount-fixture.elf";
		root = std::filesystem::temp_directory_path() /
		       ("kyty_sandbox_mounts_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Check(Name, std::filesystem::create_directories(root / "data"), "create temporary /data");
		Loader::RuntimeLinker::SetCurrentProgram(&program);
		Fs::Initialize();
		Fs::Mount(root / "data", "/data"); // Keeps fallback writes inside this fixture.
	}
	~Fixture() {
		Fs::Shutdown();
		Loader::RuntimeLinker::SetCurrentProgram(previous);
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}
	std::filesystem::path Mirror() const { return root / "data" / "savedata" / "mirror"; }
	void Select(const char* guest, const std::filesystem::path& relative, bool read_only = false) const {
		std::filesystem::create_directories(root / relative);
		Fs::Mount(root / relative, guest, read_only);
	}
	void CheckPath(const char* guest, const std::filesystem::path& relative) const {
		const auto actual = Common::FixDirectorySlash(Common::PathToGenericString(Fs::GetRealFilename(guest).lexically_normal()));
		const auto expected = Common::FixDirectorySlash(Common::PathToGenericString((root / relative).lexically_normal()));
		Check(Name, actual == expected,
		      std::string("selected mapping replaced: ") + guest);
	}
	void Seed(const std::filesystem::path& relative) const {
		std::filesystem::create_directories((root / relative).parent_path());
		Common::File file;
		Check(Name, file.Create(root / relative), "create isolated persistent sentinel");
		uint32_t written = 0;
		file.Write(Sentinel.data(), static_cast<uint32_t>(Sentinel.size()), &written);
		Check(Name, written == Sentinel.size(), "write persistent sentinel");
		file.Close();
	}
	void CheckSentinel(const std::filesystem::path& relative) const {
		Common::File file;
		Check(Name, file.Open(root / relative, Common::File::Mode::Read), "open persistent sentinel");
		std::array<uint8_t, Sentinel.size()> bytes {};
		uint32_t read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		Check(Name, file.Size() == Sentinel.size() && read == bytes.size() && bytes == Sentinel,
		      "sandbox initialization modified persisted bytes");
		file.Close();
	}
};
} // namespace FsSandboxMountTest

void TestFsSandboxMounts() {
	using namespace FsSandboxMountTest;
	{
		Fixture fixture;
		fixture.Select("/user", "selected/user", true);
		fixture.Select("/system_data", "selected/system_data");
		fixture.Select("/original_user", "selected/original_user");
		fixture.Select("/user/home", "submounts/home");
		fixture.Select("/system_data/priv", "submounts/priv", true);
		fixture.Select("/original_user/local_sandbox2", "submounts/local_sandbox2");
		fixture.Seed("selected/user/license/act.dat");
		fixture.Seed("selected/system_data/settings/persisted.bin");
		fixture.Seed("submounts/priv/mms/persisted.db");
		fixture.Seed("submounts/home/persisted.bin");
		fixture.Seed("submounts/local_sandbox2/local_sandbox_info.json");
		Loader::SymbolDatabase symbols;
		for (unsigned pass = 0; pass < 2; ++pass) {
			Libs::FsSandbox::InitFsSandbox_1(&symbols);
			fixture.CheckPath("/data", "data");
			fixture.CheckPath("/user", "selected/user");
			fixture.CheckPath("/user/license/act.dat", "selected/user/license/act.dat");
			fixture.CheckPath("/system_data", "selected/system_data");
			fixture.CheckPath("/system_data/settings/persisted.bin", "selected/system_data/settings/persisted.bin");
			fixture.CheckPath("/original_user", "selected/original_user");
			fixture.CheckPath("/original_user/local_sandbox", "selected/original_user/local_sandbox");
			fixture.CheckPath("/user/home/persisted.bin", "submounts/home/persisted.bin");
			fixture.CheckPath("/system_data/priv/mms/persisted.db", "submounts/priv/mms/persisted.db");
			fixture.CheckPath("/original_user/local_sandbox2/local_sandbox_info.json",
			                  "submounts/local_sandbox2/local_sandbox_info.json");
			Check(Name, Fs::IsReadOnly("/user") && Fs::IsReadOnly("/user/license/act.dat") &&
			            !Fs::IsReadOnly("/user/home/persisted.bin") &&
			            !Fs::IsReadOnly("/system_data/settings/persisted.bin") &&
			            Fs::IsReadOnly("/system_data/priv/mms/persisted.db"), "selected mount RO flags changed");
			Check(Name, Fs::KernelOpen("/user/license/act.dat", 0x601, 0770) == Libs::LibKernel::KERNEL_ERROR_EROFS,
			      "selected read-only /user no longer rejects truncation");
			for (const char* path: {"selected/user/license/act.dat", "selected/system_data/settings/persisted.bin",
			                       "submounts/priv/mms/persisted.db", "submounts/home/persisted.bin",
			                       "submounts/local_sandbox2/local_sandbox_info.json"}) {
				fixture.CheckSentinel(path);
			}
			Check(Name, !std::filesystem::exists(fixture.Mirror() / "user") &&
			            !std::filesystem::exists(fixture.Mirror() / "system_data") &&
			            !std::filesystem::exists(fixture.Mirror() / "original_user"),
			      "resolved guest paths caused unused shared-mirror skeleton/seeds");
			Check(Name, !std::filesystem::exists(fixture.root / "selected/user/TRASH") &&
			            !std::filesystem::exists(fixture.root / "selected/system_data/TRASH") &&
			            !std::filesystem::exists(fixture.root / "selected/original_user/local_sandbox") &&
			            !std::filesystem::exists(fixture.root / "submounts/local_sandbox2/tmp_sb"),
			      "fallback seeding leaked into a selected backing");
		}
	}
	{
		// The seed once-flag is intentionally already consumed. A new filesystem
		// still needs unresolved root mounts; selected child mounts must survive.
		Fixture fixture;
		fixture.Select("/user/home", "selected-home", true);
		fixture.Seed("selected-home/persisted.bin");
		Check(Name, Fs::GetRealFilename("/user").empty() && Fs::GetRealFilename("/system_data").empty() &&
		            Fs::GetRealFilename("/original_user").empty(), "fallback fixture roots already resolve");
		Loader::SymbolDatabase symbols;
		Libs::FsSandbox::InitFsSandbox_1(&symbols);
		for (const char* root: {"user", "system_data", "original_user"}) {
			const std::string guest = std::string("/") + root;
			const auto relative = std::filesystem::path("data/savedata/mirror") / root;
			fixture.CheckPath(guest.c_str(), relative);
			fixture.CheckPath((guest + "/new-child").c_str(), relative / "new-child");
			Check(Name, std::filesystem::is_directory(fixture.Mirror() / root), "unresolved root was not backed");
		}
		fixture.CheckPath("/user/home/persisted.bin", "selected-home/persisted.bin");
		Check(Name, Fs::IsReadOnly("/user/home/persisted.bin"), "fallback root replaced child mount flags");
		fixture.CheckSentinel("selected-home/persisted.bin");
	}
}
