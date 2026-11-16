#include "common/abi.h"
#include "common/common.h"
#include "common/singleton.h"
#include "libs/errno.h"
#include "libs/libGuestFile.h"
#include "libs/libs.h"
#include "libs/libcSort.h"
#include "libs/libMbusDevice.h"
#include "libs/network.h"
#include "libs/npGlobalMemory.h"
#include "libs/ipmiClientConfig.h"
#include "libs/kernelIoctl.h"
#include "libs/rngSysctl.h"
#include "kernel/fileSystem.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#if defined(_MSC_VER)
#pragma comment(lib, "Psapi.lib")
#endif
#endif

// ps5-vsh: ShellCore boot hits (shellcore_boot1.log, 39 "unknown stub" calls,
// 18 distinct NIDs). Each LIB_VERSION below uses the guest's EXACT
// (library, lib_version, module, mod_major, mod_minor) key, recovered from the
// SceShellCore.elf dynamic tables (DT_OS_IMPORT_LIB_1 / DT_OS_NEEDED_MODULE_1
// + NID#libid#modid symbols, EncodeId64-matched), so Resolve() exact-Finds
// these records instead of falling back to the older LogStub entries.
// InitShellHits_1 must run AFTER the LogStub registrations in Libs::InitAll
// (see snippet at end of file): SymbolDatabase::Add is insert_or_assign, so
// the last exact-key registration wins.

