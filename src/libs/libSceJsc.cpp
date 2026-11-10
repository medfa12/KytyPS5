#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef KYTY_USE_HERMES
#include <hermes/hermes.h>
#include <jsi/jsi.h>
#endif

namespace Libs {

// ps5-vsh: JavaScriptCore C API over the host Hermes VM (NPXS40000 JSCd, the
// RNPS JS daemon, imports 56 of these; unresolved slots resolve to NULL so
// the first context creation otherwise crashes at pc=0).
//
// Lifetime model (GC-safe without explicit roots):
// - JSGlobalContextRef = host pointer to JscCtx (owns the Hermes runtime).
// - JSStringRef = host pointer to JscStr (std::string + refcount).
// - JSValueRef/JSObjectRef = small-integer ids into a JS-side registry
//   object (globalThis.__kytyJscVals); JS holds the reference, so Hermes GC
//   cannot collect live values. Id 0 = null.
// - JSObjectCallAsFunctionCallback bodies are deferred: the created function
//   logs and returns undefined (guest->host re-entry needs JIT trampolines).
// All entry points are null-safe and never dereference guest memory without
// a probe. Errors funnel through the JSValueRef* exception out-param when
// provided (else swallowed + null/0 return).

namespace Jsc {

LIB_VERSION("Jsc", 1, "Jsc", 1, 1);

#ifdef KYTY_USE_HERMES

namespace {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
inline bool JscProbe(uint64_t addr, size_t len) {
	if (addr == 0 || len == 0) {
		return false;
	}
	const uint64_t end = addr + len;
	if (end < addr) {
		return false;
	}
	for (uint64_t p = addr; p < end;) {
		MEMORY_BASIC_INFORMATION mi {};
		if (VirtualQuery(reinterpret_cast<const void*>(p), &mi, sizeof(mi)) == 0 ||
		    mi.State != MEM_COMMIT) {
			return false;
		}
		const DWORD prot = mi.Protect & 0xffu;
		if (!(prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
		      prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
		      prot == PAGE_EXECUTE_WRITECOPY)) {
			return false;
		}
		const uint64_t region_end =
		    reinterpret_cast<uint64_t>(mi.BaseAddress) + mi.RegionSize;
		if (region_end <= p) {
			return false;
		}
		p = region_end;
	}
	return true;
}
#else
inline bool JscProbe(uint64_t addr, size_t len) {
	return addr != 0 && len != 0;
}
#endif

struct JscCtx {
	std::unique_ptr<facebook::hermes::HermesRuntime> rt;
	long                                            refs = 1;
};

struct JscStr {
	std::string s;
	long        refs = 1;
};

inline facebook::hermes::HermesRuntime* CtxChecked(const void* ctx) {
	if (ctx == nullptr) {
		return nullptr;
	}
	const auto* c = static_cast<const JscCtx*>(ctx);
	return (c->rt != nullptr) ? c->rt.get() : nullptr;
}

// Registry helpers: values live JS-side under globalThis.__kytyJscVals.
inline bool JscEnsureRegistry(facebook::hermes::HermesRuntime& rt) {
	try {
		facebook::jsi::Value v = rt.global().getProperty(rt, "__kytyJscVals");
		if (v.isObject()) {
			return true;
		}
	} catch (...) {
	}
	try {
		facebook::jsi::Object reg(rt);
		reg.setProperty(rt, "next", facebook::jsi::Value(1.0));
		rt.global().setProperty(rt, "__kytyJscVals", std::move(reg));
		return true;
	} catch (...) {
		return false;
	}
}

inline uint64_t JscStore(facebook::hermes::HermesRuntime& rt, facebook::jsi::Value v) {
	facebook::jsi::Object reg = rt.global().getPropertyAsObject(rt, "__kytyJscVals");
	const double           idd =
	    reg.getProperty(rt, "next").asNumber();
	const auto id = static_cast<uint64_t>(idd);
	reg.setProperty(rt, facebook::jsi::PropNameID::forAscii(rt, std::to_string(id).c_str()),
	                v);
	reg.setProperty(rt, "next", facebook::jsi::Value(idd + 1.0));
	return id;
}

inline uint64_t JscStore(facebook::hermes::HermesRuntime& rt, facebook::jsi::Object o) {
	facebook::jsi::Value v(rt, o);
	return JscStore(rt, std::move(v));
}

inline facebook::jsi::Value JscLoad(facebook::hermes::HermesRuntime& rt, uint64_t id) {
	if (id == 0) {
		return facebook::jsi::Value::undefined();
	}
	facebook::jsi::Object reg = rt.global().getPropertyAsObject(rt, "__kytyJscVals");
	facebook::jsi::Value v =
	    reg.getProperty(rt, facebook::jsi::PropNameID::forAscii(rt, std::to_string(id).c_str()));
	if (v.isUndefined()) {
		return facebook::jsi::Value::undefined();
	}
	return v;
}

inline void JscSetException(facebook::hermes::HermesRuntime& rt, uint64_t* outExc,
                            facebook::jsi::Value v) {
	if (outExc == nullptr) {
		return;
	}
	if (!JscProbe(reinterpret_cast<uint64_t>(outExc), sizeof(*outExc))) {
		return;
	}
	*outExc = JscStore(rt, std::move(v));
}

inline uint64_t JscIdOf(const void* p) {
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
}

// JS ToBoolean over a Hermes value (Hermes jsi::Value has no toBool()).
inline bool JscToBool(facebook::hermes::HermesRuntime& rt, const facebook::jsi::Value& v) {
	if (v.isUndefined() || v.isNull()) {
		return false;
	}
	if (v.isBool()) {
		return v.getBool();
	}
	if (v.isNumber()) {
		const double d = v.asNumber();
		return d != 0.0 && d == d; // nonzero, not NaN
	}
	if (v.isString()) {
		return !v.toString(rt).utf8(rt).empty();
	}
	return true; // objects (incl. zero-capable hosts) are truthy
}

} // namespace

// --- contexts ---
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSGlobalContextCreateInGroup(uint64_t group, uint64_t cls) {
	PRINT_NAME();
	(void)group;
	(void)cls;
	auto rt = facebook::hermes::makeHermesRuntime();
	if (!rt) {
		LOGF("\t makeHermesRuntime failed -> 0\n");
		return 0;
	}
	auto* ctx  = new (std::nothrow) JscCtx();
	if (ctx == nullptr) {
		return 0;
	}
	ctx->rt   = std::move(rt);
	ctx->refs = 1;
	if (!JscEnsureRegistry(*ctx->rt)) {
		delete ctx;
		return 0;
	}
	LOGF("\t ctx = %p\n", static_cast<void*>(ctx));
	return reinterpret_cast<uint64_t>(ctx);
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSGlobalContextRetain(uint64_t ctx) {
	PRINT_NAME();
	if (ctx != 0) {
		++static_cast<JscCtx*>(reinterpret_cast<void*>(ctx))->refs;
	}
	return ctx;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSGlobalContextRelease(uint64_t ctx) {
	PRINT_NAME();
	if (ctx == 0) {
		return;
	}
	auto* c = static_cast<JscCtx*>(reinterpret_cast<void*>(ctx));
	if (--c->refs <= 0) {
		delete c;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSGlobalContextSetUnhandledRejectionCallback(uint64_t ctx,
                                                                            uint64_t cb) {
	PRINT_NAME();
	(void)ctx;
	(void)cb;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSContextGetGlobalObject(uint64_t ctx) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		return JscStore(*rt, facebook::jsi::Value(*rt, rt->global()));
	} catch (...) {
		return 0;
	}
}

// --- strings ---
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSStringCreateWithUTF8CString(const char* s) {
	PRINT_NAME();
	auto* js = new (std::nothrow) JscStr();
	if (js == nullptr) {
		return 0;
	}
	if (s != nullptr) {
		if (!JscProbe(reinterpret_cast<uint64_t>(s), 1)) {
			delete js;
			return 0;
		}
		size_t n = 0;
		while (n < 65536 && JscProbe(reinterpret_cast<uint64_t>(s) + n, 1) && s[n] != '\0') {
			n++;
		}
		js->s.assign(s, n);
	}
	js->refs = 1;
	return reinterpret_cast<uint64_t>(js);
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSStringRetain(uint64_t s) {
	PRINT_NAME();
	if (s != 0) {
		++reinterpret_cast<JscStr*>(s)->refs;
	}
	return s;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSStringRelease(uint64_t s) {
	PRINT_NAME();
	if (s == 0) {
		return;
	}
	auto* js = reinterpret_cast<JscStr*>(s);
	if (--js->refs <= 0) {
		delete js;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSStringGetMaximumUTF8CStringSize(uint64_t s) {
	PRINT_NAME();
	if (s == 0) {
		return 0;
	}
	return reinterpret_cast<JscStr*>(s)->s.size() + 1;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSStringGetUTF8CString(uint64_t s, char* out, uint64_t cap) {
	PRINT_NAME();
	if (s == 0 || out == nullptr || cap == 0) {
		return 0;
	}
	const std::string& src = reinterpret_cast<JscStr*>(s)->s;
	const size_t       n   = src.size() < static_cast<size_t>(cap - 1)
	                           ? src.size()
	                           : static_cast<size_t>(cap - 1);
	if (!JscProbe(reinterpret_cast<uint64_t>(out), n + 1)) {
		return 0;
	}
	std::memcpy(out, src.data(), n);
	out[n] = '\0';
	return n;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSStringIsEqual(uint64_t a, uint64_t b) {
	PRINT_NAME();
	if (a == 0 || b == 0) {
		return (a == b) ? 1u : 0u;
	}
	return (reinterpret_cast<JscStr*>(a)->s == reinterpret_cast<JscStr*>(b)->s) ? 1u : 0u;
}

// --- values ---
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueGetType(uint64_t ctx, uint64_t v) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		const auto val = JscLoad(*rt, v);
		if (val.isUndefined()) {
			return 0u;
		}
		if (val.isNull()) {
			return 1u;
		}
		if (val.isBool()) {
			return 2u;
		}
		if (val.isNumber()) {
			return 3u;
		}
		if (val.isString()) {
			return 4u;
		}
		if (val.isObject()) {
			return 5u;
		}
		return 6u;
	} catch (...) {
		return 0u;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueToBoolean(uint64_t ctx, uint64_t v) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0u;
	}
	try {
		return JscToBool(*rt, JscLoad(*rt, v)) ? 1u : 0u;
	} catch (...) {
		return 0u;
	}
}

static double __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueToNumber(uint64_t ctx, uint64_t v, uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0.0;
	}
	try {
		return JscLoad(*rt, v).asNumber();
	} catch (const facebook::jsi::JSError& e) {
		JscSetException(*rt, exc, facebook::jsi::String::createFromUtf8(
		                                *rt, reinterpret_cast<const uint8_t*>(e.getMessage().c_str()),
		                                e.getMessage().size()));
		return 0.0;
	} catch (...) {
		return 0.0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueToStringCopy(uint64_t ctx, uint64_t v, uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		auto* js = new (std::nothrow) JscStr();
		if (js == nullptr) {
			return 0;
		}
		js->s    = JscLoad(*rt, v).toString(*rt).utf8(*rt);
		js->refs = 1;
		return reinterpret_cast<uint64_t>(js);
	} catch (const facebook::jsi::JSError& e) {
		JscSetException(*rt, exc, facebook::jsi::String::createFromUtf8(
		                                *rt, reinterpret_cast<const uint8_t*>(e.getMessage().c_str()),
		                                e.getMessage().size()));
		return 0;
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueToObject(uint64_t ctx, uint64_t v, uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Value val = JscLoad(*rt, v);
		if (val.isUndefined() || val.isNull()) {
			return 0;
		}
		facebook::jsi::Object obj =
		    val.isObject() ? val.asObject(*rt)
		                   : facebook::jsi::Object(*rt); // box primitives
		if (!val.isObject()) {
			obj.setProperty(*rt, "value", val);
		}
		return JscStore(*rt, facebook::jsi::Value(*rt, std::move(obj)));
	} catch (const facebook::jsi::JSError& e) {
		JscSetException(*rt, exc, facebook::jsi::String::createFromUtf8(
		                                *rt, reinterpret_cast<const uint8_t*>(e.getMessage().c_str()),
		                                e.getMessage().size()));
		return 0;
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueIsObject(uint64_t ctx, uint64_t v) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0u;
	}
	try {
		return JscLoad(*rt, v).isObject() ? 1u : 0u;
	} catch (...) {
		return 0u;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueIsEqual(uint64_t ctx, uint64_t a, uint64_t b,
                                             uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0u;
	}
	(void)exc;
	try {
		facebook::jsi::Value av = JscLoad(*rt, a);
		facebook::jsi::Value bv = JscLoad(*rt, b);
		if (av.isNumber() && bv.isNumber()) {
			return (av.asNumber() == bv.asNumber()) ? 1u : 0u;
		}
		if (av.isString() && bv.isString()) {
			return (av.toString(*rt).utf8(*rt) == bv.toString(*rt).utf8(*rt)) ? 1u : 0u;
		}
		if (av.isBool() && bv.isBool()) {
			return (av.getBool() == bv.getBool()) ? 1u : 0u;
		}
		if (av.isUndefined() && bv.isUndefined()) {
			return 1u;
		}
		if (av.isNull() && bv.isNull()) {
			return 1u;
		}
		return 0u;
	} catch (...) {
		return 0u;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueIsInstanceOfConstructor(uint64_t ctx, uint64_t v,
                                                              uint64_t ctor,
                                                              uint64_t* exc) {
	PRINT_NAME();
	(void)exc;
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0u;
	}
	try {
		facebook::jsi::Value val = JscLoad(*rt, v);
		facebook::jsi::Value cv  = JscLoad(*rt, ctor);
		if (!val.isObject() || !cv.isObject() ||
		    !cv.asObject(*rt).isFunction(*rt)) {
			return 0u;
		}
		return val.asObject(*rt).instanceOf(*rt, cv.asObject(*rt).asFunction(*rt)) ? 1u
		                                                                             : 0u;
	} catch (...) {
		return 0u;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueIsObjectOfClass(uint64_t ctx, uint64_t v, uint64_t cls) {
	PRINT_NAME();
	(void)cls;
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0u;
	}
	try {
		return JscLoad(*rt, v).isObject() ? 1u : 0u; // no class table: object check
	} catch (...) {
		return 0u;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueProtect(uint64_t ctx, uint64_t v) {
	PRINT_NAME();
	(void)ctx;
	return v; // values are JS-rooted via the registry; protect is a no-op
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueUnprotect(uint64_t ctx, uint64_t v) {
	PRINT_NAME();
	(void)ctx;
	(void)v; // see above: leak-by-design, Hermes frees at runtime teardown
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueMakeUndefined(uint64_t ctx) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		return JscStore(*rt, facebook::jsi::Value::undefined());
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueMakeNull(uint64_t ctx) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		return JscStore(*rt, facebook::jsi::Value::null());
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueMakeBoolean(uint64_t ctx, uint64_t b) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		return JscStore(*rt, facebook::jsi::Value(b != 0));
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueMakeNumber(uint64_t ctx, double d) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		return JscStore(*rt, facebook::jsi::Value(d));
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueMakeString(uint64_t ctx, uint64_t s) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr || s == 0) {
		return 0;
	}
	try {
		return JscStore(
		    *rt, facebook::jsi::Value(
		             *rt, facebook::jsi::String::createFromAscii(
		                      *rt, reinterpret_cast<JscStr*>(s)->s)));
	} catch (...) {
		return 0;
	}
}

// --- objects ---
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectMake(uint64_t ctx, uint64_t cls, uint64_t data) {
	PRINT_NAME();
	(void)cls;
	(void)data;
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		return JscStore(*rt, facebook::jsi::Object(*rt));
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectMakeFunctionWithCallback(uint64_t ctx, uint64_t name,
                                                                uint64_t cb) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	std::string fname = "jsc_cb";
	if (name != 0) {
		fname = reinterpret_cast<JscStr*>(name)->s;
	}
	LOGF("\t name = %s cb = 0x%016llx -> deferred undefined-function\n", fname.c_str(),
	     static_cast<unsigned long long>(cb));
	(void)cb; // deferred: guest->host re-entry needs JIT trampolines
	try {
		auto fn = facebook::jsi::Function::createFromHostFunction(
		    *rt, facebook::jsi::PropNameID::forAscii(*rt, fname.c_str()), 8,
		    [fname](facebook::jsi::Runtime& rt2, const facebook::jsi::Value&,
		            const facebook::jsi::Value*, size_t) -> facebook::jsi::Value {
			    LOGF("\t [RNPS host] JSC deferred callback %s invoked -> undefined\n",
			         fname.c_str());
			    (void)rt2;
			    return facebook::jsi::Value::undefined();
		    });
		return JscStore(*rt, std::move(fn));
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectMakeArray(uint64_t ctx, uint64_t argc,
                                                 const uint64_t* argv, uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Array arr(*rt, static_cast<size_t>(argc));
		if (argv != nullptr && argc != 0) {
			if (!JscProbe(reinterpret_cast<uint64_t>(argv), static_cast<size_t>(argc) * 8)) {
				return 0;
			}
			for (uint64_t i = 0; i < argc; i++) {
				arr.setValueAtIndex(*rt, static_cast<size_t>(i), JscLoad(*rt, argv[i]));
			}
		}
		return JscStore(*rt, std::move(arr));
	} catch (const facebook::jsi::JSError& e) {
		JscSetException(*rt, exc, facebook::jsi::String::createFromUtf8(
		                                *rt, reinterpret_cast<const uint8_t*>(e.getMessage().c_str()),
		                                e.getMessage().size()));
		return 0;
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectCallAsFunction(uint64_t ctx, uint64_t obj, uint64_t thiz,
                                                     uint64_t argc, const uint64_t* argv,
                                                     uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Value fv = JscLoad(*rt, obj);
		if (!fv.isObject() || !fv.asObject(*rt).isFunction(*rt)) {
			return JscStore(*rt, facebook::jsi::Value::undefined());
		}
		std::vector<facebook::jsi::Value> fargs;
		if (argv != nullptr && argc != 0) {
			if (!JscProbe(reinterpret_cast<uint64_t>(argv), static_cast<size_t>(argc) * 8)) {
				return 0;
			}
			for (uint64_t i = 0; i < argc; i++) {
				fargs.push_back(JscLoad(*rt, argv[i]));
			}
		}
		facebook::jsi::Value thisv = (thiz != 0)
		    ? JscLoad(*rt, thiz)
		    : facebook::jsi::Value::undefined();
		facebook::jsi::Value out;
		if (thisv.isUndefined()) {
			out = fv.asObject(*rt).asFunction(*rt).call(
			    *rt, static_cast<const facebook::jsi::Value*>(fargs.data()), fargs.size());
		} else {
			out = fv.asObject(*rt).asFunction(*rt).callWithThis(
			    *rt, thisv.asObject(*rt), static_cast<const facebook::jsi::Value*>(fargs.data()), fargs.size());
		}
		return JscStore(*rt, std::move(out));
	} catch (const facebook::jsi::JSError& e) {
		JscSetException(*rt, exc, facebook::jsi::String::createFromUtf8(
		                                *rt, reinterpret_cast<const uint8_t*>(e.getMessage().c_str()),
		                                e.getMessage().size()));
		return 0;
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectCallAsConstructor(uint64_t ctx, uint64_t obj,
                                                         uint64_t argc,
                                                         const uint64_t* argv,
                                                         uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Value fv = JscLoad(*rt, obj);
		if (!fv.isObject() || !fv.asObject(*rt).isFunction(*rt)) {
			return 0;
		}
		std::vector<facebook::jsi::Value> fargs;
		if (argv != nullptr && argc != 0) {
			if (!JscProbe(reinterpret_cast<uint64_t>(argv), static_cast<size_t>(argc) * 8)) {
				return 0;
			}
			for (uint64_t i = 0; i < argc; i++) {
				fargs.push_back(JscLoad(*rt, argv[i]));
			}
		}
		facebook::jsi::Value constructed = fv.asObject(*rt).asFunction(*rt).callAsConstructor(
		    *rt, static_cast<const facebook::jsi::Value*>(fargs.data()), fargs.size());
		return JscStore(*rt, std::move(constructed));
	} catch (const facebook::jsi::JSError& e) {
		JscSetException(*rt, exc, facebook::jsi::String::createFromUtf8(
		                                *rt, reinterpret_cast<const uint8_t*>(e.getMessage().c_str()),
		                                e.getMessage().size()));
		return 0;
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectGetProperty(uint64_t ctx, uint64_t obj,
                                                  uint64_t name, uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr || name == 0) {
		return 0;
	}
	try {
		facebook::jsi::Value ov = JscLoad(*rt, obj);
		if (!ov.isObject()) {
			return 0;
		}
		const std::string& ns = reinterpret_cast<JscStr*>(name)->s;
		facebook::jsi::Value out =
		    ov.asObject(*rt).getProperty(*rt, ns.c_str());
		(void)exc;
		return JscStore(*rt, std::move(out));
	} catch (const facebook::jsi::JSError& e) {
		JscSetException(*rt, exc, facebook::jsi::String::createFromUtf8(
		                                *rt, reinterpret_cast<const uint8_t*>(e.getMessage().c_str()),
		                                e.getMessage().size()));
		return 0;
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectGetPropertyAtIndex(uint64_t ctx, uint64_t obj,
                                                          uint64_t idx, uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Value ov = JscLoad(*rt, obj);
		if (!ov.isObject()) {
			return 0;
		}
		facebook::jsi::Value out = ov.asObject(*rt).getProperty(
		    *rt, std::to_string(static_cast<unsigned long long>(idx)).c_str());
		(void)exc;
		return JscStore(*rt, std::move(out));
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectSetProperty(uint64_t ctx, uint64_t obj, uint64_t name,
                                                   uint64_t v, uint64_t attrs,
                                                   uint64_t* exc) {
	PRINT_NAME();
	(void)attrs;
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr || name == 0) {
		return 0;
	}
	try {
		facebook::jsi::Value ov = JscLoad(*rt, obj);
		if (!ov.isObject()) {
			return 0;
		}
		const std::string& ns = reinterpret_cast<JscStr*>(name)->s;
		facebook::jsi::Value vv = JscLoad(*rt, v);
		ov.asObject(*rt).setProperty(*rt, ns.c_str(), vv);
		(void)exc;
		return JscStore(*rt, facebook::jsi::Value::undefined());
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectSetPropertyAtIndex(uint64_t ctx, uint64_t obj,
                                                          uint64_t idx, uint64_t v,
                                                          uint64_t* exc) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Value ov = JscLoad(*rt, obj);
		if (!ov.isObject()) {
			return 0;
		}
		ov.asObject(*rt).setProperty(
		    *rt, std::to_string(static_cast<unsigned long long>(idx)).c_str(),
		    JscLoad(*rt, v));
		(void)exc;
		return JscStore(*rt, facebook::jsi::Value::undefined());
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectHasProperty(uint64_t ctx, uint64_t obj, uint64_t name) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr || name == 0) {
		return 0u;
	}
	try {
		facebook::jsi::Value ov = JscLoad(*rt, obj);
		if (!ov.isObject()) {
			return 0u;
		}
		const std::string& ns = reinterpret_cast<JscStr*>(name)->s;
		return ov.asObject(*rt).hasProperty(*rt, ns.c_str()) ? 1u : 0u;
	} catch (...) {
		return 0u;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectCopyPropertyNames(uint64_t ctx, uint64_t obj) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Value ov = JscLoad(*rt, obj);
		if (!ov.isObject()) {
			return 0;
		}
		facebook::jsi::Array names = ov.asObject(*rt).getPropertyNames(*rt);
		return JscStore(*rt, std::move(names));
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectGetPrivate(uint64_t obj) {
	PRINT_NAME();
	(void)obj;
	return 0; // no private slots: objects carry no host data yet
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectSetPrivate(uint64_t obj, uint64_t data) {
	PRINT_NAME();
	(void)obj;
	(void)data;
	return 0; // accepted, not retained (see above)
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectGetPrototype(uint64_t ctx, uint64_t obj) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Value ov = JscLoad(*rt, obj);
		if (!ov.isObject()) {
			return 0;
		}
		return JscStore(*rt, ov.asObject(*rt).getProperty(*rt, "__proto__"));
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectSetPrototype(uint64_t ctx, uint64_t obj, uint64_t proto) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0;
	}
	try {
		facebook::jsi::Value ov = JscLoad(*rt, obj);
		facebook::jsi::Value pv = JscLoad(*rt, proto);
		if (!ov.isObject()) {
			return 0;
		}
		ov.asObject(*rt).setProperty(*rt, "__proto__", pv);
		return JscStore(*rt, facebook::jsi::Value::undefined());
	} catch (...) {
		return 0;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectIsFunction(uint64_t ctx, uint64_t v) {
	PRINT_NAME();
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0u;
	}
	try {
		facebook::jsi::Value val = JscLoad(*rt, v);
		return (val.isObject() && val.asObject(*rt).isFunction(*rt)) ? 1u : 0u;
	} catch (...) {
		return 0u;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSValueGetTypedArrayType(uint64_t ctx, uint64_t v, uint64_t* exc) {
	PRINT_NAME();
	(void)exc;
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr) {
		return 0u; // kJSTypedArrayTypeNone
	}
	try {
		facebook::jsi::Value val = JscLoad(*rt, v);
		(void)val;
		return 0u; // none: typed arrays unrecognized without ArrayBuffer checks
	} catch (...) {
		return 0u;
	}
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectGetArrayBufferBytesPtr(uint64_t ctx, uint64_t v,
                                                              uint64_t* exc) {
	PRINT_NAME();
	(void)exc;
	(void)ctx;
	(void)v;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSObjectGetArrayBufferByteLength(uint64_t ctx, uint64_t v,
                                                                uint64_t* exc) {
	PRINT_NAME();
	(void)exc;
	(void)ctx;
	(void)v;
	return 0u;
}

// --- classes ---
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSClassCreate(const void* def) {
	PRINT_NAME();
	(void)def;
	return 1; // opaque class token (no layout parsed yet)
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSPropertyNameArrayGetCount(uint64_t arr) {
	PRINT_NAME();
	(void)arr;
	return 0u; // arrays are registry ids, not host arrays: count via GetProperty path
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSPropertyNameArrayGetNameAtIndex(uint64_t arr, uint64_t idx) {
	PRINT_NAME();
	(void)arr;
	(void)idx;
	return 0;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSPropertyNameArrayRelease(uint64_t arr) {
	PRINT_NAME();
	(void)arr;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSPropertyNameAccumulatorAddName(uint64_t acc, uint64_t name) {
	PRINT_NAME();
	(void)acc;
	(void)name;
	return 0;
}

// --- evaluate ---
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JSEvaluateScript(uint64_t ctx, uint64_t script, uint64_t thiz,
                                                uint64_t url, int line, uint64_t* exc) {
	PRINT_NAME();
	(void)thiz;
	(void)url;
	(void)line;
	auto* rt = CtxChecked(reinterpret_cast<const void*>(ctx));
	if (rt == nullptr || script == 0) {
		return 0;
	}
	try {
		const std::string& src = reinterpret_cast<JscStr*>(script)->s;
		facebook::jsi::Value out = rt->evaluateJavaScript(
		    std::make_shared<facebook::jsi::StringBuffer>(src), "<jsc-eval>");
		return JscStore(*rt, std::move(out));
	} catch (const facebook::jsi::JSError& e) {
		JscSetException(*rt, exc, facebook::jsi::String::createFromUtf8(
		                                *rt, reinterpret_cast<const uint8_t*>(e.getMessage().c_str()),
		                                e.getMessage().size()));
		return 0;
	} catch (...) {
		return 0;
	}
}

#else // !KYTY_USE_HERMES

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JscNoVm(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;
	return 0;
}

#endif // KYTY_USE_HERMES

#define JSC_UNK_STUB(nid) static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JscUnk_##nid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { PRINT_NAME(); LOGF("\t jsc-unknown-" #nid " -> 0\n"); (void)a0; (void)a1; (void)a2; (void)a3; return 0; }
JSC_UNK_STUB(96QQlI4MnJw)
JSC_UNK_STUB(C9lqmdwYC1E)
JSC_UNK_STUB(CwdZZNHzMRQ)
JSC_UNK_STUB(EoPU50HFqsU)
JSC_UNK_STUB(IKuRRh1APME)
JSC_UNK_STUB(MatMW4cvnNE)
JSC_UNK_STUB(Mo2fgqsusu0)
JSC_UNK_STUB(P9p8AAofIMQ)
JSC_UNK_STUB(T6utZPmjDPE)
JSC_UNK_STUB(TED5qE1bJCs)
JSC_UNK_STUB(TJLtAXxNzqI)
JSC_UNK_STUB(TwZEjsOMuGM)
JSC_UNK_STUB(UKvzGoD8Kxo)
JSC_UNK_STUB(VlQiDmFVAEw)
JSC_UNK_STUB(WZzoXGc9bMY)
JSC_UNK_STUB(YmkgmczMnR0)
JSC_UNK_STUB(ZoUVzLQCGaQ)
JSC_UNK_STUB(fRiwjxLtUvc)
JSC_UNK_STUB(feXsTjlSg1g)
JSC_UNK_STUB(fqmB8BE3ZuQ)
JSC_UNK_STUB(gV349EB96Eg)
JSC_UNK_STUB(jla2mhD1HJc)
JSC_UNK_STUB(m9Y2LTyzlQc)
JSC_UNK_STUB(mERZOPXePvQ)
JSC_UNK_STUB(rHOj8QNVWcU)
JSC_UNK_STUB(uCIEdxMMR2M)
JSC_UNK_STUB(uhGil2Pav9M)
JSC_UNK_STUB(ygzUuDLGSPI)

LIB_DEFINE(InitJsc_1) {
#ifdef KYTY_USE_HERMES
	LIB_FUNC("+qsTbJFS86A", JSObjectSetPrivate);
	LIB_FUNC("0earDv9xPN8", JSValueMakeString);
	LIB_FUNC("35hf1B8ev14", JSObjectHasProperty);
	LIB_FUNC("49GAz4KpvyA", JSValueGetTypedArrayType);
	LIB_FUNC("5F4qWfh3NsI", JSGlobalContextRetain);
	LIB_FUNC("8-gHRL4zbd8", JSGlobalContextSetUnhandledRejectionCallback);
	// ps5-vsh: 8sZw9-G4ubQ is an OBJECT export (kJSClassDefinitionEmpty, 128 B),
	// not JSClassCreate - see the LIB_OBJECT of the same NID in libJscdTail.cpp
	// (JscTail), which must win. Registering a function here shadowed it whenever
	// this init ran last.
	LIB_FUNC("96QQlI4MnJw", JscUnk_96QQlI4MnJw);
	LIB_FUNC("9bSDYmkp-Vw", JSValueToStringCopy);
	LIB_FUNC("AbA05b6nRfY", JSPropertyNameArrayGetNameAtIndex);
	LIB_FUNC("C9lqmdwYC1E", JscUnk_C9lqmdwYC1E);
	LIB_FUNC("CwdZZNHzMRQ", JscUnk_CwdZZNHzMRQ);
	LIB_FUNC("DGkUQEbknmA", JSStringGetMaximumUTF8CStringSize);
	LIB_FUNC("DSTJsRjpDeg", JSClassCreate);
	LIB_FUNC("EoPU50HFqsU", JscUnk_EoPU50HFqsU);
	LIB_FUNC("FEU0ygVamU4", JSObjectGetArrayBufferBytesPtr);
	LIB_FUNC("IKuRRh1A+-E", JscUnk_IKuRRh1APME);
	LIB_FUNC("LPpUAOnfyxg", JSValueProtect);
	LIB_FUNC("Lcwli-n-8mE", JSObjectCopyPropertyNames);
	LIB_FUNC("MH3ye94AkZc", JSObjectGetPrivate);
	LIB_FUNC("Mat-W4cvnNE", JscUnk_MatMW4cvnNE);
	LIB_FUNC("Mo2fgqsusu0", JscUnk_Mo2fgqsusu0);
	LIB_FUNC("Nm-U+BAjCkY", JSStringRelease);
	LIB_FUNC("NqV8ddwLaiY", JSStringGetUTF8CString);
	LIB_FUNC("P9p8AAofI-Q", JscUnk_P9p8AAofIMQ);
	LIB_FUNC("RJW3AojPP1A", JSContextGetGlobalObject);
	LIB_FUNC("T6utZPmjDPE", JscUnk_T6utZPmjDPE);
	LIB_FUNC("TED5qE1bJCs", JscUnk_TED5qE1bJCs);
	LIB_FUNC("TJLtAXxNzqI", JscUnk_TJLtAXxNzqI);
	LIB_FUNC("Tm9bXhmwlMA", JSObjectMakeFunctionWithCallback);
	LIB_FUNC("TwZEjsOMuGM", JscUnk_TwZEjsOMuGM);
	LIB_FUNC("UKvzGoD8Kxo", JscUnk_UKvzGoD8Kxo);
	LIB_FUNC("UPiqgAaHdtU", JSValueIsInstanceOfConstructor);
	LIB_FUNC("UxDr1iQixuQ", JSStringIsEqual);
	LIB_FUNC("VlQiDmFVAEw", JscUnk_VlQiDmFVAEw);
	LIB_FUNC("WZzoXGc9b-Y", JscUnk_WZzoXGc9bMY);
	LIB_FUNC("X3L4jbWOtkc", JSStringRetain);
	LIB_FUNC("YCOZ80HgcTQ", JSValueMakeBoolean);
	LIB_FUNC("Ymkgmcz-nR0", JscUnk_YmkgmczMnR0);
	LIB_FUNC("YssF41MWxCU", JSEvaluateScript);
	LIB_FUNC("ZSHf8kyWYD4", JSValueMakeNull);
	LIB_FUNC("ZnucTodXqc0", JSValueUnprotect);
	LIB_FUNC("ZoUVzLQCGaQ", JscUnk_ZoUVzLQCGaQ);
	LIB_FUNC("b+Bla9TTD04", JSGlobalContextRelease);
	LIB_FUNC("bW+iY4EnrNY", JSValueToBoolean);
	LIB_FUNC("cT71x7KleG0", JSValueMakeNumber);
	LIB_FUNC("f9xif4l1ZhU", JSObjectCallAsConstructor);
	LIB_FUNC("fRiwjxLtUvc", JscUnk_fRiwjxLtUvc);
	LIB_FUNC("feXsTjlSg1g", JscUnk_feXsTjlSg1g);
	LIB_FUNC("fqmB8BE3ZuQ", JscUnk_fqmB8BE3ZuQ);
	LIB_FUNC("g8-wnopz0jE", JSValueGetType);
	LIB_FUNC("gFwrGZX2VHg", JSPropertyNameArrayGetCount);
	LIB_FUNC("gV349EB96Eg", JscUnk_gV349EB96Eg);
	LIB_FUNC("gcsP-2FEZIk", JSObjectIsFunction);
	LIB_FUNC("hR4Q4+3Kw10", JSPropertyNameArrayRelease);
	LIB_FUNC("huNf91I8Tjg", JSStringCreateWithUTF8CString);
	LIB_FUNC("jla2mhD1HJc", JscUnk_jla2mhD1HJc);
	LIB_FUNC("jn+QXnoXVnc", JSValueIsObject);
	LIB_FUNC("kQ0tHfaz-R4", JSGlobalContextCreateInGroup);
	LIB_FUNC("lOYH420riKo", JSObjectGetPropertyAtIndex);
	LIB_FUNC("lgC8W0Qu1iw", JSObjectCallAsFunction);
	LIB_FUNC("m9Y2LTyzlQc", JscUnk_m9Y2LTyzlQc);
	LIB_FUNC("mERZOPXePvQ", JscUnk_mERZOPXePvQ);
	LIB_FUNC("mtbYjAKi5NQ", JSValueMakeUndefined);
	LIB_FUNC("o3bxF-l3DoQ", JSObjectMakeArray);
	LIB_FUNC("pNliRQzOktU", JSObjectGetProperty);
	LIB_FUNC("pw+zuLAiI3U", JSPropertyNameAccumulatorAddName);
	LIB_FUNC("qma2Q24pDPI", JSValueToObject);
	LIB_FUNC("rHOj8QNVWcU", JscUnk_rHOj8QNVWcU);
	LIB_FUNC("rSsusHq9Y4Y", JSValueToNumber);
	LIB_FUNC("rp9ESANSaSs", JSObjectSetPrototype);
	LIB_FUNC("tr1Uc33FqHY", JSObjectSetPropertyAtIndex);
	LIB_FUNC("uAyWBZtHkgM", JSObjectGetArrayBufferByteLength);
	LIB_FUNC("uCIEdxMMR2M", JscUnk_uCIEdxMMR2M);
	LIB_FUNC("uDNVMv6AgrQ", JSValueIsObjectOfClass);
	LIB_FUNC("uhGil2+av9M", JscUnk_uhGil2Pav9M);
	LIB_FUNC("vtHRQyWv+sw", JSObjectMake);
	LIB_FUNC("w3d6oZK52-I", JSValueIsEqual);
	LIB_FUNC("ygzUuDLGS+I", JscUnk_ygzUuDLGSPI);
	LIB_FUNC("ytHFpyVl7gw", JSObjectGetPrototype);
	LIB_FUNC("z3qLvuQ6R8w", JSObjectSetProperty);
#else
	LIB_FUNC("kQ0tHfaz-R4", JscNoVm);
#endif
}

} // namespace Jsc

} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp ----
//
// namespace Jsc {
// LIB_DEFINE(InitJsc_1);
// } // namespace Jsc
//
// ... and inside Libs::InitAll:
//   Jsc::InitJsc_1(s);
