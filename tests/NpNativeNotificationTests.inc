// Include AFTER the complete NpNativeServiceTests.inc, in the same namespace.
// Parent forward declaration (before the service include):
// void TestNpNativeNotification(Loader::Program*, Loader::Program*);
// Call once with (core,np) after successful real IPC init, before IPC term.
// Parent retains the dedicated-process timeout, Core identity, mappings and
// service fixture setup. No wider presence subsystem/worker may be initialized.
//
// This is REGISTRY PLUMBING: an explicitly host observer is registered in the
// actual native list and called by its locked native broadcast. It is neither
// an NP service handler nor evidence for enabled-manager/queue quiescence.
// No manager-enabled byte, user record or notification state is manufactured.
namespace NpNativeNotificationTest {
namespace S = NpNativeServiceTest;
namespace K = Libs::LibKernel;
void Need(bool ok, const char* message) noexcept { S::Require(ok, message); }
template<class R, class... A> R Call(Loader::Program* core, uint32_t rva, A... args) {
	using F = R (KYTY_SYSV_ABI *)(A...);
	return reinterpret_cast<F>(core->base_vaddr + rva)(args...);
}
template<class F> void Await(F predicate, const char* message) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate()) {
		Need(std::chrono::steady_clock::now() < deadline, message);
		K::PthreadYield();
	}
}
struct State {
	Loader::Program* core;
	int pid, coordinator;
	int32_t id = 0;
	uint64_t payload = 0x123456789abcdef0ULL;
	std::atomic<int> calls {0}, callback_tid {0}, unregister_tid {0};
	std::atomic_bool entered {false}, release {false}, callback_returning {false};
	std::atomic_bool attempted {false}, unregistered {false}, producer_done {false}, remover_done {false};
	std::atomic_bool self_case_identified {false};
};
thread_local State* active_observer = nullptr;
bool IsSelfUnregister(const State& s) { return active_observer == &s; }
void Identity(const State& s) {
	Need(Loader::RuntimeLinker::GetCurrentProgram() == s.core &&
	     Loader::GetCurrentGuestProcessId() == s.pid && K::PthreadSelfOrNull() &&
	     K::PthreadGetthreadid() != s.coordinator, "notification worker has actual foreign Core guest identity");
}
void KYTY_SYSV_ABI Observe(int a, int b, void* payload, int c, void* context) {
	S::LiveState live;
	auto& s = *static_cast<State*>(context);
	Identity(s);
	Need(a == 17 && b == 23 && c == 31 && payload == &s.payload, "native broadcast preserves observer ABI/context");
	Need(!active_observer && s.calls.fetch_add(1) == 0, "exactly one admitted observer invocation");
	active_observer = &s;
	s.callback_tid.store(K::PthreadGetthreadid());
	// Identify the unsafe caller locally. NEVER call native unregister here:
	// recursive locking would free the traversal's current node (known UAF).
	s.self_case_identified.store(IsSelfUnregister(s));
	s.entered.store(true);
	Await([&] { return s.release.load(); }, "observer release deadline; abort rather than unwind live native traversal");
	Need(!s.unregistered.load(), "unregister cannot complete while callback retains presence lock");
	s.callback_returning.store(true);
	active_observer = nullptr;
	live.armed = false;
}
void* KYTY_SYSV_ABI Produce(void* context) {
	S::LiveState live;
	auto& s = *static_cast<State*>(context);
	Identity(s);
	Need(Call<int>(s.core, 0xce0370, 17, 23, static_cast<void*>(&s.payload), 31) == 0,
	     "native locked broadcast/unlock succeeds");
	s.producer_done.store(true);
	live.armed = false;
	return nullptr;
}
void* KYTY_SYSV_ABI Remove(void* context) {
	S::LiveState live;
	auto& s = *static_cast<State*>(context);
	Identity(s);
	Need(!IsSelfUnregister(s), "unregister is never invoked reentrantly");
	s.unregister_tid.store(K::PthreadGetthreadid());
	Need(s.unregister_tid.load() != s.callback_tid.load(), "remover is foreign to the in-flight callback");
	// Real kernel contention observation, not a substitute mutex/acknowledgment.
	auto* mutex = reinterpret_cast<K::PthreadMutex*>(s.core->base_vaddr + 0x213e858);
	Need(K::PthreadMutexTrylock(mutex) == static_cast<int>(0x80020010U),
	     "actual presence mutex is Busy on foreign thread while observer executes");
	s.attempted.store(true);
	Need(Call<int>(s.core, 0xce0290, s.id) == 0, "actual native unregister succeeds");
	Need(s.callback_returning.load(), "successful unlink follows observer completion boundary");
	s.unregistered.store(true);
	// Same genuine native producer, now after successful unlink: no invocation.
	Need(Call<int>(s.core, 0xce0370, 17, 23, static_cast<void*>(&s.payload), 31) == 0 && s.calls.load() == 1,
	     "post-unlink native broadcast does not invoke retired observer");
	Need(Call<int>(s.core, 0xce0290, s.id) == static_cast<int>(0x8055a60aU),
	     "second native removal reports not-found, not fabricated success");
	s.remover_done.store(true);
	live.armed = false;
	return nullptr;
}

