#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "kernel/guestFsContext.h"
#include "loader/authorityMetadata.h"
#include "loader/bootCredentialAdmission.h"
#include "loader/processMetadata.h"
#include "loader/runtimeGuestExecution.h"
#include "loader/runtimeNpManagerClient.h"
#include "loader/nativeLibcStartup.h"
#include "loader/nativeSslSmoke.h"
#include "loader/symbolDatabase.h"

#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Loader {

struct EntryParams {
	int         argc;
	uint32_t    pad;
	const char* argv[1];
};

class Elf64;
struct Elf64_Phdr;
struct Elf64_Sym;
struct Elf64_Rela;
class RuntimeLinker;
struct SourceBoundNpTransportEpoch;
class ApplicationImportBindings;
class ApplicationImportData;
struct ApplicationHeapTraceInfo;
enum class ApplicationSanitizerReplacement;

// Neither policy permits firmware providers or native kernel autoload.
enum class AppLocalProgramPolicy { NeededOnly, AdjacentAndNeeded };

using module_func_t                          = KYTY_SYSV_ABI int (*)(size_t args, const void* argp);
using application_heap_free_func_t           = KYTY_SYSV_ABI void (*)(void*);
using application_heap_malloc_func_t         = KYTY_SYSV_ABI void* (*)(uint64_t);
using application_heap_posix_memalign_func_t = KYTY_SYSV_ABI int (*)(void**, uint64_t, uint64_t);

struct ModuleId {
	bool operator==(const ModuleId& other) const {
		return version_major == other.version_major && version_minor == other.version_minor &&
		       name == other.name;
	}

	std::string id;
	int         version_major;
	int         version_minor;
	std::string name;
};

struct LibraryId {
	bool operator==(const LibraryId& other) const {
		return version == other.version && name == other.name;
	}

	std::string id;
	int         version;
	std::string name;
};

struct ThreadLocalStorage {
	struct Block {
		uint8_t*                     ptr        = nullptr;
		application_heap_free_func_t free_func  = nullptr;
		bool                         vm_alloc   = false;
		uint64_t                     alloc_size = 0;
	};

	~ThreadLocalStorage();

	uint64_t image_vaddr   = 0;
	uint64_t init_size     = 0;
	uint64_t image_size    = 0;
	uint64_t tcb_offset    = 0;
	uint64_t handler_vaddr = 0;

	std::vector<uint8_t>           init_image;
	std::unordered_map<int, Block> tlss;
	Common::Mutex                  mutex;
};

struct DynamicInfo {
	void*    hash_table      = nullptr;
	uint64_t hash_table_size = 0;

	char*    str_table      = nullptr;
	uint64_t str_table_size = 0;

	Elf64_Sym* symbol_table            = nullptr;
	uint64_t   symbol_table_total_size = 0;
	uint64_t   symbol_table_entry_size = 0;

	uint64_t init_vaddr          = 0;
	uint64_t fini_vaddr          = 0;
	uint64_t init_array_vaddr    = 0;
	uint64_t fini_array_vaddr    = 0;
	uint64_t preinit_array_vaddr = 0;
	uint64_t init_array_size     = 0;
	uint64_t fini_array_size     = 0;
	uint64_t preinit_array_size  = 0;

	Elf64_Rela* jmprela_table      = nullptr;
	uint64_t    jmprela_table_size = 0;

	Elf64_Rela* rela_table            = nullptr;
	uint64_t    rela_table_total_size = 0;
	uint64_t    rela_table_entry_size = 0;

	uint64_t relative_count = 0;

	uint64_t debug   = 0;
	uint64_t textrel = 0;
	uint64_t flags   = 0;

	const char* so_name = nullptr;
	std::vector<const char*> needed;
	bool needed_valid = true;

	std::vector<ModuleId>  export_modules;
	std::vector<ModuleId>  import_modules;
	std::vector<LibraryId> export_libs;
	std::vector<LibraryId> import_libs;
};

