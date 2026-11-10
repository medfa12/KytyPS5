#include "common/abi.h"
#include "libs/libMbusWire.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

// ps5-vsh: Mbus WIRE side (host-injectable event lifecycle). kyty's
// SceMbusEventReceive (libC.cpp) already delivers a scripted login schedule
// to SceLoginMgrMbusEventThread, and MbusRegInsert mirrors login/bind
// registrations. Missing was the wire: event create/destroy lifecycle plus a
// host-injectable event that flows through the SAME receive path, so future
// ShellUI/transport events reach the guest without touching the scripted
// schedule. This file adds ONLY that wire; it alters no existing
// schedule/registry behavior and performs no guest writes at all.
// NIDs (from _ProbeLogs/nids.csv, format nid,hex,name,library):
//   0LkfqnKtPQg,0xD0B91FAA72AD3D08,sceMbusEventCreate,,0,aerolib;ps5rs;ps5rs-cat
//   AcARz3iay1I,0x01C011CF789ACB52,sceMbusEventDestroy,,0,aerolib;ps5rs;ps5rs-cat
// Both verified absent repo-wide (src/libs + libC.cpp scans clean) before
// use. NOTE: c08SEHicDNU is a DIFFERENT symbol (sceMbusEventCreate_, trailing
// underscore, already stubbed in libC.cpp) and is left untouched.

namespace Libs {

namespace MbusWire {

LIB_VERSION("Mbus", 1, "Mbus", 1, 1);

namespace Wire {

// Host handle table: monotonic host handle (from 1) -> u32 event id.
// The id mapping is a placeholder (prototype unknown: low32(a0)); refined
// when disasm reveals the true arg mapping. Handles are host-side only for
// now: Create does NOT write the handle back to the guest (no guest writes).
// Single mutex guards both the handle table and the host inject queue below.
static std::mutex                          g_wireMutex;
static uint64_t                            g_nextHandle = 1;
static std::unordered_map<uint64_t, uint32_t> g_eventHandles;
static std::deque<uint32_t>                g_hostQueue;

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusEventCreate(uint64_t a0, uint64_t a1, const void* attr,
                                            uint64_t a3) {
	PRINT_NAME();
	// Null-safe: attr is logged, never dereferenced; no guest writes.
	uint64_t handle = 0;
	{
		std::lock_guard<std::mutex> lock(g_wireMutex);
		handle                  = g_nextHandle++;
		g_eventHandles[handle] = static_cast<uint32_t>(a0);
	}
	LOGF("\t a0=%llu a1=%llu attr=%p a3=%llu -> handle=%llu\n",
	     static_cast<unsigned long long>(a0), static_cast<unsigned long long>(a1), attr,
	     static_cast<unsigned long long>(a3), static_cast<unsigned long long>(handle));
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusEventDestroy(uint64_t handle, const void* opt, uint64_t a2,
                                             uint64_t a3) {
	PRINT_NAME();
	// Null-safe: opt is logged, never dereferenced. Idempotent: unknown
	// handle erases nothing and still returns 0 success. No guest writes.
	bool known = false;
	{
		std::lock_guard<std::mutex> lock(g_wireMutex);
		known = g_eventHandles.erase(handle) > 0u;
	}
	LOGF("\t handle=%llu opt=%p a2=%llu a3=%llu known=%d -> 0\n",
	     static_cast<unsigned long long>(handle), opt,
	     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
	     known ? 1 : 0);
	return 0;
}

} // namespace Wire

namespace MbusWireHost {

// Host-side inject API (non-static; declare in a future header as
// `namespace MbusWireHost { void InjectMbusEvent(uint32_t id); }`).
// Pushes {id} onto the mutex-guarded host queue.
// WIRING NOTE: SceMbusEventReceive (libC.cpp) does NOT yet consult this
// queue. The wiring turn will make Receive drain this queue FIRST (ahead of
// the scripted login schedule) so injected ShellUI/transport events flow
// through the SAME receive path; until then pushes only accumulate here and
// no behavior changes today.
void InjectMbusEvent(uint32_t id) {
	std::lock_guard<std::mutex> lock(Wire::g_wireMutex);
	Wire::g_hostQueue.push_back(id);
	LOGF("ps5-vsh: mbus-wire inject queued id=%u depth=%llu (unconsumed: Receive not wired yet)\n",
	     static_cast<unsigned>(id),
	     static_cast<unsigned long long>(Wire::g_hostQueue.size()));
}

} // namespace MbusWireHost

LIB_DEFINE(InitMbusWire_1) {
	LIB_FUNC("0LkfqnKtPQg", Wire::SceMbusEventCreate);  // sceMbusEventCreate[Mbus]
	LIB_FUNC("AcARz3iay1I", Wire::SceMbusEventDestroy); // sceMbusEventDestroy[Mbus]
}

} // namespace MbusWire

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp):

namespace MbusWire {
LIB_DEFINE(InitMbusWire_1);
} // namespace MbusWire

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   MbusWire::InitMbusWire_1(s);
*/
