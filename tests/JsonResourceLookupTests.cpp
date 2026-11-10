#include "common/abi.h"
#include "libs/json2Testing.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

namespace Libs::LibJson2 {
void InitNet_1_Json2(Loader::SymbolDatabase* symbols);
}
namespace Libs::ShellHits {
void InitShellHits_1(Loader::SymbolDatabase* symbols);
}
namespace Libs::LibJson2Iter {
void InitJson2Iter_1(Loader::SymbolDatabase* symbols);
}

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "JsonResourceLookupTests: failed: %s\n", message);
		std::abort();
	}
}

// Public guest ABI storage only. The fixture never reads host map/vector state
// or constructs values by filling implementation fields.
struct alignas(8) Value { std::byte storage[32]; };
struct String { void* opaque; };
struct Object { void* opaque; };
struct Array { void* opaque; };
struct Iterator { void* opaque; };
struct Pair {
	String key;
	std::byte padding[8];
	Value value;
};
static_assert(sizeof(Value) == 32 && sizeof(String) == 8 && sizeof(Iterator) == 8);
static_assert(offsetof(Pair, value) == 16 && sizeof(Pair) == 48);

Loader::SymbolResolve Identity(const char* nid, const char* module = "Json2") {
	return Loader::SymbolResolve {nid, "Json2", 1, module, 1, 1, Loader::SymbolType::Func};
}

template <typename Function>
Function Resolve(const Loader::SymbolDatabase& symbols, const char* nid, const char* module = "Json2") {
	const auto* record = symbols.Find(Identity(nid, module));
	Check(record != nullptr && record->vaddr != 0, "exact versioned function exists after ShellHits");
	return reinterpret_cast<Function>(record->vaddr);
}

struct Api {
	using ValueCtor = void (KYTY_SYSV_ABI *)(void*);
	using ValueDtor = void (KYTY_SYSV_ABI *)(Value*);
	using Parse = int32_t (KYTY_SYSV_ABI *)(Value*, const char*, size_t);
	using StringCtor = void (KYTY_SYSV_ABI *)(String*, const char*);
	using StringDtor = void (KYTY_SYSV_ABI *)(String*);
	using Get = const Value* (KYTY_SYSV_ABI *)(const Value*, const String*);
	using Index = const Value* (KYTY_SYSV_ABI *)(const Value*, const char*);
	using Refer = Value* (KYTY_SYSV_ABI *)(Value*, const String*);
	using Type = uint32_t (KYTY_SYSV_ABI *)(const Value*);
	using Count = int32_t (KYTY_SYSV_ABI *)(const Value*);
	using UInt = const uint64_t* (KYTY_SYSV_ABI *)(const Value*);
	using SetUInt = void (KYTY_SYSV_ABI *)(Value*, uint64_t);
	using GetString = const String* (KYTY_SYSV_ABI *)(const Value*);
	using CStr = const char* (KYTY_SYSV_ABI *)(const String*);
	using GetObject = const Object* (KYTY_SYSV_ABI *)(const Value*);
	// Non-trivial guest iterators use an explicit SysV result pointer.
	using BeginEnd = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Object*);
	using Copy = void (KYTY_SYSV_ABI *)(Iterator*, const Iterator*);
	using Compare = bool (KYTY_SYSV_ABI *)(const Iterator*, const Iterator*);
	using Arrow = Pair* (KYTY_SYSV_ABI *)(const Iterator*);
	using Increment = Iterator* (KYTY_SYSV_ABI *)(Iterator*);
	using IteratorDtor = void (KYTY_SYSV_ABI *)(Iterator*);

	explicit Api(const Loader::SymbolDatabase& symbols):
	    value_ctor(Resolve<ValueCtor>(symbols, "qBMjqyBn3OM")),
	    value_dtor(Resolve<ValueDtor>(symbols, "WTtYf+cNnXI")),
	    parse(Resolve<Parse>(symbols, "S5JxQnoGF3E")),
	    string_ctor(Resolve<StringCtor>(symbols, "9KUZFjI1IxA")),
	    string_dtor(Resolve<StringDtor>(symbols, "cG1VE2HMl6c")),
	    get(Resolve<Get>(symbols, "MsMOdxWfbwQ", "Json")),
	    index_string(Resolve<Get>(symbols, "clF7J7N9xXE", "Json")),
	    index_cstr(Resolve<Index>(symbols, "HwDt5lD9Bfo")),
	    refer(Resolve<Refer>(symbols, "wLsJlmgEIaI")),
	    type(Resolve<Type>(symbols, "SHtAad20YYM")),
	    count(Resolve<Count>(symbols, "RBw+4NukeGQ")),
	    uint_value(Resolve<UInt>(symbols, "sn4HNCtNRzY", "Json")),
	    set_uint(Resolve<SetUInt>(symbols, "SIe1ZmW7e7s")),
	    get_string(Resolve<GetString>(symbols, "epJ6x2LV0kU")),
	    c_str(Resolve<CStr>(symbols, "L1KAkYWml-M")),
	    get_object(Resolve<GetObject>(symbols, "IlsmvBtMkak")),
	    begin(Resolve<BeginEnd>(symbols, "xhAcaIwnrgk", "Json")),
	    end(Resolve<BeginEnd>(symbols, "ivMCitpSQNk", "Json")),
	    copy(Resolve<Copy>(symbols, "JATlaeBu+lU", "Json")),
	    not_equal(Resolve<Compare>(symbols, "+isUKw4zud4", "Json")),
	    arrow(Resolve<Arrow>(symbols, "EnesV4gXDtE", "Json")),
	    increment(Resolve<Increment>(symbols, "DlWmn2ZQuWY", "Json")),
	    d1(Resolve<IteratorDtor>(symbols, "hoINmSMlYjI", "Json")),
	    d2(Resolve<IteratorDtor>(symbols, "patTzc7zBjw", "Json")) {}

	ValueCtor value_ctor;
	ValueDtor value_dtor;
	Parse parse;
	StringCtor string_ctor;
	StringDtor string_dtor;
	Get get;
	Get index_string;
	Index index_cstr;
	Refer refer;
	Type type;
	Count count;
	UInt uint_value;
	SetUInt set_uint;
	GetString get_string;
	CStr c_str;
	GetObject get_object;
	BeginEnd begin;
	BeginEnd end;
	Copy copy;
	Compare not_equal;
	Arrow arrow;
	Increment increment;
	IteratorDtor d1;
	IteratorDtor d2;
};

