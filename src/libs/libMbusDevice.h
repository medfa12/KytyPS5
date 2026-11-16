#pragma once

#include "common/emulatorConfig.h"
#include "libs/libs.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace Libs::Mbus {

inline constexpr uint64_t kVirtualPadDeviceId = 2;
inline constexpr int32_t kVirtualPadTransport = 0;
// Retail 13.60 LoginMgr +0x33fea0 dispatches MBUS 22 to the Bond class;
// its +0x390310/+0x390330 getters report Bond USB (21), subtype Bond (2).
// MBUS 23/24 are Aston L/R (VR controllers), not DualSense. PSM's internal
// hardware number is a different enum from managed Orbis HardwareType.
inline constexpr int32_t kVirtualPadDeviceType = 22;
// Retail PSM +0x40eb0 maps MBUS 22 to internal hardware 10. The open-type
// table at +0x2c9a2c, indexed by hardware-1, selects standard pad port 0.
inline constexpr int32_t kVirtualPadOpenType = 0;

// The single host-input pad belongs to the active offline user. Keep device
// enumeration and owner announcements consistent with UserService. Native
// 13.60 PSM +0x40ec5 converts nonpositive DeviceInfo.user_id values to -1,
// leaving an otherwise connected and polled pad unassigned to HomeUI.
inline int VirtualPadOwnerUserId() {
	if (NoForegroundUser()) { return -1; }
	if (UserService::UserServiceStoreEnabled()) {
		int user = -1;
		return UserService::UserServiceStoreGetForegroundUser(&user) == 0 ? user : -1;
	}
	return Config::GetUserId();
}

inline std::array<uint8_t, 256> VirtualPadDeviceInfo(int32_t owner) {
	std::array<uint8_t, 256> info {};
	const int32_t function_count = 1;
	std::memcpy(info.data(), &kVirtualPadDeviceId, sizeof(kVirtualPadDeviceId));
	std::memcpy(info.data() + 8, &kVirtualPadTransport, sizeof(kVirtualPadTransport));
	std::memcpy(info.data() + 12, &kVirtualPadDeviceType, sizeof(kVirtualPadDeviceType));
	std::memcpy(info.data() + 20, &function_count, sizeof(function_count));
	std::memcpy(info.data() + 200, &owner, sizeof(owner));
	return info;
}

} // namespace Libs::Mbus
