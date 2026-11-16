#pragma once

#include "common/abi.h"
#include <cstddef>

namespace Libs::Network::NetIfaddr {
// Regular 13.60 Sce.Vsh.SysBridge.Net.SceNetIfList (Sequential, Pack=4).
// ShellUI +71919a expects a returned entry count and IPv4 at +0x28;
// +71921b reads the six-byte MAC at +0x50. The native record is 0x1e0.
#pragma pack(push, 4)
struct NetIfList {
	char name[16], destination_name[16];
	int32_t index, destination_index;
	struct Ipv4 { uint32_t address, destination, broadcast, netmask; } ipv4[2];
	uint32_t flags, type;
	uint8_t mac[6];
	uint8_t padding[2];
	uint32_t mtu, max_mtu;
	struct Statistics { uint64_t tx[8], rx[8]; } driver_statistics, emulator_statistics;
	uint32_t link_status, reserved1, total_use_timer;
	uint32_t tx_bps, rx_bps, max_tx_bps, max_rx_bps;
	uint8_t ipv6[2][16];
	uint32_t ipv6_prefix[2], ipv6_flags[2];
	uint8_t reserved[52];
};
#pragma pack(pop)
static_assert(sizeof(NetIfList) == 0x1e0);
static_assert(offsetof(NetIfList, ipv4) == 0x28);
static_assert(offsetof(NetIfList, mac) == 0x50);
static_assert(offsetof(NetIfList, link_status) == 0x160);
int KYTY_SYSV_ABI NetGetIfList(int name, NetIfList* entries, int capacity);
} // namespace Libs::Network::NetIfaddr
