// ps5-vsh: NPXS40000 JSCd (eboot.bin) net shims (jscd_boot1.log).
// Unresolved PLT imports resolve to NULL, so any called slot is fatal.
// Host-backed RNPS curl, legacy HttpCache placeholders and firmware-backed
// ShellUI AppMessaging/URI ingress,
// under the guest's EXACT request keys, verified from raw Relocate lines:
//   MDgMnL6At94[libcurl_v1][libcurl_v1.1][Func] -> ("libcurl",1,"libcurl",1,1)
//   cZp79Bc6ovs[HttpCache_v1][HttpCache_v1.0][Func] -> ("HttpCache",1,"HttpCache",1,0)
//   DWVv0MlE1sw[ShellCoreUtil_v1][SystemService_v1.1][Func] -> ("ShellCoreUtil",1,"SystemService",1,1)
//   (same module SystemService 1.1 for AppMessaging/LncUtil/SystemStateMgr/SystemServiceTelemetry2)
// NID lists extracted with py regex ` (\S+)\[(libcurl_v1|HttpCache_v1|SystemService_v1)\]`
// over 'unresolved PLT' lines (libcurl 26, HttpCache 22) plus module
// SystemService_v1.1 (23 across 5 libs). Names from _ProbeLogs/nids.csv
// (nid,hex,name,library). All 71 NIDs grep-verified absent in src/libs
// (zero skips). Curl lifecycle and callback ABI live in rnpsCurl.cpp. Other
// unimplemented exports retain their existing behavior below.

