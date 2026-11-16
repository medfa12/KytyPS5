// Offline production UserService transition/callback fixture. Synthetic profile
// and retained boot-role caller only; no console firmware executes here.
#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/libLncUtil.h"
#include "libs/libs.h"
#include "libs/runtimeUserService.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"
#include "loader/runtimeProcessIdentity.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Libs { void InitUserService_1(Loader::SymbolDatabase*); }
namespace Libs::JscdUser { void InitJscdUser_1(Loader::SymbolDatabase*); }
namespace Libs::LibKernel::Memory { void InvalidateMemory(uint64_t, uint64_t) {} }
static void Check(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
struct Event { int type; int user; };
struct State {
	Loader::Program* owner;
	int caller_tid;
	std::vector<Event> events;
	bool valid = true;
};
static uint64_t KYTY_SYSV_ABI Callback(const Event* event, State* state) {
	int foreground = -1;
	state->valid &= Loader::RuntimeLinker::GetCurrentProgram() == state->owner &&
	    Libs::LibKernel::PthreadGetOwnerProgram(Libs::LibKernel::PthreadSelfOrNull()) == state->owner &&
	    Libs::LibKernel::PthreadGetthreadid() != state->caller_tid &&
	    Libs::UserService::UserServiceStoreGetForegroundUser(&foreground) == 0;
	state->valid &= event->type == 0 ? foreground == event->user : foreground == -1;
	state->events.push_back(*event);
	return 0;
}
static uint64_t Resolve(Loader::SymbolDatabase& symbols, const char* nid) {
	const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
	Check(symbol && symbol->vaddr, "retail local/test UserService session exports must resolve");
	return symbol->vaddr;
}
int main(int argc, char** argv) try {
	if (argc != 3) { return 2; }
	const bool enabled = std::string_view(argv[2]) == "on";
	const std::filesystem::path root = argv[1];
	Check(!std::filesystem::exists(root), "fixture root must be new");
	for (const char* suffix: {"user/home", "sd/priv/home", "sd/eap/rodata/home"}) {
		std::filesystem::create_directories(root / suffix / "10010029");
	}
	const auto store = root / "users.json";
	std::ofstream(store) << R"({"version":1,"next_user_number":43,"users":[{"id":268501033,"name":"Fixture","auto_login":true,"account_id":0,"last_login_order":1}]})";
	Common::VirtualMemory::Init();
	Common::InitializeThreads();
	Common::Subsystems systems;
	systems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	options.guest_environment["KYTY_USER_STORE"] = store.string();
	options.guest_environment["KYTY_NP_LOCAL_ACCOUNT_HLE"] = "1";
	Config::Load(options);
	systems.Initialize<Log::Lifecycle>();
	systems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	namespace FS = Libs::LibKernel::FileSystem;
	FS::Initialize();
	FS::Mount(root / "user", "/user");
	FS::Mount(root / "sd", "/system_data");
	Loader::SymbolDatabase symbols, callbacks;
	Libs::InitUserService_1(&symbols);
	Libs::JscdUser::InitJscdUser_1(&callbacks);
	Check(!symbols.FindByNid("AT6NySjQ2gY", Loader::SymbolType::Func) &&
	      !symbols.FindByNid("REIRIXzv-H0", Loader::SymbolType::Func), "native LoginMgr creation/destruction remain untouched");
	if (!enabled) {
		Check(!symbols.FindByNid("uvVR70ZxFrQ", Loader::SymbolType::Func), "default-off native-credential behavior retained");
		std::cout << "default-off selection preserves native LoginMgr and prior UserService routing\n";
		return 0;
	}
	using Login = int (KYTY_SYSV_ABI *)(int, int64_t);
	using Logout = int (KYTY_SYSV_ABI *)(int, int);
	using IsLoggedIn = int (KYTY_SYSV_ABI *)(int);
	using Change = int (KYTY_SYSV_ABI *)(uint64_t, uint64_t);
	using Initialize = int (KYTY_SYSV_ABI *)(const void*);
	using Terminate = int (KYTY_SYSV_ABI *)();
	using GetNumber = int (KYTY_SYSV_ABI *)(int, int32_t*);
	using GetEvent = int (KYTY_SYSV_ABI *)(Event*);
	const auto login = reinterpret_cast<Login>(Resolve(symbols, "uvVR70ZxFrQ"));
	const auto logout = reinterpret_cast<Logout>(Resolve(symbols, "3T9y5xDcfOk"));
	const auto logged = reinterpret_cast<IsLoggedIn>(Resolve(symbols, "MZxH8029+Wg"));
	const auto initialize = reinterpret_cast<Initialize>(Resolve(symbols, "j3YMu1MVNNo"));
	const auto terminate = reinterpret_cast<Terminate>(Resolve(symbols, "bwFjS+bX9mA"));
	const auto number = reinterpret_cast<GetNumber>(Resolve(symbols, "qbwy0Ub8b3M"));
	const auto event = reinterpret_cast<GetEvent>(Resolve(symbols, "yH17Q6NWtVg"));
	const auto add = reinterpret_cast<Change>(Resolve(callbacks, "wuI7c7UNk0A"));
	const auto remove = reinterpret_cast<Change>(Resolve(callbacks, "spW--yoLQ9o"));
	Check(login(0x10010029, -1) == Libs::UserService::USER_SERVICE_ERROR_NOT_INITIALIZED, "unretained caller rejected");
	// Synthetic retained role identity, not a substitute for source-bound
	// admission verification. Host auth-record variables select comparison mode
	// but their files are not consumed by this synthetic-role fixture.
	auto* owner = new Loader::Program;
	owner->unique_id = 792123;
	owner->file_name = "NPXS40087/eboot.bin";
	const auto param = root / "boot.json";
	std::ofstream(param) << R"({"titleId":"NPXS40087","applicationCategoryType":65536})";
	Loader::RuntimeLinker::SetCurrentProgram(owner);
	Loader::SystemContentLoadParamSfo(param);
	Libs::LncUtil::Lnc::RegisterProgramLaunch(owner, param, true, owner->unique_id);
	Check(Loader::ConfigureBootProcessIdentity(owner), "retained boot-role identity");
	Libs::LibKernel::Initialize();
	Libs::LibKernel::PthreadInitSelfForMainThread();
	(void)Common::Singleton<Loader::RuntimeLinker>::Instance();
	const auto elf_path = root / "callback.elf";
	{
		Loader::Elf64_Ehdr header {};
		std::memcpy(header.e_ident, "\177ELF", 4);
		header.e_ident[Loader::EI_CLASS] = Loader::ELFCLASS64;
		header.e_ident[Loader::EI_DATA] = Loader::ELFDATA2LSB;
		header.e_ident[Loader::EI_VERSION] = Loader::EV_CURRENT;
		header.e_ident[Loader::EI_OSABI] = Loader::ELFOSABI_FREEBSD;
		header.e_type = Loader::ET_DYNAMIC;
		header.e_machine = Loader::EM_X86_64;
		header.e_version = Loader::EV_CURRENT;
		header.e_ehsize = sizeof(header);
		header.e_phoff = sizeof(header);
		header.e_phentsize = sizeof(Loader::Elf64_Phdr);
		header.e_phnum = 1;
		Loader::Elf64_Phdr segment {};
		segment.p_type = Loader::PT_LOAD;
		segment.p_flags = Loader::PF_R | Loader::PF_X;
		segment.p_vaddr = reinterpret_cast<uint64_t>(&Callback);
		segment.p_filesz = segment.p_memsz = 1;
		std::ofstream file(elf_path, std::ios::binary);
		file.write(reinterpret_cast<const char*>(&header), sizeof(header));
		file.write(reinterpret_cast<const char*>(&segment), sizeof(segment));
	}
	owner->elf = std::make_unique<Loader::Elf64>();
	owner->elf->Open(elf_path);
	Check(initialize(nullptr) == 0 && logged(0x10010029) == 0, "profile loading never fabricates login");
	State state {owner, Libs::LibKernel::PthreadGetthreadid()};
	const auto target = reinterpret_cast<uint64_t>(&Callback);
	const auto userdata = reinterpret_cast<uint64_t>(&state);
	Check(add(1, 0) == Libs::UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT, "callback must belong to caller executable segment");
	Check(add(target, userdata) == 0 && add(target, userdata) == 0, "real callback registration and pair deduplication");
	Check(login(0x10010030, -1) == Libs::UserService::USER_SERVICE_ERROR_NOT_LOGGED_IN && state.events.empty(), "missing profile cannot log in");
	Check(login(0x10010029, 7) == Libs::UserService::USER_SERVICE_ERROR_OPERATION_NOT_SUPPORTED && state.events.empty(), "unsupported device cannot fabricate login");
	Check(login(0x10010029, -1) == 0, "production session login");
	Check(state.valid && state.events.size() == 1 && state.events[0].type == 0 && state.events[0].user == 0x10010029,
	      "login callback completes before successful login returns");
	int32_t slot = -1;
	Check(logged(0x10010029) == 1 && number(0x10010029, &slot) == 0 && slot == 1, "committed session predicate and stable slot");
	Event polled {-77, -88};
	Check(event(&polled) == 0 && polled.type == 0 && polled.user == 0x10010029, "poll consumers receive committed login identity");
	Check(login(0x10010029, -1) == 0 && state.events.size() == 1, "idempotent login does not duplicate event");
	polled = {-77, -88};
	Check(event(&polled) == Libs::UserService::USER_SERVICE_ERROR_NO_EVENT && polled.type == -77 && polled.user == -88, "idempotent login preserves empty poll output");
	Check(logout(0x10010029, 0) == 0 && state.valid && state.events.size() == 2 && state.events[1].type == 1 && logged(0x10010029) == 0,
	      "logout commits and acknowledges callback");
	Check(event(&polled) == 0 && polled.type == 1 && polled.user == 0x10010029, "poll consumers receive logout");
	// A real persistence failure cannot report successful login or deliver events.
	const auto saved_store = root / "saved-users.json";
	std::filesystem::rename(store, saved_store);
	std::filesystem::create_directory(store);
	Check(login(0x10010029, -1) == Libs::UserService::USER_SERVICE_ERROR_INTERNAL && logged(0x10010029) == 0 && state.events.size() == 2,
	      "failed profile save preserves logged-out state and callbacks");
	Check(event(&polled) == Libs::UserService::USER_SERVICE_ERROR_NO_EVENT, "failed login publishes no poll event");
	std::filesystem::remove(store);
	std::filesystem::rename(saved_store, store);
	Check(remove(target, userdata) == 0 && terminate() == 0, "callback removal and worker retirement");
	std::cout << "native LoginMgr retained; local/test session login/logout, ownership, real callback acknowledgment and slots passed\n";
	return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
