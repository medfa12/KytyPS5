#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_

#include "common/abi.h"
#include "common/hostException.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "loader/timer.h" // IWYU pragma: keep

#include <fmt/format.h>

#include <filesystem>
#include <string>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLED g_print_name

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLE(flag) PRINT_NAME_ENABLED = flag;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_DEFINE(name) void name(Loader::SymbolDatabase* s)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_NAME(l, m)                                                                             \
	[[maybe_unused]] static thread_local bool PRINT_NAME_ENABLED = false;                          \
	static constexpr char                     g_library[]        = l;                              \
	static constexpr char                     g_module[]         = m;
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_VERSION(l, lv, m, mv1, mv2)                                                            \
	LIB_NAME(l, m);                                                                                \
	static constexpr int g_library_version      = lv;                                              \
	static constexpr int g_module_version_major = mv1;                                             \
	static constexpr int g_module_version_minor = mv2;
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_ADD(n, f, t)                                                                           \
	{                                                                                              \
		Loader::SymbolResolve sr {};                                                               \
		sr.name                 = n;                                                               \
		sr.library              = g_library;                                                       \
		sr.library_version      = g_library_version;                                               \
		sr.module               = g_module;                                                        \
		sr.module_version_major = g_module_version_major;                                          \
		sr.module_version_minor = g_module_version_minor;                                          \
		sr.type                 = t;                                                               \
		auto        func        = reinterpret_cast<uint64_t>(f);                                   \
		const char* dbg_name    = "" #f;                                                           \
		s->Add(sr, func, dbg_name);                                                                \
	}
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_OBJECT(n, f) LIB_ADD(n, f, Loader::SymbolType::Object)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC(n, f) LIB_ADD(n, f, Loader::SymbolType::Func)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME()                                                                               \
	if (PRINT_NAME_ENABLED) {                                                                      \
		Libs::PrintName(g_library, g_module, __func__);                                              \
	}

namespace Loader {
class SymbolDatabase;
} // namespace Loader

