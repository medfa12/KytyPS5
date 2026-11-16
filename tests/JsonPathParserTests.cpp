#include "common/abi.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/json2PathTesting.h"
#include "libs/json2Testing.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <new>
#include <string>

namespace Libs::LibJson2 { void InitNet_1_Json2(Loader::SymbolDatabase*); }
namespace Libs::LibJson2Iter { void InitJson2Iter_1(Loader::SymbolDatabase*); }
namespace Libs::ShellHits { void InitShellHits_1(Loader::SymbolDatabase*); }

namespace {
namespace FileSystem = Libs::LibKernel::FileSystem;
namespace Testing = Libs::LibJson2::Testing;
using namespace Libs::LibKernel;
constexpr int32_t ParseError = -2138799871;
constexpr int32_t NoMemory = -2138799870;
constexpr int32_t NoFile = -2138799869;
constexpr int32_t InvalidArgument = -2138799840;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "JsonPathParserTests: failed: %s\n", message);
		std::abort();
	}
}

// Guest ABI storage only, without access to the HLE's maps or vectors.
struct alignas(8) Value { std::byte storage[32]; };
struct String { void* opaque; };
struct Object { void* opaque; };
struct Iterator { void* opaque; };
struct Pair { String key; std::byte padding[8]; Value value; };
static_assert(sizeof(Value) == 32 && offsetof(Pair, value) == 16);
struct GuardedValue {
	uint64_t before = 0x123456789abcdef0ULL;
	Value value {};
	uint64_t after = 0xfedcba9876543210ULL;
	void Verify() const {
		Check(before == 0x123456789abcdef0ULL && after == 0xfedcba9876543210ULL, "destination ABI canaries");
	}
};

Loader::SymbolResolve Identity(const char* nid, const char* module = "Json2") {
	return {nid, "Json2", 1, module, 1, 1, Loader::SymbolType::Func};
}
template <typename T> T Resolve(const Loader::SymbolDatabase& db, const char* nid, const char* module = "Json2") {
	const auto* record = db.Find(Identity(nid, module));
	Check(record && record->vaddr, "exact versioned registered ABI exists");
	return reinterpret_cast<T>(record->vaddr);
}
struct Api {
	using Ctor = void (KYTY_SYSV_ABI *)(Value*);
	using Path = int32_t (KYTY_SYSV_ABI *)(Value*, const char*); // No RDX/size parameter.
	using Text = int32_t (KYTY_SYSV_ABI *)(Value*, const char*, size_t);
	using Index = const Value* (KYTY_SYSV_ABI *)(const Value*, const char*);
	using ArrayIndex = const Value* (KYTY_SYSV_ABI *)(const Value*, uint64_t);
	using Type = uint32_t (KYTY_SYSV_ABI *)(const Value*);
	using Count = int32_t (KYTY_SYSV_ABI *)(const Value*);
	using UInt = const uint64_t* (KYTY_SYSV_ABI *)(const Value*);
	using GetString = const String* (KYTY_SYSV_ABI *)(const Value*);
	using CStr = const char* (KYTY_SYSV_ABI *)(const String*);
	explicit Api(const Loader::SymbolDatabase& db):
	    ctor(Resolve<Ctor>(db, "qBMjqyBn3OM")), dtor(Resolve<Ctor>(db, "WTtYf+cNnXI")),
	    path(Resolve<Path>(db, "LB3jxppxyKU", "Json")), text(Resolve<Text>(db, "S5JxQnoGF3E")),
	    index(Resolve<Index>(db, "HwDt5lD9Bfo")), array_index(Resolve<ArrayIndex>(db, "XlWbvieLj2M")),
	    type(Resolve<Type>(db, "SHtAad20YYM")), count(Resolve<Count>(db, "RBw+4NukeGQ")),
	    number(Resolve<UInt>(db, "sn4HNCtNRzY", "Json")),
	    string(Resolve<GetString>(db, "epJ6x2LV0kU")), cstr(Resolve<CStr>(db, "L1KAkYWml-M")) {}
	Ctor ctor, dtor;
	Path path;
	Text text;
	Index index;
	ArrayIndex array_index;
	Type type;
	Count count;
	UInt number;
	GetString string;
	CStr cstr;
};

