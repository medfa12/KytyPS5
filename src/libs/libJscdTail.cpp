#include "common/abi.h"
#include "common/common.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs {

// ps5-vsh: NPXS40000 JSCd tail imports (final 12 unresolved slots; any called
// one is a fatal null-PC crash). Real implementations where semantics are
// known, tracers otherwise. All null-safe, no guest writes except via
// probed pointers.

namespace JscdTail {

namespace LibcTail {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

namespace {

uint64_t g_terminateHandler = 0;

inline bool TailProbe(uint64_t addr, size_t len) {
	if (addr == 0 || len == 0) {
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
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
		if (!(prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
		      prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY)) {
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
#else
	(void)len;
	return true;
#endif
}

} // namespace

// NG1phipELJE set_terminate: install C++ terminate handler, return old.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SetTerminate(uint64_t fn) {
	PRINT_NAME();
	const uint64_t old = g_terminateHandler;
	g_terminateHandler = fn;
	LOGF("\t set_terminate(%016llx) -> old %016llx\n",
	     static_cast<unsigned long long>(fn),
	     static_cast<unsigned long long>(old));
	return old;
}

// RY8mQlhg7mI __cxa_current_primary_exception: no exception in flight.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CurrentPrimaryException() {
	PRINT_NAME();
	return 0;
}

// nOIEswYD4Ig __cxa_free_exception: no-op (nothing tracked).
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FreeException(uint64_t ex) {
	PRINT_NAME();
	(void)ex;
	return 0;
}

// zMCYAqNRllc __cxa_demangle: fail closed (status -1, null), no buffer touch.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Demangle(uint64_t mangled, uint64_t buf, uint64_t len,
                                       uint64_t status) {
	PRINT_NAME();
	(void)mangled;
	(void)buf;
	(void)len;
	if (status != 0 && TailProbe(status, sizeof(int))) {
		*reinterpret_cast<int*>(status) = -1;
	}
	return 0;
}

// n8onIBR4Qdk clock_1700: unknown clock query; tracer.
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Clock1700(uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t jsc-tail clock_1700 -> 0\n");
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

LIB_DEFINE(InitJscdTailLibc_1) {
	LIB_FUNC("NG1phipELJE", SetTerminate);
	LIB_FUNC("RY8mQlhg7mI", CurrentPrimaryException);
	LIB_FUNC("nOIEswYD4Ig", FreeException);
	LIB_FUNC("zMCYAqNRllc", Demangle);
	LIB_FUNC("n8onIBR4Qdk", Clock1700);
}

} // namespace LibcTail

namespace JsiTail {

LIB_VERSION("Jsi-0.70", 1, "Jsi-0.70", 1, 1);

// ps5-vsh: JSCd object audit (jscd_audit10.log): 5 Jsi data objects resolve
// to ZERO (patched to no-access sentinel -> any guest read faults). Sizes
// from libSceJsi-0.70.sprx exports.
//
// MEASURED (rnpjsc8): these five ARE real libSceJsi-0.70 exports - GLOBAL
// OBJECT, shndx 10, at libSceJsi+0x183b8 / +0x18468 / +0x18480 / +0x18490 /
// +0x184a0 - and registering them here STEALS them from the loaded module.
// Resolve looks the HLE record up (m_symbols->Find, exact "NID[...][Object]"
// key) before the loaded-module pass at runtimeLinker.cpp:3531, so our blob
// wins whenever we register it. The proof is the six sibling Jsi objects we
// do NOT register - dWOUV2RULCw, n5g-WbYovQI, 6p0JTc4xgmU, -4ONoH5lEy4,
// 7awl4MXo1JE, O7Eofl9gwM4 - every one of them binds real 0x970018xxx, while
// every one of ours binds 0x141b95xxx (59 registered -> HLE, 39 unregistered
// -> REAL, 0 exceptions among Objects).
//
// The stand-in is what killed the guest: &g_jsiGv[0] was passed to
// libSceLibcInternal's exception path as a type_info, whose name pointer at
// [obj+8] is NULL -> strcmp reads address 0 -> mono's SIGSEGV handler runs ->
// pc=0 fatal. Same defect class as the sret stubs fixed in c25eb0f.
//
// The arrays are gone with the registrations: nothing else referenced them,
// and a zero-filled stand-in is strictly worse than the real object.

// ps5-vsh: these four Jsi-0.70_v1 functions are REAL libSceJsi exports and must
// not be stubbed. 42Ri9XtjIMw = libSceJsi+0x3530 = Object::getProperty(rt,
// const char*), which returns through an sret out-slot; the old stub returned 0
// without writing that slot, so the caller kept stale stack bytes (observed:
// 0x900000000, the SceSysCore base) and the next JSI call dereferenced them as a
// HermesValue -> fault at libhermes+0x73A84 (movzx edx,[rax+4]). Siblings, all
// real exports: 2fah4NV1zM0 = +0x3380, 4U74OuGvQpo = +0x44E0,
// 6YiqVRuACqc = +0x5E50.
LIB_DEFINE(InitJscdTailJsi_1) {
	// Intentionally empty: Mat-W4cvnNE / gV349EB96Eg / mERZOPXePvQ /
	// CwdZZNHzMRQ / TJLtAXxNzqI are registered by NOBODY now, so the loaded
	// libSceJsi-0.70.sprx supplies them. Do not re-add zero blobs here; if one
	// of them ever has to be faked, it has to be a faithful object, not zeros.
	(void)s;
}

} // namespace JsiTail

namespace JscTail {

LIB_VERSION("Jsc", 1, "Jsc", 1, 1);

// ps5-vsh: kJSClassDefinitionEmpty (128B per libSceJsc.sprx export).
// Zeroed empty class definition is the correct initial value.
static uint64_t g_jscEmpty[16] = {};

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JscTailUnk(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t jsc-tail 32ajtT0hAck -> 0\n");
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

LIB_DEFINE(InitJscdTailJsc_1) {
	LIB_FUNC("32ajtT0hAck", JscTailUnk);
	LIB_OBJECT("8sZw9-G4ubQ", &g_jscEmpty[0]); // kJSClassDefinitionEmpty
}

} // namespace JscTail

namespace HermesTail {

LIB_VERSION("libhermes-0.11", 1, "libhermes-0.11", 1, 1);

// ps5-vsh: WIDyseMUXEo (rva 0x28fc0 in libhermes-0.11.sprx) is a Hermes
// runtime constructor: `WIDyseMUXEo(out, config)` stores the new runtime in
// *out and returns out. It used to be answered by a zero-returning HLE stub,
// which never wrote the out-param; libNativeExtensions' RN-bridge ctor
// (rva 0x258f4) then read stale stack garbage as the runtime pointer and
// dereferenced its bogus vtable at 0x278d16 (`mov r15,[this+0x78];
// mov rax,[r15]; call [rax+0x30]`) -> host AV at [0x1fc8].
//
// Nothing needs to be emulated here: libNativeExtensions is relocated before
// libhermes-0.11.sprx is loaded (Mono DllImport, later boot), and the linker
// re-relocates unresolved imports every time another module is loaded
// (RuntimeLinker::RelocateAll). Registering the NID at all would permanently
// shadow the real export, because an HLE-resolved slot sets its rela bit and
// is never re-attempted. Leaving it unregistered makes it resolve to the real
// module exactly like Vo9D2A1RRyw (the libhermes Object import) already does.
LIB_DEFINE(InitJscdTailHermes_1) {}

} // namespace HermesTail

namespace RnMediaTail {

LIB_VERSION("libReactNative.Modules.Vsh.Media", 1, "libReactNative.Modules.Vsh.Media", 1, 1);

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetDecryptionKeys(uint64_t a0, uint64_t a1, uint64_t a2,
                                                 uint64_t a3) {
	PRINT_NAME();
	LOGF("\t rn-media getDecryptionKeys -> 0 (no keys; observe caller)\n");
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0;
}

LIB_DEFINE(InitJscdTailRnMedia_1) {
	LIB_FUNC("sv9yvbNVpAs", GetDecryptionKeys);
}

} // namespace RnMediaTail

LIB_DEFINE(InitJscdTail_1) {
	LibcTail::InitJscdTailLibc_1(s);
	JsiTail::InitJscdTailJsi_1(s);
	JscTail::InitJscdTailJsc_1(s);
	HermesTail::InitJscdTailHermes_1(s);
	RnMediaTail::InitJscdTailRnMedia_1(s);
}

} // namespace JscdTail

} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp ----
//
// namespace JscdTail {
// LIB_DEFINE(InitJscdTail_1);
// } // namespace JscdTail
//
// ... and inside Libs::InitAll:
//   JscdTail::InitJscdTail_1(s);
