#include "common/abi.h"
#include "common/common.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "libs/libGuestFile.h"
#include "libs/errno.h"
#include "libs/libs.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <string>
#include <limits>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::GuestFile {

namespace {

constexpr uint64_t kMagic = 0x4746494C4553484Dull; // "GFILSHM"

struct Shim {
	uint64_t    magic = kMagic;
	FILE*       host  = nullptr;
	std::string path;
	bool        trace = false;
};

std::mutex g_ownMutex;
std::unordered_set<const void*> g_own;

bool ShimOk(const void* p) {
	std::lock_guard<std::mutex> lock(g_ownMutex);
	return g_own.find(p) != g_own.end();
}

bool DstOk(void* p, size_t n) {
	if (p == nullptr || n == 0) {
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const uint64_t v = reinterpret_cast<uint64_t>(p);
	if (v + n < v) {
		return false;
	}
	MEMORY_BASIC_INFORMATION mi {};
	if (VirtualQuery(p, &mi, sizeof(mi)) == 0 || mi.State != MEM_COMMIT) {
		return false;
	}
	const DWORD prot = mi.Protect & 0xffu;
	if (!(prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
	      prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY)) {
		return false;
	}
	const uint64_t base = reinterpret_cast<uint64_t>(mi.BaseAddress);
	const uint64_t size = static_cast<uint64_t>(mi.RegionSize);
	if (v < base || v + n > base + size) {
		return false;
	}
#endif
	return true;
}

} // namespace

// Cross-family shim registry (see libGuestFile.h): exact-pointer mapping
// for shims owned by other families (e.g. libC GuestFopen shims read via
// CFread). Lookups never probe memory.
namespace {
std::mutex g_foreignMutex;
struct ForeignStream { FILE* host; ForeignClose close; };
std::unordered_map<const void*, ForeignStream> g_foreign;
} // namespace

void RegisterForeignShim(const void* shim, FILE* host, ForeignClose close) {
	if (shim == nullptr || host == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_foreignMutex);
	g_foreign[shim] = {host, close};
}

void UnregisterForeignShim(const void* shim) {
	if (shim == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_foreignMutex);
	g_foreign.erase(shim);
}

FILE* ForeignShimHost(const void* shim) {
	if (shim == nullptr) {
		return nullptr;
	}
	// Retail XF also mixes GuestFopen with CFread/CFseek in VSH chain boots.
	// Registration, not boot mode, establishes that this is an emulator stream.
	return RegisteredForeignShimHost(shim);
}

FILE* RegisteredForeignShimHost(const void* shim) {
	std::lock_guard<std::mutex> lock(g_foreignMutex);
	const auto it = g_foreign.find(shim);
	return it != g_foreign.end() ? it->second.host : nullptr;
}

FILE* OwnShimHost(const void* shim) {
	if (!ShimOk(shim)) {
		return nullptr;
	}
	return static_cast<const Shim*>(shim)->host;
}

const char* ShimPath(const void* shim) {
	if (!ShimOk(shim)) {
		return nullptr;
	}
	return static_cast<const Shim*>(shim)->path.c_str();
}

void* Open(const char* guest_path, int flags) {
	if (guest_path == nullptr || guest_path[0] == '\0') {
		return nullptr;
	}
	const auto resolved = LibKernel::FileSystem::ResolveGuestPath(guest_path);
	if ((resolved.error != OK && !(resolved.error == LibKernel::KERNEL_ERROR_ENOENT && resolved.unmounted_fallback)) || (resolved.read_only &&
	    ((flags & (0x200 | 0x100 | 0x8)) != 0 || (flags & 3) == 2))) {
		*Posix::GetErrorAddr() = resolved.error != OK ? LibKernel::KernelToPosix(resolved.error) : Posix::POSIX_EROFS;
		return nullptr;
	}
	guest_path = resolved.guest.c_str();
	const auto& real0 = resolved.host;
	// ps5-vsh: unmounted guest paths (per-boot random-word sandboxes) get
	// jail backing + license seeding, mirroring KernelOpen/GuestFopen.
	std::filesystem::path real = real0;
	if (real.empty() && resolved.unmounted_fallback) {
		Libs::FsSandbox::EnsureLicenseSeed(guest_path, resolved.fallback_data_root);
		real = Libs::FsSandbox::BackingForUnmounted(guest_path, false, resolved.fallback_data_root);
	}
	if (real.empty()) {
		LOGF("\t guestfile open: no mount for \"%s\" -> NULL\n", guest_path);
		return nullptr;
	}
	// MSVC _O_xxx openmode mapping. accmode 1 (_O_WRONLY) is observed on
	// pure reads (JSCd cert bundle), so never truncate on it: reads win
	// unless TRUNC is explicitly set. Destructive opens stay visible.
	const int acc = flags & 3;
	const char* mode = "rb";
	if ((flags & 0x200) != 0) {
		mode = "wb";
	} else if ((flags & 0x8) != 0) {
		mode = "ab";
	} else if (acc == 2) {
		mode = "r+b";
	}
	FILE* host = std::fopen(real.string().c_str(), mode);
	if (host == nullptr && (flags & 0x100) != 0) {
		host = std::fopen(real.string().c_str(), "w+b");
	}
	if (host == nullptr) {
		LOGF("\t guestfile open: \"%s\" (real: %s) mode %s -> ENOENT\n", guest_path,
		     real.string().c_str(), mode);
		return nullptr;
	}
	auto* shim = new (std::nothrow) Shim;
	if (shim == nullptr) {
		std::fclose(host);
		return nullptr;
	}
	shim->host  = host;
	shim->path  = guest_path;
	shim->trace = shim->path.find(".ps.bundle") != std::string::npos;
	LOGF("\t guestfile open: \"%s\" mode %s -> shim %p\n", guest_path, mode,
	     static_cast<void*>(shim));
	if (shim->trace) {
		// ps5-vsh: RNPS staging diagnostic. The guest path can resolve to
		// either the staged plaintext payload or the firmware's encrypted
		// container; log the resolved host file and its first qword so the
		// answer is visible (HBC magic vs "RNPSHEDR").
		std::error_code ec;
		const auto      size = std::filesystem::file_size(real, ec);
		unsigned char   head[8] {};
		size_t          got = 0;
		if (FILE* probe = std::fopen(real.string().c_str(), "rb")) {
			got = std::fread(head, 1, sizeof(head), probe);
			std::fclose(probe);
		}
		LOGF("\t guestfile TRACE: \"%s\" real=%s size=%llu head=%02x%02x%02x%02x%02x%02x%02x%02x (n=%llu)\n",
		     guest_path, real.string().c_str(),
		     static_cast<unsigned long long>(ec ? 0 : size),
		     head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7],
		     static_cast<unsigned long long>(got));
	}
	{
		std::lock_guard<std::mutex> lock(g_ownMutex);
		g_own.insert(shim);
	}
	return shim;
}