const Value* LookupTemporaryKey(const Api& api, const Value* value, const char* text) {
	String key {};
	api.string_ctor(&key, text); // Constructors have void ABI; do not assert RAX.
	const auto* result = api.get(value, &key);
	api.string_dtor(&key);
	return result;
}

void TestLookup(const Api& api) {
	Value root {};
	api.value_ctor(&root);
	constexpr char text[] = R"({"number":17,"presentNull":null,"array":[true],"nested":{"text":"kept"},"":9})";
	Check(api.parse(&root, text, sizeof(text) - 1) == 0, "parse fixture through registered ABI");
	Check(api.type(&root) == 7 && api.count(&root) == 5, "fixture is a five-member object");

	String key {};
	api.string_ctor(&key, "number");
	const auto* borrowed = api.get(&root, &key);
	Check(borrowed != nullptr && borrowed == api.index_string(&root, &key) &&
	          borrowed == api.index_cstr(&root, "number") && borrowed == api.refer(&root, &key),
	      "get/operator[]/refer return the same present member, not a copy");
	api.string_dtor(&key);
	Check(*api.uint_value(borrowed) == 17, "borrowed member survives temporary key destruction");

	const auto* missing = LookupTemporaryKey(api, &root, "absent");
	Check(missing != nullptr && api.type(missing) == 0, "get miss returns a valid null Value");
	api.string_ctor(&key, "absent");
	Check(api.index_string(&root, &key) == missing && api.index_cstr(&root, "absent") == missing,
	      "const lookup overloads share the no-handler fallback");
	Check(api.refer(&root, &key) == nullptr, "refer miss is a nullable pointer, not a Value reference");
	api.string_dtor(&key);
	Check(api.count(&root) == 5, "missing lookup did not insert a key");

	api.string_ctor(&key, "presentNull");
	const auto* present_null = api.get(&root, &key);
	Check(present_null != nullptr && present_null != missing && api.type(present_null) == 0 &&
	          present_null == api.refer(&root, &key),
	      "present null member has its own identity and is found by refer");
	api.string_dtor(&key);

	const auto* array = LookupTemporaryKey(api, &root, "array");
	const auto* nested = LookupTemporaryKey(api, &root, "nested");
	Check(api.type(array) == 6 && api.count(array) == 1, "array member remains intact");
	Check(api.type(nested) == 7 && api.count(nested) == 1, "object member remains intact");
	api.string_ctor(&key, "0");
	for (const auto* receiver: {array, borrowed, present_null}) {
		const auto original_type = api.type(receiver);
		const auto original_count = api.count(receiver);
		Check(api.get(receiver, &key) == missing && api.index_string(receiver, &key) == missing &&
		          api.index_cstr(receiver, "0") == missing,
		      "String-key lookup on a non-object returns the no-handler fallback");
		Check(api.refer(const_cast<Value*>(receiver), &key) == nullptr,
		      "refer on a non-object returns nullptr");
		Check(api.type(receiver) == original_type && api.count(receiver) == original_count,
		      "wrong-type access neither converts nor changes the receiver");
	}
	api.string_dtor(&key);
	Check(*api.uint_value(LookupTemporaryKey(api, &root, "")) == 9, "constructed empty String key is searchable");
	const auto* nested_text = LookupTemporaryKey(api, nested, "text");
	Check(std::strcmp(api.c_str(api.get_string(nested_text)), "kept") == 0,
	      "nested borrowed member is accessible after key destruction");

	api.string_ctor(&key, "number");
	auto* mutable_member = api.refer(&root, &key);
	api.set_uint(mutable_member, 23);
	Check(*api.uint_value(borrowed) == 23 && api.get(&root, &key) == borrowed && api.count(&root) == 5,
	      "refer mutates the owned member seen by const lookup without inserting");
	api.string_dtor(&key);
	api.value_dtor(&root); // Borrowed pointers are not independently destroyed or used after this.
}

void TestIteratorOwnership(const Api& api) {
	using Libs::LibJson2::Testing::GetIteratorAllocations;
	const auto baseline = GetIteratorAllocations();
	const auto counts = [&](size_t allocated, size_t released) {
		const auto current = GetIteratorAllocations();
		Check(current.allocated - baseline.allocated == allocated &&
		          current.released - baseline.released == released,
		      "iterator allocation/release counts match actual state storage operations");
	};

	Value root {};
	api.value_ctor(&root);
	constexpr char text[] = R"({"a_long_member_name_exceeding_small_string_storage":"owned payload","z":2})";
	constexpr char first_key[] = "a_long_member_name_exceeding_small_string_storage";
	Check(api.parse(&root, text, sizeof(text) - 1) == 0, "parse iterator fixture");
	const auto* object = api.get_object(&root);
	const auto* member = api.index_cstr(&root, first_key);
	Iterator first {}, last {}, copy {};
	Check(api.begin(&first, object) == &first && api.end(&last, object) == &last,
	      "begin/end use the explicit result-pointer ABI");
	Check(api.not_equal(&first, &last), "nonempty object begin differs from end");
	const auto* pair = api.arrow(&first);
	Check(pair != nullptr && std::strcmp(api.c_str(&pair->key), first_key) == 0,
	      "iterator arrow exposes the first key");
	api.copy(&copy, &first);
	counts(3, 0);
	api.d1(&first);
	counts(3, 1); // A log-only or pointer-clear-only destructor cannot pass this.
	pair = api.arrow(&copy);
	Check(pair != nullptr && std::strcmp(api.c_str(&pair->key), first_key) == 0 &&
	          std::strcmp(api.c_str(api.get_string(&pair->value)), "owned payload") == 0,
	      "copied iterator owns independent state after D1 destroys its source");
	Check(api.increment(&copy) == &copy && api.not_equal(&copy, &last), "copied iterator advances independently");
	pair = api.arrow(&copy);
	Check(pair != nullptr && std::strcmp(api.c_str(&pair->key), "z") == 0 &&
	          *api.uint_value(&pair->value) == 2, "second member remains intact");
	api.increment(&copy);
	Check(!api.not_equal(&copy, &last), "copied iterator reaches end");
	api.d2(&copy);
	counts(3, 2);
	api.d2(&last);
	counts(3, 3);
	Check(api.count(&root) == 2 && api.index_cstr(&root, first_key) == member &&
	          std::strcmp(api.c_str(api.get_string(member)), "owned payload") == 0,
	      "D1/D2 release only iterator state; object and borrowed member survive");

	// Repeat allocation/copy/use/destruction to catch leaks across reuse, without
	// relying on guest pointer clearing or allocator address reuse as proof.
	for (size_t i = 0; i < 16; ++i) {
		api.begin(&first, object);
		api.copy(&copy, &first);
		api.d2(&first);
		pair = api.arrow(&copy);
		Check(pair != nullptr && std::strcmp(api.c_str(&pair->key), first_key) == 0,
		      "repeated copied iterator remains usable");
		api.d1(&copy);
		counts(3 + (i + 1) * 2, 3 + (i + 1) * 2);
	}
	api.value_dtor(&root);
	counts(35, 35);
}

