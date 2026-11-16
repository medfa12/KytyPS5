// Isolated-process, exact-image component test. Parent TU supplies the normal
// loader/kernel/NP headers, Windows.h, and these registrar declarations at FILE
// scope (outside its anonymous namespace):
//   namespace Libs::LibIpmiConfig { void InitIpmiConfig_1(Loader::SymbolDatabase*); }
//   namespace Libs::OfflineLogin { void InitOfflineLogin_1(Loader::SymbolDatabase*); }
// Also include libs/npMemoryHeap.h, libs/npNativeTransport.h,
// loader/runtimeNpTransport.h, loader/runtimeProcessIdentity.h, kernel/eventflag.h.
// Required inputs: KYTY_TEST_NP_CORE_IMAGE, KYTY_TEST_NP_ALTERNATE_IMAGE,
// KYTY_APPDB_CORE_AUTH_RECORD. The process runner supplies the outer timeout.
//
// This calls IPC init/stop/term directly, NOT Core's entry/init table or NP's
// wider manager startup. Its client is another real thread of the admitted Core
// process, not a fabricated UI principal. Local GOT bindings below do not admit
// any new production exports. No fake native callback or service reply is used.
namespace NpNativeServiceTest {
constexpr const char* Name = "NpNativeService";
namespace N = Libs::NpNativeTransport;
namespace R = Libs::NpNativeTransport::Retained;
namespace I = Libs::LibKernel::Ipmi;
namespace K = Libs::LibKernel;

void Require(bool condition, const char* message) noexcept {
	if (condition) { return; }
	// Check may throw in a parent fixture. Neither it nor an unexpected exception
	// may unwind live native threads, allocator state or source-bound mappings.
	try { Check(Name, false, message); } catch (...) {}
	std::abort();
}
struct LiveState {
	bool armed = true;
	~LiveState() { if (armed) { std::abort(); } }
};
template<class T> T Read(uint64_t address) {
	T value {};
	std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
	return value;
}
bool ExactImage(Loader::Program* program, const char* hex, uint64_t size) {
	uint8_t digest[32] {};
	auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
	for (size_t i = 0; i < 32; ++i) { digest[i] = static_cast<uint8_t>(digit(hex[2*i]) * 16 + digit(hex[2*i+1])); }
	return program && program->elf && program->elf->MatchesFileSha256(digest, size);
}
void VerifyCoreSnapshot(std::span<const uint8_t> source) {
	constexpr std::array<uint8_t, 32> expected {
		0xf2,0xe5,0x69,0xce,0xbe,0x32,0x41,0x29,0xa8,0xbe,0x6a,0x40,0xa4,0x29,0xe6,0xf6,
		0x65,0x20,0x21,0xf2,0x61,0xde,0xd3,0xc0,0x2a,0x54,0x69,0xcc,0x81,0xf5,0xa4,0x34
	};
	Require(source.size() == 35337516, "retained Core source has exact length");
	std::array<uint8_t, 32> digest {};
	auto* sha = av_sha_alloc();
	Require(sha && av_sha_init(sha, 256) == 0, "allocate retained-source SHA256");
	av_sha_update(sha, source.data(), source.size());
	av_sha_final(sha, digest.data());
	av_free(sha);
	Require(digest == expected, "authenticate the actual retained Core source bytes");
}
void Environment(const char* key, const char* value) {
#if defined(_WIN32)
	Require(_putenv_s(key, value) == 0, "set isolated fixture environment");
#else
	Require(setenv(key, value, 1) == 0, "set isolated fixture environment");
#endif
}

// Verify each exact relocation, not just an RVA containing some writable word.
void Import(Loader::Program* program, uint32_t slot, const char* symbol, uint64_t target, bool bind) {
	Require(program && program->dynamic_info && target, "import owner/target exists");
	const auto& info = *program->dynamic_info;
	bool found = false;
	auto inspect = [&](const Loader::Elf64_Rela* table, uint64_t bytes) {
		for (size_t i = 0; i < bytes / sizeof(Loader::Elf64_Rela); ++i) {
			const auto& relocation = table[i];
			if (relocation.r_offset != slot) { continue; }
			Require(!found && (relocation.GetType() == 7 || relocation.GetType() == 6) && relocation.r_addend == 0,
			        "unique unadjusted function/guard relocation");
			const auto index = relocation.GetSymbol();
			Require(index < info.symbol_table_total_size / sizeof(Loader::Elf64_Sym), "import symbol extent");
			const auto offset = info.symbol_table[index].st_name;
			Require(offset < info.str_table_size && std::strcmp(info.str_table + offset, symbol) == 0,
			        "exact audited import identity");
			found = true;
		}
	};
	inspect(info.jmprela_table, info.jmprela_table_size);
	inspect(info.rela_table, info.rela_table_total_size);
	Require(found, "audited import relocation present");
	const uint64_t address = program->base_vaddr + slot;
	if (bind) {
#if defined(_WIN32)
		DWORD previous = 0, ignored = 0;
		Require(VirtualProtect(reinterpret_cast<void*>(address), 8, PAGE_READWRITE, &previous) != 0, "local GOT write admission");
		std::memcpy(reinterpret_cast<void*>(address), &target, 8);
		Require(VirtualProtect(reinterpret_cast<void*>(address), 8, previous, &ignored) != 0, "restore local GOT protection");
#else
		Require(false, "source-bound native fixture requires Windows");
#endif
	}
	const auto actual = Read<uint64_t>(address);
	const bool matches = actual == target || (!bind && Loader::TestTraceImportTarget(actual, target));
	if (!matches) {
		std::fprintf(stderr, "NP fixture import mismatch: slot=0x%x symbol=%s actual=0x%llx expected=0x%llx\n",
		             slot, symbol, static_cast<unsigned long long>(actual), static_cast<unsigned long long>(target));
	}
	Require(matches, "actual import target or registered trace wrapper equals audited native/HLE implementation");
}

struct NativeBinding { uint32_t slot; const char* symbol; uint32_t rva; };
// Fresh read-only decoding of the two SHA256-pinned images. Beyond create/join,
// native recursive mutexes, event objects and allocator-aware new/delete are
// needed by cb1d70/cb2020/cb3390/cb37d0/cb2350. In particular HLE ObjectNew must
// not be mixed with native ObjectDelete's allocator prefix at object-0x10.
constexpr NativeBinding NativeImports[] {
	{0x2068588, "fhJ5uKzcn0w#R#R", 0x6c60}, {0x2068590, "EjMsfO3GCIA#R#R", 0x6d80},
	{0x20690c8, "O1AvlQU33pI#R#R", 0x1e410}, {0x20690d0, "9zi9FTPol74#R#R", 0x1e430},
	{0x20690d8, "Yc+qj4TIEY0#R#R", 0x1e4b0}, {0x20690a8, "aTNOl9EB4V4#R#R", 0x1e4c0},
	{0x20690c0, "RgGW4f0ox1g#R#R", 0x1e500}, {0x20690b0, "VM+CXTW4F-s#R#R", 0x1e5f0},
	{0x20690b8, "eYgHIWx0Hco#R#R", 0x1e6a0},
	{0x2068cc8, "cMOgkE2M2e8#R#R", 0x1d910}, {0x2068ce0, "WWW4bvT-rSw#R#R", 0x1d930},
	{0x2068d08, "st-oQvV7HVI#R#R", 0x1d9e0}, {0x2068cc0, "amFi-Av19hU#R#R", 0x1d9f0},
	{0x2068cd8, "QlaBcxSFPZI#R#R", 0x1da60},
	{0x2069288, "0syNkhJANVw#R#R", 0x185a0}, {0x2069270, "CnDHI7sU+l0#R#R", 0x18700},
	{0x2068980, "mzlILsFx0cU#R#R", 0x55f0}, {0x2068988, "BztTl7QeYqE#R#R", 0x5620},
};
struct HleBinding { uint32_t slot; const char* symbol; };
constexpr HleBinding CoreImports[] {
	{0x2065820, "vo2FiTXAekk#I#J"}, {0x2065830, "hoWSsRT0ktA#I#J"}, {0x2065828, "OXc1BksfEIg#I#J"},
	{0x2069c78, "pHLjntY0psg#T#S"}, {0x2069c80, "hyuye+88uPo#T#S"},
	{0x2069c88, "V38nfJwXYhg#T#S"}, {0x2069c90, "VY8Xji9cAFA#T#S"}, {0x2069c98, "VBZtcFn7+aY#T#S"},
	{0x2065880, "eLdDw6l0-bU#k#g"}, {0x2065ec8, "3GPpjQdAMTw#k#g"},
	{0x2065ed0, "9rAeANT2tyE#k#g"}, {0x2065858, "tsvEmnenz48#k#g"}, {0x20660a8, "3kg7rT0NQIs#Z#X"},
};
// Direct kernel closure of native event create/delete/open/close, recursive
// mutex construction/lock/unlock/destroy, and seven-argument thread create/join.
constexpr HleBinding NpImports[] {
	{0x49320, "Ou3iL1abvng#A#B"},
	{0x49448, "BpFoboUJoZU#A#B"}, {0x49450, "8mql9OcQnd4#A#B"},
	{0x49458, "1vDaenmJtyA#A#B"}, {0x49460, "s9-RaxukuzQ#A#B"},
	{0x49490, "F8bUHwAG284#A#B"}, {0x49498, "iMp8QpE+XO4#A#B"},
	{0x494a0, "cmo1RIYva9o#A#B"}, {0x494a8, "smWEktiyyG0#A#B"},
	{0x494b0, "2Of0f+3mhhE#A#B"}, {0x494b8, "9UK1vLZQft4#A#B"},
	{0x494c0, "upoVrzMHFeE#A#B"}, {0x494c8, "tn3VlD0hG60#A#B"},
	{0x49508, "nsYoNRywwNg#A#B"}, {0x49510, "UTXzJbWhhTE#A#B"},
	{0x49518, "WB66evu8bsU#A#B"}, {0x49520, "eXbUSpEaTsA#A#B"},
	{0x49528, "4+h9EzwKF4I#A#B"}, {0x49530, "DzES9hQF4f4#A#B"},
	{0x49538, "3qxgM4ezETA#A#B"}, {0x49540, "6UgtwV+0zb4#A#B"},
	{0x49548, "62KCwEMmzcM#A#B"}, {0x49550, "onNY9Byn-W8#A#B"},
};
uint64_t Hle(Loader::SymbolDatabase* symbols, const char* name, Loader::SymbolType type = Loader::SymbolType::Func) {
	const std::string full(name);
	const auto* symbol = symbols->FindByNid(full.substr(0, full.find('#')), type);
	Require(symbol && symbol->vaddr, "required real HLE registration");
	return symbol->vaddr;
}

// FIXTURE ONLY: exact alternate Core cleanup correction, never a production
// admission/patch policy. The failed native run is retained in
// _Build/vsh-integrated/np-native-service-unlock-assert.log:7024-7027.
// cb1f38 acquires the session-manager mutex. The original tail then destroys
// that still-locked mutex, clears its initialized byte even on EBUSY, and calls
// Unlock, which asserts IsInit(). Change only the two direct rel32 call targets.
// Both calls retain their own `mov rdi,r14` and return addresses. Neither return
// value is consumed: cb2000 clears manager state and cb2004 XORs EAX to zero.
// This fixture reaches the corrected tail only through caaaf0 after its clients
// have drained and native stop/join has completed; it does not exercise init
// failure rollback or concurrent teardown. Install the transform BEFORE init,
// not while the dispatcher or any guest callback is executing.
constexpr uint32_t CleanupRva = 0xcb1ff0;
using CleanupWindow = std::array<uint8_t, 0x25>;
constexpr CleanupWindow OriginalCleanup {
	0x4c,0x89,0xf7,0xe8,0x78,0xdc,0xca,0x00, 0x4c,0x89,0xf7,0xe8,0x60,0xdc,0xca,0x00,
	0xc6,0x43,0x08,0x00,0x31,0xc0,0x48,0x83,0xc4,0x08,0x5b,0x41,0x5c,0x41,0x5d,
	0x41,0x5e,0x41,0x5f,0x5d,0xc3
};
constexpr CleanupWindow CorrectedCleanup {
	0x4c,0x89,0xf7,0xe8,0x68,0xdc,0xca,0x00, 0x4c,0x89,0xf7,0xe8,0x70,0xdc,0xca,0x00,
	0xc6,0x43,0x08,0x00,0x31,0xc0,0x48,0x83,0xc4,0x08,0x5b,0x41,0x5c,0x41,0x5d,
	0x41,0x5e,0x41,0x5f,0x5d,0xc3
};
bool PlanCleanup(std::span<const uint8_t> source, std::span<const uint8_t> mapped, CleanupWindow& output) {
	if (source.size() != OriginalCleanup.size() || mapped.size() != OriginalCleanup.size() ||
	    !std::equal(source.begin(), source.end(), OriginalCleanup.begin()) ||
	    !std::equal(mapped.begin(), mapped.end(), OriginalCleanup.begin())) { return false; }
	output = CorrectedCleanup;
	return true;
}
void CheckCleanupPlanner() {
	CleanupWindow output; output.fill(0xa5);
	const auto untouched = output;
	for (size_t i = 0; i < OriginalCleanup.size(); ++i) {
		auto bad = OriginalCleanup; bad[i] ^= 1;
		Require(!PlanCleanup(bad, OriginalCleanup, output) && output == untouched, "reject each source-window mismatch without modifying output");
		Require(!PlanCleanup(OriginalCleanup, bad, output) && output == untouched, "reject each mapped-window mismatch without modifying output");
	}
	Require(!PlanCleanup(std::span<const uint8_t>(OriginalCleanup).first(0x24), OriginalCleanup, output) && output == untouched,
	        "reject truncated source window without mutation");
	Require(!PlanCleanup(OriginalCleanup, std::span<const uint8_t>(OriginalCleanup).first(0x24), output) && output == untouched,
	        "reject truncated mapped window without mutation");
	Require(!PlanCleanup(OriginalCleanup, CorrectedCleanup, output) && output == untouched &&
	        !PlanCleanup(CorrectedCleanup, OriginalCleanup, output) && output == untouched, "reject double application or already-modified source");
	Require(PlanCleanup(OriginalCleanup, OriginalCleanup, output) && output == CorrectedCleanup, "exact window plans reviewed replacement");
	for (size_t i = 0; i < output.size(); ++i) {
		Require((output[i] != OriginalCleanup[i]) == (i == 4 || i == 12), "only reviewed rel32 displacement bytes differ");
	}
}
void CorrectFixtureCleanup(Loader::Program* core, Loader::Program* np) {
	CheckCleanupPlanner();
	Require(core && np && !core->started.load() && !np->started.load() &&
	        core->application_threads.load() == 0 && np->application_threads.load() == 0 && !N::Installed(),
	        "cleanup correction only during isolated pre-entry preparation");
	Require(core->boot_credential_admission && core->boot_credential_admission->IsCoreImage() &&
	        core->boot_credential_admission->IsLiveFor(core) &&
	        ExactImage(core, "f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434", 35337516) &&
	        ExactImage(np, "81de809c26e4cd1a73d6268c7e89d1e41cf811397f1ac7be17f70e8e14a26a35", 411092),
	        "live source-bound admission and same-open exact images before correction");
	Require(Read<uint8_t>(core->base_vaddr + 0x213cad0) == 0 &&
	        Read<uint64_t>(core->base_vaddr + 0x213cb00) == 0, "native IPC has not entered init/dispatcher");
	std::vector<uint8_t> source;
	Require(core->elf->SnapshotPlainFile(35337516, &source) && source.size() == 35337516 &&
	        ExactImage(core, "f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434", 35337516),
	        "retain same-open pinned source snapshot; never reopen by path");
	VerifyCoreSnapshot(source);
	auto source_window = [&](uint32_t rva, size_t count, uint32_t expected_offset) {
		bool found = false;
		const auto* headers = core->elf->GetPhdr();
		for (size_t i = 0; i < core->elf->GetEhdr()->e_phnum; ++i) {
			const auto& ph = headers[i];
			if (ph.p_type != Loader::PT_LOAD || !(ph.p_flags & Loader::PF_X) || rva < ph.p_vaddr ||
			    rva - ph.p_vaddr > ph.p_filesz || count > ph.p_filesz - (rva - ph.p_vaddr)) { continue; }
			Require(!found && ph.p_offset + (rva - ph.p_vaddr) == expected_offset, "exact executable source-to-mapping offset");
			found = true;
		}
		Require(found && expected_offset <= source.size() && count <= source.size() - expected_offset,
		        "bounded source instruction window");
		return std::span<const uint8_t>(source).subspan(expected_offset, count);
	};
	// Verify the COMPLETE manager Term and outer IPC Term against pinned source.
	// The latter retains its actual stop -> join -> destroy/free -> manager Term
	// order, rather than merely assuming the direct tail is reached after join.
	for (const auto& region: {std::array<uint32_t, 3>{0xcb1f10, 0x105, 0xcb5f10},
	                         std::array<uint32_t, 3>{0xcaaaf0, 0xad, 0xcaeaf0}}) {
		const auto bytes = source_window(region[0], region[1], region[2]);
		Require(std::memcmp(reinterpret_cast<const void*>(core->base_vaddr + region[0]), bytes.data(), bytes.size()) == 0,
		        "complete native cleanup functions equal pinned source before transform");
	}
	constexpr std::array<uint8_t, 32> plt {
		0xff,0x25,0x52,0x94,0x70,0x00,0x68,0x21,0x07,0x00,0x00,0xe9,0xd0,0x8d,0xff,0xff,
		0xff,0x25,0x4a,0x94,0x70,0x00,0x68,0x22,0x07,0x00,0x00,0xe9,0xc0,0x8d,0xff,0xff
	};
	const auto source_plt = source_window(0x195fc60, plt.size(), 0x1963c60);
	Require(std::equal(source_plt.begin(), source_plt.end(), plt.begin()) &&
	        std::memcmp(reinterpret_cast<const void*>(core->base_vaddr + 0x195fc60), plt.data(), plt.size()) == 0,
	        "exact source and mapped Unlock/Destroy PLT stubs");
	Import(core, 0x20690b8, "eYgHIWx0Hco#R#R", np->base_vaddr + 0x1e6a0, false);
	Import(core, 0x20690c0, "RgGW4f0ox1g#R#R", np->base_vaddr + 0x1e500, false);
	const auto original = source_window(CleanupRva, OriginalCleanup.size(), 0xcb5ff0);
	const auto mapped = Read<CleanupWindow>(core->base_vaddr + CleanupRva);
	CleanupWindow replacement {};
	Require(PlanCleanup(original, mapped, replacement), "exact cleanup source/mapping plan; reject mismatch before any code mutation");
#if defined(_WIN32)
	auto* address = reinterpret_cast<void*>(core->base_vaddr + CleanupRva);
	MEMORY_BASIC_INFORMATION first {}, last {};
	Require(VirtualQuery(address, &first, sizeof(first)) == sizeof(first) &&
	        VirtualQuery(reinterpret_cast<void*>(core->base_vaddr + CleanupRva + replacement.size() - 1), &last, sizeof(last)) == sizeof(last) &&
	        first.State == MEM_COMMIT && last.State == MEM_COMMIT && first.Protect == last.Protect,
	        "both instruction pages have identical committed protection for exact restoration");
	DWORD previous = 0, ignored = 0;
	Require(VirtualProtect(address, replacement.size(), PAGE_EXECUTE_READWRITE, &previous) != 0, "admit isolated instruction transform");
	// Write only the two verified rel32 operands; opcodes, argument setup and
	// the complete epilogue are never written by this transform.
	std::memcpy(static_cast<uint8_t*>(address) + 4, replacement.data() + 4, 4);
	std::memcpy(static_cast<uint8_t*>(address) + 12, replacement.data() + 12, 4);
	Require(VirtualProtect(address, replacement.size(), previous, &ignored) != 0, "restore instruction page protection");
	Require(FlushInstructionCache(GetCurrentProcess(), address, replacement.size()) != 0, "flush corrected instruction window");
	Require(Read<CleanupWindow>(core->base_vaddr + CleanupRva) == CorrectedCleanup, "exact corrected instruction readback");
#else
	Require(false, "native instruction correction requires Windows");
#endif
}
template<class F> int Poll(F operation) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	for (;;) {
		const int status = operation();
		if (status != I::WouldBlock && status != I::Busy) { return status; }
		Require(std::chrono::steady_clock::now() < deadline, "retained native operation timed out; preserve process state");
		K::PthreadYield();
	}
}
struct Client {
	Loader::Program* core;
	int pid;
	int main_tid;
	std::atomic_bool done {false};
};
void* KYTY_SYSV_ABI ClientEntry(void* argument) {
	LiveState live;
	auto& client = *static_cast<Client*>(argument);
	Require(Loader::RuntimeLinker::GetCurrentProgram() == client.core &&
	        Loader::GetCurrentGuestProcessId() == client.pid && K::PthreadSelfOrNull() &&
	        K::PthreadGetthreadid() != client.main_tid, "client inherits admitted Core PID on a distinct actual guest thread");
	R::Connection connection = 0;
	Require(R::Register(connection) == 0 && connection, "register retained client with real identity");
	Require(R::BeginConnect(connection) == 0, "enqueue native acceptance");
	int32_t acceptance = 123;
	Require(Poll([&] { return R::PollConnect(connection, acceptance); }) == 0 && acceptance == 0,
	        "real Core dispatcher accepted through caae10/cb2020");
	// Method zero's jump-table entry at 1f2cef8 is cadf2e -> caec60 -> cb28a0
	// -> cb38c0. It formats the actual session's event name and explicitly replies
	// through session+0x30. It has no dependency on the wider NP login state.
	Require(R::BeginInvoke(connection, 0, {}, {0x20}) == 0, "enqueue actual native method zero");
	Require(Poll([&] { return R::PollInvoke(connection); }) == 0, "native method-zero reply published");
	I::Result reply;
	Require(R::ConsumeInvoke(connection, reply) == 0 && reply.method_result == 0 &&
	        reply.outputs.size() == 1 && reply.outputs[0].size() == 0x20, "native method zero returned exact 32-byte output");
	const auto& bytes = reply.outputs[0];
	Require(std::memchr(bytes.data(), 0, bytes.size()) &&
	        std::memcmp(bytes.data(), "SceNpMgrIpcSvrCbP", sizeof("SceNpMgrIpcSvrCbP") - 1) == 0,
	        "reply contains native session-specific callback event name");
	const std::string event_name(reinterpret_cast<const char*>(bytes.data()));
	K::EventFlag::KernelEventFlag event = nullptr;
	Require(K::EventFlag::KernelOpenEventFlag(&event, event_name.c_str()) == 0 && event,
	        "native acceptance actually created the named kernel event");
	Require(K::EventFlag::KernelCloseEventFlag(event) == 0, "close observer event alias before disconnect");
	Require(R::BeginDisconnect(connection) == 0, "disconnect before native stop/term");
	int32_t disconnected = 123;
	Require(Poll([&] { return R::PollDisconnect(connection, disconnected); }) == 0 && disconnected == 0,
	        "native disconnect callback closes session and removes context");
	Require(Poll([&] { return R::Retire(connection); }) == 0, "retire client on its original guest thread");
	event = nullptr;
	Require(K::EventFlag::KernelOpenEventFlag(&event, event_name.c_str()) != 0 && !event,
	        "native session destructor deleted callback event");
	client.done.store(true);
	live.armed = false;
	return nullptr;
}
struct LifecycleCall {
	Loader::Program* core;
	Loader::Program* np;
	int pid;
	bool race;
};
void NativeLifecycleBody(LifecycleCall& call);
void* KYTY_SYSV_ABI NativeLifecycleEntry(void* argument) {
	NativeLifecycleBody(*static_cast<LifecycleCall*>(argument));
	return nullptr;
}
} // namespace NpNativeServiceTest

