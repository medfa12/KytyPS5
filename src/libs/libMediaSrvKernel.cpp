#include "common/abi.h"
#include "libs/libs.h"
#include "libs/processSdk.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>

// ps5-vsh: NPXS40039 MediaCoreServer service-loop kernel-side hits (34 PLT
// imports patched-to-stub, never called yet). Each LIB_VERSION below uses the
// guest's EXACT requested (library, lib_version, module, mod_major, mod_minor)
// key, so Resolve() exact-Finds these records instead of falling back to
// older LogStub entries. NID names from _ProbeLogs/nids.csv. All bodies are
// log-and-succeed (PRINT_NAME + LOGF NID+name+args, return 0), uint64_t x4
// args, null-safe with NO guest dereference at all. Absence-checked: none of
// these 9 NID strings is registered/implemented anywhere in src/libs or
// src/kernel.

namespace Libs {
namespace MediaSrvMbus {

LIB_VERSION("Mbus", 1, "Mbus", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusChangeAppModuleState(uint64_t a0, uint64_t a1, uint64_t a2,
                                                      uint64_t a3) {
	PRINT_NAME();
	// 7JNcL38ez2s sceMbusChangeAppModuleState. Setter ack, no state.
	LOGF("\t 7JNcL38ez2s sceMbusChangeAppModuleState: a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusTerm(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	// hykD8qmO6XA sceMbusTerm. Terminate ack.
	LOGF("\t hykD8qmO6XA sceMbusTerm: a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusGetSimulatedBusUsableStatusByBusType2(uint64_t a0, uint64_t a1,
                                                                      uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	// i9Q4sgvz7ck sceMbusGetSimulatedBusUsableStatusByBusType2. Query ack.
	LOGF("\t i9Q4sgvz7ck sceMbusGetSimulatedBusUsableStatusByBusType2: a0 = 0x%016" PRIx64
	     " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusRemoveProxy(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	// LbHgD9w6uAE sceMbusRemoveProxy. Removal ack.
	LOGF("\t LbHgD9w6uAE sceMbusRemoveProxy: a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceMbusSetProxy(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	// mP31WHwSVjQ sceMbusSetProxy. Setter ack.
	LOGF("\t mP31WHwSVjQ sceMbusSetProxy: a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	return 0;
}

LIB_DEFINE(InitMediaSrvKernelMbus_1) {
	LIB_FUNC("7JNcL38ez2s", MediaSrvMbus::SceMbusChangeAppModuleState); // sceMbusChangeAppModuleState
	LIB_FUNC("hykD8qmO6XA", MediaSrvMbus::SceMbusTerm);                 // sceMbusTerm
	LIB_FUNC("i9Q4sgvz7ck", MediaSrvMbus::SceMbusGetSimulatedBusUsableStatusByBusType2); // sceMbusGetSimulatedBusUsableStatusByBusType2
	LIB_FUNC("LbHgD9w6uAE", MediaSrvMbus::SceMbusRemoveProxy);          // sceMbusRemoveProxy
	LIB_FUNC("mP31WHwSVjQ", MediaSrvMbus::SceMbusSetProxy);             // sceMbusSetProxy
}

} // namespace MediaSrvMbus

namespace MediaSrvIpmi {

LIB_VERSION("Ipmi", 1, "Ipmi", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IpmiMsgQueueEstimateSize(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3) {
	PRINT_NAME();
	// yoVdeXZzPck IPMI::Client::Config::MsgQueueConfig::estimateMsgQueueSize.
	// Size query; 0 preserves the no-queue path. Log-only, no writes.
	LOGF("\t yoVdeXZzPck IPMI::Client::Config::MsgQueueConfig::estimateMsgQueueSize:"
	     " a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	return 0;
}

LIB_DEFINE(InitMediaSrvKernelIpmi_1) {
	LIB_FUNC("yoVdeXZzPck", MediaSrvIpmi::IpmiMsgQueueEstimateSize); // IPMI::Client::Config::MsgQueueConfig::estimateMsgQueueSize
}

} // namespace MediaSrvIpmi

namespace MediaSrvSysCore {

LIB_VERSION("SysCore", 1, "SysCore", 1, 1);

	static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceApplicationInitialize(uint64_t a0, uint64_t a1, uint64_t a2,
	                                                     uint64_t a3) {
		PRINT_NAME();
		// XFYItOxS6r0 sceApplicationInitialize. Init ack.
		LOGF("\t XFYItOxS6r0 sceApplicationInitialize: a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64
		     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
		     a0, a1, a2, a3);
		uint64_t ini_addr = Libs::LibKernel::g_psm_ini_struct_addr;
		if (ini_addr != 0) {
			uint64_t words[64] = {};
			std::memcpy(words, reinterpret_cast<const void*>(ini_addr), sizeof(words));
			LOGF("ps5-vsh: PsmIniFileData@AppInit:");
			for (int i = 0; i < 64; i += 4) {
				LOGF(" [%d] %llx %llx %llx %llx", i, (unsigned long long)words[i],
				     (unsigned long long)words[i + 1], (unsigned long long)words[i + 2],
				     (unsigned long long)words[i + 3]);
			}
			LOGF("\n");
			// ps5-vsh: sanitize host-path leak. PsmUtil stores our
			// GetRealFilename output (C:/Users/... fw-root + guest path)
			// in this struct; retail carries guest paths only. Rewrite
			// the host prefix to the guest equivalent in place.
			auto* bytes = reinterpret_cast<char*>(ini_addr);
			for (int off = 0; off + 4 < 512; off++) {
				if (bytes[off] == 'C' && bytes[off + 1] == ':' && bytes[off + 2] == '/') {
					int end = off;
					while (end < 512 && bytes[end] != '\0') {
						end++;
					}
					std::string host(bytes + off, static_cast<size_t>(end - off));
					LOGF("ps5-vsh: ini host path: %s\n", host.c_str());
					const std::string::size_type fw = host.find("13.60");
					if (fw != std::string::npos) {
						std::string guest = host.substr(fw + 5);
						for (auto& c : guest) {
							if (c == '\\') {
								c = '/';
							}
						}
						if (!guest.empty() && guest[0] != '/') {
							guest = "/" + guest;
						}
						LOGF("ps5-vsh: ini guest path: %s\n", guest.c_str());
						std::memset(bytes + off, 0, static_cast<size_t>(end - off));
						std::memcpy(bytes + off, guest.c_str(),
						            (std::min)(guest.size(), static_cast<size_t>(end - off - 1)));
					}
					break;
				}
			}
		}
		return 0;
	}

LIB_DEFINE(InitMediaSrvKernelSysCore_1) {
	LIB_FUNC("XFYItOxS6r0", MediaSrvSysCore::SceApplicationInitialize); // sceApplicationInitialize
}

} // namespace MediaSrvSysCore

namespace MediaSrvLibc {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Log10Stub(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	// WuMbPBKN1TU log10 (libc). NID-string absence-checked in src/libs and
	// src/kernel (no NID registration exists; plain-C log10 uses elsewhere
	// are unrelated). Log-and-succeed, no guest deref.
	LOGF("\t WuMbPBKN1TU log10: a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " -> 0\n",
	     a0, a1, a2, a3);
	return 0;
}

LIB_DEFINE(InitMediaSrvKernelLibc_1) {
	LIB_FUNC("WuMbPBKN1TU", MediaSrvLibc::Log10Stub); // log10
}

} // namespace MediaSrvLibc

namespace MediaSrvKernel {

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

LIB_DEFINE(InitMediaSrvKernelLibkernel_1) {
	LIB_FUNC("GGeRJk1XdWc", LibKernel::KernelGetProsperoCompiledSdkVersion);
}

LIB_DEFINE(InitMediaSrvKernel_1) {
	MediaSrvMbus::InitMediaSrvKernelMbus_1(s);
	MediaSrvIpmi::InitMediaSrvKernelIpmi_1(s);
	MediaSrvSysCore::InitMediaSrvKernelSysCore_1(s);
	MediaSrvLibc::InitMediaSrvKernelLibc_1(s);
	MediaSrvKernel::InitMediaSrvKernelLibkernel_1(s);
}

} // namespace MediaSrvKernel

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp):

namespace MediaSrvKernel {
LIB_DEFINE(InitMediaSrvKernel_1);
} // namespace MediaSrvKernel

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   MediaSrvKernel::InitMediaSrvKernel_1(s);
*/