#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/hostMemory.h"
#include "kernel/eventFlag.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libLncUtil.h"
#include "libs/libShellFocus.h"
#include "libs/rnpsCurl.h"
#include "loader/runtimeChildFrames.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace Libs {
namespace JscdNet {

[[maybe_unused]] constexpr int HTTP_ERROR_TIMEOUT = -2143088536; /* 0x80431068 */

namespace Curl {

LIB_VERSION("libcurl", 1, "libcurl", 1, 1);

LIB_DEFINE(InitJscdNetCurl_1) {
	LIB_FUNC("+0geDEimyvE", RnpsCurl::PushheaderBynum);
	LIB_FUNC("5b-D80m2TKc", RnpsCurl::ShareCleanup);
	LIB_FUNC("7KQckhaDi18", RnpsCurl::EasyPerform);
	LIB_FUNC("7ShPrvjazws", RnpsCurl::MultiRemoveHandle);
	LIB_FUNC("9cpxUNV97mM", RnpsCurl::MultiPerform);
	LIB_FUNC("Dfdbc8trvZg", RnpsCurl::MultiStrerror);
	LIB_FUNC("DiBmbfiH8J4", RnpsCurl::EasySetopt);
	LIB_FUNC("H49qutQi9a0", RnpsCurl::MultiPoll);
	LIB_FUNC("HD+IPUl76Hs", RnpsCurl::EasyCleanup);
	LIB_FUNC("I38s92i-Xjk", RnpsCurl::GlobalInit);
	LIB_FUNC("KBTWh1VVa-8", RnpsCurl::ShareInit);
	LIB_FUNC("MDgMnL6At94", RnpsCurl::SlistAppend);
	LIB_FUNC("NGTkAHTkViY", RnpsCurl::EasyStrerror);
	LIB_FUNC("NUbq2aNGojo", RnpsCurl::EasyInit);
	LIB_FUNC("SyLtMxQlS6M", RnpsCurl::MultiFdset);
	LIB_FUNC("TdLam7xSzmY", RnpsCurl::MultiWakeup);
	LIB_FUNC("ZhOwNq3lcpk", RnpsCurl::EasyGetinfo);
	LIB_FUNC("b3SWcncY+A0", RnpsCurl::ShareSetopt);
	LIB_FUNC("ea8LaHOKFDA", RnpsCurl::MultiInfoRead);
	LIB_FUNC("hGv08dyfsNQ", RnpsCurl::MultiSetopt);
	LIB_FUNC("k76sC3mIVw4", RnpsCurl::SlistFreeAll);
	LIB_FUNC("lxail8CxrHk", RnpsCurl::MultiTimeout);
	LIB_FUNC("mQBD4RanvTc", RnpsCurl::MultiAddHandle);
	LIB_FUNC("qB5W5ljFCIk", RnpsCurl::PushheaderByname);
	LIB_FUNC("wXEc3+fNcO4", RnpsCurl::MultiInit);
	LIB_FUNC("xyRRwQMH5ug", RnpsCurl::MultiCleanup);
}

} // namespace Curl

namespace Cache {

LIB_VERSION("HttpCache", 1, "HttpCache", 1, 0);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheRevalidateWithMemoryPool(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [+8M7+xoeeaE] sceHttpCacheRevalidateWithMemoryPool a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheSetData(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [7UnZ7b3kkvM] sceHttpCacheSetData a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheCreateRequestWithTag(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [8ztpPAczQLs] sceHttpCacheCreateRequestWithTag a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_Unk912OawFQtAw(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [912OawFQtAw] unk a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheClearAll(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [BHrUtjVRivQ] sceHttpCacheClearAll a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheCompleteRequest(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [IXdwKopqJHo] sceHttpCacheCompleteRequest a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheClear(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [NjhuM7DQT08] sceHttpCacheClear a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheRetrieveWithMemoryPool(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [TPKIWtZkFU0] sceHttpCacheRetrieveWithMemoryPool a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheTerm(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [UU-WJhJQWxQ] sceHttpCacheTerm a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheRemoveTag(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [V03vkc5c50w] sceHttpCacheRemoveTag a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheClearAllUserTags(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [XHGzVlZEHn4] sceHttpCacheClearAllUserTags a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkZxMbPyMPSWc(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [ZxMbPyMPSWc] unk a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheDeleteRequest(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [c8pxB0Z4QNY] sceHttpCacheDeleteRequest a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheInit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [cZp79Bc6ovs] sceHttpCacheInit a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheEnableCacheInfo(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [dIQhFL4sKBo] sceHttpCacheEnableCacheInfo a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheRegisterCacheInfoUrl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [eYC8sG2Ikjc] sceHttpCacheRegisterCacheInfoUrl a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheClearCacheInfo(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [jAJt5YrL7GI] sceHttpCacheClearCacheInfo a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheSetResponseHeader(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [moPKPd0A5FA] sceHttpCacheSetResponseHeader a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheCreateRequest(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [ridDhAPwj4c] sceHttpCacheCreateRequest a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_UnkvyG7AkKGIAg(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [vyG7AkKGIAg] unk a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheReadData(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [xExAZRNEpEM] sceHttpCacheReadData a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheSetQuota(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [yg6hx58UNqc] sceHttpCacheSetQuota a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitJscdNetCache_1) {
	LIB_FUNC("+8M7+xoeeaE", Cache::HttpCacheRevalidateWithMemoryPool);
	LIB_FUNC("7UnZ7b3kkvM", Cache::HttpCacheSetData);
	LIB_FUNC("8ztpPAczQLs", Cache::HttpCacheCreateRequestWithTag);
	LIB_FUNC("912OawFQtAw", Cache::LogStub_Unk912OawFQtAw);
	LIB_FUNC("BHrUtjVRivQ", Cache::HttpCacheClearAll);
	LIB_FUNC("IXdwKopqJHo", Cache::HttpCacheCompleteRequest);
	LIB_FUNC("NjhuM7DQT08", Cache::HttpCacheClear);
	LIB_FUNC("TPKIWtZkFU0", Cache::HttpCacheRetrieveWithMemoryPool);
	LIB_FUNC("UU-WJhJQWxQ", Cache::HttpCacheTerm);
	LIB_FUNC("V03vkc5c50w", Cache::HttpCacheRemoveTag);
	LIB_FUNC("XHGzVlZEHn4", Cache::HttpCacheClearAllUserTags);
	LIB_FUNC("ZxMbPyMPSWc", Cache::LogStub_UnkZxMbPyMPSWc);
	LIB_FUNC("c8pxB0Z4QNY", Cache::HttpCacheDeleteRequest);
	LIB_FUNC("cZp79Bc6ovs", Cache::HttpCacheInit);
	LIB_FUNC("dIQhFL4sKBo", Cache::HttpCacheEnableCacheInfo);
	LIB_FUNC("eYC8sG2Ikjc", Cache::HttpCacheRegisterCacheInfoUrl);
	LIB_FUNC("jAJt5YrL7GI", Cache::HttpCacheClearCacheInfo);
	LIB_FUNC("moPKPd0A5FA", Cache::HttpCacheSetResponseHeader);
	LIB_FUNC("ridDhAPwj4c", Cache::HttpCacheCreateRequest);
	LIB_FUNC("vyG7AkKGIAg", Cache::LogStub_UnkvyG7AkKGIAg);
	LIB_FUNC("xExAZRNEpEM", Cache::HttpCacheReadData);
	LIB_FUNC("yg6hx58UNqc", Cache::HttpCacheSetQuota);
}

} // namespace Cache

namespace AppMsgSys {

LIB_VERSION("AppMessaging", 1, "SystemService", 1, 1);

// FW 13.60 libSceSystemService: +5c40/+5ce0 send, +5dc0 receive,
// +5fc0 event poll. The old 4.03-reference 80550006 is NOT this ABI.
constexpr int kNotInitialized = static_cast<int>(0x80c50002);
constexpr int kTooLarge = static_cast<int>(0x80c50003);
constexpr int kBadAddress = static_cast<int>(0x80c50004);
constexpr int kAppMessagingNoMessage = static_cast<int>(0x80c50005);
constexpr int kNoMemory = static_cast<int>(0x80c50006);
constexpr int kNoDestination = static_cast<int>(0x80c50007);

struct Envelope {
	int32_t sender;
	uint32_t type;
	uint8_t payload[0x2000];
	uint32_t payload_size;
	uint32_t reserved;
	uint64_t timestamp;
};
static_assert(sizeof(Envelope) == 0x2018 && offsetof(Envelope, payload_size) == 0x2008 &&
              offsetof(Envelope, timestamp) == 0x2010);

namespace {
// A host resource bound, not a claimed firmware queue-depth constant.
constexpr size_t kQueueCapacity = 64;
std::mutex g_messages_mutex;
std::array<Envelope, kQueueCapacity> g_messages {};
size_t g_message_count = 0;
int32_t g_shell_app_id = -1;

bool CopyIn(void* local, const void* guest, size_t size) {
	if (size == 0) { return true; }
	// Existing host range probe plus the filesystem's fault-contained OS copy.
	// That primitive probes both endpoints inside the OS; using a local
	// destination also contains a concurrent unmap of the source (no memcpy).
	return Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(guest), size) &&
	       LibKernel::FileSystem::CopyPathResultToGuest(local, guest, size);
}

bool CopyOut(void* guest, const void* local, size_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	// The shared copyout preflights Windows/macOS. Linux process_vm_writev can
	// otherwise write a prefix before discovering an inaccessible second page.
	const auto address = reinterpret_cast<uintptr_t>(guest);
	if (address == 0 || size > UINTPTR_MAX - address) { return false; }
	auto cursor = address;
	const auto end = address + size;
	FILE* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) { return false; }
	char line[1024];
	while (cursor < end && std::fgets(line, sizeof(line), maps) != nullptr) {
		uint64_t begin = 0, finish = 0;
		char permissions[5] {};
		if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &begin, &finish, permissions) != 3 ||
		    finish <= cursor) { continue; }
		if (begin > cursor || permissions[1] != 'w') { break; }
		cursor = finish < end ? finish : end;
	}
	std::fclose(maps);
	if (cursor != end) { return false; }
#endif
	return LibKernel::FileSystem::CopyPathResultToGuest(guest, local, size);
}
} // namespace

// Session teardown hook: call only after guest threads have quiesced.
void ResetShellUiMessaging() {
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_messages_mutex);
	g_message_count = 0;
	g_shell_app_id = -1;
}

bool RegisterCurrentShellUi() {
	LncUtil::Lnc::AppStatus status;
	if (LncUtil::Lnc::sceLncUtilGetAppStatus(&status) != 0 || status.app_type != 1) { return false; }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_messages_mutex);
	if (g_shell_app_id != -1 && g_shell_app_id != status.app_id) { return false; }
	if (g_shell_app_id == -1) { LOGF("AppMessaging: registered ShellUI app=%d\n", status.app_id); }
	g_shell_app_id = status.app_id;
	return true;
}

static int Send(int32_t destination, bool shell_ui, uint32_t type, const void* data,
                uint64_t size, uint32_t flags) {
	int32_t sender = -1;
	if (!LncUtil::Lnc::TryGetCurrentAppId(&sender)) { return kNotInitialized; }
	if (size > sizeof(Envelope::payload)) { return kTooLarge; }
	Envelope message {};
	if (!CopyIn(message.payload, data, static_cast<size_t>(size))) { return kBadAddress; }
	message.sender = sender;
	message.type = type;
	message.payload_size = static_cast<uint32_t>(size);
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_messages_mutex);
	// No success-only fallback for unimplemented per-application routing.
	if (g_shell_app_id == -1 || (!shell_ui && destination != g_shell_app_id)) { return kNoDestination; }
	// SceShellCore +323450: only flag bit 0 is tested. It removes pending
	// messages of the same type (regardless of sender); other bits are ignored.
	size_t replace = 0;
	if ((flags & 1) != 0) {
		for (size_t i = 0; i < g_message_count; ++i) { replace += g_messages[i].type == type; }
	}
	if (g_message_count - replace == kQueueCapacity) { return kNoMemory; }
	if (replace != 0) {
		size_t remaining = 0;
		for (size_t i = 0; i < g_message_count; ++i) {
			if (g_messages[i].type != type) { g_messages[remaining++] = g_messages[i]; }
		}
		g_message_count = remaining;
	}
	// Firmware +3234f7 uses CLOCK_MONOTONIC converted to microseconds.
	message.timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count());
	g_messages[g_message_count++] = message;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AppMessagingSendMsg(
    int32_t destination, uint32_t type, const void* data, uint64_t size, uint32_t flags) {
	return Send(destination, false, type, data, size, flags);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AppMessagingSendMsgToShellUI(
    uint32_t type, const void* data, uint64_t size, uint32_t flags) {
	return Send(-1, true, type, data, size, flags);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AppMessagingTryReceiveMsg(Envelope* out) {
	int32_t caller = -1;
	if (!LncUtil::Lnc::TryGetCurrentAppId(&caller)) { return kNotInitialized; }
	if (out == nullptr) { return kBadAddress; }
	bool needs_registration = false;
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_messages_mutex);
		needs_registration = g_shell_app_id == -1;
	}
	// Older boot paths do not call RegisterShellUI. Only LNC's immutable
	// ShellUI classification may lazily establish the receiving endpoint.
	if (needs_registration) { RegisterCurrentShellUi(); }
	Common::HostLockCriticalGuard critical;
	std::lock_guard lock(g_messages_mutex);
	if (caller != g_shell_app_id || g_message_count == 0) { return kAppMessagingNoMessage; }
	if (!CopyOut(out, &g_messages[0], sizeof(*out))) { return kBadAddress; }
	if (g_messages[0].type == 0) {
		LOGF("AppMessaging: delivered type=0 sender=%d receiver=%d bytes=%u\n",
		     g_messages[0].sender, caller, g_messages[0].payload_size);
	}
	for (size_t i = 1; i < g_message_count; ++i) { g_messages[i - 1] = g_messages[i]; }
	--g_message_count;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AppMessagingTryGetEventFlag(
    uint64_t mask, uint64_t* out, uint8_t clear) {
	int32_t caller = -1;
	if (!LncUtil::Lnc::TryGetCurrentAppId(&caller)) { return kNotInitialized; }
	if (out == nullptr) { return kBadAddress; }
	// FW +5950 opens SceAppMessaging%x. Reuse an existing kernel flag; never
	// invent bit 63 on message arrival: +323923 signals a semaphore, not this
	// event flag. Native ShellUI's existing TryReceiveMsg poll drains the FIFO.
	char name[32];
	std::snprintf(name, sizeof(name), "SceAppMessaging%x", static_cast<unsigned>(caller));
	LibKernel::EventFlag::KernelEventFlag flag = nullptr;
	uint64_t bits = 0;
	const int opened = LibKernel::EventFlag::KernelOpenEventFlag(&flag, name);
	if (opened == 0) {
		// The kernel API already performs fault-contained output validation.
		// Let it atomically observe/clear the flag, as FW +5ff3 does; a separate
		// ClearEventFlag after copyout could erase a newer arrival.
		const int result = LibKernel::EventFlag::KernelPollEventFlag(
		    flag, mask, (static_cast<uint32_t>(clear) << 5) | 2, out);
		LibKernel::EventFlag::KernelCloseEventFlag(flag);
		if (result != 0 && result != LibKernel::KERNEL_ERROR_EBUSY) {
			return result;
		}
		if (result == 0) {
			if (!CopyIn(&bits, out, sizeof(bits))) { return kBadAddress; }
			bits &= mask;
		}
	} else if (opened != LibKernel::KERNEL_ERROR_ESRCH) {
		return opened;
	}
	return CopyOut(out, &bits, sizeof(bits)) ? 0 : kBadAddress;
}

LIB_DEFINE(InitJscdNetAppMsg_1) {
	LIB_FUNC("+zuv20FsXrA", AppMsgSys::AppMessagingSendMsg);
	LIB_FUNC("5ygy1IPUh5c", AppMsgSys::AppMessagingSendMsgToShellUI);
	LIB_FUNC("ZVRXXqj1n80", AppMsgSys::AppMessagingTryReceiveMsg);
	LIB_FUNC("iKNXKsUtOjY", AppMsgSys::AppMessagingTryGetEventFlag);
}

} // namespace AppMsgSys

namespace ShellUtilSys {

LIB_VERSION("ShellCoreUtil", 1, "SystemService", 1, 1);

namespace {

// ps5-vsh ShellCoreUtil UI-status / user-focus state (guest-visible).
//
// Signatures/semantics verified from the 13.60 ShellUI managed wrapper
// (Sce.Vsh.ShellCoreUtilWrapper.dll, ilspy decompile):
//   int sceShellCoreUtilGetUIStatus(SceShellCoreUtilUIStatus* status)
//       decompiled.cs:1347 - out param is the [Flags] ulong enum (:274)
//   int sceShellCoreUtilSetUIStatus(SceShellCoreUtilUIStatus status,
//                                   SceShellCoreUtilUIStatus mask)  (:1357)
//   int sceShellCoreUtilGetUserFocus(int appId, int* userId)        (:1800)
//   int sceShellCoreUtilSetUserFocus(int userId)                    (:1810)
// Enum bits used here (:274-297): PS_BUTTON_LOCKED=0x8, VSH_READY=0x10
// (also INVALID=0x1, BIG_APP_VISIBLE=0x2, MINI_APP_VISIBLE=0x4,
// BIG_APP_FOCUS=0x20, QUICK_MENU_FOCUS=0x100, DIMMER=0x200,
// DISPATCH_SIGNAL=0x400, LIVE_AREA_FOCUS=0x2000).
// Set is a masked replace, new = (old & ~mask) | (status & mask): proven by
// the AOT body of Sce.Vsh.ShellUI.Base.UIStatusUpdater.Set in
// Sce.Vsh.ShellUI.Legacy.dll.sprx @0x43460 (and/andn/or, store, then the
// native SetUIStatus pair); OnLockTypeChanged @0x434E0 toggles only
// PS_BUTTON_LOCKED (status=8 or ~8, mask=8), and UIStatusUpdater.Initialize
// calls Set(..., mask=0xF0003F3FF, signal=true).
// svcprobe14 log: GetUIStatus polled 11370x from one caller with a single
// out address (a0=0x00000007edfe6118); Get/SetUserFocus and SetUIStatus only
// appear as relocations in _ShellUIManaged/native/eboot.bin (never called
// through a logging stub), so they are implemented here from the wrapper
// signatures above.
constexpr uint64_t kUiStatusPsButtonLocked = 0x8;
constexpr uint64_t kUiStatusVshReady       = 0x10;

// 0x80A10003 SCE_SYSTEM_SERVICE_ERROR_PARAMETER (libs/errno.h SystemService).
constexpr int kErrorParameter = -2136932349;

// Boot state: VSH is already rendering the opening (animated) background,
// and OpeningPsButtonLocker holds the PS button locked until a foreground
// session opens, i.e. VSH_READY|PS_BUTTON_LOCKED. No user has focus yet
// (-1 = none, the empty-slot sentinel used by the UserService id lists).
std::atomic<uint64_t> g_ui_status {kUiStatusVshReady | kUiStatusPsButtonLocked};
std::atomic<int32_t>  g_user_focus {-1};

// Masked-replace UI status update; logs only on an actual transition.
uint64_t ApplyUiStatus(uint64_t status, uint64_t mask, const char* why) {
	uint64_t old = g_ui_status.load(std::memory_order_relaxed);
	for (;;) {
		const uint64_t nw = (old & ~mask) | (status & mask);
		if (nw == old) {
			return old;
		}
		if (g_ui_status.compare_exchange_weak(old, nw, std::memory_order_relaxed)) {
			LOGF("\t shellcoreutil: UIStatus 0x%016" PRIx64 " -> 0x%016" PRIx64 " (%s)\n", old,
			     nw, why);
			return nw;
		}
	}
}

// User-focus store; logs only on an actual transition.
void StoreUserFocus(int32_t user_id, const char* why) {
	const int32_t old = g_user_focus.exchange(user_id, std::memory_order_relaxed);
	if (old != user_id) {
		LOGF("\t shellcoreutil: UserFocus %d -> %d (%s)\n", static_cast<int>(old),
		     static_cast<int>(user_id), why);
	}
}

} // namespace

// Host PS-press transition (body for
// Libs::JscdNet::ShellCoreUtilHost::OnHostPsButtonPressed, declared in
// libs/libShellFocus.h): focus the Config user and publish the unlocked
// UI status (VSH_READY set, PS_BUTTON_LOCKED cleared) - the end state the
// real OpeningPsButtonLocker/UIStatusUpdater path reaches after a press.
void HostApplyPsPress() {
	StoreUserFocus(Config::GetUserId(), "host PS press");
	ApplyUiStatus(kUiStatusVshReady, kUiStatusVshReady | kUiStatusPsButtonLocked,
	              "host PS press");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LogStub_Unk1m_E1Xh8COo(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [1m-E1Xh8COo] unk a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilRequestShutdown(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [7JgSJnaByIs] sceShellCoreUtilRequestShutdown a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilTickHeartBeat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [DWVv0MlE1sw] sceShellCoreUtilTickHeartBeat a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
// FW 13.60 +109d0/+109f0. There is no size/version member.
struct LaunchByUriParam {
	uint32_t option;
	int32_t user_id;
};
struct LaunchByUriPayload {
	int32_t app_id;
	int32_t user_id;
	uint32_t option;
	char uri[0x1800];
};
static_assert(sizeof(LaunchByUriParam) == 8 && alignof(LaunchByUriParam) == 4);
static_assert(sizeof(LaunchByUriPayload) == 0x180c && offsetof(LaunchByUriPayload, uri) == 12);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilLaunchByUri(
    const char* uri, const LaunchByUriParam* param) {
	constexpr int bad_parameter = static_cast<int>(0x80e30003);
	constexpr int too_long = static_cast<int>(0x80e30006);
	if (uri == nullptr || param == nullptr) { return bad_parameter; }
	LaunchByUriParam local_param;
	if (!AppMsgSys::CopyIn(&local_param, param, sizeof(local_param))) { return bad_parameter; }
	LaunchByUriPayload payload {};
	payload.app_id = -1; // Native ShellUI +96d0 replaces this with envelope.sender.
	payload.user_id = local_param.user_id;
	payload.option = local_param.option;
	const auto base = reinterpret_cast<uintptr_t>(uri);
	for (size_t offset = 0; offset < sizeof(payload.uri);) {
		if (offset > UINTPTR_MAX - base) { return bad_parameter; }
		const auto cursor = base + offset;
		size_t count = std::min<size_t>({64, sizeof(payload.uri) - offset, 4096 - (cursor & 4095)});
		if (!AppMsgSys::CopyIn(payload.uri + offset, reinterpret_cast<const void*>(cursor), count)) {
			count = 1; // A valid short NUL-terminated mapping need not cover the chunk.
			if (!AppMsgSys::CopyIn(payload.uri + offset, reinterpret_cast<const void*>(cursor), count)) {
				return bad_parameter;
			}
		}
		for (size_t i = 0; i < count; ++i) {
			if (payload.uri[offset + i] == '\0') {
				// strlcpy semantics: no bytes beyond the terminator escape the guest.
				std::memset(payload.uri + offset + i, 0, sizeof(payload.uri) - offset - i);
				const auto result = AppMsgSys::AppMessagingSendMsgToShellUI(0, &payload, sizeof(payload), 0);
				LOGF("ShellCoreUtilLaunchByUri: uri=%s user=%d option=0x%x delivery=0x%08x\n",
				     payload.uri, payload.user_id, payload.option, result);
				return result;
			}
		}
		offset += count;
	}
	return too_long;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilGetFreeSizeOfUserPartition(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [ZbY5LxmH6uA] sceShellCoreUtilGetFreeSizeOfUserPartition a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilPostPsmEventToShellUI(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [cZCJTMamDOE] sceShellCoreUtilPostPsmEventToShellUI a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilLaunchByUriParamInit(LaunchByUriParam* param) {
	if (param != nullptr) {
		const LaunchByUriParam initial {0, -1};
		AppMsgSys::CopyOut(param, &initial, sizeof(initial));
	}
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilGetPsStoreIconState(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [zS6ZPLgQJcA] sceShellCoreUtilGetPsStoreIconState a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilGetUIStatus(uint64_t* status) {
	PRINT_NAME();
	if (status == nullptr) {
		return kErrorParameter;
	}
	const uint64_t cur = g_ui_status.load(std::memory_order_relaxed);
	*status            = cur;
	// Hot poll (11370 calls in svcprobe14): bounded census, same pattern as
	// AppMessagingTryReceiveMsg above. State changes are logged by
	// ApplyUiStatus/StoreUserFocus at the transition, not here.
	static std::atomic<uint64_t> polls {0};
	const uint64_t               n = polls.fetch_add(1, std::memory_order_relaxed);
	if (n < 64 || (n % 4096) == 0) {
		LOGF("\t [fORZmlh1TQo] sceShellCoreUtilGetUIStatus #%llu -> status=0x%016" PRIx64 "\n",
		     (unsigned long long)n, cur);
	}
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilSetUIStatus(uint64_t status, uint64_t mask) {
	PRINT_NAME();
	const uint64_t cur = ApplyUiStatus(status, mask, "guest sceShellCoreUtilSetUIStatus");
	Loader::RuntimeChild::SetShellUiFrameStatus(status, mask);
	LOGF("\t [atiUTsTFJ3k] sceShellCoreUtilSetUIStatus status=0x%016" PRIx64 " mask=0x%016" PRIx64 " -> 0x%016" PRIx64 "\n",
	     status, mask, cur);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilGetUserFocus(int32_t app_id, int32_t* user_id) {
	PRINT_NAME();
	if (user_id == nullptr) {
		return kErrorParameter;
	}
	const int32_t cur = g_user_focus.load(std::memory_order_relaxed);
	*user_id          = cur;
	LOGF("\t [E4I7uCoWbkM] sceShellCoreUtilGetUserFocus appId=%d -> userId=%d\n",
	     static_cast<int>(app_id), static_cast<int>(cur));
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilSetUserFocus(int32_t user_id) {
	PRINT_NAME();
	LOGF("\t [-9djWj1NU4E] sceShellCoreUtilSetUserFocus userId=%d\n", static_cast<int>(user_id));
	StoreUserFocus(user_id, "guest sceShellCoreUtilSetUserFocus");
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilSetAppData(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [xKSgaSVX1io] sceShellCoreUtilSetAppData\n");
	return 0;
}
static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilGetFreeSizeOfAvContentsTmp(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [guf+xcMoCas] sceShellCoreUtilGetFreeSizeOfAvContentsTmp\n");
	return 100ULL * 1024 * 1024 * 1024;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilSetDeviceIndexBehavior(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [l96YlUEtMPk] sceShellCoreUtilSetDeviceIndexBehavior\n");
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellCoreUtilInit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [9plZCCRm9x4] sceShellCoreUtilInit\n");
	AppMsgSys::RegisterCurrentShellUi();
	return 0;
}

LIB_DEFINE(InitJscdNetShellUtil_1) {
	LIB_FUNC("1m-E1Xh8COo", ShellUtilSys::LogStub_Unk1m_E1Xh8COo);
	LIB_FUNC("7JgSJnaByIs", ShellUtilSys::ShellCoreUtilRequestShutdown);
	LIB_FUNC("DWVv0MlE1sw", ShellUtilSys::ShellCoreUtilTickHeartBeat);
	LIB_FUNC("GYUk4t27Myw", ShellUtilSys::ShellCoreUtilLaunchByUri);
	LIB_FUNC("ZbY5LxmH6uA", ShellUtilSys::ShellCoreUtilGetFreeSizeOfUserPartition);
	LIB_FUNC("cZCJTMamDOE", ShellUtilSys::ShellCoreUtilPostPsmEventToShellUI);
	LIB_FUNC("gYOugDgfYJw", ShellUtilSys::ShellCoreUtilLaunchByUriParamInit);
	LIB_FUNC("zS6ZPLgQJcA", ShellUtilSys::ShellCoreUtilGetPsStoreIconState);
	LIB_FUNC("fORZmlh1TQo", ShellUtilSys::ShellCoreUtilGetUIStatus);
	LIB_FUNC("atiUTsTFJ3k", ShellUtilSys::ShellCoreUtilSetUIStatus);
	LIB_FUNC("E4I7uCoWbkM", ShellUtilSys::ShellCoreUtilGetUserFocus);
	LIB_FUNC("-9djWj1NU4E", ShellUtilSys::ShellCoreUtilSetUserFocus);
	LIB_FUNC("xKSgaSVX1io", ShellUtilSys::ShellCoreUtilSetAppData);
	LIB_FUNC("guf+xcMoCas", ShellUtilSys::ShellCoreUtilGetFreeSizeOfAvContentsTmp);
	LIB_FUNC("l96YlUEtMPk", ShellUtilSys::ShellCoreUtilSetDeviceIndexBehavior);
	LIB_FUNC("9plZCCRm9x4", ShellUtilSys::ShellCoreUtilInit);
}

} // namespace ShellUtilSys

namespace ShellCoreUtilHost {

// Declared in libs/libShellFocus.h; called from
// Libs::ShellFocus::ShellFocusHost::PulsePsButton (window/host GUIDE path).
void OnHostPsButtonPressed() {
	ShellUtilSys::HostApplyPsPress();
}

} // namespace ShellCoreUtilHost

namespace LncSys {

LIB_VERSION("LncUtil", 1, "SystemService", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilUnregisterDaemon(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [BnMaW5wfnlQ] sceLncUtilUnregisterDaemon a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilGetAppId(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [MVF+elex8Sw] sceLncUtilGetAppId a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilRegisterDaemon(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [V25-9U+YauY] sceLncUtilRegisterDaemon a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilTerminate(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [cCod+B3EdhI] sceLncUtilTerminate a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilInitialize(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [f-Q8Nd33FBc] sceLncUtilInitialize a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilGetAppStatusList(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [pMK2RT0cyus] sceLncUtilGetAppStatusList a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilIsAppSuspended(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [teGoPWnEgd4] sceLncUtilIsAppSuspended a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilGetEventForDaemon(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [yUh0BIPbhVo] sceLncUtilGetEventForDaemon a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LncUtilRegisterShellUI() {
	return AppMsgSys::RegisterCurrentShellUi() ? 0 : static_cast<int>(0x80940001);
}

LIB_DEFINE(InitJscdNetLnc_1) {
	LIB_FUNC("BnMaW5wfnlQ", LncSys::LncUtilUnregisterDaemon);
	LIB_FUNC("MVF+elex8Sw", LncSys::LncUtilGetAppId);
	LIB_FUNC("V25-9U+YauY", LncSys::LncUtilRegisterDaemon);
	LIB_FUNC("cCod+B3EdhI", LncSys::LncUtilTerminate);
	LIB_FUNC("f-Q8Nd33FBc", LncSys::LncUtilInitialize);
	LIB_FUNC("pMK2RT0cyus", LncSys::LncUtilGetAppStatusList);
	LIB_FUNC("teGoPWnEgd4", LncSys::LncUtilIsAppSuspended);
	LIB_FUNC("yUh0BIPbhVo", LncSys::LncUtilGetEventForDaemon);
	LIB_FUNC("awS+eYVuXJA", LncSys::LncUtilRegisterShellUI);
}

} // namespace LncSys

namespace StateMgrSys {

LIB_VERSION("SystemStateMgr", 1, "SystemService", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemStateMgrWakeUp(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [6gtqLPVTdJY] sceSystemStateMgrWakeUp a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemStateMgrRequestToKeepMainOnStandby(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [9WRHk1wQT08] sceSystemStateMgrRequestToKeepMainOnStandby a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitJscdNetStateMgr_1) {
	LIB_FUNC("6gtqLPVTdJY", StateMgrSys::SystemStateMgrWakeUp);
	LIB_FUNC("9WRHk1wQT08", StateMgrSys::SystemStateMgrRequestToKeepMainOnStandby);
}

} // namespace StateMgrSys

namespace TelemetrySys {

LIB_VERSION("SystemServiceTelemetry2", 1, "SystemService", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemServiceTelemetrySetData2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	LOGF("\t [83leZmsmmT4] sceSystemServiceTelemetrySetData2 a0=0x%016" PRIx64 " a1=0x%016" PRIx64 " a2=0x%016" PRIx64 " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64 "\n", a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitJscdNetTelemetry_1) {
	LIB_FUNC("83leZmsmmT4", TelemetrySys::SystemServiceTelemetrySetData2);
}

} // namespace TelemetrySys

namespace CoredumpSeqSys {
LIB_VERSION("CoredumpSeqHandlerIf", 1, "SystemService", 1, 1);

// 13.60 SystemService: update 1f2d0 -> receive 1f5e0 -> IPMI vtable+38;
// Ipmi d70 -> 4950 -> syscall 252. Kernel 80d53270 supplies a zero timeout;
// its empty-channel path 805b126a returns errno23, propagated as 80020023.
// ReportSeq uses the same receive path with a 6c0-byte cache (1fc30).
// These HLE inboxes have no producer/transport hookup yet. Model only their
// empty state: a successful receive must never be invented from zeroed data.
// In particular, native managed EventMonitor.Update (ShellCoreUtilWrapper
// +3480/+2450) dispatches on update==0 and does not check the getter's result.
constexpr int kNoEvent = static_cast<int>(0x80020023u);
constexpr int kNotInitialized = static_cast<int>(0x81378012u);
constexpr int kInvalidArgument = static_cast<int>(0x81378000u);

template <size_t Size> struct EmptyEventCache {
	std::mutex mutex;
	bool initialized = false;
	int result = kNotInitialized;
	std::array<uint8_t, Size> data {};

	int Initialize() {
		std::lock_guard<std::mutex> lock(mutex);
		if (initialized) { return kNotInitialized; } // native duplicate-init result
		initialized = true;
		result = kNoEvent;
		return 0;
	}

	int Receive() {
		std::lock_guard<std::mutex> lock(mutex);
		result = initialized ? kNoEvent : kNotInitialized;
		return result;
	}

	int Get(void* out) {
		if (out == nullptr) { return kInvalidArgument; }
		std::lock_guard<std::mutex> lock(mutex);
		// Native getters copy the entire cache even when the receive failed,
		// then return its saved status (1f2f0/1f9b0). Never label this an event.
		std::memcpy(out, data.data(), data.size());
		return result;
	}
};

static EmptyEventCache<0x6ac> g_events;

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Initialize() {
	PRINT_NAME();
	return g_events.Initialize();
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ReceiveEvent() {
	return g_events.Receive();
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetEvent(void* out) {
	return g_events.Get(out);
}
LIB_DEFINE(InitJscdNetCoredumpSeq_1) {
	LIB_FUNC("Fn9dTNR6wMM", CoredumpSeqSys::Initialize);
	LIB_FUNC("gQ-7OfXu59I", CoredumpSeqSys::ReceiveEvent);
	LIB_FUNC("jLUxdy7mfas", CoredumpSeqSys::GetEvent);
}
} // namespace CoredumpSeqSys

namespace ReportSeqSys {
LIB_VERSION("ReportSeqHandlerIf", 1, "SystemService", 1, 1);

static CoredumpSeqSys::EmptyEventCache<0x6c0> g_events;

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Initialize() {
	PRINT_NAME();
	return g_events.Initialize();
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ReceiveEvent() {
	return g_events.Receive();
}
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetEvent(void* out) {
	return g_events.Get(out);
}
LIB_DEFINE(InitJscdNetReportSeq_1) {
	LIB_FUNC("7P8xYWW8YaY", ReportSeqSys::Initialize);
	LIB_FUNC("Hdtr0jFNOYI", ReportSeqSys::ReceiveEvent);
	LIB_FUNC("9QRrQtmkp10", ReportSeqSys::GetEvent);
}
} // namespace ReportSeqSys

namespace CsbMgrSys {
LIB_VERSION("CsbMgr", 1, "SystemService", 1, 1);
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CsbStub(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	(void)a0; (void)a1; (void)a2; (void)a3;
	return 0;
}
LIB_DEFINE(InitJscdNetCsbMgr_1) {
	LIB_FUNC("2NtkcpO+bsM", CsbMgrSys::CsbStub);
}
} // namespace CsbMgrSys

LIB_DEFINE(InitJscdNet_1) {
	Curl::InitJscdNetCurl_1(s);
	Cache::InitJscdNetCache_1(s);
	AppMsgSys::InitJscdNetAppMsg_1(s);
	ShellUtilSys::InitJscdNetShellUtil_1(s);
	LncSys::InitJscdNetLnc_1(s);
	StateMgrSys::InitJscdNetStateMgr_1(s);
	TelemetrySys::InitJscdNetTelemetry_1(s);
	CoredumpSeqSys::InitJscdNetCoredumpSeq_1(s);
	ReportSeqSys::InitJscdNetReportSeq_1(s);
	CsbMgrSys::InitJscdNetCsbMgr_1(s);
}

} // namespace JscdNet
} // namespace Libs

// Orchestrator registration snippet for src/libs/libs.cpp (add exactly this):
//
//   namespace JscdNet {
//   LIB_DEFINE(InitJscdNet_1);
//   } // namespace JscdNet
//
// ... and inside Libs::InitAll(Loader::SymbolDatabase* s):
//
//   JscdNet::InitJscdNet_1(s);