void TestNpNativeServiceImpl(bool race) {
	using namespace NpNativeServiceTest;
#if !defined(_WIN32)
	Require(false, "source-bound native service fixture requires Windows");
#else
	LiveState live;
	const char* core_path = std::getenv("KYTY_TEST_NP_CORE_IMAGE");
	const char* np_path = std::getenv("KYTY_TEST_NP_ALTERNATE_IMAGE");
	const char* auth_path = std::getenv("KYTY_APPDB_CORE_AUTH_RECORD");
	Require(core_path && *core_path && np_path && *np_path && auth_path && *auth_path, "explicit pinned Core/NpCommon/raw-auth inputs required");
	for (const char* key: {"KYTY_NATIVE_NP_OWNERSHIP", "KYTY_NATIVE_NP_WORKQUEUE", "KYTY_NATIVE_NP_CALLOUT",
	                      "KYTY_NATIVE_NP_HTTP_POOL", "KYTY_NATIVE_NP_DISPATCHER"}) { Environment(key, ""); }
	Environment("KYTY_NATIVE_NP_ALTERNATE_STARTUP", "1");
	K::Initialize();
	K::PthreadInitSelfForMainThread();
	auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitLibKernel_1(linker->Symbols());
	Libs::LibC::InitLibC_1(linker->Symbols());
	Libs::LibIpmiConfig::InitIpmiConfig_1(linker->Symbols());
	Libs::OfflineLogin::InitOfflineLogin_1(linker->Symbols());
	auto* core = linker->LoadProgram(core_path);
	auto* np = linker->LoadProgram(np_path);
	Require(ExactImage(core, "f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434", 35337516) &&
	        core->boot_credential_admission && core->boot_credential_admission->IsCoreImage() &&
	        core->boot_credential_admission->IsLiveFor(core), "exact alternate Core loaded with source/mapping/auth admission");
	Require(ExactImage(np, "81de809c26e4cd1a73d6268c7e89d1e41cf811397f1ac7be17f70e8e14a26a35", 411092) && np->np_alternate54,
	        "exact alternate NpCommon co-provider admitted");
	Require(!np->export_symbols->FindByNid("fhJ5uKzcn0w", Loader::SymbolType::Func) &&
	        !np->export_symbols->FindByNid("EjMsfO3GCIA", Loader::SymbolType::Func), "fixture must not widen production pair exports");
	// Unrelated Core imports are not executed. Every import in this bounded path
	// is separately verified/bound below; unresolved unrelated imports stay unused.
	core->fail_if_global_not_resolved = false;
	linker->RelocateProgram(np);
	linker->RelocateProgram(core);
	for (const auto& binding: NativeImports) { Import(core, binding.slot, binding.symbol, np->base_vaddr + binding.rva, true); }
	for (const auto& binding: CoreImports) { Import(core, binding.slot, binding.symbol, Hle(linker->Symbols(), binding.symbol), false); }
	for (const auto& binding: NpImports) { Import(np, binding.slot, binding.symbol, Hle(linker->Symbols(), binding.symbol), false); }
	Import(core, 0x2065450, "f7uOxY9mM1U#Z#X", Hle(linker->Symbols(), "f7uOxY9mM1U", Loader::SymbolType::Object), false);
	Import(np, 0x492d8, "f7uOxY9mM1U#A#B", Hle(linker->Symbols(), "f7uOxY9mM1U", Loader::SymbolType::Object), false);
	// Critical create/join imports must not resolve to the old NP success stubs.
	Import(np, 0x49540, "6UgtwV+0zb4#A#B", reinterpret_cast<uint64_t>(&K::PthreadCreate), false);
	Import(np, 0x49550, "onNY9Byn-W8#A#B", reinterpret_cast<uint64_t>(&K::PthreadJoin), false);
	CorrectFixtureCleanup(core, np); // Exact, fixture-only pre-entry transform.

	Libs::LncUtil::Lnc::RegisterProgramLaunch(core, {}, false, 791001);
	int32_t app_id = -1;
	Require(Libs::LncUtil::Lnc::TryGetProgramAppId(core, &app_id), "real Core launch identity");
	const int pid = Loader::RegisterCohostProcessIdentity(core, app_id);
	Require(pid > 100, "distinct retained cohost PID");
	Loader::RuntimeLinker::SetCurrentProgram(core);
	std::array<uint64_t, 17> auth {};
	Require(Loader::TryGetSourceBoundProcessAuthInfo(pid, &auth) && (auth[1] & (UINT64_C(1) << 62)), "source-bound Core registration capability");
	Require(Loader::InstallSourceBoundNpTransport(core), "explicit production source-bound hooks installed for component test");
	Loader::SourceBoundNpTransportEpoch epoch;
	Require(Loader::InspectSourceBoundNpTransportEpoch(epoch) == 0 &&
	        Loader::CloseSourceBoundNpTransportEpoch(epoch.generation) == I::Invalid,
	        "selecting Core on the ownerless launcher cannot authorize epoch close");
	LifecycleCall call {core, np, pid, race};
	K::Pthread lifecycle_thread = nullptr;
	Require(K::PthreadCreate(&lifecycle_thread, nullptr, NativeLifecycleEntry, &call, "NpNativeLifecycle") == 0 && lifecycle_thread,
	        "native init/cleanup execute on one actual inherited Core pthread");
	RuntimeNpManagerClientTest::JoinExited(lifecycle_thread);
	Loader::RuntimeLinker::SetCurrentProgram(nullptr);
	live.armed = false;
#endif
}