enum class ModuleExecutionState { NotStarted, Starting, Started, Stopping, Stopped, Failed };
enum class ModuleCallStatus { Returned, ThreadExited, Aborted, Stopped, Unavailable, Busy, Failed };
struct ModuleCallResult {
	ModuleCallStatus status = ModuleCallStatus::Unavailable;
	int value = 0; // Initializer/finalizer output, meaningful ONLY for Returned.
	RuntimeGuestResult guest;
	bool Returned() const noexcept { return status == ModuleCallStatus::Returned; }
	bool ThreadExited() const noexcept { return status == ModuleCallStatus::ThreadExited && guest.ThreadExited(); }
};

struct Program {
	Program();
	~Program();

	int32_t                      unique_id = -1;
	RuntimeLinker*               rt        = nullptr;
	// Runtime VSH applications own their executable and private modules. Null
	// retains the existing boot-session/direct-game symbol namespace.
	Program*                     application_owner = nullptr;
	std::shared_ptr<RuntimeGuestExecution> application_execution; // Only on the explicit application owner.
	// Native UserService backend for this boot incarnation; UserService mutex.
	std::shared_ptr<RuntimeGuestExecution> user_service_execution;
	uint64_t user_service_property_getter = 0;
	application_heap_malloc_func_t application_heap_malloc = nullptr;
	application_heap_free_func_t application_heap_free = nullptr;
	application_heap_posix_memalign_func_t application_heap_posix_memalign = nullptr;
	std::atomic_uint32_t application_threads {0};
	std::atomic_int application_exit_code {0};
	std::atomic_bool application_exit_requested {false};
	std::unique_ptr<ApplicationImportBindings> application_import_bindings;
	std::unique_ptr<ApplicationImportData> application_import_data; // Executable owner only; pinned RW guest objects.
	uint32_t                     load_count = 0; // Successful sceKernelLoadStartModule calls.
	std::filesystem::path        file_name;
	std::unique_ptr<Elf64>       elf;
	std::unique_ptr<DynamicInfo> dynamic_info;
	uint64_t                     base_vaddr        = 0;
	uint64_t                     base_size         = 0;
	uint64_t                     base_size_aligned = 0;
	uint64_t                     mapped_size       = 0;
	uint64_t                     flexible_size     = 0;
	uint64_t instruction_trampoline_size = 0;
	std::unique_ptr<SymbolDatabase> export_symbols;
	std::unique_ptr<SymbolDatabase> import_symbols;
	ThreadLocalStorage              tls;
	bool                            fail_if_global_not_resolved = true;
	bool                            dbg_print_reloc             = false;
	std::vector<uint8_t>            rela_bits;
	std::atomic_bool                started                     {false};
	// Module lifecycle; never hold this mutex during guest execution.
	GuestExecutionMutex             module_mutex;
	std::condition_variable_any     module_changed;
	ModuleExecutionState            module_state = ModuleExecutionState::NotStarted;
	std::thread::id                 module_thread;
	ModuleCallResult                module_start_result;
	std::atomic_bool                initializer_returned        {false};
	std::atomic_int                 initializer_result          {0};
	bool                            native_ssl_smoke_dependency = false;
	uint64_t                        proc_param_vaddr            = 0;
	uint32_t                        compiled_sdk_version        = 0; // Executable metadata snapshot, not firmware SDK.
	ProcessAbi                      process_abi                 = ProcessAbi::Unknown;
	uint32_t                        prospero_compiled_sdk_version = 0; // Separate executable +0x14 snapshot.
	std::optional<AuthorityMetadata> authority_metadata; // Assigned before publication; immutable during execution.
	// Cohost identity lifetime is not extended by registry/receipt references.
	std::shared_ptr<BootCredentialLifetime> boot_credential_lifetime = std::make_shared<BootCredentialLifetime>();
	std::shared_ptr<BootCredentialAdmission> boot_credential_admission;
	bool np_alternate54 = false; // Verified immutable provider, not a credential.
};