struct IoProbe {
	int reads = 0;
	int closes = 0;
	int eof = 0;
	int fd = -1;
	int fail_read = -1;
	size_t read_limit = SIZE_MAX;
	bool fail_close = false;
	int materialize_budget = -1;
};
thread_local IoProbe probe;

int64_t KYTY_SYSV_ABI Read(int fd, void* buffer, size_t size) {
	probe.fd = fd;
	const int call = probe.reads++;
	// Deliberately pass a null buffer to the REAL kernel API to produce EFAULT.
	// This isolates propagation at the adapter boundary, not host disk-fault handling.
	const auto result = FileSystem::KernelRead(fd, call == probe.fail_read ? nullptr : buffer,
	                                         (std::min)(size, probe.read_limit));
	if (result == 0) { ++probe.eof; }
	return result;
}
int KYTY_SYSV_ABI Close(int fd) {
	++probe.closes;
	probe.fd = fd;
	Check(FileSystem::KernelClose(fd) == 0, "adapter closes its still-valid real descriptor exactly once");
	// Only the reported close result is injected; the descriptor is really closed.
	return probe.fail_close ? KERNEL_ERROR_EIO : 0;
}
void BeforeMaterialize() {
	if (probe.materialize_budget == 0) { throw std::bad_alloc(); }
	if (probe.materialize_budget > 0) { --probe.materialize_budget; }
}
struct Hooks {
	Hooks() { Testing::SetPathParserHooks({Read, Close, BeforeMaterialize}); }
	~Hooks() { Testing::SetPathParserHooks({}); }
};
void CheckClosed(int count) {
	Check(probe.closes == count, "expected adapter close count");
	if (count != 0) {
		FileSystem::FileStat stat {};
		Check(probe.fd >= 3 && FileSystem::KernelFstat(probe.fd, &stat) == KERNEL_ERROR_EBADF,
		      "descriptor is absent after return");
	}
}
void Write(const std::filesystem::path& file, const std::string& bytes) {
	std::ofstream out(file, std::ios::binary);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	Check(out.good(), "write isolated host fixture input");
}
void CheckOwnership(const Value* value, const Value* parent, const void* rootparam) {
	const auto ownership = Testing::GetValueOwnership(value);
	Check(ownership.parent == parent && ownership.rootparam == rootparam,
	      "actual HLE parent/rootparam fields match expected ownership");
}
void CheckNested(const Api& api, const Value* root, const std::string& payload) {
	Check(api.type(root) == 7 && api.count(root) == 2, "nested file became an object");
	const auto* nested = api.index(root, "nested");
	CheckOwnership(nested, root, nullptr);
	CheckOwnership(api.index(root, "payload"), root, nullptr);
	Check(api.type(nested) == 7 && *api.number(api.index(nested, "n")) == 17, "nested integer");
	const auto* items = api.index(nested, "items");
	CheckOwnership(items, nested, nullptr);
	Check(api.type(items) == 6 && api.count(items) == 3 && api.type(api.array_index(items, 0)) == 1 &&
	          api.type(api.array_index(items, 1)) == 0, "nested bool/null array");
	const auto* label = api.index(api.array_index(items, 2), "label");
	Check(std::strcmp(api.cstr(api.string(label)), "kept") == 0 &&
	          std::string(api.cstr(api.string(api.index(root, "payload")))) == payload,
	      "owned nested strings and multichunk payload");
}
void CheckArray(const Api& api, const Value* root) {
	Check(api.type(root) == 6 && api.count(root) == 3, "top-level file array has three elements");
	const auto* object = api.array_index(root, 0);
	const auto* array = api.array_index(root, 1);
	const auto* null = api.array_index(root, 2);
	for (const auto* child: {object, array, null}) { CheckOwnership(child, root, nullptr); }
	Check(api.type(object) == 7 && api.type(array) == 6 && api.type(null) == 0 &&
	          *api.number(api.index(object, "n")) == 17 &&
	          std::strcmp(api.cstr(api.string(api.array_index(array, 0))), "kept") == 0,
	      "moved file array retains nested object, array and null values");
	CheckOwnership(api.index(object, "n"), object, nullptr);
	CheckOwnership(api.array_index(array, 0), array, nullptr);
}