namespace NpNativeServiceTest {
void NativeLifecycleBody(LifecycleCall& call) {
#if defined(_WIN32)
	LiveState live;
	auto* core = call.core;
	auto* np = call.np;
	const auto pid = call.pid;
	const auto race = call.race;
	Require(Loader::RuntimeLinker::GetCurrentProgram() == core &&
	        K::PthreadGetOwnerProgram(K::PthreadSelfOrNull()) == core && Loader::GetCurrentGuestProcessId() == pid,
	        "actual lifecycle pthread retains Core ownership and process identity");
	using Lifecycle = int (KYTY_SYSV_ABI *)();
	const auto init = reinterpret_cast<Lifecycle>(core->base_vaddr + 0xcaa790);
	const auto stop = reinterpret_cast<Lifecycle>(core->base_vaddr + 0xcaaad0);
	const auto term = reinterpret_cast<Lifecycle>(core->base_vaddr + 0xcaaaf0);
	Require(Read<uint8_t>(core->base_vaddr + 0x213cad0) == 0, "fresh native IPC initialized flag");
	Require(init() == 0, "actual native IPC init creates heap/server/work/dispatcher");
	TestNpNativeNotification(core, np);
	const auto ipc_heap = Libs::NpManagerIpcMemory::TestSnapshot();
	Require(ipc_heap.base && ipc_heap.size == 0x18000 && ipc_heap.mode == 0, "native IPC initialized its actual owned heap");
	const auto server = Read<uint64_t>(core->base_vaddr + 0x213cad8);
	const auto work = Read<uint64_t>(core->base_vaddr + 0x213cae0);
	const auto dispatcher = Read<K::Pthread>(core->base_vaddr + 0x213cb00);
	Require(Read<uint8_t>(core->base_vaddr + 0x213cad0) == 1 && server && work && dispatcher &&
	        Read<uint64_t>(core->base_vaddr + 0x213cae8) == server &&
	        Read<uint64_t>(core->base_vaddr + 0x213caf0) == work &&
	        Read<uint64_t>(core->base_vaddr + 0x213caf8) == 0x4100, "native globals contain the audited dispatcher tuple");
	Require(K::PthreadGetOwnerProgram(dispatcher) == core, "native dispatcher inherits Core owner");
	N::Lifecycle::ServerLease server_lease;
	Require(N::Lifecycle::AcquireServer(server, Read<uint64_t>(server + 8), server_lease) == 0 && server_lease,
	        "retain the actual published native server generation");
	K::PthreadLegacyNativeTarget target;
	const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (K::PthreadPinLegacyNativeTarget(dispatcher, &target) != 0) {
		Require(std::chrono::steady_clock::now() < ready_deadline, "native dispatcher handle publication timed out");
		K::PthreadYield();
	}
	Require(target.Handle(), "retain actual dispatcher OS handle before native join");
	target.UnlockRecord(); // Never hold a kernel record lock across guest calls/join.
	Require(WaitForSingleObject(static_cast<HANDLE>(target.Handle()), 0) == WAIT_TIMEOUT, "dispatcher has not prematurely exited");
	Client client {core, pid, K::PthreadGetthreadid()};
	K::Pthread client_thread = nullptr;
	Require(K::PthreadCreate(&client_thread, nullptr, ClientEntry, &client, "NpServiceProbe") == 0 && client_thread, "actual retained-client guest worker");
	Require(K::PthreadJoin(client_thread, nullptr) == 0 && client.done.load(), "client accepted/replied/disconnected/retired before stop");
	const auto client_epoch = TestRuntimeNpManagerClientAttached(core, np, race);
	Require(Read<uint32_t>(core->base_vaddr + 0x213cb60) == 0, "native session-manager context list drained");
	Require(WaitForSingleObject(static_cast<HANDLE>(target.Handle()), 0) == WAIT_TIMEOUT, "real dispatcher remains live after native callbacks");
	N::Lifecycle::DispatcherSnapshot dispatcher_receipt;
	Require(N::Lifecycle::InspectDispatcher(server_lease, dispatcher_receipt) == 0 &&
	        dispatcher_receipt.binding == N::Lifecycle::DispatcherBinding::Bound &&
	        dispatcher_receipt.receipt.generation && dispatcher_receipt.receipt.identity.pid == pid &&
	        !dispatcher_receipt.proof.native_exited && !dispatcher_receipt.proof.successfully_joined,
	        "actual native Run bound one live dispatcher incarnation without exit/join proof");
	K::PthreadNativeSnapshot native_receipt;
	Require(K::PthreadInspectNativeReceipt(std::static_pointer_cast<K::PthreadNativeReceiptState>(
	            dispatcher_receipt.receipt.token), native_receipt) == 0 &&
	        native_receipt.native_tid == target.ThreadId() && native_receipt.entry == core->base_vaddr + 0xcaaa80 &&
	        native_receipt.argument == core->base_vaddr + 0x213cae8 &&
	        native_receipt.owner_cookie == reinterpret_cast<uintptr_t>(core),
	        "receipt identifies original OS thread and exact native dispatcher entry/tuple");
	const auto dispatcher_generation = dispatcher_receipt.receipt.generation;
	// IPC-only helper: original workers drain, genuine server Stop, then native
	// OS exit. Native Term still owns the sole dispatcher join and destruction.
	auto quiescence = TestRuntimeNpQuiescenceBegin(core, server, Read<uint64_t>(server + 8), client_epoch);
	TestRuntimeNpQuiescenceAdvance(quiescence);
	N::Lifecycle::ServerSnapshot stopped_snapshot;
	Require(N::Lifecycle::InspectServer(server_lease, stopped_snapshot) == 0 &&
	        stopped_snapshot.stop_succeeded && stopped_snapshot.published && !stopped_snapshot.destroyed,
	        "successful Stop is distinct from native destruction");
	Require(WaitForSingleObject(static_cast<HANDLE>(target.Handle()), 10000) == WAIT_OBJECT_0,
	        "early Stop observed actual native exit without joining");
	Require(N::Lifecycle::InspectDispatcher(server_lease, dispatcher_receipt) == 0 &&
	        dispatcher_receipt.receipt.generation == dispatcher_generation &&
	        dispatcher_receipt.proof.native_exited && !dispatcher_receipt.proof.successfully_joined,
	        "receipt observes actual OS exit but cannot claim the pending native join");
	N::Lifecycle::StopResult stopped;
	Require(N::Lifecycle::RequestStop(server_lease, stopped) == 0 && stopped.stop_errno == I::Busy &&
	        stopped.snapshot.stop_succeeded, "repeated Stop preserves Busy and the prior successful-stop proof");
	// caaad0 detaches the optional wider-manager callback. caaaf0 itself calls
	// server+20, actual NpCommon join, destroy and frees. No fake stop result and
	// no pre-join of its thread: native term owns the one real join.
	Require(stop() == 0, "native IPC stop detaches optional callback");
	Require(term() == 0, "native IPC term returned");
	Require(WaitForSingleObject(static_cast<HANDLE>(target.Handle()), 0) == WAIT_OBJECT_0,
	        "actual dispatcher OS thread terminated, independently of term's ignored join result");
	Require(Read<uint8_t>(core->base_vaddr + 0x213cad0) == 0 &&
	        Read<uint64_t>(core->base_vaddr + 0x213cad8) == 0 &&
	        Read<uint64_t>(core->base_vaddr + 0x213cb00) == 0, "native term reset initialized/server/worker globals");
	Require(Read<uint64_t>(core->base_vaddr + 0x213cb48) == 0, "kernel mutex destruction actually cleared its handle after native unlock");
	N::Lifecycle::ServerSnapshot destroyed;
	Require(N::Lifecycle::InspectServer(server_lease, destroyed) == 0 && destroyed.destroyed &&
	        !destroyed.published && !destroyed.dispatching && destroyed.stop_succeeded,
	        "native Term genuinely destroyed the retained server generation after its actual join");
	Require(N::Lifecycle::InspectDispatcher(server_lease, dispatcher_receipt) == 0 &&
	        dispatcher_receipt.receipt.generation == dispatcher_generation &&
	        dispatcher_receipt.proof.native_exited && dispatcher_receipt.proof.successfully_joined &&
	        dispatcher_receipt.proof.join_origin == N::DispatcherJoinOrigin::Explicit &&
	        dispatcher_receipt.proof.join_source.pid == pid &&
	        dispatcher_receipt.proof.join_source.tid == K::PthreadGetthreadid(),
	        "native Term's actual successful join is retained independently of reusable pthread/global storage");
	// Policy-test inputs checked against actual receipts; these zeroes are not
	// captured inner return values and do not certify postcall gateway placement.
	TestRuntimeNpQuiescenceAfterTerm(quiescence, 0, 0);
	Require(!Loader::RetireSourceBoundNpTransport(), "lifecycle lease retains provider after native destruction");
	server_lease.reset();
	Require(Loader::RetireSourceBoundNpTransport() && !N::Installed(), "all native objects/callbacks/API pins drained");
	Libs::NpMemoryHeap::RetryRetirements();
	Require(!Libs::NpMemoryHeap::HasPendingRetirements(), "IPC allocator backing retirement completed");
	Require(Libs::NpManagerIpcMemory::TestSnapshot().mode == 2 &&
	        K::Memory::TestPlaceholderRangeIsFree(reinterpret_cast<uintptr_t>(ipc_heap.base), ipc_heap.size),
	        "native IPC heap destruction and deferred retirement actually released the original mapping");
	TestRuntimeNpManagerClientEpochRetired(client_epoch, core);
	// Keep both images resident until this dedicated process exits: direct IPC
	// init registered native singleton CXA destructors, and this fixture has not
	// run the outer Core CRT/module lifecycle. Do not pretend Clear is its fini.
	live.armed = false;
	Check(Name, true, "real native service init/accept/method0/disconnect/stop/join completed");
#endif
}
} // namespace NpNativeServiceTest

void TestNpNativeService() { TestNpNativeServiceImpl(false); }
void TestNpNativeServiceRace() { TestNpNativeServiceImpl(true); }