size_t Read(void* stream, void* dst, size_t size, size_t count) {
	FILE* host = OwnShimHost(stream);
	if (host == nullptr) {
		host = ForeignShimHost(stream);
	}
	if (host == nullptr) {
		return 0;
	}
	if (size == 0 || count == 0) {
		return 0;
	}
	if (count > std::numeric_limits<size_t>::max() / size) { return 0; }
	if (!DstOk(dst, size * count) && !DstOk(dst, 1)) {
		return 0;
	}
	const size_t got = std::fread(dst, size, count, host);
	if (ShimOk(stream)) {
		const auto* sh = static_cast<const Shim*>(stream);
		if (sh->trace && got > 0) {
			// ps5-vsh: RNPS staging diagnostic (see Open).
			char   hex[33] = {};
			size_t n       = got * size < 16 ? got * size : 16;
			static const char* kD = "0123456789abcdef";
			for (size_t i = 0; i < n; i++) {
				hex[2 * i]     = kD[(static_cast<const unsigned char*>(dst)[i] >> 4) & 0xf];
				hex[2 * i + 1] = kD[static_cast<const unsigned char*>(dst)[i] & 0xf];
			}
			LOGF("\t guestfile TRACE read: \"%s\" size=%llu count=%llu -> %llu pos=%ld head=%s\n",
			     sh->path.c_str(), static_cast<unsigned long long>(size),
			     static_cast<unsigned long long>(count),
			     static_cast<unsigned long long>(got * size), std::ftell(host), hex);
			// ps5-vsh: read-only probe of libNativeExtensions' bundle-policy
			// word (sprx va 0x9c350, module base 0xdf0000000). Low byte is
			// the first bool passed to _sceRnpsBundleRegistryInit(); nonzero
			// makes 0x1f060 throw "Bundle file must be encrypted." instead
			// of accepting a plaintext payload.
			unsigned char policy[2] = {0xff, 0xff};
			const bool    policy_ok =
			    LibKernel::Memory::TryReadBacking(0xdf009c350ull, policy, sizeof(policy));
			LOGF("\t guestfile TRACE policy @0xdf009c350 = %02x %02x (read=%d)\n", policy[0],
			     policy[1], policy_ok ? 1 : 0);
		}
	}
	// ps5-vsh: RecordFile magic probe (JSCd entitlement): show first bytes
	// of small header reads so the expected magic is observable.
	if (got > 0 && size * count <= 64) {
		char hex[129] = {};
		const size_t n =
		    got * size < sizeof(hex) / 2 - 1 ? got * size : sizeof(hex) / 2 - 1;
		static const char* kD = "0123456789abcdef";
		for (size_t i = 0; i < n; i++) {
			hex[2 * i]     = kD[(static_cast<const unsigned char*>(dst)[i] >> 4) & 0xf];
			hex[2 * i + 1] = kD[static_cast<const unsigned char*>(dst)[i] & 0xf];
		}
		LOGF("\t guestfile read %llu x %llu -> %llu bytes %s\n",
		     (unsigned long long)size, (unsigned long long)count,
		     (unsigned long long)(got * size), hex);
	}
	return got;
}

