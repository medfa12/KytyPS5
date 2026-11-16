#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

// ps5-vsh: SceSysCore.elf (system-core daemon) network-config / IPMI-debug /
// Mbus-debug / libc-odds / devtool-log close-out.
// Raw request keys verified verbatim (NID[Lib][LibVer]):
//   26VqvOFbxYo/2ee14ktE1lw/PcdLABhYga4/nO17GBCP-es/QltDK6wWqF0/cKvcNpw4q7o/
//   wIGold7Lro0/TwjkDIPdZ1Q/XfV-XBCuhDo/cYW1ISGlOmo [Net_v1][Net_v1.1]
//   (10, sceNet*)
//   0RFK6ZS8I9E/L4j7nnv2fVU/k+LGes6TQQI [IpmiDbg_v1][IpmiDbg_v1.1]
//   (3, sceIpmiMgr*)
//   w-zWDsN+0bQ/D1A-mAFwjT4 [Ipmi_v1][Ipmi_v1.1]
//   (2, IPMI::impl::ServerImpl ctor/dtor)
//   1TpZWrOOCw0/5ixbljAMYeg/lrH9rFkouoY/k6Ki9MjO9sI
//   [MbusDebug_v1][MbusDebug_v1.1] (4, sceMbusDebug* + 1 unknown)
//   E4wZaG1zSFc/cOYia2dE0Ik/oVkZ8W8-Q8A/pDBDcY6uLSA
//   [LibcInternal_v1][LibcInternal_v1.1] (4, dirname/asprintf/strtok/vfprintf)
//   JFpAREOJnag/W7Vpt62Xtf4/ilQLkGlAjWA
//   [libkernel_devtoollog_v1][libkernel_devtoollog_v1.1] (3 unknowns)
// Absence check: every NID above grepped over src/libs -> all 26 clean, 0 hits.
// Semantics: safe tracers so boot proceeds. Config/dump/debug getters report
// absent/empty (return 0 / NULL), init/start/stop/close calls succeed-empty
// (return 0, caller ignores output). libc odds: dirname returns NULL without
// touching the guest pointer (never calls host dirname on it), asprintf returns
// -1 without writing anything (allocation semantics unknown), strtok returns
// NULL (no tokens), vfprintf discards output (returns 0). PRINT_NAME + LOGF
// NID+name+args, uint64_t x4 args, null-safe, no guest writes at all, never
// halt. C++ NIDs starting with '+' or containing '-'/'+" stay verbatim inside
// LIB_FUNC("...") string literals; C++ function names use the sce names, and
// only NID-derived identifiers sanitize (+ -> P, - -> M).