template <typename T>
struct Guard {
	uint64_t before = 0x123456789abcdef0ULL;
	T value;
	uint64_t after = 0x0fedcba987654321ULL;
	Guard() { std::memset(&value, 0xa5, sizeof(value)); }
	void CheckCanaries() const {
		Check(before == 0x123456789abcdef0ULL && after == 0x0fedcba987654321ULL,
		      "guest output stayed inside its ABI storage");
	}
};

template <typename Function>
Function Container(const Loader::SymbolDatabase& db, const char* nid) {
	return Resolve<Function>(db, nid, "Json");
}

void TestExactObjectChain(const Loader::SymbolDatabase& db, const Api& api) {
	using Ctor = void (KYTY_SYSV_ABI *)(Iterator*);
	using Assign = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Iterator*);
	using Find = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Object*, const String*);
	using Insert = Iterator* (KYTY_SYSV_ABI *)(Iterator*, Object*, const Pair*);
	using Postfix = Iterator* (KYTY_SYSV_ABI *)(Iterator*, int);
	using Advance = void (KYTY_SYSV_ABI *)(Iterator*, size_t);
	using PairCtor = void (KYTY_SYSV_ABI *)(Pair*, const String*, const Value*);
	using PairDtor = void (KYTY_SYSV_ABI *)(Pair*);
	using ObjectCtor = void (KYTY_SYSV_ABI *)(Object*);
	using ObjectCopy = void (KYTY_SYSV_ABI *)(Object*, const Object*);
	using ObjectAssign = Object* (KYTY_SYSV_ABI *)(Object*, const Object*);
	using ObjectDtor = void (KYTY_SYSV_ABI *)(Object*);
	const auto ctor = Container<Ctor>(db, "a2Zv3idT0sU");
	const auto assign = Container<Assign>(db, "w7nqTT3zgxU");
	const auto find = Container<Find>(db, "ehN1Biw3tw8");
	const auto insert = Container<Insert>(db, "ol7KhQl8kfw");
	const auto equal = Container<Api::Compare>(db, "q3YO1c0RqFo");
	const auto deref = Container<Api::Arrow>(db, "ZCd6IYoD3Bc");
	const auto postfix = Container<Postfix>(db, "dIVxrrdpS9M");
	const auto advance = Container<Advance>(db, "h-V8r4EF7bo");
	const auto pair_ctor = Container<PairCtor>(db, "+hYsagaV-Ug");
	const auto pair_dtor = Container<PairDtor>(db, "Q0ng4sWj2E8");
	const auto object_ctor = Container<ObjectCtor>(db, "OJPTonqdg0I");
	const auto object_copy = Container<ObjectCopy>(db, "a+W7HHlwpBs");
	const auto object_assign = Container<ObjectAssign>(db, "urOpESTBZmo");
	const auto object_dtor = Container<ObjectDtor>(db, "5JmzZt8twAo");

	Guard<Value> root;
	api.value_ctor(&root.value);
	constexpr char text[] = R"({"a":11,"z":22})";
	Check(api.parse(&root.value, text, sizeof(text) - 1) == 0, "parse exact Object chain fixture");
	auto* object = const_cast<Object*>(api.get_object(&root.value));
	String key {};
	api.string_ctor(&key, "a");
	Guard<Iterator> first, last, found, missing, copy, assigned;
	Check(api.begin(&first.value, object) == &first.value && api.end(&last.value, object) == &last.value,
	      "poisoned begin/end outputs use RDI and return it");
	Check(find(&found.value, object, &key) == &found.value, "find result ABI includes third key argument");
	api.string_dtor(&key);
	api.string_ctor(&key, "not-present");
	Check(find(&missing.value, object, &key) == &missing.value, "find miss constructs its hidden output");
	api.string_dtor(&key);
	Check(equal(&missing.value, &last.value) && !api.not_equal(&missing.value, &last.value),
	      "find miss and end agree under exact comparisons");
	api.copy(&copy.value, &found.value);
	ctor(&assigned.value);
	Check(assign(&assigned.value, &found.value) == &assigned.value, "iterator assignment returns receiver");
	auto* pair = deref(&found.value);
	Check(pair != nullptr && pair == api.arrow(&first.value) && pair == deref(&copy.value) &&
	          pair == deref(&assigned.value) && &pair->value == api.index_cstr(&root.value, "a"),
	      "arrow/deref/copy/assignment expose the actual owned Pair and member identity");
	api.set_uint(&pair->value, 41);
	Check(*api.uint_value(api.index_cstr(&root.value, "a")) == 41,
	      "primitive write through Pair::second changes the actual object member");
	Check(assign(&assigned.value, &assigned.value) == &assigned.value && deref(&assigned.value) == pair,
	      "iterator self-assignment preserves the current node");
	Check(postfix(&found.value, 0) == &found.value &&
	          std::strcmp(api.c_str(&deref(&found.value)->key), "z") == 0,
	      "Object postfix returns updated receiver, without a hidden result or old value");
	Check(api.increment(&copy.value) == &copy.value && equal(&found.value, &copy.value),
	      "Object prefix and postfix land on the same second member");
	advance(&assigned.value, 2);
	Check(equal(&assigned.value, &last.value), "Object advance reaches end");

	Guard<Pair> temporary;
	Value number {};
	api.value_ctor(&number);
	api.set_uint(&number, 73);
	api.string_ctor(&key, "m");
	pair_ctor(&temporary.value, &key, &number);
	api.string_dtor(&key);
	Guard<Iterator> inserted, duplicate, find_inserted;
	Check(insert(&inserted.value, object, &temporary.value) == &inserted.value,
	      "insert constructs poisoned hidden output with Object and Pair arguments");
	auto* inserted_pair = deref(&inserted.value);
	api.set_uint(&temporary.value.value, 99);
	Check(insert(&duplicate.value, object, &temporary.value) == &duplicate.value &&
	          deref(&duplicate.value) == inserted_pair && *api.uint_value(&inserted_pair->value) == 73,
	      "duplicate insert returns existing member without overwriting it");
	pair_dtor(&temporary.value);
	api.value_dtor(&number);
	api.string_ctor(&key, "m");
	find(&find_inserted.value, object, &key);
	api.string_dtor(&key);
	Check(deref(&find_inserted.value) == inserted_pair && api.count(&root.value) == 3,
	      "inserted owned Pair survives source Pair/key/value destruction");
	Check(deref(&first.value) == pair && *api.uint_value(&pair->value) == 41,
	      "unrelated insertion preserves existing Pair identity");
	for (auto* it: {&first, &last, &found, &missing, &copy, &assigned, &inserted, &duplicate, &find_inserted}) {
		it->CheckCanaries();
		const auto before = Libs::LibJson2::Testing::GetIteratorAllocations();
		api.d1(&it->value);
		const auto after = Libs::LibJson2::Testing::GetIteratorAllocations();
		Check(after.released == before.released + 1, "exact D1 actually releases every produced iterator state");
		it->CheckCanaries();
	}
	Check(*api.uint_value(&pair->value) == 41 && *api.uint_value(&inserted_pair->value) == 73,
	      "owned Pair addresses survive destruction of all borrowing iterators");
	root.CheckCanaries();
	temporary.CheckCanaries();

	Guard<Object> copied_object, assigned_object;
	object_copy(&copied_object.value, object);
	object_ctor(&assigned_object.value);
	Check(object_assign(&assigned_object.value, &copied_object.value) == &assigned_object.value,
	      "Object assignment returns receiver");
	Guard<Iterator> independent;
	api.begin(&independent.value, &assigned_object.value);
	auto* independent_pair = deref(&independent.value);
	Check(independent_pair != pair && *api.uint_value(&independent_pair->value) == 41,
	      "Object copy/assignment own distinct Pairs");
	Check(object_assign(&assigned_object.value, &assigned_object.value) == &assigned_object.value &&
	          deref(&independent.value) == independent_pair, "Object self-assignment preserves its storage");
	api.set_uint(&independent_pair->value, 88);
	Check(*api.uint_value(&pair->value) == 41, "copy mutation does not alias the source member");
	api.d2(&independent.value);
	object_dtor(&copied_object.value);
	object_dtor(&assigned_object.value);
	copied_object.CheckCanaries();
	assigned_object.CheckCanaries();
	api.value_dtor(&root.value);

	// Empty producers must also initialize poison before D1/D2; this reproduces
	// the dangerous end/find -> destructor sequence even when no member exists.
	Guard<Object> empty;
	object_ctor(&empty.value);
	Guard<Iterator> eb, ee, ef, def, def_copy;
	api.begin(&eb.value, &empty.value);
	api.end(&ee.value, &empty.value);
	api.string_ctor(&key, "absent");
	find(&ef.value, &empty.value, &key);
	api.string_dtor(&key);
	Check(equal(&eb.value, &ee.value) && equal(&ef.value, &ee.value) &&
	          !api.not_equal(&eb.value, &ee.value), "empty begin/find/end compare coherently");
	Container<Ctor>(db, "OxO+V0ouoS4")(&def.value);
	Container<Api::Copy>(db, "r5CShmcua+M")(&def_copy.value, &def.value);
	Check(equal(&def.value, &def_copy.value), "default and copied-default iterators own null-node state");
	// Assignment from a default null node releases destination storage natively.
	Check(assign(&ef.value, &def.value) == &ef.value, "assignment from a null node returns destination");
	Check(!equal(&ef.value, &ee.value) && !api.not_equal(&ef.value, &ee.value),
	      "native comparisons both return false when either implementation is null");
	for (auto* it: {&eb, &ee, &ef, &def, &def_copy}) {
		it->CheckCanaries();
		api.d2(&it->value);
	}
	Guard<Pair> empty_pair;
	Guard<Iterator> empty_insert, empty_duplicate, empty_found;
	api.value_ctor(&number);
	api.set_uint(&number, 7);
	api.string_ctor(&key, "first");
	pair_ctor(&empty_pair.value, &key, &number);
	Check(insert(&empty_insert.value, &empty.value, &empty_pair.value) == &empty_insert.value &&
	          insert(&empty_duplicate.value, &empty.value, &empty_pair.value) == &empty_duplicate.value &&
	          find(&empty_found.value, &empty.value, &key) == &empty_found.value &&
	          equal(&empty_insert.value, &empty_duplicate.value) && equal(&empty_found.value, &empty_insert.value),
	      "empty Object insertion, duplicate insertion and subsequent find share one member");
	pair_dtor(&empty_pair.value);
	api.string_dtor(&key);
	api.value_dtor(&number);
	Check(*api.uint_value(&deref(&empty_found.value)->value) == 7, "empty insertion owns its copied value");
	for (auto* it: {&empty_insert, &empty_duplicate, &empty_found}) {
		api.d1(&it->value);
		it->CheckCanaries();
	}
	empty_pair.CheckCanaries();
	object_dtor(&empty.value);
	empty.CheckCanaries();
}