void Bind(Loader::Program* core, Loader::Program* np) {
	Need(S::ExactImage(core, "f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434", 35337516) &&
	     S::ExactImage(np, "81de809c26e4cd1a73d6268c7e89d1e41cf811397f1ac7be17f70e8e14a26a35", 411092) &&
	     core->boot_credential_admission && core->boot_credential_admission->IsLiveFor(core) &&
	     core->boot_credential_admission->IsCoreImage() && np->np_alternate54,
	     "exact notification images and live Core source admission");
	std::vector<uint8_t> source;
	Need(core->elf->SnapshotPlainFile(35337516, &source) && source.size() == 35337516 &&
	     S::ExactImage(core, "f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434", 35337516),
	     "same-open exact registry source snapshot");
	S::VerifyCoreSnapshot(source);
	// This exact image maps these entire functions with file offset RVA+4000.
	Need(std::memcmp(reinterpret_cast<void*>(core->base_vaddr + 0xce0040), source.data() + 0xce4040, 0x5f6) == 0,
	     "complete registry/list/mutex/heap function window matches pinned source");
	Need(S::Read<uint64_t>(core->base_vaddr + 0x202ee38) == core->base_vaddr + 0xce0120 &&
	     S::Read<uint64_t>(core->base_vaddr + 0x202ee40) == core->base_vaddr + 0xce0130,
	     "actual native registry entry destructor vtable");
	const S::NativeBinding locks[] {
		{0x2068a80, "1CiXI-MyEKs#R#R", 0x6a50}, {0x2068a88, "4zxevggtYrQ#R#R", 0x6ae0},
		{0x2068a90, "18j+qk6dRwk#R#R", 0x6af0}, {0x2068a98, "CQG2oyx1-nM#R#R", 0x6b30}
	};
	for (const auto& b: locks) { S::Import(core, b.slot, b.symbol, np->base_vaddr + b.rva, true); }
	// Coherent actual HLE flexible heap implementation, not NpCommon's unrelated
	// heap implementation mixed with HLE arena callbacks. No success-only stubs.
	S::Import(core, 0x2068ab0, "kZizwrFvWZY#R#R", reinterpret_cast<uint64_t>(&Libs::NpMemoryHeap::Init), true);
	S::Import(core, 0x2068ab8, "dfXSH2Tsjkw#R#R", reinterpret_cast<uint64_t>(&Libs::NpMemoryHeap::Destroy), true);
	S::Import(core, 0x2068ac0, "FaMNvjMA6to#R#R", reinterpret_cast<uint64_t>(&Libs::NpMemoryHeap::MakeAllocator), true);
	// Already established by service fixture; validate again without mutation.
	for (const auto& b: S::NativeImports) {
		if (b.slot == 0x2069288 || b.slot == 0x2069270 || b.slot == 0x2068980 || b.slot == 0x2068988) {
			S::Import(core, b.slot, b.symbol, np->base_vaddr + b.rva, false);
		}
	}
	struct KernelBinding { uint32_t slot; const char* symbol; uint64_t target; };
	const KernelBinding kernel[] {
		{0x49490, "F8bUHwAG284#A#B", reinterpret_cast<uint64_t>(&K::PthreadMutexattrInit)},
		{0x49498, "iMp8QpE+XO4#A#B", reinterpret_cast<uint64_t>(&K::PthreadMutexattrSettype)},
		{0x494a0, "cmo1RIYva9o#A#B", reinterpret_cast<uint64_t>(&K::PthreadMutexInit)},
		{0x494a8, "smWEktiyyG0#A#B", reinterpret_cast<uint64_t>(&K::PthreadMutexattrDestroy)},
		{0x494b0, "2Of0f+3mhhE#A#B", reinterpret_cast<uint64_t>(&K::PthreadMutexDestroy)},
		{0x494b8, "9UK1vLZQft4#A#B", reinterpret_cast<uint64_t>(&K::PthreadMutexLock)},
		{0x494c8, "tn3VlD0hG60#A#B", reinterpret_cast<uint64_t>(&K::PthreadMutexUnlock)}
	};
	// Import accepts the parent's TestTraceImportTarget registered wrappers.
	for (const auto& b: kernel) { S::Import(np, b.slot, b.symbol, b.target, false); }
}
} // namespace NpNativeNotificationTest

