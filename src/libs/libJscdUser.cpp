#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/runtimeUserService.h"
#include "loader/symbolDatabase.h"

#include <cstdint>

namespace Libs {

LIB_VERSION("UserService", 1, "UserService", 1, 1);

namespace JscdUser {

namespace {
// File-local INVALID_ARGUMENT (0x80960005), matching
// UserService::USER_SERVICE_ERROR_INVALID_ARGUMENT. Kept local so this TU
// never depends on libUserService.cpp internals.
constexpr int kInvalidArgument = -2137653243;
} // namespace

// Generic named stub: log NID + name + raw args, succeed, never touch guest.
#define JSCD_NAMED_STUB(Func, NidStr, FullName)                                                    \
	static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int Func(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {         \
		PRINT_NAME();                                                                      \
		LOGF("\t jscd-user: NID=%s %s(%llu, %llu, %llu, %llu)\n", NidStr, FullName,          \
		     static_cast<unsigned long long>(a0), static_cast<unsigned long long>(a1),      \
		     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3));     \
		return OK;                                                                     \
	}

// Unknown NID: log NID + raw args, succeed, never touch guest.
#define JSCD_UNK_STUB(Func, NidStr)                                                                 \
	static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int Func(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {         \
		PRINT_NAME();                                                                      \
		LOGF("\t jscd-user-unk: NID=%s(%llu, %llu, %llu, %llu)\n", NidStr,                   \
		     static_cast<unsigned long long>(a0), static_cast<unsigned long long>(a1),      \
		     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3));     \
		return OK;                                                                     \
	}

// 13.60 eNb53LQJmIM -> 0x41f0 -> 0x750: IPC 0x3000c, one 4-byte output.
static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int JscdUserServiceGetForegroundUser(int* user_id) {
	PRINT_NAME();
	if (user_id == nullptr) {
		return kInvalidArgument;
	}
	if (UserService::UserServiceStoreEnabled()) {
		return UserService::UserServiceStoreGetForegroundUser(user_id);
	}
	*user_id = Libs::NoForegroundUser() ? -1 : Config::GetUserId();
	LOGF("\t jscd-user: NID=eNb53LQJmIM sceUserServiceGetForegroundUser -> uid=%d\n",
	     static_cast<int>(*user_id));
	return OK;
}

struct JscdRegisteredUserIdList {
	// 13.60 libSceUserService: 5EiQCnL2G1Y -> 0x4250 -> 0x8b0,
	// IPC 0x3000f with an 0x50-byte output. Managed MaxRegisteredUserNum = 20.
	int user_id[20];
};

static_assert(sizeof(JscdRegisteredUserIdList) == 0x50);

static KYTY_SYSV_ABI int
JscdUserServiceGetRegisteredUserIdList(JscdRegisteredUserIdList* list) {
	PRINT_NAME();
	if (list == nullptr) {
		return kInvalidArgument;
	}
	if (UserService::UserServiceStoreEnabled()) {
		return UserService::UserServiceStoreGetRegisteredUsers(list->user_id);
	}
	for (int& id : list->user_id) {
		id = -1;
	}
	// Preserve the legacy direct-title and no-foreground behavior.
	if (!Libs::IsDirectTitleBoot() && !Libs::NoForegroundUser()) {
		list->user_id[0] = Config::GetUserId();
		LOGF("\t jscd-user: NID=5EiQCnL2G1Y sceUserServiceGetRegisteredUserIdList -> uid=%d\n",
		     static_cast<int>(list->user_id[0]));
		return OK;
	}
	LOGF("\t jscd-user: NID=5EiQCnL2G1Y sceUserServiceGetRegisteredUserIdList -> empty (offline)\n");
	return OK;
}

// ps5-vsh: matches libSceUserService.sprx FnWkLNOmJXw exactly (disassembled):
// a one-argument predicate on the user id's top bits, result in eax. There is
// NO out-pointer - treating arg2 as int* wrote through a garbage register and
// crashed the host (0x1404c1c0e) when Sony's AsyncStorageInternal called it.
static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int JscdUserServiceIsGuestUser(int user_id) {
	PRINT_NAME();
	const int is_guest = ((static_cast<uint32_t>(user_id) & 0xe0000000u) == 0x20000000u) ? 1 : 0;
	LOGF("\t jscd-user: NID=FnWkLNOmJXw sceUserServiceIsGuestUser(uid=%d) -> %d\n", user_id, is_guest);
	return is_guest;
}

// --- Named log-and-succeed stubs (null-safe, no guest deref) ---
static KYTY_SYSV_ABI int JscdUserServiceRegisterEventCallback(uint64_t callback, uint64_t userdata) {
	return UserService::ChangeEventCallback(callback, userdata, true);
}
// 13.60 +0x4e60: property5, exactly four output bytes.
static KYTY_SYSV_ABI int JscdUserServiceGetLastLoginOrder(int user, uint32_t* output) {
	return UserService::GetStoredScalar(user, 5, output);
}
JSCD_NAMED_STUB(JscdUserServiceGetPlayingBigAppTitleId, "i1yCh-jxLZM",
                "sceUserServiceGetPlayingBigAppTitleId")
// 13.60 +0x48f0: property 2 string getter, bounded to 0x400 bytes.
static KYTY_SYSV_ABI int JscdUserServiceGetPasscode(int user, char* output, size_t size) {
	return UserService::GetStoredPasscode(user, output, size < 0x400 ? size : 0x400);
}
// 13.60 +0x4aa0 -> +0x520: property3 uses a four-byte boolean, not bool*.
static KYTY_SYSV_ABI int JscdUserServiceGetAutoLoginEnabled(int user, uint32_t* output) {
	return UserService::GetStoredScalar(user, 3, output);
}
JSCD_NAMED_STUB(JscdUserServiceGetIsRemotePlayAllowed, "Lgi5A4fQwHc",
                "sceUserServiceGetIsRemotePlayAllowed")
JSCD_NAMED_STUB(JscdUserServiceSetIsRemotePlayAllowed, "AQ680L4Sr74",
                "sceUserServiceSetIsRemotePlayAllowed")
JSCD_NAMED_STUB(JscdUserServiceGetIsSpartacusEnabled, "ImAMsZEEA5I",
                "sceUserServiceGetIsSpartacusEnabled")
JSCD_NAMED_STUB(JscdUserServiceGetIsCronosEnabled, "Dj7OdPACkhg",
                "sceUserServiceGetIsCronosEnabled")
static KYTY_SYSV_ABI int JscdUserServiceUnregisterEventCallback(uint64_t callback, uint64_t userdata) {
	return UserService::ChangeEventCallback(callback, userdata, false);
}
JSCD_NAMED_STUB(JscdUserServiceIsSharePlayClientUser, "-7XgCmEwKrs",
                "sceUserServiceIsSharePlayClientUser")
JSCD_NAMED_STUB(JscdUserServiceGetParentalGame, "OdiXSuoIK7c", "sceUserServiceGetParentalGame")
JSCD_NAMED_STUB(JscdUserServiceSetParentalGame, "gRI+BnPA6UI", "sceUserServiceSetParentalGame")
JSCD_NAMED_STUB(JscdUserServiceGetParentalBdAge, "lXKtAHMrwig",
                "sceUserServiceGetParentalBdAge")
JSCD_NAMED_STUB(JscdUserServiceSetParentalBdAge, "KJw6rahYNdQ",
                "sceUserServiceSetParentalBdAge")
JSCD_NAMED_STUB(JscdUserServiceGetParentalDvdRegion, "d9DOmIk9-y4",
                "sceUserServiceGetParentalDvdRegion")
JSCD_NAMED_STUB(JscdUserServiceSetParentalDvdRegion, "NJpUvo+rezg",
                "sceUserServiceSetParentalDvdRegion")
JSCD_NAMED_STUB(JscdUserServiceGetParentalDvd, "5vtFYXFJ7OU", "sceUserServiceGetParentalDvd")
JSCD_NAMED_STUB(JscdUserServiceSetParentalDvd, "UT8+lb5fypc", "sceUserServiceSetParentalDvd")
JSCD_NAMED_STUB(JscdUserServiceGetParentalBrowser, "t04S4aC0LCM",
                "sceUserServiceGetParentalBrowser")
JSCD_NAMED_STUB(JscdUserServiceSetParentalBrowser, "6jPYBCGQgiQ",
                "sceUserServiceSetParentalBrowser")
JSCD_NAMED_STUB(JscdUserServiceGetParentalMorpheus, "yXvfR+AcgaY",
                "sceUserServiceGetParentalMorpheus")
JSCD_NAMED_STUB(JscdUserServiceSetParentalMorpheus, "mmFgyjXMQBs",
                "sceUserServiceSetParentalMorpheus")
JSCD_NAMED_STUB(JscdUserServiceGetParentalGameAgeLevel, "oXARzvLAiyc",
                "sceUserServiceGetParentalGameAgeLevel")
JSCD_NAMED_STUB(JscdUserServiceSetParentalGameAgeLevel, "BPFs-TiU+8Q",
                "sceUserServiceSetParentalGameAgeLevel")
JSCD_NAMED_STUB(JscdUserServiceGetParentalContentControl, "QBLzc0izFLc",
                "sceUserServiceGetParentalContentControl")
JSCD_NAMED_STUB(JscdUserServiceSetParentalContentControl, "VOclFUxMNQ0",
                "sceUserServiceSetParentalContentControl")
JSCD_NAMED_STUB(JscdUserServiceGetParentalGameWhiteList, "5ZJPtZf6EaY",
                "sceUserServiceGetParentalGameWhiteList")
JSCD_NAMED_STUB(JscdUserServiceSetParentalGameWhiteList, "hQAhxWNHwvM",
                "sceUserServiceSetParentalGameWhiteList")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcFlag, "HsOlaoGngDc", "sceUserServiceGetPbtcFlag")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcFlag, "JK0fCuBEWJM", "sceUserServiceSetPbtcFlag")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcMode, "Q5Um9Yri-VA", "sceUserServiceGetPbtcMode")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcMode, "94ZcZmcnXK4", "sceUserServiceSetPbtcMode")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcTzOffset, "NjEMsEjXlTY", "sceUserServiceGetPbtcTzOffset")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcTzOffset, "HsjvaxD7veE", "sceUserServiceSetPbtcTzOffset")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcAdditionalTime, "m1h-E6BU6CA",
                "sceUserServiceGetPbtcAdditionalTime")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcAdditionalTime, "dukLb11bY9c",
                "sceUserServiceSetPbtcAdditionalTime")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcMondayDuration, "NnvYm9PFJiw",
                "sceUserServiceGetPbtcMondayDuration")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcMondayDuration, "SoxZWGb3l0U",
                "sceUserServiceSetPbtcMondayDuration")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcMondayHoursStart, "WunW7G5bHYo",
                "sceUserServiceGetPbtcMondayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcMondayHoursStart, "7XIlJQQZ2fg",
                "sceUserServiceSetPbtcMondayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcMondayHoursEnd, "42K0F17ml9c",
                "sceUserServiceGetPbtcMondayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcMondayHoursEnd, "uBDKFasVr2c",
                "sceUserServiceSetPbtcMondayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcTuesdayDuration, "UPDgXiV1Zp0",
                "sceUserServiceGetPbtcTuesdayDuration")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcTuesdayDuration, "YX-64Vjk5oM",
                "sceUserServiceSetPbtcTuesdayDuration")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcTuesdayHoursStart, "azCh0Ibz8ls",
                "sceUserServiceGetPbtcTuesdayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcTuesdayHoursStart, "bLfjqFmN4s4",
                "sceUserServiceSetPbtcTuesdayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcTuesdayHoursEnd, "Kpds+6CpTus",
                "sceUserServiceGetPbtcTuesdayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcTuesdayHoursEnd, "MtE3Me0UJKc",
                "sceUserServiceSetPbtcTuesdayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcWednesdayDuration, "VwF4r--aouQ",
                "sceUserServiceGetPbtcWednesdayDuration")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcWednesdayDuration, "EqfGtRCryNg",
                "sceUserServiceSetPbtcWednesdayDuration")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcWednesdayHoursStart, "7Wes8MVwuoM",
                "sceUserServiceGetPbtcWednesdayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcWednesdayHoursStart, "dDaO7svUM8w",
                "sceUserServiceSetPbtcWednesdayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcWednesdayHoursEnd, "nxGZSi5FEwc",
                "sceUserServiceGetPbtcWednesdayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcWednesdayHoursEnd, "uZG5rmROeg4",
                "sceUserServiceSetPbtcWednesdayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcThursdayDuration, "z-hJNdfLRN0",
                "sceUserServiceGetPbtcThursdayDuration")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcThursdayDuration, "R9vnyf-B1pU",
                "sceUserServiceSetPbtcThursdayDuration")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcThursdayHoursStart, "T70Qyzo51uw",
                "sceUserServiceGetPbtcThursdayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcThursdayHoursStart, "JO5QXiyBcjQ",
                "sceUserServiceSetPbtcThursdayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcThursdayHoursEnd, "BkOBCo0sdLM",
                "sceUserServiceGetPbtcThursdayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcThursdayHoursEnd, "W3oNrewI7bc",
                "sceUserServiceSetPbtcThursdayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcFridayDuration, "3DuTkVXaj9Y",
                "sceUserServiceGetPbtcFridayDuration")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcFridayDuration, "RUrfnne6Dds",
                "sceUserServiceSetPbtcFridayDuration")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcFridayHoursStart, "vcd5Kfs1QeA",
                "sceUserServiceGetPbtcFridayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcFridayHoursStart, "GMLAWOO7I2Y",
                "sceUserServiceSetPbtcFridayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcFridayHoursEnd, "5dM-i0Ox2d8",
                "sceUserServiceGetPbtcFridayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcFridayHoursEnd, "YWmKJ8pWEkw",
                "sceUserServiceSetPbtcFridayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcSaturdayDuration, "DembpCGx9DU",
                "sceUserServiceGetPbtcSaturdayDuration")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcSaturdayDuration, "RTrsbjUnFNo",
                "sceUserServiceSetPbtcSaturdayDuration")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcSaturdayHoursStart, "+1qj-S-k6m0",
                "sceUserServiceGetPbtcSaturdayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcSaturdayHoursStart, "p2NKAA3BS6k",
                "sceUserServiceSetPbtcSaturdayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcSaturdayHoursEnd, "Cf8NftzheE4",
                "sceUserServiceGetPbtcSaturdayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcSaturdayHoursEnd, "8wVUn7AO8mA",
                "sceUserServiceSetPbtcSaturdayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcSundayDuration, "JVMIyR8vDec",
                "sceUserServiceGetPbtcSundayDuration")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcSundayDuration, "hGnwgvLREHM",
                "sceUserServiceSetPbtcSundayDuration")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcSundayHoursStart, "J+cECJ7CBFM",
                "sceUserServiceGetPbtcSundayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcSundayHoursStart, "cTpHiHGMWpk",
                "sceUserServiceSetPbtcSundayHoursStart")