void TestArrayFamily(const Loader::SymbolDatabase& db, const Api& api) {
	using ArrayCtor = void (KYTY_SYSV_ABI *)(Array*);
	using ArrayCopy = void (KYTY_SYSV_ABI *)(Array*, const Array*);
	using ArrayAssign = Array* (KYTY_SYSV_ABI *)(Array*, const Array*);
	using ArrayDtor = void (KYTY_SYSV_ABI *)(Array*);
	using GetArray = const Array* (KYTY_SYSV_ABI *)(const Value*);
	using BeginEnd = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Array*);
	using Ctor = void (KYTY_SYSV_ABI *)(Iterator*);
	using Assign = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Iterator*);
	using Deref = Value* (KYTY_SYSV_ABI *)(const Iterator*);
	using Postfix = Iterator* (KYTY_SYSV_ABI *)(Iterator*, int);
	using Advance = void (KYTY_SYSV_ABI *)(Iterator*, size_t);
	const auto begin = Container<BeginEnd>(db, "bcH5EnFE2xY");
	const auto end = Container<BeginEnd>(db, "WXF2ihRF+B8");
	const auto ne = Container<Api::Compare>(db, "5AZPp99ogrc");
	const auto deref = Container<Deref>(db, "wcgr5mte7T8");
	const auto arrow = Container<Deref>(db, "iAIYn4oAWvI");
	const auto post = Container<Postfix>(db, "GEylwSsiGG8");
	const auto assign = Container<Assign>(db, "T3WReElX4BY");
	const auto destroy = Container<Api::IteratorDtor>(db, "9yLjn46Ypfs");
	const auto destroy2 = Container<Api::IteratorDtor>(db, "9niNrXsF3Bc");
	Value root {};
	api.value_ctor(&root);
	constexpr char text[] = "[10,20]";
	Check(api.parse(&root, text, sizeof(text) - 1) == 0, "parse Array fixture via size overload");
	const auto* source = Resolve<GetArray>(db, "ONT8As5R1ug")(&root);
	Guard<Array> copy, assigned, empty;
	Container<ArrayCopy>(db, "bI5AGFMydrA")(&copy.value, source);
	Container<ArrayCtor>(db, "JP-PtKMiI1E")(&assigned.value);
	Container<ArrayCtor>(db, "t6AHhkWu4mY")(&empty.value);
	const auto array_assign = Container<ArrayAssign>(db, "w5UbvPLGye0");
	Check(array_assign(&assigned.value, &copy.value) == &assigned.value &&
	          array_assign(&assigned.value, &assigned.value) == &assigned.value,
	      "Array container assignment and self-assignment return receiver");
	Guard<Iterator> first, last, copied, target, extra, eb, ee;
	Check(begin(&first.value, &assigned.value) == &first.value && end(&last.value, &assigned.value) == &last.value,
	      "Array begin/end initialize poisoned hidden results");
	Container<Api::Copy>(db, "IaIvX3kgyb0")(&copied.value, &first.value);
	Container<Ctor>(db, "TAPTWYaYf8Q")(&target.value);
	Container<Ctor>(db, "JSg1MSo7NKE")(&extra.value);
	Check(assign(&target.value, &first.value) == &target.value &&
	          assign(&target.value, &target.value) == &target.value,
	      "T3 is iterator assignment with updated receiver return");
	Check(deref(&first.value) == arrow(&copied.value) && deref(&target.value) == deref(&first.value),
	      "Array copies and assignment borrow the same element");
	Check(post(&first.value, 0) == &first.value && *api.uint_value(deref(&first.value)) == 20 &&
	          *api.uint_value(deref(&copied.value)) == 10,
	      "Array postfix updates receiver; copy position is independent");
	Check(Container<Api::Increment>(db, "w5+VCznos5E")(&target.value) == &target.value &&
	          !ne(&target.value, &first.value), "Array prefix matches postfix");
	Container<Advance>(db, "43MESLndGvQ")(&target.value, 1);
	Check(!ne(&target.value, &last.value), "Array advance reaches stable end");
	api.set_uint(deref(&copied.value), 31);
	using Index = const Value* (KYTY_SYSV_ABI *)(const Value*, uint64_t);
	Check(*api.uint_value(Resolve<Index>(db, "XlWbvieLj2M")(&root, 0)) == 10,
	      "Array container copies own distinct elements");
	Check(begin(&eb.value, &empty.value) == &eb.value && end(&ee.value, &empty.value) == &ee.value &&
	          !ne(&eb.value, &ee.value), "empty Array producers initialize comparable outputs");
	for (auto* it: {&first, &last, &copied, &target, &extra, &eb, &ee}) {
		it->CheckCanaries();
		const auto before = Libs::LibJson2::Testing::GetIteratorAllocations();
		destroy(&it->value);
		Check(Libs::LibJson2::Testing::GetIteratorAllocations().released == before.released + 1,
		      "Array D1 releases actual independently owned iterator state");
	}
	Guard<Iterator> c2source, c2copy;
	begin(&c2source.value, source);
	Container<Api::Copy>(db, "Hdc3DSIsG2I")(&c2copy.value, &c2source.value);
	destroy2(&c2source.value);
	Check(*api.uint_value(deref(&c2copy.value)) == 10, "Array C2 copy survives D2 of source iterator");
	destroy2(&c2copy.value);
	c2source.CheckCanaries();
	c2copy.CheckCanaries();
	Guard<Array> c2array;
	Container<ArrayCopy>(db, "6NlbpyIfUuY")(&c2array.value, source);
	Container<ArrayDtor>(db, "MIyMPw7YTGM")(&c2array.value);
	for (auto* array: {&copy, &assigned, &empty}) {
		Container<ArrayDtor>(db, "HJ8GpRT1aiw")(&array->value);
		array->CheckCanaries();
	}
	c2array.CheckCanaries();
	Check(api.count(&root) == 2, "source Array survives destruction of copies and iterators");
	api.value_dtor(&root);
}