void TestNpNativeNotification(Loader::Program* core, Loader::Program* np) {
	using namespace NpNativeNotificationTest;
	S::LiveState live;
	static bool used = false;
	Need(!used, "notification fixture is single-use in isolated service process");
	used = true;
	Need(core && np && Loader::RuntimeLinker::GetCurrentProgram() == core && K::PthreadSelfOrNull(),
	     "notification coordinator is admitted Core guest thread");
	const auto base = core->base_vaddr;
	Need(S::Read<uint8_t>(base + 0x213cad0) == 1 && S::Read<uint64_t>(base + 0x213cb00),
	     "parent actual IPC initialized before registry component");
	Need(S::Read<uint8_t>(base + 0x2133f40) == 0 && S::Read<uint64_t>(base + 0x213e928) == 0 &&
	     S::Read<uint64_t>(base + 0x213e838) == 0 && S::Read<uint32_t>(base + 0x213e840) == 0 &&
	     S::Read<uint64_t>(base + 0x213e848) == 0 && S::Read<uint8_t>(base + 0x213e854) == 0 &&
	     S::Read<uint8_t>(base + 0x213e860) == 0,
	     "fresh registry/heap/lock, no presence worker or enabled manager");
	Bind(core, np);
	Need(Call<int>(core, 0xce0510) == 0, "native presence recursive mutex initialized");
	Need(Call<int>(core, 0xce05a0, uint64_t{0x10000}) == 0, "native presence heap initializer uses real flexible heap");
	Need(Call<int>(core, 0xce00b0) == 0 && S::Read<uint64_t>(base + 0x213e848) == base + 0x213e888,
	     "native ce0610 allocator descriptor published; list allocation is lazy");
	State s {core, Loader::GetCurrentGuestProcessId(), K::PthreadGetthreadid()};
	Need(s.pid > 0 && Call<int>(core, 0xce0140, &Observe, static_cast<void*>(&s), &s.id) == 0 && s.id > 0,
	     "actual native observer registration and ID");
	Need(S::Read<uint32_t>(base + 0x213e840) == 1, "one actual list entry registered");
	K::Pthread producer = nullptr, remover = nullptr;
	Need(K::PthreadCreate(&producer, nullptr, Produce, &s, "NpNotifyProduce") == 0 && producer, "create actual native producer guest thread");
	Await([&] { return s.entered.load(); }, "native observer entry deadline");
	Need(s.self_case_identified.load(), "observer identifies unsafe self-unregister without executing it");
	Need(K::PthreadCreate(&remover, nullptr, Remove, &s, "NpNotifyRemove") == 0 && remover, "create actual foreign unregister guest thread");
	Await([&] { return s.attempted.load(); }, "foreign remover contention observation deadline");
	// Bounded negative observation plus real Busy trylock and the verified native
	// lock-before-unlink instruction path. A pre-call flag alone is not claimed
	// to expose the scheduler's precise blocked-PC state.
	const auto observe_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
	do {
		Need(!s.unregistered.load() && !s.producer_done.load(), "unregister/broadcast cannot finish with observer held in flight");
		K::PthreadYield();
	} while (std::chrono::steady_clock::now() < observe_until);
	s.release.store(true);
	Await([&] { return s.producer_done.load() && s.remover_done.load(); }, "producer/remover completion deadline");
	Need(K::PthreadJoin(producer, nullptr) == 0 && K::PthreadJoin(remover, nullptr) == 0,
	     "actual guest joins precede all registry storage cleanup");
	Need(s.calls.load() == 1 && s.unregistered.load() && S::Read<uint32_t>(base + 0x213e840) == 0,
	     "successful native unlink and no post-unlink callback");
	const auto sentinel = S::Read<uint64_t>(base + 0x213e838);
	Need(sentinel && S::Read<uint64_t>(sentinel) == sentinel && S::Read<uint64_t>(sentinel + 8) == sentinel,
	     "empty native list sentinel before destructor");
	// ce00d0 is a no-op, NOT storage cleanup. ce0040 is the actual list
	// destructor and frees the empty sentinel via the still-live allocator.
	Call<void>(core, 0xce0040, reinterpret_cast<void*>(base + 0x213e838));
	// Destructor leaves stale fields. Execute the exact native list constructor
	// to reset them, before retiring the heap. It also registers the normal CXA
	// destructor, now harmless on an empty/null list. Parent retains Core until
	// process exit; no manual writes, skipped free or fabricated heap state.
	Need(Call<int>(core, 0xce04d0) == 0, "native list reset and real CXA registration");
	Need(S::Read<uint64_t>(base + 0x213e838) == 0 && S::Read<uint64_t>(base + 0x213e848) == 0,
	     "native reset removes stale list/allocator pointers");
	const auto retired_handle = S::Read<Libs::NpMemoryHeap::Heap>(base + 0x213e868).handle;
	Need(retired_handle != nullptr, "presence heap generation exists before real destruction");
	Call<void>(core, 0xce05e0); // HLE Destroy is void; do not invent a return status.
	const auto heap = S::Read<Libs::NpMemoryHeap::Heap>(base + 0x213e868);
	Need(!heap.handle && !heap.mapping && !Libs::NpMemoryHeap::HasRetainedGeneration(retired_handle) &&
	     S::Read<uint8_t>(base + 0x213e860) == 0,
	     "actual presence heap/mapping destroyed");
	Need(Call<int>(core, 0xce0550) == 0 && S::Read<uint8_t>(base + 0x213e854) == 0,
	     "native mutex destroyed only after joins and list/heap cleanup");
	Need(S::Read<uint8_t>(base + 0x2133f40) == 0, "manager remains genuinely uninitialized");
	live.armed = false;
	Check("NpNativeNotification", true, "native registry foreign-thread barrier/unlink/cleanup; host observer only");
}