namespace Libs {

namespace SysNetNet {

LIB_VERSION("Net", 1, "Net", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetDbgInit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 26VqvOFbxYo sceNetDbgInit a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetFreeAllRouteInfo(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 2ee14ktE1lw sceNetFreeAllRouteInfo a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing held; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetAllocateAllRouteInfo(uint64_t a0, uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
	PRINT_NAME();
	LOGF("\t PcdLABhYga4 sceNetAllocateAllRouteInfo a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no table; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetAllocateAllRouteInfo6(uint64_t a0, uint64_t a1, uint64_t a2,
                                                      uint64_t a3) {
	PRINT_NAME();
	LOGF("\t nO17GBCP-es sceNetAllocateAllRouteInfo6 a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no table; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetConfigEtherSetLinkMode(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t QltDK6wWqF0 sceNetConfigEtherSetLinkMode a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, no NIC to program; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetConfigSetDefaultRouteDev(uint64_t a0, uint64_t a1, uint64_t a2,
                                                         uint64_t a3) {
	PRINT_NAME();
	LOGF("\t cKvcNpw4q7o sceNetConfigSetDefaultRouteDev a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, no route table; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetConfigAddRouteWithInterface(uint64_t a0, uint64_t a1, uint64_t a2,
                                                            uint64_t a3) {
	PRINT_NAME();
	LOGF("\t wIGold7Lro0 sceNetConfigAddRouteWithInterface a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // accept, no route table; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetDuplicateIpStart(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t TwjkDIPdZ1Q sceNetDuplicateIpStart a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing to probe; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetInfoDumpStop(uint64_t a0, uint64_t a1, uint64_t a2,
                                             uint64_t a3) {
	PRINT_NAME();
	LOGF("\t XfV-XBCuhDo sceNetInfoDumpStop a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing dumping; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceNetInfoDumpStart(uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
	PRINT_NAME();
	LOGF("\t cYW1ISGlOmo sceNetInfoDumpStart a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing to dump; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysNetNet_1) {
	LIB_FUNC("26VqvOFbxYo", Impl::SceNetDbgInit);                 // sceNetDbgInit
	LIB_FUNC("2ee14ktE1lw", Impl::SceNetFreeAllRouteInfo);        // sceNetFreeAllRouteInfo
	LIB_FUNC("PcdLABhYga4", Impl::SceNetAllocateAllRouteInfo);    // sceNetAllocateAllRouteInfo
	LIB_FUNC("nO17GBCP-es", Impl::SceNetAllocateAllRouteInfo6);   // sceNetAllocateAllRouteInfo6
	LIB_FUNC("QltDK6wWqF0", Impl::SceNetConfigEtherSetLinkMode);  // sceNetConfigEtherSetLinkMode
	LIB_FUNC("cKvcNpw4q7o", Impl::SceNetConfigSetDefaultRouteDev); // sceNetConfigSetDefaultRouteDev
	LIB_FUNC("wIGold7Lro0", Impl::SceNetConfigAddRouteWithInterface); // sceNetConfigAddRouteWithInterface
	LIB_FUNC("TwjkDIPdZ1Q", Impl::SceNetDuplicateIpStart);       // sceNetDuplicateIpStart
	LIB_FUNC("XfV-XBCuhDo", Impl::SceNetInfoDumpStop);            // sceNetInfoDumpStop
	LIB_FUNC("cYW1ISGlOmo", Impl::SceNetInfoDumpStart);           // sceNetInfoDumpStart
}

} // namespace SysNetNet

namespace SysNetIpmiDbg {

LIB_VERSION("IpmiDbg", 1, "IpmiDbg", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceIpmiMgrGetIpcBlockerPid(uint64_t a0, uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 0RFK6ZS8I9E sceIpmiMgrGetIpcBlockerPid a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no blocker; report-absent
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceIpmiMgrStartDump(uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
	PRINT_NAME();
	LOGF("\t L4j7nnv2fVU sceIpmiMgrStartDump a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing to dump; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceIpmiMgrPauseDump(uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3) {
	PRINT_NAME();
	LOGF("\t k+LGes6TQQI sceIpmiMgrPauseDump a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // nothing dumping; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysNetIpmiDbg_1) {
	LIB_FUNC("0RFK6ZS8I9E", Impl::SceIpmiMgrGetIpcBlockerPid); // sceIpmiMgrGetIpcBlockerPid
	LIB_FUNC("L4j7nnv2fVU", Impl::SceIpmiMgrStartDump);       // sceIpmiMgrStartDump
	LIB_FUNC("k+LGes6TQQI", Impl::SceIpmiMgrPauseDump);        // sceIpmiMgrPauseDump
}

} // namespace SysNetIpmiDbg

namespace SysNetIpmi {

LIB_VERSION("Ipmi", 1, "Ipmi", 1, 1);

namespace Impl {

// C++ identifiers sanitize raw NIDs (+ -> P, - -> M):
//   w-zWDsN+0bQ -> wMzWDsNP0bQ, D1A-mAFwjT4 -> D1AMmAFwjT4.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IpmiServerImplCtor_wMzWDsNP0bQ(uint64_t a0, uint64_t a1, uint64_t a2,
                                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t w-zWDsN+0bQ _ZN4IPMI4impl10ServerImplC1Ev (ctor) a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // ctor tracer; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IpmiServerImplDtor_D1AMmAFwjT4(uint64_t a0, uint64_t a1, uint64_t a2,
                                                        uint64_t a3) {
	PRINT_NAME();
	LOGF("\t D1A-mAFwjT4 _ZN4IPMI4impl10ServerImplD1Ev (dtor) a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // dtor tracer; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysNetIpmi_1) {
	LIB_FUNC("w-zWDsN+0bQ", Impl::IpmiServerImplCtor_wMzWDsNP0bQ); // _ZN4IPMI4impl10ServerImplC1Ev (ctor)
	LIB_FUNC("D1A-mAFwjT4", Impl::IpmiServerImplDtor_D1AMmAFwjT4); // _ZN4IPMI4impl10ServerImplD1Ev (dtor)
}

} // namespace SysNetIpmi

namespace SysNetMbusDebug {

LIB_VERSION("MbusDebug", 1, "MbusDebug", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusDebugGetInternalInfo(uint64_t a0, uint64_t a1, uint64_t a2,
                                                       uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 1TpZWrOOCw0 sceMbusDebugGetInternalInfo a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no info; report-absent
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusDebugGetPriorityInfo(uint64_t a0, uint64_t a1, uint64_t a2,
                                                      uint64_t a3) {
	PRINT_NAME();
	LOGF("\t 5ixbljAMYeg sceMbusDebugGetPriorityInfo a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no info; report-absent
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusDebugGetDeviceInfo(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3) {
	PRINT_NAME();
	LOGF("\t lrH9rFkouoY sceMbusDebugGetDeviceInfo a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // no info; report-absent
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_Unkk6Ki9MjO9sI(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t k6Ki9MjO9sI MbusDebugUnk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // tracer; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysNetMbusDebug_1) {
	LIB_FUNC("1TpZWrOOCw0", Impl::SceMbusDebugGetInternalInfo); // sceMbusDebugGetInternalInfo
	LIB_FUNC("5ixbljAMYeg", Impl::SceMbusDebugGetPriorityInfo);  // sceMbusDebugGetPriorityInfo
	LIB_FUNC("lrH9rFkouoY", Impl::SceMbusDebugGetDeviceInfo);    // sceMbusDebugGetDeviceInfo
	LIB_FUNC("k6Ki9MjO9sI", Impl::LogStub_Unkk6Ki9MjO9sI); // name unresolved (no nids.csv row)
}

} // namespace SysNetMbusDebug

namespace SysNetLibcInternal {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

namespace Impl {

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Dirname(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t E4wZaG1zSFc dirname a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a1;
	(void)a2;
	(void)a3;
	if (a0 == 0) {
		return 0; // NULL in -> NULL out
	}
	// Never call host dirname on the guest pointer; no guest writes.
	(void)a0;
	return 0; // report-absent (NULL)
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Asprintf(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t cOYia2dE0Ik asprintf a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	// Allocation semantics unknown: write nothing, report failure.
	return -1;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Strtok(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t oVkZ8W8-Q8A strtok a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // NULL: no tokens
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Vfprintf(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	LOGF("\t pDBDcY6uLSA vfprintf a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // discard output; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysNetLibcInternal_1) {
	LIB_FUNC("E4wZaG1zSFc", Impl::Dirname);  // dirname
	LIB_FUNC("cOYia2dE0Ik", Impl::Asprintf); // asprintf
	LIB_FUNC("oVkZ8W8-Q8A", Impl::Strtok);   // strtok
	LIB_FUNC("pDBDcY6uLSA", Impl::Vfprintf); // vfprintf
}

} // namespace SysNetLibcInternal

namespace SysNetDevtoolLog {

LIB_VERSION("libkernel_devtoollog", 1, "libkernel_devtoollog", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkJFpAREOJnag(uint64_t a0, uint64_t a1, uint64_t a2,
                                                 uint64_t a3) {
	PRINT_NAME();
	LOGF("\t JFpAREOJnag DevtoolLogUnk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // tracer; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkW7Vpt62Xtf4(uint64_t a0, uint64_t a1, uint64_t a2,
                                                 uint64_t a3) {
	PRINT_NAME();
	LOGF("\t W7Vpt62Xtf4 DevtoolLogUnk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // tracer; succeed-empty
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkilQLkGlAjWA(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3) {
	PRINT_NAME();
	LOGF("\t ilQLkGlAjWA DevtoolLogUnk a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	return 0; // tracer; succeed-empty
}

} // namespace Impl

LIB_DEFINE(InitSysNetDevtoolLog_1) {
	LIB_FUNC("JFpAREOJnag", Impl::LogStub_UnkJFpAREOJnag); // name unresolved (no nids.csv row)
	LIB_FUNC("W7Vpt62Xtf4", Impl::LogStub_UnkW7Vpt62Xtf4); // name unresolved (no nids.csv row)
	LIB_FUNC("ilQLkGlAjWA", Impl::LogStub_UnkilQLkGlAjWA); // name unresolved (no nids.csv row)
}

} // namespace SysNetDevtoolLog

namespace SysNet {

LIB_DEFINE(InitSysNet_1) {
SysNetNet::InitSysNetNet_1(s);
SysNetIpmiDbg::InitSysNetIpmiDbg_1(s);
SysNetIpmi::InitSysNetIpmi_1(s);
SysNetMbusDebug::InitSysNetMbusDebug_1(s);
SysNetLibcInternal::InitSysNetLibcInternal_1(s);
SysNetDevtoolLog::InitSysNetDevtoolLog_1(s);
}

} // namespace SysNet

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp,
inside namespace Libs next to the other bare LIB_DEFINE lines):

LIB_DEFINE(InitSysNet_1);

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   InitSysNet_1(s);
*/