JSCD_NAMED_STUB(JscdUserServiceGetPbtcSundayHoursEnd, "J+bKHRzY4nw",
                "sceUserServiceGetPbtcSundayHoursEnd")
JSCD_NAMED_STUB(JscdUserServiceSetPbtcSundayHoursEnd, "rp4DB+ICfcg",
                "sceUserServiceSetPbtcSundayHoursEnd")

// --- Unknown NIDs (no name in nids.csv) ---
JSCD_UNK_STUB(LogStub_Unk_dkPbHlL9O8, "-dkPbHlL9O8")
JSCD_UNK_STUB(LogStub_UnkBx_ESDBWJm4, "Bx+ESDBWJm4")

LIB_DEFINE(InitJscdUser_1) {
	LIB_FUNC("wuI7c7UNk0A", JscdUser::JscdUserServiceRegisterEventCallback);
	LIB_FUNC("eNb53LQJmIM", JscdUser::JscdUserServiceGetForegroundUser);
	LIB_FUNC("FnWkLNOmJXw", JscdUser::JscdUserServiceIsGuestUser);
	LIB_FUNC("4nUbGGBcGco", JscdUser::JscdUserServiceGetLastLoginOrder);
	LIB_FUNC("i1yCh-jxLZM", JscdUser::JscdUserServiceGetPlayingBigAppTitleId);
	LIB_FUNC("X5rJZNDZ2Ss", JscdUser::JscdUserServiceGetPasscode);
	LIB_FUNC("nqDEnj7M0QE", JscdUser::JscdUserServiceGetAutoLoginEnabled);
	LIB_FUNC("Lgi5A4fQwHc", JscdUser::JscdUserServiceGetIsRemotePlayAllowed);
	LIB_FUNC("AQ680L4Sr74", JscdUser::JscdUserServiceSetIsRemotePlayAllowed);
	LIB_FUNC("ImAMsZEEA5I", JscdUser::JscdUserServiceGetIsSpartacusEnabled);
	LIB_FUNC("Dj7OdPACkhg", JscdUser::JscdUserServiceGetIsCronosEnabled);
	LIB_FUNC("spW--yoLQ9o", JscdUser::JscdUserServiceUnregisterEventCallback);
	LIB_FUNC("5EiQCnL2G1Y", JscdUser::JscdUserServiceGetRegisteredUserIdList);
	LIB_FUNC("-7XgCmEwKrs", JscdUser::JscdUserServiceIsSharePlayClientUser);
	LIB_FUNC("OdiXSuoIK7c", JscdUser::JscdUserServiceGetParentalGame);
	LIB_FUNC("gRI+BnPA6UI", JscdUser::JscdUserServiceSetParentalGame);
	LIB_FUNC("lXKtAHMrwig", JscdUser::JscdUserServiceGetParentalBdAge);
	LIB_FUNC("KJw6rahYNdQ", JscdUser::JscdUserServiceSetParentalBdAge);
	LIB_FUNC("d9DOmIk9-y4", JscdUser::JscdUserServiceGetParentalDvdRegion);
	LIB_FUNC("NJpUvo+rezg", JscdUser::JscdUserServiceSetParentalDvdRegion);
	LIB_FUNC("5vtFYXFJ7OU", JscdUser::JscdUserServiceGetParentalDvd);
	LIB_FUNC("UT8+lb5fypc", JscdUser::JscdUserServiceSetParentalDvd);
	LIB_FUNC("t04S4aC0LCM", JscdUser::JscdUserServiceGetParentalBrowser);
	LIB_FUNC("6jPYBCGQgiQ", JscdUser::JscdUserServiceSetParentalBrowser);
	LIB_FUNC("yXvfR+AcgaY", JscdUser::JscdUserServiceGetParentalMorpheus);
	LIB_FUNC("mmFgyjXMQBs", JscdUser::JscdUserServiceSetParentalMorpheus);
	LIB_FUNC("oXARzvLAiyc", JscdUser::JscdUserServiceGetParentalGameAgeLevel);
	LIB_FUNC("BPFs-TiU+8Q", JscdUser::JscdUserServiceSetParentalGameAgeLevel);
	LIB_FUNC("QBLzc0izFLc", JscdUser::JscdUserServiceGetParentalContentControl);
	LIB_FUNC("VOclFUxMNQ0", JscdUser::JscdUserServiceSetParentalContentControl);
	LIB_FUNC("5ZJPtZf6EaY", JscdUser::JscdUserServiceGetParentalGameWhiteList);
	LIB_FUNC("hQAhxWNHwvM", JscdUser::JscdUserServiceSetParentalGameWhiteList);
	LIB_FUNC("-dkPbHlL9O8", JscdUser::LogStub_Unk_dkPbHlL9O8);
	LIB_FUNC("Bx+ESDBWJm4", JscdUser::LogStub_UnkBx_ESDBWJm4);
	LIB_FUNC("HsOlaoGngDc", JscdUser::JscdUserServiceGetPbtcFlag);
	LIB_FUNC("JK0fCuBEWJM", JscdUser::JscdUserServiceSetPbtcFlag);
	LIB_FUNC("Q5Um9Yri-VA", JscdUser::JscdUserServiceGetPbtcMode);
	LIB_FUNC("94ZcZmcnXK4", JscdUser::JscdUserServiceSetPbtcMode);
	LIB_FUNC("NjEMsEjXlTY", JscdUser::JscdUserServiceGetPbtcTzOffset);
	LIB_FUNC("HsjvaxD7veE", JscdUser::JscdUserServiceSetPbtcTzOffset);
	LIB_FUNC("m1h-E6BU6CA", JscdUser::JscdUserServiceGetPbtcAdditionalTime);
	LIB_FUNC("dukLb11bY9c", JscdUser::JscdUserServiceSetPbtcAdditionalTime);
	LIB_FUNC("NnvYm9PFJiw", JscdUser::JscdUserServiceGetPbtcMondayDuration);
	LIB_FUNC("SoxZWGb3l0U", JscdUser::JscdUserServiceSetPbtcMondayDuration);
	LIB_FUNC("WunW7G5bHYo", JscdUser::JscdUserServiceGetPbtcMondayHoursStart);
	LIB_FUNC("7XIlJQQZ2fg", JscdUser::JscdUserServiceSetPbtcMondayHoursStart);
	LIB_FUNC("42K0F17ml9c", JscdUser::JscdUserServiceGetPbtcMondayHoursEnd);
	LIB_FUNC("uBDKFasVr2c", JscdUser::JscdUserServiceSetPbtcMondayHoursEnd);
	LIB_FUNC("UPDgXiV1Zp0", JscdUser::JscdUserServiceGetPbtcTuesdayDuration);
	LIB_FUNC("YX-64Vjk5oM", JscdUser::JscdUserServiceSetPbtcTuesdayDuration);
	LIB_FUNC("azCh0Ibz8ls", JscdUser::JscdUserServiceGetPbtcTuesdayHoursStart);
	LIB_FUNC("bLfjqFmN4s4", JscdUser::JscdUserServiceSetPbtcTuesdayHoursStart);
	LIB_FUNC("Kpds+6CpTus", JscdUser::JscdUserServiceGetPbtcTuesdayHoursEnd);
	LIB_FUNC("MtE3Me0UJKc", JscdUser::JscdUserServiceSetPbtcTuesdayHoursEnd);
	LIB_FUNC("VwF4r--aouQ", JscdUser::JscdUserServiceGetPbtcWednesdayDuration);
	LIB_FUNC("EqfGtRCryNg", JscdUser::JscdUserServiceSetPbtcWednesdayDuration);
	LIB_FUNC("7Wes8MVwuoM", JscdUser::JscdUserServiceGetPbtcWednesdayHoursStart);
	LIB_FUNC("dDaO7svUM8w", JscdUser::JscdUserServiceSetPbtcWednesdayHoursStart);
	LIB_FUNC("nxGZSi5FEwc", JscdUser::JscdUserServiceGetPbtcWednesdayHoursEnd);
	LIB_FUNC("uZG5rmROeg4", JscdUser::JscdUserServiceSetPbtcWednesdayHoursEnd);
	LIB_FUNC("z-hJNdfLRN0", JscdUser::JscdUserServiceGetPbtcThursdayDuration);
	LIB_FUNC("R9vnyf-B1pU", JscdUser::JscdUserServiceSetPbtcThursdayDuration);
	LIB_FUNC("T70Qyzo51uw", JscdUser::JscdUserServiceGetPbtcThursdayHoursStart);
	LIB_FUNC("JO5QXiyBcjQ", JscdUser::JscdUserServiceSetPbtcThursdayHoursStart);
	LIB_FUNC("BkOBCo0sdLM", JscdUser::JscdUserServiceGetPbtcThursdayHoursEnd);
	LIB_FUNC("W3oNrewI7bc", JscdUser::JscdUserServiceSetPbtcThursdayHoursEnd);
	LIB_FUNC("3DuTkVXaj9Y", JscdUser::JscdUserServiceGetPbtcFridayDuration);
	LIB_FUNC("RUrfnne6Dds", JscdUser::JscdUserServiceSetPbtcFridayDuration);
	LIB_FUNC("vcd5Kfs1QeA", JscdUser::JscdUserServiceGetPbtcFridayHoursStart);
	LIB_FUNC("GMLAWOO7I2Y", JscdUser::JscdUserServiceSetPbtcFridayHoursStart);
	LIB_FUNC("5dM-i0Ox2d8", JscdUser::JscdUserServiceGetPbtcFridayHoursEnd);
	LIB_FUNC("YWmKJ8pWEkw", JscdUser::JscdUserServiceSetPbtcFridayHoursEnd);
	LIB_FUNC("DembpCGx9DU", JscdUser::JscdUserServiceGetPbtcSaturdayDuration);
	LIB_FUNC("RTrsbjUnFNo", JscdUser::JscdUserServiceSetPbtcSaturdayDuration);
	LIB_FUNC("+1qj-S-k6m0", JscdUser::JscdUserServiceGetPbtcSaturdayHoursStart);
	LIB_FUNC("p2NKAA3BS6k", JscdUser::JscdUserServiceSetPbtcSaturdayHoursStart);
	LIB_FUNC("Cf8NftzheE4", JscdUser::JscdUserServiceGetPbtcSaturdayHoursEnd);
	LIB_FUNC("8wVUn7AO8mA", JscdUser::JscdUserServiceSetPbtcSaturdayHoursEnd);
	LIB_FUNC("JVMIyR8vDec", JscdUser::JscdUserServiceGetPbtcSundayDuration);
	LIB_FUNC("hGnwgvLREHM", JscdUser::JscdUserServiceSetPbtcSundayDuration);
	LIB_FUNC("J+cECJ7CBFM", JscdUser::JscdUserServiceGetPbtcSundayHoursStart);
	LIB_FUNC("cTpHiHGMWpk", JscdUser::JscdUserServiceSetPbtcSundayHoursStart);
	LIB_FUNC("J+bKHRzY4nw", JscdUser::JscdUserServiceGetPbtcSundayHoursEnd);
	LIB_FUNC("rp4DB+ICfcg", JscdUser::JscdUserServiceSetPbtcSundayHoursEnd);
}

} // namespace JscdUser

} // namespace Libs

// --- libs.cpp registration (paste manually; this file does NOT self-register) ---
// LIB_DEFINE(InitJscdUser_1);
// InitJscdUser_1(s);