void CheckResolution(const Loader::SymbolDatabase& db) {
	const auto* exact = db.Find(Identity("LB3jxppxyKU", "Json"));
	const auto* compat = db.Find(Identity("LB3jxppxyKU"));
	const auto* first = db.FindByNid("LB3jxppxyKU", Loader::SymbolType::Func);
	const auto* name = db.FindByName("LB3jxppxyKU[Json2_v1]", Loader::SymbolType::Func);
	Check(exact && compat && first && name && exact->vaddr == compat->vaddr && exact->vaddr == first->vaddr &&
	          exact->vaddr == name->vaddr && exact->dbg_name == "LibJson2::JsonParserParseVsh" &&
	          db.FindRegistration(Identity("LB3jxppxyKU", "Json"), exact->dbg_name) &&
	          db.FindRegistration(Identity("LB3jxppxyKU"), exact->dbg_name),
	      "exact, compatibility and historical fallbacks agree after all registrars");
}

void RunCases(const Loader::SymbolDatabase& db, const Api& api, const std::filesystem::path& root) {
	const std::string payload(4096 * 3 + 37, 'x');
	const std::string text = "{\"nested\":{\"n\":17,\"items\":[true,null,{\"label\":\"kept\"}]},\"payload\":\"" + payload + "\"}";
	Write(root / "valid.json", text);
	Write(root / "malformed.json", "{\"partial\":[1,2,}");
	Write(root / "truncated.json", "{\"partial\":\"unterminated");
	Write(root / "empty.json", "");
	Write(root / "whitespace.json", " \r\n\t");
	Write(root / "nul.json", std::string("{}\0junk", 7));
	Write(root / "terminal-nul.json", std::string("{}\0", 3));
	Write(root / "garbage.json", "{}x");
	Write(root / "second-document.json", "{} []");
	Write(root / "trailing-whitespace.json", text + " \r\n\t");
	// Place NUL at both sides of a 4096-byte transport boundary, either as
	// the final byte or followed by another document. Each prefix is valid JSON.
	for (const size_t offset: {size_t {4095}, size_t {4096}}) {
		const std::string prefix = "{}" + std::string(offset - 2, ' ');
		Write(root / ("terminal-nul-" + std::to_string(offset) + ".json"), prefix + '\0');
		Write(root / ("embedded-nul-" + std::to_string(offset) + ".json"), prefix + '\0' + "{}");
	}
	Write(root / "scalar.json", "null");
	Write(root / "array.json", "[{\"n\":17},[\"kept\"],null]");
	Write(root / "boundary.json", "\"" + std::string(4094, 'a') + "\xc3\xa9\"");
	FileSystem::Mount(root, "/json-path-stage", true);
	struct MountCleanup { ~MountCleanup() { FileSystem::Umount("/json-path-stage"); } } mount_cleanup;
	FileSystem::ScopedGuestFsContext context(FileSystem::CreateBootstrapFsContext());
	Check(FileSystem::ChangeDirectory("/json-path-stage") == 0, "isolated guest relative cwd");
	Check(FileSystem::KernelOpen("valid.json", 1, 0) == KERNEL_ERROR_EROFS, "mount enforces readonly policy");
	// First exercise the production default (no read/close test replacements).
	const int free_slot = FileSystem::KernelOpen("valid.json", 0, 0);
	Check(free_slot >= 3 && FileSystem::KernelClose(free_slot) == 0, "record free descriptor slot");
	GuardedValue control;
	api.ctor(&control.value);
	Check(api.path(&control.value, "valid.json") == 0, "unhooked registered path adapter reads real guest file");
	CheckNested(api, &control.value, payload);
	api.dtor(&control.value);
	control.Verify();
	const int reused_slot = FileSystem::KernelOpen("valid.json", 0, 0);
	Check(reused_slot == free_slot && FileSystem::KernelClose(reused_slot) == 0,
	      "unhooked adapter releases its real descriptor slot");
	GuardedValue dst;
	api.ctor(&dst.value);
	constexpr char sentinel[] = "{\"sentinel\":42}";
	Check(api.text(&dst.value, sentinel, sizeof(sentinel) - 1) == 0, "seed destination via size overload");
	const auto* borrowed = api.index(&dst.value, "sentinel");
	const Value snapshot = dst.value;
	const auto preserved = [&] {
		Check(std::memcmp(&dst.value, &snapshot, sizeof(Value)) == 0 &&
		          api.index(&dst.value, "sentinel") == borrowed && *api.number(borrowed) == 42,
		      "failure preserves destination and borrowed member identity (HLE contract)");
		dst.Verify();
	};
	Hooks hooks;
	for (const auto* path: {"missing.json", "malformed.json", "truncated.json", "empty.json", "whitespace.json", "nul.json",
	                       "terminal-nul.json", "garbage.json", "second-document.json", "terminal-nul-4095.json",
	                       "terminal-nul-4096.json", "embedded-nul-4095.json", "embedded-nul-4096.json"}) {
		probe = {};
		const auto result = api.path(&dst.value, path);
		const bool missing = std::strcmp(path, "missing.json") == 0;
		Check(result < 0 && result == (missing ? NoFile : ParseError), "truthful missing/malformed/trailing-data/EOF result");
		if (std::strstr(path, "nul-") != nullptr) {
			Check(probe.reads == (std::strstr(path, "4096") != nullptr ? 2 : 1) && probe.eof == 0,
			      "NUL at either side of a chunk boundary is rejected without pretending it was kernel EOF");
		}
		CheckClosed(missing ? 0 : 1);
		preserved();
	}
	probe = {};
	Check(api.path(&dst.value, nullptr) == InvalidArgument && api.path(nullptr, "valid.json") == InvalidArgument,
	      "null arguments fail rather than manufacture JSON");
	CheckClosed(0);
	preserved();
	probe = {};
	const int open_error = FileSystem::KernelOpen("valid.json/child", 0, 0x16d);
	Check(open_error == KERNEL_ERROR_ENOTDIR && api.path(&dst.value, "valid.json/child") == open_error,
	      "non-ENOENT open error passes through unchanged");
	CheckClosed(0);
	preserved();
	// A JSON literal is a path for LB3, but source text for S5.
	probe = {};
	Check(api.path(&dst.value, "{}") == NoFile &&
	          api.text(&dst.value, "valid.json", sizeof("valid.json") - 1) == ParseError,
	      "path/text overloads are distinct, with failures returned to the guest");
	CheckClosed(0);
	preserved();
	for (int fail_read: {0, 1, 4}) {
		probe = {};
		probe.fail_read = fail_read;
		probe.fail_close = true;
		Check(api.path(&dst.value, "valid.json") == KERNEL_ERROR_EFAULT && probe.reads == fail_read + 1,
		      "real kernel EFAULT on initial/later/final read is retained over EOF, parse and close results");
		CheckClosed(1);
		preserved();
	}
	probe = {};
	probe.fail_close = true;
	Check(api.path(&dst.value, "malformed.json") == ParseError, "close failure cannot replace malformed JSON error");
	CheckClosed(1);
	preserved();
	// Throw before successive recursive materializations, including after owned
	// Pairs/array elements exist. Only these checkpoints are injected: no failures
	// in wrapper construction, container insertion, DOM teardown or filesystem
	// allocation are tested. This is not general OOM/descriptor-cleanup proof.
	for (int budget = 0; budget < 9; ++budget) {
		probe = {};
		probe.materialize_budget = budget;
		const auto before = Testing::GetIteratorAllocations();
		Check(api.path(&dst.value, "valid.json") == NoMemory, "recursive materialization checkpoint returns NOMEM");
		const auto after = Testing::GetIteratorAllocations();
		Check(after.pairs_allocated - before.pairs_allocated == after.pairs_released - before.pairs_released,
		      "failed conversion releases every actual temporary owned Pair");
		CheckClosed(1);
		preserved();
	}
	for (const char* module: {"Json", "Json2"}) {
		probe = {};
		probe.fail_close = true;
		char path[] = "/json-path-stage//valid.json";
		const std::string original(path);
		Check(Resolve<Api::Path>(db, "LB3jxppxyKU", module)(&dst.value, path) == 0 && original == path,
		      "actual two-argument aliases load file without changing caller path");
		Check(probe.reads > 3 && probe.eof == 1, "multichunk input reaches real kernel EOF exactly once");
		CheckClosed(1);
		CheckNested(api, &dst.value, payload);
		dst.Verify();
	}
	probe = {};
	probe.read_limit = 7;
	Check(api.path(&dst.value, "valid.json") == 0 && probe.eof == 1 && probe.reads > 100,
	      "short positive reads refill until actual EOF");
	CheckClosed(1);
	CheckNested(api, &dst.value, payload);
	probe = {};
	Check(api.path(&dst.value, "trailing-whitespace.json") == 0 && probe.eof == 1,
	      "valid trailing whitespace is consumed through real kernel EOF");
	CheckClosed(1);
	CheckNested(api, &dst.value, payload);
	probe = {};
	Check(api.path(&dst.value, "boundary.json") == 0 &&
	          std::string(api.cstr(api.string(&dst.value))) == std::string(4094, 'a') + "\xc3\xa9",
	      "UTF-8 token spans transport chunks without corruption");
	CheckClosed(1);
	probe = {};
	Check(api.path(&dst.value, "scalar.json") == 0 && api.type(&dst.value) == 0, "valid null differs from empty-file failure");
	CheckClosed(1);
	probe = {};
	Check(api.path(&dst.value, "array.json") == 0 && probe.eof == 1, "registered path ABI loads a top-level array");
	CheckOwnership(&dst.value, nullptr, nullptr);
	CheckArray(api, &dst.value); // Checks the array commit branch's actual reparenting.
	CheckClosed(1);
	dst.Verify();
	std::array<char, 5> bounded_text {'[', '7', ']', 'x', '!'};
	const auto bounded_before = bounded_text;
	Check(api.text(&dst.value, bounded_text.data(), 3) == 0 && bounded_text == bounded_before &&
	          api.count(&dst.value) == 1 && *api.number(api.array_index(&dst.value, 0)) == 7,
	      "size overload still honors explicit text extent and leaves input unchanged");
	api.dtor(&dst.value);
	dst.Verify();

	// Replace an inline borrowed Pair value while retaining its owner and a
	// pre-existing Object iterator. The parent object must continue to own it.
	api.ctor(&dst.value);
	constexpr char container[] = "{\"sibling\":9,\"slot\":{\"old\":1}}";
	Check(api.text(&dst.value, container, sizeof(container) - 1) == 0, "seed borrowed destination");
	auto* slot = const_cast<Value*>(api.index(&dst.value, "slot"));
	CheckOwnership(slot, &dst.value, nullptr); // Naturally set by the registered size overload.
	uint64_t root_token = 0x8c77f01d4a9082e3ULL;
	Testing::SetValueRootParamForTest(slot, &root_token);
	CheckOwnership(slot, &dst.value, &root_token);
	using GetObject = const Object* (KYTY_SYSV_ABI *)(const Value*);
	using Begin = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Object*);
	using Increment = Iterator* (KYTY_SYSV_ABI *)(Iterator*);
	using Arrow = Pair* (KYTY_SYSV_ABI *)(const Iterator*);
	using Destroy = void (KYTY_SYSV_ABI *)(Iterator*);
	Iterator it {};
	Resolve<Begin>(db, "xhAcaIwnrgk", "Json")(&it, Resolve<GetObject>(db, "IlsmvBtMkak")(&dst.value));
	Resolve<Increment>(db, "DlWmn2ZQuWY", "Json")(&it);
	const auto arrow = Resolve<Arrow>(db, "EnesV4gXDtE", "Json");
	const auto* pair = arrow(&it);
	Check(pair && &pair->value == slot, "iterator borrows actual slot Pair");
	probe = {};
	Check(api.path(slot, "valid.json") == 0 && api.index(&dst.value, "slot") == slot && arrow(&it) == pair &&
	          *api.number(api.index(&dst.value, "sibling")) == 9, "commit preserves owner, Pair identity and sibling");
	CheckOwnership(slot, &dst.value, &root_token);
	CheckNested(api, &pair->value, payload);
	CheckClosed(1);
	// Exercise the other reparent branch with the same borrowed destination and
	// non-null opaque metadata token; no native root/callback behavior is implied.
	probe = {};
	Check(api.path(slot, "array.json") == 0 && arrow(&it) == pair && api.index(&dst.value, "slot") == slot,
	      "borrowed Pair also retains identity when replaced with a file array");
	CheckOwnership(slot, &dst.value, &root_token);
	CheckArray(api, &pair->value);
	Check(*api.number(api.index(&dst.value, "sibling")) == 9 && root_token == 0x8c77f01d4a9082e3ULL,
	      "sibling and non-dereferenced root metadata token remain untouched");
	CheckClosed(1);
	Resolve<Destroy>(db, "hoINmSMlYjI", "Json")(&it);
	api.dtor(&dst.value);
	dst.Verify();

	// Same guest spelling selects the active program's scoped mount.
	std::filesystem::create_directory(root / "scoped");
	Write(root / "scoped/valid.json", "23");
	Loader::Program program;
	program.unique_id = 5243;
	FileSystem::MountForProgram(root / "scoped", "/json-path-stage", program.unique_id);
	auto* previous_program = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::RuntimeLinker::SetCurrentProgram(&program);
	api.ctor(&dst.value);
	probe = {};
	Check(api.path(&dst.value, "valid.json") == 0 && *api.number(&dst.value) == 23,
	      "path adapter follows active program mount scope and guest relative cwd");
	CheckClosed(1);
	api.dtor(&dst.value);
	Loader::RuntimeLinker::SetCurrentProgram(previous_program);
	// Confirm the original fixture bytes survived all reads and readonly accesses.
	std::ifstream original(root / "valid.json", std::ios::binary);
	Check(std::string(std::istreambuf_iterator<char>(original), {}) == text, "source file bytes remain unchanged");
}
} // namespace

// Main owns runner/CMake integration. Call only with the real filesystem and
// hidden Vulkan context initialized, and a disposable host directory.
void TestJsonPathParser(const std::filesystem::path& root) {
	Loader::SymbolDatabase db;
	Libs::LibJson2Iter::InitJson2Iter_1(&db);
	Libs::LibJson2::InitNet_1_Json2(&db);
	Libs::ShellHits::InitShellHits_1(&db);
	CheckResolution(db);
	const Api api(db);
	const auto before = Testing::GetIteratorAllocations();
	RunCases(db, api, root);
	const auto after = Testing::GetIteratorAllocations();
	Check(after.pairs_allocated > before.pairs_allocated &&
	          after.pairs_allocated - before.pairs_allocated == after.pairs_released - before.pairs_released &&
	          after.allocated - before.allocated == after.released - before.released,
	      "fixture releases all actual owned Pairs and iterator states");
}