namespace Libs {
namespace LibcInternal {
void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Memalign(size_t boundary, size_t size);
} // namespace LibcInternal
namespace ShellHits {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Guest memory is identity-mapped, but a stale/garbage pointer must never
// take the emulator down (cf. null+0x18 crash). Page-walk probe; false when
// any page is uncommitted or lacks the needed access.
static bool ProbeGuest(uint64_t addr, size_t len, bool need_write) {
	if (addr == 0 || len == 0) {
		return false;
	}
	const uint64_t end = addr + len;
	if (end < addr) {
		return false;
	}
	for (uint64_t p = addr; p < end;) {
		MEMORY_BASIC_INFORMATION mi {};
		if (VirtualQuery(reinterpret_cast<const void*>(p), &mi, sizeof(mi)) == 0 ||
		    mi.State != MEM_COMMIT || (mi.Protect & PAGE_GUARD) != 0) {
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
	return addr != 0 && len != 0;
}
#endif

static bool ReadGuestStr(uint64_t addr, char* out, size_t cap) {
	if (out == nullptr || cap == 0) {
		return false;
	}
	out[0] = '\0';
	if (addr == 0) {
		return false;
	}
	// Bounded scan; re-probe per byte so a string running into an unmapped
	// page fails instead of faulting.
	size_t n = 0;
	for (; n + 1 < cap; n++) {
		if (!ProbeGuest(addr + n, 1, false)) {
			out[n] = '\0';
			return false;
		}
		const char c = *reinterpret_cast<const char*>(addr + n);
		out[n]       = c;
		if (c == '\0') {
			return true;
		}
	}
	out[n] = '\0';
	return true; // truncated but NUL-terminated
}

static bool WriteGuestBytes(uint64_t addr, const void* src, size_t len) {
	if (src == nullptr || len == 0) {
		return addr != 0;
	}
	if (!ProbeGuest(addr, len, true)) {
		return false;
	}
	std::memcpy(reinterpret_cast<void*>(addr), src, len);
	return true;
}

// Zero-slot validated scalar writes for sensor structs (power-watch feeders).
// Reads back the probed slot first and only fills it when it is still zero,
// so live guest data (pointers, counters, status words) is never clobbered.
// 4/8-byte only, null-safe, never bulk — the canary rule.
static bool ReadGuestU32(uint64_t addr, uint32_t* out) {
	if (out == nullptr || !ProbeGuest(addr, sizeof(*out), false)) {
		return false;
	}
	std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(*out));
	return true;
}

static bool ReadGuestU64(uint64_t addr, uint64_t* out) {
	if (out == nullptr || !ProbeGuest(addr, sizeof(*out), false)) {
		return false;
	}
	std::memcpy(out, reinterpret_cast<const void*>(addr), sizeof(*out));
	return true;
}

static bool WriteGuestU32ZeroSlot(uint64_t addr, uint32_t v) {
	uint32_t cur = 0;
	if (!ReadGuestU32(addr, &cur) || cur != 0) {
		return false;
	}
	return WriteGuestBytes(addr, &v, sizeof(v));
}

static bool WriteGuestU64ZeroSlot(uint64_t addr, uint64_t v) {
	uint64_t cur = 0;
	if (!ReadGuestU64(addr, &cur) || cur != 0) {
		return false;
	}
	return WriteGuestBytes(addr, &v, sizeof(v));
}

// ps5-vsh boot90: guest-vs-host range gate for power-watch data pointers.
// Guest thread stacks grow down from PTHREAD_STACK_TOP (0x7efff8000,
// src/kernel/pthread.cpp); every observed watch struct (CA5 a0, ioctl a2,
// CA5 a4-boot60 0x7ecdbfcf8, CA5 a4-early 0x7ef1cfdb8) sits in that window.
// Host thread objects (ioctl a3 0xffe..., CA5 a1 0xffe...) and small-int
// channel IDs (0x42xxxx/0x52xxxx) must never be touched: ProbeGuest alone
// cannot tell host-heap from guest (identity-mapped), so require this window
// as well. Conservative by design: a wrong exclusion only logs, a wrong
// inclusion would corrupt host memory.
static bool IsGuestWatchRange(uint64_t addr) {
	return addr >= 0x100000000ull && addr < 0x7efff8000ull;
}

// Claim-zero + re-ramp-while-marker over a watch range of n u32 slots.
// Same canary discipline as the ioctl a2 temp-sweep: a slot is claimed only
// while zero; it is re-ramped only while it still holds our last marker, so
// a guest takeover releases the claim and live words are never clobbered.
// u32 milli-C markers: on LE a zero high word keeps the slot a valid u64
// milli-C too, so one width covers u32 and u64 readers (width disambiguated
// post-hoc from the dump + print correlation). div/spacing differ per range
// so a moved print identifies its source range and offset.
static void RampWatchRange(uint64_t base, const char* tag, uint32_t* last,
                           uint32_t* claimed, uint64_t tick, uint32_t div,
                           uint32_t spacing, int n) {
	for (int i = 0; i < n; i++) {
		const uint64_t off = base + static_cast<uint64_t>(i) * 4u;
		uint32_t cur = 0;
		if (!ReadGuestU32(off, &cur)) {
			continue;
		}
		const uint32_t want = 1000u * (45u + static_cast<uint32_t>(
		    (tick / div + static_cast<uint64_t>(i) * spacing) % 21u));
		if ((*claimed & (1u << i)) != 0) {
			if (cur == last[i]) {
				if (WriteGuestBytes(off, &want, sizeof(want))) {
					last[i] = want;
				}
			} else {
				*claimed &= ~(1u << i);
				LOGF("\t %s: +%u taken over (now %u), released\n", tag,
				     static_cast<unsigned>(i) * 4u, cur);
			}
		} else if (cur == 0) {
			if (WriteGuestBytes(off, &want, sizeof(want))) {
				*claimed |= (1u << i);
				last[i] = want;
				LOGF("\t %s: claimed +%u = %u.%03uC\n", tag,
				     static_cast<unsigned>(i) * 4u, want / 1000u, want % 1000u);
			}
		}
	}
}

namespace Kernel {

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HasTrinityMode() {
	PRINT_NAME();
	// ps5-vsh: yu17wG8L5FI sceKernelHasTrinityMode (7x). Retail: no Trinity.
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IsDevKit() {
	PRINT_NAME();
	// ps5-vsh: QNjGUdj1HPM sceKernelIsDevKit. Retail emulation.
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IsAuthenticTrinity() {
	PRINT_NAME();
	// ps5-vsh: X0HkB92+NRE sceKernelIsAuthenticTrinity.
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IccSetBootTimestamp(uint64_t kind, uint64_t ts_ptr, uint64_t a2,
                                             uint64_t a3) {
	PRINT_NAME();
	// ps5-vsh: Ht23vmmuP0Q sceKernelIccSetBootTimestamp(kind, ts). Setter ack.
	LOGF("\t kind = %llu ts_ptr = 0x%016" PRIx64 "\n",
	     static_cast<unsigned long long>(kind), ts_ptr);
	(void)a2;
	(void)a3;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IccGetPowerUpCause(uint64_t out, uint64_t a1, uint64_t a2,
                                            uint64_t a3) {
	PRINT_NAME();
	// ps5-vsh: wXmx+vN8puE sceKernelIccGetPowerUpCause(out). 0 = cold/unknown.
	(void)a1;
	(void)a2;
	(void)a3;
	if (out == 0) {
		LOGF("\t null out-param\n");
		return -1;
	}
	const uint32_t cause = 0;
	if (!WriteGuestBytes(out, &cause, sizeof(cause))) {
		LOGF("\t unwritable out-param 0x%016" PRIx64 "\n", out);
		return -1;
	}
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Ioctl(uint64_t fd, uint64_t req, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	req = static_cast<uint32_t>(req); // Native ioctl normalizes request numbers to u32.
	int device_result = 0;
	if (LibKernel::FileSystem::TryMetaDbgIoctl(static_cast<int32_t>(fd), &device_result) ||
	    LibKernel::FileSystem::TryDevctlIoctl(static_cast<int32_t>(fd), &device_result) ||
	    LibKernel::FileSystem::TryM2Ioctl(static_cast<int32_t>(fd), static_cast<uint32_t>(req),
	                                   reinterpret_cast<void*>(a2), &device_result) ||
	    LibKernel::FileSystem::TrySshotIoctl(static_cast<int32_t>(fd), static_cast<uint32_t>(req),
	                                      reinterpret_cast<void*>(a2), &device_result) ||
	    LibKernel::FileSystem::TryNpDrmIoctl(static_cast<int32_t>(fd), static_cast<uint32_t>(req),
	                                      reinterpret_cast<void*>(a2), &device_result)) {
		if (device_result != 0) {
			*Posix::GetErrorAddr() = LibKernel::KernelToPosix(device_result);
			return -1;
		}
		return device_result;
	}
	// Existing non-NPDRM socket and device operations.
	LOGF("\t fd = 0x%016" PRIx64 " req = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     fd, req, a2, a3, a4, a5);
	// AgcCompositor identifies GnmCompositor clients by their peer PID
	// (_IOR(0x93, 3, int)); report it from the real local socket.
	if (req == 0x40049303u && Network::Net::IsSocket(static_cast<int>(fd))) {
		int        pid    = 0;
		const auto result = Network::Net::GetUnixPeerProcessId(static_cast<int>(fd), &pid);
		if (result == 0 && !WriteGuestBytes(a2, &pid, sizeof(pid))) {
			*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
			return -1;
		}
		LOGF("\t socket peer pid -> %d (result %d)\n", pid, result);
		return result;
	}
	// ps5-vsh: boot60 power-watch feeder. Every [SceSoCPowerWatch] Current
	// print is immediately preceded by ioctl(fd 0x80020002, 0x406cae01,
	// struct, ...) with a CONSTANT watch-loop struct (a2 = 0x...feb0,
	// a5 = 0x40 = 64 bytes) plus one init call at /dev/gbase bring-up.
	// boot60 proof: GFX W/V/A live via the CA5jcDb2vYI a0+0/+4/+8 feed
	// (42.5W ~= 0.824V x 51.571A), CPU 2.2W derived, but die temp STILL
	// 0.3C and CPU V/A 0. The boot40 u64 temp write at +16 MISSED: it
	// landed (t:1) on the INIT struct, while the watch struct either has
	// temp elsewhere or holds it nonzero (300 mC?) so the zero-slot write
	// skips. This replaces the single-offset guess with (a) a read-only
	// 16-word dump of the first 2 hits (init vs watch layout) and (b) a
	// claim-and-ramp temp sweep: u32 milli-C markers at
	// +12/+20/+24/+28/+32/+36, claimed only when zero and re-ramped only
	// while the slot still holds OUR last marker (a guest takeover
	// releases the claim, so live guest words are never clobbered: the
	// canary rule). Markers are 3C apart inside the 45-65C cap, so
	// whichever value the print shows identifies its offset. The u64 +16
	// write is kept (harmless per boot60; a ~63C print would confirm +16).
	if (req == 0x406cae01u && a2 != 0 && ProbeGuest(a2, 64, true)) {
		const uint64_t tick = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::seconds>(
		        std::chrono::steady_clock::now().time_since_epoch())
		        .count());
		const uint32_t cpuMw = 15000u + static_cast<uint32_t>((tick * 211u) % 10001u);
		const uint32_t cpuMv = 1000u + static_cast<uint32_t>((tick * 13u) % 151u);
		const uint32_t cpuMa = cpuMw * 1000u / (cpuMv != 0 ? cpuMv : 1u);
		const uint64_t dieMilliC = 1000ull * (45ull + ((tick / 8ull) % 21ull));
		const bool w = WriteGuestU32ZeroSlot(a2, cpuMw);
		const bool v = WriteGuestU32ZeroSlot(a2 + 4, cpuMv);
		const bool a = WriteGuestU32ZeroSlot(a2 + 8, cpuMa);
		const bool t = WriteGuestU64ZeroSlot(a2 + 16, dieMilliC);
		// (a) layout dump: pristine words, logged before any sweep write.
		static int hits = 0;
		if (hits < 2) {
			hits++;
			uint32_t w0[16] = {};
			for (int i = 0; i < 16; i++) {
				if (!ReadGuestU32(a2 + static_cast<uint64_t>(i) * 4u, &w0[i])) {
					w0[i] = 0xdeadbeefu;
				}
			}
			LOGF("\t PfccT7qURYE gbase dump #%d: a2=0x%016" PRIx64
			     " a3=0x%016" PRIx64 " a4=0x%016" PRIx64 " a5=0x%016" PRIx64
			     " w=[%08x %08x %08x %08x %08x %08x %08x %08x"
			     " %08x %08x %08x %08x %08x %08x %08x %08x]\n",
			     hits, a2, a3, a4, a5, w0[0], w0[1], w0[2], w0[3], w0[4],
			     w0[5], w0[6], w0[7], w0[8], w0[9], w0[10], w0[11], w0[12],
			     w0[13], w0[14], w0[15]);
		}
		// (b) temp sweep: claim-and-ramp, 3C-spaced markers in 45-65C cap.
		static const uint32_t kTempOffs[6] = {12, 20, 24, 28, 32, 36};
		static uint32_t       sLast[6]     = {0, 0, 0, 0, 0, 0};
		static uint32_t       sClaimed     = 0;
		const uint64_t        half         = tick / 2u;
		for (int i = 0; i < 6; i++) {
			const uint64_t off = a2 + kTempOffs[i];
			uint32_t       cur = 0;
			if (!ReadGuestU32(off, &cur)) {
				continue;
			}
			const uint32_t want = 1000u * (45u + static_cast<uint32_t>(
				(half + static_cast<uint64_t>(i) * 3u) % 21u));
			if ((sClaimed & (1u << i)) != 0) {
				if (cur == sLast[i]) {
					if (WriteGuestBytes(off, &want, sizeof(want))) {
						sLast[i] = want;
					}
				} else {
					// Guest (or another feeder) took the slot: release,
					// never touch it again.
					sClaimed &= ~(1u << i);
					LOGF("\t PfccT7qURYE temp-sweep: +%u taken over (now %u), released\n",
					     kTempOffs[i], cur);
				}
			} else if (cur == 0) {
				if (WriteGuestBytes(off, &want, sizeof(want))) {
					sClaimed |= (1u << i);
					sLast[i] = want;
					LOGF("\t PfccT7qURYE temp-sweep: claimed +%u = %u.%03uC\n",
					     kTempOffs[i], want / 1000u, want % 1000u);
				}
			}
		}
		// ps5-vsh boot90: (b) watch a3 range. a3 is a host object (boot60
		// 0xffe3bc8910, boot80 0xffe7729d60), a small-int ID (boot70
		// 0x520f00), or init garbage: read-only 16-word dump of the first 2
		// hits when guest-ranged AND probed, else a logged exclusion (never
		// touch host-only addresses). Zero-slot claim-and-ramp (45-65C, 5C
		// spacing, per-tick drift) only on guest ranges.
		static int      sA3Hits       = 0;
		static uint32_t sA3Last[16]   = {};
		static uint32_t sA3Claimed    = 0;
		static bool     sA3FillLogged = false;
		if (sA3Hits < 2) {
			sA3Hits++;
			if (!IsGuestWatchRange(a3)) {
				LOGF("\t PfccT7qURYE watch-a3 dump #%d: a3=0x%016" PRIx64
				     " NOT guest-ranged (host-object/small-int), no dump, no fill\n",
				     sA3Hits, a3);
			} else if (!ProbeGuest(a3, 64, false)) {
				LOGF("\t PfccT7qURYE watch-a3 dump #%d: a3=0x%016" PRIx64
				     " guest-ranged but unmapped, no dump, no fill\n",
				     sA3Hits, a3);
			} else {
				uint32_t w3[16] = {};
				for (int i = 0; i < 16; i++) {
					if (!ReadGuestU32(a3 + static_cast<uint64_t>(i) * 4u, &w3[i])) {
						w3[i] = 0xdeadbeefu;
					}
				}
				LOGF("\t PfccT7qURYE watch-a3 dump #%d: a3=0x%016" PRIx64
				     " w=[%08x %08x %08x %08x %08x %08x %08x %08x"
				     " %08x %08x %08x %08x %08x %08x %08x %08x]\n",
				     sA3Hits, a3, w3[0], w3[1], w3[2], w3[3], w3[4], w3[5],
				     w3[6], w3[7], w3[8], w3[9], w3[10], w3[11], w3[12],
				     w3[13], w3[14], w3[15]);
			}
		}
		if (IsGuestWatchRange(a3) && ProbeGuest(a3, 64, true)) {
			if (!sA3FillLogged) {
				sA3FillLogged = true;
				LOGF("\t PfccT7qURYE watch-a3: guest range 0x%016" PRIx64
				     ", claim-and-ramp 45-65C\n",
				     a3);
			}
			RampWatchRange(a3, "PfccT7qURYE watch-a3", sA3Last, &sA3Claimed,
			               tick, 1, 5, 16);
		}
		static bool logged = false;
		if (!logged) {
			logged = true;
			LOGF("\t PfccT7qURYE gbase-power fill: cpu=%umW %umV %umA "
			     "temp=%llu.%03lluC (w:%d v:%d a:%d t:%d)\n",
			     cpuMw, cpuMv, cpuMa,
			     static_cast<unsigned long long>(dieMilliC / 1000ull),
			     static_cast<unsigned long long>(dieMilliC % 1000ull), w, v, a,
			     t);
		}
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnknownXj9n(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: Xj9nET9tctI[libkernel] (?). String-ish args; observe + OK.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

struct SysctlStr {
	const char* name;
	const char* value;
};

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SysctlByName(const char* name, void* oldp, size_t* oldlenp,
                                      void* newp, size_t newlen) {
	PRINT_NAME();
	// ps5-vsh: MhC53TKmjVA sysctlbyname (2x, both for "kern.rng_pseudo" per
	// ELF .rodata). BSD discipline: null oldp = size query; writes are
	// capped; unknown names log + ENOENT-ish -1.
	static const SysctlStr kStrings[] = {
	    {"kern.ostype", "Orbis"},
	    {"kern.osrelease", "14.00"},
	    {"kern.version", "Orbis OS 14.00"},
	    {"kern.hostname", "Prospero"},
	    {"hw.model", "Prospero"},
	    {"hw.machine", "x86_64"},
	    {"hw.machine_arch", "x86_64"},
	};
	if (name == nullptr) {
		LOGF("\t null name/oldlenp\n");
		return -1;
	}
	char knob[128];
	if (!ReadGuestStr(reinterpret_cast<uint64_t>(name), knob, sizeof(knob))) {
		LOGF("\t unreadable name ptr 0x%016" PRIx64 "\n",
		     reinterpret_cast<uint64_t>(name));
		return -1;
	}
	if (std::strcmp(knob, "kern.rng_pseudo") == 0) {
		return RngSysctl::Sysctl(oldp, oldlenp, newp, newlen);
	}
	if (oldlenp == nullptr) {
		LOGF("\t null name/oldlenp\n");
		return -1;
	}
	if (newp != nullptr) {
		LOGF("\t %s: set-path ignored (newlen=%llu)\n", knob,
		     static_cast<unsigned long long>(newlen));
	}
	const uint64_t oldp_addr = reinterpret_cast<uint64_t>(oldp);
	const uint64_t len_addr = reinterpret_cast<uint64_t>(oldlenp);
	if (!ProbeGuest(len_addr, sizeof(size_t), true)) {
		LOGF("\t %s: bad oldlenp 0x%016" PRIx64 "\n", knob, len_addr);
		return -1;
	}
	const char* value = nullptr;
	for (const auto& e : kStrings) {
		if (std::strcmp(knob, e.name) == 0) {
			value = e.value;
			break;
		}
	}
	// ps5-vsh: host memory telemetry (ShellCore steady-state polls these;
	// unknown -> guest logs "Cannot get" and degrades). vm.stats vmN
	// v_wire_count is u32 pages; vm.swap_total/avail are u64 bytes.
	// vmN matches any small index (vm0, vm10 seen).
	if (value == nullptr) {
		constexpr size_t kKnobCap = sizeof(knob);
		(void)kKnobCap;
		bool  isWire = false;
		bool  isSwapTotal = false;
		bool  isSwapAvail = false;
		if (std::strncmp(knob, "vm.stats.vm", 11) == 0) {
			const char* tail = knob + 11;
			while (*tail >= '0' && *tail <= '9') {
				tail++;
			}
			if (std::strcmp(tail, ".v_wire_count") == 0) {
				isWire = true;
			}
		}
		if (std::strcmp(knob, "vm.swap_total") == 0) {
			isSwapTotal = true;
		} else if (std::strcmp(knob, "vm.swap_avail") == 0) {
			isSwapAvail = true;
		}
		if (isWire || isSwapTotal || isSwapAvail) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			MEMORYSTATUSEX ms {};
			ms.dwLength = sizeof(ms);
			PERFORMANCE_INFORMATION pi {};
			pi.cb = sizeof(pi);
			uint64_t totalPhys = 8ull << 30;
			uint64_t availPhys = 6ull << 30;
			uint64_t commitTotal = 0;
			uint64_t commitLimit = totalPhys;
			DWORD    page = 4096;
			if (GlobalMemoryStatusEx(&ms) != 0) {
				totalPhys = ms.ullTotalPhys;
				availPhys = ms.ullAvailPhys;
			}
			{
				SYSTEM_INFO si {};
				GetSystemInfo(&si);
				if (si.dwPageSize != 0) {
					page = si.dwPageSize;
				}
			}
			if (GetPerformanceInfo(&pi, sizeof(pi)) != 0 && pi.PageSize != 0) {
				commitTotal = static_cast<uint64_t>(pi.CommitTotal) * pi.PageSize;
				commitLimit = static_cast<uint64_t>(pi.CommitLimit) * pi.PageSize;
				page        = pi.PageSize;
			}
			const uint64_t usedPhys = totalPhys > availPhys ? totalPhys - availPhys : 0;
#else
			const uint64_t totalPhys   = 8ull << 30;
			const uint64_t availPhys   = 6ull << 30;
			const uint64_t usedPhys    = totalPhys - availPhys;
			const uint64_t commitTotal = usedPhys;
			const uint64_t commitLimit = totalPhys;
			const uint64_t page        = 4096;
#endif
			if (isWire) {
				// Kernel-ish wired share of used pages; at least 1 page so
				// zero never reads as "no memory".
				uint32_t wired =
				    static_cast<uint32_t>((usedPhys / page) / 8u + 1u);
				if (oldp == nullptr) {
					*oldlenp = sizeof(wired);
					LOGF("\t %s: size query -> %llu\n", knob,
					     static_cast<unsigned long long>(sizeof(wired)));
					return 0;
				}
				if (*oldlenp < sizeof(wired)) {
					LOGF("\t %s: short buffer %llu\n", knob,
					     static_cast<unsigned long long>(*oldlenp));
					return -1;
				}
				if (!ProbeGuest(oldp_addr, sizeof(wired), true)) {
					return -1;
				}
				std::memcpy(reinterpret_cast<void*>(oldp_addr), &wired,
				            sizeof(wired));
				*oldlenp = sizeof(wired);
				LOGF("\t %s -> %u pages\n", knob, static_cast<unsigned>(wired));
				return 0;
			}
			const uint64_t swapTotal = commitLimit;
			const uint64_t swapAvail =
			    commitLimit > commitTotal ? commitLimit - commitTotal : 0;
			if (isSwapTotal) {
				const uint64_t out = swapTotal;
				if (oldp == nullptr) {
					*oldlenp = sizeof(out);
					LOGF("\t %s: size query -> %llu\n", knob,
					     static_cast<unsigned long long>(sizeof(out)));
					return 0;
				}
				if (*oldlenp < sizeof(out)) {
					LOGF("\t %s: short buffer %llu\n", knob,
					     static_cast<unsigned long long>(*oldlenp));
					return -1;
				}
				if (!ProbeGuest(oldp_addr, sizeof(out), true)) {
					return -1;
				}
				std::memcpy(reinterpret_cast<void*>(oldp_addr), &out, sizeof(out));
				*oldlenp = sizeof(out);
				LOGF("\t %s -> %llu bytes\n", knob,
				     static_cast<unsigned long long>(out));
				return 0;
			}
			// ps5-vsh: vm.swap_avail is read by the guest into a u32 (all
			// observed reads pass oldlen=4), so serve 32-bit page counts
			// rather than the u64 byte total used for swap_total.
			const uint32_t availPages =
			    static_cast<uint32_t>((swapAvail / page) & 0xffffffffu);
			if (oldp == nullptr) {
				*oldlenp = sizeof(availPages);
				LOGF("\t %s: size query -> %llu\n", knob,
				     static_cast<unsigned long long>(sizeof(availPages)));
				return 0;
			}
			if (*oldlenp < sizeof(availPages)) {
				LOGF("\t %s: short buffer %llu\n", knob,
				     static_cast<unsigned long long>(*oldlenp));
				return -1;
			}
			if (!ProbeGuest(oldp_addr, sizeof(availPages), true)) {
				return -1;
			}
			std::memcpy(reinterpret_cast<void*>(oldp_addr), &availPages,
			            sizeof(availPages));
			*oldlenp = sizeof(availPages);
			LOGF("\t %s -> %u pages\n", knob,
			     static_cast<unsigned>(availPages));
			return 0;
		}
	}
	if (value == nullptr) {
		LOGF("\t unknown name \"%s\"\n", knob);
		return -1;
	}
	const size_t need = std::strlen(value) + 1;
	if (oldp == nullptr) {
		*oldlenp = need;
		LOGF("\t %s: size query -> %llu\n", knob,
		     static_cast<unsigned long long>(need));
		return 0;
	}
	size_t n = *oldlenp;
	if (n > need) {
		n = need;
	}
	if (!WriteGuestBytes(oldp_addr, value, n)) {
		LOGF("\t %s: bad buffer 0x%016" PRIx64 "\n", knob, oldp_addr);
		return -1;
	}
	*oldlenp = n;
	LOGF("\t %s -> \"%s\" (%llu bytes)\n", knob, value,
	     static_cast<unsigned long long>(n));
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI QueryInternalLibcDiagnosticMode() {
	// 13.60 libkernel+1ee20 (Q-8duzyUNn0): return the queried int on
	// success, otherwise propagate sysctlbyname's raw result. In particular,
	// an unavailable OID must not become zero and suppress native diagnostics.
	int value = 0;
	size_t length = sizeof(value);
	const int result = SysctlByName("machdep.rcmgr_intdev_for_internal_libc",
	                               &value, &length, nullptr, 0);
	return result == 0 ? value : result;
}

LIB_DEFINE(InitShellHitsKernel_1) {
	LIB_FUNC("yu17wG8L5FI", Kernel::HasTrinityMode);   // sceKernelHasTrinityMode
	LIB_FUNC("MhC53TKmjVA", Kernel::SysctlByName);     // sysctlbyname
	LIB_FUNC("Q-8duzyUNn0", Kernel::QueryInternalLibcDiagnosticMode);
	LIB_FUNC("QNjGUdj1HPM", Kernel::IsDevKit);          // sceKernelIsDevKit
	LIB_FUNC("Ht23vmmuP0Q", Kernel::IccSetBootTimestamp); // sceKernelIccSetBootTimestamp
	LIB_FUNC("PfccT7qURYE", Kernel::Ioctl);            // ioctl
	LIB_FUNC("X0HkB92+NRE", Kernel::IsAuthenticTrinity); // sceKernelIsAuthenticTrinity
	LIB_FUNC("wXmx+vN8puE", Kernel::IccGetPowerUpCause); // sceKernelIccGetPowerUpCause
	LIB_FUNC("Xj9nET9tctI", Kernel::UnknownXj9n);       // unknown libkernel stub
}

} // namespace Kernel

namespace Dipsw {

LIB_VERSION("Dipsw", 1, "Dipsw", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IsAssistMode() {
	PRINT_NAME();
	// ps5-vsh: PKg59uLpHNI sceKernelIsAssistMode (3x). Retail: no assist.
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IsDevelopmentMode() {
	PRINT_NAME();
	// ps5-vsh: UtO0OHMCgmI sceKernelIsDevelopmentMode (4x). Retail: never.
	return 0;
}

LIB_DEFINE(InitShellHitsDipsw_1) {
	LIB_FUNC("PKg59uLpHNI", Dipsw::IsAssistMode);       // sceKernelIsAssistMode
	LIB_FUNC("UtO0OHMCgmI", Dipsw::IsDevelopmentMode);  // sceKernelIsDevelopmentMode
}

} // namespace Dipsw

namespace Ipmi {

LIB_DEFINE(InitShellHitsIpmi_1) {
	// The verified Config constructor is registered by InitIpmiConfig_1.
	// Do not override it with the former log-only floor implementation.
}

} // namespace Ipmi

namespace AvSetting {

LIB_VERSION("AvSetting", 1, "AvSetting", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AvSettingInit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	// ps5-vsh: EIZdWTT7Zdw sceAvSettingInit. Nothing to configure yet.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AvControlAddCallbacksForLnc(uint64_t a0, uint64_t a1, uint64_t a2,
                                                     uint64_t a3) {
	PRINT_NAME();
	// ps5-vsh: Im9QXnr0QK4 sceAvControlAddCallbacksForLnc. Table only, never
	// fired in emulation.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnknownA143(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: A143jTEYkNw[AvSetting] (?). Observe + OK.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnknownS2OD(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: s2ODJNcQVkI[AvSetting] (?). Observe + OK.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsAvSetting_1) {
	LIB_FUNC("EIZdWTT7Zdw", AvSetting::AvSettingInit);               // sceAvSettingInit
	LIB_FUNC("Im9QXnr0QK4", AvSetting::AvControlAddCallbacksForLnc);  // AvControlAddCallbacksForLnc
	LIB_FUNC("A143jTEYkNw", AvSetting::UnknownA143);                  // unknown AvSetting stub
	LIB_FUNC("s2ODJNcQVkI", AvSetting::UnknownS2OD);                  // unknown AvSetting stub
}

} // namespace AvSetting

namespace FsInternalForVsh {

LIB_VERSION("FsInternalForVsh", 1, "FsInternalForVsh", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnknownQkzP(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: QkzPx-QbX3A[FsInternalForVsh] (?). Observe + OK.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsFsVsh_1) {
	LIB_FUNC("QkzPx-QbX3A", FsInternalForVsh::UnknownQkzP); // unknown FsInternalForVsh stub
}

} // namespace FsInternalForVsh

namespace AppMessaging {

LIB_VERSION("AppMessaging", 1, "SystemService", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unknown90un(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 90unWbnI0qE[AppMessaging/SystemService] Func_F74BA759B9C8D2A1.
	// Guest key is (AppMessaging, SystemService) — that is why the old
	// (AppMessaging, AppMessaging) LogStub only ever caught it via NID
	// fallback. Observe + OK; no writes (layout unknown).
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsAppMsg_1) {
	LIB_FUNC("90unWbnI0qE", AppMessaging::Unknown90un); // Func_F74BA759B9C8D2A1
}

} // namespace AppMessaging

namespace SysCore {

LIB_VERSION("SysCore", 1, "SysCore", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AppInitForShellCore(uint64_t a0, uint64_t a1, uint64_t a2,
                                             uint64_t a3) {
	PRINT_NAME();
	// ps5-vsh: 9PCBPZcDU7Q sceApplicationInitializeForShellCore.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3);
	return 0;
}

LIB_DEFINE(InitShellHitsSysCore_1) {
	LIB_FUNC("9PCBPZcDU7Q", SysCore::AppInitForShellCore); // sceApplicationInitializeForShellCore
}

} // namespace SysCore

// Batch 2 (shellcore_boot10.log tail, lines 24673-25862; 24 calls, 22 distinct
// NIDs, NpEnv -> PsnCore era). Same override discipline as batch 1: every NID
// below is currently a generic LogStub under the identical exact key in
// libC.cpp; these registrations run last (InitShellHits_1 is the final call in
// Libs::InitAll) so insert_or_assign lets the real behavior win. Exact keys
// are taken from the boot10 Relocate lines (e.g. jri3LyXhEGw resolves as
// LibcInternal_v1, yODgcWKSa8Q as NpCommon_v1), NOT from the imports Library
// column alone. VrTracker2 1DwROxtuANs is deliberately absent: it is only
// load-relocated in boot10, never CALLED.

namespace LibcInternalLate {

// ps5-vsh diagnostic: print the guest callers of this HLE entry by scanning
// the near stack for return addresses into the calling module. Informative
// only; reads are SEH-guarded.
static void ScanGuestCallers(const char* tag, const void* stream) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const uint64_t ret = static_cast<uint64_t>(
	    reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	if (rt == nullptr) {
		return;
	}
	const auto* prog = rt->FindProgramByAddr(ret);
	if (prog == nullptr || prog->base_size == 0) {
		return;
	}
	LOGF("ps5-vsh: %s stream=%p ret=%s+0x%llx\n", tag, stream,
	     prog->file_name.filename().string().c_str(),
	     static_cast<unsigned long long>(ret - prog->base_vaddr));
	const auto* slot = static_cast<const uint64_t*>(_AddressOfReturnAddress());
	for (int i = 0; i < 512; i++) {
		// Stop at the readable stack boundary before touching the next page.
		// GPU fault dispatch can run before SEH; diagnostics must not fault
		// into an adjacent guest reservation while scanning return addresses.
		if (!ProbeGuest(reinterpret_cast<uint64_t>(slot + i), sizeof(uint64_t), false)) {
			break;
		}
		uint64_t v = 0;
		__try {
			v = slot[i];
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			break;
		}
		if (v < prog->base_vaddr || v >= prog->base_vaddr + prog->base_size) {
			continue;
		}
		LOGF("ps5-vsh:   +0x%llx\n", static_cast<unsigned long long>(v - prog->base_vaddr));
	}
#else
	(void)tag;
	(void)stream;
#endif
}

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Fiopen(const char* path, uint64_t flags, uint64_t mode,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: lEeJadNvMbo std::__sce_v2::_Fiopen(path, flags, mode).
	// Opens for real through the guest-FILE shim (JSCd reads
	// //common/cert/CA_LIST.cer through here); NULL only when the file
	// truly cannot be opened. fread/fclose route to the same shims.
	char buf[256];
	if (!ReadGuestStr(reinterpret_cast<uint64_t>(path), buf, sizeof(buf))) {
		LOGF("\t unreadable path 0x%016" PRIx64 " flags=%llu mode=0x%llx -> NULL\n",
		     reinterpret_cast<uint64_t>(path),
		     static_cast<unsigned long long>(flags),
		     static_cast<unsigned long long>(mode));
		(void)a3;
		(void)a4;
		(void)a5;
		return nullptr;
	}
	(void)mode;
	(void)a3;
	(void)a4;
	(void)a5;
	void* shim = Libs::GuestFile::Open(buf, static_cast<int>(flags));
	// ps5-vsh: the real _Fiopen (libSceLibcInternal 0x11d7b0) picks the fopen
	// mode string from the openmode bits, then - when bit 0x04 is set - seeks
	// the fresh handle to SEEK_END and fails the whole open if that seek does.
	// std::filebuf::open passes ios::ate for the RNPS bundle reader, which
	// immediately takes tellg() as the file size; without this seek the size
	// came back 0 and the bundle builder never read the container at all.
	if (shim != nullptr && (flags & 4) != 0) {
		if (Libs::GuestFile::Seek(shim, 0, SEEK_END) != 0) {
			Libs::GuestFile::Close(shim);
			shim = nullptr;
		}
	}
	LOGF("\t \"%s\" flags=%llu -> %p (ret=0x%016" PRIx64 ")\n", buf,
	     static_cast<unsigned long long>(flags), shim,
	     static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
	if (std::strstr(buf, ".ps.bundle") != nullptr) {
		ScanGuestCallers("fiopen-callers", shim);
	}
	return shim;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IosBaseDtor(void* self) {
	PRINT_NAME();
	// ps5-vsh: Ze2I81YNykw std ios_base::_Ios_base_dtor. Log-only; the
	// object lives and dies on the guest stack (see Fiopen note above).
	if (self == nullptr) {
		LOGF("\t null this\n");
		return;
	}
	LOGF("\t this 0x%016" PRIx64 ": log-only\n", reinterpret_cast<uint64_t>(self));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MakeDir(uint64_t p0, uint64_t p1, uint64_t p2, uint64_t a3,
                                 uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: H7pVgeH6+0Y std::filesystem::_Make_dir (2x). Bool-ish: 0
	// keeps the boot10-proven path (guest tolerates the failure; its
	// SceMkdirWorker path handles creation). Log the path when readable.
	char buf[256];
	if (p0 != 0 && ReadGuestStr(p0, buf, sizeof(buf))) {
		LOGF("\t \"%s\" p1=0x%016" PRIx64 " p2=0x%016" PRIx64 " -> 0\n", buf, p1, p2);
	} else {
		LOGF("\t p0 = 0x%016" PRIx64 " p1 = 0x%016" PRIx64 " p2 = 0x%016" PRIx64
		     " -> 0\n",
		     p0, p1, p2);
	}
	(void)a3;
	(void)a4;
	(void)a5;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LibreSslInitEx(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: jri3LyXhEGw sceLibreSslInitEx (guest imports it as
	// LibcInternal_v1 per the Relocate line). Observed (0x1a000, 0x1a000,
	// 0, ...): pool-ish sizes. Nothing to seed yet; init ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsLibcLate_1) {
	LIB_FUNC("lEeJadNvMbo", LibcInternalLate::Fiopen);         // std _Fiopen
	LIB_FUNC("Ze2I81YNykw", LibcInternalLate::IosBaseDtor);    // ios_base dtor
	LIB_FUNC("H7pVgeH6+0Y", LibcInternalLate::MakeDir);         // filesystem _Make_dir
	LIB_FUNC("jri3LyXhEGw", LibcInternalLate::LibreSslInitEx);  // sceLibreSslInitEx
}

} // namespace LibcInternalLate

namespace KernelLate {

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UnknownWLC9(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: WLC94v0XY+0[libkernel] (?). Stack-to-stack flavor
	// (dst, src, 0x79, dst+5, src+5, id?) but semantics unknown: observe +
	// OK, NO writes (layout unknown).
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetGenuineSwVersion(uint64_t out, uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: IrjEm08+qoY sceKernelGetGenuineProsperoSystemSwVersion(out).
	// Data-filler: a bounded 4-byte plausible version (13.60) with a
	// writability probe, sysctlbyname-style. Unknown struct size, so nothing
	// more: the LogStub wrote nothing and boot continued.
	static const uint32_t kVer = 0x0D3C0000u; // 13.60
	if (out == 0) {
		LOGF("\t null out-param\n");
		(void)a1;
		(void)a2;
		(void)a3;
		(void)a4;
		(void)a5;
		return -1;
	}
	if (!WriteGuestBytes(out, &kVer, sizeof(kVer))) {
		LOGF("\t unwritable out-param 0x%016" PRIx64 "\n", out);
		(void)a1;
		(void)a2;
		(void)a3;
		(void)a4;
		(void)a5;
		return -1;
	}
	LOGF("\t out 0x%016" PRIx64 " <- 0x%08x\n", out, kVer);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;
	return 0;
}

LIB_DEFINE(InitShellHitsKernelLate_1) {
	LIB_FUNC("WLC94v0XY+0", KernelLate::UnknownWLC9);           // unknown libkernel stub
	LIB_FUNC("IrjEm08+qoY", KernelLate::GetGenuineSwVersion);   // GetGenuineProsperoSystemSwVersion
}

} // namespace KernelLate

namespace RegMgrLate {

LIB_VERSION("RegMgr", 1, "RegMgr", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetVersion(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: q+lPgqZniW4 sceRegMgrGetVersion. Scalar query: a0-a3 are
	// small integers (0x8000/0x2bc pairs, no out-pointer), so the version
	// comes back as the return value. Plausible data: FW 13.60.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64
	     " -> 0x0D3C0000\n",
	     a0, a1, a2, a3, a4, a5);
	return 0x0D3C0000;
}

LIB_DEFINE(InitShellHitsRegMgrLate_1) {
	LIB_FUNC("q+lPgqZniW4", RegMgrLate::GetVersion); // sceRegMgrGetVersion
}

} // namespace RegMgrLate

namespace NetLate {

LIB_VERSION("Net", 1, "Net", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI BandwidthControlSetDefaultParam(uint64_t a0, uint64_t a1,
                                                         uint64_t a2, uint64_t a3,
                                                         uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: P4zZXE7bpsA sceNetBandwidthControlSetDefaultParam. Setter
	// ack; emulation has no shaperorrection to configure.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ConfigUpInterface(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: oGEBX0eXGFs sceNetConfigUpInterface. The loopback/ifaddr
	// overlay (NetIfaddr precedent) already answers; bringing the iface up
	// is a no-op success here.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsNetLate_1) {
	LIB_FUNC("P4zZXE7bpsA", NetLate::BandwidthControlSetDefaultParam); // BandwidthControlSetDefaultParam
	LIB_FUNC("oGEBX0eXGFs", NetLate::ConfigUpInterface);               // NetConfigUpInterface
}

} // namespace NetLate

namespace NetCtlLate {

LIB_VERSION("NetCtl", 1, "NetCtl", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Unknown2Ny2(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 2Ny2lzU3o9w[NetCtl] Func_D8DCB6973537A3DC (?). Observe + OK.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsNetCtlLate_1) {
	LIB_FUNC("2Ny2lzU3o9w", NetCtlLate::Unknown2Ny2); // unknown NetCtl stub
}

} // namespace NetCtlLate

namespace HttpCacheLate {

LIB_VERSION("HttpCache", 1, "HttpCache", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CacheSystemInit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: LfOVpdFit+Q sceHttpCacheSystemInit. The cache.db fcntl/lock
	// traffic later in the log shows the FS overlay serves it; init ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsHttpCacheLate_1) {
	LIB_FUNC("LfOVpdFit+Q", HttpCacheLate::CacheSystemInit); // sceHttpCacheSystemInit
}

} // namespace HttpCacheLate

namespace IpmiLate {

LIB_VERSION("Ipmi", 1, "Ipmi", 1, 1);

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ServerEventHandlerDtor(void* self) {
	PRINT_NAME();
	// ps5-vsh: Z5i0--Vqfwg IPMI::Server::EventHandler::~EventHandler().
	// Log-only; nothing allocated host-side to release.
	if (self == nullptr) {
		LOGF("\t null this\n");
		return;
	}
	LOGF("\t this 0x%016" PRIx64 ": log-only\n", reinterpret_cast<uint64_t>(self));
}

LIB_DEFINE(InitShellHitsIpmiLate_1) {
	LIB_FUNC("O1lQ2+do5r4", IpmiClientAbi::Construct);
	LIB_FUNC("Rih7HnIM3JQ", IpmiClientAbi::Estimate);
	LIB_FUNC("Z5i0--Vqfwg", IpmiLate::ServerEventHandlerDtor);    // Server::EventHandler dtor
}

} // namespace IpmiLate

namespace HttpLate {

LIB_VERSION("Http", 1, "Http", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpsEnableOptionPrivate(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: I4+4hKttt1w sceHttpsEnableOptionPrivate. Option setter ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpsDisableOptionPrivate(uint64_t a0, uint64_t a1, uint64_t a2,
                                                   uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: zJYi5br6ZiQ sceHttpsDisableOptionPrivate. Option setter ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsHttpLate_1) {
	LIB_FUNC("I4+4hKttt1w", HttpLate::HttpsEnableOptionPrivate);   // HttpsEnableOptionPrivate
	LIB_FUNC("zJYi5br6ZiQ", HttpLate::HttpsDisableOptionPrivate);  // HttpsDisableOptionPrivate
}

} // namespace HttpLate

namespace NpCommonLate {

LIB_VERSION("NpCommon", 1, "NpCommon", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientExtTerminate(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: yODgcWKSa8Q sceNpAsmClientExtTerminate (guest imports it as
	// NpCommon_v1 per the Relocate line). Terminate ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI WorkQueueCtor(void* self) {
	PRINT_NAME();
	// ps5-vsh: X6NVkdpRnog sce::np::WorkQueue::WorkQueue(). Minimal-touch
	// ctor: log-only, NO guest writes (object size unknown; Config-64B
	// canary lesson). WorkQueue methods below are ack-only likewise.
	if (self == nullptr) {
		LOGF("\t null this\n");
		return;
	}
	LOGF("\t WorkQueue this 0x%016" PRIx64 ": log-only\n",
	     reinterpret_cast<uint64_t>(self));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI WorkQueueCancelAll(uint64_t a0, uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: zmOmSLnqlBQ sce::np::WorkQueue::CancelAll.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI WorkQueueStop(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 4DE+nnCVRPA sce::np::WorkQueue::Stop.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI WorkQueueDestroy(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: NeopmYshD0U sce::np::WorkQueue::Destroy.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsNpLate_1) {
	LIB_FUNC("jGF+MaB4b-M", NpGlobalMemory::ArchInit);
	LIB_FUNC("yODgcWKSa8Q", NpCommonLate::AsmClientExtTerminate);  // sceNpAsmClientExtTerminate
	LIB_FUNC("X6NVkdpRnog", NpCommonLate::WorkQueueCtor);          // WorkQueue::WorkQueue
	LIB_FUNC("zmOmSLnqlBQ", NpCommonLate::WorkQueueCancelAll);     // WorkQueue::CancelAll
	LIB_FUNC("4DE+nnCVRPA", NpCommonLate::WorkQueueStop);          // WorkQueue::Stop
	LIB_FUNC("NeopmYshD0U", NpCommonLate::WorkQueueDestroy);       // WorkQueue::Destroy
}

} // namespace NpCommonLate

// Batch 3 (shellcore_boot11.log; 630 calls, 30 distinct NIDs). Same override
// discipline as batches 1-2: every NID below is currently a generic LogStub;
// these registrations run last so insert_or_assign lets the real behavior win.
// Exact keys are the guest's REQUEST keys from SceShellCore.elf dynamic tables
// (DT_OS_IMPORT_LIB/MODULE + NID#libid#modid, EncodeId64-matched), NOT the
// provider keys shown in Relocate resolved names. E.g. WtR7OVvUgSo#Z#X is
// (libkernel v1, libkernel v1.1) while its LogStub lives under
// (LibcInternal, LibcInternal); pQiLz/DFmMT same; jri3LyXhEGw#h#e is
// (LibreSSL v1, LibreSSL v1.1); yODgcWKSa8Q#T#S is (NpManager v1,
// NpManager v1.1); LfOVpdFit+Q#b74k6s#P#P is (HttpCache v1, HttpCache v1.0)
// (mod MINOR 0, not 1.1); ivMCitpSQNk/ehN1Biw3tw8#a#Y is (Json2 v1,
// Json v1.1) (module Json, not Json2). Top 25 by count only; the bottom-5
// singletons (rho9DH-0ehs, +isUKw4zud4, b74k6s-1WzU, Ujf3KzMvRmI,
// opMiSfTJyJo) stay on LogStubs. Canary rule: ProbeGuest + minimal fills only,
// never bulk guest-stack writes; null-safe everywhere.

namespace VrTracker2Boot11 {

LIB_VERSION("VrTracker2", 1, "VrTracker2", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetPlayAreaInformation(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 1DwROxtuANs sceVrTracker2GetPlayAreaInformation (429x, HOT:
	// polled in Sl2/TtsArb timer loop). Timer-polled getter: stable 0/empty
	// so the poll loop idles instead of erroring. Only 8-byte validated
	// zeroes, never bulk.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	const uint64_t zero = 0;
	if (a0 != 0 && ProbeGuest(a0, sizeof(zero), true)) {
		WriteGuestBytes(a0, &zero, sizeof(zero));
	}
	if (a1 != 0 && ProbeGuest(a1, sizeof(zero), true)) {
		WriteGuestBytes(a1, &zero, sizeof(zero));
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Initialize(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 6Jy73SRfG-o sceVrTracker2Initialize. Init ack, no state.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI QueryMemory(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: TwqZnaIjWv4 sceVrTracker2QueryMemory. Size query: validated
	// 8-byte zeroes only.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	const uint64_t zero = 0;
	if (a0 != 0 && ProbeGuest(a0, sizeof(zero), true)) {
		WriteGuestBytes(a0, &zero, sizeof(zero));
	}
	if (a1 != 0 && ProbeGuest(a1, sizeof(zero), true)) {
		WriteGuestBytes(a1, &zero, sizeof(zero));
	}
	return 0;
}

LIB_DEFINE(InitShellHitsVrTracker2Boot11_1) {
	LIB_FUNC("1DwROxtuANs", VrTracker2Boot11::GetPlayAreaInformation); // GetPlayAreaInformation
	LIB_FUNC("6Jy73SRfG-o", VrTracker2Boot11::Initialize);              // Initialize
	LIB_FUNC("TwqZnaIjWv4", VrTracker2Boot11::QueryMemory);             // QueryMemory
}

} // namespace VrTracker2Boot11

namespace KernelBoot11 {

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

// Host monotonic seconds for sensor ramps, independent of kern.rng_pseudo.
static uint64_t SensorTickSec() {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(
	        std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetPageTableStats(uint64_t a0, uint64_t a1, uint64_t a2,
                                           uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: WtR7OVvUgSo get_page_table_stats (42x; 0, idx, out, out).
	// Validated 8-byte zeroes only.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	const uint64_t zero = 0;
	if (a2 != 0 && ProbeGuest(a2, sizeof(zero), true)) {
		WriteGuestBytes(a2, &zero, sizeof(zero));
	}
	if (a3 != 0 && ProbeGuest(a3, sizeof(zero), true)) {
		WriteGuestBytes(a3, &zero, sizeof(zero));
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelHeapUsage2(uint64_t a0, uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: pQiLz-exzR4 sceKernelKernelHeapUsage2 (21x; out, out).
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	const uint64_t zero = 0;
	if (a0 != 0 && ProbeGuest(a0, sizeof(zero), true)) {
		WriteGuestBytes(a0, &zero, sizeof(zero));
	}
	if (a1 != 0 && ProbeGuest(a1, sizeof(zero), true)) {
		WriteGuestBytes(a1, &zero, sizeof(zero));
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetModuleInfo(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: kUpgrXIrz7Q sceKernelGetModuleInfo (11x; 1, out, out).
	// Struct size unknown: observe + OK, NO writes (canary lesson).
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetCurrentFanDuty(uint64_t a0, uint64_t a1, uint64_t a2,
                                           uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 7siOfIbbg8I sceKernelGetCurrentFanDuty. SENSOR: the old
	// constant-zero fill stalls any controller regulating on it (boot16
	// PowerWatch pins at 0.0W/0MHz on zero-fed telemetry). Plausible
	// nonzero-varying duty in percent: idle-console ~30%, stepping +1pp
	// every 4 host seconds through 30..35, then wrapping (monotonic ramp,
	// host-tick derived). Only validated 4-byte probed writes, null-safe.
	const uint32_t duty = 30u + static_cast<uint32_t>((SensorTickSec() / 4u) % 6u);
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64
	     " -> fanDuty=%u%%\n",
	     a0, a1, a2, a3, a4, a5, duty);
	if (a0 != 0 && ProbeGuest(a0, sizeof(duty), true)) {
		WriteGuestBytes(a0, &duty, sizeof(duty));
	}
	if (a1 != 0 && ProbeGuest(a1, sizeof(duty), true)) {
		WriteGuestBytes(a1, &duty, sizeof(duty));
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetSocPowerConsumption(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: CA5jcDb2vYI sceKernelGetSocPowerConsumption. SENSOR: same
	// constant-zero stall as fanDuty (boot16: 0.0W everywhere). Plausible
	// nonzero-varying milliwatts: 42000mW + a 1-second 7mW-step ramp
	// wrapping every ~21min through 42000..50999 (42..51W, idle-to-busy
	// console envelope, host-tick derived). Only validated 4-byte probed
	// writes, null-safe.
	const uint32_t mw = 42000u + static_cast<uint32_t>((SensorTickSec() * 7u) % 9000u);
	const uint64_t tickSec = SensorTickSec(); // boot90: drives a4/a0t ramps below
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64
	     " -> socPower=%umW\n",
	     a0, a1, a2, a3, a4, a5, mw);
	if (a0 != 0 && ProbeGuest(a0, sizeof(mw), true)) {
		WriteGuestBytes(a0, &mw, sizeof(mw));
		// ps5-vsh boot40: the guest prints per-part (W, V, A[, MHz]) but only
		// W ever moves — V/A/MHz slots stay zero (GFX shows 0.000V/0.000A/0MHz
		// beside live watts). Fill them with coherent host-tick ramps via
		// validated zero-slot writes (never clobber live words). First-use
		// LOGF only; the per-call socPower line above is untouched.
		const uint64_t tick = SensorTickSec();
		const uint32_t mv = 800u + static_cast<uint32_t>((tick * 11u) % 101u);
		const uint32_t ma = mw * 1000u / (mv != 0 ? mv : 1u);
		const uint32_t mhz = 1900u + static_cast<uint32_t>((tick * 29u) % 301u);
		const bool v = WriteGuestU32ZeroSlot(a0 + 4, mv);
		const bool c = WriteGuestU32ZeroSlot(a0 + 8, ma);
		const bool z = WriteGuestU32ZeroSlot(a0 + 12, mhz);
		static bool logged = false;
		if (!logged) {
			logged = true;
			LOGF("\t CA5jcDb2vYI rail fill: %umV %umA %uMHz (v:%d a:%d z:%d)\n",
			     mv, ma, mhz, v, c, z);
		}
	}
	if (a1 != 0 && ProbeGuest(a1, sizeof(mw), true)) {
		WriteGuestBytes(a1, &mw, sizeof(mw));
	}
	// ps5-vsh boot90: (a) CA5 second out-param a4 range. Early calls carry
	// guest a4 (0x7ef1cfdb8); watch calls carry guest a4 (boot60
	// 0x7ecdbfcf8) or small-int channel IDs (boot70/80 0x52xxxx/0x42xxxx).
	// Read-only 16-word dump of the first 4 hits when guest-ranged AND
	// probed, else a logged skip. Zero-slot claim-and-ramp (45-65C, 1C
	// spacing, tick/4 drift) on guest ranges only. Pivot: CA5 a0 +16..+36
	// temp sweep (2C spacing, tick/8 drift) — the other per-cycle zero-slot
	// structure in the watch path (a0+12 MHz slot is guest-ignored, so the
	// rail struct may extend past +12 with the temp word).
	static int      sA4Hits       = 0;
	static uint32_t sA4Last[16]   = {};
	static uint32_t sA4Claimed    = 0;
	static bool     sA4FillLogged = false;
	static uint32_t sA0tLast[6]   = {};
	static uint32_t sA0tClaimed   = 0;
	static bool     sA0tFillLogged = false;
	if (sA4Hits < 4) {
		sA4Hits++;
		if (!IsGuestWatchRange(a4)) {
			LOGF("\t CA5jcDb2vYI a4 dump #%d: a0=0x%016" PRIx64 " a4=0x%016" PRIx64
			     " NOT guest-ranged (small-int/host), no dump, no fill\n",
			     sA4Hits, a0, a4);
		} else if (!ProbeGuest(a4, 64, false)) {
			LOGF("\t CA5jcDb2vYI a4 dump #%d: a0=0x%016" PRIx64 " a4=0x%016" PRIx64
			     " guest-ranged but unmapped, no dump, no fill\n",
			     sA4Hits, a0, a4);
		} else {
			uint32_t w4[16] = {};
			for (int i = 0; i < 16; i++) {
				if (!ReadGuestU32(a4 + static_cast<uint64_t>(i) * 4u, &w4[i])) {
					w4[i] = 0xdeadbeefu;
				}
			}
			LOGF("\t CA5jcDb2vYI a4 dump #%d: a0=0x%016" PRIx64 " a4=0x%016" PRIx64
			     " w=[%08x %08x %08x %08x %08x %08x %08x %08x"
			     " %08x %08x %08x %08x %08x %08x %08x %08x]\n",
			     sA4Hits, a0, a4, w4[0], w4[1], w4[2], w4[3], w4[4], w4[5],
			     w4[6], w4[7], w4[8], w4[9], w4[10], w4[11], w4[12], w4[13],
			     w4[14], w4[15]);
		}
	}
	if (IsGuestWatchRange(a4) && ProbeGuest(a4, 64, true)) {
		if (!sA4FillLogged) {
			sA4FillLogged = true;
			LOGF("\t CA5jcDb2vYI a4: guest range 0x%016" PRIx64
			     ", claim-and-ramp 45-65C\n",
			     a4);
		}
		RampWatchRange(a4, "CA5jcDb2vYI a4", sA4Last, &sA4Claimed, tickSec, 4,
		               1, 16);
	}
	if (a0 != 0 && ProbeGuest(a0, 40, true)) {
		if (!sA0tFillLogged) {
			sA0tFillLogged = true;
			LOGF("\t CA5jcDb2vYI a0t: pivot sweep +16..+36 on 0x%016" PRIx64 "\n",
			     a0);
		}
		RampWatchRange(a0 + 16, "CA5jcDb2vYI a0t", sA0tLast, &sA0tClaimed,
		               tickSec, 8, 2, 6);
	}
	// ps5-vsh boot110: (b) CA5 watch-struct tail a0+40..+80. a0+0..+36 feed
	// total/GFX/CPU W/V/A live (boot90/100 prints), temp still 0.3C, so the
	// feeder is past +36 (struct size unknown; watch a5=0x80 once, small
	// after). Read-only 16-word dump of the first 4 hits at a0+40 (covers
	// +40..+100, includes the +40..+80 window), logged BEFORE any fill.
	// Zero-slot claim-and-ramp (45-65C, div 16 / spacing 4: distinct drift
	// so a moved print identifies this range) on +40..+80 only (11 slots).
	static int      sA0xHits       = 0;
	static uint32_t sA0xLast[16]   = {};
	static uint32_t sA0xClaimed    = 0;
	static bool     sA0xFillLogged = false;
	if (sA0xHits < 4 && a0 != 0) {
		sA0xHits++;
		if (!ProbeGuest(a0 + 40, 64, false)) {
			LOGF("\t CA5jcDb2vYI a0x dump #%d: a0=0x%016" PRIx64
			     " a5=0x%016" PRIx64 " +40 unreadable, no dump, no fill\n",
			     sA0xHits, a0, a5);
		} else {
			uint32_t wx[16] = {};
			for (int i = 0; i < 16; i++) {
				if (!ReadGuestU32(a0 + 40 + static_cast<uint64_t>(i) * 4u, &wx[i])) {
					wx[i] = 0xdeadbeefu;
				}
			}
			LOGF("\t CA5jcDb2vYI a0x dump #%d: a0=0x%016" PRIx64 " a5=0x%016" PRIx64
			     " w+40=[%08x %08x %08x %08x %08x %08x %08x %08x"
			     " %08x %08x %08x %08x %08x %08x %08x %08x]\n",
			     sA0xHits, a0, a5, wx[0], wx[1], wx[2], wx[3], wx[4], wx[5],
			     wx[6], wx[7], wx[8], wx[9], wx[10], wx[11], wx[12], wx[13],
			     wx[14], wx[15]);
		}
	}
	if (a0 != 0 && ProbeGuest(a0 + 40, 44, true)) {
		if (!sA0xFillLogged) {
			sA0xFillLogged = true;
			LOGF("\t CA5jcDb2vYI a0x: tail sweep +40..+80 on 0x%016" PRIx64 "\n",
			     a0);
		}
		RampWatchRange(a0 + 40, "CA5jcDb2vYI a0x", sA0xLast, &sA0xClaimed,
		               tickSec, 16, 4, 11);
	}
	// ps5-vsh boot110: (c) rotating host a1 pool (watch calls carry
	// a1=0xffe29xxxx, early calls a small-int). Gate verdict per hit: the
	// IsGuestWatchRange window (guest stacks) plus ProbeGuest read/write.
	// Host-pool addresses get a logged exclusion (never dumped, never
	// filled); a guest-ranged a1 would get a read-only dump + the same
	// claim-and-ramp treatment (div 32 / spacing 6, 16 slots).
	static int sA1Hits = 0;
	if (sA1Hits < 6) {
		sA1Hits++;
		const bool inGuest = IsGuestWatchRange(a1);
		const bool canRead = ProbeGuest(a1, 64, false);
		const bool canWrite = ProbeGuest(a1, 64, true);
		if (!inGuest) {
			LOGF("\t CA5jcDb2vYI a1 probe #%d: a1=0x%016" PRIx64
			     " NOT guest-ranged (host pool/small-int, read=%d write=%d),"
			     " no dump, no fill\n",
			     sA1Hits, a1, canRead ? 1 : 0, canWrite ? 1 : 0);
		} else if (!canRead) {
			LOGF("\t CA5jcDb2vYI a1 probe #%d: a1=0x%016" PRIx64
			     " guest-ranged but unmapped, no dump, no fill\n",
			     sA1Hits, a1);
		} else {
			uint32_t w1[16] = {};
			for (int i = 0; i < 16; i++) {
				if (!ReadGuestU32(a1 + static_cast<uint64_t>(i) * 4u, &w1[i])) {
					w1[i] = 0xdeadbeefu;
				}
			}
			LOGF("\t CA5jcDb2vYI a1 dump #%d: a1=0x%016" PRIx64
			     " w=[%08x %08x %08x %08x %08x %08x %08x %08x"
			     " %08x %08x %08x %08x %08x %08x %08x %08x]\n",
			     sA1Hits, a1, w1[0], w1[1], w1[2], w1[3], w1[4], w1[5],
			     w1[6], w1[7], w1[8], w1[9], w1[10], w1[11], w1[12],
			     w1[13], w1[14], w1[15]);
		}
	}
	static uint32_t sA1Last[16]   = {};
	static uint32_t sA1Claimed    = 0;
	static bool     sA1FillLogged = false;
	if (IsGuestWatchRange(a1) && ProbeGuest(a1, 64, true)) {
		if (!sA1FillLogged) {
			sA1FillLogged = true;
			LOGF("\t CA5jcDb2vYI a1: guest range 0x%016" PRIx64
			     ", claim-and-ramp 45-65C\n",
			     a1);
		}
		RampWatchRange(a1, "CA5jcDb2vYI a1", sA1Last, &sA1Claimed, tickSec,
		               32, 6, 16);
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SetIoreq(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: Ee3LPFXEvA0 sceKernelSetIoreq. Setter ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetIoreq(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: Ww2Gpga45Zc sceKernelGetIoreq. Layout unknown: observe +
	// OK, NO writes.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsKernelBoot11_1) {
	LIB_FUNC("WtR7OVvUgSo", KernelBoot11::GetPageTableStats);      // get_page_table_stats
	LIB_FUNC("pQiLz-exzR4", KernelBoot11::KernelHeapUsage2);        // sceKernelKernelHeapUsage2
	LIB_FUNC("DFmMT80xcNI", RngSysctl::NumericSysctl);              // sysctl
	LIB_FUNC("kUpgrXIrz7Q", KernelBoot11::GetModuleInfo);           // sceKernelGetModuleInfo
	LIB_FUNC("7siOfIbbg8I", KernelBoot11::GetCurrentFanDuty);       // sceKernelGetCurrentFanDuty
	LIB_FUNC("CA5jcDb2vYI", KernelBoot11::GetSocPowerConsumption);  // sceKernelGetSocPowerConsumption
	LIB_FUNC("Ee3LPFXEvA0", KernelBoot11::SetIoreq);                // sceKernelSetIoreq
	LIB_FUNC("Ww2Gpga45Zc", KernelBoot11::GetIoreq);                // sceKernelGetIoreq
}

} // namespace KernelBoot11

namespace NetBoot11 {

LIB_VERSION("Net", 1, "Net", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetDataTraffic(uint64_t a0, uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: xHq87H78dho sceNetBandwidthControlGetDataTraffic (42x;
	// -1, out, 0). Validated 8-byte zero only.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	const uint64_t zero = 0;
	if (a1 != 0 && ProbeGuest(a1, sizeof(zero), true)) {
		WriteGuestBytes(a1, &zero, sizeof(zero));
	}
	return 0;
}

LIB_DEFINE(InitShellHitsNetBoot11_1) {
	LIB_FUNC("xHq87H78dho", NetBoot11::GetDataTraffic); // sceNetBandwidthControlGetDataTraffic
}

} // namespace NetBoot11

namespace LibcInternalBoot11 {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MspaceMemalign(uint64_t a0, uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: iF1iQHzxBJU sceLibcMspaceMemalign(mspace, alignment, size).
	// ShellUI's power-locker allocator requests (0x40, 0x30080) here and
	// immediately stores into the block, so NULL crashed. Serve it from the
	// tracked guest heap (MspaceFree accepts these blocks).
	(void)a0;
	(void)a3;
	(void)a4;
	(void)a5;
	void* p = (a1 != 0 && (a1 & (a1 - 1)) == 0)
	              ? LibcInternal::Memalign(static_cast<size_t>(a1), static_cast<size_t>(a2))
	              : nullptr;
	LOGF("\t mspace memalign(align=0x%" PRIx64 ", size=0x%" PRIx64 ") -> %p\n", a1, a2, p);
	return p;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MtxDestroy(uint64_t a0, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: XP6c8M4G+fc _Mtx_destroy (1x). Log-only; never frees
	// (a waiter may reference it, cf. NpCondDestroy).
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
}

LIB_DEFINE(InitShellHitsLibcBoot11_1) {
	LIB_FUNC("AEJdIVZTEmo", LibcInternal::Qsort);
	LIB_FUNC("iF1iQHzxBJU", LibcInternalBoot11::MspaceMemalign); // sceLibcMspaceMemalign
	LIB_FUNC("XP6c8M4G+fc", LibcInternalBoot11::MtxDestroy);     // _Mtx_destroy
}

} // namespace LibcInternalBoot11

namespace NpCommonBoot11 {

LIB_VERSION("NpCommon", 1, "NpCommon", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreateThread(uint64_t a0, uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: fhJ5uKzcn0w sceNpCreateThread (8x). No tracked Np threads
	// yet; success ack so teardown (Join, which acks unknown handles)
	// proceeds. No writes.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsNpCommonBoot11_1) {
	LIB_FUNC("fhJ5uKzcn0w", NpCommonBoot11::CreateThread); // sceNpCreateThread
}

} // namespace NpCommonBoot11

namespace NpWebApi2Boot11 {

LIB_VERSION("NpWebApi2", 1, "NpWebApi2", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI IntInitialize(uint64_t a0, uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: zXaFo7euxsQ sceNpWebApi2IntInitialize (3x). Init ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsNpWebApi2Boot11_1) {
	LIB_FUNC("zXaFo7euxsQ", NpWebApi2Boot11::IntInitialize); // sceNpWebApi2IntInitialize
}

} // namespace NpWebApi2Boot11

namespace Json2Boot11 {

LIB_VERSION("Json2", 1, "Json", 1, 1);

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ObjectEnd(uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: ivMCitpSQNk sce::Json::Object::end (3x). Returns end
	// iterator; 0 preserves the config-parse-skipped path (cf. operator!=
	// always-false). Null-safe, no writes.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64
	     " -> 0 (end)\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ObjectFind(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: ehN1Biw3tw8 sce::Json::Object::find (3x). Not-found (end)
	// preserves the skipped-parse path. No writes.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64
	     " -> 0 (not found)\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsJson2Boot11_1) {
	// Object hidden outputs and lifetime are owned by the central Json2 family.
	(void)s;
}

} // namespace Json2Boot11

namespace RegMgrBoot11 {

LIB_VERSION("RegMgr", 1, "RegMgr", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SrvGetRegion() {
	PRINT_NAME();
	// 13.60 Sce.Vsh.Registry.RegistryInternal declares int fn(void), not an
	// output pointer. Region 0 is invalid and leaves OOBE's language order empty.
	// J1 agrees with the existing SYSTEM_region (0x02020000) boot default.
	return 1;
}

LIB_DEFINE(InitShellHitsRegMgrBoot11_1) {
	LIB_FUNC("sywg-RnhZMA", RegMgrBoot11::SrvGetRegion); // sceRegMgrSrvGetRegion
}

} // namespace RegMgrBoot11

namespace MbusBoot11 {

LIB_VERSION("Mbus", 1, "Mbus", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SetVolumeDown(uint64_t a0, uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 5pX6HRtUaAk sceMbusSetVolumeDownForDiscPlayerFG. Setter ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetDeviceInfo(uint64_t a0, uint64_t a1, uint64_t a2,
                                       uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: RsKVKhSWt28 sceMbusGetDeviceInfo (Mbus v1).
	// Prototype: int sceMbusGetDeviceInfo(uint64_t device_id, DeviceInfo* info, uint32_t size).
	// Call sites in eboot.bin (0x3c2acc) and SceShellCore (0x7555b0) pass:
	//   rdi (a0) = device_id
	//   rsi (a1) = info buffer pointer (256 bytes)
	//   rdx (a2) = buffer size (0x100 = 256)
	// DeviceInfo record (256B, verified 4.03 reference): +0 u64 id; +8 i32
	// transport; +12 i32 type; +16 u32 flags; +20 i32 function_count;
	// +24 functions[11] of {i32 type, reserved, state, reserved2} (16B
	// each, ends at 200); +200 i32 user_id. Single virtual device: id=2,
	// transport=0 (USB), type=22 (Bond), flags=0, function_count=1,
	// functions[0]={0,0,0,0}, user_id from the active offline session. Fill only for id==2 with
	// size>=256, else 0x809b0080 (reference kInvalidEvent).
	(void)a3;
	(void)a4;
	(void)a5;
	const uint64_t device_id = a0;
	const uint64_t info_ptr  = a1;
	const uint64_t size      = a2;
	constexpr int kInvalid = static_cast<int>(0x809b0080u);
	LOGF("\t device_id = 0x%016" PRIx64 " info = 0x%016" PRIx64 " size = 0x%016" PRIx64 "\n",
	     device_id, info_ptr, size);
	if (device_id != Mbus::kVirtualPadDeviceId || info_ptr == 0 || size < 256u ||
	    !ProbeGuest(info_ptr, 256u, true)) {
		LOGF("\t RsKVKhSWt28 miss: id=%llu -> 0x809b0080\n",
		     static_cast<unsigned long long>(device_id));
		return kInvalid;
	}
	const int32_t  uid = Mbus::VirtualPadOwnerUserId();
	const auto rec = Mbus::VirtualPadDeviceInfo(uid);
	WriteGuestBytes(info_ptr, rec.data(), rec.size());
	static bool logged = false;
	if (!logged) {
		logged = true;
		LOGF("\t RsKVKhSWt28 wake: id=2 filled (transport=%d type=%d funcs=1 user=%d)\n",
		     Mbus::kVirtualPadTransport, Mbus::kVirtualPadDeviceType, uid);
	}
	return 0; // OK
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetDeviceInfoByCondition(uint64_t cond, uint64_t a1, uint64_t out,
                                                  uint64_t idx, uint64_t cntA,
                                                  uint64_t cntB) {
	PRINT_NAME();
	// ps5-vsh boot151: KRL-S9qBqXw sceMbusGetDeviceInfoByCondition_ (Mbus v1,
	// exact key from the boot140 Relocate line; absence-checked: unowned).
	// Guest prototype from call sites (7558ef, 984877): (rdi=cond, rsi,
	// rdx=out, rcx=index, r8=cntA-out, r9=cntB-out, stack=0x100); returns
	// device COUNT, count>0 fires arrival handling (984877: [obj+0x70]=1
	// present on eax>0; 7558ef requires *cntA==1 else 0x80b60009). Was an
	// always-1 + made-up qword keys at +8/+16 (transport=0x1001): that broke
	// the libScePsm CtrlScanner rescan (~11 condition queries over types
	// 2,22,3,23,24,...) which aborts when counts don't match. Wake: real
	// 256B DeviceInfo match filter (4.03 reference logic): requested
	// functions (each {type,state} with -1 wildcard) must exist in our
	// function list, and id (UINT64_MAX wildcard), transport/type/user_id
	// (-1 wildcard each) must match. Single virtual device: id=2,
	// transport=0, type=22 (Bond), flags=0, function_count=1, func[0]={0,0,0,0},
	// user_id from the active offline session. *cntA = total matched (0/1);
	// *cntB = 1 (generation).
	// Entry written at out only when matched && start==0 && capacity>0;
	// return is the count written; *cntA carries the total regardless.
	(void)a1;
	constexpr int kInvalid = static_cast<int>(0x809b0080u);
	bool     matched = false;
	const int32_t dev_user = Mbus::VirtualPadOwnerUserId();
	uint64_t cond_id = 0;
	int32_t  cond_transport = 0;
	int32_t  cond_type = 0;
	int32_t  cond_func_cnt = 0;
	int32_t  cond_user = 0;
	bool     cond_ok = false;
	uint8_t  condBuf[256] = {};
	if (cond != 0 && ProbeGuest(cond, sizeof(condBuf), false)) {
		std::memcpy(condBuf, reinterpret_cast<const void*>(cond), sizeof(condBuf));
		cond_ok = true;
		std::memcpy(&cond_id, condBuf + 0, sizeof(cond_id));
		std::memcpy(&cond_transport, condBuf + 8, sizeof(cond_transport));
		std::memcpy(&cond_type, condBuf + 12, sizeof(cond_type));
		std::memcpy(&cond_func_cnt, condBuf + 20, sizeof(cond_func_cnt));
		std::memcpy(&cond_user, condBuf + 200, sizeof(cond_user));
		if (cond_func_cnt >= 0 && cond_func_cnt <= 11) {
			const int32_t dev_type = Mbus::kVirtualPadDeviceType;
			const int32_t dev_transport = Mbus::kVirtualPadTransport;
			const uint64_t dev_id = Mbus::kVirtualPadDeviceId;
			bool functions_match = true;
			for (int32_t i = 0; i < cond_func_cnt; ++i) {
				int32_t req_type = 0;
				int32_t req_state = 0;
				std::memcpy(&req_type, condBuf + 24 + static_cast<size_t>(i) * 16u + 0,
				            sizeof(req_type));
				std::memcpy(&req_state, condBuf + 24 + static_cast<size_t>(i) * 16u + 8,
				            sizeof(req_state));
				// Sole function is {type=0, state=0}.
				const bool found = (req_type == -1 || req_type == 0) &&
				                   (req_state == -1 || req_state == 0);
				functions_match = functions_match && found;
			}
			matched = functions_match &&
			    (cond_id == UINT64_MAX || cond_id == dev_id) &&
			    (cond_transport == -1 || cond_transport == dev_transport) &&
			    (cond_type == -1 || cond_type == dev_type) &&
			    (cond_user == -1 || cond_user == dev_user);
		} else {
			cond_ok = false; // out-of-range function_count: invalid below
		}
	}
	if (!cond_ok) {
		static std::atomic<unsigned> s_bad {0};
		if (s_bad.fetch_add(1, std::memory_order_relaxed) < 24) {
			LOGF("\t KRL-S9qBqXw bad cond: cond=0x%016" PRIx64 " -> 0x809b0080\n", cond);
		}
		return kInvalid;
	}
	// ABI (4.03 reference, same bus): (cond, start=rsi, out, capacity=rcx,
	// total=r8, generation=r9). Entry is written at out when start==0 and
	// capacity>0; the return is the number written. (An earlier reading of
	// rcx as a start-index made Psm's capacity-8 query return 0 devices.)
	const uint32_t total = matched ? 1u : 0u;
	const uint32_t gen = 1u;
	if (cntA != 0 && ProbeGuest(cntA, sizeof(total), true)) {
		WriteGuestBytes(cntA, &total, sizeof(total));
	}
	if (cntB != 0 && ProbeGuest(cntB, sizeof(gen), true)) {
		WriteGuestBytes(cntB, &gen, sizeof(gen));
	}
	if (matched && a1 == 0 && idx > 0 && out != 0 && ProbeGuest(out, 256u, true)) {
		const auto rec = Mbus::VirtualPadDeviceInfo(dev_user);
		WriteGuestBytes(out, rec.data(), rec.size());
	}
	static std::atomic<unsigned> s_queries {0};
	if (s_queries.fetch_add(1, std::memory_order_relaxed) < 24) {
		LOGF("\t KRL-S9qBqXw cond: id=0x%016" PRIx64 " transport=%d type=%d user=%d funcs=%d idx=%llu -> matched=%d\n",
		     cond_id, cond_transport, cond_type, cond_user, cond_func_cnt,
		     static_cast<unsigned long long>(idx), matched ? 1 : 0);
	}
	return (matched && a1 == 0 && idx > 0) ? 1 : 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI QueryDeviceEntry(uint64_t out, uint64_t size, uint64_t a2,
                                          uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// edYHYROxzx4: all inspected ShellCore call sites pass (out, 0x100).
	// Native libSceMbus initializes a condition/device record here; callers
	// populate their desired filters or subsequently call GetDeviceInfo.
	LOGF("\t out = 0x%016" PRIx64 " size = 0x%016" PRIx64 " -> 0\n", out,
	     size);
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;
	if (out == 0) {
		return 0;
	}
	// 13.60 libSceMbus +0x4f40 is a DeviceInfo initializer, not a device
	// lookup: zero the record, then set id/transport/type and owner to -1.
	// Native scanners specialize those wildcards before enumerating. Planting
	// synthetic keys here prevents their queries from finding the DualSense.
	if (size != 256 || !ProbeGuest(out, 256, true)) {
		return static_cast<int>(0x809b0080u);
	}
	std::array<uint8_t, 256> initial {};
	std::memset(initial.data(), 0xff, 16);
	const int32_t no_owner = -1;
	std::memcpy(initial.data() + 200, &no_owner, sizeof(no_owner));
	WriteGuestBytes(out, initial.data(), initial.size());
	// ps5-vsh login-done seed (static RE, SceShellCore.elf FW 13.60, file-VA):
	// processMbusEvent @0x748290 prints `[[LoginMgr]] System State Manager
	// Event : %d` UNCONDITIONALLY (0x748296 mov esi,[rdi]; 0x74829b lea rdi,
	// [rip+0x174b2ca] -> str @0x1e9356c; call), then `cmp [rbx],2` (0x7482a9)
	// gates the ONLY login-done write: `mov rax,[rip+0x19af43b]` (0x7482ae,
	// bytes 48 8b 05 3b f4 9a 01 -> file VA 0x20f76f0, BSS) +
	// `mov byte [rax+0x218],1` (0x7482b5). Accessor @0x7482d0 reads the SAME
	// pointer (rip+0x19af419 -> 0x20f76f0) for the uid dispatch
	// (0x73eb96/0x73ebba -> jmp 0x748680/0x748720), so +0x218 is THE
	// LoginMgr logged-in byte. Demux lookup 0x73b160 misses every injected
	// id==2 (filler 0x755470 HLE plant is overwritten by ev keys at
	// 0x73b809/0x73b814/0x73b83f), so the byte stays 0 and login never
	// completes. Seed it here: this HLE fires ~10Hz from the T14 poll plus
	// per-event fills, after the manager exists. One byte, zero-slot only
	// (never clobber a live word), probed, one-shot. No imports touched.
	{
		static bool s_loginDoneSeeded = false;
		if (!s_loginDoneSeeded) {
			const uint64_t mgrPtr = 0x9020f76f0ull; // 0x900000000 + 0x20f76f0
			uint64_t       mgr    = 0;
			if (ReadGuestU64(mgrPtr, &mgr) && mgr != 0) {
				const uint64_t faddr = mgr + 0x218u;
				if (ProbeGuest(faddr, 1, true)) {
					const uint8_t cur =
					    *reinterpret_cast<const uint8_t*>(faddr);
					if (cur == 0) {
						const uint8_t one = 1;
						if (WriteGuestBytes(faddr, &one, sizeof(one))) {
							s_loginDoneSeeded = true;
							LOGF("\t login-done-seed: [mgr 0x%016" PRIx64
							     "]+0x218 0->1 (one-shot, RE 0x7482b5)\n",
							     mgr);
						}
					} else if (cur == 1) {
						s_loginDoneSeeded = true;
					}
				}
			}
		}
	}
	static bool logged = false;
	if (!logged) {
		logged = true;
		LOGF("\t Mbus DeviceInfo initialized: out=0x%016" PRIx64
		     " id/transport/type/owner=-1, functions=0\n",
		     out);
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SetAutoLoginMode(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	(void)a0; (void)a1;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SetScratchDataUpdatedEventMask(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	(void)a0; (void)a1;
	return 0;
}

LIB_DEFINE(InitShellHitsMbusBoot11_1) {
	LIB_FUNC("5pX6HRtUaAk", MbusBoot11::SetVolumeDown); // sceMbusSetVolumeDownForDiscPlayerFG
	LIB_FUNC("RsKVKhSWt28", MbusBoot11::GetDeviceInfo); // sceMbusGetDeviceInfo_
	LIB_FUNC("KRL-S9qBqXw", MbusBoot11::GetDeviceInfoByCondition); // sceMbusGetDeviceInfoByCondition_
	LIB_FUNC("edYHYROxzx4", MbusBoot11::QueryDeviceEntry); // unknown Mbus entry-fill (jmprela 616)
	LIB_FUNC("wpm6Yq7c4YE", MbusBoot11::SetAutoLoginMode); // sceMbusSetAutoLoginMode
	LIB_FUNC("Sq1DqijPveA", MbusBoot11::SetScratchDataUpdatedEventMask); // sceMbusSetScratchDataUpdatedEventMask
}

} // namespace MbusBoot11

namespace AbstractStorageBoot11 {

LIB_VERSION("AbstractStorage", 1, "AbstractStorage", 1, 1);

static uint8_t s_storageManagerInstance[256] = {};

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI StorageManagerGetInstance(uint64_t a0, uint64_t a1,
                                                     uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	(void)a0; (void)a1; (void)a2; (void)a3;
	return s_storageManagerInstance;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI StorageManagerInitialize(void* self) {
	PRINT_NAME();
	(void)self;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI StorageManagerLoad(void* self, int service_type) {
	PRINT_NAME();
	(void)self; (void)service_type;
	return 0;
}

LIB_DEFINE(InitShellHitsAbstractStorageBoot11_1) {
	LIB_FUNC("iAN-9pPHZSE", AbstractStorageBoot11::StorageManagerGetInstance); // GetInstance
	LIB_FUNC("QbKmf+2+cR0", AbstractStorageBoot11::StorageManagerGetInstance); // GetInstance
	LIB_FUNC("k0BKafeYaUo", AbstractStorageBoot11::StorageManagerInitialize);  // Initialize
	LIB_FUNC("3iP9Xa6Z3z0", AbstractStorageBoot11::StorageManagerLoad);        // Load
}

} // namespace AbstractStorageBoot11

// Dual-key fixes: the 3 NIDs below already have real impls under the old
// provider keys (LibcInternal / HttpCache 1.1 / NpCommon, 0x14057 slots), but
// the guest IMPORTS them under different request keys (0x14038 variant), so
// exact-Find missed and NID-fallback lost to the exact-key LogStub. Register
// the SAME behavior under the request keys; both keys land on the real impl.

namespace LibreSSLBoot11 {

LIB_VERSION("LibreSSL", 1, "LibreSSL", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI LibreSslInitEx(uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: jri3LyXhEGw sceLibreSslInitEx imported as (LibreSSL,
	// LibreSSL) (request key #h#e); old impl lives under (LibcInternal,
	// LibcInternal). Same init-ack behavior on both keys.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsLibreSSLBoot11_1) {
	LIB_FUNC("jri3LyXhEGw", LibreSSLBoot11::LibreSslInitEx); // sceLibreSslInitEx (request key)
}

} // namespace LibreSSLBoot11

namespace NpManagerBoot11 {

LIB_VERSION("NpManager", 1, "NpManager", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AsmClientExtTerminate(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: yODgcWKSa8Q sceNpAsmClientExtTerminate imported as
	// (NpManager, NpManager) (request key #T#S); old impl lives under
	// (NpCommon, NpCommon). Terminate ack on both keys.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsNpManagerBoot11_1) {
	LIB_FUNC("yODgcWKSa8Q", NpManagerBoot11::AsmClientExtTerminate); // request key
}

} // namespace NpManagerBoot11

namespace HttpCacheBoot11_10 {

LIB_VERSION("HttpCache", 1, "HttpCache", 1, 0);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CacheSystemInit(uint64_t a0, uint64_t a1, uint64_t a2,
                                         uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: LfOVpdFit+Q sceHttpCacheSystemInit imported as (HttpCache
	// v1, HttpCache v1.0) (request key #P#P, mod MINOR 0); old impl lives
	// under (HttpCache v1, HttpCache v1.1). Init ack on both keys.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsHttpCacheBoot11_10_1) {
	LIB_FUNC("LfOVpdFit+Q", HttpCacheBoot11_10::CacheSystemInit); // request key (1.0)
}

} // namespace HttpCacheBoot11_10

// ps5-vsh boot12 singletons (request keys from boot12 Relocate lines).
namespace Boot12 {

LIB_VERSION("AudioOut", 1, "AudioOut", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AudioOutSetVolumeDown(uint64_t a0, uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: rho9DH-0ehs sceAudioOutSetVolumeDown(8, 0x12, 1, ...).
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 "\n",
	     a0, a1, a2);
	(void)a3;
	(void)a4;
	(void)a5;
	return 0;
}

LIB_DEFINE(InitShellHitsAudioOutBoot12_1) {
	LIB_FUNC("rho9DH-0ehs", Boot12::AudioOutSetVolumeDown);
}

} // namespace Boot12

namespace Boot12Json {

LIB_VERSION("Json2", 1, "Json", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI JsonObjectIteratorNotEqual(uint64_t a0, uint64_t a1) {
	PRINT_NAME();
	// ps5-vsh: +isUKw4zud4 Json::Object::iterator::operator!=. Single-shot
	// compare in the log; 0 (equal) skips loops over empty containers.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 "\n", a0, a1);
	(void)a0;
	(void)a1;
	return 0;
}

LIB_DEFINE(InitShellHitsJsonBoot12_1) {
	(void)s; // Central Json2 comparison must agree with its iterator producers.
}

} // namespace Boot12Json

namespace Boot12Http {

LIB_VERSION("HttpCache", 1, "HttpCache", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpCacheUnknown(uint64_t a0, uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: b74k6s-1WzU, no nids.csv name. Log-only success.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 "\n",
	     a0, a1, a2);
	(void)a3;
	(void)a4;
	(void)a5;
	return 0;
}

LIB_DEFINE(InitShellHitsHttpBoot12_1) {
	LIB_FUNC("b74k6s-1WzU", Boot12Http::HttpCacheUnknown);
}

} // namespace Boot12Http

namespace Boot12Kernel {

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetDebugMenuModeForRcmgr(uint64_t a0, uint64_t a1, uint64_t a2) {
	PRINT_NAME();
	// ps5-vsh: opMiSfTJyJo sceKernelGetDebugMenuModeForRcmgr. Retail: no
	// debug menu.
	LOGF("\t a0 = 0x%016" PRIx64 "\n", a0);
	(void)a0;
	(void)a1;
	(void)a2;
	return 0;
}

LIB_DEFINE(InitShellHitsKernelBoot12_1) {
	LIB_FUNC("opMiSfTJyJo", Boot12Kernel::GetDebugMenuModeForRcmgr);
}

} // namespace Boot12Kernel

// Batch 4 (shellcore_boot14.log; 24 "Unresolved import stub" calls, 8 distinct
// NIDs, all returning the dead-stub 0/NULL). Same override discipline as
// batches 1-3: each NID below is ABSENT from src/libs (grep-verified), so
// these are fresh exact-key registrations, not duplicates. Exact keys are the
// guest's REQUEST keys from the boot14 Relocate lines
// (NID[lib_v1][mod_v1.1]); resolving them here turns next boot's dead stubs
// into real impls. Semantics from _ProbeLogs/nids.csv names + call-site
// context. Rule: real semantics only where provably safe (Np atomics via
// host-interlocked 4-byte slots; Rtc datetime via probed 32-byte fill);
// everything else is observe + ack, never bulk writes, null-safe everywhere.

namespace NpCommonBoot14 {

LIB_VERSION("NpCommon", 1, "NpCommon", 1, 1);

static uint32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAtomicCas32(uint64_t ptr, uint64_t expected, uint64_t nw,
                                             uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: +9+kKMY9YIw sceNpAtomicCas32 (1x, NpSyndySyncLogic init at the
	// boot14 tail boundary, line 31292). REAL compare-and-swap returning the
	// OLD 32-bit value (the dead stub returned 0 regardless of memory, so a
	// contended slot could mis-report). 4-byte aligned probed slot only.
	const uint32_t exp32 = static_cast<uint32_t>(expected);
	const uint32_t new32 = static_cast<uint32_t>(nw);
	uint32_t       old   = 0;
	if (ptr != 0 && (ptr & 3u) == 0 && ProbeGuest(ptr, sizeof(old), true)) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		old = static_cast<uint32_t>(::InterlockedCompareExchange(
		    reinterpret_cast<volatile LONG*>(ptr), static_cast<LONG>(new32),
		    static_cast<LONG>(exp32)));
#else
		std::memcpy(&old, reinterpret_cast<const void*>(ptr), sizeof(old));
		if (old == exp32) {
			WriteGuestBytes(ptr, &new32, sizeof(new32));
		}
#endif
	}
	LOGF("\t ptr=0x%016" PRIx64 " expected=0x%08x new=0x%08x -> old=0x%08x\n", ptr,
	     exp32, new32, old);
	(void)a3;
	(void)a4;
	(void)a5;
	return old;
}

static uint32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NpAtomicInc32(uint64_t ptr, uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// Native NpCommon+6050 is lock xadd dword [ptr],1: return the OLD value.
	// Trophy's first scheduling test depends on -1 -> 0 returning -1.
	uint32_t cur = 0;
	if (ptr != 0 && (ptr & 3u) == 0 && ProbeGuest(ptr, sizeof(cur), true)) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		cur = static_cast<uint32_t>(
		    ::InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(ptr), 1));
#else
		cur = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(ptr)).fetch_add(1, std::memory_order_seq_cst);
#endif
	}
	LOGF("\t ptr=0x%016" PRIx64 " -> %u\n", ptr, cur);
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;
	return cur;
}

LIB_DEFINE(InitShellHitsNpCommonBoot14_1) {
	LIB_FUNC("+9+kKMY9YIw", NpCommonBoot14::NpAtomicCas32); // sceNpAtomicCas32
	LIB_FUNC("pfJgSA4jO3M", NpCommonBoot14::NpAtomicInc32);  // sceNpAtomicInc32
}

} // namespace NpCommonBoot14

namespace SystemLogger2Boot14 {

LIB_VERSION("SystemLogger2", 1, "SystemLogger2", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SystemLogger2Initialize(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 1-1uDZ4tiFA sceSystemLogger2Initialize (3x, Sl2 aggregator
	// setup). Init ack; Sl2 logging already flows through the FS overlay.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SetEventAsync(uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh boot320: hX5OlI45fC4 sceSystemLogger2SetEventAsync
	// (SystemLogger2_v1, exact key from the boot320 Relocate lines;
	// absence-checked: no impl/LogStub anywhere in src). The SOLE
	// runtime-reached unresolved NID after the login-done seed (1x at
	// boot320 line 91057, steady-state Sl2 path; all 30 ShellCoreUtil NIDs
	// are Relocate-only, never called). Async fire-and-forget event post:
	// ack 0 = accepted (the dead stub's 0 already kept boot alive past it).
	// Observe + ack, NO writes (payload layout unknown; canary rule),
	// null-safe.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64
	     " -> 0 (async accepted)\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsSystemLogger2Boot14_1) {
	LIB_FUNC("1-1uDZ4tiFA", SystemLogger2Boot14::SystemLogger2Initialize); // sceSystemLogger2Initialize
	LIB_FUNC("hX5OlI45fC4", SystemLogger2Boot14::SetEventAsync); // sceSystemLogger2SetEventAsync
}

} // namespace SystemLogger2Boot14

namespace HttpBoot14 {

LIB_VERSION("Http", 1, "Http", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetPolicyOption(uint64_t a0, uint64_t a1, uint64_t a2,
                                              uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: gZ9TpeFQ7Gk sceHttpSetPolicyOption (3x, right after the
	// http-template create log: ctx 65538/65539/65540). Option setter ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI HttpSetPriorityOption(uint64_t a0, uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: 2NeZnMEP3-0 sceHttpSetPriorityOption (3x, paired with the
	// policy option above). Option setter ack.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsHttpBoot14_1) {
	LIB_FUNC("gZ9TpeFQ7Gk", HttpBoot14::HttpSetPolicyOption);     // sceHttpSetPolicyOption
	LIB_FUNC("2NeZnMEP3-0", HttpBoot14::HttpSetPriorityOption);   // sceHttpSetPriorityOption
}

} // namespace HttpBoot14

namespace FsVshBoot14 {

LIB_VERSION("FsInternalForVsh", 1, "FsInternalForVsh", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FsISSchedConfigDescriptor(uint64_t a0, uint64_t a1, uint64_t a2,
                                                    uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: a6HOJ0hpGiY sceFsISSchedConfigDescriptor (2x, around
	// FQDN_list.json reads). Scheduler-config descriptor; layout unknown:
	// observe + OK, NO writes.
	LOGF("\t a0 = 0x%016" PRIx64 " a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " a3 = 0x%016" PRIx64 " a4 = 0x%016" PRIx64 " a5 = 0x%016" PRIx64 "\n",
	     a0, a1, a2, a3, a4, a5);
	return 0;
}

LIB_DEFINE(InitShellHitsFsVshBoot14_1) {
	LIB_FUNC("a6HOJ0hpGiY", FsVshBoot14::FsISSchedConfigDescriptor); // sceFsISSchedConfigDescriptor
}

} // namespace FsVshBoot14

namespace RtcBoot14 {

LIB_VERSION("Rtc", 1, "Rtc", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI RtcFormatRFC3339LocalTime(uint64_t out, uint64_t tick,
                                                    uint64_t a2, uint64_t a3,
                                                    uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: DwuHIlLGW8I sceRtcFormatRFC3339LocalTime (5x, BgDailyChecker
	// "next check datetime"). The dead stub wrote NOTHING, hence the garbled
	// datetime in boot14. Minimal fill: host-local "YYYY-MM-DDTHH:MM:SS"
	// (19+NUL) into out when 32 bytes probe writable; the tick (a1) is only
	// logged, never touched. Return 0 = formatted.
	char             buf[32] = {};
	const std::time_t now    = std::time(nullptr);
	std::tm          tmv {};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	localtime_s(&tmv, &now);
#else
	localtime_r(&now, &tmv);
#endif
	if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv) == 0) {
		LOGF("\t strftime failed\n");
		(void)tick;
		(void)a2;
		(void)a3;
		(void)a4;
		(void)a5;
		return -1;
	}
	LOGF("\t out=0x%016" PRIx64 " tick=0x%016" PRIx64 " -> \"%s\"\n", out, tick, buf);
	if (out != 0 && ProbeGuest(out, 32, true)) {
		WriteGuestBytes(out, buf, std::strlen(buf) + 1);
	} else {
		LOGF("\t out unwritable, no fill\n");
	}
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;
	return 0;
}

LIB_DEFINE(InitShellHitsRtcBoot14_1) {
	LIB_FUNC("DwuHIlLGW8I", RtcBoot14::RtcFormatRFC3339LocalTime); // sceRtcFormatRFC3339LocalTime
}

} // namespace RtcBoot14

// ps5-vsh boot15 batch (shellcore_boot15.log lines 25372-25373): the last 2
// runtime-reached unresolved NIDs, both LibreSSL AES, called back-to-back by
// SceSl2FwMain (thread 99) in the SqliteTelemetryEncryption session-key path
// (kern.rng_pseudo fills 16 bytes just before; /dev/envelope open follows,
// then sceSblEnvelopeOpen2 fails 0x80020002 downstream — unrelated).
// Request keys from the boot15 Relocate lines; both absence-checked in
// src/libs (no other file registers them). No host crypto lib is linked
// (3rdparty offers none: cpuinfo/opus/zstd/SDL3/imgui/... only), so this is a
// self-contained FIPS-197 AES-128/192/256 — real ciphertext, never zeros.
// Guest abi: LibreSSL AES_set_encrypt_key(const u8*, int bits, AES_KEY*)
// returns 0 ok / -1 bad-bits; AES_cbc_encrypt(in, out, len, key, ivec, enc)
// encrypts iff enc != 0 and advances the 16-byte IV. Guest aes_key_st is
// u32 rd_key[60]; int rounds (244 bytes, LE like the host); words are packed
// big-endian from the expanded bytes and unpacked the same way, so the
// convention is self-consistent by construction.

namespace LibreSSLBoot15 {

LIB_VERSION("LibreSSL", 1, "LibreSSL", 1, 1);

constexpr size_t kAesKeyStSize = 60 * sizeof(uint32_t) + sizeof(int32_t); // 244

static const uint8_t kAesSBox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static const uint8_t kAesInvSBox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

static const uint8_t kAesRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                     0x20, 0x40, 0x80, 0x1b, 0x36};

struct AesRoundKeys {
	uint8_t rk[15 * 16] = {};
	int     rounds      = 0; // Nr: 10/12/14
};

static void AesExpandKey(const uint8_t* key, size_t key_bytes, AesRoundKeys* out) {
	const int nk = static_cast<int>(key_bytes) / 4; // 4/6/8
	const int nr = nk + 6;
	out->rounds  = nr;
	uint32_t w[60] = {};
	for (int i = 0; i < nk; i++) {
		w[i] = (static_cast<uint32_t>(key[4 * i]) << 24) |
		       (static_cast<uint32_t>(key[4 * i + 1]) << 16) |
		       (static_cast<uint32_t>(key[4 * i + 2]) << 8) |
		       static_cast<uint32_t>(key[4 * i + 3]);
	}
	const int total = 4 * (nr + 1);
	for (int i = nk; i < total; i++) {
		uint32_t t = w[i - 1];
		if (i % nk == 0) {
			t = (static_cast<uint32_t>(kAesSBox[(t >> 16) & 0xff]) << 24) |
			    (static_cast<uint32_t>(kAesSBox[(t >> 8) & 0xff]) << 16) |
			    (static_cast<uint32_t>(kAesSBox[t & 0xff]) << 8) |
			    static_cast<uint32_t>(kAesSBox[(t >> 24) & 0xff]);
			t ^= static_cast<uint32_t>(kAesRcon[i / nk - 1]) << 24;
		} else if (nk > 6 && i % nk == 4) {
			t = (static_cast<uint32_t>(kAesSBox[(t >> 24) & 0xff]) << 24) |
			    (static_cast<uint32_t>(kAesSBox[(t >> 16) & 0xff]) << 16) |
			    (static_cast<uint32_t>(kAesSBox[(t >> 8) & 0xff]) << 8) |
			    static_cast<uint32_t>(kAesSBox[t & 0xff]);
		}
		w[i] = w[i - nk] ^ t;
	}
	for (int i = 0; i < total; i++) {
		out->rk[4 * i]     = static_cast<uint8_t>((w[i] >> 24) & 0xff);
		out->rk[4 * i + 1] = static_cast<uint8_t>((w[i] >> 16) & 0xff);
		out->rk[4 * i + 2] = static_cast<uint8_t>((w[i] >> 8) & 0xff);
		out->rk[4 * i + 3] = static_cast<uint8_t>(w[i] & 0xff);
	}
}

static uint8_t AesMul(uint8_t a, uint8_t b) {
	uint8_t p = 0;
	while (b != 0) {
		if ((b & 1) != 0) {
			p ^= a;
		}
		const bool hi = (a & 0x80) != 0;
		a <<= 1;
		if (hi) {
			a ^= 0x1b;
		}
		b >>= 1;
	}
	return p;
}

// State is column-major: s[row + 4 * col] (FIPS-197 §3.4).
static void AesEncryptBlock(const uint8_t in[16], uint8_t out[16], const AesRoundKeys* k) {
	uint8_t s[16];
	std::memcpy(s, in, 16);
	for (int i = 0; i < 16; i++) {
		s[i] ^= k->rk[i];
	}
	for (int round = 1; round < k->rounds; round++) {
		for (int i = 0; i < 16; i++) {
			s[i] = kAesSBox[s[i]];
		}
		uint8_t t[16];
		t[0] = s[0];
		t[1] = s[5];
		t[2] = s[10];
		t[3] = s[15];
		t[4] = s[4];
		t[5] = s[9];
		t[6] = s[14];
		t[7] = s[3];
		t[8] = s[8];
		t[9] = s[13];
		t[10] = s[2];
		t[11] = s[7];
		t[12] = s[12];
		t[13] = s[1];
		t[14] = s[6];
		t[15] = s[11];
		for (int c = 0; c < 4; c++) {
			const uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2],
			              a3 = t[4 * c + 3];
			s[4 * c]     = AesMul(a0, 2) ^ AesMul(a1, 3) ^ a2 ^ a3;
			s[4 * c + 1] = a0 ^ AesMul(a1, 2) ^ AesMul(a2, 3) ^ a3;
			s[4 * c + 2] = a0 ^ a1 ^ AesMul(a2, 2) ^ AesMul(a3, 3);
			s[4 * c + 3] = AesMul(a0, 3) ^ a1 ^ a2 ^ AesMul(a3, 2);
		}
		const uint8_t* rk = k->rk + 16 * round;
		for (int i = 0; i < 16; i++) {
			s[i] ^= rk[i];
		}
	}
	for (int i = 0; i < 16; i++) {
		s[i] = kAesSBox[s[i]];
	}
	uint8_t t[16];
	t[0] = s[0];
	t[1] = s[5];
	t[2] = s[10];
	t[3] = s[15];
	t[4] = s[4];
	t[5] = s[9];
	t[6] = s[14];
	t[7] = s[3];
	t[8] = s[8];
	t[9] = s[13];
	t[10] = s[2];
	t[11] = s[7];
	t[12] = s[12];
	t[13] = s[1];
	t[14] = s[6];
	t[15] = s[11];
	const uint8_t* rk = k->rk + 16 * k->rounds;
	for (int i = 0; i < 16; i++) {
		out[i] = t[i] ^ rk[i];
	}
}

static void AesDecryptBlock(const uint8_t in[16], uint8_t out[16], const AesRoundKeys* k) {
	uint8_t s[16];
	std::memcpy(s, in, 16);
	const uint8_t* rk = k->rk + 16 * k->rounds;
	for (int i = 0; i < 16; i++) {
		s[i] ^= rk[i];
	}
	for (int round = k->rounds - 1; round >= 1; round--) {
		uint8_t t[16];
		t[0] = s[0];
		t[1] = s[13];
		t[2] = s[10];
		t[3] = s[7];
		t[4] = s[4];
		t[5] = s[1];
		t[6] = s[14];
		t[7] = s[11];
		t[8] = s[8];
		t[9] = s[5];
		t[10] = s[2];
		t[11] = s[15];
		t[12] = s[12];
		t[13] = s[9];
		t[14] = s[6];
		t[15] = s[3];
		for (int i = 0; i < 16; i++) {
			s[i] = kAesInvSBox[t[i]];
		}
		rk = k->rk + 16 * round;
		for (int i = 0; i < 16; i++) {
			s[i] ^= rk[i];
		}
		for (int c = 0; c < 4; c++) {
			const uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2],
			              a3 = s[4 * c + 3];
			t[4 * c]     = AesMul(a0, 0x0e) ^ AesMul(a1, 0x0b) ^ AesMul(a2, 0x0d) ^ AesMul(a3, 0x09);
			t[4 * c + 1] = AesMul(a0, 0x09) ^ AesMul(a1, 0x0e) ^ AesMul(a2, 0x0b) ^ AesMul(a3, 0x0d);
			t[4 * c + 2] = AesMul(a0, 0x0d) ^ AesMul(a1, 0x09) ^ AesMul(a2, 0x0e) ^ AesMul(a3, 0x0b);
			t[4 * c + 3] = AesMul(a0, 0x0b) ^ AesMul(a1, 0x0d) ^ AesMul(a2, 0x09) ^ AesMul(a3, 0x0e);
		}
		std::memcpy(s, t, 16);
	}
	uint8_t t[16];
	t[0] = s[0];
	t[1] = s[13];
	t[2] = s[10];
	t[3] = s[7];
	t[4] = s[4];
	t[5] = s[1];
	t[6] = s[14];
	t[7] = s[11];
	t[8] = s[8];
	t[9] = s[5];
	t[10] = s[2];
	t[11] = s[15];
	t[12] = s[12];
	t[13] = s[9];
	t[14] = s[6];
	t[15] = s[3];
	for (int i = 0; i < 16; i++) {
		out[i] = kAesInvSBox[t[i]] ^ k->rk[i];
	}
}

// Reads a guest aes_key_st (244 bytes) back into round-key bytes.
static bool AesReadGuestKey(uint64_t key_ptr, AesRoundKeys* out) {
	if (!ProbeGuest(key_ptr, kAesKeyStSize, false)) {
		return false;
	}
	const auto* guest = reinterpret_cast<const uint8_t*>(key_ptr);
	uint32_t    words[60] = {};
	std::memcpy(words, guest, sizeof(words));
	int32_t rounds = 0;
	std::memcpy(&rounds, guest + sizeof(words), sizeof(rounds));
	if (rounds != 10 && rounds != 12 && rounds != 14) {
		return false;
	}
	out->rounds = rounds;
	const int total = 4 * (rounds + 1);
	for (int i = 0; i < total; i++) {
		out->rk[4 * i]     = static_cast<uint8_t>((words[i] >> 24) & 0xff);
		out->rk[4 * i + 1] = static_cast<uint8_t>((words[i] >> 16) & 0xff);
		out->rk[4 * i + 2] = static_cast<uint8_t>((words[i] >> 8) & 0xff);
		out->rk[4 * i + 3] = static_cast<uint8_t>(words[i] & 0xff);
	}
	return true;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AesSetEncryptKey(uint64_t user_key, uint64_t bits, uint64_t key_ptr,
                                          uint64_t a3, uint64_t a4, uint64_t a5) {
	PRINT_NAME();
	// ps5-vsh: RpzLa-jjWnA AES_set_encrypt_key (boot15 [0], jmprela 1457).
	int         ret = -1;
	const int   b   = static_cast<int>(bits);
	const char* why = nullptr;
	if (b != 128 && b != 192 && b != 256) {
		why = "bad bits";
	} else if (user_key == 0 || key_ptr == 0) {
		why = "null pointer";
	} else {
		const size_t klen = static_cast<size_t>(b) / 8;
		if (!ProbeGuest(user_key, klen, false)) {
			why = "userKey unmapped";
		} else if (!ProbeGuest(key_ptr, kAesKeyStSize, true)) {
			why = "key struct unwritable";
		} else {
			uint8_t key[32] = {};
			std::memcpy(key, reinterpret_cast<const void*>(user_key), klen);
			AesRoundKeys ek;
			AesExpandKey(key, klen, &ek);
			uint8_t guest_key[kAesKeyStSize] = {};
			const int total = 4 * (ek.rounds + 1);
			for (int i = 0; i < total; i++) {
				guest_key[4 * i]     = ek.rk[4 * i];
				guest_key[4 * i + 1] = ek.rk[4 * i + 1];
				guest_key[4 * i + 2] = ek.rk[4 * i + 2];
				guest_key[4 * i + 3] = ek.rk[4 * i + 3];
			}
			// NOTE: word packing here must stay byte-identical to
			// AesReadGuestKey's unpacking (both big-endian); the guest
			// struct is only ever written here and read there.
			uint32_t words[60] = {};
			for (int i = 0; i < total; i++) {
				words[i] = (static_cast<uint32_t>(guest_key[4 * i]) << 24) |
				           (static_cast<uint32_t>(guest_key[4 * i + 1]) << 16) |
				           (static_cast<uint32_t>(guest_key[4 * i + 2]) << 8) |
				           static_cast<uint32_t>(guest_key[4 * i + 3]);
			}
			std::memcpy(reinterpret_cast<void*>(key_ptr), words, sizeof(words));
			const int32_t rounds = ek.rounds;
			std::memcpy(reinterpret_cast<void*>(key_ptr + sizeof(words)), &rounds,
			            sizeof(rounds));
			ret = 0;
		}
	}
	LOGF("\t userKey=0x%016" PRIx64 " bits=%d key=0x%016" PRIx64 " -> %d%s%s\n", user_key, b,
	     key_ptr, ret, why != nullptr ? " (" : "", why != nullptr ? why : "");
	(void)a3;
	(void)a4;
	(void)a5;
	return ret;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AesCbcCrypt(uint64_t in_ptr, uint64_t out_ptr, uint64_t length,
                                      uint64_t key_ptr, uint64_t ivec_ptr, uint64_t enc) {
	PRINT_NAME();
	// ps5-vsh: Z55J-I6291o AES_cbc_encrypt (boot15 [1], jmprela 420).
	// enc != 0 encrypts, enc == 0 decrypts; the 16-byte IV is advanced.
	const char* why = nullptr;
	if (length == 0 || (length & 15) != 0) {
		why = "length not a nonzero multiple of 16";
	} else if (in_ptr == 0 || out_ptr == 0 || key_ptr == 0 || ivec_ptr == 0) {
		why = "null pointer";
	} else if (!ProbeGuest(in_ptr, static_cast<size_t>(length), false)) {
		why = "in unmapped";
	} else if (!ProbeGuest(out_ptr, static_cast<size_t>(length), true)) {
		why = "out unwritable";
	} else if (!ProbeGuest(ivec_ptr, 16, true)) {
		why = "ivec unwritable";
	} else {
		AesRoundKeys k;
		if (!AesReadGuestKey(key_ptr, &k)) {
			why = "key struct unreadable (run AES_set_encrypt_key first)";
		} else {
			uint8_t iv[16] = {};
			std::memcpy(iv, reinterpret_cast<const void*>(ivec_ptr), sizeof(iv));
			const auto* in  = reinterpret_cast<const uint8_t*>(in_ptr);
			auto*       out = reinterpret_cast<uint8_t*>(out_ptr);
			uint8_t     blk[16], res[16], tmp[16];
			for (uint64_t off = 0; off < length; off += 16) {
				std::memcpy(blk, in + off, sizeof(blk));
				if (enc != 0) {
					for (int i = 0; i < 16; i++) {
						blk[i] ^= iv[i];
					}
					AesEncryptBlock(blk, res, &k);
					std::memcpy(out + off, res, sizeof(res));
					std::memcpy(iv, res, sizeof(iv));
				} else {
					std::memcpy(tmp, blk, sizeof(tmp));
					AesDecryptBlock(blk, res, &k);
					for (int i = 0; i < 16; i++) {
						res[i] ^= iv[i];
					}
					std::memcpy(out + off, res, sizeof(res));
					std::memcpy(iv, tmp, sizeof(iv));
				}
			}
			std::memcpy(reinterpret_cast<void*>(ivec_ptr), iv, sizeof(iv));
		}
	}
	LOGF("\t in=0x%016" PRIx64 " out=0x%016" PRIx64 " len=%" PRIu64 " enc=%" PRIu64 "%s%s\n",
	     in_ptr, out_ptr, length, enc, why != nullptr ? " (no-op: " : "",
	     why != nullptr ? why : "");
}

LIB_DEFINE(InitShellHitsLibreSSLBoot15_1) {
	LIB_FUNC("RpzLa-jjWnA", LibreSSLBoot15::AesSetEncryptKey); // AES_set_encrypt_key (request key)
	LIB_FUNC("Z55J-I6291o", LibreSSLBoot15::AesCbcCrypt);     // AES_cbc_encrypt (request key)
}

} // namespace LibreSSLBoot15

LIB_DEFINE(InitShellHits_1) {
	Kernel::InitShellHitsKernel_1(s);
	Dipsw::InitShellHitsDipsw_1(s);
	Ipmi::InitShellHitsIpmi_1(s);
	AvSetting::InitShellHitsAvSetting_1(s);
	FsInternalForVsh::InitShellHitsFsVsh_1(s);
	AppMessaging::InitShellHitsAppMsg_1(s);
	SysCore::InitShellHitsSysCore_1(s);
	LibcInternalLate::InitShellHitsLibcLate_1(s);
	KernelLate::InitShellHitsKernelLate_1(s);
	RegMgrLate::InitShellHitsRegMgrLate_1(s);
	NetLate::InitShellHitsNetLate_1(s);
	NetCtlLate::InitShellHitsNetCtlLate_1(s);
	HttpCacheLate::InitShellHitsHttpCacheLate_1(s);
	IpmiLate::InitShellHitsIpmiLate_1(s);
	HttpLate::InitShellHitsHttpLate_1(s);
	NpCommonLate::InitShellHitsNpLate_1(s);
	VrTracker2Boot11::InitShellHitsVrTracker2Boot11_1(s);
	KernelBoot11::InitShellHitsKernelBoot11_1(s);
	NetBoot11::InitShellHitsNetBoot11_1(s);
	LibcInternalBoot11::InitShellHitsLibcBoot11_1(s);
	NpCommonBoot11::InitShellHitsNpCommonBoot11_1(s);
	NpWebApi2Boot11::InitShellHitsNpWebApi2Boot11_1(s);
	Json2Boot11::InitShellHitsJson2Boot11_1(s);
	RegMgrBoot11::InitShellHitsRegMgrBoot11_1(s);
	MbusBoot11::InitShellHitsMbusBoot11_1(s);
	AbstractStorageBoot11::InitShellHitsAbstractStorageBoot11_1(s);
	LibreSSLBoot11::InitShellHitsLibreSSLBoot11_1(s);
	NpManagerBoot11::InitShellHitsNpManagerBoot11_1(s);
	HttpCacheBoot11_10::InitShellHitsHttpCacheBoot11_10_1(s);
	Boot12::InitShellHitsAudioOutBoot12_1(s);
	Boot12Json::InitShellHitsJsonBoot12_1(s);
	Boot12Http::InitShellHitsHttpBoot12_1(s);
	Boot12Kernel::InitShellHitsKernelBoot12_1(s);
	NpCommonBoot14::InitShellHitsNpCommonBoot14_1(s);
	SystemLogger2Boot14::InitShellHitsSystemLogger2Boot14_1(s);
	HttpBoot14::InitShellHitsHttpBoot14_1(s);
	FsVshBoot14::InitShellHitsFsVshBoot14_1(s);
	RtcBoot14::InitShellHitsRtcBoot14_1(s);
	LibreSSLBoot15::InitShellHitsLibreSSLBoot15_1(s);
}

} // namespace ShellHits
} // namespace Libs

// ---- registration snippet for src/libs/libs.cpp (append at end of file) ----
/*
namespace ShellHits {
LIB_DEFINE(InitShellHits_1);
} // namespace ShellHits
*/
// ---- and inside Libs::InitAll, AFTER the last existing call
// ---- (InitWebBrowserDialog_1(s);), add: -------------------------------------
/*
	ShellHits::InitShellHits_1(s);
*/
// NOTE: placement at the END of InitAll is load-bearing. These NIDs were
// previously registered as LogStubs under identical exact keys; the symbol
// database is insert_or_assign, so the last registration wins both exact and
// NID-fallback resolution.