struct NestedMembers {
	Value* number;
	Value* text;
	Value* array_text;
	Value* label;
};

NestedMembers ReadNestedMembers(const Loader::SymbolDatabase& db, const Api& api, const Value* payload) {
	using Index = const Value* (KYTY_SYSV_ABI *)(const Value*, uint64_t);
	const auto index = Resolve<Index>(db, "XlWbvieLj2M");
	Check(api.type(payload) == 7 && api.count(payload) == 2, "copied payload retains nested Object");
	const auto* items = LookupTemporaryKey(api, payload, "items");
	Check(api.type(items) == 6 && api.count(items) == 2, "copied payload retains nested Array");
	const auto* object = index(items, 0);
	const auto* array = index(items, 1);
	Check(api.type(object) == 7 && api.count(object) == 2 && api.type(array) == 6 && api.count(array) == 1,
	      "nested Object and Array elements survive source destruction");
	NestedMembers result {
	    const_cast<Value*>(LookupTemporaryKey(api, object, "number")),
	    const_cast<Value*>(LookupTemporaryKey(api, object, "text")),
	    const_cast<Value*>(index(array, 0)),
	    const_cast<Value*>(LookupTemporaryKey(api, payload, "label"))
	};
	Check(*api.uint_value(result.number) == 7, "nested primitive survives deep copy");
	for (const auto* value: {result.text, result.array_text, result.label}) {
		Check(api.type(value) == 5 &&
		          std::strcmp(api.c_str(api.get_string(value)), "long owned string beyond small string storage") == 0,
		      "nested strings own valid content after source destruction");
	}
	return result;
}

