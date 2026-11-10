#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "libs/errno.h"
#include "libs/guestHeap.h"
#include "libs/libs.h"
#include "libs/npManagerMemory.h"
#include "libs/npManagerIpcMemory.h"
#include "libs/npManagerClient.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef GetUserName
#endif

namespace Libs {

// ps5-vsh ShellCore boot: offline login fake (NpCommon data-driven).
// Implements the ShellCore-imported libSceNpManager account/signin/user surface
// (133 NIDs: imports-file count 140 minus 7 already registered elsewhere —
// 3Zl8BePTh9Y, 4uhgVNAqiag, fHGhS3uP52k, rbknaUjpqWo, ukEeOizCkIU, XDncXQIJUSk,
// yODgcWKSa8Q — skipped to avoid duplicate registration) plus 60 libSceNpCommon
// NIDs implemented for real: the JSON/allocator/platform base plus the 40
// hottest ShellCore NpCommon imports from _ProbeLogs/shellcore_boot8.log
// (ordered by import-site count; the boot makes zero runtime NpCommon calls,
// so slot count is the hotness proxy — see NpCommon below). Everything is
// null-safe; guest memory is identity-mapped so validated non-null guest
// pointers are touched directly. No network traffic is ever attempted: online
// token/request paths return an offline-branch error ONLY where the guest is
// expected to branch on it, otherwise empty-success so the menu proceeds
// offline. Real implementations never log the `npcommon-deferred` marker, so
// a future census sees only the true remainder (unresolved-import stub calls).

namespace OfflineLogin {

namespace NpManager {

LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

namespace Impl {

// Offline-branch error (guest falls back to its offline path; never touches
// network). Aliases the signed-out code used by the existing NpManager HLE.
constexpr int kNpOfflineError = -2141913082; // 0x80550006 (SCE_NP_ERROR_SIGNED_OUT)
constexpr int kNpInvalidArg   = -2141913085; // 0x80550003
constexpr uint64_t kFakeAccountId    = 0x00000000feedfaceULL; // valid nonzero offline account id

static int32_t FakeUserId() {
	return Config::GetUserId(); // --user-id, default 1000
}

static std::atomic<uint32_t> g_nextHandle{1}; // 0 = failure to the guest

static uint32_t AllocHandle(const char* what) {
	uint32_t h = g_nextHandle.fetch_add(1);
	if (h == 0) {
		h = g_nextHandle.fetch_add(1); // never hand out 0
	}
	LOGF("\t %s: handle = %u\n", what, h);
	return h;
}

// Probe guest memory pointer safely before any write.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static bool ProbeGuest(uint64_t addr, size_t len, bool need_write) {
	if (addr < 0x10000 || len == 0) {
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
		const bool  read = (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
		                    prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
		                    prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
		const bool  write = (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
		                     prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY);
		if (!read || (need_write && !write)) {
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
static bool ProbeGuest(uint64_t addr, size_t len, bool) {
	return addr >= 0x10000 && len != 0;
}
#endif

// Resolves whether output pointer is in a1 (user_id passed in a0) or in a0 (direct pointer).
static uint64_t ResolveOutPtr(uint64_t a0, uint64_t a1, size_t sz) {
	if (a1 >= 0x10000 && ProbeGuest(a1, sz, true)) return a1;
	if (a0 >= 0x10000 && ProbeGuest(a0, sz, true)) return a0;
	return 0;
}

// Guest out-param write. Validates pointer is writable guest memory, never faults.
template <typename T>
static int WriteGuest(uint64_t guest_addr, T value) {
	if (guest_addr < 0x10000 || !ProbeGuest(guest_addr, sizeof(T), true)) {
		LOGF("\t invalid guest out-param: 0x%016" PRIx64 "\n", guest_addr);
		return kNpInvalidArg;
	}
	*reinterpret_cast<T*>(static_cast<uintptr_t>(guest_addr)) = value;
	return OK;
}

// ---- account / signin / user surface (fake offline user) ----

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetUserIdByAccountId(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_user = ResolveOutPtr(a0, a1, sizeof(int32_t));
	return WriteGuest<int32_t>(out_user, FakeUserId());
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetAccountCountry(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_country = ResolveOutPtr(a0, a1, 4u);
	if (out_country == 0) {
		LOGF("\t null/invalid guest out-param\n");
		return kNpInvalidArg;
	}
	// NpCountryCode { char data[2]; char term; char pad; }: "US".
	auto* p = reinterpret_cast<char*>(static_cast<uintptr_t>(out_country));
	p[0] = 'U';
	p[1] = 'S';
	p[2] = '\0';
	p[3] = '\0';
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetCachedAccountInfo(uint64_t a1, uint64_t a2, uint64_t a3,
                                                    uint64_t a4) {
	PRINT_NAME();
	LOGF("\t (unknown out-layout, success-noop)\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetAccountNpEnv(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_env = ResolveOutPtr(a0, a1, sizeof(uint32_t));
	return WriteGuest<uint32_t>(out_env, 0);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetAccountId(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_account = ResolveOutPtr(a0, a1, sizeof(uint64_t));
	const int rc = WriteGuest<uint64_t>(out_account, kFakeAccountId);
	LOGF("\t account_id = 0x%016" PRIx64 " (out=0x%016" PRIx64 ")\n", kFakeAccountId, out_account);
	return rc;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetUserIdByOnlineId(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_user = ResolveOutPtr(a0, a1, sizeof(int32_t));
	return WriteGuest<int32_t>(out_user, FakeUserId());
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetUserIdByNpId(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_user = ResolveOutPtr(a0, a1, sizeof(int32_t));
	return WriteGuest<int32_t>(out_user, FakeUserId());
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetNpIdByOnlineId(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_np_id = ResolveOutPtr(a0, a1, 36u);
	if (out_np_id == 0) {
		LOGF("\t null/invalid guest out-param\n");
		return kNpInvalidArg;
	}
	auto* p = reinterpret_cast<char*>(static_cast<uintptr_t>(out_np_id));
	std::memset(p, 0, 36);
	std::snprintf(p, 17, "%s", Config::GetUserName().c_str());
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetAccountCountryA(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	return MgrIntGetAccountCountry(a0, a1);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntIsSubAccount(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_is_sub = ResolveOutPtr(a0, a1, sizeof(uint32_t));
	return WriteGuest<uint32_t>(out_is_sub, 0); // master account
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetUserList(uint64_t max_users, uint64_t out_list, uint64_t a3,
                                          uint64_t a4) {
	PRINT_NAME();
	// Real shape per eboot caller (max_users, user_info_array, ...): arg1 is
	// a count, not a pointer (a prior revision dereferenced max=3 and died).
	// Single offline user: publish the leading user id, report count 1.
	(void)a3;
	(void)a4;
	if (out_list == 0) {
		LOGF("\t null guest out-param\n");
		return kNpInvalidArg;
	}
	LOGF("\t max_users = %llu\n", static_cast<unsigned long long>(max_users));
	if (max_users == 0) {
		return 0; // nowhere to publish; count 0
	}
	if (WriteGuest<int32_t>(out_list, FakeUserId()) != OK) {
		return kNpInvalidArg;
	}
	return 1;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetAccountType(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_type = ResolveOutPtr(a0, a1, sizeof(uint32_t));
	return WriteGuest<uint32_t>(out_type, 0);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetNpEnv(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_env = ResolveOutPtr(a0, a1, sizeof(uint32_t));
	return WriteGuest<uint32_t>(out_env, 0);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetAuthServerErrorFlag(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	uint64_t out_flag = ResolveOutPtr(a0, a1, sizeof(uint32_t));
	return WriteGuest<uint32_t>(out_flag, 0); // no auth server contact, no error latched
}

// Private request lifecycle is implemented by the retained owner's NP client.

// ---- online token/info getters: offline-branch error (guest goes offline) ----

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetClientCredentialAccessToken(uint64_t a1, uint64_t a2,
                                                              uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t offline: no network, guest branches (0x%08x)\n", kNpOfflineError);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return kNpOfflineError;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetVshAccessToken(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	LOGF("\t offline: no network, guest branches (0x%08x)\n", kNpOfflineError);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return kNpOfflineError;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MgrIntGetTicket(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t offline: no network, guest branches (0x%08x)\n", kNpOfflineError);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return kNpOfflineError;
}

// ---- AsmClient family: local resource handles succeed, server data is offline ----

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmCreateConnection(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<int>(AllocHandle("asm-connection"));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmDeleteConnection(uint64_t conn) {
	PRINT_NAME();
	LOGF("\t conn = 0x%016" PRIx64 "\n", conn);
	(void)conn;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmCreateRequest(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<int>(AllocHandle("asm-request"));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmDeleteRequest(uint64_t req) {
	PRINT_NAME();
	LOGF("\t req = 0x%016" PRIx64 "\n", req);
	(void)req;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmAbort(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientCreateResourceContext(uint64_t a1, uint64_t a2, uint64_t a3,
                                                        uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<int>(AllocHandle("asm-resource-ctx"));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientDeleteResourceContext(uint64_t ctx) {
	PRINT_NAME();
	LOGF("\t ctx = 0x%016" PRIx64 "\n", ctx);
	(void)ctx;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientCreateRequest2(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return static_cast<int>(AllocHandle("asm-client-request"));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientDeleteRequest(uint64_t req) {
	PRINT_NAME();
	LOGF("\t req = 0x%016" PRIx64 "\n", req);
	(void)req;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientAbortRequest(uint64_t req) {
	PRINT_NAME();
	LOGF("\t req = 0x%016" PRIx64 "\n", req);
	(void)req;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientOnlineError(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t offline: no network, guest branches (0x%08x)\n", kNpOfflineError);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return kNpOfflineError;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmGenerateNpTitleToken(uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4) {
	PRINT_NAME();
	LOGF("\t offline: no network, guest branches (0x%08x)\n", kNpOfflineError);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return kNpOfflineError;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientExtInitialize(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

// ---- sce::np::User C++ helpers (unknown object layout: never touch this) ----

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserCtor(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserDtor(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserClear(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserGetUser(uint64_t user_id, uint64_t out_user, uint64_t a3,
                                     uint64_t a4) {
	PRINT_NAME();
	LOGF("\t user_id = %d\n", static_cast<int>(user_id));
	(void)a3;
	(void)a4;
	// Object layout unknown; publish the fake uid at offset 0 (id-first
	// convention) so id readers see the offline user.
	if (out_user == 0) {
		LOGF("\t null guest out-param\n");
		return kNpInvalidArg;
	}
	*reinterpret_cast<int32_t*>(static_cast<uintptr_t>(out_user)) = FakeUserId();
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserEqInt(uint64_t self, uint64_t user_id, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a3;
	(void)a4;
	return (static_cast<int32_t>(user_id) == FakeUserId()) ? 1 : 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserEqUser(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 1; // single offline user: all User objects compare equal
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserNeUser(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserIsLoggedIn(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a2;
	(void)a3;
	(void)a4;
	return 1; // offline fake user is logged in locally
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserIsEmpty(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserIsGuest(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserGetUserId(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a2;
	(void)a3;
	(void)a4;
	return FakeUserId();
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserGetAccountId(uint64_t self, uint64_t out_account) {
	PRINT_NAME();
	uint64_t out_ptr = ResolveOutPtr(self, out_account, sizeof(uint64_t));
	return WriteGuest<uint64_t>(out_ptr, kFakeAccountId);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserHasAccountId(uint64_t self, uint64_t out_has) {
	PRINT_NAME();
	uint64_t out_ptr = ResolveOutPtr(self, out_has, 1u);
	if (out_ptr != 0) {
		*reinterpret_cast<char*>(static_cast<uintptr_t>(out_ptr)) = 1;
	}
	return 1;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UserGetAccountIdFromRegistory(uint64_t self, uint64_t out_account) {
	PRINT_NAME();
	uint64_t out_ptr = ResolveOutPtr(self, out_account, sizeof(uint64_t));
	return WriteGuest<uint64_t>(out_ptr, kFakeAccountId);
}

// ---- sce::np::NpOnlineId / NpId / NpTicket / tokens / http (local noops) ----

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpObjectNoop(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpOnlineIdSetStr(uint64_t str, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t str = 0x%016" PRIx64 " (accepted, offline)\n", str);
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpOnlineIdGet(uint64_t user, uint64_t out_online_id) {
	PRINT_NAME();
	(void)user;
	if (out_online_id == 0) {
		LOGF("\t null guest out-param\n");
		return kNpInvalidArg;
	}
	// SceNpOnlineId { char[16], term, dummy[3] }: fake user name, zeroed tail.
	auto* p = reinterpret_cast<char*>(static_cast<uintptr_t>(out_online_id));
	std::memset(p, 0, 20);
	std::snprintf(p, 17, "%s", Config::GetUserName().c_str());
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpOnlineIdEq(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 1; // single offline identity
}

static const char kEmptyTicket[16] = {};

static const void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpTicketGetData(uint64_t self) {
	PRINT_NAME();
	(void)self;
	LOGF("\t npcommon-style empty ticket (%llu bytes)\n",
	     static_cast<unsigned long long>(sizeof(kEmptyTicket)));
	return kEmptyTicket;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpTicketGetSize(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)self;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static const char* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpTitleTokenGetStr(uint64_t self) {
	PRINT_NAME();
	(void)self;
	return "";
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IpcServiceGetIpmiClient(uint64_t self) {
	PRINT_NAME();
	(void)self;
	return LibcInternal::MakeIpmiClient();
}

// ---- guest-heap memory impls (host malloc; free is a leak-by-design noop so
//      guest-owned pointers can never crash us — same rule as NpObjectNew) ----

// ---- misc Vsh / push / unknown-? NIDs: success-noop ----

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VshNoop(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

} // namespace Impl

LIB_DEFINE(InitOfflineLoginNpManager_1) {
	// Account / signin / user. Typed private state/offline-ID queries use the NP
	// client; adjacent legacy account compatibility exports still need conversion.
	LIB_FUNC("OZTedKNUeFU", Impl::MgrIntGetUserIdByAccountId);
	LIB_FUNC("7+uKCMe4SLk", Impl::MgrIntGetAccountCountry);
	LIB_FUNC("70Swvw7h6ck", Libs::NpManagerClient::GetOfflineAccountIdForUser);
	LIB_FUNC("mZV+Pm4hvbI", Impl::MgrIntGetCachedAccountInfo);
	LIB_FUNC("HvNrMhlWBSk", Impl::MgrIntGetAccountNpEnv);
	LIB_FUNC("6miba-pcQt8", Libs::NpManagerClient::GetSigninStateForUser);
	LIB_FUNC("UAA2-ZTmgJc", Libs::NpManagerClient::GetActiveSigninStateForUser);
	LIB_FUNC("XS-eY7KRqjQ", Impl::MgrIntGetAccountId);
	LIB_FUNC("uSLgWz8ohak", Impl::MgrIntGetUserIdByOnlineId);
	LIB_FUNC("gFB0RmKjyaI", Impl::MgrIntGetNpIdByOnlineId);
	LIB_FUNC("MDczH3SxE9Q", Impl::MgrIntGetUserIdByNpId);
	LIB_FUNC("fjJ4xXM+3Tw", Impl::MgrIntGetAccountCountryA);
	LIB_FUNC("ossvuXednsc", Impl::MgrIntIsSubAccount);
	LIB_FUNC("PL10NiZ0XNA", Impl::MgrIntGetUserList);
	LIB_FUNC("9lz4fkS+eEk", Impl::MgrIntGetAccountType);
	LIB_FUNC("BTRVfOx7K1c", Impl::MgrIntGetNpEnv);
	LIB_FUNC("HkUgFhrpAD4", Impl::MgrIntGetAuthServerErrorFlag);
	// Request lifecycle.
	LIB_FUNC("xZk+QcivrFE", Libs::NpManagerClient::CreatePrivateRequest);
	LIB_FUNC("HneC+SpeLwc", Libs::NpManagerClient::DeletePrivateRequest);
	LIB_FUNC("AUuzKQIwhXY", Libs::NpManagerClient::AbortPrivateRequest);
	// Online tokens: offline-branch errors (no network).
	LIB_FUNC("TXzpCgPmXEQ", Impl::MgrIntGetClientCredentialAccessToken);
	LIB_FUNC("mBTFixSxTzQ", Impl::MgrIntGetVshAccessToken);
	LIB_FUNC("uVAfWmv+cc8", Impl::MgrIntGetTicket);
	// AsmClient family.
	LIB_FUNC("GPRRxFM01r4", Impl::AsmClientCreateResourceContext);
	LIB_FUNC("tOJ-WGFDt-Y", Impl::AsmClientCreateRequest2);
	LIB_FUNC("3rlqy6XxrmI", Impl::AsmClientOnlineError); // GetServiceIdInfoA
	LIB_FUNC("1wMn3X94WME", Impl::AsmClientDeleteRequest);
	LIB_FUNC("KA2AITpVTCg", Impl::AsmClientDeleteResourceContext);
	LIB_FUNC("0cn2c-bk8wA", Impl::AsmClientAbortRequest);
	LIB_FUNC("vf+lYeOXdI8", Impl::AsmClientOnlineError); // GetGameNpTitleInfo
	LIB_FUNC("TtHBV0mH8kY", Impl::AsmClientOnlineError); // GetRelatedGameNpTitleIdsResult
	LIB_FUNC("YQ7-z4zFWok", Impl::AsmClientOnlineError); // GetGameNpTitleToken
	LIB_FUNC("cOLn5A3ZoqU", Impl::AsmClientOnlineError); // GetNpComInfo2
	LIB_FUNC("O42ZlBvIPMM", Impl::AsmClientOnlineError); // GetServiceBaseUrl
	LIB_FUNC("nuPl4uVXYMM", Impl::AsmClientOnlineError); // GetNpTitleId
	LIB_FUNC("YDDHD6RP4HQ", Impl::AsmCreateConnection);
	LIB_FUNC("UxOJvGmy3mA", Impl::AsmDeleteConnection);
	LIB_FUNC("hIFFMeoLhcY", Impl::AsmCreateRequest);
	LIB_FUNC("RfokKHMuOsE", Impl::AsmDeleteRequest);
	LIB_FUNC("ulPuWk7bYCU", Impl::AsmGenerateNpTitleToken);
	LIB_FUNC("JrXA7baBMPQ", Impl::AsmAbort);
	LIB_FUNC("kKCa1japBxI", Impl::AsmClientExtInitialize);
	// sce::np::User.
	LIB_FUNC("S7Afe0llsL8", Impl::UserCtor);
	LIB_FUNC("bwwspVgS4hQ", Impl::UserGetUser);
	LIB_FUNC("gQFyT9aIsOk", Impl::UserDtor);
	LIB_FUNC("GtMgx4YcBuo", Impl::UserClear);
	LIB_FUNC("rgtbpTzx0RA", Impl::UserCtor); // copy ctor
	LIB_FUNC("Z4wnPrd9jIE", Impl::UserCtor); // User(int)
	LIB_FUNC("0qjYM9bp5vs", Impl::UserEqInt);
	LIB_FUNC("-BgzebSMaVY", Impl::UserEqUser);
	LIB_FUNC("ta8lASAxro4", Impl::UserNeUser);
	LIB_FUNC("noJm52uLN00", Impl::UserIsLoggedIn);
	LIB_FUNC("f2K8i7KU20k", Impl::UserGetAccountId);
	LIB_FUNC("2hiV0v27kcY", Impl::UserHasAccountId);
	LIB_FUNC("GZ2YtnlAzH4", Impl::UserGetAccountIdFromRegistory);
	LIB_FUNC("IyT41iG8Ync", Impl::UserIsEmpty);
	LIB_FUNC("JwpT2LYSxrg", Impl::UserIsGuest);
	LIB_FUNC("td8GJFROaEA", Impl::UserGetUserId);
	// NpOnlineId / NpId / ticket / tokens / http / ipc service.
	LIB_FUNC("1GRQfw+bhcE", Impl::NpOnlineIdGet);
	LIB_FUNC("Icc9+aRUQfQ", Impl::NpOnlineIdSetStr);
	LIB_FUNC("oGASj6Qjq7M", Impl::NpObjectNoop); // NpOnlineId ctor
	LIB_FUNC("dgSWiLGbjuY", Impl::NpObjectNoop); // NpOnlineId copy ctor
	LIB_FUNC("YYfLHMi0+2M", Impl::NpObjectNoop); // NpOnlineId ctor
	LIB_FUNC("UyUHeYA21sg", Impl::NpObjectNoop); // NpOnlineId dtor
	LIB_FUNC("m3jEtGAP9jE", Impl::NpOnlineIdEq);
	LIB_FUNC("SuCCD+AZwCc", Impl::NpObjectNoop); // NpId copy ctor
	LIB_FUNC("YU-PxwZq21U", Impl::NpObjectNoop); // NpId ctor
	LIB_FUNC("WcfJXQ2NFP4", Impl::NpObjectNoop); // NpId dtor
	LIB_FUNC("AIMCjPPVWZM", Impl::NpObjectNoop); // NpTicket dtor
	LIB_FUNC("ox2Ie98lPAQ", Impl::NpTicketGetData);
	LIB_FUNC("fs1TCWwTYCA", Impl::NpTicketGetSize);
	LIB_FUNC("h8d3tfMiyhc", Impl::NpObjectNoop); // NpAccessToken ctor
	LIB_FUNC("0SfP+1+7aB4", Impl::NpObjectNoop); // NpAccessToken dtor
	LIB_FUNC("j6oWzyuDal4", Impl::NpObjectNoop); // NpTitleToken ctor
	LIB_FUNC("+fA-tpNvZNg", Impl::NpObjectNoop); // NpTitleToken dtor
	LIB_FUNC("lDCWWROsrEg", Impl::NpTitleTokenGetStr);
	LIB_FUNC("CvGog64+vCk", Impl::NpObjectNoop); // NpHttpClient::Init
	LIB_FUNC("QvqOkNK5ThU", Impl::NpObjectNoop); // NpHttpClient ctor
	LIB_FUNC("kR3ed2pAvV8", Impl::NpObjectNoop); // NpHttpClient dtor
	LIB_FUNC("JDYbbgccPDE", Impl::NpObjectNoop); // NpHttpTrans ctor
	LIB_FUNC("D5qJmwMlccI", Impl::NpObjectNoop); // NpHttpTrans dtor
	LIB_FUNC("hKTdrR1+dN0", Impl::IpcServiceGetIpmiClient);
	LIB_FUNC("tpXVNSFwJRs", Impl::NpObjectNoop); // service_client::InitService
	LIB_FUNC("UvDQq9+QMuI", Impl::NpObjectNoop); // service_client::TermService
	// Guest memory impls.
	LIB_FUNC("p0TfCdPEcsk", NpManagerMemory::Allocate);
	LIB_FUNC("8JX-S2ADen4", NpManagerMemory::Free);
	LIB_FUNC("pHLjntY0psg", NpManagerIpcMemory::CreateKernel);
	LIB_FUNC("UdhQmx64-uM", NpManagerIpcMemory::CreateBuffer);
	LIB_FUNC("hyuye+88uPo", NpManagerIpcMemory::Destroy);
	LIB_FUNC("V38nfJwXYhg", NpManagerIpcMemory::GetAllocator);
	LIB_FUNC("VY8Xji9cAFA", NpManagerIpcMemory::Free);
	LIB_FUNC("VBZtcFn7+aY", NpManagerIpcMemory::Allocate);
	LIB_FUNC("TyACAxDH3Uw", NpManagerIpcMemory::Reallocate);
	// Vsh / push / unknown-? NIDs: success-noop.
	LIB_FUNC("TJqSgUEzexM", Impl::VshNoop); // sceNpSetNpTitleIdVsh
	LIB_FUNC("LGEIdgILQek", Impl::VshNoop); // sceNpManagerPrxStartVsh
	LIB_FUNC("9P8qV9WtgKA", Impl::VshNoop); // sceNpManagerPrxStopVsh
	LIB_FUNC("+rPzLhUKj1Y", Impl::VshNoop); // sceNpPush2SetGlobalMutex
	LIB_FUNC("81ldjv-ybsA", Impl::VshNoop);
	LIB_FUNC("VCYDmZygruk", Impl::VshNoop);
	LIB_FUNC("2o4V3QCvnfg", Impl::VshNoop);
	LIB_FUNC("Gv4cB8leZaU", Impl::VshNoop);
	LIB_FUNC("uqHeyEjZlpA", Impl::VshNoop);
	LIB_FUNC("sUonpM7eAg8", Impl::VshNoop);
	LIB_FUNC("WmA5X4w-4Sg", Impl::VshNoop);
	LIB_FUNC("glO5Roao0-0", Impl::VshNoop);
	LIB_FUNC("krFzyfMTVKo", Impl::VshNoop);
	LIB_FUNC("5FWkeFFM1WI", Impl::VshNoop);
	LIB_FUNC("AydqLDpvliU", Impl::VshNoop);
	LIB_FUNC("z8eiw8+UfEo", Impl::VshNoop);
	LIB_FUNC("Dw8yC2rYpT0", Impl::VshNoop);
	LIB_FUNC("K3B--gWssAk", Impl::VshNoop);
	LIB_FUNC("iC9I+uYJfAw", Impl::VshNoop);
	LIB_FUNC("hABPod2fe5Y", Impl::VshNoop);
	LIB_FUNC("VGkLQcESh5k", Impl::VshNoop);
	LIB_FUNC("u4zM1slIDrI", Impl::VshNoop);
	LIB_FUNC("j26eOWbUwwo", Impl::VshNoop);
	LIB_FUNC("lB7tiCkrHcw", Impl::VshNoop);
	LIB_FUNC("ue6sId17JRE", Impl::VshNoop);
	LIB_FUNC("+fUJ4QmdNxg", Impl::VshNoop);
	LIB_FUNC("-1Q057TFjDE", Impl::VshNoop);
	LIB_FUNC("hNBwchvmG90", Impl::VshNoop);
	LIB_FUNC("gg2CzDord9E", Impl::VshNoop);
	LIB_FUNC("1I4EiKvohEo", Impl::VshNoop);
	LIB_FUNC("0cZdUbbTaMw", Impl::VshNoop);
	LIB_FUNC("lQfpsyGl4Nc", Impl::VshNoop);
	LIB_FUNC("DDiKTyHJivk", Impl::VshNoop);
	LIB_FUNC("aQaOGIVChN4", Impl::VshNoop);
	LIB_FUNC("899ScRQvFV0", Impl::VshNoop);
	LIB_FUNC("4gVqbwFkKGY", Impl::VshNoop);
	LIB_FUNC("-OrDVMqLIG4", Impl::VshNoop);
	LIB_FUNC("ukG+D0QVfuQ", Impl::VshNoop);
	LIB_FUNC("vsJdqui4uB8", Impl::VshNoop);
	LIB_FUNC("p7wseS6VIsU", Impl::VshNoop);
	LIB_FUNC("v+6TY5GrDHA", Impl::VshNoop);
	LIB_FUNC("3sU9cWXBN98", Impl::VshNoop);
}

} // namespace NpManager

namespace NpCommon {

LIB_VERSION("NpCommon", 1, "NpCommon", 1, 1);

namespace Impl {

// NpCommon data-driven set (60 NIDs real; the ~445-NID remainder is intentionally
// unregistered: the runtime linker's UnresolvedImportStub logs any future call
// with its symbol name, which is the next census). Base: JSON + allocator +
// platform families as empty-success. Round 2: the 40 hottest ShellCore
// NpCommon imports from _ProbeLogs/shellcore_boot8.log, ordered by import-site
// (slot) count with msg_pack-family frequency as tiebreak — the boot log holds
// zero npcommon-deferred/runtime NpCommon call lines, so static slot count is
// the hotness signal. Document calls return valid non-null empty handles
// (never null — guest parsers AV on null); verbs return OK; sizes/counts and
// Is*/Has* predicates return 0; proven pointer-to-pointer outs are filled with
// a fresh empty handle; unknown-size single-pointer outs are left untouched.

constexpr int kNpInvalidArg = -2141913085; // 0x80550003

static void* EmptyJsonHandle(const char* what) {
	// Valid non-null empty object: parsers must never see null.
	void* p = ::Libs::GuestHeap::AllocateZeroed(1, 64);
	LOGF("\t npcommon: %s -> %p (empty handle)\n", what, p);
	if (p == nullptr) {
		static uint64_t dummy = 0;
		return &dummy;
	}
	return p; // deliberately leaked (teardown frees nothing here)
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonAllocHandle(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return EmptyJsonHandle("json");
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAllocatorMalloc(uint64_t size, uint64_t a2, uint64_t a3,
                                             uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	const uint64_t want = (size == 0 || size > (16ull << 20)) ? 64ull : size;
	void*          p    = ::Libs::GuestHeap::Allocate(static_cast<size_t>(want));
	LOGF("\t npcommon: allocator-malloc(%llu) -> %p\n",
	     static_cast<unsigned long long>(want), p);
	if (p == nullptr) {
		static int dummy = 0;
		return &dummy; // never null to the guest
	}
	return p;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAllocatorFree(uint64_t ptr, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t npcommon: allocator-free(%p): noop (never-free rule)\n",
	     reinterpret_cast<void*>(static_cast<uintptr_t>(ptr)));
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static char* DupGuestString(uint64_t guest_str, uint64_t max_len, const char* what) {
	// Bounded copy of a guest (identity-mapped) string; null guest or empty
	// yields a valid non-null empty string, never null.
	char* out = static_cast<char*>(::Libs::GuestHeap::Allocate(max_len + 1));
	if (out == nullptr) {
		static char empty = '\0';
		return &empty;
	}
	size_t n = 0;
	if (guest_str != 0) {
		const auto* src = reinterpret_cast<const char*>(static_cast<uintptr_t>(guest_str));
		while (n < max_len && src[n] != '\0') {
			out[n] = src[n];
			++n;
		}
	}
	out[n] = '\0';
	LOGF("\t npcommon: %s(%p) -> %p (%llu chars)\n", what,
	     reinterpret_cast<const void*>(static_cast<uintptr_t>(guest_str)), out,
	     static_cast<unsigned long long>(n));
	return out;
}

static char* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAllocatorStrdup(uint64_t guest_str) {
	PRINT_NAME();
	return DupGuestString(guest_str, 4096, "strdup");
}

static char* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAllocatorStrndup(uint64_t guest_str, uint64_t len) {
	PRINT_NAME();
	const uint64_t cap = len > 4096 ? 4096 : len;
	return DupGuestString(guest_str, cap, "strndup");
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAllocatorExConvert(uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	static int dummy = 0; // valid non-null empty allocator token
	LOGF("\t npcommon: allocator-ex-convert -> %p\n", static_cast<const void*>(&dummy));
	return &dummy;
}

static const char* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpUtilGetEnv(uint64_t a1, uint64_t a2, uint64_t a3,
                                              uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	LOGF("\t npcommon: util-get-env -> \"\" (empty)\n");
	return "";
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetPlatformType(uint64_t out_type) {
	PRINT_NAME();
	LOGF("\t npcommon: get-platform-type\n");
	if (out_type == 0) {
		return kNpInvalidArg;
	}
	*reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(out_type)) = 0;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpSetPlatformType(uint64_t type, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t npcommon: set-platform-type(%llu)\n", static_cast<unsigned long long>(type));
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetSdkVersion(uint64_t out_version) {
	PRINT_NAME();
	LOGF("\t npcommon: get-sdk-version\n");
	if (out_version == 0) {
		return kNpInvalidArg;
	}
	*reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(out_version)) = 0;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpGetSdkVersionUInt(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t npcommon: get-sdk-version-uint -> 0\n");
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpMutexTryLock(uint64_t m, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	LOGF("\t npcommon: mutex-trylock(%p) -> acquired\n",
	     reinterpret_cast<void*>(static_cast<uintptr_t>(m)));
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

// ---- round 2: 40 hottest ShellCore NpCommon imports (boot8 slot census) ----
// sce::np C++ surface: Cancelable/Handle lifecycle, HttpTrans I/O, NpCommId,
// msg_pack document model. Object layouts are unknown so `this` is never
// touched; unknown-size single-pointer outs are left unwritten (success-noop).

static void* FreshDocHandle(const char* what) {
	// Valid non-null empty document node: parsers must never see null.
	void* p = ::Libs::GuestHeap::AllocateZeroed(1, 64);
	LOGF("\t npcommon: %s -> %p (empty doc handle)\n", what, p);
	if (p == nullptr) {
		static uint64_t dummy = 0;
		return &dummy;
	}
	return p; // deliberately leaked (teardown frees nothing here)
}

// Fill a proven pointer-to-pointer guest out-param with a fresh empty handle.
static int FillHandleOut(uint64_t guest_out, const char* what) {
	if (guest_out == 0) {
		LOGF("\t null guest out-param\n");
		return kNpInvalidArg;
	}
	*reinterpret_cast<void**>(static_cast<uintptr_t>(guest_out)) = FreshDocHandle(what);
	return OK;
}

// Verbs, ctors, dtors, and unknown-size-out getters: success-noop. One body
// serves all arities (unused register args are ignored).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpNoopOk(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return OK;
}

// Factories / iterators with no provable out-param: fresh non-null handle
// (nonzero also reads as success if the guest checks it as an int).
static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpNoopHandle(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return FreshDocHandle("factory");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpFillHandleOut1(uint64_t o1, uint64_t a2, uint64_t a3,
                                          uint64_t a4) {
	PRINT_NAME();
	(void)a2;
	(void)a3;
	(void)a4;
	return FillHandleOut(o1, "get-result");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpFillHandleOut2(uint64_t a1, uint64_t o2, uint64_t a3,
                                          uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a3;
	(void)a4;
	return FillHandleOut(o2, "get-ptr");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpFillHandleOut3(uint64_t a1, uint64_t a2, uint64_t o3,
                                          uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a4;
	return FillHandleOut(o3, "put-and-get-ptr");
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAddRef(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 1; // object stays alive under the never-free rule; ignored if void
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpZeroSize(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	PRINT_NAME();
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0; // empty document/collection: count 0
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpRead(uint64_t h, uint64_t buf, uint64_t len, uint64_t out) {
	PRINT_NAME();
	LOGF("\t http-read: h = 0x%016" PRIx64 " len = %llu (offline EOF)\n", h,
	     static_cast<unsigned long long>(len));
	(void)h;
	if (out != 0) {
		*reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(out)) = 0;
	}
	if (buf != 0 && len != 0) {
		// Guest-owned buffer of exactly len bytes; cap the fill so an absurd
		// length cannot stall us.
		uint64_t fill = len > (16ull << 20) ? (16ull << 20) : len;
		std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(buf)), 0,
		            static_cast<size_t>(fill));
	}
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpWrite(uint64_t h, uint64_t buf, uint64_t len, uint64_t out) {
	PRINT_NAME();
	LOGF("\t http-write: h = 0x%016" PRIx64 " len = %llu (accepted, offline)\n", h,
	     static_cast<unsigned long long>(len));
	(void)h;
	(void)buf;
	if (out != 0) {
		*reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(out)) = len;
	}
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MsgPackBuildBuf(uint64_t buf, uint64_t cap, uint64_t out,
                                         uint64_t a4) {
	PRINT_NAME();
	(void)a4;
	if (out != 0) {
		*reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(out)) = 0; // empty build
	}
	if (buf != 0 && cap != 0) {
		uint64_t fill = cap > (16ull << 20) ? (16ull << 20) : cap;
		std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(buf)), 0,
		            static_cast<size_t>(fill));
	}
	return OK;
}

} // namespace Impl

LIB_DEFINE(InitOfflineLoginNpCommon_1) {
	// JSON family: valid empty handles, never null.
	LIB_FUNC("SnAdybtBK3o", Impl::JsonAllocHandle); // sceNpJsonParseBufInit
	LIB_FUNC("RgfCYkjW7As", Impl::JsonAllocHandle); // sceNpJsonParseBuf
	LIB_FUNC("vJGDnNh4I0g", Impl::JsonAllocHandle); // sceNpJsonParse
	LIB_FUNC("teVnFAL6GNY", Impl::JsonAllocHandle); // sceNpJsonParseInit
	LIB_FUNC("nhgjiwPUIzI", Impl::JsonAllocHandle); // sceNpJsonParseExInit
	LIB_FUNC("p5ZkSMRR7AU", Impl::JsonAllocHandle); // sceNpJsonParseEx
	// Allocator family.
	LIB_FUNC("mzlILsFx0cU", Impl::NpAllocatorMalloc);
	LIB_FUNC("BztTl7QeYqE", Impl::NpAllocatorFree);
	LIB_FUNC("c8-4aC9opYE", Impl::NpAllocatorStrdup);
	LIB_FUNC("a2qdVU8RWb4", Impl::NpAllocatorMalloc); // ExMalloc
	LIB_FUNC("Zh23aSLeeZo", Impl::NpAllocatorFree); // ExFree
	LIB_FUNC("Cmd4+m7V00c", Impl::NpAllocatorStrdup); // ExStrdup
	LIB_FUNC("EziLjfyTnKI", Impl::NpAllocatorStrndup); // ExStrndup
	LIB_FUNC("0gdlCVNNHCI", Impl::NpAllocatorExConvert);
	// Platform / env family.
	LIB_FUNC("kBON3bAtfGs", Impl::NpUtilGetEnv);
	LIB_FUNC("sXVQUIGmk2U", Impl::NpGetPlatformType);
	LIB_FUNC("m9JzZSoDVFY", Impl::NpSetPlatformType);
	LIB_FUNC("Pglk7zFj0DI", Impl::NpGetSdkVersion);
	LIB_FUNC("ljqnF0hmLjo", Impl::NpGetSdkVersionUInt);
	LIB_FUNC("DuslmoqQ+nk", Impl::NpMutexTryLock);
	// Round 2: 40 hottest ShellCore NpCommon imports from
	// _ProbeLogs/shellcore_boot8.log (slot counts in comments).
	// Cancelable/Handle lifecycle (20/4/4 slots + thunk).
	LIB_FUNC("Jhbrpz0YhHU", Impl::NpNoopOk); // Cancelable::Cancel (20 slots)
	LIB_FUNC("-c9QK+CpQLg", Impl::NpNoopOk); // Handle::CancelImpl (4 slots)
	LIB_FUNC("xDrWJARfCbk", Impl::NpNoopOk); // Handle::CancelImpl thunk (4 slots)
	// HttpTrans I/O (3/2/2/2 slots): EOF reads, accepted writes, offline.
	LIB_FUNC("aWo+7jvpllY", Impl::HttpRead); // HttpTrans::Read (3 slots)
	LIB_FUNC("cocNRQpq+NA", Impl::HttpWrite); // HttpTrans::Write (2 slots)
	LIB_FUNC("eRlqlofFKYg", Impl::HttpWrite); // HttpTrans::Write thunk (2 slots)
	LIB_FUNC("G+v692ul7MA", Impl::HttpRead); // HttpTrans::Read thunk (2 slots)
	// NpCommId dtor (2 slots).
	LIB_FUNC("EPJbX73AVeU", Impl::NpNoopOk); // NpCommId::~NpCommId (2 slots)
	// msg_pack document model (1 slot each; hottest family, 42 refs).
	LIB_FUNC("mcksYTt3a6c", Impl::NpNoopOk); // MsgPackDoc::Add
	LIB_FUNC("H3xH9j+vDL4", Impl::NpNoopOk); // MsgPackDoc::~MsgPackDoc
	LIB_FUNC("MrXN6wk7gYk", Impl::NpFillHandleOut3); // MsgPackMap::PutAndGetPtr
	LIB_FUNC("QClFP2KKPF0", Impl::NpNoopOk); // MsgPackMap::Put
	LIB_FUNC("isr1XxY2gIc", Impl::NpNoopHandle); // MsgPackMap::Create
	LIB_FUNC("6eCw3RJWCxY", Impl::NpNoopOk); // MsgPackMap::Remove
	LIB_FUNC("jXZjoKUWiBQ", Impl::NpFillHandleOut2); // MsgPackArray::AddAndGetPtr
	LIB_FUNC("pqht4bHLsdk", Impl::NpNoopOk); // MsgPackArray::Add
	LIB_FUNC("y55nRnJYB1c", Impl::NpNoopHandle); // MsgPackArray::Create
	LIB_FUNC("Wqvp6nAuan8", Impl::NpNoopOk); // MsgPackValue::MsgPackValue(copy)
	LIB_FUNC("g2rmacQqWek", Impl::NpNoopOk); // MsgPackValue::MsgPackValue()
	LIB_FUNC("ubrdHLu65Pg", Impl::NpNoopOk); // MsgPackValue::MsgPackValue(type)
	LIB_FUNC("G7OZdy22jgg", Impl::NpNoopOk); // MsgPackValue::MsgPackValue(copy)
	LIB_FUNC("QvQfxWPMNlQ", Impl::NpNoopOk); // MsgPackValue::~MsgPackValue
	LIB_FUNC("pI5mbDNOcmw", Impl::NpNoopOk); // MsgPackValue::~MsgPackValue
	LIB_FUNC("MUk+VbtOj2Y", Impl::NpNoopHandle); // MsgPackBinary::Create
	LIB_FUNC("h5bNnlNV06Y", Impl::NpNoopOk); // MsgPackBinary::SetValue
	LIB_FUNC("tGUr9CtgQ2A", Impl::NpNoopOk); // MsgPackParser::Parse(buf)
	LIB_FUNC("Av6dlMaFg1U", Impl::NpNoopOk); // MsgPackParser::Parse(stream)
	LIB_FUNC("Mq-XgqBhtSY", Impl::NpFillHandleOut1); // MsgPackParser::GetResult
	LIB_FUNC("ezGVzRFN7Oc", Impl::NpNoopOk); // MsgPackParser::~MsgPackParser
	LIB_FUNC("KDqpahluouo", Impl::NpNoopHandle); // MsgPackString::Create
	LIB_FUNC("nUesWVRd6eg", Impl::NpNoopOk); // MsgPackString::SetValue
	LIB_FUNC("HMuylrBDF74", Impl::NpNoopOk); // MsgPackBuilder::Build(stream)
	LIB_FUNC("h9N+tt3BnZk", Impl::MsgPackBuildBuf); // MsgPackBuilder::Build(buf)
	LIB_FUNC("D4r++h0mvxo", Impl::NpAddRef); // MsgPackValueImpl::AddRef
	LIB_FUNC("2hfOTyl0hTY", Impl::NpAddRef); // MsgPackValueImpl::Release
	LIB_FUNC("2GCKkDEZ10Y", Impl::NpZeroSize); // MsgPackDoc::GetMessagePackSize
	LIB_FUNC("Q8zIb0yTAmo", Impl::NpNoopOk); // MsgPackMap::Get
	LIB_FUNC("yHjqkRTF5JA", Impl::NpFillHandleOut2); // MsgPackMap::GetPtr
	LIB_FUNC("C76Kms3ZD98", Impl::NpZeroSize); // MsgPackMap::GetSize
	LIB_FUNC("AnxdSIcTprM", Impl::NpNoopHandle); // MsgPackMap::Iterator
}

} // namespace NpCommon

LIB_DEFINE(InitOfflineLogin_1) {
	NpManager::InitOfflineLoginNpManager_1(s);
	NpCommon::InitOfflineLoginNpCommon_1(s);
}

} // namespace OfflineLogin

} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp (paste, do not edit this file) ----
// namespace OfflineLogin {
// LIB_DEFINE(InitOfflineLogin_1);
// } // namespace OfflineLogin
// ... in InitAll():  OfflineLogin::InitOfflineLogin_1(s);