namespace Libs {

void NoteHleCall(const char* library, const char* module, const char* function);
void NoteHleReturn();
void TraceHostThread(uint64_t tid);

// ps5-vsh: guest boot flags mirrored from argv (run_shellui-style:
// -NoForegroundUser, -OfflineDatabases). HLE reads these to match the
// guest's expected environment instead of assuming defaults.
void SetNoForegroundUser(bool v);
bool NoForegroundUser();

// ps5-vsh: true when booting a title directly (--launch-title), i.e. the
// only guest is the title with no ShellCore launcher context. Title-scoped
// workarounds (assumed appId, exists-count stat) must ONLY apply in this
// mode; the chain boot keeps exact legacy semantics.
void SetDirectTitleBoot(bool v);
bool IsDirectTitleBoot();

namespace UserService {
// ps5-vsh stage 2 (KYTY_USER_STORE): store-mode entry points for other Libs
// TUs (implemented in libUserService.cpp by forwarding to the stage-1
// statics). No guest NIDs; the Mbus login/logout HLEs in libC.cpp call these
// only after UserServiceStoreEnabled(). Mutex discipline lives in the
// implementation; callers hold no locks.
bool UserServiceStoreEnabled();
// Call in store mode with non-null outputs; registered users requires 20 ints
// (13.60: up to 16 home users plus 4 guests).
int  UserServiceStoreGetRegisteredUsers(int* user_ids);
int  UserServiceInstallNativeBackend(const void* table);
int  UserServiceRetireNativeBackend();
int  UserServiceStoreGetForegroundUser(int* user_id);
int  UserServiceStoreLogin(int user_id);
int  UserServiceStoreLogout(int user_id);
} // namespace UserService

namespace FsSandbox {
// Host backing for an otherwise-unmounted guest path (per-boot random-word
// sandboxes like /<hex>/priv/... can't be pre-mounted). Empty when the guest
// path is unsuitable (relative, escapes, overlong).
std::filesystem::path BackingForUnmounted(const std::string& guest);
// ps5-vsh chainSUI187: create-intent variant. O_CREAT opens must land
// somewhere writable (HW creates); existence probes (stat/open w/o
// O_CREAT) keep exact ENOENT semantics via the default overload.
std::filesystem::path BackingForUnmounted(const std::string& guest, bool create);
// Operation-scoped variant: /data has already been selected from the caller's
// cwd/mount/scope snapshot. Empty retains the existing _SaveData/title fallback.
std::filesystem::path BackingForUnmounted(const std::string& guest, bool create,
                                         const std::filesystem::path& captured_data_root);
// Ensure a valid (empty) sqlite DB exists at the jail backing for license DB
// paths (priv/license/*.db). The entitlement daemon stats/opens these before
// any CREATE; without a seed they stay ENOENT and its CSI aborts.
void EnsureLicenseSeed(const std::string& guest);
void EnsureLicenseSeed(const std::string& guest, const std::filesystem::path& captured_data_root);
} // namespace FsSandbox

// ps5-vsh: slow-call guard for blocking HLEs (sema/flag/cond waits). One
// line per site; logs waiter + object + blocked time when a single call
// blocks >5s (boot deadlock signature). Negligible cost (two QPC reads).
struct HleSlowCallGuard {
	const char* func;
	const char* what;
	uint64_t    t0;
	HleSlowCallGuard(const char* func_, const char* what_ = nullptr);
	~HleSlowCallGuard();
};
#define HLE_SLOW_GUARD(what_) \
	const HleSlowCallGuard _hle_slow_guard_(__func__, (what_))

// Keep the formatting path from inflating fiber functions' stack frames under LTO.
[[gnu::noinline]] inline void PrintName(const char* library, const char* module, const char* function) {
	if (Log::GetDirection() != Log::Direction::Silent) {
		const auto elapsed_ms      = static_cast<uint64_t>(Loader::Timer::GetTimeMs());
		const auto print_name_time = fmt::format(
		    "{:02}:{:02}:{:02}.{:03}", elapsed_ms / 3600000, (elapsed_ms / 60000) % 60,
		    (elapsed_ms / 1000) % 60, elapsed_ms % 1000);
		LOGF_COLOR(Log::Color::Cyan, "[%d][%s] %s::%s::%s()\n",
		           Common::Thread::GetThreadIdUnique(), print_name_time.c_str(), library, module,
		           function);
	}
	NoteHleCall(library, module, function);
}

void InitAll(Loader::SymbolDatabase* s);

namespace LibKernel {
extern uint64_t g_psm_ini_struct_addr;
// ps5-vsh chainSUI229: dispatch a guest null-deref fault to the registered
// SIGSEGV handler (mono relies on fault-driven null checks).
// Returns true when the handler redirected execution; false declines.
bool Ps5VshTryGuestSigsegv(const Common::HostException::ExceptionInfo& info);
} // namespace LibKernel

namespace LibcInternal {
// Platform nullfs binding used by the co-hosted SysCore bootstrap.
void BindSysCoreServiceSandbox();
// posix_memalign (tracked backing shared with Memalign; plain free() unwraps
// through the aligned-block registry). Defined in libC.cpp.
int KYTY_SYSV_ABI PosixMemalign(void** memptr, size_t alignment, size_t size);
void* MakeIpmiClient(const void* config = nullptr, void* storage = nullptr);
int KYTY_SYSV_ABI SceIpmiCreateClient(void** out, const void* config = nullptr, void* mem1 = nullptr, void* mem2 = nullptr);
int KYTY_SYSV_ABI IpmiServerCreate(void** out, const void* config = nullptr, void* a2 = nullptr, void* a3 = nullptr);
// ps5-vsh TEMP-DIAG (chainSUI98): crash-time dump of the mmap-out arg ring.
void Ps5VshDumpMmapOut();
void Ps5VshDumpStackWatch();
void Ps5VshWatchStack(uint64_t tag);
// ps5-vsh: true while a guarded host copy runs on this thread. The
// top-level fault handler must decline (return false) so SEH truncates.
bool Ps5VshInGuardedCopy();
// ps5-vsh: image-struct watchpoint (0 when disarmed).
uint64_t Ps5VshImageGuardPage();
uint64_t Ps5VshIniGuardPage();
void     Ps5VshSetIniGuardPage(uint64_t page);
void     Ps5VshArmIniGuard(uint64_t addr);
uint64_t Ps5VshNoteGuardHit();
void     Ps5VshRearmImageGuard();
// ps5-vsh: lock-free image-touch ring (VEH-safe); flushed from HLE context.
void     Ps5VshRecordImageTouch(uint64_t pc, uint64_t addr, uint8_t what);
uint64_t Ps5VshTouchCount();
void     Ps5VshDumpTouches();
void     Ps5VshDumpTouchesSince(uint64_t since);
} // namespace LibcInternal

namespace ShellCoreFloor {
// ps5-vsh chainSUI188: address of the fake streambuf object handed to
// guests as cerr/cout rdbuf (0 when ios never initialized).
uint64_t Ps5VshFakeBufAddr();
} // namespace ShellCoreFloor

} // namespace Libs

// ps5-vsh chainSUI235: expect-ud2 flag for the abort redirect. Set by
// the abort/stack_chk HLE survive paths; consumed by the VEH redirect.
// Declared here (global ::Loader) so TUs inside namespace Libs still
// resolve ::Loader instead of a nested intruder.
namespace Loader {
void Ps5VshExpectUd2Redirect(uint64_t ud2addr);
} // namespace Loader
#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_ */