void MutateNestedMembers(const Api& api, const NestedMembers& members, uint64_t value) {
	// Replacing strings must release their independent allocations, not a sibling's.
	for (auto* member: {members.number, members.text, members.array_text, members.label}) {
		api.set_uint(member, value);
		Check(api.type(member) == 3 && *api.uint_value(member) == value, "copied nested member is independently mutable");
	}
}

void CheckIndependentMembers(const NestedMembers& a, const NestedMembers& b) {
	Check(a.number != b.number && a.text != b.text && a.array_text != b.array_text && a.label != b.label,
	      "container copies own distinct nested Value storage");
}

void TestNestedContainerCopies(const Loader::SymbolDatabase& db, const Api& api) {
	// Both container families contain Object -> Array -> Object/string and Array/string.
	constexpr char object_text[] = R"({"payload":{"items":[{"number":7,"text":"long owned string beyond small string storage"},["long owned string beyond small string storage"]],"label":"long owned string beyond small string storage"}})";
	constexpr char array_text[] = R"([{"items":[{"number":7,"text":"long owned string beyond small string storage"},["long owned string beyond small string storage"]],"label":"long owned string beyond small string storage"}])";
	using ObjectCopy = void (KYTY_SYSV_ABI *)(Object*, const Object*);
	using ObjectAssign = Object* (KYTY_SYSV_ABI *)(Object*, const Object*);
	using ObjectDtor = void (KYTY_SYSV_ABI *)(Object*);
	using ArrayCopy = void (KYTY_SYSV_ABI *)(Array*, const Array*);
	using ArrayAssign = Array* (KYTY_SYSV_ABI *)(Array*, const Array*);
	using ArrayDtor = void (KYTY_SYSV_ABI *)(Array*);
	using GetArray = const Array* (KYTY_SYSV_ABI *)(const Value*);
	using ArrayBegin = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Array*);
	using ArrayDeref = Value* (KYTY_SYSV_ABI *)(const Iterator*);
	const auto object_destroy = Container<ObjectDtor>(db, "5JmzZt8twAo");
	const auto array_destroy = Container<ArrayDtor>(db, "HJ8GpRT1aiw");
	const auto array_begin = Container<ArrayBegin>(db, "bcH5EnFE2xY");
	const auto array_deref = Container<ArrayDeref>(db, "wcgr5mte7T8");
	const auto iterator_destroy = Container<Api::IteratorDtor>(db, "9yLjn46Ypfs");
	Guard<Value> root;
	api.value_ctor(&root.value);
	Check(api.parse(&root.value, object_text, sizeof(object_text) - 1) == 0, "parse nested Object copy fixture");
	Guard<Object> object_copy, object_assigned;
	const auto* source_object = api.get_object(&root.value);
	Container<ObjectCopy>(db, "a+W7HHlwpBs")(&object_copy.value, source_object);
	Container<ObjectCopy>(db, "tSRqQ-+Y4PE")(&object_assigned.value, source_object);
	Check(Container<ObjectAssign>(db, "urOpESTBZmo")(&object_assigned.value, &object_copy.value) == &object_assigned.value,
	      "nested Object assignment replaces a nonempty destination");
	api.value_dtor(&root.value);
	Guard<Iterator> oi, oj;
	api.begin(&oi.value, &object_copy.value);
	api.begin(&oj.value, &object_assigned.value);
	const auto* object_payload = &api.arrow(&oi.value)->value;
	const auto* assigned_payload = &api.arrow(&oj.value)->value;
	api.d1(&oi.value);
	api.d2(&oj.value);
	const auto a = ReadNestedMembers(db, api, object_payload);
	const auto b = ReadNestedMembers(db, api, assigned_payload);
	CheckIndependentMembers(a, b);
	MutateNestedMembers(api, a, 81);
	ReadNestedMembers(db, api, assigned_payload);
	object_destroy(&object_copy.value);
	ReadNestedMembers(db, api, assigned_payload);
	MutateNestedMembers(api, b, 82);
	Container<ObjectDtor>(db, "1siRBursJjw")(&object_assigned.value);
	object_copy.CheckCanaries();
	object_assigned.CheckCanaries();
	oi.CheckCanaries();
	oj.CheckCanaries();

	api.value_ctor(&root.value);
	Check(api.parse(&root.value, array_text, sizeof(array_text) - 1) == 0, "parse nested Array copy fixture");
	Guard<Array> array_copy, array_assigned;
	const auto* source_array = Resolve<GetArray>(db, "ONT8As5R1ug")(&root.value);
	Container<ArrayCopy>(db, "bI5AGFMydrA")(&array_copy.value, source_array);
	Container<ArrayCopy>(db, "6NlbpyIfUuY")(&array_assigned.value, source_array);
	Check(Container<ArrayAssign>(db, "w5UbvPLGye0")(&array_assigned.value, &array_copy.value) == &array_assigned.value,
	      "nested Array assignment replaces a nonempty destination");
	api.value_dtor(&root.value);
	Guard<Iterator> ai, aj;
	array_begin(&ai.value, &array_copy.value);
	array_begin(&aj.value, &array_assigned.value);
	const auto* array_payload = array_deref(&ai.value);
	const auto* array_assigned_payload = array_deref(&aj.value);
	iterator_destroy(&ai.value);
	iterator_destroy(&aj.value);
	const auto c = ReadNestedMembers(db, api, array_payload);
	const auto d = ReadNestedMembers(db, api, array_assigned_payload);
	CheckIndependentMembers(c, d);
	MutateNestedMembers(api, c, 91);
	ReadNestedMembers(db, api, array_assigned_payload);
	array_destroy(&array_copy.value);
	ReadNestedMembers(db, api, array_assigned_payload);
	MutateNestedMembers(api, d, 92);
	array_destroy(&array_assigned.value);
	array_copy.CheckCanaries();
	array_assigned.CheckCanaries();
	ai.CheckCanaries();
	aj.CheckCanaries();
	root.CheckCanaries();
}

