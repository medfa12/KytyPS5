#include "common/abi.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/hostHttp.h"
#include "libs/netIfList.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <vector>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

// ps5-vsh: real implementations for the SceNetConfig ifaddr batch.
//
// Background: ShellCore boot died at SceShellCore.elf+0x40a1af (null+0x18)
// right after the stubbed sceNetConfigSetIfaddr6 (QJbV3vfBQ8Q) wrote NOTHING
// into the guest buffer; the guest then consumed the empty buffer.
// Observed calls (shellcore_boot3.log):
//   8Kh+1eidI3c(0, buf, 0, 0, 1, buf)          with src = 127.0.0.1
//   QJbV3vfBQ8Q(0, buf, 0x80, 0, 0, buf+6)     with src = ::1
// Except NetGetIfList (verified typed ABI below), these legacy layouts remain
// unknown, so the older entrypoints below LOGF all
// six registers, never crashes on nulls, and (for Set/Add/Get variants)
// fills the (buf, buflen) pair at (a1, a2) with the matching loopback string
// capped at min(buflen, 0x80) with null-termination. Pure-query siblings
// (DNS info, stats getters, route/config no-ops) log and return empty-success
// without touching guest memory.

namespace Libs::Network::NetIfaddr {

namespace {

constexpr uint64_t kMaxIfaddrWrite = 0x80;

void Log6(const char* name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
          uint64_t a5) {
	LOGF("ps5-vsh: %s(%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ", %016" PRIx64
	     ", %016" PRIx64 ")\n",
	     name, a0, a1, a2, a3, a4, a5);
}

// Writability probe + capped loopback write. Returns bytes written (0 = none).
uint64_t WriteLoopback(const char* name, uint64_t buf_addr, uint64_t buflen, bool want_v6) {
	const char* loopback = want_v6 ? "::1" : "127.0.0.1";
	if (buf_addr == 0) {
		LOGF("\t %s: null buffer, nothing written\n", name);
		return 0;
	}
	// Strict cap: min(buflen, 0x80). A zero buflen (observed on the v4
	// SetIfaddr call) means "no length given" -> write nothing, log only.
	// Writing blind there diverted the guest (boot4 crashed before the v6
	// call); only the v6-shaped (buf, 0x80) call carries an explicit length.
	if (buflen == 0) {
		LOGF("\t %s: zero buflen, nothing written\n", name);
		return 0;
	}
	uint64_t cap = buflen;
	if (cap > kMaxIfaddrWrite) {
		cap = kMaxIfaddrWrite;
	}
	const uint64_t need = static_cast<uint64_t>(std::strlen(loopback)) + 1;
	uint64_t           n    = need < cap ? need : cap;
	if (n == 0) {
		return 0;
	}
	auto* buf = reinterpret_cast<char*>(static_cast<uintptr_t>(buf_addr));
	std::memcpy(buf, loopback, static_cast<size_t>(n - 1));
	buf[n - 1] = '\0';
	LOGF("\t %s: wrote \"%s\" (%llu bytes) to guest 0x%016" PRIx64 "\n", name, loopback,
	     static_cast<unsigned long long>(n), buf_addr);
	return n;
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IfaddrLike(const char* name, bool want_v6, bool write, uint64_t a0,
                                 uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
	Log6(name, a0, a1, a2, a3, a4, a5);
	if (write) {
		WriteLoopback(name, a1, a2, want_v6);
	}
	return 0;
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI QueryEmpty(const char* name, uint64_t a0, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
	Log6(name, a0, a1, a2, a3, a4, a5);
	return 0;
}

} // namespace

// --- Set/Add/Get/Del ifaddr batch (NIDs from shellcore_imports_full.txt) ---

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigSetIfaddr(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigSetIfaddr", false, true, a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigSetIfaddr6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigSetIfaddr6", true, true, a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigGetIfaddr(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigGetIfaddr", false, true, a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigGetIfaddr6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigGetIfaddr6", true, true, a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigAddIfaddr(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigAddIfaddr", false, true, a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigAddIfaddr6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigAddIfaddr6", true, true, a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigDelIfaddr(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigDelIfaddr", false, false, a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigDelIfaddr6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigDelIfaddr6", true, false, a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigSetIfLinkLocalAddr6(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3, uint64_t a4, uint64_t a5) {
	return IfaddrLike("sceNetConfigSetIfLinkLocalAddr6", true, true, a0, a1, a2, a3, a4, a5);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetGetIfList(int name, NetIfList* entries, int capacity) {
	if (name < 0 || name >= 10 || capacity < 0) { return NET_ERROR_EINVAL; }
	if (capacity == 0) { return 0; }
	if (!entries) { return NET_ERROR_EFAULT; }
	if (!HostHttp::Enabled()) { return 0; }
#if defined(_WIN32)
	// Adapter discovery can hold host heap locks; protect it from guest GC
	// suspension just like NetCtl's host adapter query.
	Common::HostLockCriticalGuard critical;
	ULONG bytes = 0;
	constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
	auto result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &bytes);
	if (result == ERROR_NO_DATA) { return 0; }
	if (result != ERROR_BUFFER_OVERFLOW || !bytes) { return NET_ERROR_EIO; }
	std::vector<uint8_t> storage(bytes);
	IP_ADAPTER_ADDRESSES* adapters = nullptr;
	for (int attempt = 0; attempt < 3; ++attempt) {
		adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
		result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &bytes);
		if (result != ERROR_BUFFER_OVERFLOW) { break; }
		storage.resize(bytes);
	}
	if (result == ERROR_NO_DATA) { return 0; }
	if (result != NO_ERROR) { return NET_ERROR_EIO; }
	int ethernet = 0, wireless = 0;
	for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
		int guest_name = -1;
		const char* label = nullptr;
		uint32_t type = 0;
		if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) { guest_name = 0; label = "lo0"; }
		else if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD && ethernet < 2) {
			guest_name = ++ethernet; label = guest_name == 1 ? "eth0" : "eth1"; type = 1;
		} else if (adapter->IfType == IF_TYPE_IEEE80211 && wireless < 2) {
			guest_name = 4 + wireless++; label = guest_name == 4 ? "wlan0" : "wlan1"; type = 2;
		}
		if (guest_name != name) { continue; }
		NetIfList entry {};
		std::strcpy(entry.name, label);
		entry.index = static_cast<int32_t>(adapter->IfIndex);
		entry.type = type;
		entry.mtu = adapter->Mtu;
		if (adapter->PhysicalAddressLength == sizeof(entry.mac)) {
			std::memcpy(entry.mac, adapter->PhysicalAddress, sizeof(entry.mac));
		}
		const bool up = adapter->OperStatus == IfOperStatusUp;
		entry.flags = (up ? 0x41u : 0u) | (name == 0 ? 8u : 2u);
		if (!(adapter->Flags & IP_ADAPTER_NO_MULTICAST)) { entry.flags |= 0x8000u; }
		// UP/RUNNING describe the adapter. INTERNET is not inferred from a link.
		entry.link_status = up ? 1 : 0;
		entry.tx_bps = static_cast<uint32_t>(std::min<ULONG64>(adapter->TransmitLinkSpeed, UINT32_MAX));
		entry.rx_bps = static_cast<uint32_t>(std::min<ULONG64>(adapter->ReceiveLinkSpeed, UINT32_MAX));
		unsigned v4 = 0, v6 = 0;
		for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
			const auto* socket = address->Address.lpSockaddr;
			if (!socket) { continue; }
			if (socket->sa_family == AF_INET && v4 < 2 && address->OnLinkPrefixLength <= 32) {
				auto& dest = entry.ipv4[v4++];
				dest.address = reinterpret_cast<const sockaddr_in*>(socket)->sin_addr.s_addr;
				const auto prefix = address->OnLinkPrefixLength;
				dest.netmask = htonl(prefix ? UINT32_MAX << (32 - prefix) : 0);
				if (name != 0) { dest.broadcast = dest.address | ~dest.netmask; }
			} else if (socket->sa_family == AF_INET6 && v6 < 2) {
				std::memcpy(entry.ipv6[v6], &reinterpret_cast<const sockaddr_in6*>(socket)->sin6_addr, 16);
				entry.ipv6_prefix[v6] = address->OnLinkPrefixLength;
				if (address->DadState == IpDadStateTentative) { entry.ipv6_flags[v6] |= 2; }
				if (address->DadState == IpDadStateDuplicate) { entry.ipv6_flags[v6] |= 4; }
				++v6;
			}
		}
		if (!LibKernel::FileSystem::CopyPathResultToGuest(entries, &entry, sizeof(entry))) { return NET_ERROR_EFAULT; }
		LOGF("sceNetGetIfList: name=%d returned=1 host_index=%u up=%d type=%u\n", name, adapter->IfIndex, up, type);
		return 1;
	}
	return 0;
#else
	return NET_ERROR_EOPNOTSUPP;
#endif
}

// --- Pure-query / config no-ops: log + empty-success, no guest writes ---

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetGetDnsInfo(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5) {
	return QueryEmpty("sceNetGetDnsInfo", a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetGetDns6Info(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
	return QueryEmpty("sceNetGetDns6Info", a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetGetMemoryPoolStats(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                             uint64_t a4, uint64_t a5) {
	return QueryEmpty("sceNetGetMemoryPoolStats", a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetAddrConfig6GetInfo(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                            uint64_t a4, uint64_t a5) {
	return QueryEmpty("sceNetAddrConfig6GetInfo", a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetDhcpGetInfo(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                      uint64_t a5) {
	return QueryEmpty("sceNetDhcpGetInfo", a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetEventCallbackGetError(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4, uint64_t a5) {
	return QueryEmpty("sceNetEventCallbackGetError", a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigEtherGetLinkMode(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4, uint64_t a5) {
	return QueryEmpty("sceNetConfigEtherGetLinkMode", a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigGetDefaultRoute(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                                uint64_t a4, uint64_t a5) {
	return QueryEmpty("sceNetConfigGetDefaultRoute", a0, a1, a2, a3, a4, a5);
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NetConfigGetDefaultRoute6(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                                 uint64_t a4, uint64_t a5) {
	return QueryEmpty("sceNetConfigGetDefaultRoute6", a0, a1, a2, a3, a4, a5);
}

} // namespace Libs::Network::NetIfaddr