// ps5-vsh: one co-hosted ELF entry to run on its own guest thread after
// relocation + module start (see --co-elf). argv[0] is the file name.
struct CoElfLaunch {
	uint64_t                 entry = 0;
	std::string              argv0;
	std::vector<std::string> args;
	// Owning program for per-thread TLS scoping (voldemort parity:
	// SetCurrentProgram on thread entry). Set by the loader; null = main.
	Program* program = nullptr;
	Libs::LibKernel::FileSystem::GuestFsContextPtr fs_context;
};

class RuntimeLinker {
public:
	RuntimeLinker();
	virtual ~RuntimeLinker();
	void Clear();

	KYTY_CLASS_NO_COPY(RuntimeLinker);

	void DbgDump(const std::string& folder);

	Program* LoadProgram(const std::filesystem::path& elf_name, bool new_application = false);
	// Map-only app-local dependency closure. Explicit executable/root, owner-scoped
	// identity reuse; no firmware autoload, relocation or guest initializer calls.
	bool PrepareAppLocalPrograms(Program* executable, const std::filesystem::path& root,
	                             AppLocalProgramPolicy policy, std::vector<Program*>* prepared, std::string* error);
	std::vector<Program*> GetVisiblePrograms(const Program* requester);
	// Positive guest errno only, never success: owner-local absent native
	// libkernel mapping => ENOENT, candidate/ambiguity => ENOSYS, unavailable
	// active/current process => ESRCH. No output access, mapping or autoload.
	int QueryKernelTextLocationError();
	std::vector<Program*> GetApplicationStartupModules(const Program* owner);
	std::vector<std::string> GetUnresolvedApplicationImports(const Program* owner);
	std::vector<std::string> GetApplicationImportBlockers(const Program* owner);
	bool PublishApplicationStopText(const Program* owner);
	bool IsApplicationExecutableAddress(const Program* owner, uint64_t address);
	std::vector<std::string> GetApplicationHardImportBlockers(const Program* owner);
	int ResolveApplicationDlsym(Program* handle, const std::string& symbol, const std::string& nid, void** address);
	// Host loader path shared by relocation, hard checking and dynamic lookup.
	// Never returns a raw HLE object; optional/unsupported probes return zero.
	uint64_t ResolveApplicationImportData(Program* importer, const std::string& raw_name,
	    const SymbolRecord& resolved, int64_t addend = 0, uint64_t declared_size = 0);
	// Current-owner, stop-serialized query result; never falls back to session
	// data. The calling HLE returns normally through its host return gateway.
	uint64_t GetApplicationSanitizerReplacement(ApplicationSanitizerReplacement kind);
	// Registered guest heap bootstrap only. Validate/copy before stop-serialized
	// publication; failure returns normally to the matching host-call gateway.
	bool SetApplicationHeapApiFromGuest(void* const api[10]);
	bool WriteApplicationHeapTraceInfo(ApplicationHeapTraceInfo* output);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	static bool TestApplicationSymbolScope(std::string* failure);
	static bool TestProgramFileReuse(std::string* failure);
	static bool TestAppLocalPrograms(std::string* failure);
	static bool TestKernelTextLocation(std::string* failure);
	static bool TestRuntimeGuestExecution(std::string* failure);
	static bool TestApplicationImportBindings(std::string* failure);
	static bool TestRuntimeApplicationImportData(std::string* failure);
	static bool TestRuntimeSanitizerOwner(std::string* failure);
	static bool TestRuntimeAllocatorBootstrap(std::string* failure);
	static bool TestRuntimeCrtSync(std::string* failure);
#endif