void TestArrayGrowth(const Loader::SymbolDatabase& db, const Api& api) {
	using Ctor = void (KYTY_SYSV_ABI *)(Array*);
	using Dtor = void (KYTY_SYSV_ABI *)(Array*);
	using Push = void (KYTY_SYSV_ABI *)(Array*, const Value*);
	using BeginEnd = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Array*);
	using Deref = Value* (KYTY_SYSV_ABI *)(const Iterator*);
	using Size = size_t (KYTY_SYSV_ABI *)(const Array*);
	const auto push = Container<Push>(db, "zQtLRTqceMY");
	const auto begin = Container<BeginEnd>(db, "bcH5EnFE2xY");
	const auto end = Container<BeginEnd>(db, "WXF2ihRF+B8");
	const auto deref = Container<Deref>(db, "wcgr5mte7T8");
	const auto increment = Container<Api::Increment>(db, "w5+VCznos5E");
	const auto ne = Container<Api::Compare>(db, "5AZPp99ogrc");
	const auto destroy = Container<Api::IteratorDtor>(db, "9yLjn46Ypfs");
	Guard<Array> array;
	Container<Ctor>(db, "JP-PtKMiI1E")(&array.value);
	Guard<Value> number;
	api.value_ctor(&number.value);
	api.set_uint(&number.value, 10);
	push(&array.value, &number.value);
	Guard<Iterator> retained, old_end, walk, new_end;
	begin(&retained.value, &array.value);
	end(&old_end.value, &array.value);
	Container<Api::Copy>(db, "IaIvX3kgyb0")(&walk.value, &retained.value);
	auto* retained_value = deref(&retained.value);
	// Repeated growth beyond the initial one-element allocation exercises vector
	// relocation without reading private capacity or host iterator representation.
	for (uint64_t i = 1; i <= 1024; ++i) {
		api.set_uint(&number.value, i);
		push(&array.value, &number.value);
		Check(deref(&retained.value) == retained_value && *api.uint_value(retained_value) == 10,
		      "retained Array iterator and element survive repeated growth");
	}
	api.value_dtor(&number.value);
	end(&new_end.value, &array.value);
	Check(!ne(&old_end.value, &new_end.value) && ne(&retained.value, &old_end.value) &&
	          Container<Size>(db, "rQGJeNjOuUk")(&array.value) == 1025,
	      "pre-growth end remains the actual end after vector growth");
	api.set_uint(retained_value, 77);
	Check(*api.uint_value(deref(&walk.value)) == 77, "retained element mutation is visible through copied iterator");
	for (uint64_t i = 1; i <= 1024; ++i) {
		increment(&walk.value);
		Check(ne(&walk.value, &old_end.value) && *api.uint_value(deref(&walk.value)) == i,
		      "pre-growth iterator traverses every appended element");
	}
	increment(&walk.value);
	Check(!ne(&walk.value, &old_end.value), "traversal terminates at pre-growth end");
	for (auto* it: {&retained, &old_end, &walk, &new_end}) {
		destroy(&it->value);
		it->CheckCanaries();
	}
	Container<Dtor>(db, "HJ8GpRT1aiw")(&array.value);
	array.CheckCanaries();
	number.CheckCanaries();
}

void TestReleasedIteratorAssignment(const Loader::SymbolDatabase& db, const Api& api) {
	using Ctor = void (KYTY_SYSV_ABI *)(Iterator*);
	using Assign = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Iterator*);
	using GetArray = const Array* (KYTY_SYSV_ABI *)(const Value*);
	using ArrayBegin = Iterator* (KYTY_SYSV_ABI *)(Iterator*, const Array*);
	using Libs::LibJson2::Testing::GetIteratorAllocations;
	for (bool array: {false, true}) {
		const auto ctor = Container<Ctor>(db, array ? "TAPTWYaYf8Q" : "a2Zv3idT0sU");
		const auto assign = Container<Assign>(db, array ? "T3WReElX4BY" : "w7nqTT3zgxU");
		const auto destroy = Container<Api::IteratorDtor>(db, array ? "9yLjn46Ypfs" : "hoINmSMlYjI");
		const auto ne = Container<Api::Compare>(db, array ? "5AZPp99ogrc" : "+isUKw4zud4");
		Guard<Value> root;
		api.value_ctor(&root.value);
		const char* text = array ? "[7]" : "{\"a\":7}";
		Check(api.parse(&root.value, text, std::strlen(text)) == 0, "parse null-state assignment fixture");
		Guard<Iterator> source, null_node, released, destination;
		if (array) {
			Container<ArrayBegin>(db, "bcH5EnFE2xY")(&source.value, Resolve<GetArray>(db, "ONT8As5R1ug")(&root.value));
		} else {
			api.begin(&source.value, api.get_object(&root.value));
		}
		ctor(&null_node.value);
		ctor(&released.value);
		ctor(&destination.value);
		Check(assign(&released.value, &source.value) == &released.value &&
		          assign(&destination.value, &source.value) == &destination.value, "assignment starts with live destination nodes");
		const auto before = GetIteratorAllocations();
		Check(assign(&released.value, &null_node.value) == &released.value, "null node assignment returns receiver");
		const auto one = GetIteratorAllocations();
		Check(one.allocated == before.allocated && one.released == before.released + 1,
		      "null node assignment actually releases destination state");
		Check(assign(&destination.value, &released.value) == &destination.value, "null implementation assignment returns receiver");
		const auto two = GetIteratorAllocations();
		Check(two.allocated == one.allocated && two.released == one.released + 1,
		      "assignment from a released implementation releases destination state");
		for (auto* it: {&released, &destination}) {
			Check(assign(&it->value, &source.value) == &it->value && !ne(&it->value, &null_node.value),
			      "assignment into released destination does not recreate a live node");
			if (!array) {
				Check(!Container<Api::Compare>(db, "q3YO1c0RqFo")(&it->value, &source.value),
				      "released Object destination remains unequal even to assignment source");
			}
			destroy(&it->value);
			it->CheckCanaries();
		}
		const auto after = GetIteratorAllocations();
		Check(after.allocated == two.allocated && after.released == two.released,
		      "released destinations neither allocate on reassignment nor release again on destruction");
		destroy(&source.value);
		destroy(&null_node.value);
		source.CheckCanaries();
		null_node.CheckCanaries();
		api.value_dtor(&root.value);
		root.CheckCanaries();
	}
}

