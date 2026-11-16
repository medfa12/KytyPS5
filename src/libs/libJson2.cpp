#include "common/abi.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstddef>
#include <cstring>
#include <istream>
#include <map>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <streambuf>
#include <string>
#include <vector>

#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
#include "libs/json2Testing.h"
#include "libs/json2PathTesting.h"
#endif

namespace Libs {

namespace LibJson2 {

LIB_VERSION("Json2", 1, "Json2", 1, 1);

enum JsonValueType : uint32_t {
	JsonValueTypeNull = 0,
	JsonValueTypeBoolean,
	JsonValueTypeInteger,
	JsonValueTypeUInteger,
	JsonValueTypeReal,
	JsonValueTypeString,
	JsonValueTypeArray,
	JsonValueTypeObject,
};

constexpr int32_t JSON_ERROR_PARSE_INVALID_CHAR = -2138799871; // 0x80848101
constexpr int32_t JSON_ERROR_NOMEM              = -2138799870; // 0x80848102
constexpr int32_t JSON_ERROR_NOFILE             = -2138799869; // 0x80848103
constexpr int32_t JSON_ERROR_NOINIT             = -2138799856; // 0x80848110
constexpr int32_t JSON_ERROR_INVALID_ARGUMENT   = -2138799840; // 0x80848120

struct JsonValue;

struct JsonString {
	std::string* impl;
};

struct JsonArray {
	std::vector<JsonValue*>* impl;
};

struct JsonArrayIteratorState;
struct JsonArrayIterator { JsonArrayIteratorState* position; };

struct JsonObjectPair;
using JsonObjectMap = std::map<std::string, JsonObjectPair*>;
struct JsonObject {
	JsonObjectMap* impl;
};

struct JsonValue {
	JsonValue* parent;
	void*      rootparam;
	union {
		bool        boolean;
		int64_t     integer;
		uint64_t    uinteger;
		double      real;
		JsonString* string;
		JsonArray*  array;
		JsonObject* object;
	};
	char     padding[4];
	uint32_t type;
};

static_assert(sizeof(JsonArray) == 8);
static_assert(sizeof(JsonArrayIterator) == 8);
static_assert(sizeof(JsonValue) == 32 && offsetof(JsonValue, type) == 28);

#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
static thread_local Testing::IteratorAllocations g_iterator_allocations;
static thread_local Testing::PathParserHooks g_path_parser_hooks;

void Testing::SetPathParserHooks(const PathParserHooks& hooks) {
	g_path_parser_hooks = hooks;
}

Testing::ValueOwnership Testing::GetValueOwnership(const void* value) {
	const auto* self = static_cast<const JsonValue*>(value);
	return {self->parent, self->rootparam};
}

void Testing::SetValueRootParamForTest(void* value, void* token) {
	static_cast<JsonValue*>(value)->rootparam = token;
}

Testing::IteratorAllocations Testing::GetIteratorAllocations() {
	return g_iterator_allocations;
}
#endif
struct JsonIteratorAllocation {
#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
	// Count real storage allocation/release, not pointer clearing in the guest wrapper.
	static void* operator new(size_t size) {
		auto* storage = ::operator new(size);
		++g_iterator_allocations.allocated;
		return storage;
	}
	static void operator delete(void* storage) noexcept {
		::operator delete(storage);
		++g_iterator_allocations.released;
	}
#endif
};
struct JsonArrayIteratorState: JsonIteratorAllocation {
	const std::vector<JsonValue*>* owner {};
	size_t index = SIZE_MAX; // Stable end marker, independent of vector reallocation.
	JsonArrayIteratorState() = default;
	JsonArrayIteratorState(const std::vector<JsonValue*>* values, size_t offset): owner(values), index(offset) {}
};
struct JsonObjectPair {
	JsonString key;
	uint64_t reserved;
	JsonValue value;
#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
	static void* operator new(size_t size) {
		auto* storage = ::operator new(size);
		++g_iterator_allocations.pairs_allocated;
		return storage;
	}
	static void operator delete(void* storage) noexcept {
		::operator delete(storage);
		++g_iterator_allocations.pairs_released;
	}
#endif
};
struct JsonObjectIteratorState: JsonIteratorAllocation {
	const JsonObjectMap* owner {};
	JsonObjectMap::const_iterator position {};
	JsonObjectIteratorState() = default;
	JsonObjectIteratorState(const JsonObjectMap* object, JsonObjectMap::const_iterator it): owner(object), position(it) {}
};
struct JsonObjectIterator { JsonObjectIteratorState* position; };
static_assert(sizeof(JsonObjectPair) == 48 && offsetof(JsonObjectPair, value) == 16);

struct JsonInitParameter2 {
	void*    allocator;
	void*    user_data;
	size_t   file_buffer_size;
	uint32_t special_float_format_type;
	uint32_t reserved[3];
};

static std::string* JsonStringImpl(JsonString* self) {
	if (self == nullptr) {
		return nullptr;
	}
	if (self->impl == nullptr) {
		self->impl = new std::string;
	}
	return self->impl;
}

static const std::string* JsonStringImpl(const JsonString* self) {
	static const std::string empty;
	return (self != nullptr && self->impl != nullptr ? self->impl : &empty);
}

static std::vector<JsonValue*>* JsonArrayImpl(JsonArray* self) {
	if (self == nullptr) {
		return nullptr;
	}
	if (self->impl == nullptr) {
		self->impl = new std::vector<JsonValue*>;
	}
	return self->impl;
}

static const std::vector<JsonValue*>* JsonArrayImpl(const JsonArray* self) {
	static const std::vector<JsonValue*> empty;
	return (self != nullptr && self->impl != nullptr ? self->impl : &empty);
}

static JsonObjectMap* JsonObjectImpl(JsonObject* self) {
	if (self == nullptr) {
		return nullptr;
	}
	if (self->impl == nullptr) {
		self->impl = new JsonObjectMap;
	}
	return self->impl;
}

static const JsonObjectMap* JsonObjectImpl(const JsonObject* self) {
	static const JsonObjectMap empty;
	return (self != nullptr && self->impl != nullptr ? self->impl : &empty);
}

static JsonValue* JsonStaticNullValue() {
	static JsonValue value {};
	value.type = JsonValueTypeNull;
	return &value;
}

static JsonString* JsonStaticString() {
	static JsonString str {new std::string};
	return &str;
}

static JsonArray* JsonStaticArray() {
	static JsonArray array {new std::vector<JsonValue*>};
	return &array;
}

static JsonObject* JsonStaticObject() {
	static JsonObject object {new JsonObjectMap};
	return &object;
}

static void JsonValueInit(JsonValue* self) {
	if (self != nullptr) {
		std::memset(self, 0, sizeof(JsonValue));
		self->type = JsonValueTypeNull;
	}
}

static void JsonValueClear(JsonValue* self);
static void JsonValueCopy(JsonValue* dst, const JsonValue* src);

// The object owns actual ABI Pairs. All lookups and iterators borrow the same
// inline second Value, so primitive writes through Pair::second are observable.
static JsonObjectPair* JsonObjectPairNew(const std::string& key, const JsonValue* src, JsonValue* parent) {
	auto storage = std::unique_ptr<JsonObjectPair>(new JsonObjectPair {});
	auto* pair = storage.get();
	pair->key.impl = new std::string(key);
	storage.release();
	JsonValueInit(&pair->value);
	if (src != nullptr) {
		JsonValueCopy(&pair->value, src);
	}
	pair->value.parent = parent;
	return pair;
}

static void JsonObjectPairDelete(JsonObjectPair* pair) {
	if (pair != nullptr) {
		JsonValueClear(&pair->value);
		delete pair->key.impl;
		delete pair;
	}
}

static JsonValue* JsonValueNew() {
	auto* value = new JsonValue;
	JsonValueInit(value);
	return value;
}

static JsonString* JsonStringNew(const std::string& value) {
	auto str = std::make_unique<JsonString>();
	str->impl = new std::string(value);
	return str.release();
}

static JsonArray* JsonArrayNew() {
	auto array = std::make_unique<JsonArray>();
	array->impl = new std::vector<JsonValue*>;
	return array.release();
}

static JsonObject* JsonObjectNew() {
	auto object = std::make_unique<JsonObject>();
	object->impl = new JsonObjectMap;
	return object.release();
}

static std::vector<JsonValue*>* JsonArrayCopy(const JsonArray* src, JsonValue* parent) {
	auto* impl = new std::vector<JsonValue*>;
	for (const auto* value: *JsonArrayImpl(src)) {
		auto* copy = JsonValueNew();
		JsonValueCopy(copy, value);
		copy->parent = parent;
		impl->push_back(copy);
	}
	return impl;
}

static JsonObjectMap* JsonObjectCopy(const JsonObject* src, JsonValue* parent) {
	auto* impl = new JsonObjectMap;
	for (const auto& [key, pair]: *JsonObjectImpl(src)) {
		impl->emplace(key, JsonObjectPairNew(key, &pair->value, parent));
	}
	return impl;
}

static void JsonValueSetEmptyType(JsonValue* self, uint32_t type) {
	if (self == nullptr) {
		return;
	}
	self->type = type;
	switch (type) {
		case JsonValueTypeString: self->string = JsonStringNew(""); break;
		case JsonValueTypeArray: self->array = JsonArrayNew(); break;
		case JsonValueTypeObject: self->object = JsonObjectNew(); break;
		default: self->uinteger = 0; break;
	}
}

static void JsonValueDelete(JsonValue* self) {
	if (self != nullptr) {
		JsonValueClear(self);
		delete self;
	}
}

static void JsonStringDelete(JsonString* self) {
	if (self != nullptr) {
		delete self->impl;
		delete self;
	}
}

static void JsonArrayDestroy(JsonArray* self) {
	if (self != nullptr) {
		if (self->impl != nullptr) {
			for (auto* value: *self->impl) {
				JsonValueDelete(value);
			}
		}
		delete self->impl;
		self->impl = nullptr;
	}
}

static void JsonObjectDestroy(JsonObject* self) {
	if (self != nullptr) {
		if (self->impl != nullptr) {
			for (auto& item: *self->impl) {
				JsonObjectPairDelete(item.second);
			}
		}
		delete self->impl;
		self->impl = nullptr;
	}
}

static void JsonValueClear(JsonValue* self) {
	if (self == nullptr) {
		return;
	}

	switch (self->type) {
		case JsonValueTypeString: JsonStringDelete(self->string); break;
		case JsonValueTypeArray:
			JsonArrayDestroy(self->array);
			delete self->array;
			break;
		case JsonValueTypeObject:
			JsonObjectDestroy(self->object);
			delete self->object;
			break;
		default: break;
	}

	self->parent    = nullptr;
	self->rootparam = nullptr;
	self->uinteger  = 0;
	self->type      = JsonValueTypeNull;
}

static void JsonValueCopy(JsonValue* dst, const JsonValue* src) {
	if (dst == nullptr || src == nullptr || dst == src) {
		return;
	}

	JsonValueClear(dst);
	dst->type = src->type;
	switch (src->type) {
		case JsonValueTypeBoolean: dst->boolean = src->boolean; break;
		case JsonValueTypeInteger: dst->integer = src->integer; break;
		case JsonValueTypeUInteger: dst->uinteger = src->uinteger; break;
		case JsonValueTypeReal: dst->real = src->real; break;
		case JsonValueTypeString: dst->string = JsonStringNew(*JsonStringImpl(src->string)); break;
		case JsonValueTypeArray: dst->array = new JsonArray {JsonArrayCopy(src->array, dst)}; break;
		case JsonValueTypeObject: dst->object = new JsonObject {JsonObjectCopy(src->object, dst)}; break;
		default: dst->uinteger = 0; break;
	}
}

static JsonValue* JsonObjectLookup(JsonObject* object, const std::string& key, bool create) {
	auto* impl = JsonObjectImpl(object);
	if (impl == nullptr) {
		return JsonStaticNullValue();
	}
	auto it = impl->find(key);
	if (it != impl->end()) {
		return &it->second->value;
	}
	if (!create) {
		return JsonStaticNullValue();
	}
	auto* pair = JsonObjectPairNew(key, nullptr, nullptr);
	(*impl)[key] = pair;
	return &pair->value;
}

static bool JsonValueFromNlohmann(JsonValue* out, const nlohmann::json& value) {
	if (out == nullptr) {
		return false;
	}

	JsonValueClear(out);
#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
	if (g_path_parser_hooks.before_materialize != nullptr) {
		g_path_parser_hooks.before_materialize();
	}
#endif
	if (value.is_null()) {
		return true;
	}
	if (value.is_boolean()) {
		out->type    = JsonValueTypeBoolean;
		out->boolean = value.get<bool>();
		return true;
	}
	if (value.is_number_integer() && !value.is_number_unsigned()) {
		out->type    = JsonValueTypeInteger;
		out->integer = value.get<int64_t>();
		return true;
	}
	if (value.is_number_unsigned()) {
		out->type     = JsonValueTypeUInteger;
		out->uinteger = value.get<uint64_t>();
		return true;
	}
	if (value.is_number_float()) {
		out->type = JsonValueTypeReal;
		out->real = value.get<double>();
		return true;
	}
	if (value.is_string()) {
		out->string = JsonStringNew(value.get_ref<const std::string&>());
		out->type   = JsonValueTypeString;
		return true;
	}
	if (value.is_array()) {
		out->array = JsonArrayNew();
		out->type  = JsonValueTypeArray;
		for (const auto& item: value) {
			auto child = std::unique_ptr<JsonValue, decltype(&JsonValueDelete)>(JsonValueNew(), JsonValueDelete);
			if (!JsonValueFromNlohmann(child.get(), item)) {
				return false;
			}
			child->parent = out;
			out->array->impl->push_back(child.get());
			child.release();
		}
		return true;
	}
	if (value.is_object()) {
		out->object = JsonObjectNew();
		out->type   = JsonValueTypeObject;
		for (auto it = value.begin(); it != value.end(); ++it) {
			auto pair = std::unique_ptr<JsonObjectPair, decltype(&JsonObjectPairDelete)>(
			    JsonObjectPairNew(it.key(), nullptr, out), JsonObjectPairDelete);
			if (!JsonValueFromNlohmann(&pair->value, *it)) {
				return false;
			}
			pair->value.parent = out;
			out->object->impl->emplace(it.key(), pair.get());
			pair.release();
		}
		return true;
	}
	return false;
}

static void JsonSerializeValue(const JsonValue* value, std::string* out) {
	if (value == nullptr) {
		out->append("null");
		return;
	}
	switch (value->type) {
		case JsonValueTypeBoolean: out->append(value->boolean ? "true" : "false"); break;
		case JsonValueTypeInteger: out->append(std::to_string(value->integer)); break;
		case JsonValueTypeUInteger: out->append(std::to_string(value->uinteger)); break;
		case JsonValueTypeReal: out->append(std::to_string(value->real)); break;
		case JsonValueTypeString:
			out->push_back('"');
			out->append(*JsonStringImpl(value->string));
			out->push_back('"');
			break;
		case JsonValueTypeArray: {
			out->push_back('[');
			bool first = true;
			for (auto* item: *JsonArrayImpl(value->array)) {
				if (!first) {
					out->push_back(',');
				}
				first = false;
				JsonSerializeValue(item, out);
			}
			out->push_back(']');
			break;
		}
		case JsonValueTypeObject: {
			out->push_back('{');
			bool first = true;
			for (const auto& item: *JsonObjectImpl(value->object)) {
				if (!first) {
					out->push_back(',');
				}
				first = false;
				out->push_back('"');
				out->append(item.first);
				out->append("\":");
				JsonSerializeValue(&item.second->value, out);
			}
			out->push_back('}');
			break;
		}
		default: out->append("null"); break;
	}
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonMemAllocatorCtor(void* self) {
	PRINT_NAME();

	return self;
}

static JsonInitParameter2* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitParameter2Ctor(JsonInitParameter2* self) {
	PRINT_NAME();

	if (self != nullptr) {
		self->allocator                 = nullptr;
		self->user_data                 = nullptr;
		self->file_buffer_size          = 0;
		self->special_float_format_type = 0;
		self->reserved[0]               = 0;
		self->reserved[1]               = 0;
		self->reserved[2]               = 0;
	}

	return self;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitParameter2SetAllocator(JsonInitParameter2* self, void* allocator,
                                                         void* user_data) {
	PRINT_NAME();

	if (self != nullptr) {
		self->allocator = allocator;
		self->user_data = user_data;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitParameter2SetFileBufferSize(JsonInitParameter2* self,
                                                              size_t              size) {
	PRINT_NAME();

	if (self != nullptr) {
		self->file_buffer_size = size;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitParameter2SetSpecialFloatFormatType(JsonInitParameter2* self,
                                                                      uint32_t            type) {
	PRINT_NAME();

	if (self != nullptr) {
		self->special_float_format_type = type;
	}
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitializerCtor(void* self) {
	PRINT_NAME();

	return self;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitializerInitialize(void*                     self,
                                                   const JsonInitParameter2* init_param) {
	PRINT_NAME();

	LOGF("\t self       = 0x%016" PRIx64 "\n"
	     "\t init_param = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(self), reinterpret_cast<uint64_t>(init_param));

	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitializerInitializeV1(void* self, const void* init_param) {
	PRINT_NAME();

	LOGF("\t self       = 0x%016" PRIx64 "\n"
	     "\t init_param = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(self), reinterpret_cast<uint64_t>(init_param));

	return 0;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitializerTerminate(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitializerDtor(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonMemAllocatorDtor(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueCtor(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));

	if (self != nullptr) {
		JsonValueInit(reinterpret_cast<JsonValue*>(self));
	}

	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueCopyCtor(JsonValue* self, const JsonValue* src) {
	PRINT_NAME();

	JsonValueInit(self);
	JsonValueCopy(self, src);
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueTypeCtor(JsonValue* self, uint32_t type) {
	PRINT_NAME();

	JsonValueInit(self);
	JsonValueSetEmptyType(self, type);
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueBoolCtor(JsonValue* self, bool value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeBoolean;
		self->boolean = value;
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueIntCtor(JsonValue* self, int64_t value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeInteger;
		self->integer = value;
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueUIntCtor(JsonValue* self, uint64_t value) {
	// ps5-vsh: Value::Value(unsigned long). Same storage as int64.
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeInteger;
		self->integer = static_cast<int64_t>(value);
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueDoubleCtor(JsonValue* self, double value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type = JsonValueTypeReal;
		self->real = value;
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueCStringCtor(JsonValue* self, const char* value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type   = JsonValueTypeString;
		self->string = JsonStringNew(value != nullptr ? value : "");
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueStringCtor(JsonValue* self, const JsonString* value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type   = JsonValueTypeString;
		self->string = JsonStringNew(*JsonStringImpl(value));
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueObjectCtor(JsonValue* self, const JsonObject* value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type   = JsonValueTypeObject;
		self->object = new JsonObject {JsonObjectCopy(value, self)};
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueArrayCtor(JsonValue* self, const JsonArray* value) {
	PRINT_NAME();

	JsonValueInit(self);
	if (self != nullptr) {
		self->type  = JsonValueTypeArray;
		self->array = new JsonArray {JsonArrayCopy(value, self)};
	}
	return self;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSetObject(JsonValue* self, const JsonObject* value) {
	PRINT_NAME();

	if (self != nullptr) {
		// The source may be this value's object or one of its descendants.
		auto* object = new JsonObject {JsonObjectCopy(value, self)};
		JsonValueClear(self);
		self->type   = JsonValueTypeObject;
		self->object = object;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSetArray(JsonValue* self, const JsonArray* src) {
	PRINT_NAME();
	if (self != nullptr && src != nullptr && src->impl != nullptr) {
		auto* copy = new JsonArray {JsonArrayCopy(src, self)};
		JsonValueClear(self);
		self->type = JsonValueTypeArray;
		self->array = copy;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueDtor(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));

	JsonValueClear(reinterpret_cast<JsonValue*>(self));
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueAssign(JsonValue* self, const JsonValue* src) {
	PRINT_NAME();

	JsonValueCopy(self, src);
	return self;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSetBool(JsonValue* self, bool value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeBoolean;
		self->boolean = value;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSetInt(JsonValue* self, int64_t value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type    = JsonValueTypeInteger;
		self->integer = value;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSetUInt(JsonValue* self, uint64_t value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type     = JsonValueTypeUInteger;
		self->uinteger = value;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSetDouble(JsonValue* self, double value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type = JsonValueTypeReal;
		self->real = value;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSetString(JsonValue* self, const JsonString* value) {
	PRINT_NAME();

	JsonValueClear(self);
	if (self != nullptr) {
		self->type   = JsonValueTypeString;
		self->string = JsonStringNew(*JsonStringImpl(value));
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSetType(JsonValue* self, uint32_t type) {
	PRINT_NAME();

	JsonValueClear(self);
	JsonValueSetEmptyType(self, type);
}

static uint32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetType(const JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr ? self->type : JsonValueTypeNull);
}

static JsonArray* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueReferArray(JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeArray ? self->array : nullptr);
}

static JsonObject* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueReferObject(JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeObject ? self->object : nullptr);
}

static const JsonString* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetString(const JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeString ? self->string
	                                                             : JsonStaticString());
}

static const JsonArray* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetArray(const JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeArray ? self->array : JsonStaticArray());
}

static const JsonObject* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetObject(const JsonValue* self) {
	PRINT_NAME();

	return (self != nullptr && self->type == JsonValueTypeObject ? self->object
	                                                             : JsonStaticObject());
}

static const bool* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetBoolean(const JsonValue* self) {
	PRINT_NAME();

	static const bool false_value = false;
	return (self != nullptr && self->type == JsonValueTypeBoolean ? &self->boolean : &false_value);
}

static bool __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueToBool(const JsonValue* self) {
	PRINT_NAME();
	if (self == nullptr) {
		return false;
	}
	switch (self->type) {
		case JsonValueTypeNull:
			return false;
		case JsonValueTypeBoolean:
			return self->boolean;
		case JsonValueTypeInteger:
			return self->integer != 0;
		case JsonValueTypeUInteger:
			return self->uinteger != 0;
		case JsonValueTypeReal:
			return self->real != 0.0;
		case JsonValueTypeString:
			return self->string != nullptr && self->string->impl != nullptr && !self->string->impl->empty();
		case JsonValueTypeArray:
		case JsonValueTypeObject:
			return true;
		default:
			return true;
	}
}

static const int64_t* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetInteger(const JsonValue* self) {
	PRINT_NAME();

	static const int64_t zero = 0;
	if (self != nullptr &&
	    (self->type == JsonValueTypeInteger || self->type == JsonValueTypeUInteger)) {
		return &self->integer;
	}
	return &zero;
}

static const uint64_t* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetUInteger(const JsonValue* self) {
	PRINT_NAME();

	static const uint64_t zero = 0;
	if (self != nullptr &&
	    (self->type == JsonValueTypeUInteger || self->type == JsonValueTypeInteger)) {
		return &self->uinteger;
	}
	return &zero;
}

static const double* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetReal(const JsonValue* self) {
	PRINT_NAME();

	static const double zero = 0.0;
	return (self != nullptr && self->type == JsonValueTypeReal ? &self->real : &zero);
}

// Shared const lookup for Value::getValue(String) and operator[](char*).
// This HLE family has no element-access handlers/root-specific null values yet;
// misses return its borrowed static null, without inserting or changing type.
static const JsonValue* JsonValueLookupKey(const JsonValue* self, const std::string& key) {
	if (self == nullptr || self->type != JsonValueTypeObject) {
		return JsonStaticNullValue();
	}
	const auto* object = JsonObjectImpl(static_cast<const JsonObject*>(self->object));
	const auto it = object->find(key);
	return it != object->end() ? &it->second->value : JsonStaticNullValue();
}

static const JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueGetValueKey(const JsonValue* self,
                                                                                               const JsonString* key) {
	PRINT_NAME();
	if (key == nullptr || key->impl == nullptr) {
		return JsonStaticNullValue();
	}
	return JsonValueLookupKey(self, *JsonStringImpl(key));
}

static const JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueIndexString(const JsonValue* self, const char* key) {
	PRINT_NAME();
	return JsonValueLookupKey(self, key != nullptr ? key : "");
}

static const JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueIndexUInt(const JsonValue* self, uint64_t index) {
	PRINT_NAME();

	if (self == nullptr || self->type != JsonValueTypeArray) {
		return JsonStaticNullValue();
	}
	const auto* impl = JsonArrayImpl(self->array);
	return (index < impl->size() ? (*impl)[static_cast<size_t>(index)] : JsonStaticNullValue());
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueReferValueKey(JsonValue* self, const JsonString* key) {
	PRINT_NAME();

	if (self == nullptr || self->type != JsonValueTypeObject) {
		return nullptr;
	}
	const auto* impl = JsonObjectImpl(self->object);
	const auto  it   = impl->find(*JsonStringImpl(key));
	return (it != impl->end() ? &it->second->value : nullptr);
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueToString(const JsonValue* self, JsonString* dst) {
	PRINT_NAME();

	auto* impl = JsonStringImpl(dst);
	if (impl == nullptr) {
		return;
	}
	if (self != nullptr && self->type == JsonValueTypeString) {
		*impl = *JsonStringImpl(self->string);
	} else {
		impl->clear();
		JsonSerializeValue(self, impl);
	}
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSerialize(JsonValue* self, JsonString* dst) {
	PRINT_NAME();

	auto* impl = JsonStringImpl(dst);
	if (impl != nullptr) {
		impl->clear();
		JsonSerializeValue(self, impl);
	}
	return 0;
}

static JsonString* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringCtor(JsonString* self) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new std::string;
	}
	return self;
}

static JsonString* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringCStringCtor(JsonString* self, const char* str) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new std::string(str != nullptr ? str : "");
	}
	return self;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringDtor(JsonString* self) {
	PRINT_NAME();

	if (self != nullptr) {
		delete self->impl;
		self->impl = nullptr;
	}
}

static JsonString* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringAssign(JsonString* self, const JsonString* src) {
	PRINT_NAME();

	auto* impl = JsonStringImpl(self);
	if (impl != nullptr) {
		*impl = *JsonStringImpl(src);
	}
	return self;
}

static JsonString* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringCopyCtor(JsonString* self, const JsonString* src) {
	// ps5-vsh: String::String(const String&) was missing; unconstructed
	// strings crash readers at [str+0x1c].
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new std::string(src != nullptr ? *JsonStringImpl(src) : "");
	}
	return self;
}

static const char* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringCStr(const JsonString* self) {
	PRINT_NAME();

	return JsonStringImpl(self)->c_str();
}

static size_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringLength(const JsonString* self) {
	PRINT_NAME();

	return JsonStringImpl(self)->length();
}

static JsonString* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringAppend(JsonString* self, const char* text) {
	PRINT_NAME();
	if (self != nullptr && text != nullptr) { JsonStringImpl(self)->append(text); }
	return self;
}

static size_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonStringFind(const JsonString* self, const char* text, size_t start) {
	PRINT_NAME();
	return (self != nullptr && text != nullptr) ? JsonStringImpl(self)->find(text, start) : std::string::npos;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayCtor(JsonArray* self) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new std::vector<JsonValue*>;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayDtor(JsonArray* self) {
	PRINT_NAME();

	JsonArrayDestroy(self);
}

static JsonArray* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayPushBack(JsonArray* self, const JsonValue* value) {
	PRINT_NAME();

	auto* impl = JsonArrayImpl(self);
	if (impl != nullptr) {
		auto* copy = JsonValueNew();
		JsonValueCopy(copy, value);
		impl->push_back(copy);
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayBack(const JsonArray* self) {
	PRINT_NAME();

	const auto* impl = JsonArrayImpl(self);
	return (!impl->empty() ? impl->back() : JsonStaticNullValue());
}

static size_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArraySize(const JsonArray* self) {
	PRINT_NAME();

	return JsonArrayImpl(self)->size();
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayClear(JsonArray* self) {
	PRINT_NAME();
	auto* values = JsonArrayImpl(self);
	if (values != nullptr) {
		for (auto* value: *values) { JsonValueClear(value); delete value; }
		values->clear();
	}
}

static JsonArray* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayAssign(JsonArray* self, const JsonArray* src) {
	PRINT_NAME();
	if (self != nullptr && self != src) {
		auto* impl = JsonArrayCopy(src, nullptr);
		JsonArrayDestroy(self);
		self->impl = impl;
	}
	return self;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayCopyCtor(JsonArray* self, const JsonArray* src) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = JsonArrayCopy(src, nullptr);
	}
}

// The non-trivial guest iterator is returned through an explicit result pointer.
static JsonArrayIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayBegin(JsonArrayIterator* result,
                                                    const JsonArray* self) {
	PRINT_NAME();

	if (result != nullptr) {
		const auto* impl = JsonArrayImpl(self);
		result->position = new JsonArrayIteratorState(impl, impl->empty() ? SIZE_MAX : 0);
	}
	return result;
}

static JsonArrayIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayEnd(JsonArrayIterator* result,
                                                  const JsonArray* self) {
	PRINT_NAME();

	if (result != nullptr) {
		const auto* impl = JsonArrayImpl(self);
		result->position = new JsonArrayIteratorState(impl, SIZE_MAX);
	}
	return result;
}

static bool __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorNotEqual(const JsonArrayIterator* self,
                                                 const JsonArrayIterator* other) {
	PRINT_NAME();

	if (self == nullptr || other == nullptr || self->position == nullptr || other->position == nullptr) {
		return false;
	}
	return self->position->owner != other->position->owner || self->position->index != other->position->index;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorDereference(const JsonArrayIterator* self) {
	PRINT_NAME();

	if (self != nullptr && self->position != nullptr && self->position->owner != nullptr &&
	    self->position->index < self->position->owner->size()) {
		return (*self->position->owner)[self->position->index];
	}
	return JsonStaticNullValue();
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorArrow(const JsonArrayIterator* self) {
	PRINT_NAME();

	return JsonArrayIteratorDereference(self);
}

static JsonArrayIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorIncrement(JsonArrayIterator* self) {
	PRINT_NAME();

	if (self != nullptr && self->position != nullptr && self->position->owner != nullptr &&
	    self->position->index < self->position->owner->size()) {
		if (++self->position->index == self->position->owner->size()) {
			self->position->index = SIZE_MAX;
		}
	}
	return self;
}

static JsonArrayIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorPostInc(JsonArrayIterator* self, int) {
	PRINT_NAME();
	return JsonArrayIteratorIncrement(self); // Native +0xb510 returns updated iterator&.
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorCtor(JsonArrayIterator* self) {
	self->position = new JsonArrayIteratorState;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorCopyCtor(JsonArrayIterator* self,
                                                                                         const JsonArrayIterator* other) {
	self->position = other->position ? new JsonArrayIteratorState(*other->position) : nullptr;
}

static JsonArrayIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorAssign(JsonArrayIterator* self,
                                                                                                   const JsonArrayIterator* other) {
	// Native +0xb450 updates existing state, and releases it for a null node.
	if (self->position != nullptr) {
		if (other->position != nullptr && other->position->owner != nullptr) {
			*self->position = *other->position;
		} else {
			delete self->position;
			self->position = nullptr;
		}
	}
	return self;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorAdvance(JsonArrayIterator* self, size_t offset) {
	while (offset-- != 0 && self->position != nullptr && self->position->index != SIZE_MAX) {
		JsonArrayIteratorIncrement(self);
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonArrayIteratorDtor(JsonArrayIterator* self) {
	PRINT_NAME();
	delete self->position;
	self->position = nullptr;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectCtor(JsonObject* self) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = new JsonObjectMap;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectCopyCtor(JsonObject* self, const JsonObject* src) {
	PRINT_NAME();

	if (self != nullptr) {
		self->impl = JsonObjectCopy(src, nullptr);
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectDtor(JsonObject* self) {
	PRINT_NAME();

	JsonObjectDestroy(self);
}

static JsonObject* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectAssign(JsonObject* self, const JsonObject* src) {
	PRINT_NAME();

	if (self != nullptr && self != src) {
		auto* impl = JsonObjectCopy(src, nullptr);
		JsonObjectDestroy(self);
		self->impl = impl;
	}
	return self;
}

static JsonValue* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIndex(JsonObject* self, const JsonString* key) {
	PRINT_NAME();

	return JsonObjectLookup(self, *JsonStringImpl(key), true);
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectClear(JsonObject* self) {
	PRINT_NAME();

	auto* impl = JsonObjectImpl(self);
	if (impl != nullptr) {
		for (auto& item: *impl) {
			JsonObjectPairDelete(item.second);
		}
		impl->clear();
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectPairCtor(JsonObjectPair* self,
                                                      const JsonString* key, const JsonValue* value) {
	PRINT_NAME();
	if (self != nullptr) {
		self->key.impl = new std::string(*JsonStringImpl(key));
		self->reserved = 0;
		self->value = {};
		JsonValueCopy(&self->value, value);
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectPairDtor(JsonObjectPair* self) {
	PRINT_NAME();
	if (self != nullptr) {
		JsonStringDtor(&self->key);
		JsonValueClear(&self->value);
	}
}

static JsonObjectIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectInsert(JsonObjectIterator* result,
                                                        JsonObject* self, const JsonObjectPair* pair) {
	PRINT_NAME();
	auto* object = JsonObjectImpl(self);
	const auto& key = *JsonStringImpl(&pair->key);
	auto [it, inserted] = object->try_emplace(key, nullptr);
	if (inserted) {
		it->second = JsonObjectPairNew(key, &pair->value, nullptr);
	}
	result->position = new JsonObjectIteratorState {object, it};
	return result;
}

static JsonObjectIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectBegin(JsonObjectIterator* result, const JsonObject* self) {
	const auto* object = JsonObjectImpl(self);
	result->position = new JsonObjectIteratorState {object, object->begin()};
	return result;
}

static JsonObjectIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectEnd(JsonObjectIterator* result, const JsonObject* self) {
	const auto* object = JsonObjectImpl(self);
	result->position = new JsonObjectIteratorState {object, object->end()};
	return result;
}

static JsonObjectIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectFind(JsonObjectIterator* result,
                                                       const JsonObject* self, const JsonString* key) {
	const auto* object = JsonObjectImpl(self);
	result->position = new JsonObjectIteratorState {object, object->find(*JsonStringImpl(key))};
	return result;
}

static bool __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorEqual(const JsonObjectIterator* self, const JsonObjectIterator* other) {
	const auto* lhs = self ? self->position : nullptr;
	const auto* rhs = other ? other->position : nullptr;
	if (!lhs || !rhs) { return false; }
	return lhs->owner == rhs->owner && (!lhs->owner || lhs->position == rhs->position);
}

static bool __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorNotEqual(const JsonObjectIterator* self, const JsonObjectIterator* other) {
	if (!self || !other || !self->position || !other->position) { return false; }
	return !JsonObjectIteratorEqual(self, other);
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorCtor(JsonObjectIterator* self) {
	if (self != nullptr) {
		self->position = new JsonObjectIteratorState;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorCopyCtor(JsonObjectIterator* self,
                                                                   const JsonObjectIterator* other) {
	if (self != nullptr) {
		self->position = (other && other->position) ? new JsonObjectIteratorState(*other->position) : nullptr;
	}
}

static JsonObjectIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorAssign(JsonObjectIterator* self,
                                                                 const JsonObjectIterator* other) {
	if (self != nullptr && self->position != nullptr) {
		if (other && other->position && other->position->owner) {
			*self->position = *other->position;
		} else {
			delete self->position;
			self->position = nullptr;
		}
	}
	return self;
}

static JsonObjectIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorIncrement(JsonObjectIterator* self) {
	if (self && self->position && self->position->owner && self->position->position != self->position->owner->end()) {
		++self->position->position;
	}
	return self;
}

static JsonObjectIterator* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorPostInc(JsonObjectIterator* self, int) {
	return JsonObjectIteratorIncrement(self); // Native +0xc650 returns updated iterator&.
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorAdvance(JsonObjectIterator* self, size_t offset) {
	while (offset-- != 0 && self->position && self->position->owner &&
	       self->position->position != self->position->owner->end()) {
		JsonObjectIteratorIncrement(self);
	}
}

static JsonObjectPair* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorArrow(const JsonObjectIterator* self) {
	auto* state = self ? self->position : nullptr;
	if (!state || !state->owner || state->position == state->owner->end()) {
		return nullptr;
	}
	return state->position->second;
}

static JsonObjectPair* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorDereference(const JsonObjectIterator* self) {
	return JsonObjectIteratorArrow(self);
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorDtor(JsonObjectIterator* self) {
	PRINT_NAME();
	if (self != nullptr) {
		delete self->position;
		self->position = nullptr;
	}
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonParserParse(JsonValue* dst, const char* src, size_t size) {
	PRINT_NAME();

	if (dst == nullptr || src == nullptr) {
		return JSON_ERROR_INVALID_ARGUMENT;
	}

	JsonValue parsed {};
	JsonValueInit(&parsed);
	auto json = nlohmann::json::parse(src, src + size, nullptr, false);
	if (json.is_discarded() || !JsonValueFromNlohmann(&parsed, json)) {
		JsonValueClear(&parsed);
		return JSON_ERROR_PARSE_INVALID_CHAR;
	}

	JsonValueCopy(dst, &parsed);
	JsonValueClear(&parsed);
	return 0;
}

// Owns only an fd successfully returned by KernelOpen. This does not repair
// dependency exception safety: KernelOpen can allocate after publishing its fd,
// and KernelClose can allocate before removing it. A throw from KernelClose in
// this implicitly noexcept destructor would terminate, not return JSON NOMEM.
class JsonGuestFileDescriptor final {
public:
	explicit JsonGuestFileDescriptor(int fd) noexcept: m_fd(fd) {}
	~JsonGuestFileDescriptor() {
#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
		if (g_path_parser_hooks.close != nullptr) {
			g_path_parser_hooks.close(m_fd);
			return;
		}
#endif
		// Native +0x6a97 ignores close's result and returns the parser result.
		LibKernel::FileSystem::KernelClose(m_fd);
	}
	JsonGuestFileDescriptor(const JsonGuestFileDescriptor&) = delete;
	JsonGuestFileDescriptor& operator=(const JsonGuestFileDescriptor&) = delete;
private:
	int m_fd;
};

// Fixed-size transport storage, with no file-size probe or host-path bypass.
// The JSON DOM still grows with the input. Initializer/custom allocator state
// is not implemented by this HLE family; this is its no-handler file adapter.
class JsonGuestFileBuffer final: public std::streambuf {
public:
	explicit JsonGuestFileBuffer(int fd): m_fd(fd) {}
	int32_t Error() const { return m_error; }
	bool Eof() const { return m_eof; }

protected:
	int_type underflow() override {
		if (gptr() != egptr()) { return traits_type::to_int_type(*gptr()); }
		if (m_error != 0 || m_eof) { return traits_type::eof(); }
		auto read = &LibKernel::FileSystem::KernelRead;
#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
		if (g_path_parser_hooks.read != nullptr) { read = g_path_parser_hooks.read; }
#endif
		const auto count = read(m_fd, m_buffer, sizeof(m_buffer));
		if (count < 0) {
			m_error = static_cast<int32_t>(count); // Preserve kernel read errors, including ENOENT.
			return traits_type::eof();
		}
		if (count == 0) {
			m_eof = true;
			return traits_type::eof();
		}
		setg(m_buffer, m_buffer, m_buffer + count);
		return traits_type::to_int_type(*gptr());
	}

private:
	int m_fd;
	int32_t m_error = 0;
	bool m_eof = false;
	char m_buffer[4096];
};

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonParserParseVsh(JsonValue* dst, const char* path) {
	// LB3jxppxyKU, native +0x6980: parse(Value&, const char* path), TWO arguments.
	PRINT_NAME();
	if (dst == nullptr || path == nullptr) { return JSON_ERROR_INVALID_ARGUMENT; }

	try {
		const int fd = LibKernel::FileSystem::KernelOpen(path, 0, 0x16d);
		if (fd < 0) {
			if (fd == LibKernel::KERNEL_ERROR_ENOENT) { return JSON_ERROR_NOFILE; }
			return fd == -1 ? JSON_ERROR_NOINIT : fd;
		}
		JsonGuestFileDescriptor descriptor(fd); // Own before even constructing the stream buffer.
		JsonGuestFileBuffer input(fd);
		std::istream stream(&input);
		auto json = nlohmann::json::parse(stream, nullptr, false);
		if (input.Error() != 0) { return input.Error(); }
		// A NUL byte must not masquerade as real EOF in the nlohmann lexer.
		if (json.is_discarded() || !input.Eof()) { return JSON_ERROR_PARSE_INVALID_CHAR; }

		JsonValue parsed {};
		auto cleanup = std::unique_ptr<JsonValue, decltype(&JsonValueClear)>(&parsed, JsonValueClear);
		if (!JsonValueFromNlohmann(&parsed, json)) { return JSON_ERROR_PARSE_INVALID_CHAR; }
		// Commit without a second allocating deep copy. Keep a borrowed Pair's
		// ownership metadata; only immediate children's parent changes on moving.
		parsed.parent = dst->parent;
		parsed.rootparam = dst->rootparam;
		JsonValueClear(dst);
		*dst = parsed;
		parsed = {};
		if (dst->type == JsonValueTypeArray) {
			for (auto* child: *dst->array->impl) { child->parent = dst; }
		} else if (dst->type == JsonValueTypeObject) {
			for (auto& item: *dst->object->impl) { item.second->value.parent = dst; }
		}
		return 0;
	} catch (const std::bad_alloc&) {
		// Covers exceptions that reach this handler, not general OOM safety.
		// KernelOpen publication, KernelClose, and nlohmann's allocating noexcept
		// DOM teardown are dependency exception boundaries (see staging notes).
		return JSON_ERROR_NOMEM;
	}
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonInitializerSetGlobalNullAccessCallback(void* self, void* callback,
                                                                        void* context) {
	PRINT_NAME();

	LOGF("\t self     = 0x%016" PRIx64 "\n"
	     "\t callback = 0x%016" PRIx64 "\n"
	     "\t context  = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(self), reinterpret_cast<uint64_t>(callback),
	     reinterpret_cast<uint64_t>(context));

	return 0;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueCount(const JsonValue* self) {
	PRINT_NAME();

	if (self == nullptr) {
		return 0;
	}

	switch (self->type) {
		case JsonValueTypeArray: return static_cast<int32_t>(JsonArrayImpl(self->array)->size());
		case JsonValueTypeObject: return static_cast<int32_t>(JsonObjectImpl(self->object)->size());
		default: return 0;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueClearMethod(void* self) {
	PRINT_NAME();

	LOGF("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));

	auto* value = reinterpret_cast<JsonValue*>(self);
	if (value != nullptr) {
		auto* parent    = value->parent;
		auto* rootparam = value->rootparam;
		JsonValueClear(value);
		value->parent    = parent;
		value->rootparam = rootparam;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonValueSwap(JsonValue* self, JsonValue* other) {
	PRINT_NAME();
	if (self != nullptr && other != nullptr && self != other) {
		std::swap(*self, *other);
	}
}

// Register supported container operations once per identity, from one provider.
// Calling this first for Json also makes historical first-NID lookup coherent.
static void InitJsonContainers(Loader::SymbolDatabase* s, const char* module) {
	const auto* g_module = module; // LIB_FUNC's exact module identity.
	LIB_FUNC("OJPTonqdg0I", LibJson2::JsonObjectCtor);
	LIB_FUNC("SkKczWHdklc", LibJson2::JsonObjectCtor);
	LIB_FUNC("a+W7HHlwpBs", LibJson2::JsonObjectCopyCtor);
	LIB_FUNC("tSRqQ-+Y4PE", LibJson2::JsonObjectCopyCtor);
	LIB_FUNC("5JmzZt8twAo", LibJson2::JsonObjectDtor);
	LIB_FUNC("1siRBursJjw", LibJson2::JsonObjectDtor);
	LIB_FUNC("urOpESTBZmo", LibJson2::JsonObjectAssign);
	LIB_FUNC("ERuf9y0DY84", LibJson2::JsonObjectIndex);
	LIB_FUNC("oH8aBmLU+fc", LibJson2::JsonObjectClear);
	LIB_FUNC("+hYsagaV-Ug", LibJson2::JsonObjectPairCtor);
	LIB_FUNC("lt3J4dPvEtM", LibJson2::JsonObjectPairCtor);
	LIB_FUNC("Q0ng4sWj2E8", LibJson2::JsonObjectPairDtor);
	LIB_FUNC("TM0Sj+Ebins", LibJson2::JsonObjectPairDtor);
	LIB_FUNC("ol7KhQl8kfw", LibJson2::JsonObjectInsert);
	LIB_FUNC("ivMCitpSQNk", LibJson2::JsonObjectEnd);
	LIB_FUNC("xhAcaIwnrgk", LibJson2::JsonObjectBegin);
	LIB_FUNC("ehN1Biw3tw8", LibJson2::JsonObjectFind);
	LIB_FUNC("q3YO1c0RqFo", LibJson2::JsonObjectIteratorEqual);
	LIB_FUNC("+isUKw4zud4", LibJson2::JsonObjectIteratorNotEqual);
	LIB_FUNC("a2Zv3idT0sU", LibJson2::JsonObjectIteratorCtor);
	LIB_FUNC("OxO+V0ouoS4", LibJson2::JsonObjectIteratorCtor);
	LIB_FUNC("JATlaeBu+lU", LibJson2::JsonObjectIteratorCopyCtor);
	LIB_FUNC("r5CShmcua+M", LibJson2::JsonObjectIteratorCopyCtor);
	LIB_FUNC("w7nqTT3zgxU", LibJson2::JsonObjectIteratorAssign);
	LIB_FUNC("DlWmn2ZQuWY", LibJson2::JsonObjectIteratorIncrement);
	LIB_FUNC("dIVxrrdpS9M", LibJson2::JsonObjectIteratorPostInc);
	LIB_FUNC("h-V8r4EF7bo", LibJson2::JsonObjectIteratorAdvance);
	LIB_FUNC("EnesV4gXDtE", LibJson2::JsonObjectIteratorArrow);
	LIB_FUNC("ZCd6IYoD3Bc", LibJson2::JsonObjectIteratorDereference);
	LIB_FUNC("hoINmSMlYjI", LibJson2::JsonObjectIteratorDtor);
	LIB_FUNC("patTzc7zBjw", LibJson2::JsonObjectIteratorDtor);
	LIB_FUNC("JP-PtKMiI1E", LibJson2::JsonArrayCtor);
	LIB_FUNC("t6AHhkWu4mY", LibJson2::JsonArrayCtor);
	LIB_FUNC("bI5AGFMydrA", LibJson2::JsonArrayCopyCtor);
	LIB_FUNC("6NlbpyIfUuY", LibJson2::JsonArrayCopyCtor);
	LIB_FUNC("HJ8GpRT1aiw", LibJson2::JsonArrayDtor);
	LIB_FUNC("MIyMPw7YTGM", LibJson2::JsonArrayDtor);
	LIB_FUNC("w5UbvPLGye0", LibJson2::JsonArrayAssign);
	LIB_FUNC("qVOSuDRHCpA", LibJson2::JsonArrayClear);
	LIB_FUNC("zQtLRTqceMY", LibJson2::JsonArrayPushBack);
	LIB_FUNC("bAM9Qwofus0", LibJson2::JsonArrayBack);
	LIB_FUNC("rQGJeNjOuUk", LibJson2::JsonArraySize);
	LIB_FUNC("bcH5EnFE2xY", LibJson2::JsonArrayBegin);
	LIB_FUNC("WXF2ihRF+B8", LibJson2::JsonArrayEnd);
	LIB_FUNC("TAPTWYaYf8Q", LibJson2::JsonArrayIteratorCtor);
	LIB_FUNC("JSg1MSo7NKE", LibJson2::JsonArrayIteratorCtor);
	LIB_FUNC("IaIvX3kgyb0", LibJson2::JsonArrayIteratorCopyCtor);
	LIB_FUNC("Hdc3DSIsG2I", LibJson2::JsonArrayIteratorCopyCtor);
	LIB_FUNC("T3WReElX4BY", LibJson2::JsonArrayIteratorAssign);
	LIB_FUNC("5AZPp99ogrc", LibJson2::JsonArrayIteratorNotEqual);
	LIB_FUNC("wcgr5mte7T8", LibJson2::JsonArrayIteratorDereference);
	LIB_FUNC("iAIYn4oAWvI", LibJson2::JsonArrayIteratorArrow);
	LIB_FUNC("w5+VCznos5E", LibJson2::JsonArrayIteratorIncrement);
	LIB_FUNC("GEylwSsiGG8", LibJson2::JsonArrayIteratorPostInc);
	LIB_FUNC("43MESLndGvQ", LibJson2::JsonArrayIteratorAdvance);
	LIB_FUNC("9yLjn46Ypfs", LibJson2::JsonArrayIteratorDtor);
	LIB_FUNC("9niNrXsF3Bc", LibJson2::JsonArrayIteratorDtor);
}

namespace LibJsonSub {
LIB_VERSION("Json2", 1, "Json", 1, 1);
static void InitJsonSub(Loader::SymbolDatabase* s) {
	LIB_FUNC("LB3jxppxyKU", LibJson2::JsonParserParseVsh);
	// Native 13.60: getValue(String) at +0x5100; const operator[] jumps to it.
	LIB_FUNC("MsMOdxWfbwQ", LibJson2::JsonValueGetValueKey);
	LIB_FUNC("clF7J7N9xXE", LibJson2::JsonValueGetValueKey);
	LIB_FUNC("sn4HNCtNRzY", LibJson2::JsonValueGetUInteger);
}
} // namespace LibJsonSub

LIB_DEFINE(InitNet_1_Json2) {
	InitJsonContainers(s, "Json");
	InitJsonContainers(s, "Json2");
	LibJsonSub::InitJsonSub(s);
	LIB_FUNC("-hJRce8wn1U", LibJson2::JsonMemAllocatorCtor);
	LIB_FUNC("WSOuge5IsCg", LibJson2::JsonInitParameter2Ctor);
	LIB_FUNC("GvGvswb0v34", LibJson2::JsonInitParameter2Ctor);
	LIB_FUNC("I2QC8PYhJWY", LibJson2::JsonInitParameter2SetAllocator);
	LIB_FUNC("W72B9ylU2JA", LibJson2::JsonInitParameter2SetAllocator);
	LIB_FUNC("Eu95jmqn5Rw", LibJson2::JsonInitParameter2SetFileBufferSize);
	LIB_FUNC("WVZBP4IyM+E", LibJson2::JsonInitParameter2SetSpecialFloatFormatType);
	LIB_FUNC("cK6bYHf-Q5E", LibJson2::JsonInitializerCtor);
	LIB_FUNC("IXW-z8pggfg", LibJson2::JsonInitializerInitialize);
	LIB_FUNC("Cxwy7wHq4J0", LibJson2::JsonInitializerInitializeV1);
	LIB_FUNC("PR5k1penBLM", LibJson2::JsonInitializerTerminate);
	LIB_FUNC("RujUxbr3haM", LibJson2::JsonInitializerDtor);
	LIB_FUNC("OcAgPxcq5Vk", LibJson2::JsonMemAllocatorDtor);
	LIB_FUNC("qBMjqyBn3OM", LibJson2::JsonValueCtor);
	LIB_FUNC("-wa17B7TGnw", LibJson2::JsonValueCtor);
	LIB_FUNC("fSb2oQTNrgA", LibJson2::JsonValueCopyCtor);
	LIB_FUNC("CbrT3dwDILo", LibJson2::JsonValueTypeCtor);
	LIB_FUNC("WTtYf+cNnXI", LibJson2::JsonValueDtor);
	LIB_FUNC("0eUrW9JAxM0", LibJson2::JsonValueDtor);
	LIB_FUNC("S5JxQnoGF3E", LibJson2::JsonParserParse);
	LIB_FUNC("LB3jxppxyKU", LibJson2::JsonParserParseVsh); // Compatible Json2 module identity.
	LIB_FUNC("HwDt5lD9Bfo", LibJson2::JsonValueIndexString);
	LIB_FUNC("epJ6x2LV0kU", LibJson2::JsonValueGetString);
	LIB_FUNC("L1KAkYWml-M", LibJson2::JsonStringCStr);
	LIB_FUNC("sZIoMRGO+jk", LibJson2::JsonValueStringCtor);
	LIB_FUNC("4zrm6VrgIAw", LibJson2::JsonValueAssign);
	LIB_FUNC("nM5XqdeXFPw", LibJson2::JsonValueReferArray);
	LIB_FUNC("-NxEk7XLkDY", LibJson2::JsonValueReferObject);
	LIB_FUNC("iZeYfOxtMRg", LibJson2::JsonValueArrayCtor);
	LIB_FUNC("dFCphqnd+a4", LibJson2::JsonValueSetObject);
	LIB_FUNC("0lLK8+kDqmE", LibJson2::JsonValueIntCtor);
	LIB_FUNC("x4AUdbhpRB0", LibJson2::JsonValueUIntCtor); // ps5-vsh: Value ulong ctor
	LIB_FUNC("zTwZdI8AZ5Y", LibJson2::JsonValueGetBoolean);
	LIB_FUNC("R7FDWtcN6f8", LibJson2::JsonValueSerialize);
	LIB_FUNC("wLsJlmgEIaI", LibJson2::JsonValueReferValueKey);
	LIB_FUNC("Ncel8t2Rrpc", LibJson2::JsonValueToString);
	LIB_FUNC("UeuWT+yNdCQ", LibJson2::JsonValueBoolCtor);
	LIB_FUNC("3xUXnmUkXfo", LibJson2::JsonValueObjectCtor);
	LIB_FUNC("cn9svYGWKDQ", LibJson2::JsonStringAssign);
	LIB_FUNC("b9V6fmppLXY", LibJson2::JsonValueCStringCtor);
	LIB_FUNC("EUH+EmT-v9E", LibJson2::JsonStringLength);
	LIB_FUNC("XlWbvieLj2M", LibJson2::JsonValueIndexUInt);
	LIB_FUNC("IlsmvBtMkak", LibJson2::JsonValueGetObject);
	LIB_FUNC("SHtAad20YYM", LibJson2::JsonValueGetType);
	LIB_FUNC("DIxvoy7Ngvk", LibJson2::JsonValueGetInteger);
	LIB_FUNC("qSmqLXXCPas", LibJson2::JsonStringCtor);
	LIB_FUNC("0CAesfH963Q", LibJson2::JsonStringCopyCtor); // ps5-vsh: String copy ctor
	LIB_FUNC("ONT8As5R1ug", LibJson2::JsonValueGetArray);
	LIB_FUNC("3qrge7L-AU4", LibJson2::JsonValueGetReal);
	LIB_FUNC("sOmU4vnx3s0", LibJson2::JsonValueDoubleCtor);
	LIB_FUNC("5yHuiWXo2gg", LibJson2::JsonValueSetBool);
	LIB_FUNC("QxVVYhP-mvg", LibJson2::JsonValueSetInt);
	LIB_FUNC("SIe1ZmW7e7s", LibJson2::JsonValueSetUInt);
	LIB_FUNC("BSmWDIkV4w4", LibJson2::JsonValueSetDouble);
	LIB_FUNC("6l3Bv2gysNc", LibJson2::JsonValueSetString);
	LIB_FUNC("9KUZFjI1IxA", LibJson2::JsonStringCStringCtor);
	LIB_FUNC("cG1VE2HMl6c", LibJson2::JsonStringDtor);
	LIB_FUNC("IKQimvG9Wqs", LibJson2::JsonValueSetType);
	LIB_FUNC("RBw+4NukeGQ", LibJson2::JsonValueCount);
	LIB_FUNC("+drDFyAS6u4", LibJson2::JsonInitializerSetGlobalNullAccessCallback);
	LIB_FUNC("00oCq0RwSAY", LibJson2::JsonInitializerSetGlobalNullAccessCallback);
	LIB_FUNC("FIjXN2TkuTs", LibJson2::JsonValueClearMethod);
	LIB_FUNC("9PqrCLfKZZI", LibJson2::JsonValueSwap);
	LIB_FUNC("a-aMMUXqrN0", LibJson2::JsonValueToBool);
	LIB_FUNC("Va5FJ6TmNCw", LibJson2::JsonStringAppend);
	LIB_FUNC("yjsf6SvXA08", LibJson2::JsonStringFind);
	LIB_FUNC("195ad-jAsTU", LibJson2::JsonValueSetArray);
	LIB_FUNC("XL8+BUqjB1w", LibJson2::JsonValueAssign);
	LIB_FUNC("6qFqND4iwPA", LibJson2::JsonInitializerCtor);
}

} // namespace LibJson2

} // namespace Libs
