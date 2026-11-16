#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

// ps5-vsh: NPXS40039 MediaCoreServer service loop (npxs40039_boot1.log
// `unresolved PLT import patched to stub` lines). Media-side close-out:
// 3x ResourceArbitrator + 1x AudioOut + 1x SystemServiceAppInfo, all
// log-and-succeed (PRINT_NAME + LOGF, no guest deref, return 0).
// NIDs (name from _ProbeLogs/nids.csv `nid,hex,name` rows; verified absent
// repo-wide via src/libs SimpleMatch scan before use):
//   CYjfezkWcaE,0x0988DF7B391671A1,sceResourceArbitratorChangeState
//   kJvjeet+f9I,0x909BE379EB7E7FD2,sceResourceArbitratorSetCallback
//   LqmYZGB-UEQ,0x2EA99864607F5044,sceResourceArbitratorClearCallback
//   c7mVozxJkPU,0x73B995A33C4990F5,sceAudioOutGetSimulatedBusUsableStatusByBusType
//   ZWNP+-dWr8E: NO row in nids.csv (boot log only gives the requested
//     [SystemServiceAppInfo_v1][SystemService_v1.1] key); stub named
//     SceSystemServiceAppInfoUnk below.

namespace Libs {

namespace MediaSrvMedia {

namespace ResourceArbitrator {

LIB_VERSION("ResourceArbitrator", 1, "ResourceArbitrator", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceResourceArbitratorChangeState(uint64_t a1, uint64_t a2,
                                                          uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t CYjfezkWcaE sceResourceArbitratorChangeState a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceResourceArbitratorSetCallback(uint64_t a1, uint64_t a2,
                                                          uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t kJvjeet+f9I sceResourceArbitratorSetCallback a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceResourceArbitratorClearCallback(uint64_t a1, uint64_t a2,
                                                            uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t LqmYZGB-UEQ sceResourceArbitratorClearCallback a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitMediaSrvMediaResourceArbitrator_1) {
	LIB_FUNC("CYjfezkWcaE", Impl::SceResourceArbitratorChangeState); // sceResourceArbitratorChangeState
	LIB_FUNC("kJvjeet+f9I", Impl::SceResourceArbitratorSetCallback); // sceResourceArbitratorSetCallback
	LIB_FUNC("LqmYZGB-UEQ", Impl::SceResourceArbitratorClearCallback); // sceResourceArbitratorClearCallback
}

} // namespace ResourceArbitrator

namespace AudioOut {

LIB_VERSION("AudioOut", 1, "AudioOut", 1, 1);

namespace Impl {

// Owner: src/libs/libAudio.cpp LibAudioOut (+ src/libs/audio.cpp
// Audio::AudioOut, real fns return OK; libs/errno.h: OK == 0) and
// src/libs/libC.cpp LibAudioOut (LogStub, returns 0). Conventions agree,
// so plain `return 0` mirrors both.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceAudioOutGetSimulatedBusUsableStatusByBusType(uint64_t a1,
                                                                         uint64_t a2,
                                                                         uint64_t a3,
                                                                         uint64_t a4) {
	PRINT_NAME();
	LOGF("\t c7mVozxJkPU sceAudioOutGetSimulatedBusUsableStatusByBusType a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitMediaSrvMediaAudioOut_1) {
	LIB_FUNC("c7mVozxJkPU", Impl::SceAudioOutGetSimulatedBusUsableStatusByBusType); // sceAudioOutGetSimulatedBusUsableStatusByBusType
}

} // namespace AudioOut

namespace SystemServiceAppInfo {

LIB_VERSION("SystemServiceAppInfo", 1, "SystemService", 1, 1);

namespace Impl {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceSystemServiceAppInfoUnk(uint64_t a1, uint64_t a2,
                                                    uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t ZWNP+-dWr8E SceSystemServiceAppInfoUnk a1 = 0x%016" PRIx64
	     " a2 = 0x%016" PRIx64 " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 "\n",
	     a1, a2, a3, a4);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

} // namespace Impl

LIB_DEFINE(InitMediaSrvMediaSystemServiceAppInfo_1) {
	LIB_FUNC("ZWNP+-dWr8E", Impl::SceSystemServiceAppInfoUnk); // name unresolved (no nids.csv row)
}

} // namespace SystemServiceAppInfo

LIB_DEFINE(InitMediaSrvMedia_1) {
	ResourceArbitrator::InitMediaSrvMediaResourceArbitrator_1(s);
	AudioOut::InitMediaSrvMediaAudioOut_1(s);
	SystemServiceAppInfo::InitMediaSrvMediaSystemServiceAppInfo_1(s);
}

} // namespace MediaSrvMedia

} // namespace Libs

/*
Registration snippet for the orchestrator (add to src/libs/libs.cpp):

namespace MediaSrvMedia {
LIB_DEFINE(InitMediaSrvMedia_1);
} // namespace MediaSrvMedia

// ... in Libs::InitAll(Loader::SymbolDatabase* s):
//   MediaSrvMedia::InitMediaSrvMedia_1(s);
*/
