// Real regular-image execution through production-filtered exports. No Core
// initializer, credentials, service adapter or online backend participates.
namespace NpNativeEventQueueTest {
namespace K = Libs::LibKernel;
constexpr const char* Name = "NpNativeEventQueue";
constexpr int SizeError = static_cast<int>(0x80559e11u);
constexpr int Cancelled = static_cast<int>(0x80559e14u);
constexpr int Empty = static_cast<int>(0x80559e16u);
constexpr int AllocationError = static_cast<int>(0x8055000cu);
void Require(bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "%s: %s\n", Name, message); std::abort(); }
}
struct Allocator {
	void* (KYTY_SYSV_ABI *allocate)(uint64_t, void*);
	void* (KYTY_SYSV_ABI *reallocate)(void*, uint64_t, void*);
	void (KYTY_SYSV_ABI *free)(void*, void*);
	void* context;
	int error = AllocationError;
};
struct Memory {
	std::map<void*, uint64_t> live;
	bool fail = false;
	unsigned frees = 0;
};
void* KYTY_SYSV_ABI Allocate(uint64_t size, void* context) {
	auto& memory = *static_cast<Memory*>(context);
	if (memory.fail) { return nullptr; }
	void* result = std::malloc(size);
	Require(result != nullptr, "fixture allocation");
	memory.live.emplace(result, size);
	return result;
}
void* KYTY_SYSV_ABI Reallocate(void*, uint64_t, void*) {
	Require(false, "queue unexpectedly reallocated backing"); return nullptr;
}
void KYTY_SYSV_ABI Free(void* address, void* context) {
	if (!address) { return; }
	auto& memory = *static_cast<Memory*>(context);
	Require(memory.live.erase(address) == 1, "free must return exact original allocation once");
	++memory.frees;
	std::free(address);
}
using Item = std::array<uint64_t, 4>; // Core CtlCmdQueue's 0x20-byte item ABI
struct Api {
	Loader::Program* provider;
	template<class R, class... A> R Call(const char* nid, A... args) const {
		const auto* symbol = provider->export_symbols->FindByNid(nid, Loader::SymbolType::Func);
		Require(symbol && symbol->vaddr >= provider->base_vaddr && symbol->vaddr < provider->base_vaddr + 0x64000,
		        "selected function must be the mapped native provider");
		return reinterpret_cast<R (KYTY_SYSV_ABI *)(A...)>(symbol->vaddr)(args...);
	}
	int Enqueue(void* queue, const Item& item, uint32_t timeout = 20000) const {
		return Call<int>("AV5jHo8O3+E", queue, item.data(), uint64_t(sizeof(Item)), timeout);
	}
	int TryDequeue(void* queue, Item& item) const {
		return Call<int>("QlP4t2SGZ4I", queue, item.data(), uint64_t(sizeof(Item)));
	}
	bool IsEmpty(void* queue) const { return Call<bool>("jxPY-0x8e-M", queue); }
};
struct Worker {
	Api api;
	void* queue;
	bool producer;
	Item item {9, 8, 7, 6};
	std::atomic_bool entered {false}, done {false};
	int result = -1;
};
void* KYTY_SYSV_ABI Entry(void* argument) {
	auto& w = *static_cast<Worker*>(argument);
	w.entered.store(true);
	w.result = w.producer ? w.api.Enqueue(w.queue, w.item, 2000000) :
	    w.api.Call<int>("suxln7PooIo", w.queue, w.item.data(), uint64_t(sizeof(Item)), uint32_t(2000000));
	w.done.store(true);
	return nullptr;
}
K::Pthread Start(Worker& worker) {
	K::Pthread thread = nullptr;
	Require(K::PthreadCreate(&thread, nullptr, Entry, &worker, "NpEventQueueProbe") == 0 && thread, "actual queue worker");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (!worker.entered.load() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
	Require(worker.entered.load(), "worker entered");
	const auto observation = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
	while (!worker.done.load() && std::chrono::steady_clock::now() < observation) { std::this_thread::yield(); }
	Require(!worker.done.load(), "native queue must wait for data/space, not acknowledge early");
	return thread;
}
void Join(K::Pthread thread) { Require(K::PthreadJoin(thread, nullptr) == 0, "join before queue destruction/unmap"); }
void Environment(const char* key, const char* value) {
#if defined(_WIN32)
	Require(_putenv_s(key, value) == 0, "fixture environment");
#else
	Require(setenv(key, value, 1) == 0, "fixture environment");
#endif
}
} // namespace NpNativeEventQueueTest