	// ps5-vsh: load firmware modules that provide a library an importer needs
	// (libSceJsc for Jsc_v1, ...) before that importer is relocated. Public
	// because the guest module load path (libKernel LoadStartModule) must run it
	// for modules loaded after Start().
	void EnsureImportProviders();
	void     SaveMainProgram(const std::filesystem::path& elf_name);
	void     SaveProgram(Program* program, const std::filesystem::path& elf_name);
	void     UnloadProgram(Program* program);

	[[nodiscard]] uint64_t GetEntry();
	[[nodiscard]] uint64_t GetProcParam();

	void RelocateAll();
	void RelocateProgram(Program* program);

	void  Execute(const std::filesystem::path& game_patch = {},
	            const std::vector<std::string>& guest_args = {},
	            const std::vector<CoElfLaunch>& co_elfs = {});
	// ps5-vsh: runs a co-hosted ELF entry on the calling thread (used by
	// co-elf worker threads). argv[0] is the file name, then args.
	void RunCoElfEntry(uint64_t addr, const std::string& argv0,
	                   const std::vector<std::string>& args,
	                   Libs::LibKernel::FileSystem::GuestFsContextPtr fs_context);
	// Called from a PthreadCreate worker, already on its owned guest stack.
	RuntimeGuestResult RunApplicationEntry(Program* program, EntryParams* params);
	int   StartModule(Program* program, size_t args, const void* argp, module_func_t func);
	int   StopModule(Program* program, size_t args, const void* argp, module_func_t func);
	int   StopModule(Program* program, size_t args, const void* argp, module_func_t func, bool* finalized);
	ModuleCallResult StartModuleChecked(Program* program, size_t args, const void* argp, module_func_t func);
	ModuleCallResult StopModuleChecked(Program* program, size_t args, const void* argp, module_func_t func);
	// Host-only kernel publication/rollback. Failed images stay mapped/pinned;
	// do not use UnloadProgram as emergency recovery in a shared session.
	int LoadStartModuleReference(Program* program, size_t args, const void* argp, int* result);
	int StopModuleReference(Program* program, size_t args, const void* argp, int* result);
	void RollbackModuleReference(Program* program);
	void  StopAllModules();
	void  StartAllModules();
	// Failed-owner cleanup may release host-backed TLS, but must retain blocks
	// whose release requires an uncovered guest allocator callback.
	void  DeleteTlss(int thread_id, bool allow_guest_callbacks = true);
	void* ApplicationHeapMalloc(uint64_t size);
	void* ApplicationHeapMemalign(uint64_t alignment, uint64_t size);

	void Resolve(const std::string& name, SymbolType type, Program* program, SymbolRecord* out_info,
	             bool* bind_self);
	// True means handled (including a miss); callers must not fall through to HLE/global lookup.
	bool ResolveNativeSslDlsym(Program* handle, std::string_view symbol, std::string_view nid, uint64_t* address);
	bool IsNativeSslAddress(uint64_t address) const;

	SymbolDatabase* Symbols() { return m_symbols.get(); }

	const std::vector<Program*>& GetPrograms() const { return m_programs; }

	static uint64_t ReadFromElf(Program* program, uint64_t vaddr);
	Program*        FindProgramByAddr(uint64_t vaddr);
	Program*        FindProgramById(int32_t id);
	Program*        FindProgramByFileName(const std::filesystem::path& elf_name);

	static uint8_t* TlsGetAddr(Program* program);
	static void     DeleteTls(Program* program, int thread_id, bool allow_guest_callbacks = true);

	// ps5-vsh: per-thread program scope (voldemort parity). FileSystem
	// mount resolution + guest env use this to scope per-program state.
	static void     SetCurrentProgram(Program* program);
	static Program* GetCurrentProgram();