int Close(void* stream) {
	bool owned = false;
	{
		std::lock_guard<std::mutex> lock(g_ownMutex);
		owned = g_own.erase(stream) != 0;
	}
	if (owned) {
		auto* shim = static_cast<Shim*>(stream);
		const int rc = shim->host != nullptr ? std::fclose(shim->host) : 0;
		shim->host  = nullptr;
		delete shim;
		return rc;
	}
	// Return cleanup to the registered owner, outside the registry lock (the
	// owner unregisters before destroying its wrapper). Never guess its layout.
	ForeignClose close = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_foreignMutex);
		if (const auto found = g_foreign.find(stream); found != g_foreign.end()) {
			close = found->second.close;
		}
	}
	return close != nullptr ? close(stream) : 0;
}

int Seek(void* stream, long offset, int whence) {
	FILE* host = OwnShimHost(stream);
	if (host == nullptr) {
		host = ForeignShimHost(stream);
	}
	if (host == nullptr) {
		return -1;
	}
	return std::fseek(host, offset, whence);
}

long Tell(void* stream) {
	FILE* host = OwnShimHost(stream);
	if (host == nullptr) {
		host = ForeignShimHost(stream);
	}
	if (host == nullptr) {
		return -1L;
	}
	return std::ftell(host);
}

int Getc(void* stream) {
	FILE* host = OwnShimHost(stream);
	if (host == nullptr) {
		host = ForeignShimHost(stream);
	}
	if (host == nullptr) {
		return -1;
	}
	return std::fgetc(host);
}

} // namespace Libs::GuestFile