void TestNpNativeEventQueue() {
	using namespace NpNativeEventQueueTest;
	const char* path = std::getenv("KYTY_TEST_NP_ORIGINAL_IMAGE");
	Require(path && *path, "explicit regular NpCommon image required");
	Environment("KYTY_NATIVE_NP_ALTERNATE_STARTUP", "");
	for (const char* key: {"KYTY_NATIVE_NP_OWNERSHIP", "KYTY_NATIVE_NP_WORKQUEUE", "KYTY_NATIVE_NP_CALLOUT",
	                      "KYTY_NATIVE_NP_HTTP_POOL", "KYTY_NATIVE_NP_DISPATCHER", "KYTY_NATIVE_NP_EVENT_QUEUE"}) { Environment(key, "1"); }
	K::Initialize(); K::PthreadInitSelfForMainThread();
	auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitLibKernel_1(linker->Symbols());
	Libs::LibC::InitLibC_1(linker->Symbols());
	auto* provider = linker->LoadProgram(path);
	Require(provider && !provider->np_alternate54 && !provider->boot_credential_admission &&
	        provider->export_symbols->GetExportPolicy() == Loader::SymbolDatabase::ExportPolicy::NpEventQueue,
	        "loader must pin regular provider, select complete queue policy and grant no credentials");
	linker->RelocateProgram(provider);
	// Audit closure bindings, including exceptional-path dependencies that the
	// success fixture must not trigger (abort/stack failure). Reject unresolved
	// success stubs even when a particular test branch does not call them.
	for (const char* nid: {"2Of0f+3mhhE", "2Tb92quprl0", "9UK1vLZQft4", "BmMjYxmew1w", "F8bUHwAG284",
	                      "JGgj7Uvrl+A", "L1SBTkC+Cvw", "Ou3iL1abvng", "Q3VBxCXhUHs", "WKAXJ4XBPQ4",
	                      "cmo1RIYva9o", "eLdDw6l0-bU", "g+PZd2hiacg", "hcuQgD53UxM", "iMp8QpE+XO4",
	                      "kDh-NfxgMtE", "m5-2bsNfv7s", "smWEktiyyG0", "tn3VlD0hG60", "waPcxYiR3WA"}) {
		const auto* expected = linker->Symbols()->FindByNid(nid, Loader::SymbolType::Func);
		Require(expected && expected->vaddr, "queue dependency has an actual HLE registration");
		bool bound = false;
		const auto& dynamic = *provider->dynamic_info;
		for (size_t i = 0; i < dynamic.jmprela_table_size / sizeof(Loader::Elf64_Rela); ++i) {
			const auto& relocation = dynamic.jmprela_table[i];
			const auto& symbol = dynamic.symbol_table[relocation.GetSymbol()];
			const std::string_view name(dynamic.str_table + symbol.st_name);
			if (name.substr(0, name.find('#')) != nid) { continue; }
			uint64_t target = 0;
			std::memcpy(&target, reinterpret_cast<const void*>(provider->base_vaddr + relocation.r_offset), sizeof(target));
			Require(target == expected->vaddr || Loader::TestTraceImportTarget(target, expected->vaddr), "actual native queue dependency binding");
			bound = true;
		}
		Require(bound, "audited queue dependency relocation present");
	}
	Api api {provider};
	Memory memory;
	Allocator allocator {Allocate, Reallocate, Free, &memory};
	alignas(16) std::array<uint8_t, 0xa0> queue {};
	api.Call<void>("esiO4He2WTU", queue.data(), &allocator);
	Require(api.IsEmpty(queue.data()) && !api.Call<bool>("VI8AHrfLdqY", queue.data()), "constructed empty/uninitialized state");
	memory.fail = true;
	Require(api.Call<int>("b20e017Ei94", queue.data(), "NpQueueFail", uint64_t(sizeof(Item)), uint64_t(2)) == AllocationError,
	        "allocator failure propagates through native initialization rollback");
	Require(memory.live.empty() && !api.Call<bool>("VI8AHrfLdqY", queue.data()), "failed initialization owns no backing");
	memory.fail = false;
	Require(api.Call<int>("b20e017Ei94", queue.data(), "NpQueueTest", uint64_t(sizeof(Item)), uint64_t(2)) == 0,
	        "retry after rollback initializes actual mutex/conditions/ring buffer");
	Require(memory.live.size() == 1 && memory.live.begin()->second == 2 * sizeof(Item), "allocator owns bounded queue backing");
	Require(api.Call<int>("b20e017Ei94", queue.data(), "NpQueueTwice", uint64_t(sizeof(Item)), uint64_t(2)) ==
	        static_cast<int>(0x80559e03u), "double initialization rejected");
	Item a {1, 2, 3, 4}, b {5, 6, 7, 8}, output {99, 99, 99, 99};
	const auto sentinel = output;
	Require(api.TryDequeue(queue.data(), output) == Empty && output == sentinel, "empty try-dequeue preserves output");
	Require(api.Call<int>("AV5jHo8O3+E", queue.data(), a.data(), uint64_t(8), uint32_t(20000)) == SizeError && api.IsEmpty(queue.data()),
	        "wrong-size enqueue preserves state");
	Require(api.Enqueue(queue.data(), a) == 0 && api.Enqueue(queue.data(), b) == 0 && !api.IsEmpty(queue.data()), "bounded FIFO writes");
	const auto begin = std::chrono::steady_clock::now();
	Require(api.Enqueue(queue.data(), a) == static_cast<int>(0x8055800bu) &&
	        std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(10), "full queue waits and returns timeout");
	Require(api.TryDequeue(queue.data(), output) == 0 && output == a, "first payload");
	Require(api.Enqueue(queue.data(), a) == 0, "ring-buffer wraparound");
	Require(api.TryDequeue(queue.data(), output) == 0 && output == b, "second payload");
	Require(api.TryDequeue(queue.data(), output) == 0 && output == a && api.IsEmpty(queue.data()), "wrapped payload and empty transition");
	Worker consumer {api, queue.data(), false};
	auto thread = Start(consumer);
	Require(api.Enqueue(queue.data(), b) == 0, "wake consumer by publishing actual item"); Join(thread);
	Require(consumer.result == 0 && consumer.item == b && api.IsEmpty(queue.data()), "consumer receives exact native bytes");
	Require(api.Enqueue(queue.data(), a) == 0 && api.Enqueue(queue.data(), b) == 0, "fill for blocked producer");
	Worker producer {api, queue.data(), true};
	thread = Start(producer);
	Require(api.TryDequeue(queue.data(), output) == 0 && output == a, "release one slot"); Join(thread);
	Require(producer.result == 0, "producer resumed after available space");
	// Core cleanup cancels both directions, then drains with TryDequeue/IsEmpty.
	Require(api.Call<int>("slmKkuIoC28", queue.data(), uint32_t(3)) == 0, "cancel both directions");
	Require(api.Enqueue(queue.data(), a) == Cancelled, "canceled producer retains failure");
	unsigned drained = 0;
	while (!api.IsEmpty(queue.data()) && drained < 3) {
		Require(api.TryDequeue(queue.data(), output) == 0 && output == (drained ? producer.item : b), "cancel retains drainable FIFO data");
		++drained;
	}
	Require(drained == 2 && api.IsEmpty(queue.data()), "Core-style cleanup terminates after actual retained items");
	api.Call<void>("aoas3bJANfY", queue.data(), uint32_t(3));
	Worker canceled {api, queue.data(), false};
	thread = Start(canceled);
	Require(api.Call<int>("slmKkuIoC28", queue.data(), uint32_t(3)) == 0, "cancel wakes blocked consumer"); Join(thread);
	Require(canceled.result == Cancelled, "cancel returns native error without an item");
	api.Call<void>("aoas3bJANfY", queue.data(), uint32_t(3));
	Require(api.Enqueue(queue.data(), a) == 0 && api.TryDequeue(queue.data(), output) == 0 && output == a, "clear cancellation restores use");
	api.Call<void>("qvpEuKumIGM", queue.data());
	Require(memory.live.empty() && !api.Call<bool>("VI8AHrfLdqY", queue.data()), "terminate returns backing and clears init");
	api.Call<void>("8kUkQPQP7bA", queue.data());
	Require(memory.frees == 1, "destructor after terminate does not double free");
	// Virtual deleting destruction must return both backing and prefixed object
	// to the SAME callback owner, including when termination was not called first.
	void* object = api.Call<void*>("0syNkhJANVw", uint64_t(0xa0), &allocator);
	Require(object, "native allocator-prefixed new");
	api.Call<void>("OGAIG7dVmUk", object, &allocator);
	Require(api.Call<int>("b20e017Ei94", object, "NpQueueDelete", uint64_t(sizeof(Item)), uint64_t(2)) == 0, "heap queue init");
	Require(api.Enqueue(object, a) == 0, "nonempty raw-byte queue destruction fixture");
	const auto deleting = reinterpret_cast<void (KYTY_SYSV_ABI *)(void*)>((*static_cast<uint64_t**>(object))[1]);
	deleting(object);
	Require(memory.live.empty() && memory.frees == 3, "virtual destruction returns object and ring to allocator exactly once");
	linker->Clear();
	std::printf("[native] %-48s ok\n", Name);
}