	void StackTrace(uint64_t frame_ptr, uint64_t stack_ptr);

private:
	friend int RuntimeNpManagerClient::StartForCurrentOwner(Program*, uint64_t&) noexcept;
	friend std::shared_ptr<Libs::NpManagerClient::Client> RuntimeNpManagerClient::AcquireQueryClient() noexcept;
	friend bool RetireSourceBoundNpTransport();
	friend bool InstallSourceBoundNpTransport(Program*);
	friend bool InstallSourceBoundLoginMgrTransport(Program*);
	friend bool RetireSourceBoundLoginMgrTransport();
	friend int InspectSourceBoundNpTransportEpoch(SourceBoundNpTransportEpoch&) noexcept;
	friend int CloseSourceBoundNpTransportEpoch(uint64_t) noexcept;
	// Trusted host-only registration used by legacy startup and the original
	// callback-boundary fixture. NEVER wire a guest export directly to this helper.
	void SetApplicationHeapApi(void* const api[10]);
	bool IsApplicationHeapSpan(const Program* owner, uint64_t address, uint64_t size, bool writable);
	static void LoadProgramToMemory(Program* program);
	static void ParseProgramDynamicInfo(Program* program);
	static void CreateSymbolDatabase(Program* program);
	static void Relocate(Program* program);
	static void DeleteProgram(Program* program);
	static void SetupTlsHandler(Program* program);
	void        PreloadAdjacentPrograms();
	void        BindNativeLibcStartup(Program* program);
	bool        ValidateNativeLibcStartup(Program* program);
	void        RunNativeLibcStartup();
	void        AdmitGuestStartup(bool smoke_initializer = false); // Caller owns m_mutex.
	void        CheckStartupMutation();
	void        PrepareNativeSslSmoke();
	void        ValidateNativeSslSmoke(bool relocated);
	bool        ResolveNativeSslSmoke(const std::string& name, SymbolType type, Program* importer, SymbolRecord* out);
	[[noreturn]] void RunNativeSslSmoke(void* stack_top);
	void        RunNativeSslProduction(void* stack_top);
	bool        NativeSslSelected() const { return m_ssl_smoke_enabled || m_ssl_production_enabled; }
	bool        ResolveNativeSslProduction(const std::string& name, SymbolType type, Program* importer, SymbolRecord* out);

	static const ModuleId*  FindModule(const Program& program, const std::string& id);
	static const LibraryId* FindLibrary(const Program& program, const std::string& id);

	std::vector<Program*>           m_programs;
	std::unique_ptr<SymbolDatabase> m_symbols;
	Common::Mutex                   m_mutex;
	NativeLibcStartup::State         m_native_libc_startup;
	bool                            m_primary_startup_entered = false;
	bool                            m_ssl_smoke_enabled = false;
	bool                            m_ssl_production_enabled = false;
	bool                            m_ssl_production_ready = false;
	// Shared smoke/production admission. Frozen is released only by successful production DT_INIT.
	bool                            m_ssl_smoke_prepared = false;
	bool                            m_ssl_smoke_frozen = false;
	uint64_t                        m_ssl_smoke_thread = 0;
	NativeSslSmoke::ProviderStartup  m_ssl_provider_startup;
	Program*                        m_ssl_smoke_libc = nullptr;
	Program*                        m_ssl_smoke_ssl = nullptr;

	application_heap_malloc_func_t         m_application_heap_malloc         = nullptr;
	application_heap_free_func_t           m_application_heap_free           = nullptr;
	application_heap_posix_memalign_func_t m_application_heap_posix_memalign = nullptr;
};

uint64_t CalcFlexibleMemorySize(const Elf64_Phdr* phdr, uint16_t phnum);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestMainEntryUsesGuestStack();
bool TestModuleRelocationUsesWritableHostMapping();
bool TestInstallNativeFatalHandler();
bool TestNativeFatalHandler(uint64_t pc, unsigned access_kind, void* context);
bool TestNativeLibcRawExport(const DynamicInfo& info);
#endif

void DumpTraceRings();
uint64_t TraceWrapImport(uint64_t real_vaddr, const std::string& name);

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_ */