void TestResolution(const Loader::SymbolDatabase& symbols) {
	// Independent native ABI inventory: verify exact identities and FIRST fallback,
	// not just the latest map record. These were the early/late conflicting paths.
	constexpr const char* nids[] = {
	    "ivMCitpSQNk", "ehN1Biw3tw8", "+isUKw4zud4", "q3YO1c0RqFo", "hoINmSMlYjI", "patTzc7zBjw",
	    "xhAcaIwnrgk", "ol7KhQl8kfw", "ZCd6IYoD3Bc", "dIVxrrdpS9M", "EnesV4gXDtE", "h-V8r4EF7bo",
	    "a2Zv3idT0sU", "OxO+V0ouoS4", "JATlaeBu+lU", "r5CShmcua+M", "w7nqTT3zgxU", "DlWmn2ZQuWY",
	    "JP-PtKMiI1E", "t6AHhkWu4mY", "bI5AGFMydrA", "6NlbpyIfUuY", "HJ8GpRT1aiw", "MIyMPw7YTGM",
	    "TAPTWYaYf8Q", "JSg1MSo7NKE", "IaIvX3kgyb0", "Hdc3DSIsG2I", "9yLjn46Ypfs", "9niNrXsF3Bc",
	    "T3WReElX4BY", "GEylwSsiGG8", "bcH5EnFE2xY", "WXF2ihRF+B8", "5AZPp99ogrc", "w5+VCznos5E",
	    "wcgr5mte7T8", "iAIYn4oAWvI", "43MESLndGvQ", "w5UbvPLGye0",
	    "OJPTonqdg0I", "SkKczWHdklc", "a+W7HHlwpBs", "tSRqQ-+Y4PE", "5JmzZt8twAo", "1siRBursJjw",
	    "urOpESTBZmo", "ERuf9y0DY84", "oH8aBmLU+fc", "+hYsagaV-Ug", "lt3J4dPvEtM", "Q0ng4sWj2E8",
	    "TM0Sj+Ebins", "qVOSuDRHCpA", "zQtLRTqceMY", "bAM9Qwofus0", "rQGJeNjOuUk"
	};
	static_assert(sizeof(nids) / sizeof(nids[0]) == 57);
	for (const char* nid: nids) {
		const auto* exact = symbols.Find(Identity(nid, "Json"));
		const auto* compat = symbols.Find(Identity(nid, "Json2"));
		const auto* first = symbols.FindByNid(nid, Loader::SymbolType::Func);
		const auto* named = symbols.FindByName(nid, Loader::SymbolType::Func);
		const auto* library_prefix = symbols.FindByName(std::string(nid) + "[Json2_v1]", Loader::SymbolType::Func);
		Check(exact && compat && first && exact->vaddr == compat->vaddr && exact->vaddr == first->vaddr &&
		          named && named->vaddr == exact->vaddr && exact->dbg_name.starts_with("LibJson2::") &&
		          library_prefix && library_prefix->vaddr == exact->vaddr &&
		          symbols.FindRegistration(Identity(nid, "Json"), exact->dbg_name) &&
		          symbols.FindRegistration(Identity(nid, "Json2"), exact->dbg_name),
		      "exact/compat/FIRST lookup and all historical registrations select one Json2 family");
	}
	constexpr const char* aliases[][2] = {
	    {"OJPTonqdg0I", "SkKczWHdklc"}, {"a+W7HHlwpBs", "tSRqQ-+Y4PE"}, {"5JmzZt8twAo", "1siRBursJjw"},
	    {"+hYsagaV-Ug", "lt3J4dPvEtM"}, {"Q0ng4sWj2E8", "TM0Sj+Ebins"},
	    {"a2Zv3idT0sU", "OxO+V0ouoS4"}, {"JATlaeBu+lU", "r5CShmcua+M"}, {"hoINmSMlYjI", "patTzc7zBjw"},
	    {"JP-PtKMiI1E", "t6AHhkWu4mY"}, {"bI5AGFMydrA", "6NlbpyIfUuY"}, {"HJ8GpRT1aiw", "MIyMPw7YTGM"},
	    {"TAPTWYaYf8Q", "JSg1MSo7NKE"}, {"IaIvX3kgyb0", "Hdc3DSIsG2I"}, {"9yLjn46Ypfs", "9niNrXsF3Bc"}
	};
	for (const auto& alias: aliases) {
		Check(symbols.Find(Identity(alias[0], "Json"))->vaddr == symbols.Find(Identity(alias[1], "Json"))->vaddr,
		      "supported native constructor/destructor aliases select the same implementation");
	}
}

} // namespace

void TestJsonResourceLookup() {
	Loader::SymbolDatabase symbols;
	// The actual production registrars, in libs.cpp's order. Do not call InitAll:
	// it starts unrelated services. ShellHits must run before resolving any ABI.
	Libs::LibJson2Iter::InitJson2Iter_1(&symbols);
	Libs::LibJson2::InitNet_1_Json2(&symbols);
	Libs::ShellHits::InitShellHits_1(&symbols);
	for (const char* nid: {"MsMOdxWfbwQ", "clF7J7N9xXE"}) {
		Check(symbols.FindRegistration(Identity(nid, "Json"), "LibJson2::JsonValueGetValueKey") != nullptr,
		      "String-key lookup has one consistent exact provider");
	}
	for (const char* nid: {"hoINmSMlYjI", "patTzc7zBjw"}) {
		Check(symbols.FindRegistration(Identity(nid, "Json"), "LibJson2::JsonObjectIteratorDtor") != nullptr,
		      "ShellHits leaves no competing log-only destructor registration");
	}
	const Api api(symbols);
	Check(api.get == api.index_string && api.d1 == api.d2, "proven native aliases share HLE implementations");
	TestResolution(symbols);
	const auto baseline = Libs::LibJson2::Testing::GetIteratorAllocations();
	TestLookup(api);
	TestIteratorOwnership(api);
	TestExactObjectChain(symbols, api);
	TestArrayFamily(symbols, api);
	TestNestedContainerCopies(symbols, api);
	TestArrayGrowth(symbols, api);
	TestReleasedIteratorAssignment(symbols, api);
	const auto final = Libs::LibJson2::Testing::GetIteratorAllocations();
	Check(final.allocated > baseline.allocated && final.pairs_allocated > baseline.pairs_allocated &&
	          final.allocated - baseline.allocated == final.released - baseline.released &&
	          final.pairs_allocated - baseline.pairs_allocated == final.pairs_released - baseline.pairs_released,
	      "all actual iterator and owned Pair allocations are released after registered-ABI fixtures");
}
