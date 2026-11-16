#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/host_gpu/hostMemory.h"
#include "kernel/eventFlag.h"
#include "kernel/eventQueue.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/module.h"
#include "kernel/pthread.h"
#include "kernel/semaphore.h"
#include "kernel/syncOnAddress.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "libs/libShellCoreFloor.h"
#include "libs/processSdk.h"
#include "loader/elf.h"
#include "loader/kernelTextLocation.h"
#include "loader/runtimeLinker.h"
#include "loader/applicationImportData.h"
#include "loader/runtimeLaunch.h"
#include "loader/runtimeProcessIdentity.h"
#include "libs/ipmiObserve.h"
#include "kernel/socketpairProvenance.h"
#include "libs/kernelBacktrace.h"
#include "libs/standardWrite.h"
#include "loader/systemContent.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#elif defined(__APPLE__)
#include <csignal>
#include <pthread.h>
#include <sys/ucontext.h>
#else
#include <csignal>
#include <pthread.h>
#include <sys/uio.h>
#include <unistd.h>
#include <ucontext.h>
#endif

#include "libs/semaphoreProvenance.inc"

namespace Libs {

namespace LibC {
int          GetArgc();
const char** GetArgv();
} // namespace LibC

namespace LibcInternal {
// ps5-vsh: libScePosix kqueue bridge implemented in libC.cpp.
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKqueue();
void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GuestMmap(void* addr, size_t len, int prot, int flags, int fd, int64_t offset);
} // namespace LibcInternal

LIB_VERSION("libkernel", 1, "libkernel", 1, 1);

namespace LibKernelApr {
LIB_DEFINE(InitLibKernel_1_Apr);
} // namespace LibKernelApr

namespace LibKernel {

using get_thread_atexit_count_func_t = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int (*)(KernelModule);
using thread_atexit_report_func_t    = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void (*)(KernelModule);

static std::array<uint8_t, 20> sha1_digest(const uint8_t* data, size_t size) {
	uint32_t h0 = 0x67452301u;
	uint32_t h1 = 0xefcdab89u;
	uint32_t h2 = 0x98badcfeu;
	uint32_t h3 = 0x10325476u;
	uint32_t h4 = 0xc3d2e1f0u;

	std::vector<uint8_t> msg(data, data + size);
	const uint64_t       bit_len = static_cast<uint64_t>(size) * 8u;

	msg.push_back(0x80u);
	while ((msg.size() % 64u) != 56u) {
		msg.push_back(0u);
	}
	for (int i = 7; i >= 0; i--) {
		msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xffu));
	}

	for (size_t offset = 0; offset < msg.size(); offset += 64u) {
		uint32_t w[80] = {};
		for (int i = 0; i < 16; i++) {
			const auto j = offset + static_cast<size_t>(i) * 4u;
			w[i] = (static_cast<uint32_t>(msg[j + 0]) << 24u) |
			       (static_cast<uint32_t>(msg[j + 1]) << 16u) |
			       (static_cast<uint32_t>(msg[j + 2]) << 8u) | static_cast<uint32_t>(msg[j + 3]);
		}
		for (int i = 16; i < 80; i++) {
			w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		}

		uint32_t a = h0;
		uint32_t b = h1;
		uint32_t c = h2;
		uint32_t d = h3;
		uint32_t e = h4;

		for (int i = 0; i < 80; i++) {
			uint32_t f = 0;
			uint32_t k = 0;
			if (i < 20) {
				f = (b & c) | ((~b) & d);
				k = 0x5a827999u;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ed9eba1u;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdcu;
			} else {
				f = b ^ c ^ d;
				k = 0xca62c1d6u;
			}

			const auto temp = std::rotl(a, 5) + f + e + k + w[i];
			e               = d;
			d               = c;
			c               = std::rotl(b, 30);
			b               = a;
			a               = temp;
		}

		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d;
		h4 += e;
	}

	std::array<uint8_t, 20> digest = {};
	const uint32_t          h[5]   = {h0, h1, h2, h3, h4};
	for (int i = 0; i < 5; i++) {
		digest[static_cast<size_t>(i) * 4u + 0u] = static_cast<uint8_t>((h[i] >> 24u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 1u] = static_cast<uint8_t>((h[i] >> 16u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 2u] = static_cast<uint8_t>((h[i] >> 8u) & 0xffu);
		digest[static_cast<size_t>(i) * 4u + 3u] = static_cast<uint8_t>(h[i] & 0xffu);
	}
	return digest;
}

static std::string kernel_symbol_to_nid(const char* symbol) {
	static constexpr std::array<uint8_t, 16> salt = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde,
	                                                 0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1,
	                                                 0xc3, 0xe5, 0x52, 0x30};
	static constexpr char                    codes[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

	const auto           symbol_size = std::strlen(symbol);
	std::vector<uint8_t> input(symbol_size + salt.size());
	std::memcpy(input.data(), symbol, symbol_size);
	std::memcpy(input.data() + symbol_size, salt.data(), salt.size());

	const auto hash = sha1_digest(input.data(), input.size());

	uint64_t digest = 0;
	std::memcpy(&digest, hash.data(), sizeof(digest));

	char nid[12] = {};
	for (int i = 0; i < 10; i++) {
		nid[i] = codes[(digest >> (58 - i * 6)) & 0x3fu];
	}
	nid[10] = codes[(digest & 0xfu) * 4u];
	return std::string(nid);
}

static const Loader::SymbolRecord* kernel_find_export_symbol(const Loader::SymbolDatabase* symbols,
                                                             const char*                   symbol) {
	EXIT_IF(symbols == nullptr);
	EXIT_IF(symbol == nullptr);

	const auto nid = kernel_symbol_to_nid(symbol);

	if (const auto* record = symbols->FindByNid(nid, Loader::SymbolType::Func); record != nullptr) {
		return record;
	}
	if (const auto* record = symbols->FindByNid(nid, Loader::SymbolType::Object);
	    record != nullptr) {
		return record;
	}
	if (const auto* record = symbols->FindByNid(nid, Loader::SymbolType::NoType);
	    record != nullptr) {
		return record;
	}

	const auto symbol_name = std::string(symbol);
	if (const auto* record = symbols->FindByName(symbol_name, Loader::SymbolType::Func);
	    record != nullptr) {
		return record;
	}
	if (const auto* record = symbols->FindByName(symbol_name, Loader::SymbolType::Object);
	    record != nullptr) {
		return record;
	}
	return symbols->FindByName(symbol_name, Loader::SymbolType::NoType);
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelApplicationHeapGetMem(uint64_t alignment, uint64_t size) {
	static std::atomic_uint32_t log_count = 0;

	if (alignment < 0x10u) {
		alignment = 0x10u;
	}
	if ((alignment & (alignment - 1u)) != 0u) {
		return nullptr;
	}

	auto* rt  = Common::Singleton<Loader::RuntimeLinker>::Instance();
	void* ptr = rt->ApplicationHeapMemalign(alignment, size);

	const auto index = log_count.fetch_add(1, std::memory_order_relaxed);
	if (index < 8 || (index % 600) == 0) {
		LOGF("\t KernelApplicationHeapGetMem alignment = 0x%016" PRIx64 ", size = 0x%016" PRIx64
		     ", ptr = 0x%016" PRIx64 "\n",
		     alignment, size, reinterpret_cast<uint64_t>(ptr));
	}

	return ptr;
}

static uint64_t g_real_get_psm_ini_file = 0;
uint64_t g_psm_ini_struct_addr = 0;

using ini_func_t = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                              uint64_t);

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t GetPsmIniFileLogged(uint64_t a0, uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4, uint64_t a5) {
	uint64_t ret = 0;
	if (g_real_get_psm_ini_file != 0) {
		auto* real = reinterpret_cast<ini_func_t>(g_real_get_psm_ini_file);
		ret        = real(a0, a1, a2, a3, a4, a5);
	}
	LOGF("ps5-vsh: GetPsmIniFile(%llx) -> %llx\n", (unsigned long long)a0,
	     (unsigned long long)ret);
	if (a0 != 0) {
		uint64_t out0 = 0, out1 = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		__try {
			out0 = *reinterpret_cast<const uint64_t*>(a0);
			out1 = *reinterpret_cast<const uint64_t*>(a0 + 8);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			out0 = 0;
		}
#else
		out0 = *reinterpret_cast<const uint64_t*>(a0);
		out1 = *reinterpret_cast<const uint64_t*>(a0 + 8);
#endif
		LOGF("ps5-vsh: GetPsmIniFile out=[%llx, %llx]\n", (unsigned long long)out0,
		     (unsigned long long)out1);
		if (out0 != 0) {
			g_psm_ini_struct_addr = out0;
			uint64_t words[16] = {};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			__try {
				for (int i = 0; i < 16; i++) {
					words[i] = reinterpret_cast<const uint64_t*>(out0)[i];
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
#else
			for (int i = 0; i < 16; i++) {
				words[i] = reinterpret_cast<const uint64_t*>(out0)[i];
			}
#endif
			LOGF("ps5-vsh: GetPsmIniFile struct: %llx %llx %llx %llx\n"
			     "\t %llx %llx %llx %llx\n"
			     "\t %llx %llx %llx %llx\n"
			     "\t %llx %llx %llx %llx\n",
			     (unsigned long long)words[0], (unsigned long long)words[1],
			     (unsigned long long)words[2], (unsigned long long)words[3],
			     (unsigned long long)words[4], (unsigned long long)words[5],
			     (unsigned long long)words[6], (unsigned long long)words[7],
			     (unsigned long long)words[8], (unsigned long long)words[9],
			     (unsigned long long)words[10], (unsigned long long)words[11],
			     (unsigned long long)words[12], (unsigned long long)words[13],
			     (unsigned long long)words[14], (unsigned long long)words[15]);
		}
	}
	return ret;
}

// ps5-vsh gate1b: ReactNative.PUI resolves `_sceRnpsUpdateAppData` out of
// libNativeExtensions.sprx via DllImport/dlsym. The native side parses the
// managed app-data JSON with folly and dies on a folly::ConversionError, so log
// the exact buffer the managed side hands over (length + head + raw bytes) and
// then run the real export untouched. Diagnostic only: no behaviour change.
static uint64_t g_real_rnps_update_app_data = 0;

using rnps_update_app_data_func_t = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t (*)(
    uint64_t, const char*);

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t
RnpsUpdateAppDataLogged(uint64_t a0, const char* a1) {
	const auto len  = (a1 != nullptr) ? std::strlen(a1) : static_cast<size_t>(0);
	const auto head = (a1 != nullptr) ? std::string(a1, std::min<size_t>(len, 2048)) : std::string("(null)");
	std::string raw;
	for (size_t i = 0; i < 48 && i < len; i++) {
		raw += fmt::sprintf("%02x ", static_cast<unsigned>(static_cast<unsigned char>(a1[i])));
	}
	LOGF("ps5-vsh: RnpsUpdateAppData handle=0x%016" PRIx64 " strlen=%zu\n", a0, len);
	LOGF("ps5-vsh: RnpsUpdateAppData json=%s\n", head.c_str());
	LOGF("ps5-vsh: RnpsUpdateAppData raw=[%s]\n", raw.c_str());
	uint64_t ret = 0;
	if (g_real_rnps_update_app_data != 0) {
		auto* real = reinterpret_cast<rnps_update_app_data_func_t>(g_real_rnps_update_app_data);
		ret        = real(a0, a1);
		LOGF("ps5-vsh: RnpsUpdateAppData -> 0x%016" PRIx64 "\n", ret);
	} else {
		LOGF("ps5-vsh: RnpsUpdateAppData real address missing\n");
	}
	return ret;
}

// ps5-vsh: log-then-call diagnostic for _sceRnpsBundleRegistryInit. The
// managed side passes (bool, bool); the first bool is stored into the
// libNativeExtensions bundle-policy word (sprx va 0x9c350). Nonzero makes
// the bundle reader throw "Bundle file must be encrypted." instead of
// accepting a plaintext payload. Diagnostic only: the real export still runs.
static uint64_t g_real_rnps_bundle_registry_init = 0;

using rnps_bundle_registry_init_func_t = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t (*)(
    uint64_t, uint64_t);

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t
RnpsBundleRegistryInitLogged(uint64_t a0, uint64_t a1) {
	LOGF("ps5-vsh: RnpsBundleRegistryInit(a0=%llu a1=%llu) -> forcing a0=0 (allow plaintext Hermes HBC)\n",
	     a0 & 0xffull, a1 & 0xffull);
	uint64_t ret = 0;
	if (g_real_rnps_bundle_registry_init != 0) {
		ret = reinterpret_cast<rnps_bundle_registry_init_func_t>(
		          g_real_rnps_bundle_registry_init)(0, a1);
	} else {
		LOGF("ps5-vsh: RnpsBundleRegistryInit real address missing\n");
	}
	LOGF("ps5-vsh: RnpsBundleRegistryInit -> 0x%016llx\n",
	     static_cast<unsigned long long>(ret));
	// ps5-vsh: read back and force the policy word to 0 (allow unencrypted Hermes bundles).
	// va 0x1e860 is _sceRnpsBundleRegistryInit, va 0x9c350 the policy word.
	const uint64_t policy_va =
	    g_real_rnps_bundle_registry_init - 0x1e860ull + 0x9c350ull;
	if (Graphics::HostMemoryIsReadable(policy_va)) {
		*reinterpret_cast<volatile unsigned char*>(policy_va) = 0;
	}
	unsigned char policy[2] = {0xff, 0xff};
	const bool policy_ok = Graphics::HostMemoryIsReadable(policy_va) &&
	                       Graphics::HostMemoryIsReadable(policy_va + 1);
	if (policy_ok) {
		policy[0] = *reinterpret_cast<volatile unsigned char*>(policy_va);
		policy[1] = *reinterpret_cast<volatile unsigned char*>(policy_va + 1);
	}
	LOGF("ps5-vsh: policy word @0x%016" PRIx64 " = %02x %02x (read=%d)\n", policy_va,
	     policy[0], policy[1], policy_ok ? 1 : 0);
	return ret;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDlsym(KernelModule handle, const char* symbol, void** addr) {
	static std::atomic_uint32_t log_count = 0;
	const auto                  index     = log_count.fetch_add(1, std::memory_order_relaxed);
	if (index < 32 || (index % 600) == 0) {
		PRINT_NAME();
		LOGF("\t call_count = %" PRIu32 ", handle = %" PRId32 ", symbol = %s\n", index + 1, handle,
		     symbol != nullptr ? symbol : "(null)");
	}

	if (addr == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (symbol == nullptr) {
		*addr = nullptr;
		return KERNEL_ERROR_EFAULT;
	}

	// ps5-vsh: the RN path resolves through dlsym at a high call index, which
	// the sampled logging below would miss. Always show these.
	const bool trace_symbol = std::strstr(symbol, "PreloadHostDll") != nullptr ||
	                          std::strstr(symbol, "Rnps") != nullptr ||
	                          std::strstr(symbol, "rnps") != nullptr;
	if (trace_symbol) {
		LOGF("\t dlsym(handle = %" PRId32 ", symbol = %s)\n", handle, symbol);
	}

	auto* rt      = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = rt->FindProgramById(handle);
	if (const auto* current = Loader::RuntimeLinker::GetCurrentProgram(); current && current->application_owner) {
		return rt->ResolveApplicationDlsym(program, symbol, kernel_symbol_to_nid(symbol), addr);
	}
	if (program == nullptr || program->export_symbols == nullptr) {
		*addr = nullptr;
		LOGF("\t unresolved dlsym: handle = %" PRId32 ", symbol = %s, reason = module not found\n",
		     handle, symbol);
		return KERNEL_ERROR_ESRCH;
	}
	// Bounded run6 split-state diagnostic. P/Invoke resolves by module handle,
	// independently of PSM's ordinary imports; record the actual provider.
	const bool composite = Common::EqualNoCase(program->file_name.filename().string(), "libSceComposite.sprx") ||
	                       std::strcmp(symbol, "sceCompsoitorGetProcessRenderingTime") == 0 ||
	                       std::strcmp(symbol, "EcTHemmz6n4") == 0;
	static std::atomic_uint32_t composite_dlsym_count {0};
	const bool trace_composite = composite && composite_dlsym_count.fetch_add(1, std::memory_order_relaxed) < 64;
	const auto trace_composite_result = [&](const Loader::Program* provider, uint64_t address) {
		if (!trace_composite) { return; }
		const auto* requester = Loader::RuntimeLinker::GetCurrentProgram();
		const auto* owner = requester != nullptr ? requester->application_owner : nullptr;
		LOGF("ps5-vsh: Composite dlsym handle=%d symbol=%s requester=%d owner=%d provider=%d provider-owner=%d address=0x%016" PRIx64 " path=%s\n",
		     handle, symbol, requester != nullptr ? requester->unique_id : -1,
		     owner != nullptr ? owner->unique_id : -1, provider != nullptr ? provider->unique_id : -1,
		     provider != nullptr && provider->application_owner != nullptr ? provider->application_owner->unique_id : -1,
		     address, provider != nullptr ? Common::PathToString(provider->file_name).c_str() : "<unresolved>");
	};
	uint64_t native_ssl_address = 0;
	if (Loader::NativeSslSmoke::ProductionRequested() &&
	    rt->ResolveNativeSslDlsym(program, symbol, kernel_symbol_to_nid(symbol), &native_ssl_address)) {
		*addr = reinterpret_cast<void*>(native_ssl_address);
		return native_ssl_address != 0 ? OK : KERNEL_ERROR_ESRCH;
	}

	const auto* record = kernel_find_export_symbol(program->export_symbols.get(), symbol);
	if (record != nullptr) {
		*addr = reinterpret_cast<void*>(record->vaddr);
		trace_composite_result(program, record->vaddr);
		if (index < 32 || (index % 600) == 0 || trace_symbol) {
			LOGF("\t resolved = 0x%016" PRIx64 " (%s)\n", record->vaddr, record->name.c_str());
		}
		if (std::strcmp(symbol, "_sceRnpsUpdateAppData") == 0) {
			g_real_rnps_update_app_data = record->vaddr;
			*addr                       = reinterpret_cast<void*>(&RnpsUpdateAppDataLogged);
			LOGF("\t RnpsUpdateAppData interposed (real = 0x%016" PRIx64 ")\n",
			     g_real_rnps_update_app_data);
		}
		if (std::strcmp(symbol, "_sceRnpsBundleRegistryInit") == 0) {
			g_real_rnps_bundle_registry_init = record->vaddr;
			*addr                             = reinterpret_cast<void*>(&RnpsBundleRegistryInitLogged);
			LOGF("\t RnpsBundleRegistryInit interposed (real = 0x%016" PRIx64 ")\n",
			     g_real_rnps_bundle_registry_init);
		}
		return OK;
	}

	if (handle == 0 && std::strcmp(symbol, "scriptingGetMem") == 0) {
		*addr = reinterpret_cast<void*>(&KernelApplicationHeapGetMem);
		if (index < 32 || (index % 600) == 0) {
			LOGF("\t resolved = 0x%016" PRIx64 " (application heap scriptingGetMem)\n",
			     reinterpret_cast<uint64_t>(*addr));
		}
		return OK;
	}

	{
		const auto nid = kernel_symbol_to_nid(symbol);
		for (const auto* mod : rt->GetVisiblePrograms(Loader::RuntimeLinker::GetCurrentProgram())) {
			if (mod == nullptr || mod == program || mod->export_symbols == nullptr) {
				continue;
			}
			if (const auto* grec = kernel_find_export_symbol(mod->export_symbols.get(), symbol);
			    grec != nullptr) {
				*addr = reinterpret_cast<void*>(grec->vaddr);
				trace_composite_result(mod, grec->vaddr);
				LOGF("\t resolved (global) = 0x%016" PRIx64 " (%s in %s)\n", grec->vaddr,
				     grec->name.c_str(), Common::PathToString(mod->file_name).c_str());
				if (std::strcmp(symbol, "scePsmUtilGetPsmIniFile") == 0) {
					g_real_get_psm_ini_file = reinterpret_cast<uint64_t>(*addr);
					*addr = reinterpret_cast<void*>(&GetPsmIniFileLogged);
					LOGF("\t GetPsmIniFile interposed\n");
					return OK;
				}
				if (grec->name.size() >= 6 &&
				    grec->name.compare(grec->name.size() - 6, 6, "[Func]") == 0) {
					*addr = reinterpret_cast<void*>(
					    Loader::TraceWrapImport(reinterpret_cast<uint64_t>(*addr), symbol));
				}
				return OK;
			}
		}
	}

	*addr = nullptr;
	trace_composite_result(nullptr, 0);
	LOGF("\t unresolved dlsym: handle = %" PRId32 ", module = %s, symbol = %s\n", handle,
	     Common::PathToString(program->file_name).c_str(), symbol);
	return KERNEL_ERROR_ESRCH;
}

#pragma pack(1)

struct KernelLoadModuleOpt {
	size_t size;
};

struct KernelUnloadModuleOpt {
	size_t size;
};

struct TlsInfo {
	Loader::Program* program;
	uint64_t         offset;
};

struct MallocReplace {
	uint64_t size               = sizeof(MallocReplace);
	void*    malloc_initialize  = nullptr;
	void*    malloc_finalize    = nullptr;
	void*    malloc             = nullptr;
	void*    free               = nullptr;
	void*    calloc             = nullptr;
	void*    realloc            = nullptr;
	void*    memalign           = nullptr;
	void*    reallocalign       = nullptr;
	void*    posix_memalign     = nullptr;
	void*    malloc_stats       = nullptr;
	void*    malloc_stats_fast  = nullptr;
	void*    malloc_usable_size = nullptr;
	void*    aligned_alloc      = nullptr;
};

struct NewReplace {
	uint64_t size                           = sizeof(NewReplace);
	void*    new_p                          = nullptr;
	void*    new_nothrow                    = nullptr;
	void*    new_array                      = nullptr;
	void*    new_array_nothrow              = nullptr;
	void*    delete_p                       = nullptr;
	void*    delete_nothrow                 = nullptr;
	void*    delete_array                   = nullptr;
	void*    delete_array_nothrow           = nullptr;
	void*    delete_with_size               = nullptr;
	void*    delete_with_size_nothrow       = nullptr;
	void*    delete_array_with_size         = nullptr;
	void*    delete_array_with_size_nothrow = nullptr;
};

static_assert(sizeof(MallocReplace) == sizeof(Loader::ApplicationSanitizerMallocTable));
static_assert(sizeof(NewReplace) == sizeof(Loader::ApplicationSanitizerNewTable));

struct ModuleInfo {
	uint64_t     size;
	uint64_t     info[32];
	KernelModule handle;
	uint8_t      reserved[60];
	uint64_t     eh_frame_hdr_addr;
	uint8_t      pad[88];
};

struct ModuleInfoForUnwind {
	uint64_t st_size;
	char     name[256];
	uint64_t eh_frame_hdr_addr;
	uint64_t eh_frame_addr;
	uint64_t eh_frame_size;
	uint64_t seg0_addr;
	uint64_t seg0_size;
};

static_assert(sizeof(ModuleInfoForUnwind) == 304);

#pragma pack()

constexpr size_t PROGNAME_MAX_SIZE = 511;

static uint64_t             g_stack_chk_guard                     = 0xDeadBeef00000007;
static char                 g_progname_buf[PROGNAME_MAX_SIZE + 1] = {0};
static const char*          g_progname                            = g_progname_buf;
static std::atomic_uint64_t g_gpo_state_bits {0};

static std::atomic<get_thread_atexit_count_func_t> g_get_thread_atexit_count_func {nullptr};
static std::atomic<thread_atexit_report_func_t>    g_thread_atexit_report_func {nullptr};

void SetProgName(const std::string& name) {
	strncpy(g_progname_buf, name.c_str(), PROGNAME_MAX_SIZE);
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int* get_error_addr() {
	return Posix::GetErrorAddr();
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int getargc() {
	PRINT_NAME();

	return LibC::GetArgc();
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI const char** getargv() {
	PRINT_NAME();

	return LibC::GetArgv();
}

static void dump_stack_chk_fail_context(uint64_t caller, uint64_t rsp) {
	LOGF("stack_chk_fail caller = 0x%016" PRIx64 "\n", caller);
	LOGF("stack_chk_fail guard  = 0x%016" PRIx64 " @ 0x%016" PRIx64 "\n", g_stack_chk_guard,
	     reinterpret_cast<uint64_t>(&g_stack_chk_guard));

	auto* linker  = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = linker->FindProgramByAddr(caller);

	LOGF("stack_chk_fail rsp    = 0x%016" PRIx64 "\n"
	     "stack_chk_fail caller/saved-guard scan:\n",
	     rsp);
	// ps5-vsh chainSUI321: single range probe (host guard pages fault on
	// raw reads; the scan ran unguarded 100k+ times on the hot loop).
	const bool stack_readable = Libs::Graphics::HostMemoryRangeIsReadable(
	    rsp - 256, 4352);
	for (int i = -32; i < 512 && stack_readable; i++) {
		auto addr  = rsp + static_cast<int64_t>(i) * 8;
		auto value = *reinterpret_cast<uint64_t*>(addr);

		if (value == caller) {
			auto saved_guard_addr = addr + 0x4a8;
			uint64_t saved_guard  = 0;
			if (Libs::Graphics::HostMemoryRangeIsReadable(saved_guard_addr, 8)) {
				saved_guard = *reinterpret_cast<uint64_t*>(saved_guard_addr);
			}
			LOGF("  return addr at 0x%016" PRIx64
			     "; saved guard candidate [ret+0x4a8] = 0x%016" PRIx64 " -> 0x%016" PRIx64 "\n",
			     addr, saved_guard_addr, saved_guard);
		}

		if (value == g_stack_chk_guard) {
			LOGF("  guard value found at stack 0x%016" PRIx64 "\n", addr);
		}
	}

	LOGF("stack_chk_fail stack qwords:\n");
	for (int i = -4; i < 20 && stack_readable; i++) {
		auto  addr  = rsp + static_cast<int64_t>(i) * 8;
		auto  value = *reinterpret_cast<uint64_t*>(addr);
		auto* p     = linker->FindProgramByAddr(value);
		auto  module_name =
		    (p == nullptr ? std::string() : Common::PathToString(p->file_name.filename()));
		LOGF("  [%+03d] 0x%016" PRIx64 " : 0x%016" PRIx64 " %s\n", i, addr, value,
		     module_name.c_str());
	}

	if (program == nullptr) {
		LOGF("stack_chk_fail caller module = <unknown>\n");
		return;
	}

	auto caller_module_name = Common::PathToString(program->file_name.filename());
	LOGF("stack_chk_fail caller module = %s, base = 0x%016" PRIx64 ", offset = 0x%016" PRIx64 "\n",
	     caller_module_name.c_str(), program->base_vaddr, caller - program->base_vaddr);

	auto read_byte = [program](uint64_t vaddr) {
		auto     aligned = vaddr & ~0x07ull;
		uint64_t value   = Loader::RuntimeLinker::ReadFromElf(program, aligned);
		return static_cast<uint8_t>((value >> ((vaddr - aligned) * 8u)) & 0xffu);
	};

	if (caller >= 5 && read_byte(caller - 5) == 0xe8) {
		uint32_t rel = 0;
		for (uint32_t i = 0; i < 4; i++) {
			rel |= static_cast<uint32_t>(read_byte(caller - 4 + i)) << (i * 8u);
		}
		auto target = caller + static_cast<int32_t>(rel);
		LOGF("stack_chk_fail preceding call = rel32 at 0x%016" PRIx64 ", target = 0x%016" PRIx64
		     "\n",
		     caller - 5, target);
	} else if (caller >= 6 && read_byte(caller - 6) == 0xff &&
	           (read_byte(caller - 5) & 0x38u) == 0x10) {
		LOGF("stack_chk_fail preceding call = indirect at 0x%016" PRIx64 "\n", caller - 6);
	}

	auto dump_start = (caller >= 64 ? caller - 64 : 0);
	auto dump_end   = caller + 32;

	LOGF("stack_chk_fail caller bytes from ELF:\n");
	for (auto addr = dump_start; addr < dump_end; addr += 16) {
		LOGF("  0x%016" PRIx64 ":", addr);
		for (uint32_t i = 0; i < 16; i++) {
			LOGF("%c%02" PRIx8, (addr + i == caller ? '>' : ' '), read_byte(addr + i));
		}
		LOGF("\n");
	}
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void stack_chk_fail() {
	PRINT_NAME();

	// ps5-vsh chainSUI164: capture the guest rbp too (our thunk preserves
	// it; the failing frame's canary slot is [rbp-8]). Read-only.
	// ps5-vsh chainSUI165: capture callee-saved walk state too (mono layout
	// checks walk lists in r13/r15/rbx; the failing node identifies the
	// corrupt object). Our thunk preserves these; read-only here.
	uint64_t rsp = 0, rbp = 0, grbx = 0, gr13 = 0, gr15 = 0, gr12 = 0;
	asm volatile("movq %%rsp, %0" : "=r"(rsp));
	asm volatile("movq %%rbp, %0" : "=r"(rbp));
	asm volatile("movq %%rbx, %0" : "=r"(grbx));
	asm volatile("movq %%r13, %0" : "=r"(gr13));
	asm volatile("movq %%r15, %0" : "=r"(gr15));
	asm volatile("movq %%r12, %0" : "=r"(gr12));
	uint64_t r13_0 = 0, r13_10 = 0;
	if (Libs::Graphics::HostMemoryRangeIsReadable(gr13, 24)) {
		r13_0 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(gr13));
		r13_10 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(gr13 + 0x10));
	}
	LOGF("stack_chk_fail walk: rbx=%llx r12=%llx r13=%llx [r13]=%llx [r13+10]=%llx r15=%llx\n",
	     (unsigned long long)grbx, (unsigned long long)gr12,
	     (unsigned long long)gr13, (unsigned long long)r13_0,
	     (unsigned long long)r13_10, (unsigned long long)gr15);
	// ps5-vsh chainSUI166: dump the node pointee + head + short chain.
	for (int wi = 0; wi < 3; wi++) {
		uint64_t base = wi == 0 ? r13_0 : (wi == 1 ? gr15 : gr13);
		if (!Libs::Graphics::HostMemoryRangeIsReadable(base, 64)) {
			LOGF("stack_chk_fail obj[%d]=%llx <unreadable>\n", wi,
			     (unsigned long long)base);
			continue;
		}
		const auto* q = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(base));
		LOGF("stack_chk_fail obj[%d]=%llx: %llx %llx %llx %llx %llx %llx %llx %llx\n",
		     wi, (unsigned long long)base, (unsigned long long)q[0],
		     (unsigned long long)q[1], (unsigned long long)q[2],
		     (unsigned long long)q[3], (unsigned long long)q[4],
		     (unsigned long long)q[5], (unsigned long long)q[6],
		     (unsigned long long)q[7]);
	}
	uint64_t c8 = 0, c0 = 0, c8p = 0;
	if (Libs::Graphics::HostMemoryRangeIsReadable(rbp - 8, 8)) {
		c8 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(rbp - 8));
	}
	if (Libs::Graphics::HostMemoryRangeIsReadable(rbp, 16)) {
		c0 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(rbp));
		c8p = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(rbp + 8));
	}
	LOGF("stack_chk_fail rbp=0x%016llx [rbp-8]=0x%016llx [rbp]=0x%016llx [rbp+8]=0x%016llx\n",
	     (unsigned long long)rbp, (unsigned long long)c8,
	     (unsigned long long)c0, (unsigned long long)c8p);
	const uint64_t chk_caller =
	    reinterpret_cast<uint64_t>(__builtin_return_address(0));
	Libs::IpmiObserve::StackFailure(chk_caller, __builtin_frame_address(0), g_stack_chk_guard);

	// ps5-vsh: audit - the nativeext 0x268f6 canary site has an
	// internally-consistent frame (single balanced prologue/epilogue, no rsp
	// motion, no indexed stack write reachable to 0x8c0). Measure the
	// overwritten slot directly instead of inferring: locate the pushed
	// return address on the guest stack, derive the failing frame's rsp
	// (raddr+8) and the canary slot [frame_rsp+0x8c0], and dump it.
	{
		auto* lrt0 = Common::Singleton<Loader::RuntimeLinker>::Instance();
		auto* lp0  = (lrt0 != nullptr ? lrt0->FindProgramByAddr(chk_caller) : nullptr);
		if (lp0 != nullptr &&
		    Common::PathToString(lp0->file_name.filename()) == "libNativeExtensions.sprx" &&
		    chk_caller - lp0->base_vaddr == 0x268f6u) {
			static std::atomic<uint64_t> s_slot_dumps {0};
			if (s_slot_dumps.fetch_add(1, std::memory_order_relaxed) < 8u) {
				uint64_t raddr = 0;
				for (int i = -32; i < 512; i++) {
					const auto a = rsp + static_cast<int64_t>(i) * 8;
					if (!Libs::Graphics::HostMemoryRangeIsReadable(a, 8)) { break; }
					if (*reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(a)) ==
					    chk_caller) {
						raddr = a;
						break;
					}
				}
				LOGF("ps5-vsh: nativeext slot-audit raddr=0x%016llx rsp=0x%016llx\n",
				     (unsigned long long)raddr, (unsigned long long)rsp);
				if (raddr != 0) {
					const uint64_t frame_rsp = raddr + 8;
					const uint64_t slot      = frame_rsp + 0x8c0u;
					LOGF("ps5-vsh: nativeext slot-audit frame_rsp=0x%016llx slot=0x%016llx "
					     "guard=0x%016llx\n",
					     (unsigned long long)frame_rsp, (unsigned long long)slot,
					     (unsigned long long)g_stack_chk_guard);
					for (int i = -8; i < 8; i += 4) {
						uint64_t q[4] = {};
						for (int k = 0; k < 4; k++) {
							const auto a = slot + static_cast<int64_t>(i + k) * 8;
							q[k] = Libs::Graphics::HostMemoryRangeIsReadable(a, 8)
							           ? *reinterpret_cast<const uint64_t*>(
							                 static_cast<uintptr_t>(a))
							           : 0xDEAD5A5Bull;
						}
						LOGF("ps5-vsh: nativeext slot-audit [slot%+03d..] %016llx %016llx "
						     "%016llx %016llx\n",
						     i, (unsigned long long)q[0], (unsigned long long)q[1],
						     (unsigned long long)q[2], (unsigned long long)q[3]);
					}
				}
			}
		}
	}

	// ps5-vsh: audit B - pin the true frame. The RN-bridge factory 0x273b0
	// calls the failing ctor 0x24fe0 at rva 0x273d0, so the qword 0x273d5 is
	// the ctor's entry rsp (E). E pins the frame exactly:
	//   A = (E - 48) & ~31 ; frame_rsp = A - 0x8e0 ; canary slot = A - 0x20.
	// Also dump the raw window so the slot content can be read off directly
	// (each line carries its own base address, so interleaving is decodable).
	{
		auto* lrt1 = Common::Singleton<Loader::RuntimeLinker>::Instance();
		auto* lp1  = (lrt1 != nullptr ? lrt1->FindProgramByAddr(chk_caller) : nullptr);
		if (lp1 != nullptr &&
		    Common::PathToString(lp1->file_name.filename()) == "libNativeExtensions.sprx" &&
		    chk_caller - lp1->base_vaddr == 0x268f6u) {
			static std::atomic<uint64_t> s_win_dumps {0};
			if (s_win_dumps.fetch_add(1, std::memory_order_relaxed) < 3u) {
				const uint64_t modbase = chk_caller - 0x268f6u;
				char           hits[768];
				int            hp = 0;
				hits[0]           = 0;
				for (int i = -0x20; i < 0x600; i++) {
					const auto a = rsp + static_cast<int64_t>(i) * 8;
					if (!Libs::Graphics::HostMemoryRangeIsReadable(a, 8)) { break; }
					const uint64_t v =
					    *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(a));
					if (v == chk_caller || v == modbase + 0x273d5u ||
					    v == modbase + 0x19604u || v == g_stack_chk_guard) {
						hp += std::snprintf(hits + hp, sizeof(hits) - static_cast<size_t>(hp),
						                    " %+d=0x%llx@0x%llx", i,
						                    (unsigned long long)v,
						                    (unsigned long long)a);
						if (hp > static_cast<int>(sizeof(hits)) - 128) { break; }
					}
				}
				LOGF("ps5-vsh: A2 anchors rsp=0x%016llx modbase=0x%016llx "
				     "guard=0x%016llx rbp=0x%016llx :%s\n",
				     (unsigned long long)rsp, (unsigned long long)modbase,
				     (unsigned long long)g_stack_chk_guard, (unsigned long long)rbp, hits);
				// Raw window into a side file: the emulator log is written
				// by several threads and interleaves mid-line, so a hex
				// dump in the log is unreliable. One record per hit:
				//   u64 magic, rsp, rbp, chk_caller, guard, then the raw
				//   bytes of [rsp-0x200, rsp+0x3000).
				static constexpr int64_t kBack  = 0x200;
				static constexpr int64_t kFwd   = 0x3000;
				static constexpr int64_t kBytes = kBack + kFwd;
				FILE* sf = nullptr;
				fopen_s(&sf, "_TempData\\stackwin.bin",
				        (s_win_dumps.load(std::memory_order_relaxed) == 1) ? "wb" : "ab");
				if (sf != nullptr) {
					const uint64_t hdr[5] = {0x5053357632483254ull, rsp,
					                         (unsigned long long)rbp,
					                         (unsigned long long)chk_caller,
					                         (unsigned long long)g_stack_chk_guard};
					std::fwrite(hdr, 1, sizeof(hdr), sf);
					const uint64_t start = rsp - static_cast<uint64_t>(kBack);
					std::vector<uint8_t> buf(static_cast<size_t>(kBytes), 0);
					for (int64_t k = 0; k < kBytes; k++) {
						const auto a = start + static_cast<uint64_t>(k);
						if (Libs::Graphics::HostMemoryRangeIsReadable(a, 1)) {
							buf[static_cast<size_t>(k)] =
							    *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(a));
						}
					}
					std::fwrite(buf.data(), 1, buf.size(), sf);
					std::fclose(sf);
				}
			}
		}
	}

	dump_stack_chk_fail_context(chk_caller, rsp);

	// ps5-vsh chainSUI168: survive the mono arena-walk check (monosgen
	// 0x15e791 `call __stack_chk_fail`, a layout-consistency trap like the
	// RFH asserts, not a real smash). The HLE sees the post-call retaddr
	// 0x15e796. Returning lands on the ud2 padding there, which the VEH
	// abort-redirect unwinds to the caller.
	// Narrow: exact caller (rva + module) or live-verified shape + cap.
	// Callers with neither still EXIT.
	{
		auto* lrt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		auto* lprog = (lrt != nullptr ? lrt->FindProgramByAddr(chk_caller) : nullptr);
		static std::atomic<uint64_t> s_chk_survived {0};
		// chainSUI301: the nativeext trap fires per-frame in a hot loop
		// (1027 hits in one run); the 1024 mono-era cap exhausts and EXITs.
		// Exact (module, rva) + live shape probes already gate each hit, so
		// exact-shape nativeext hits get their own generous cap + census.
		static std::atomic<uint64_t> s_chk_survived_nativeext {0};
		static std::atomic<uint64_t> s_chk_survived_eboot {0};
		const bool is_nativeext_hit =
		    (lprog != nullptr &&
		     Common::PathToString(lprog->file_name.filename()) ==
		         "libNativeExtensions.sprx" &&
		     chk_caller - lprog->base_vaddr == 0x268f6u);
		const bool is_eboot_hit =
		    (lprog != nullptr &&
		     Common::PathToString(lprog->file_name.filename()) ==
		         "eboot.bin" &&
		     (chk_caller - lprog->base_vaddr == 0x1549b4u ||
		      chk_caller - lprog->base_vaddr == 0x154a9bu));
		const uint64_t cap = (is_nativeext_hit || is_eboot_hit) ? 100000u : 1024u;
		std::atomic<uint64_t>& counter =
		    is_nativeext_hit ? s_chk_survived_nativeext : (is_eboot_hit ? s_chk_survived_eboot : s_chk_survived);
		if (lprog != nullptr &&
		    ([&] {
		        const std::string modname =
		            Common::PathToString(lprog->file_name.filename());
		        const bool is_mono = (modname == "libmonosgen-2.0.sprx");
		        const uint64_t crva = chk_caller - lprog->base_vaddr;
		        if (is_mono && (crva == 0x15e796u || crva == 0x54f72u)) { return true; }
		        // chainSUI292: native-extensions canary trap hit during
		        // onboarding. Exact caller only; the shared shape probes
		        // below re-verify per-hit (call+ud2 bytes live, host-shaped
		        // regs). Fw bytes at 0x268f1/0x268f6 confirm the shape and
		        // the guard is our uninitialized sentinel.
		        const bool is_nativeext = (modname == "libNativeExtensions.sprx" &&
		                                   crva == 0x268f6u);
		        const bool is_eboot = (modname == "eboot.bin" &&
		                               (crva == 0x1549b4u || crva == 0x154a9bu));
		        if (!is_mono && !is_nativeext && !is_eboot) { return false; }
		        // chainSUI241: cascade survive (host-poisoned inputs). Kept as
		        // one signal; the padding shape below is the other.
		        const auto hostshape = [](uint64_t v) {
		            if (v >= 0x140000000ull && v < 0x143000000ull) { return true; }
		            const uint64_t hi = v >> 32;
		            return hi == 0x7FFull || hi >= 0xFF0ull;
		        };
		        const bool poisoned =
		            (hostshape(gr12) || hostshape(gr13) || hostshape(gr15) || hostshape(grbx));
		        // chainSUI243: padding-shape survive. Returning from the HLE
		        // lands on the post-call bytes; when those are ud2 padding
		        // preceded by the E8 call, the VEH redirect (armed below via
		        // pending) unwinds from a LIVE frame (this call really
		        // executed and pushed its return). Verified shape, narrow.
		        bool padshape = false;
		        if (Libs::Graphics::HostMemoryRangeIsReadable(chk_caller, 2) &&
		            Libs::Graphics::HostMemoryRangeIsReadable(chk_caller - 5, 7)) {
		            const auto* pb =
		                reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(chk_caller));
		            const auto* pc =
		                reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(chk_caller - 5));
		            padshape = (pb[0] == 0x0Fu && pb[1] == 0x0Bu && pc[0] == 0xE8u);
		        }
		        if (!poisoned && !padshape) {
		            return false;
		        }
		        LOGF("ps5-vsh: stack_chk_fail cascade (poison=%d pad=%d) caller=%llx\n",
		             poisoned ? 1 : 0, padshape ? 1 : 0,
		             (unsigned long long)crva);
		        return true;
		    }()) &&
		    ([&] {
		        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
		        if (is_nativeext_hit && (n % 500) == 0) {
			        LOGF("ps5-vsh: stack_chk_fail nativeext survived x%llu\n",
			             (unsigned long long)n);
		        }
		        return n < cap;
		    }())) {
			LOGF("ps5-vsh: stack_chk_fail survived (arena-walk)\n");
			// chainSUI235: arm ud2 redirect (returns into padding).
			::Loader::Ps5VshExpectUd2Redirect(chk_caller);
			// ps5-vsh chainSUI176: neuter the check itself. The sole
			// caller of this fail site is the jne at monosgen+0x15e4df
			// (cmp [r13],[rbp-0x30]); NOP it so the function always takes
			// the pass path with epilogue intact (callee-saved regs
			// restored, unlike our VEH unwind which loses them).
			// Runtime guest-code patch; fw files untouched; once-only.
			// chainSUI321 (audit): gate on module too (the IIFE above
			// already proved mono), and only neuter the known site for
			// 0x15e796 — the 0x54f72 site was never recovered, and the old
			// once-only flag burned the single patch on the wrong site.
			static std::atomic<uint64_t> s_chk_patched {0};
			const bool is_mono_mod =
			    (Common::PathToString(lprog->file_name.filename()) ==
			     "libmonosgen-2.0.sprx");
			const uint64_t chk_crva = chk_caller - lprog->base_vaddr;
			if (is_mono_mod && chk_crva == 0x15e796u &&
			    s_chk_patched.fetch_add(1, std::memory_order_relaxed) == 0) {
				uint8_t* const jp = reinterpret_cast<uint8_t*>(
				    static_cast<uintptr_t>(lprog->base_vaddr + 0x15e4dfu));
				if (Libs::Graphics::HostMemoryRangeIsReadable(
				        lprog->base_vaddr + 0x15e4dfu, 6) &&
				    jp[0] == 0x0Fu && jp[1] == 0x85u) {
					#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					DWORD old = 0;
					if (VirtualProtect(jp, 6, PAGE_EXECUTE_READWRITE, &old) != 0) {
						for (int bi = 0; bi < 6; bi++) {
							jp[bi] = 0x90;
						}
						VirtualProtect(jp, 6, old, &old);
						FlushInstructionCache(GetCurrentProcess(), jp, 6);
						LOGF("ps5-vsh: arena-check jne neutered\n");
					} else {
						LOGF("ps5-vsh: arena-check patch VirtualProtect failed\n");
					}
					#else
					(void)jp;
					#endif
				} else {
					LOGF("ps5-vsh: arena-check jne bytes unexpected\n");
				}
			}

			const bool is_eboot_mod =
			    (Common::PathToString(lprog->file_name.filename()) ==
			     "eboot.bin");
			static std::atomic<uint64_t> s_chk_patched_eboot {0};
			if (is_eboot_mod && chk_crva == 0x1549b4u &&
			    s_chk_patched_eboot.fetch_add(1, std::memory_order_relaxed) == 0) {
				uint8_t* const jp = reinterpret_cast<uint8_t*>(
				    static_cast<uintptr_t>(lprog->base_vaddr + 0x15499bu));
				if (Libs::Graphics::HostMemoryRangeIsReadable(
				        lprog->base_vaddr + 0x15499bu, 2) &&
				    jp[0] == 0x75u && jp[1] == 0x12u) {
					#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					DWORD old = 0;
					if (VirtualProtect(jp, 2, PAGE_EXECUTE_READWRITE, &old) != 0) {
						jp[0] = 0x90;
						jp[1] = 0x90;
						VirtualProtect(jp, 2, old, &old);
						FlushInstructionCache(GetCurrentProcess(), jp, 2);
						LOGF("ps5-vsh: eboot WaitBeforeVSync jne neutered\n");
					} else {
						LOGF("ps5-vsh: eboot WaitBeforeVSync patch VirtualProtect failed\n");
					}
					#endif
				} else {
					LOGF("ps5-vsh: eboot WaitBeforeVSync jne bytes unexpected\n");
				}
			}

			if (is_eboot_mod && chk_crva == 0x154a9bu &&
			    s_chk_patched_eboot.fetch_add(1, std::memory_order_relaxed) == 0) {
				uint8_t* const jp = reinterpret_cast<uint8_t*>(
				    static_cast<uintptr_t>(lprog->base_vaddr + 0x154a29u));
				if (Libs::Graphics::HostMemoryRangeIsReadable(
				        lprog->base_vaddr + 0x154a29u, 2) &&
				    jp[0] == 0x75u && jp[1] == 0x6bu) {
					#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					DWORD old = 0;
					if (VirtualProtect(jp, 2, PAGE_EXECUTE_READWRITE, &old) != 0) {
						jp[0] = 0x90;
						jp[1] = 0x90;
						VirtualProtect(jp, 2, old, &old);
						FlushInstructionCache(GetCurrentProcess(), jp, 2);
						LOGF("ps5-vsh: eboot WaitBeforeVSync caller jne neutered\n");
					} else {
						LOGF("ps5-vsh: eboot WaitBeforeVSync caller patch VirtualProtect failed\n");
					}
					#endif
				} else {
					LOGF("ps5-vsh: eboot WaitBeforeVSync caller jne bytes unexpected\n");
				}
			}
			return;
		}
	}

	EXIT("stack fail!!!");
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int sigprocmask(int /*how*/, const void* /*set*/, void* /*oset*/) {
	// PRINT_NAME();

	// LOGF("\t how = %d\n", how);
	// LOGF("\t set = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(set));
	// LOGF("\t oset = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(oset));

	return 0;
}

struct GuestSigaction {
	uint64_t handler = 0;
	int32_t  flags   = 0;
	int32_t  pad     = 0;
	uint64_t mask[2] = {};
};

#define MONO_DIAG(fname, nidstr)                                                              \
	static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t fname(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, \
	                                    uint64_t a4, uint64_t a5) {                         \
		LOGF("ps5-vsh: MonoDiag %s(%llx, %llx, %llx, %llx, %llx, %llx)\n", nidstr,          \
		     (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2,       \
		     (unsigned long long)a3, (unsigned long long)a4, (unsigned long long)a5);      \
		return 0;                                                                          \
	}
MONO_DIAG(MonoDiag_wUgg, "-wUggz2S5yk")

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t MonoDiagY1nE(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5) {
	LOGF("ps5-vsh: MonoDiag Y1nEpkCieOY(%llx, %llx, %llx, %llx, %llx, %llx)\n",
	     (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2,
	     (unsigned long long)a3, (unsigned long long)a4, (unsigned long long)a5);
	auto preview = [](uint64_t v) {
		if (v == 0 || v < 0x10000) {
			return;
		}
		char buf[64] = {};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		__try {
			for (size_t i = 0; i + 1 < sizeof(buf); i++) {
				const char c = reinterpret_cast<const char*>(v)[i];
				if (c == '\0' || c < 0x20 || c > 0x7e) {
					break;
				}
				buf[i] = c;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			buf[0] = '\0';
		}
#else
		for (size_t i = 0; i + 1 < sizeof(buf); i++) {
			const char c = reinterpret_cast<const char*>(v)[i];
			if (c == '\0' || c < 0x20 || c > 0x7e) {
				break;
			}
			buf[i] = c;
		}
#endif
		if (buf[0] != '\0') {
			LOGF("ps5-vsh: MonoDiag Y1nE str: \"%s\"\n", buf);
		}
	};
	preview(a0);
	preview(a5);
	return static_cast<uint64_t>(-1);
}
MONO_DIAG(MonoDiag_YK28, "dYK287SsmDs")
MONO_DIAG(MonoDiag_u09V, "Nd-u09VFSCA")
MONO_DIAG(MonoDiag_Dc3M, "VADc3MNQ3cM")
MONO_DIAG(MonoDiag_cypt, "aPcyptbOiZs")
MONO_DIAG(MonoDiag_qPGL, "enqPGLfmVNU")
MONO_DIAG(MonoDiag_F7C, "+F7C-hdk7+E")
MONO_DIAG(MonoDiag_UimF, "JUimFtKe0Kc")
MONO_DIAG(MonoDiag_TAsr, "VkTAsrZDcJ0")

static void* g_exception_handlers[130] = {};
static uint64_t g_sigaction_handlers[130] = {};
static int32_t g_sigaction_flags[130] = {};

// Shared native-only owners have NO accepted legacy asynchronous/context route.
// This is entry exclusion, not an active-bit promise with an enqueue race.
// Pool kinds are immutable, so an accepted legacy target cannot be recycled
// into a shared target before a queued APC/helper runs.
static bool SharedRuntimeCaller() {
	auto* self = PthreadSelfOrNull();
	return (self && PthreadIsRuntimeOwnedOnTarget(self)) ||
	    bool(Loader::GetApplicationExecution(Loader::RuntimeLinker::GetCurrentProgram()));
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int Sigaction(int signum, const GuestSigaction* act, GuestSigaction* oldact) {
	if (SharedRuntimeCaller()) { return KERNEL_ERROR_EOPNOTSUPP; }
	if (signum <= 0 || signum >= 130) {
		return KERNEL_ERROR_EINVAL;
	}
	LOGF("ps5-vsh: Sigaction(%d, act=%p, old=%p)\n", signum, (void*)act, (void*)oldact);
	if (oldact != nullptr) {
		oldact->handler  = g_sigaction_handlers[signum];
		oldact->flags    = g_sigaction_flags[signum];
		oldact->mask[0]  = 0;
		oldact->mask[1]  = 0;
	}
	if (act != nullptr) {
		g_sigaction_handlers[signum] = act->handler;
		g_exception_handlers[signum] = reinterpret_cast<void*>(act->handler);
		g_sigaction_flags[signum]    = act->flags;
	}
	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t Signal(int signum, uint64_t handler) {
	if (SharedRuntimeCaller()) { *Posix::GetErrorAddr() = Posix::POSIX_EOPNOTSUPP; return UINT64_MAX; }
	if (signum <= 0 || signum >= 130) {
		return static_cast<uint64_t>(-1);
	}
	uint64_t prev                = g_sigaction_handlers[signum];
	g_sigaction_handlers[signum] = handler;
	g_exception_handlers[signum] = reinterpret_cast<void*>(handler);
	g_sigaction_flags[signum]    = 0;
	return prev;
}

using once_routine_t = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void (*)();

static bool Ps5VshAddrIsExecutable(uint64_t addr) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION mi {};
	if (::VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mi, sizeof(mi)) == 0) {
		return false;
	}
	if (mi.State != MEM_COMMIT) {
		return false;
	}
	const DWORD prot = mi.Protect & 0xffu;
	return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
	       prot == PAGE_EXECUTE_WRITECOPY;
#else
	(void)addr;
	return true;
#endif
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int PthreadOnce(int32_t* once, once_routine_t routine) {
	LOGF("ps5-vsh: PthreadOnce(once=%p, routine=%p)\n", (void*)once, (void*)routine);
	if (once == nullptr || routine == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	if (*once == 0) {
		if (!Ps5VshAddrIsExecutable(reinterpret_cast<uint64_t>(routine))) {
			LOGF("ps5-vsh: PthreadOnce skipping non-executable routine\n");
			return OK;
		}
		*once = 1;
		routine();
		*once = 2;
	}
	return OK;
}

struct GuestSpinlock {
	std::atomic<bool> held {false};
};
static std::mutex                                             g_spinlocks_mutex;
static std::map<uintptr_t, std::shared_ptr<GuestSpinlock>>     g_spinlocks;
static uintptr_t                                              g_next_spinlock = 1;

static std::shared_ptr<GuestSpinlock> FindSpinlock(const uintptr_t* handle) {
	if (handle == nullptr) {
		return {};
	}
	std::lock_guard lock(g_spinlocks_mutex);
	auto            found = g_spinlocks.find(*handle);
	return found == g_spinlocks.end() ? nullptr : found->second;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSpinInit(uintptr_t* handle, int shared) {
	if (handle == nullptr || (shared != 0 && shared != 1)) {
		return Posix::POSIX_EINVAL;
	}
	if (shared != 0) {
		return Posix::POSIX_ENOTSUP;
	}
	auto            spin = std::make_shared<GuestSpinlock>();
	std::lock_guard lock(g_spinlocks_mutex);
	const auto      id = g_next_spinlock++;
	g_spinlocks.emplace(id, std::move(spin));
	*handle = id;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSpinDestroy(uintptr_t* handle) {
	if (handle == nullptr) {
		return Posix::POSIX_EINVAL;
	}
	std::lock_guard lock(g_spinlocks_mutex);
	auto            found = g_spinlocks.find(*handle);
	if (found == g_spinlocks.end()) {
		return Posix::POSIX_EINVAL;
	}
	if (found->second->held.load(std::memory_order_acquire) || found->second.use_count() != 1) {
		return Posix::POSIX_EBUSY;
	}
	g_spinlocks.erase(found);
	*handle = 0;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSpinTrylock(uintptr_t* handle) {
	auto spin = FindSpinlock(handle);
	if (!spin) {
		return Posix::POSIX_EINVAL;
	}
	return spin->held.exchange(true, std::memory_order_acquire) ? Posix::POSIX_EBUSY : 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSpinLock(uintptr_t* handle) {
	auto spin = FindSpinlock(handle);
	if (!spin) {
		return Posix::POSIX_EINVAL;
	}
	for (;;) {
		if (!spin->held.exchange(true, std::memory_order_acquire)) {
			return 0;
		}
		while (spin->held.load(std::memory_order_relaxed)) {
			std::this_thread::yield();
		}
	}
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSpinUnlock(uintptr_t* handle) {
	auto spin = FindSpinlock(handle);
	if (!spin) {
		return Posix::POSIX_EINVAL;
	}
	return spin->held.exchange(false, std::memory_order_release) ? 0 : Posix::POSIX_EPERM;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SigEmptySet(void* set) {
	if (set == nullptr) {
		return Posix::POSIX_EINVAL;
	}
	std::memset(set, 0, 16);
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SigFillSet(void* set) {
	if (set == nullptr) {
		return Posix::POSIX_EINVAL;
	}
	std::memset(set, 0xff, 16);
	return 0;
}

static int SignalSetBit(void* set, int signum, bool add) {
	if (set == nullptr) {
		return Posix::POSIX_EINVAL;
	}
	if (signum < 1 || signum > 128) {
		return Posix::POSIX_EINVAL;
	}
	uint32_t bits[4] = {};
	std::memcpy(bits, set, sizeof(bits));
	const uint32_t bit = uint32_t {1} << ((signum - 1) % 32);
	if (add) {
		bits[(signum - 1) / 32] |= bit;
	} else {
		bits[(signum - 1) / 32] &= ~bit;
	}
	std::memcpy(set, bits, sizeof(bits));
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SigAddSet(void* set, int signum) {
	return SignalSetBit(set, signum, true);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SigDelSet(void* set, int signum) {
	return SignalSetBit(set, signum, false);
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SigIsMember(const void* set, int signum) {
	if (set == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}
	if (signum < 1 || signum > 128) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}
	uint32_t bits[4] = {};
	std::memcpy(bits, set, sizeof(bits));
	return static_cast<int>((bits[(signum - 1) / 32] >> ((signum - 1) % 32)) & 1u);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Sony's pthread user-context API has a 64-byte header before mcontext.
// Mono consumes a 1280-byte record; its register offsets are verified in
// mono_arch_get_thread_state (libmonosgen-2.0.sprx).
struct SuspendedUserThread { HANDLE handle; CONTEXT context; uint64_t thread_id; };
static std::mutex g_user_context_mutex;
static std::map<Pthread, SuspendedUserThread> g_user_contexts;

static bool TraceUserContext() {
	const auto& environment = Config::GetGuestEnvironment();
	const auto flag = environment.find("KYTY_GC_THREAD_TRACE");
	return flag != environment.end() && flag->second == "1";
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSuspendUserContext(Pthread thread) {
	if (SharedRuntimeCaller()) {
		static std::atomic_uint32_t refused {0};
		if (refused.fetch_add(1, std::memory_order_relaxed) < 8) {
			LOGF("ps5-vsh: GC suspend refused: shared-runtime caller target=%p\n", static_cast<void*>(thread));
		}
		return Posix::POSIX_EOPNOTSUPP;
	}
	if (thread == nullptr) {
		return Posix::POSIX_EINVAL;
	}
	PthreadLegacyNativeTarget target;
	const int pin_result = PthreadPinLegacyNativeTarget(thread, &target);
	if (pin_result != OK) {
		static std::atomic_uint32_t refused {0};
		if (refused.fetch_add(1, std::memory_order_relaxed) < 8) {
			LOGF("ps5-vsh: GC suspend target pin failed target=%p kernel=%08x\n", static_cast<void*>(thread), unsigned(pin_result));
		}
		return pin_result == KERNEL_ERROR_EOPNOTSUPP ? Posix::POSIX_EOPNOTSUPP : Posix::POSIX_ESRCH;
	}
	const auto id = target.ThreadId();
	if (id == GetCurrentThreadId()) {
		return Posix::POSIX_EDEADLK;
	}
	// Only the owned handle and captured metadata survive this point. A join
	// may reclaim/reuse the record while this operation is retrying suspension.
	target.UnlockRecord();
	std::lock_guard lock(g_user_context_mutex);
	if (g_user_contexts.contains(thread)) {
		return Posix::POSIX_EBUSY;
	}
	const auto handle = static_cast<HANDLE>(target.Handle());
	// The kernel suspends a guest thread outside user-mode locks. Here HLE runs on
	// the guest thread itself, so suspending a thread inside emulator code can
	// freeze it while it holds a host lock (e.g. g_pthread_shadow_mutex) that
	// other pthread HLE calls need, wedging the GC coordinator for good. A thread
	// in the emulator exe is therefore never left suspended: it is resumed and
	// retried for up to ~2s, then the suspend is refused with EBUSY. Threads in
	// other host modules can still hold logging locks. The post-suspend logging
	// counter check below overrides PC classification and the retry budget.
	// The module ranges are captured once so this check never takes a lock itself.
	static const auto host_ranges = [] {
		std::vector<std::pair<uint64_t, uint64_t>> ranges;
		HMODULE  modules[1024];
		DWORD    needed  = 0;
		HANDLE   process = GetCurrentProcess();
		if (EnumProcessModules(process, modules, sizeof(modules), &needed)) {
			for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); i++) {
				MODULEINFO info {};
				if (GetModuleInformation(process, modules[i], &info, sizeof(info))) {
					const auto base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
					ranges.emplace_back(base, base + info.SizeOfImage);
				}
			}
		}
		return ranges;
	}();
	static const auto exe_range = [] {
		std::pair<uint64_t, uint64_t> range {0, 0};
		const HMODULE                 exe = GetModuleHandleW(nullptr);
		if (exe != nullptr) {
			MODULEINFO info {};
			if (GetModuleInformation(GetCurrentProcess(), exe, &info, sizeof(info))) {
				const auto base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
				range           = {base, base + info.SizeOfImage};
			}
		}
		return range;
	}();
	auto in_host_module = [](uint64_t rip) {
		return std::any_of(host_ranges.begin(), host_ranges.end(),
		                   [rip](const auto& r) { return rip >= r.first && rip < r.second; });
	};
	auto in_exe_module = [](uint64_t rip) {
		return exe_range.first != 0 && rip >= exe_range.first && rip < exe_range.second;
	};
	CONTEXT context {};
	auto    exe_deadline = std::chrono::steady_clock::time_point {};
	struct ResumeOnFailure {
		HANDLE handle;
		bool suspended = false;
		~ResumeOnFailure() { if (suspended) { ResumeThread(handle); } }
		DWORD Resume() {
			const auto result = ResumeThread(handle);
			if (result != static_cast<DWORD>(-1)) { suspended = false; }
			return result;
		}
	} resume {handle};
	for (unsigned attempt = 0;;) {
		if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
			return Posix::POSIX_ESRCH;
		}
		resume.suspended = true;
		context              = {};
		context.ContextFlags = CONTEXT_FULL;
		if (!GetThreadContext(handle, &context)) {
			return Posix::POSIX_EINVAL;
		}
		// GetThreadContext synchronizes the asynchronous SuspendThread request.
		// Inspect only atomics while stopped: no logging, locks, or allocation.
		if (target.IsHostLockCritical()) {
			if (resume.Resume() == static_cast<DWORD>(-1)) {
				return Posix::POSIX_ESRCH;
			}
			// Mono treats a nonzero suspend result as fatal. Protected host-lock contention
			// is not an API failure: let the owner progress, then try again.
			// Never retain an unsafe context after a retry-count/time budget.
			exe_deadline = {};
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		if (in_exe_module(context.Rip)) {
			const auto now = std::chrono::steady_clock::now();
			if (exe_deadline == std::chrono::steady_clock::time_point {}) {
				exe_deadline = now + std::chrono::seconds(2);
			}
			if (resume.Resume() == static_cast<DWORD>(-1)) { return Posix::POSIX_ESRCH; }
			if (now >= exe_deadline) {
				static std::atomic_uint32_t refuse_log_count = 0;
				if (refuse_log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
					LOGF("ps5-vsh: GC suspend refused: thread in emulator code pc=0x%016llx after budget\n",
					     static_cast<unsigned long long>(context.Rip));
				}
				return Posix::POSIX_EBUSY;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		if (attempt >= 64 || !in_host_module(context.Rip)) {
			break;
		}
		++attempt;
		if (resume.Resume() == static_cast<DWORD>(-1)) { return Posix::POSIX_ESRCH; }
		std::this_thread::yield();
	}
	g_user_contexts.emplace(thread, SuspendedUserThread {handle, context, id});
	target.ReleaseHandle(); // g_user_contexts owns the exact admitted native object.
	resume.suspended = false;
	if (TraceUserContext()) {
		LOGF("GC suspended: os_tid=%llu, pc=0x%016llx, suspended=%zu\n",
		     static_cast<unsigned long long>(id), static_cast<unsigned long long>(context.Rip), g_user_contexts.size());
		// ps5-vsh: stack-margin probe for the recurring guest-stack-bottom
		// AV (KERNELBASE spill at rsp+0x20). Only near-bottom or out-of-range
		// RSPs are logged, so volume stays tiny.
		const auto stack_lo = target.StackAddress();
		const auto stack_sz = target.StackSize();
		if (stack_lo != 0 && stack_sz != 0) {
			const uint64_t rsp    = static_cast<uint64_t>(context.Rsp);
			const uint64_t margin = (rsp >= stack_lo) ? (rsp - stack_lo) : 0;
			if (rsp < stack_lo || rsp >= stack_lo + stack_sz || margin < 0x10000ull) {
				LOGF("GC stack-margin suspend: os_tid=%llu rsp=0x%016llx stack=[0x%016llx,+0x%llx) margin=0x%llx\n",
				     static_cast<unsigned long long>(id), static_cast<unsigned long long>(rsp),
				     static_cast<unsigned long long>(stack_lo),
				     static_cast<unsigned long long>(stack_sz),
				     static_cast<unsigned long long>(margin));
			}
		}
	}
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadGetUserContext(Pthread thread, void* output) {
	if (SharedRuntimeCaller() || (thread && PthreadIsRuntimeOwned(thread))) { return Posix::POSIX_EOPNOTSUPP; }
	if (thread == nullptr || output == nullptr) {
		return Posix::POSIX_EINVAL;
	}
	std::lock_guard lock(g_user_context_mutex);
	auto it = g_user_contexts.find(thread);
	if (it == g_user_contexts.end()) {
		return Posix::POSIX_EINVAL;
	}
	const auto& c = it->second.context;
	std::array<uint64_t, 160> record {};
	record[8] = 0; // mc_onstack
	record[9] = c.Rdi; record[10] = c.Rsi; record[11] = c.Rdx;
	record[12] = c.Rcx; record[13] = c.R8; record[14] = c.R9;
	record[15] = c.Rax; record[16] = c.Rbx; record[17] = c.Rbp;
	record[18] = c.R10; record[19] = c.R11; record[20] = c.R12;
	record[21] = c.R13; record[22] = c.R14; record[23] = c.R15;
	record[28] = c.Rip; record[29] = c.SegCs; record[30] = c.EFlags;
	record[31] = c.Rsp; record[32] = c.SegSs;
	std::memcpy(output, record.data(), sizeof(record));
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadResumeUserContext(Pthread thread) {
	if (SharedRuntimeCaller() || (thread && PthreadIsRuntimeOwned(thread))) { return Posix::POSIX_EOPNOTSUPP; }
	if (thread == nullptr) {
		return Posix::POSIX_EINVAL;
	}
	std::lock_guard lock(g_user_context_mutex);
	auto it = g_user_contexts.find(thread);
	if (it == g_user_contexts.end()) {
		return Posix::POSIX_EINVAL;
	}
	if (ResumeThread(it->second.handle) == static_cast<DWORD>(-1)) {
		return Posix::POSIX_ESRCH;
	}
	const auto id = it->second.thread_id;
	CloseHandle(it->second.handle);
	g_user_contexts.erase(it);
	if (TraceUserContext()) {
		LOGF("GC resumed: os_tid=%llu, suspended=%zu\n",
		     static_cast<unsigned long long>(id), g_user_contexts.size());
	}
	return 0;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
int TestSuspendLoggingThread(Pthread thread) { return PthreadSuspendUserContext(thread); }
int TestResumeLoggingThread(Pthread thread) { return PthreadResumeUserContext(thread); }
#endif
#else
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadSuspendUserContext(Pthread) { return Posix::POSIX_ENOTSUP; }
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadGetUserContext(Pthread, void*) { return Posix::POSIX_ENOTSUP; }
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI PthreadResumeUserContext(Pthread) { return Posix::POSIX_ENOTSUP; }
#endif

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelMapFlexibleMemoryInternal2(void** address, size_t length, int prot,
                                                          int flags, uint64_t memory_type) {
	(void)memory_type;
	void* ra = __builtin_return_address(0);
	LOGF("ps5-vsh: MapFlexMem ENTER ra=%p out_ptr=%p (in_val=%p), len=%llu, prot=%x, flags=%x\n",
	     ra, (void*)address, (address != nullptr ? *address : nullptr),
	     (unsigned long long)length, prot, flags);
	const int result = Memory::KernelMapNamedFlexibleMemory(address, length, prot & ~4, flags & ~3, "");
	LOGF("ps5-vsh: MapFlexMem EXIT ret=%d (0x%x), out_val=%p\n",
	     result, static_cast<unsigned>(result), (address != nullptr ? *address : nullptr));
	return result;
}

static bool IsAllowedExceptionSignal(int signum) {
	constexpr int POSIX_SIGHUP  = 1;
	constexpr int POSIX_SIGILL  = 4;
	constexpr int POSIX_SIGFPE  = 8;
	constexpr int POSIX_SIGBUS  = 10;
	constexpr int POSIX_SIGSEGV = 11;
	constexpr int POSIX_SIGUSR1 = 30;

	return signum == POSIX_SIGHUP || signum == POSIX_SIGILL || signum == POSIX_SIGFPE ||
	       signum == POSIX_SIGBUS || signum == POSIX_SIGSEGV || signum == POSIX_SIGUSR1;
}

using exception_handler_func_t = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void (*)(int, void*);

static thread_local bool g_dispatching_signal_handler = false;
// Only the new narrow native wait adapters opt in. Async delivery must leave
// pending bits intact until their stack waiter/registry ownership is released.
static thread_local std::atomic_bool g_defer_address_wait_signal {false};
static thread_local bool g_address_wait_interrupted = false;
static_assert(std::atomic_bool::is_always_lock_free);

struct SignalDispatchScope {
	SignalDispatchScope(): previous(g_dispatching_signal_handler) {
		g_dispatching_signal_handler = true;
	}
	~SignalDispatchScope() { g_dispatching_signal_handler = previous; }

	bool previous;
};

bool KernelIsDispatchingSignalOnCurrentThread() {
	return g_dispatching_signal_handler;
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
static void WaitForSignalDispatch(Pthread thread, int signum) {
	constexpr auto DISPATCH_WAIT_STEP = std::chrono::microseconds(1000);
	constexpr auto DISPATCH_WAIT_MAX  = std::chrono::milliseconds(2);

	auto waited = std::chrono::microseconds(0);
	while (PthreadHasPendingSignal(thread, signum) && waited < DISPATCH_WAIT_MAX) {
		Common::Thread::SleepMicro(DISPATCH_WAIT_STEP.count());
		waited += DISPATCH_WAIT_STEP;
	}
}
#endif

struct SignalMcontext {
	uint64_t mc_onstack;
	uint64_t mc_rdi;
	uint64_t mc_rsi;
	uint64_t mc_rdx;
	uint64_t mc_rcx;
	uint64_t mc_r8;
	uint64_t mc_r9;
	uint64_t mc_rax;
	uint64_t mc_rbx;
	uint64_t mc_rbp;
	uint64_t mc_r10;
	uint64_t mc_r11;
	uint64_t mc_r12;
	uint64_t mc_r13;
	uint64_t mc_r14;
	uint64_t mc_r15;
	int      mc_trapno;
	uint16_t mc_fs;
	uint16_t mc_gs;
	uint64_t mc_addr;
	int      mc_flags;
	uint16_t mc_es;
	uint16_t mc_ds;
	uint64_t mc_err;
	uint64_t mc_rip;
	uint64_t mc_cs;
	uint64_t mc_rflags;
	uint64_t mc_reserved_0xb8[8];
	uint64_t mc_rsp;
	uint64_t mc_ss;
	uint64_t mc_len;
	uint64_t mc_fpformat;
	uint64_t mc_ownedfp;
	uint64_t mc_lbrfrom;
	uint64_t mc_lbrto;
	uint64_t mc_aux1;
	uint64_t mc_aux2;
	uint64_t mc_fpstate[104];
	uint64_t mc_fsbase;
	uint64_t mc_gsbase;
	uint64_t mc_spare[6];
};

struct SignalStack {
	void*    ss_sp;
	uint64_t ss_size;
	int      ss_flags;
	int      _align;
};

struct SignalUcontext {
	SignalMcontext uc_mcontext;
};

static_assert(offsetof(SignalUcontext, uc_mcontext) == 0x00);
static_assert(offsetof(SignalMcontext, mc_rip) == 0xa0);
static_assert(offsetof(SignalUcontext, uc_mcontext) + offsetof(SignalMcontext, mc_rsp) == 0xf8);

static SignalUcontext CreateSignalUcontext(
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    const CONTEXT* source_ctx = nullptr
#endif
) {
	SignalUcontext ctx = {};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	CONTEXT host_ctx = {};
	if (source_ctx != nullptr) {
		host_ctx = *source_ctx;
	} else {
		host_ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
		RtlCaptureContext(&host_ctx);
	}

	ctx.uc_mcontext.mc_r8     = host_ctx.R8;
	ctx.uc_mcontext.mc_r9     = host_ctx.R9;
	ctx.uc_mcontext.mc_r10    = host_ctx.R10;
	ctx.uc_mcontext.mc_r11    = host_ctx.R11;
	ctx.uc_mcontext.mc_r12    = host_ctx.R12;
	ctx.uc_mcontext.mc_r13    = host_ctx.R13;
	ctx.uc_mcontext.mc_r14    = host_ctx.R14;
	ctx.uc_mcontext.mc_r15    = host_ctx.R15;
	ctx.uc_mcontext.mc_rdi    = host_ctx.Rdi;
	ctx.uc_mcontext.mc_rsi    = host_ctx.Rsi;
	ctx.uc_mcontext.mc_rbp    = host_ctx.Rbp;
	ctx.uc_mcontext.mc_rbx    = host_ctx.Rbx;
	ctx.uc_mcontext.mc_rdx    = host_ctx.Rdx;
	ctx.uc_mcontext.mc_rax    = host_ctx.Rax;
	ctx.uc_mcontext.mc_rcx    = host_ctx.Rcx;
	ctx.uc_mcontext.mc_rsp    = host_ctx.Rsp;
	ctx.uc_mcontext.mc_rip    = host_ctx.Rip;
	ctx.uc_mcontext.mc_cs     = host_ctx.SegCs;
	ctx.uc_mcontext.mc_fs     = host_ctx.SegFs;
	ctx.uc_mcontext.mc_gs     = host_ctx.SegGs;
	ctx.uc_mcontext.mc_ss     = host_ctx.SegSs;
	ctx.uc_mcontext.mc_ds     = host_ctx.SegDs;
	ctx.uc_mcontext.mc_es     = host_ctx.SegEs;
	ctx.uc_mcontext.mc_rflags = host_ctx.EFlags;
	ctx.uc_mcontext.mc_len    = sizeof(SignalMcontext);
#else
	ctx.uc_mcontext.mc_len = sizeof(SignalMcontext);
	uint64_t stack_marker  = 0;
	ctx.uc_mcontext.mc_rsp = reinterpret_cast<uint64_t>(&stack_marker);
	ctx.uc_mcontext.mc_rbp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
	ctx.uc_mcontext.mc_rip = reinterpret_cast<uint64_t>(__builtin_return_address(0));

#if defined(__x86_64__)
	// Capture the remaining guest-visible registers.
	uint64_t rax = 0;
	uint64_t rbx = 0;
	uint64_t rcx = 0;
	uint64_t rdx = 0;
	uint64_t rsi = 0;
	uint64_t rdi = 0;
	asm volatile("movq %%rax, %0\n\t"
	             "movq %%rbx, %1\n\t"
	             "movq %%rcx, %2\n\t"
	             "movq %%rdx, %3\n\t"
	             "movq %%rsi, %4\n\t"
	             "movq %%rdi, %5\n\t"
	             : "=m"(rax), "=m"(rbx), "=m"(rcx), "=m"(rdx), "=m"(rsi), "=m"(rdi)
	             :
	             : "memory");

	uint64_t r8  = 0;
	uint64_t r9  = 0;
	uint64_t r10 = 0;
	uint64_t r11 = 0;
	uint64_t r12 = 0;
	uint64_t r13 = 0;
	uint64_t r14 = 0;
	uint64_t r15 = 0;
	asm volatile("movq %%r8,  %0\n\t"
	             "movq %%r9,  %1\n\t"
	             "movq %%r10, %2\n\t"
	             "movq %%r11, %3\n\t"
	             "movq %%r12, %4\n\t"
	             "movq %%r13, %5\n\t"
	             "movq %%r14, %6\n\t"
	             "movq %%r15, %7\n\t"
	             : "=m"(r8), "=m"(r9), "=m"(r10), "=m"(r11), "=m"(r12), "=m"(r13), "=m"(r14),
	               "=m"(r15)
	             :
	             : "memory");

	uint64_t rflags = 0;
	uint64_t cs     = 0;
	uint64_t ss     = 0;
	asm volatile("pushfq\n\t"
	             "popq %0\n\t"
	             "movq %%cs, %1\n\t"
	             "movq %%ss, %2\n\t"
	             : "=r"(rflags), "=r"(cs), "=r"(ss)
	             :
	             : "memory");

	ctx.uc_mcontext.mc_rax    = rax;
	ctx.uc_mcontext.mc_rbx    = rbx;
	ctx.uc_mcontext.mc_rcx    = rcx;
	ctx.uc_mcontext.mc_rdx    = rdx;
	ctx.uc_mcontext.mc_rsi    = rsi;
	ctx.uc_mcontext.mc_rdi    = rdi;
	ctx.uc_mcontext.mc_r8     = r8;
	ctx.uc_mcontext.mc_r9     = r9;
	ctx.uc_mcontext.mc_r10    = r10;
	ctx.uc_mcontext.mc_r11    = r11;
	ctx.uc_mcontext.mc_r12    = r12;
	ctx.uc_mcontext.mc_r13    = r13;
	ctx.uc_mcontext.mc_r14    = r14;
	ctx.uc_mcontext.mc_r15    = r15;
	ctx.uc_mcontext.mc_rflags = rflags;
	ctx.uc_mcontext.mc_cs     = cs;
	ctx.uc_mcontext.mc_ss     = ss;
#endif
#endif

	return ctx;
}

static SignalUcontext CreateCurrentGuestCallSignalUcontext(uint64_t rip) {
	auto ctx = CreateSignalUcontext();

	uint64_t rsp = 0;
	uint64_t rbp = 0;
	uint64_t rbx = 0;
	uint64_t r12 = 0;
	uint64_t r13 = 0;
	uint64_t r14 = 0;
	uint64_t r15 = 0;
	asm volatile("movq %%rsp, %0\n\t"
	             "movq %%rbp, %1\n\t"
	             "movq %%rbx, %2\n\t"
	             "movq %%r12, %3\n\t"
	             "movq %%r13, %4\n\t"
	             "movq %%r14, %5\n\t"
	             "movq %%r15, %6\n\t"
	             : "=r"(rsp), "=r"(rbp), "=r"(rbx), "=r"(r12), "=r"(r13), "=r"(r14), "=r"(r15)
	             :
	             : "memory");

	ctx.uc_mcontext.mc_rax = 0;
	ctx.uc_mcontext.mc_rcx = 0;
	ctx.uc_mcontext.mc_rdx = 0;
	ctx.uc_mcontext.mc_rsi = 0;
	ctx.uc_mcontext.mc_rdi = 0;
	ctx.uc_mcontext.mc_r8  = 0;
	ctx.uc_mcontext.mc_r9  = 0;
	ctx.uc_mcontext.mc_r10 = 0;
	ctx.uc_mcontext.mc_r11 = 0;
	ctx.uc_mcontext.mc_rbx = rbx;
	ctx.uc_mcontext.mc_rbp = rbp;
	ctx.uc_mcontext.mc_rsp = rsp;
	ctx.uc_mcontext.mc_r12 = r12;
	ctx.uc_mcontext.mc_r13 = r13;
	ctx.uc_mcontext.mc_r14 = r14;
	ctx.uc_mcontext.mc_r15 = r15;
	ctx.uc_mcontext.mc_rip = rip;

	return ctx;
}

struct FaultInfo {
	int32_t signo, error, code, pid;
	uint32_t uid;
	int32_t status;
	uint64_t address;
	uint8_t reserved[96];
};
static_assert(offsetof(FaultInfo, address) == 0x18);

// POSIX SA_SIGINFO call: (signum, siginfo_t*, ucontext_t*). The ucontext_t wire image has a
// 64-byte header with the mcontext at +0x40 (RDI +0x48, RAX +0x78, RIP +0xe0, RSP +0xf8,
// FXSAVE +0x140), matching voldemort's KernelDispatchGuestFault.
static void CallSiginfoHandler(exception_handler_func_t handler, FaultInfo fault_info, SignalUcontext& ctx) {
	auto& mc = ctx.uc_mcontext;
	alignas(16) std::array<uint8_t, 1280> wire {};
	std::memcpy(wire.data() + 0x40, &mc, 0xb8);
	std::memcpy(wire.data() + 0xf8, reinterpret_cast<const uint8_t*>(&mc) + 0xf8, sizeof(mc) - 0xf8);
	const uint64_t mc_length = sizeof(mc) - 64;
	std::memcpy(wire.data() + 0x108, &mc_length, sizeof(mc_length));

	using SiginfoHandler = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void (*)(int, void*, void*);
	reinterpret_cast<SiginfoHandler>(handler)(fault_info.signo, &fault_info, wire.data());

	std::memcpy(&mc, wire.data() + 0x40, 0xb8);
	std::memcpy(reinterpret_cast<uint8_t*>(&mc) + 0xf8, wire.data() + 0xf8, sizeof(mc) - 0xf8);
}

// SA_SIGINFO applies only to the Sigaction handler in this slot. A handler installed through
// sceKernelInstallExceptionHandler keeps the 2-arg call.
static void CallGuestSignalHandler(exception_handler_func_t handler, int signum, SignalUcontext& ctx) {
	if ((g_sigaction_flags[signum] & 0x40) != 0 &&
	    reinterpret_cast<uint64_t>(handler) == g_sigaction_handlers[signum]) {
		CallSiginfoHandler(handler, FaultInfo {signum, 0, 0, 0, 0, 0, 0, {}}, ctx);
		return;
	}
	handler(signum, &ctx);
}

static bool IsGuestCodeAddress(uint64_t addr) {
	if (addr >= 0x0000000900000000ull && addr < 0x0000000b00000000ull) {
		return true;
	}
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	return rt != nullptr && rt->FindProgramByAddr(addr) != nullptr;
}

static bool IsAddressInRange(uint64_t addr, uint64_t base, uint64_t size) {
	return size != 0 && addr >= base && addr < base + size;
}

static uint64_t GetFallbackStackPointer(Pthread thread) {
	uint64_t stack_addr = 0;
	uint64_t stack_size = 0;
	if (!PthreadGetGuestStack(thread, &stack_addr, &stack_size)) {
		return 0;
	}

	const auto stack_top = (stack_addr + stack_size) & ~0xfull;
	if (stack_top <= stack_addr + 0x100) {
		return stack_top;
	}
	return stack_top - 0x100;
}

static void SanitizeNonGuestSignalUcontext(SignalUcontext* ctx, Pthread thread) {
	if (ctx == nullptr || IsGuestCodeAddress(ctx->uc_mcontext.mc_rip)) {
		return;
	}

	uint64_t   stack_addr = 0;
	uint64_t   stack_size = 0;
	const bool has_stack  = PthreadGetGuestStack(thread, &stack_addr, &stack_size);
	const bool rsp_valid =
	    has_stack && IsAddressInRange(ctx->uc_mcontext.mc_rsp, stack_addr, stack_size);
	const bool rbp_valid =
	    has_stack && IsAddressInRange(ctx->uc_mcontext.mc_rbp, stack_addr, stack_size);
	const auto fallback_sp = GetFallbackStackPointer(thread);

	ctx->uc_mcontext.mc_rax = 0;
	ctx->uc_mcontext.mc_rbx = 0;
	ctx->uc_mcontext.mc_rcx = 0;
	ctx->uc_mcontext.mc_rdx = 0;
	ctx->uc_mcontext.mc_rsi = 0;
	ctx->uc_mcontext.mc_rdi = 0;
	ctx->uc_mcontext.mc_rbp = (rbp_valid ? ctx->uc_mcontext.mc_rbp : fallback_sp);
	ctx->uc_mcontext.mc_rsp = (rsp_valid ? ctx->uc_mcontext.mc_rsp : fallback_sp);
	ctx->uc_mcontext.mc_r8  = 0;
	ctx->uc_mcontext.mc_r9  = 0;
	ctx->uc_mcontext.mc_r10 = 0;
	ctx->uc_mcontext.mc_r11 = 0;
	ctx->uc_mcontext.mc_r12 = 0;
	ctx->uc_mcontext.mc_r13 = 0;
	ctx->uc_mcontext.mc_r14 = 0;
	ctx->uc_mcontext.mc_r15 = 0;
	ctx->uc_mcontext.mc_rip = 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static void ApplySignalUcontext(CONTEXT* dst_ctx, const SignalUcontext& src_ctx) {
	if (dst_ctx == nullptr) {
		return;
	}

	dst_ctx->R8     = src_ctx.uc_mcontext.mc_r8;
	dst_ctx->R9     = src_ctx.uc_mcontext.mc_r9;
	dst_ctx->R10    = src_ctx.uc_mcontext.mc_r10;
	dst_ctx->R11    = src_ctx.uc_mcontext.mc_r11;
	dst_ctx->R12    = src_ctx.uc_mcontext.mc_r12;
	dst_ctx->R13    = src_ctx.uc_mcontext.mc_r13;
	dst_ctx->R14    = src_ctx.uc_mcontext.mc_r14;
	dst_ctx->R15    = src_ctx.uc_mcontext.mc_r15;
	dst_ctx->Rdi    = src_ctx.uc_mcontext.mc_rdi;
	dst_ctx->Rsi    = src_ctx.uc_mcontext.mc_rsi;
	dst_ctx->Rbp    = src_ctx.uc_mcontext.mc_rbp;
	dst_ctx->Rbx    = src_ctx.uc_mcontext.mc_rbx;
	dst_ctx->Rdx    = src_ctx.uc_mcontext.mc_rdx;
	dst_ctx->Rax    = src_ctx.uc_mcontext.mc_rax;
	dst_ctx->Rcx    = src_ctx.uc_mcontext.mc_rcx;
	dst_ctx->Rsp    = src_ctx.uc_mcontext.mc_rsp;
	dst_ctx->Rip    = src_ctx.uc_mcontext.mc_rip;
	dst_ctx->SegCs  = static_cast<DWORD>(src_ctx.uc_mcontext.mc_cs);
	dst_ctx->SegFs  = src_ctx.uc_mcontext.mc_fs;
	dst_ctx->SegGs  = src_ctx.uc_mcontext.mc_gs;
	dst_ctx->SegSs  = static_cast<DWORD>(src_ctx.uc_mcontext.mc_ss);
	dst_ctx->SegDs  = src_ctx.uc_mcontext.mc_ds;
	dst_ctx->SegEs  = src_ctx.uc_mcontext.mc_es;
	dst_ctx->EFlags = static_cast<DWORD>(src_ctx.uc_mcontext.mc_rflags);
}
#endif

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS && defined(__x86_64__)

static SignalUcontext CreateSignalUcontextFromHost(const ucontext_t* host_ctx) {
	SignalUcontext ctx = {};
	if (host_ctx == nullptr) {
		return ctx;
	}

#if defined(__APPLE__)
	const auto& ss = host_ctx->uc_mcontext->__ss;

	ctx.uc_mcontext.mc_rdi    = ss.__rdi;
	ctx.uc_mcontext.mc_rsi    = ss.__rsi;
	ctx.uc_mcontext.mc_rdx    = ss.__rdx;
	ctx.uc_mcontext.mc_rcx    = ss.__rcx;
	ctx.uc_mcontext.mc_r8     = ss.__r8;
	ctx.uc_mcontext.mc_r9     = ss.__r9;
	ctx.uc_mcontext.mc_rax    = ss.__rax;
	ctx.uc_mcontext.mc_rbx    = ss.__rbx;
	ctx.uc_mcontext.mc_rbp    = ss.__rbp;
	ctx.uc_mcontext.mc_r10    = ss.__r10;
	ctx.uc_mcontext.mc_r11    = ss.__r11;
	ctx.uc_mcontext.mc_r12    = ss.__r12;
	ctx.uc_mcontext.mc_r13    = ss.__r13;
	ctx.uc_mcontext.mc_r14    = ss.__r14;
	ctx.uc_mcontext.mc_r15    = ss.__r15;
	ctx.uc_mcontext.mc_rip    = ss.__rip;
	ctx.uc_mcontext.mc_rsp    = ss.__rsp;
	ctx.uc_mcontext.mc_rflags = ss.__rflags;
	ctx.uc_mcontext.mc_cs     = ss.__cs & 0xffffu;
	ctx.uc_mcontext.mc_gs     = static_cast<uint16_t>(ss.__gs & 0xffffu);
	ctx.uc_mcontext.mc_fs     = static_cast<uint16_t>(ss.__fs & 0xffffu);
	ctx.uc_mcontext.mc_len    = sizeof(SignalMcontext);

	return ctx;
#else
	const auto* gregs = host_ctx->uc_mcontext.gregs;

	ctx.uc_mcontext.mc_rdi    = static_cast<uint64_t>(gregs[REG_RDI]);
	ctx.uc_mcontext.mc_rsi    = static_cast<uint64_t>(gregs[REG_RSI]);
	ctx.uc_mcontext.mc_rdx    = static_cast<uint64_t>(gregs[REG_RDX]);
	ctx.uc_mcontext.mc_rcx    = static_cast<uint64_t>(gregs[REG_RCX]);
	ctx.uc_mcontext.mc_r8     = static_cast<uint64_t>(gregs[REG_R8]);
	ctx.uc_mcontext.mc_r9     = static_cast<uint64_t>(gregs[REG_R9]);
	ctx.uc_mcontext.mc_rax    = static_cast<uint64_t>(gregs[REG_RAX]);
	ctx.uc_mcontext.mc_rbx    = static_cast<uint64_t>(gregs[REG_RBX]);
	ctx.uc_mcontext.mc_rbp    = static_cast<uint64_t>(gregs[REG_RBP]);
	ctx.uc_mcontext.mc_r10    = static_cast<uint64_t>(gregs[REG_R10]);
	ctx.uc_mcontext.mc_r11    = static_cast<uint64_t>(gregs[REG_R11]);
	ctx.uc_mcontext.mc_r12    = static_cast<uint64_t>(gregs[REG_R12]);
	ctx.uc_mcontext.mc_r13    = static_cast<uint64_t>(gregs[REG_R13]);
	ctx.uc_mcontext.mc_r14    = static_cast<uint64_t>(gregs[REG_R14]);
	ctx.uc_mcontext.mc_r15    = static_cast<uint64_t>(gregs[REG_R15]);
	ctx.uc_mcontext.mc_rip    = static_cast<uint64_t>(gregs[REG_RIP]);
	ctx.uc_mcontext.mc_rsp    = static_cast<uint64_t>(gregs[REG_RSP]);
	ctx.uc_mcontext.mc_rflags = static_cast<uint64_t>(gregs[REG_EFL]);

	// Linux packs cs/gs/fs into one greg.
	const auto csgsfs      = static_cast<uint64_t>(gregs[REG_CSGSFS]);
	ctx.uc_mcontext.mc_cs  = csgsfs & 0xffffu;
	ctx.uc_mcontext.mc_gs  = static_cast<uint16_t>((csgsfs >> 16u) & 0xffffu);
	ctx.uc_mcontext.mc_fs  = static_cast<uint16_t>((csgsfs >> 32u) & 0xffffu);
	ctx.uc_mcontext.mc_len = sizeof(SignalMcontext);

	return ctx;
#endif
}

static void ApplySignalUcontextToHost(ucontext_t* dst_ctx, const SignalUcontext& src_ctx) {
	if (dst_ctx == nullptr) {
		return;
	}

#if defined(__APPLE__)
	auto& ss = dst_ctx->uc_mcontext->__ss;

	ss.__rdi    = src_ctx.uc_mcontext.mc_rdi;
	ss.__rsi    = src_ctx.uc_mcontext.mc_rsi;
	ss.__rdx    = src_ctx.uc_mcontext.mc_rdx;
	ss.__rcx    = src_ctx.uc_mcontext.mc_rcx;
	ss.__r8     = src_ctx.uc_mcontext.mc_r8;
	ss.__r9     = src_ctx.uc_mcontext.mc_r9;
	ss.__rax    = src_ctx.uc_mcontext.mc_rax;
	ss.__rbx    = src_ctx.uc_mcontext.mc_rbx;
	ss.__rbp    = src_ctx.uc_mcontext.mc_rbp;
	ss.__r10    = src_ctx.uc_mcontext.mc_r10;
	ss.__r11    = src_ctx.uc_mcontext.mc_r11;
	ss.__r12    = src_ctx.uc_mcontext.mc_r12;
	ss.__r13    = src_ctx.uc_mcontext.mc_r13;
	ss.__r14    = src_ctx.uc_mcontext.mc_r14;
	ss.__r15    = src_ctx.uc_mcontext.mc_r15;
	ss.__rip    = src_ctx.uc_mcontext.mc_rip;
	ss.__rsp    = src_ctx.uc_mcontext.mc_rsp;
	ss.__rflags = src_ctx.uc_mcontext.mc_rflags;
	// Segment selectors are left untouched; XNU validates them on sigreturn.
#else
	auto* gregs = dst_ctx->uc_mcontext.gregs;

	gregs[REG_RDI] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rdi);
	gregs[REG_RSI] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rsi);
	gregs[REG_RDX] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rdx);
	gregs[REG_RCX] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rcx);
	gregs[REG_R8]  = static_cast<greg_t>(src_ctx.uc_mcontext.mc_r8);
	gregs[REG_R9]  = static_cast<greg_t>(src_ctx.uc_mcontext.mc_r9);
	gregs[REG_RAX] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rax);
	gregs[REG_RBX] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rbx);
	gregs[REG_RBP] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rbp);
	gregs[REG_R10] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_r10);
	gregs[REG_R11] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_r11);
	gregs[REG_R12] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_r12);
	gregs[REG_R13] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_r13);
	gregs[REG_R14] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_r14);
	gregs[REG_R15] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_r15);
	gregs[REG_RIP] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rip);
	gregs[REG_RSP] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rsp);
	gregs[REG_EFL] = static_cast<greg_t>(src_ctx.uc_mcontext.mc_rflags);

	// The kernel validates packed segment selectors on sigreturn.
#endif
}

static int SignalDispatchHostSignal() {
#if defined(__APPLE__)
	// macOS has no realtime signals; SIGUSR1 is otherwise unused on the host side (the
	// guest's SIGUSR1 is an emulated signal number, not a host registration).
	static const int host_signal = SIGUSR1;
#else
	static const int host_signal = SIGRTMIN + 3;
#endif
	return host_signal;
}

static void HostSignalDispatchHandler(int /*host_signal*/, siginfo_t* /*info*/,
                                      void* native_context) {
	if (PthreadIsRuntimeOwnedOnTarget(PthreadSelfOrNull())) { return; } // Leave incompatible pending work intact.
	if (g_defer_address_wait_signal.load(std::memory_order_relaxed)) {
		return;
	}
	Pthread current = PthreadSelfOrNull();
	if (current == nullptr) {
		return;
	}

	auto* host_ctx = static_cast<ucontext_t*>(native_context);

	for (int signum = 0; signum < static_cast<int>(std::size(g_exception_handlers)); signum++) {
		if (!PthreadTakePendingSignal(current, signum)) {
			continue;
		}

		auto* handler = reinterpret_cast<exception_handler_func_t>(g_exception_handlers[signum]);
		if (handler != nullptr) {
			SignalDispatchScope scope;
			auto                ctx = CreateSignalUcontextFromHost(host_ctx);
			if (IsGuestCodeAddress(ctx.uc_mcontext.mc_rip)) {
				CallGuestSignalHandler(handler, signum, ctx);
				ApplySignalUcontextToHost(host_ctx, ctx);
			} else {
				// Do not expose or restore a host frame.
				SanitizeNonGuestSignalUcontext(&ctx, current);
				CallGuestSignalHandler(handler, signum, ctx);
			}
		}
		return;
	}
}

static bool EnsureHostSignalDispatchInstalled() {
	static const bool installed = [] {
		struct sigaction action {};
		action.sa_sigaction = HostSignalDispatchHandler;
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_SIGINFO | SA_RESTART;
		return ::sigaction(SignalDispatchHostSignal(), &action, nullptr) == 0;
	}();
	return installed;
}

#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
enum KytyQueueUserApcFlags : ULONG_PTR {
	KytyQueueUserApcFlagsNone            = 0,
	KytyQueueUserApcFlagsSpecialUserApc  = 1,
	KytyQueueUserApcFlagsCallbackContext = 0x00010000,
	KytyQueueUserApcFlagsMaxValue        = 2,
};

union KytyUserApcOption {
	ULONG_PTR UserApcFlags;
	HANDLE    MemoryReserveHandle;
};

using KytyPsApcRoutine = void (*)(void* apc_arg1, void* apc_arg2, void* apc_arg3, PCONTEXT context);
using NtQueueApcThreadExFunc = uint64_t(WINAPI*)(HANDLE thread, KytyUserApcOption option,
                                                 KytyPsApcRoutine routine, void* arg1, void* arg2,
                                                 void* arg3);

static NtQueueApcThreadExFunc GetNtQueueApcThreadEx() {
	static auto* nt_queue_apc_thread_ex = reinterpret_cast<NtQueueApcThreadExFunc>(
	    GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueueApcThreadEx"));
	return nt_queue_apc_thread_ex;
}

static void SignalApcHandler(void* arg1, void* arg2, void* /*arg3*/, PCONTEXT context) {
	// Check the ACTUAL executing thread before consuming any pending work, not
	// only the sender's record. Enqueue also pins the original native object.
	if (PthreadIsRuntimeOwnedOnTarget(PthreadSelfOrNull()) ||
	    PthreadIsRuntimeOwnedOnTarget(static_cast<Pthread>(arg1))) { return; }
	if (g_defer_address_wait_signal.load(std::memory_order_relaxed)) {
		return;
	}
	auto*      thread = static_cast<Pthread>(arg1);
	const auto signum = static_cast<int>(reinterpret_cast<intptr_t>(arg2));
	if (!PthreadTakePendingSignal(thread, signum)) {
		return;
	}

	auto* handler = reinterpret_cast<exception_handler_func_t>(g_exception_handlers[signum]);
	if (handler != nullptr) {
		SignalDispatchScope scope;
		auto                ctx           = CreateSignalUcontext(context);
		auto                guest_context = IsGuestCodeAddress(ctx.uc_mcontext.mc_rip);
		if (!guest_context) {
			SanitizeNonGuestSignalUcontext(&ctx, thread);
			// Capture the target's ownership now, not the helper's TLS or the
			// sender's identity. This APC is executing on the live target: its
			// record binding is immutable until host join, even if interrupted in
			// RunThread's TLS installation. No atomic<shared_ptr> lock is taken.
			// Helpers never re-read the record binding after target cleanup/reuse.
			auto fs_context = PthreadGetFsContext(thread);
			auto* owner_program = PthreadGetOwnerProgram(thread);
			std::thread([thread, signum, handler, ctx, fs_context, owner_program]() mutable {
				SignalDispatchScope helper_scope;
				FileSystem::ScopedGuestFsContext fs_scope(std::move(fs_context));
				auto* previous_program = Loader::RuntimeLinker::GetCurrentProgram();
				Loader::RuntimeLinker::SetCurrentProgram(owner_program);
				auto*               previous_self = PthreadSwapSelfForSignal(thread);
				CallGuestSignalHandler(handler, signum, ctx);
				PthreadSwapSelfForSignal(previous_self);
				Loader::RuntimeLinker::SetCurrentProgram(previous_program);
			}).detach();
			return;
		}
		CallGuestSignalHandler(handler, signum, ctx);
		if (guest_context) {
			ApplySignalUcontext(context, ctx);
		}
	}
}

#endif

// ps5-vsh chainSUI229: fault-driven null checks. Mono registers a SIGSEGV
// handler and compiles implicit null checks (no explicit test); a null
// deref must reach that handler (which raises managed NullReference) 
// instead of killing the process. Late VEH fallback: guest READ/WRITE AV
// with a low fault address, faulting code in a loaded program, handler
// installed, not already dispatching. Applies the handler-updated context
// only when it redirected execution; otherwise declines (terminate).
// Capped; guest-context only; fw files untouched.
bool Ps5VshTryGuestSigsegv(const Common::HostException::ExceptionInfo& info) {
	if (PthreadIsRuntimeOwnedOnTarget(PthreadSelfOrNull())) { return false; }
	using AVT = Common::HostException::AccessViolationType;
	if (info.type != Common::HostException::ExceptionType::AccessViolation) {
		return false;
	}
	const bool is_read = (info.access_violation_type == AVT::Read);
	const bool is_write = (info.access_violation_type == AVT::Write);
	if (!is_read && !is_write) {
		return false;
	}
	const uint64_t fault = info.access_violation_vaddr;
	if (fault >= 0x10000ull) {
		return false;
	}
	if (KernelIsDispatchingSignalOnCurrentThread()) {
		return false;
	}
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* prog = (rt != nullptr) ? rt->FindProgramByAddr(info.exception_address)
		                                  : nullptr;
	if (prog == nullptr) {
		return false;
	}
	if (prog->application_owner != nullptr) { return false; } // No shared fault may call session-global handlers.
	constexpr int kSigsegv = 11;
	const uint64_t handler = g_sigaction_handlers[kSigsegv];
	if (handler == 0) {
		return false;
	}
	if (rt->FindProgramByAddr(handler) == nullptr) {
		return false;
	}
	static std::atomic<uint64_t> s_sigsegv {0};
	const uint64_t n = s_sigsegv.fetch_add(1, std::memory_order_relaxed);
	if (n >= 1024) {
		return false;
	}
	LOGF("ps5-vsh: sigsegv dispatch #%llu pc=%llx fault=%llx(%s) prog=%s handler=%llx\n"
	     "     rdi=%llx rsi=%llx rcx=%llx\n",
	     (unsigned long long)n, (unsigned long long)info.exception_address,
	     (unsigned long long)fault, is_write ? "w" : "r",
	     Common::PathToString(prog->file_name.filename()).c_str(),
	     (unsigned long long)handler, (unsigned long long)info.rdi,
	     (unsigned long long)info.rsi, (unsigned long long)info.rcx);
	#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Capture the original context before Mono consumes it and enters its fatal
	// reporter. ReadProcessMemory contains bad stack pointers without recursively
	// faulting inside this handler. Diagnostic only: never alter guest execution.
	if (std::getenv("KYTY_GUEST_SIGSEGV_TRACE") != nullptr && n < 4) {
		const auto copy = [](uint64_t address, void* output, size_t size) {
			const auto saved_error = GetLastError();
			SIZE_T transferred = 0;
			const bool ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
			                                  output, size, &transferred) != 0 && transferred == size;
			SetLastError(saved_error);
			return ok;
		};
		LOGF("ps5-vsh: original-sigsegv pc=%llx rva=%llx rsp=%llx rbp=%llx rax=%llx rbx=%llx rdi=%llx fault=%llx\n",
		     (unsigned long long)info.exception_address,
		     (unsigned long long)(info.exception_address - prog->base_vaddr),
		     (unsigned long long)info.rsp, (unsigned long long)info.rbp,
		     (unsigned long long)info.rax, (unsigned long long)info.rbx,
		     (unsigned long long)info.rdi, (unsigned long long)fault);
		std::array<uint64_t, 16> stack {};
		if (copy(info.rsp, stack.data(), sizeof(stack))) {
			std::array<char, 16 * 17 + 1> text {};
			for (size_t i = 0; i < stack.size(); ++i) {
				std::snprintf(text.data() + i * 17, 18, " %016llx", (unsigned long long)stack[i]);
			}
			LOGF("ps5-vsh: original-sigsegv stack:%s\n", text.data());
		}
		uint64_t fp = info.rbp;
		for (unsigned i = 0; i < 8; ++i) {
			std::array<uint64_t, 2> frame {};
			if (!copy(fp, frame.data(), sizeof(frame))) { break; }
			LOGF("ps5-vsh: original-sigsegv frame[%u] bp=%llx next=%llx ret=%llx\n", i,
			     (unsigned long long)fp, (unsigned long long)frame[0], (unsigned long long)frame[1]);
			if (frame[0] <= fp || (frame[0] & 7u) != 0) { break; }
			fp = frame[0];
		}
		Log::Flush();
	}
	#endif
	// Observe the original PAF queue-pump fault before the guest signal handler
	// can fault again. Fixed-size, read-only snapshots; no list traversal or
	// attempt to repair a null/stale job. Dispatch below remains unchanged.
	#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (Common::PathToString(prog->file_name.filename()) == "SceShellCore.elf" &&
	    info.exception_address - prog->base_vaddr == 0xf4ff8b) {
		const auto copy = [](uint64_t address, void* output, size_t size) {
			const auto saved_error = GetLastError();
			SIZE_T transferred = 0;
			const bool ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
			                                  output, size, &transferred) != 0 && transferred == size;
			SetLastError(saved_error);
			return ok;
		};
		constexpr uint8_t site[] = {0x4c, 0x8b, 0x78, 0x10, 0x41, 0x8b, 0x4f, 0x28};
		std::array<uint8_t, sizeof(site)> code {};
		static std::atomic<uint32_t> captures {0};
		if (copy(info.exception_address - 4, code.data(), code.size()) &&
		    std::memcmp(code.data(), site, sizeof(site)) == 0 &&
		    captures.fetch_add(1, std::memory_order_relaxed) < 4) {
			const auto snapshot = [&copy](const char* label, uint64_t address, size_t words) {
				std::array<uint64_t, 10> values {};
				const auto bytes = words * sizeof(uint64_t);
				if (!copy(address, values.data(), bytes)) {
					LOGF("ps5-vsh: PAF-fault %s @%llx unreadable\n", label,
					     static_cast<unsigned long long>(address));
					return std::array<uint64_t, 10> {};
				}
				// Ten fixed-width qwords fit, including the final terminator.
				std::array<char, 171> text {};
				for (size_t i = 0; i < words; ++i) {
					std::snprintf(text.data() + i * 17, 18, " %016llx",
					              static_cast<unsigned long long>(values[i]));
				}
				LOGF("ps5-vsh: PAF-fault %s @%llx:%s\n", label,
				     static_cast<unsigned long long>(address), text.data());
				return values;
			};
			const auto node = snapshot("node", info.rax, 4);
			snapshot("queue", info.rbx, 10);
			snapshot("job", info.r15, 7);
			if (node[3] != 0) { snapshot("control", node[3], 4); }
		}
	}
	#endif
	// TEMP diag (strip before final commit): strcmp-fault triage (rnpjsc8). At the
	// fault inside libc strcmp the callee-saved r12/r15 still hold the two objects
	// being compared by the type_info method: r15=objA (this), r12=objB (arg).
	{
		auto readable = [](uint64_t p, uint64_t n) {
			return Libs::Graphics::HostMemoryRangeIsReadable(p, n);
		};
		const uint64_t b0 = readable(info.r12, 16) ? *reinterpret_cast<const uint64_t*>(info.r12) : 0;
		const uint64_t b8 = readable(info.r12 + 8, 8) ? *reinterpret_cast<const uint64_t*>(info.r12 + 8) : 0;
		const uint64_t a8 = readable(info.r15 + 8, 8) ? *reinterpret_cast<const uint64_t*>(info.r15 + 8) : 0;
		LOGF("ps5-vsh: strcmp-fault objs r15(objA)=%llx [r15+8]=%llx r12(objB)=%llx [r12]=%llx "
		     "[r12+8]=%llx rax=%llx rbx=%llx r13=%llx r14=%llx\n",
		     (unsigned long long)info.r15, (unsigned long long)a8, (unsigned long long)info.r12,
		     (unsigned long long)b0, (unsigned long long)b8, (unsigned long long)info.rax,
		     (unsigned long long)info.rbx, (unsigned long long)info.r13,
		     (unsigned long long)info.r14);
	}
	#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	auto* nctx = reinterpret_cast<PCONTEXT>(info.native_context);
	if (nctx == nullptr) {
		return false;
	}
	SignalUcontext ctx = CreateSignalUcontext(nctx);
	ctx.uc_mcontext.mc_trapno = 12;
	ctx.uc_mcontext.mc_err = is_write ? 2 : 0;
	ctx.uc_mcontext.mc_addr = fault;
	ctx.uc_mcontext.mc_fpformat = 0x10002; // FXSAVE
	ctx.uc_mcontext.mc_ownedfp = 0x20001;
	std::memcpy(ctx.uc_mcontext.mc_fpstate, &nctx->FltSave, sizeof(nctx->FltSave));
	const uint64_t old_rip = ctx.uc_mcontext.mc_rip;
	{
		SignalDispatchScope scope;
		auto* h = reinterpret_cast<exception_handler_func_t>(handler);
		if ((g_sigaction_flags[kSigsegv] & 0x40) != 0) {
			CallSiginfoHandler(h, FaultInfo {kSigsegv, 0, 1, 0, 0, 0, fault, {}}, ctx);
		} else {
			h(kSigsegv, &ctx);
		}
	}
	if (ctx.uc_mcontext.mc_rip == old_rip) {
		LOGF("ps5-vsh: sigsegv not redirected, declining\n");
		// ps5-vsh chainSUI230: deterministic fallback for the declined
		// Finalizer null site (mscorlib+0x67e6c mov ecx,[rcx+0x18] with
		// rcx==0). The dispatch converts it once, then declines the
		// identical repeat; skipping is deterministic here (ecx already
		// 0; downstream lea just uses [rax]). Narrow: exact program+rva,
		// byte-verified insn, rcx==0 gate, cap. No guest-data writes.
		auto* nrt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		auto* nprog = (nrt != nullptr)
		    ? nrt->FindProgramByAddr(info.exception_address)
		    : nullptr;
		if (nprog != nullptr &&
		    Common::PathToString(nprog->file_name.filename()) ==
		        "mscorlib.dll.sprx" &&
		    info.exception_address - nprog->base_vaddr == 0x67e6cu &&
		    info.rcx == 0 &&
		    Libs::Graphics::HostMemoryRangeIsReadable(info.exception_address, 3)) {
			const auto* nb = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info.exception_address));
			static std::atomic<uint64_t> s_nullskip {0};
			if (nb[0] == 0x8Bu && nb[1] == 0x49u && nb[2] == 0x18u &&
			    s_nullskip.fetch_add(1, std::memory_order_relaxed) < 4096) {
				auto* nctx = reinterpret_cast<PCONTEXT>(info.native_context);
				if (nctx != nullptr) {
					nctx->Rcx = 0;
					nctx->Rip = info.exception_address + 3;
					LOGF("ps5-vsh: nullskip mscorlib+67e6c\n");
					return true;
				}
			}
		}
		return false;
	}
	ApplySignalUcontext(nctx, ctx);
	std::memcpy(&nctx->FltSave, ctx.uc_mcontext.mc_fpstate, sizeof(nctx->FltSave));
	LOGF("ps5-vsh: sigsegv redirected rip=%llx rsp=%llx\n",
	     (unsigned long long)ctx.uc_mcontext.mc_rip,
	     (unsigned long long)ctx.uc_mcontext.mc_rsp);
	return true;
	#else
	return false;
	#endif
}

void KernelDispatchPendingSignalForCurrentThread() {
	if (PthreadIsRuntimeOwnedOnTarget(PthreadSelfOrNull())) { return; }
	Pthread current = PthreadSelfOrNull();
	if (current == nullptr) {
		return;
	}

	for (int signum = 0; signum < static_cast<int>(std::size(g_exception_handlers)); signum++) {
		if (!PthreadTakePendingSignal(current, signum)) {
			continue;
		}

		auto* handler = reinterpret_cast<exception_handler_func_t>(g_exception_handlers[signum]);
		if (handler != nullptr) {
			SignalDispatchScope scope;
			auto                ctx = CreateSignalUcontext();
			SanitizeNonGuestSignalUcontext(&ctx, current);
			CallGuestSignalHandler(handler, signum, ctx);
		}
		return;
	}
}

static bool AddressWaitHasPendingSignal() {
	const auto current = PthreadSelfOrNull();
	for (int signum = 0; signum < 64; ++signum) {
		if (PthreadHasPendingSignal(current, signum)) {
			g_address_wait_interrupted = true;
			return true;
		}
	}
	return false;
}

static void DispatchAddressWaitSignal() {
	if (PthreadIsRuntimeOwnedOnTarget(PthreadSelfOrNull())) { return; }
	const auto current = PthreadSelfOrNull();
	for (int signum = 0; signum < 64; ++signum) {
		if (!PthreadTakePendingSignal(current, signum)) {
			continue;
		}
		auto* handler = reinterpret_cast<exception_handler_func_t>(g_exception_handlers[signum]);
		if (handler == nullptr) {
			handler = reinterpret_cast<exception_handler_func_t>(g_sigaction_handlers[signum]);
		}
		if (handler != nullptr && reinterpret_cast<uintptr_t>(handler) > 1) {
			// No waiter, mutex, shared_ptr or scope guard may survive this call:
			// guest handlers are allowed to leave through siglongjmp.
			auto ctx = CreateSignalUcontext();
			SanitizeNonGuestSignalUcontext(&ctx, current);
			CallGuestSignalHandler(handler, signum, ctx);
		}
		return;
	}
}

static uint64_t AddressWaitBudgetClock() {
	KernelTimespec now {};
	KernelClockGettime(15, &now);
	return static_cast<uint64_t>(now.tv_sec) * 1000000000u + static_cast<uint64_t>(now.tv_nsec);
}

static bool AddressWaitCopyBudget(uint32_t* guest, uint32_t* local, bool write) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SIZE_T copied = 0;
	if (write) {
		// Do not use WriteProcessMemory: it may make a read-only guest page
		// writable. Keep faults in the host syscall, not in a host-frame VEH.
		using WriteMemory = LONG (NTAPI *)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
		const auto module = GetModuleHandleW(L"ntdll.dll");
		const auto function = module != nullptr ? reinterpret_cast<WriteMemory>(
		    GetProcAddress(module, "NtWriteVirtualMemory")) : nullptr;
		return function != nullptr && function(GetCurrentProcess(), guest, local, sizeof(*local), &copied) >= 0 &&
		       copied == sizeof(*local);
	}
	return ReadProcessMemory(GetCurrentProcess(), guest, local, sizeof(*local), &copied) != 0 && copied == sizeof(*local);
#elif defined(__linux__)
	iovec local_iov {local, sizeof(*local)};
	iovec guest_iov {guest, sizeof(*guest)};
	const auto copied = write ? ::process_vm_writev(::getpid(), &local_iov, 1, &guest_iov, 1, 0) :
	                            ::process_vm_readv(::getpid(), &local_iov, 1, &guest_iov, 1, 0);
	return copied == static_cast<ssize_t>(sizeof(*local));
#else
	return false; // No fault-contained native budget access on this backend.
#endif
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelInstallExceptionHandler(int signum, void* handler) {
	if (SharedRuntimeCaller()) { return KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	LOGF("\t signum  = %d\n"
	     "\t handler = 0x%016" PRIx64 "\n",
	     signum, reinterpret_cast<uint64_t>(handler));

	if (!IsAllowedExceptionSignal(signum)) {
		return KERNEL_ERROR_EINVAL;
	}
	if (g_exception_handlers[signum] != nullptr) {
		return KERNEL_ERROR_EAGAIN;
	}

	g_exception_handlers[signum] = handler;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRemoveExceptionHandler(int signum) {
	if (SharedRuntimeCaller()) { return KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	LOGF("\t signum = %d\n", signum);

	if (!IsAllowedExceptionSignal(signum)) {
		return KERNEL_ERROR_EINVAL;
	}

	g_exception_handlers[signum] = nullptr;
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRaiseException(Pthread thread, int signum) {
	if (SharedRuntimeCaller() || (thread && PthreadIsRuntimeOwned(thread))) { return KERNEL_ERROR_EOPNOTSUPP; }
	if (signum <= 0 || signum >= 130 || thread == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	auto* handler = reinterpret_cast<exception_handler_func_t>(g_exception_handlers[signum]);
	if (handler == nullptr && g_sigaction_handlers[signum] != 0) {
		handler = reinterpret_cast<exception_handler_func_t>(g_sigaction_handlers[signum]);
	}
	if (handler != nullptr) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		{
			PthreadLegacyNativeTarget target;
			const auto admitted = PthreadPinLegacyNativeTarget(thread, &target);
			if (admitted != OK) { return admitted; }
			const auto target_thread_id = target.ThreadId();
			if (target_thread_id != static_cast<uint64_t>(GetCurrentThreadId())) {
				auto* nt_queue_apc_thread_ex = GetNtQueueApcThreadEx();
				if (!nt_queue_apc_thread_ex) { return KERNEL_ERROR_EINVAL; }
				// Keep the record pin through reservation/enqueue/rollback/wake. A
				// concurrent join may finish, but cannot recycle the pending mask.
				const bool introduced = target.QueueSignal(signum);
				KytyUserApcOption option {};
				option.UserApcFlags = KytyQueueUserApcFlagsSpecialUserApc;
				const auto status = nt_queue_apc_thread_ex(static_cast<HANDLE>(target.Handle()), option,
				    SignalApcHandler, thread, reinterpret_cast<void*>(static_cast<intptr_t>(signum)), nullptr);
				if (status != 0) {
					if (introduced) { PthreadTakePendingSignal(thread, signum); }
					target.UnlockRecord();
					LOGF("\t NtQueueApcThreadEx failed: target_os_thread=%" PRIu64 ", status=0x%016" PRIx64 "\n",
					     target_thread_id, status);
					return KERNEL_ERROR_EINVAL;
				}
				PthreadWakeForSignal(thread);
				return OK;
			}
		} // No native-handle ownership/record lock may cross direct guest dispatch.
		SignalDispatchScope scope;
		auto ctx = CreateCurrentGuestCallSignalUcontext(reinterpret_cast<uint64_t>(__builtin_return_address(0)));
		CallGuestSignalHandler(handler, signum, ctx);
		return OK;
#elif defined(__x86_64__)
		// Deliver on the target thread.
		if (thread == PthreadSelfOrNull()) {
			SignalDispatchScope scope;
			auto                ctx = CreateCurrentGuestCallSignalUcontext(
			    reinterpret_cast<uint64_t>(__builtin_return_address(0)));
			CallGuestSignalHandler(handler, signum, ctx);
			return OK;
		}

		if (!EnsureHostSignalDispatchInstalled()) {
			return KERNEL_ERROR_EINVAL;
		}

		PthreadQueuePendingSignal(thread, signum);
		if (!PthreadKillHost(thread, SignalDispatchHostSignal())) {
			PthreadTakePendingSignal(thread, signum);
			LOGF("\t pthread_kill failed for target thread\n");
			return KERNEL_ERROR_EINVAL;
		}

		PthreadWakeForSignal(thread);
		WaitForSignalDispatch(thread, signum);
		return OK;
#else
		auto ctx = CreateSignalUcontext();
		CallGuestSignalHandler(handler, signum, ctx);
#endif
	}

	return OK;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelModule KernelLoadStartModule(const char* module_file_name, size_t args,
                                                        const void* argp, uint32_t flags,
                                                        const KernelLoadModuleOpt* opt, int* res);

// Host-side load of a firmware system module by guest path (used where the HLE
// sysmodule layer must bring in a real module, e.g. libSceSysBridge whose
// module_start registers its DllImport tables with PSM). Returns the handle or
// a negative kernel error.
int LoadStartSystemModule(const char* guest_path, size_t args, const void* argp, int* result) {
	int module_result = 0;
	const auto handle = KernelLoadStartModule(guest_path, args, argp, 0, nullptr, &module_result);
	if (result != nullptr) {
		*result = module_result;
	}
	return handle;
}

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelModule KernelLoadStartModule(const char* module_file_name, size_t args,
                                                        const void* argp, uint32_t flags,
                                                        const KernelLoadModuleOpt* opt, int* res) {
	PRINT_NAME();
	if (res != nullptr) { *res = 0; }
	if (module_file_name == nullptr) { return KERNEL_ERROR_EFAULT; }
	const auto* requester = Loader::RuntimeLinker::GetCurrentProgram();
	const auto execution = Loader::GetApplicationExecution(requester);
	Loader::RuntimeGuestExecution::Lease admission;
	if (requester != nullptr && requester->application_owner != nullptr) {
		if (!execution) { return KERNEL_ERROR_ENOSYS; }
		admission = execution->TryAcquire(Loader::GuestWorkKind::Module);
		if (!admission) { return KERNEL_ERROR_ECANCELED; }
	}

	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	LOGF("\tmodule_file_name = %s\n", module_file_name);

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(opt != nullptr);

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto module_path = FileSystem::GetRealFilename(std::string(module_file_name));
	// Mono's native-library probing eventually supplies a bare module name.
	// Resolve that name through the same mounted system-library directories used
	// for import providers, then reuse the canonical loaded image below. Explicit
	// guest paths must not accidentally bind a different file with the same basename.
	if (!std::filesystem::path(module_file_name).has_parent_path() &&
	    !Common::File::IsFileExisting(module_path)) {
		for (const char* directory: {"/common_ex/lib/", "/common/lib/"}) {
			auto candidate = FileSystem::GetRealFilename(std::string(directory) + module_file_name);
			if (Common::File::IsFileExisting(candidate)) {
				module_path = std::move(candidate);
				LOGF("\tmodule search: %s -> %s\n", module_file_name,
				     Common::PathToString(module_path).c_str());
				break;
			}
		}
	}
	if (!Common::File::IsFileExisting(module_path)) {
		LOGF("\tmodule real path missing = %s\n", Common::PathToString(module_path).c_str());
		if (res != nullptr) {
			*res = KERNEL_ERROR_ENOENT;
		}
		return KERNEL_ERROR_ENOENT;
	}

	auto* program = rt->FindProgramByFileName(module_path);
	if (program != nullptr) {
		if (program->application_owner != nullptr) {
			// An image can be visible while another loader is still preparing it.
			// Relocation is serialized by the linker; initializer completion is
			// separately serialized by the shared module lifecycle.
			rt->RelocateProgram(program);
			return rt->LoadStartModuleReference(program, args, argp, res);
		}
		if (!admission) {
			// A mapped boot-session provider is not necessarily initialized yet.
			// Join its real startup before publishing a new handle/reference.
			rt->RelocateProgram(program);
			return rt->LoadStartModuleReference(program, args, argp, res);
		}
		// Explicit pinned session providers retain their own startup policy, but
		// a stopped child must not acquire a fresh reference to them.
		std::lock_guard reference_lock(program->module_mutex);
		if (program->initializer_returned && program->initializer_result < 0) {
			if (res != nullptr) { *res = program->initializer_result; }
			return KERNEL_ERROR_ENOEXEC;
		}
		if (program->load_count == UINT32_MAX) { return KERNEL_ERROR_EBUSY; }
		if (admission) {
			if (!program->started || !program->initializer_returned || program->initializer_result < 0) { return KERNEL_ERROR_EBUSY; }
			if (!admission.Publish([&] { ++program->load_count; })) { return KERNEL_ERROR_ECANCELED; }
		} else { ++program->load_count; }
		if (Common::EqualNoCase(program->file_name.filename().string(), "libSceComposite.sprx")) {
			static std::atomic_uint32_t composite_reuse_count {0};
			if (composite_reuse_count.fetch_add(1, std::memory_order_relaxed) < 32) {
				const auto* requester = Loader::RuntimeLinker::GetCurrentProgram();
				const auto* owner = requester != nullptr ? requester->application_owner : nullptr;
				LOGF("ps5-vsh: Composite reuse requested=%s path=%s handle=%d owner=%d provider-owner=%d base=0x%016" PRIx64 " started=%d load-count=%u\n",
				     Common::PathToString(module_path).c_str(), Common::PathToString(program->file_name).c_str(),
				     program->unique_id, owner != nullptr ? owner->unique_id : -1,
				     program->application_owner != nullptr ? program->application_owner->unique_id : -1,
				     program->base_vaddr, program->started ? 1 : 0, program->load_count);
			}
		}
		if (res != nullptr) {
			*res = OK;
		}
		return static_cast<KernelModule>(program->unique_id);
	}

	program = rt->LoadProgram(module_path);
	if (program == nullptr) { return KERNEL_ERROR_EIO; }

	// ps5-vsh: provider modules must load BEFORE this program is relocated;
	// relocation is what freezes an import to an HLE slot (rela bit), and an HLE
	// slot is never re-attempted. Start() scans once, so runtime-loaded modules
	// (libNativeExtensions/libhermes, which import Jsc_v1) never got a provider:
	// libSceJsc.sprx was never placed, m9Y2LTyzlQc returned 0 for
	// libSceJsc+0x25430, and the JSI bridge at libNativeExtensions+0x57D90 ate
	// that null -> execute-at-0. The program is fully loaded here, so the
	// RelocateAll() inside the scan is safe; RelocateProgram() below is then a
	// no-op for it (rela bits).
	rt->EnsureImportProviders();

	program->dbg_print_reloc = true;

	rt->RelocateProgram(program);
	return rt->LoadStartModuleReference(program, args, argp, res);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelStopUnloadModule(KernelModule handle, size_t args, const void* argp,
                                        uint32_t flags, const KernelUnloadModuleOpt* opt,
                                        int* res) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(opt != nullptr);

	auto* program = rt->FindProgramById(handle);

	if (program == nullptr) {
		LOGF("\tinvalid module handle = %" PRId32 "\n", handle);
		return KERNEL_ERROR_ESRCH;
	}
	if (program->application_owner != nullptr) {
		// Normal host rollback only. Do not abort here: sysmodule registry
		// owners and waiters still have to complete above this internal call.
		const auto* requester = Loader::RuntimeLinker::GetCurrentProgram();
		if (requester != nullptr && requester->application_owner != nullptr &&
		    requester->application_owner != program->application_owner) { return KERNEL_ERROR_EPERM; }
		return rt->StopModuleReference(program, args, argp, res);
	}
	if (const auto* requester = Loader::RuntimeLinker::GetCurrentProgram();
	    requester != nullptr && requester->application_owner != nullptr) {
		// A private application's handle must never finalize/unmap a provider
		// owned by the session (notably pinned native SSL/libc). There is not yet
		// a per-owner borrowed-provider release protocol; reject it explicitly.
		return KERNEL_ERROR_EPERM;
	}

	{
		std::lock_guard reference_lock(program->module_mutex);
		if (program->load_count > 1) {
			--program->load_count;
			return OK;
		}
	}

	const auto atexit_count = g_get_thread_atexit_count_func.load();
	if (atexit_count != nullptr && atexit_count(program->unique_id) > 0) {
		LOGF("KernelStopUnloadModule: cannot unload %s\n",
		     Common::PathToString(program->file_name).c_str());
		if (const auto report = g_thread_atexit_report_func.load()) {
			report(program->unique_id);
		}
		return KERNEL_ERROR_EBUSY;
	}

	bool finalized = false;
	int result = rt->StopModule(program, args, argp, nullptr, &finalized);
	if (!finalized) { return result; }

	LOGF("\tmodule_stop() result = %d\n", result);

	// Finalizer output is informational; the API can unload after a negative
	// ordinary return. A shared-owner boundary abort took the branch above.

	if (res != nullptr) {
		*res = result;
	}

	rt->UnloadProgram(program);

	return OK;
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI tls_get_addr(TlsInfo* info) {
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	return Loader::RuntimeLinker::TlsGetAddr(info->program) + info->offset;
}

static void* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetProcParam() {
	PRINT_NAME();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance(); // NOLINT

	return reinterpret_cast<void*>(rt->GetProcParam());
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelIsTrinityMode() {
	return Loader::SystemContentIsTrinityMode() ? 1 : 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetOperationMode(int* mode, int* submode) {
	*mode    = Loader::SystemContentIsTrinityMode() ? 3 : 2;
	*submode = 0; // None
	return OK;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSync() {
	PRINT_NAME();
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI getpid() {
	PRINT_NAME();

	return Loader::GetCurrentGuestProcessId();
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRtldSetApplicationHeapAPI(void* api[]) {
	PRINT_NAME();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	// This void ABI reports invalid shared input through the owner failure state;
	// the gateway propagates it only AFTER all host frames have returned. Never
	// reread/log the borrowed vector after the checked snapshot/publication.
	rt->SetApplicationHeapApiFromGuest(api);
}

static int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI write(int d, const void* str, uint64_t size) {
	// PRINT_NAME();

	if (d <= 2) {
		const auto result = StandardWrite::Write(d, str, size);
		if (result.count < 0) { *Posix::GetErrorAddr() = result.error; }
		return result.count;
	}
	if (size > static_cast<uint64_t>(INT64_MAX)) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	if (Network::Net::IsSocket(d)) {
		return Network::Net::Send(d, str, static_cast<uint64_t>(size), 0);
	}

	return POSIX_N_CALL(FileSystem::KernelWrite(d, str, size));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI open(const char* path, int flags, int mode) {
	return POSIX_N_CALL(FileSystem::KernelOpen(path, flags, mode));
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI close(int d) {
	if (d < 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	if (d <= 2) {
		return 0;
	}

	if (Network::Net::IsSocket(d)) {
		return POSIX_NET_CALL(Network::Net::SocketClose(d));
	}

	return POSIX_CALL(FileSystem::KernelClose(d));
}

static int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI read(int d, void* buf, uint64_t nbytes) {
	// PRINT_NAME();
	FileSystem::SocketpairProvenance::ReadCallerScope observer_caller(
	    FileSystem::SocketpairProvenance::Enabled() ? KYTY_SPP_CALLER : 0);

	if (buf == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EFAULT;
		return -1;
	}

	if (d < 0 || d == 1 || d == 2) {
		*Posix::GetErrorAddr() = Posix::POSIX_EBADF;
		return -1;
	}

	if (Network::Net::IsSocket(d)) {
		return Network::Net::Recv(d, buf, nbytes, 0);
	}

	if (d > 2) {
		return POSIX_N_CALL(FileSystem::KernelRead(d, buf, nbytes));
	}

	if (nbytes == 0) {
		return 0;
	}

	auto* str = std::fgets(static_cast<char*>(buf), static_cast<int>(nbytes), stdin);

	return (str != nullptr ? static_cast<int64_t>(strlen(str)) : 0);
}

static bool DecodeEhFramePointer(const uint8_t* data, const uint8_t* end, uint8_t encoding,
                                 uint64_t field_addr, uint64_t* value) {
	EXIT_IF(value == nullptr);

	if (encoding == 0xff) {
		return false;
	}

	uint64_t out       = 0;
	auto     format    = encoding & 0x0f;
	auto     app       = encoding & 0x70;
	bool     is_signed = false;

	switch (format) {
		case 0x00:
			if (data + sizeof(uint64_t) > end) {
				return false;
			}
			std::memcpy(&out, data, sizeof(uint64_t));
			break;
		case 0x03:
			if (data + sizeof(uint32_t) > end) {
				return false;
			}
			{
				uint32_t tmp = 0;
				std::memcpy(&tmp, data, sizeof(tmp));
				out = tmp;
			}
			break;
		case 0x04:
			if (data + sizeof(uint64_t) > end) {
				return false;
			}
			std::memcpy(&out, data, sizeof(uint64_t));
			break;
		case 0x0b:
			if (data + sizeof(int32_t) > end) {
				return false;
			}
			{
				int32_t tmp = 0;
				std::memcpy(&tmp, data, sizeof(tmp));
				out       = static_cast<uint64_t>(static_cast<int64_t>(tmp));
				is_signed = true;
			}
			break;
		case 0x0c:
			if (data + sizeof(int64_t) > end) {
				return false;
			}
			std::memcpy(&out, data, sizeof(int64_t));
			is_signed = true;
			break;
		default: return false;
	}

	if (app == 0x10) {
		out = (is_signed ? static_cast<uint64_t>(static_cast<int64_t>(field_addr) +
		                                         static_cast<int64_t>(out))
		                 : field_addr + out);
	} else if (app != 0) {
		return false;
	}

	*value = out;
	return true;
}

// Kyty does not create guest signal-return trampoline frames.
int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelIsSignalReturn(uint64_t /*pc*/) {
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags,
                                               ModuleInfoForUnwind* info) {
	if (flags >= 3) {
		if (info != nullptr) {
			std::memset(info, 0, sizeof(ModuleInfoForUnwind));
		}
		return KERNEL_ERROR_EINVAL;
	}

	if (info == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}

	if (info->st_size < sizeof(ModuleInfoForUnwind)) {
		return KERNEL_ERROR_EINVAL;
	}

	// ps5-vsh: proof-of-life for the guest's own unwinder. The real
	// libSceLibcInternal __cxa_throw -> _Unwind_RaiseException resolves every
	// stack frame through this call (mono's backtrace path uses it too), so the
	// lookups of a run show which modules it walks and from where.
	//
	// Diagnostic only. Baseline (no env): the first 64 calls, index in the
	// message. 2026-10-18: that budget was always eaten by
	// KernelGetModuleInfoFromAddr's mono AOT lookups (which call this with
	// flags = 0) long before the fatal throw, so the failing unwind was never
	// observed. KYTY_UNWIND_TRACE=1 lifts it: every call with flags != 0 (the
	// real unwinder passes flags = 1, libc+0x36727) is printed up to a bound,
	// and the mono/AOT ones only for the first stretch.
	//
	// Fields: caller = return address at the HLE entry, i.e. the guest call
	// site (guest stack == host stack); hdr4 = the first 4 bytes at
	// eh_frame_hdr (a valid GCC header starts 01 1b 03 3b); frame8 = the first 8
	// bytes at eh_frame (a valid 64-bit CIE starts 14 00 00 00 00 00 00 00).
	static std::atomic_uint32_t unwind_log_count = 0; // every call, used as the index
	static std::atomic_uint32_t unwind_log_shown = 0; // calls allowed to print so far
	const uint32_t              unwind_log_n     = unwind_log_count.fetch_add(1);
	const uint32_t              unwind_log_seen  = unwind_log_shown.fetch_add(1);
	static const bool           unwind_log_deep  = std::getenv("KYTY_UNWIND_TRACE") != nullptr;
	const bool                  unwind_log_this =
	    unwind_log_deep ? ((flags != 0) ? (unwind_log_seen < 300000u) : (unwind_log_seen < 1536u))
	                    : (unwind_log_seen < 64u);

	auto* rt      = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* program = rt->FindProgramByAddr(addr);
	if (program == nullptr || program->elf == nullptr) {
		if (unwind_log_this) {
			LOGF("ps5-vsh: UNWINDINFO[%u] caller=0x%016" PRIx64
			     " flags=%d addr=0x%016" PRIx64 " -> no module%s (addr%s0x800000000)\n",
			     unwind_log_n, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))),
			     flags, addr, addr < 0x800000000ull ? " [kyty host return address]" : "",
			     addr < 0x800000000ull ? " < " : " >= ");
		}
		if (addr < 0x800000000ull) {
			// TODO(unwind): guest unwinding can reach a Kyty host return address below the guest VA
			// range. Report a synthetic boundary with no unwind tables so libc stops cleanly
			// instead of raising.
			std::memset(info, 0, sizeof(ModuleInfoForUnwind));
			info->st_size = sizeof(ModuleInfoForUnwind);
			std::snprintf(info->name, sizeof(info->name), "%s", "KytyHostBoundary");
			info->seg0_addr = addr & ~0xfffffull;
			info->seg0_size = 0x100000;
			return OK;
		}

		return KERNEL_ERROR_ESRCH;
	}

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();
	if (ehdr == nullptr || phdr == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	constexpr Loader::Elf64_Word PT_GNU_EH_FRAME = 0x6474e550;

	uint64_t eh_frame_hdr_size = 0;

	std::memset(info, 0, sizeof(ModuleInfoForUnwind));
	info->st_size = sizeof(ModuleInfoForUnwind);

	if (program->dynamic_info != nullptr && program->dynamic_info->so_name != nullptr &&
	    program->dynamic_info->so_name[0] != '\0') {
		std::snprintf(info->name, sizeof(info->name), "%s", program->dynamic_info->so_name);
	} else {
		auto fallback_name = Common::PathToString(program->file_name.filename());
		std::snprintf(info->name, sizeof(info->name), "%s", fallback_name.c_str());
	}

	uint64_t image_start = UINT64_MAX;
	uint64_t image_end   = 0;

	for (int i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz == 0) {
			continue;
		}

		if (phdr[i].p_type == Loader::PT_LOAD || phdr[i].p_type == Loader::PT_OS_RELRO) {
			const auto start = program->base_vaddr + phdr[i].p_vaddr;
			const auto end   = start + phdr[i].p_memsz;
			image_start      = std::min(image_start, start);
			image_end        = std::max(image_end, end);
		}

		if (phdr[i].p_type == PT_GNU_EH_FRAME) {
			info->eh_frame_hdr_addr = program->base_vaddr + phdr[i].p_vaddr;
			eh_frame_hdr_size       = phdr[i].p_memsz;
		}
	}

	const auto* shdr = program->elf->GetShdr();
	if (shdr != nullptr) {
		for (int i = 0; i < ehdr->e_shnum; i++) {
			const char* section_name = program->elf->GetSectionName(i);
			if (section_name != nullptr && std::strcmp(section_name, ".eh_frame") == 0) {
				info->eh_frame_addr = program->base_vaddr + shdr[i].sh_addr;
				info->eh_frame_size = shdr[i].sh_size;
				break;
			}
		}
	}

	if (info->eh_frame_hdr_addr != 0 && eh_frame_hdr_size >= 4) {
		const auto* hdr = reinterpret_cast<const uint8_t*>(info->eh_frame_hdr_addr);
		const auto* end = hdr + eh_frame_hdr_size;
		if (hdr[0] == 1) {
			uint64_t eh_frame_addr = 0;
			if (DecodeEhFramePointer(hdr + 4, end, hdr[1], info->eh_frame_hdr_addr + 4,
			                         &eh_frame_addr)) {
				info->eh_frame_addr = eh_frame_addr;
				if (info->eh_frame_size == 0 && info->eh_frame_hdr_addr > eh_frame_addr) {
					info->eh_frame_size = info->eh_frame_hdr_addr - eh_frame_addr;
				} else if (info->eh_frame_size == 0 &&
				           program->base_size_aligned >
				               info->eh_frame_hdr_addr - program->base_vaddr) {
					info->eh_frame_size = program->base_size_aligned -
					                      (info->eh_frame_hdr_addr - program->base_vaddr);
				}
			}
		}
	}

	if (image_start != UINT64_MAX && image_end > image_start) {
		info->seg0_addr = image_start;
		info->seg0_size = image_end - image_start;
	}

	if (info->eh_frame_addr != 0 && info->eh_frame_size == 0) {
		for (int i = 0; i < ehdr->e_phnum; i++) {
			if (phdr[i].p_memsz == 0 ||
			    (phdr[i].p_type != Loader::PT_LOAD && phdr[i].p_type != Loader::PT_OS_RELRO)) {
				continue;
			}

			const auto start = program->base_vaddr + phdr[i].p_vaddr;
			const auto end   = start + phdr[i].p_memsz;
			if (info->eh_frame_addr >= start && info->eh_frame_addr < end) {
				info->eh_frame_size = end - info->eh_frame_addr;
				break;
			}
		}
	}

	if (unwind_log_this) {
		uint32_t hdr4 = 0;
		uint64_t frame8 = 0;
		if (info->eh_frame_hdr_addr != 0) {
			std::memcpy(&hdr4, reinterpret_cast<const void*>(info->eh_frame_hdr_addr), sizeof(hdr4));
		}
		if (info->eh_frame_addr != 0) {
			std::memcpy(&frame8, reinterpret_cast<const void*>(info->eh_frame_addr), sizeof(frame8));
		}
		LOGF("ps5-vsh: UNWINDINFO[%u] caller=0x%016" PRIx64 " flags=%d addr=0x%016" PRIx64
		     " name=%s hdr=0x%016" PRIx64 " hdr4=0x%08" PRIx32 " frame=0x%016" PRIx64
		     " frame8=0x%016" PRIx64 " size=0x%" PRIx64 " seg0=0x%016" PRIx64 "+0x%" PRIx64 "\n",
		     unwind_log_n, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0))),
		     flags, addr, info->name, static_cast<uint64_t>(info->eh_frame_hdr_addr), hdr4,
		     static_cast<uint64_t>(info->eh_frame_addr), frame8,
		     static_cast<uint64_t>(info->eh_frame_size), static_cast<uint64_t>(info->seg0_addr),
		     static_cast<uint64_t>(info->seg0_size));
	}

	return OK;
}

// ps5-vsh chainSUI152: force entry realignment (misaligned guest stacks
// fault big-frame HLEs; see libC.cpp HLE attrs).
static __attribute__((force_align_arg_pointer)) int KYTY_SYSV_ABI KernelGetModuleInfoFromAddr(uint64_t addr, int n, ModuleInfo* r) {
	// ps5-vsh: ShellCore queries with n=1 too. Only handle is filled,
	// so any version is safe.
	if (r == nullptr) {
		return KERNEL_ERROR_EFAULT;
	}
	// ps5-vsh (chainSUI141): zero first (voldemort parity). monosgen's
	// AOT/unwind path reads eh_frame_hdr_addr at +0x148; stale stack
	// garbage here crashed boot (CoElf:eboot.pc=0x9602f3901).
	std::memset(r, 0, sizeof(*r));
	if (n != 1 && n != 2) {
		LOGF("\tunexpected n = %d\n", n);
		return KERNEL_ERROR_EINVAL;
	}

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto* p = rt->FindProgramByAddr(addr);

	if (p == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}

	r->size = sizeof(*r);
	const auto module_name = Common::PathToString(p->file_name.filename());
	std::memcpy(r->info, module_name.data(), std::min(module_name.size(), sizeof(r->info) - 1));
	r->handle = p->unique_id;
	ModuleInfoForUnwind unwind {};
	unwind.st_size = sizeof(unwind);
	if (KernelGetModuleInfoForUnwind(addr, 0, &unwind) == OK) {
		r->eh_frame_hdr_addr = unwind.eh_frame_hdr_addr;
	}

	return 0;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDebugRaiseExceptionOnReleaseMode(int /*c1*/, int /*c2*/) {
	PRINT_NAME();
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDebugRaiseException(int /*c1*/, int /*c2*/) {
	PRINT_NAME();
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDebugRaiseExceptionWithContext(int /*c1*/, int /*c2*/) {
	PRINT_NAME();
	return 0;
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI exit(int code) {
	PRINT_NAME();
	if (Loader::ExitCurrentVshApplication(code)) { return; }

	// ps5-vsh: always loud (like LibC::exit): silent guest shutdowns are
	// indistinguishable from kills in post-mortem logs.
	LOGF("ps5-vsh: guest exit(%d) [libkernel]\n", code);
	std::fflush(stdout);

	// ps5-vsh: same teardown grace as LibC::exit (see above).
	Common::Thread::SleepMicro(500000);
	::exit(code);
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI MallocReplace* KernelGetSanitizerMallocReplaceExternal() {
	PRINT_NAME();
	// A refused/stopped shared query must never fall through to session globals.
	if (const auto* current = Loader::RuntimeLinker::GetCurrentProgram(); current && current->application_owner) {
		return current->rt ? reinterpret_cast<MallocReplace*>(current->rt->GetApplicationSanitizerReplacement(
		    Loader::ApplicationSanitizerReplacement::Malloc)) : nullptr;
	}

	static MallocReplace ret;

	return &ret;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI NewReplace* KernelGetSanitizerNewReplaceExternal() {
	PRINT_NAME();
	if (const auto* current = Loader::RuntimeLinker::GetCurrentProgram(); current && current->application_owner) {
		return current->rt ? reinterpret_cast<NewReplace*>(current->rt->GetApplicationSanitizerReplacement(
		    Loader::ApplicationSanitizerReplacement::New)) : nullptr;
	}

	static NewReplace ret;

	return &ret;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int elf_phdr_match_addr(ModuleInfo* m, uint64_t dtor_vaddr) {
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(m == nullptr);

	auto* rt     = Common::Singleton<Loader::RuntimeLinker>::Instance();
	auto* p      = rt->FindProgramByAddr(dtor_vaddr);
	int   result = (p != nullptr && p->unique_id == m->handle) ? 1 : 0;

	LOGF("\thandle     = %" PRId32 "\n"
	     "\tdtor_vaddr = %016" PRIx64 "\n"
	     "\tmatch      = %s\n",
	     m->handle, dtor_vaddr, result == 1 ? "true" : "false");

	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelUuidCreate(uint32_t* uuid) {
	PRINT_NAME();

	if (uuid == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	static thread_local std::mt19937 rng(std::random_device {}());
	uuid[0] = rng();
	uuid[1] = rng();
	uuid[2] = rng();
	uuid[3] = rng();

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int KernelGetOpenPsId(void* open_ps_id) {
	PRINT_NAME();

	if (open_ps_id == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	static constexpr uint8_t id[16] = {
	    0x4b, 0x79, 0x74, 0x79, 0x4f, 0x70, 0x65, 0x6e,
	    0x50, 0x73, 0x49, 0x64, 0x00, 0x00, 0x00, 0x01,
	};

	std::memcpy(open_ps_id, id, sizeof(id));

	return OK;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void pthread_cxa_finalize(void* /*p*/) {
	PRINT_NAME();
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int64_t PsmDiagZMn3(uint64_t a0, uint64_t a1, uint64_t a2) {
	LOGF("\t PsmDiagZMn3: %llx %llx %llx\n", a0, a1, a2);
	return 0;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int64_t PsmDiagLEf(uint64_t a0, uint64_t a1, uint64_t a2) {
	LOGF("\t PsmDiagLEf: %llx %llx %llx\n", a0, a1, a2);
	return 0;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int64_t PsmDiagIJIg(uint64_t a0, uint64_t a1, uint64_t a2) {
	LOGF("\t PsmDiagIJIg: %llx %llx %llx\n", a0, a1, a2);
	return 0;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int64_t PsmDiagEZ8h(uint64_t a0, uint64_t a1, uint64_t a2) {
	LOGF("\t PsmDiagEZ8h: %llx %llx %llx\n", a0, a1, a2);
	return 0;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSetThreadAtexitCount(get_thread_atexit_count_func_t func) {
	PRINT_NAME();

	// Native 13.60 setter at 0x36b60 replaces or clears the process callback.
	g_get_thread_atexit_count_func = func;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSetThreadAtexitReport(thread_atexit_report_func_t func) {
	PRINT_NAME();

	// Native 13.60 setter at 0x36b70 has the same replace/clear contract.
	g_thread_atexit_report_func = func;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRtldThreadAtexitIncrement(uint64_t* /*c*/) {
	PRINT_NAME();

	//__sync_fetch_and_add(c, 1);

	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelRtldThreadAtexitDecrement(uint64_t* /*c*/) {
	PRINT_NAME();

	//__sync_fetch_and_sub(c, 1);

	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetAvailableCpumask() {
	PRINT_NAME();

	return 0x1fff;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int KernelGetCurrentCpu() {
	PRINT_NAME();

	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI clock_gettime(int clock_id, LibKernel::KernelTimespec* time) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGettime(clock_id, time));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI clock_getres(int clock_id, LibKernel::KernelTimespec* res) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGetres(clock_id, res));
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSetGPO(uint32_t bits) {
	g_gpo_state_bits.store(bits, std::memory_order_relaxed);

	static std::atomic_uint32_t call_count {0};
	auto                        count = call_count.fetch_add(1, std::memory_order_relaxed);
	if (count < 16 || (count % 1024) == 0) {
		PRINT_NAME();
		LOGF("\tcall = %u, bits = 0x%08" PRIx32 "\n", count + 1, bits);
	}
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetGPI() {
	auto bits = g_gpo_state_bits.load(std::memory_order_relaxed);

	static std::atomic_uint32_t call_count {0};
	auto                        count = call_count.fetch_add(1, std::memory_order_relaxed);
	if (count < 16 || (count % 1024) == 0) {
		PRINT_NAME();
		LOGF("\tcall = %u, return = 0x%016" PRIx64 "\n", count + 1, bits);
	}

	return bits;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelGetBackupRestoreMode() {
	PRINT_NAME();

	// ps5-vsh: QtLhuYZf9jg. No backup/restore in emulation; report normal mode (0).
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIsEcoChargeAvailable() {
	PRINT_NAME();

	// ps5-vsh: a9683+DNj84.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIsDevelopmentMode() {
	PRINT_NAME();

	// ps5-vsh: UtO0OHMCgmI. Retail emulation; never development mode.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIsAssistMode() {
	PRINT_NAME();

	// ps5-vsh: PKg59uLpHNI.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIsCronos() {
	PRINT_NAME();

	// ps5-vsh: nQ5AHlyJghQ.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelError(uint64_t code) {
	PRINT_NAME();

	// ps5-vsh: D4yla3vx4tY. Guest-side error report; log and continue.
	LOGF("\t code = 0x%016" PRIx64 "\n", code);
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelEnablePthreadObjectCheck() {
	PRINT_NAME();

	// ps5-vsh: vhlQXKkQFe0.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIccGetCpuInfoBit() {
	PRINT_NAME();

	// ps5-vsh: Z7NoR9m5SVo.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIccSetCpuInfoBit() {
	PRINT_NAME();

	// ps5-vsh: RP9ImTNDfP8.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SysKernelGetIntdevModeForRcmgr() {
	PRINT_NAME();

	// ps5-vsh: EIZbVQs381s.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SysKernelGetManufacturingMode() {
	PRINT_NAME();

	// ps5-vsh: 6jj29MbyzuI.
	return 0;
}

// Host monotonic seconds for the CPU power/temp sensor ramps below.
// Separate from any guest PRNG stream; sensor reads never disturb it.
static uint64_t CpuSensorTickSec() {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(
	        std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// Guest memory is identity-mapped, but a stale/garbage pointer must never
// take the emulator down. Page-walk probe; false when any page is
// uncommitted or lacks the needed access.
static bool CpuSensorProbe(uint64_t addr, size_t len, bool need_write) {
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
static bool CpuSensorProbe(uint64_t addr, size_t len, bool) {
	return addr != 0 && len != 0;
}
#endif

static bool CpuSensorWrite(uint64_t addr, const void* src, size_t len) {
	if (src == nullptr || len == 0) {
		return false;
	}
	if (!CpuSensorProbe(addr, len, true)) {
		return false;
	}
	std::memcpy(reinterpret_cast<void*>(addr), src, len);
	return true;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelGetCpuUsage(void* a0, uint64_t* out) {
	PRINT_NAME();

	// ps5-vsh: ssmH9nMYO4o. SENSOR: the old idle-zero fill stalls the
	// power-watch CPU rail + die temp at 0.0W/0.3C (boot21) while
	// GFX+CPU/GFX already run 46.8->46.9W off the socPower ramp.
	// Plausible nonzero-varying feeds, host-tick derived: CPU rail
	// 15000..25000mW (15-25W envelope, 1s steps) via a0 (4-byte) and die
	// temp 45000..65000 millidegree-C (45-65C slow ramp, 8s steps) via
	// out (8-byte). Only validated 4/8-byte probed writes, null-safe;
	// LOGF first-use-only (this polls fast).
	const uint64_t tick      = CpuSensorTickSec();
	const uint32_t cpuMw     = 15000u + static_cast<uint32_t>((tick * 211u) % 10001u);
	const uint64_t dieMilliC = 1000ull * (45ull + ((tick / 8ull) % 21ull));
	static bool    logged    = false;
	if (!logged) {
		logged = true;
		LOGF("\t ssmH9nMYO4o -> cpuPower=%umW dieTemp=%llu.%03lluC\n", cpuMw,
		     static_cast<unsigned long long>(dieMilliC / 1000ull),
		     static_cast<unsigned long long>(dieMilliC % 1000ull));
	}
	if (a0 != nullptr) {
		CpuSensorWrite(reinterpret_cast<uint64_t>(a0), &cpuMw, sizeof(cpuMw));
	}
	if (out != nullptr) {
		CpuSensorWrite(reinterpret_cast<uint64_t>(out), &dieMilliC, sizeof(dieMilliC));
	}
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShmOpen(const char* name, int oflag, unsigned mode) {
	PRINT_NAME();
	// SHM_ANON is a pointer sentinel, never a guest string or namespace name.
	if (reinterpret_cast<uintptr_t>(name) == 1) {
		const int result = FileSystem::KernelShmOpenAnonymous(oflag, static_cast<uint16_t>(mode));
		if (result < 0) { *Posix::GetErrorAddr() = KernelToPosix(result); return -1; }
		return result;
	}

	// ps5-vsh: QuJYZ2KVGGQ shm_open (e.g. /SceShellCoreUtil). Back with a
	// temp file so the fd supports read/write/ftruncate/mmap.
	if (name == nullptr || name[0] == '\0') {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}
	std::string base(name);
	while (!base.empty() && base.front() == '/') {
		base.erase(base.begin());
	}
	// POSIX shm names may contain bytes the host filesystem rejects (AgcCompositor
	// generates names such as "/tmp/Compositor...*...`"). Encode everything outside
	// a portable set as %XX so the mapping stays injective; '/' keeps its old '_'.
	std::string encoded;
	encoded.reserve(base.size());
	for (const unsigned char c: base) {
		if (c == '/') {
			encoded.push_back('_');
		} else if (std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.') {
			encoded.push_back(static_cast<char>(c));
		} else {
			static constexpr char kHex[] = "0123456789ABCDEF";
			encoded.push_back('%');
			encoded.push_back(kHex[c >> 4u]);
			encoded.push_back(kHex[c & 0xfu]);
		}
	}
	const std::string guest_path = "/temp0/kyty_shm_" + encoded;
	LOGF("\t name = %s -> %s (oflag=0x%x mode=0%o)\n", name, guest_path.c_str(), oflag, mode);
	// ps5-vsh chainSUI319: oflag/mode are GUEST (FreeBSD) values and MUST be
	// passed through unchanged: this calls our own guest-filesystem open,
	// whose decoder understands PS5 bits (O_CREAT 0x200). Translating to
	// host bits here (O_CREAT 0x100) silently drops create intent and
	// breaks Agc/PSM shm rendezvous (verified: translated build fails
	// where the passthrough build creates fine).
	int fd = open(guest_path.c_str(), oflag, static_cast<int>(mode));
	if (fd < 0 && std::strncmp(name, "/ScePsmSharedDmem_", 18) == 0) {
		// ps5-vsh chainSUI321: PSM's SharedDmem server side never creates
		// indexed segments (no IPMI PsmSharedDmemRequest port yet); the
		// client shm_open(O_RDWR|O_EXCL, no CREAT) then dies ENOENT and PSM
		// graphics never starts (0x81668204). Mirror voldemort: create the
		// 1MB zeroed backing on demand so client open+map+publish proceeds.
		// Narrow: exact name prefix only; success path untouched.
		fd = open(guest_path.c_str(), oflag | 0x200, static_cast<int>(mode));
		if (fd >= 0) {
			if (LibKernel::FileSystem::KernelFtruncate(fd, 0x100000) != 0) {
				LOGF("\t PsmSharedDmem backing ftruncate failed\n");
			} else {
				LOGF("\t PsmSharedDmem backing created (1MB): %s\n", name);
			}
		}
	}
	return fd;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelOpenEventFlag(LibKernel::EventFlag::KernelEventFlag* out,
                                         const char* name) {
	return LibKernel::EventFlag::KernelOpenEventFlag(out, name);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelGetMainSocId() {
	PRINT_NAME();

	// ps5-vsh: 0vTn5IDMU9A.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelGetDataTransferMode() {
	PRINT_NAME();

	// ps5-vsh: i-H8tE6wTqI. Matches the RegMgr 0x2110000 default.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIsPs4Process() {
	PRINT_NAME();

	// ps5-vsh: Yrwoq3bti3c. Native PS5 process.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ScePthreadSetBesteffort() {
	PRINT_NAME();

	// ps5-vsh: i3tB6CuvHb4. QoS hint only.
	return 0;
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Sysconf(int name) {
	PRINT_NAME();

	// ps5-vsh: mkawd0NA9ts sysconf. Only _SC_PAGESIZE (47) observed;
	// PS5 pages are 16 KiB like the emulator's guest pages. Unknown
	// names read 0 (matching stub behavior that already boots).
	constexpr int kPagesize      = 47;
	constexpr int kNprocConf     = 57;
	constexpr int kNprocOnln     = 58;
	constexpr int kPhysPages     = 121;
	constexpr int kOpenMax       = 5;
	constexpr int kChildMax      = 2;
	constexpr int kClockTick     = 3;
	constexpr int64_t kPageBytes = 0x4000;
	if (name == kPagesize) {
		LOGF("ps5-vsh: sysconf(PAGESIZE) -> 0x4000\n");
		return kPageBytes;
	}
	if (name == kNprocConf || name == kNprocOnln) {
		return 8;
	}
	if (name == kPhysPages) {
		return static_cast<int64_t>(8ull * 1024 * 1024 * 1024 / kPageBytes);
	}
	if (name == kOpenMax) {
		return 1024;
	}
	if (name == kChildMax) {
		return 40;
	}
	if (name == kClockTick) {
		return 100;
	}
	LOGF("\t unknown sysconf(%d) -> 0\n", name);
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelGetProcessName(int id, char* name) {
	PRINT_NAME();

	// ps5-vsh: fUJRLEbJOuQ. Report the shell module name (bounded).
	(void)id;
	static const char kName[] = "SceShellCore";
	if (name == nullptr) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::strncpy(name, kName, 31);
	name[31] = '\0';
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIccNotifyBootStatus(uint64_t status) {
	PRINT_NAME();

	// ps5-vsh: wNeGsNes5ck.
	LOGF("\t status = 0x%016" PRIx64 "\n", status);
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelIccGetUSBPowerState() {
	PRINT_NAME();

	// ps5-vsh: NtOA+0RApBg.
	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceKernelAddDynlibEvent() {
	PRINT_NAME();

	// ps5-vsh: dGFwUPT+BK8. Dylib event subscription; never fired here.
	return 0;
}

} // namespace LibKernel

namespace LibKernelWriteThrottling {

LIB_VERSION("libkernel_write_throttling", 1, "libkernel", 1, 1);

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI WriteThrottlingStub() {
	return 0;
}

LIB_DEFINE(InitLibKernelWriteThrottling) {
	LIB_FUNC("YFC3dBBipj8", WriteThrottlingStub);
}

} // namespace LibKernelWriteThrottling

namespace Posix {

LIB_VERSION("Posix", 1, "libkernel", 1, 1);

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI getpagesize() {
	PRINT_NAME();
	LOGF("ps5-vsh: getpagesize() -> 0x4000\n");

	return 0x4000;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI clock_gettime(int clock_id, LibKernel::KernelTimespec* time) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGettime(clock_id, time));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI clock_getres(int clock_id, LibKernel::KernelTimespec* res) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::KernelClockGetres(clock_id, res));
}

struct KernelTimezone {
	int32_t tz_minuteswest;
	int32_t tz_dsttime;
};

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI gettimeofday(LibKernel::KernelTimeval* time, KernelTimezone* timezone) {
	PRINT_NAME();

	if (time != nullptr) {
		int result = LibKernel::KernelGettimeofday(time);
		if (result != OK) {
			*GetErrorAddr() = LibKernel::KernelToPosix(result);
			return -1;
		}
	}

	if (timezone != nullptr) {
		timezone->tz_minuteswest = 0;
		timezone->tz_dsttime     = 0;
	}

	return 0;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI nanosleep(const LibKernel::KernelTimespec* rqtp,
                            LibKernel::KernelTimespec*       rmtp) {
	PRINT_NAME();

	// ps5-vsh chainSUI246: parked-main-thread triage. Log requested
	// durations (cap 8): infinite/finite sleep decides whether the main
	// thread is parked by design or stuck.
	static std::atomic_uint64_t s_nslog {0};
	if (rqtp != nullptr && s_nslog.fetch_add(1, std::memory_order_relaxed) < 8) {
		LOGF("ps5-vsh: nanosleep req=%llds %lldns\n", (unsigned long long)rqtp->tv_sec,
		     (unsigned long long)rqtp->tv_nsec);
	}
	return POSIX_CALL(LibKernel::KernelNanosleep(rqtp, rmtp));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI stat(const char* path, LibKernel::FileSystem::FileStat* sb) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelStat(path, sb));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI mkdir(const char* path, uint16_t mode) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelMkdir(path, mode));
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI lseek(int d, int64_t offset, int whence) {
	PRINT_NAME();

	return POSIX_N_CALL(LibKernel::FileSystem::KernelLseek(d, offset, whence));
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pread(int d, void* buf, size_t nbytes, int64_t offset) {
	PRINT_NAME();

	return POSIX_N_CALL(LibKernel::FileSystem::KernelPread(d, buf, nbytes, offset));
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI pwrite(int d, const void* buf, size_t nbytes, int64_t offset) {
	PRINT_NAME();

	return POSIX_N_CALL(LibKernel::FileSystem::KernelPwrite(d, buf, nbytes, offset));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI flock(int d, int operation) {
	PRINT_NAME();

	constexpr int lock_sh = 0x01;
	constexpr int lock_ex = 0x02;
	constexpr int lock_nb = 0x04;
	constexpr int lock_un = 0x08;

	auto lock_mode = operation & ~(lock_nb);
	if (lock_mode != lock_sh && lock_mode != lock_ex && lock_mode != lock_un) {
		*GetErrorAddr() = POSIX_EINVAL;
		return -1;
	}

	LibKernel::FileSystem::FileStat sb {};
	auto                            result = LibKernel::FileSystem::KernelFstat(d, &sb);
	if (result != OK) {
		*GetErrorAddr() = LibKernel::KernelToPosix(result);
		return -1;
	}

	return 0;
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI fstat(int d, LibKernel::FileSystem::FileStat* sb) {
	PRINT_NAME();

	return POSIX_N_CALL(LibKernel::FileSystem::KernelFstat(d, sb));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ftruncate(int d, int64_t length) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelFtruncate(d, length));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI futimes(int d, const LibKernel::KernelTimeval* times) {
	PRINT_NAME();
	return POSIX_CALL(LibKernel::FileSystem::KernelFutimes(d, times));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI fcntl(int d, int command, int64_t arg) {
	PRINT_NAME();
	// The shared file/socket/SHM implementation already returns -1 and sets errno.
	return LibKernel::FileSystem::KernelFcntl(d, command, arg);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI fsync(int d) {
	PRINT_NAME();

	return POSIX_CALL(LibKernel::FileSystem::KernelFsync(d));
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI socket(int family, int type, int protocol) {
	PRINT_NAME();
	return Network::Net::Socket(family, type, protocol);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI bind(int s, const void* addr, uint32_t addrlen) {
	PRINT_NAME();
	return Network::Net::Bind(s, addr, addrlen);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI connect(int s, const void* addr, uint32_t addrlen) {
	PRINT_NAME();
	return Network::Net::Connect(s, addr, addrlen);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI shutdown(int s, int how) {
	PRINT_NAME();
	return Network::Net::Shutdown(s, how);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI listen(int s, int backlog) {
	PRINT_NAME();
	return Network::Net::Listen(s, backlog);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI accept(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();
	return Network::Net::Accept(s, addr, addrlen);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI getsockname(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();
	return Network::Net::Getsockname(s, addr, addrlen);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI getpeername(int s, void* addr, uint32_t* addrlen) {
	PRINT_NAME();
	return Network::Net::Getpeername(s, addr, addrlen);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI getsockopt(int s, int level, int optname, void* optval, uint32_t* optlen) {
	PRINT_NAME();
	return Network::Net::Getsockopt(s, level, optname, optval, optlen);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI setsockopt(int s, int level, int optname, const void* optval, uint32_t optlen) {
	PRINT_NAME();
	return Network::Net::Setsockopt(s, level, optname, optval, optlen);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI select(int nfds, void* readfds, void* writefds, void* exceptfds,
                         const void* timeout) {
	PRINT_NAME();
	return Network::Net::Select(nfds, readfds, writefds, exceptfds, timeout);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI send(int s, const void* buf, uint64_t len, int flags) {
	PRINT_NAME();
	return Network::Net::Send(s, buf, len, flags);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sendto(int s, const void* buf, uint64_t len, int flags, const void* addr,
                             uint32_t addrlen) {
	PRINT_NAME();
	return Network::Net::Sendto(s, buf, len, flags, addr, addrlen);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI recv(int s, void* buf, uint64_t len, int flags) {
	PRINT_NAME();
	return Network::Net::Recv(s, buf, len, flags);
}

int64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI recvfrom(int s, void* buf, uint64_t len, int flags, void* addr,
                               uint32_t* addrlen) {
	PRINT_NAME();
	return Network::Net::Recvfrom(s, buf, len, flags, addr, addrlen);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI inet_pton(int af, const char* src, void* dst) {
	PRINT_NAME();

	const int result = Network::Net::NetInetPton(af, src, dst);
	if (result < 0) {
		*GetErrorAddr() = Network::NetToPosix(result);
		return -1;
	}
	return result;
}

const char* __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI inet_ntop(int af, const void* src, char* dst, uint32_t size) {
	PRINT_NAME();

	const char* result = Network::Net::NetInetNtop(af, src, dst, size);
	if (result == nullptr) {
		*GetErrorAddr() = (af == 2 ? POSIX_ENOSPC : POSIX_EAFNOSUPPORT);
	}

	return result;
}

uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI cfwBSQyr5Ys(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                                   uint64_t a6) {
	PRINT_NAME();

	LOGF("\t return = 0x%016" PRIx64 "\n"
	     "\t a1     = 0x%016" PRIx64 "\n"
	     "\t a2     = 0x%016" PRIx64 "\n"
	     "\t a3     = 0x%016" PRIx64 "\n"
	     "\t a4     = 0x%016" PRIx64 "\n"
	     "\t a5     = 0x%016" PRIx64 "\n"
	     "\t a6     = 0x%016" PRIx64 "\n"
	     "\t result = 0\n",
	     reinterpret_cast<uint64_t>(__builtin_return_address(0)), a1, a2, a3, a4, a5, a6);

	return 0;
}

static void LogExperimentalSyncOnAddress(std::atomic_bool& logged, const char* function_name) {
	if (!logged.exchange(true, std::memory_order_relaxed)) {
		::printf("WARNING: %s is experimental\n", function_name);
		LOGF("WARNING: %s is experimental\n", function_name);
	}
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSyncOnAddressWait(volatile uint32_t* address, uint32_t expected,
                                          const uint32_t* timeout_micros) {
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("futex");
	return LibKernel::SyncOnAddress::Wait32(address, expected, timeout_micros,
	                                        LibKernel::KernelDispatchPendingSignalForCurrentThread);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSyncOnAddressWait32(volatile uint32_t* address, uint32_t expected,
                                            const uint32_t* timeout_micros) {
	static std::atomic_bool logged {false};
	LogExperimentalSyncOnAddress(logged, "sceKernelSyncOnAddressWait32");
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("futex");
	return LibKernel::SyncOnAddress::Wait32(address, expected, timeout_micros,
	                                        LibKernel::KernelDispatchPendingSignalForCurrentThread);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSyncOnAddressWait64(volatile uint64_t* address, uint64_t expected,
                                            const uint32_t* timeout_micros) {
	static std::atomic_bool logged {false};
	LogExperimentalSyncOnAddress(logged, "sceKernelSyncOnAddressWait64");
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD("futex");
	return LibKernel::SyncOnAddress::Wait64(address, expected, timeout_micros,
	                                        LibKernel::KernelDispatchPendingSignalForCurrentThread);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSyncOnAddressWake(volatile void* address, int32_t count) {
	LibKernel::g_defer_address_wait_signal.store(true, std::memory_order_relaxed);
	const int result = LibKernel::SyncOnAddress::WakeNative(address, count);
	LibKernel::g_defer_address_wait_signal.store(false, std::memory_order_relaxed);
	LibKernel::DispatchAddressWaitSignal();
	return result;
}

template <typename T>
static int NativeNarrowAddressWait(volatile T* address, T expected, uint32_t* timeout_micros, const char* name) {
	// Mirror the verified helper's prechecks before validating the load range.
	uint32_t budget = 0;
	if (timeout_micros != nullptr &&
	    !LibKernel::AddressWaitCopyBudget(timeout_micros, &budget, false)) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if ((reinterpret_cast<uintptr_t>(address) >> 60u) != 0) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (timeout_micros != nullptr && budget == 0) {
		return LibKernel::KERNEL_ERROR_ETIMEDOUT;
	}
	if (!Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uintptr_t>(address), sizeof(T))) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	LibKernel::g_defer_address_wait_signal.store(true, std::memory_order_relaxed);
	LibKernel::g_address_wait_interrupted = false;
	auto* local_timeout = timeout_micros != nullptr ? &budget : nullptr;
	int result;
	if constexpr (sizeof(T) == 1) {
		result = LibKernel::SyncOnAddress::Wait8(address, expected, local_timeout, name,
		    LibKernel::AddressWaitHasPendingSignal, LibKernel::AddressWaitBudgetClock);
	} else {
		result = LibKernel::SyncOnAddress::Wait16(address, expected, local_timeout, name,
		    LibKernel::AddressWaitHasPendingSignal, LibKernel::AddressWaitBudgetClock);
	}
	if (timeout_micros != nullptr && !LibKernel::g_address_wait_interrupted &&
	    (result == OK || result == LibKernel::KERNEL_ERROR_ETIMEDOUT) &&
	    !LibKernel::AddressWaitCopyBudget(timeout_micros, &budget, true)) {
		result = LibKernel::KERNEL_ERROR_EFAULT;
	}
	LibKernel::g_defer_address_wait_signal.store(false, std::memory_order_relaxed);
	LibKernel::DispatchAddressWaitSignal();
	return result;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSyncOnAddressWait8(
    volatile uint8_t* address, uint8_t expected, uint32_t* timeout_micros, const char* name) {
	return NativeNarrowAddressWait(address, expected, timeout_micros, name);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelSyncOnAddressWait16(
    volatile uint16_t* address, uint16_t expected, uint32_t* timeout_micros, const char* name) {
	return NativeNarrowAddressWait(address, expected, timeout_micros, name);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI UmtxOp(volatile void* address, int operation, uint64_t value,
                         void* uaddr, const void* timeout) {
	NoteHleCall(g_library, g_module, __func__);
	HLE_SLOW_GUARD(nullptr);
	constexpr int UMTX_OP_WAIT = 2;
	constexpr int UMTX_OP_WAKE = 3;

	EXIT_NOT_IMPLEMENTED(uaddr != nullptr);

	switch (operation) {
		case UMTX_OP_WAIT:
			if (timeout != nullptr) {
				LibKernel::KernelTimespec duration {};
				std::memcpy(&duration, timeout, sizeof(duration));
				if (duration.tv_sec < 0 || duration.tv_nsec < 0 || duration.tv_nsec >= 1000000000) {
					*GetErrorAddr() = POSIX_EINVAL;
					return -1;
				}
				const auto max_ns = std::chrono::nanoseconds::max().count();
				const auto timeout_ns = duration.tv_sec > (max_ns - duration.tv_nsec) / 1000000000
				                            ? max_ns
				                            : duration.tv_sec * 1000000000 + duration.tv_nsec;
				return POSIX_CALL(LibKernel::SyncOnAddress::Wait64(
				    static_cast<volatile uint64_t*>(address), value, std::chrono::nanoseconds(timeout_ns),
				    LibKernel::KernelDispatchPendingSignalForCurrentThread));
			}
			return POSIX_CALL(LibKernel::SyncOnAddress::Wait64(
			    static_cast<volatile uint64_t*>(address), value, nullptr,
			    LibKernel::KernelDispatchPendingSignalForCurrentThread));
		case UMTX_OP_WAKE:
			return POSIX_CALL(LibKernel::SyncOnAddress::Wake(address, static_cast<int32_t>(value)));
		default: EXIT("Unsupported _umtx_op operation: %d\n", operation);
	}
}

// ps5-vsh: libSceJsc and libhermes-0.11 both import this, and it was the most
// frequently executed unresolved kernel stub in the RN runs. The JS engines
// only read it for diagnostics, so answer with the host's process CPU time and
// zeroed fault/IO counters (a valid "nothing to report", not an error).
// Layout is the FreeBSD/PS5 one: two timevals then 14 64-bit counters (144 B).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetrusage(int who, void* usage) {
	struct Timeval {
		int64_t sec;
		int64_t usec;
	};
	struct Rusage {
		Timeval utime;
		Timeval stime;
		int64_t counters[14];
	};
	if (usage != nullptr) {
		auto* ru = static_cast<Rusage*>(usage);
		std::memset(ru, 0, sizeof(Rusage));
		const auto ticks = std::clock();
		if (ticks != static_cast<std::clock_t>(-1)) {
			ru->utime.sec  = static_cast<int64_t>(ticks / CLOCKS_PER_SEC);
			ru->utime.usec = static_cast<int64_t>((ticks % CLOCKS_PER_SEC) * 1000000 / CLOCKS_PER_SEC);
		}
	}
	return 0;
}

LIB_DEFINE(InitLibKernel_1_Posix) {
	LIB_FUNC("hHlZQUnlxSM", KernelGetrusage);
	LIB_FUNC("cfjAjVTFG6A", LibKernel::PthreadSuspendUserContext);
	LIB_FUNC("YkGOXpJEtO8", LibKernel::PthreadGetUserContext);
	LIB_FUNC("QRdE7dBfNks", LibKernel::PthreadResumeUserContext);
	LIB_FUNC("k+AXqu2-eBc", getpagesize);
	LIB_FUNC("lLMT9vJAck0", clock_gettime);
	LIB_FUNC("smIj7eqzZE8", clock_getres);
	LIB_FUNC("n88vx3C5nW8", gettimeofday);
	LIB_FUNC("NhpspxdjEKU", nanosleep);
	LIB_FUNC("yS8U2TGCe1A", nanosleep);
	LIB_FUNC("E6ao34wPw+U", stat);
	LIB_FUNC("JGMio+21L4c", mkdir);
	LIB_FUNC("ih4CD9-gghM", Posix::ftruncate);
	LIB_FUNC("+0EDo7YzcoU", Posix::futimes);
	LIB_FUNC("juWbTNM+8hw", Posix::fsync);
	LIB_FUNC("8nY19bKoiZk", Posix::fcntl);
	LIB_FUNC("pDuPEf3m4fI", Posix::sem_init);
	LIB_FUNC("cDW233RAwWo", Posix::sem_destroy);
	LIB_FUNC("YCV5dGGBcCo", Posix::sem_wait);
	LIB_FUNC("WBWzsRifCEA", Posix::sem_trywait);
	LIB_FUNC("4SbrhCozqQU", Posix::sem_reltimedwait_np);
	LIB_FUNC("w5IHyvahg-o", Posix::sem_timedwait);
	LIB_FUNC("IKP8typ0QUk", Posix::sem_post);
	LIB_FUNC("Bq+LRV-N6Hk", Posix::sem_getvalue);

	LIB_FUNC("OxhIB8LB-PQ", Posix::pthread_create);
	LIB_FUNC("Jmi+9w9u0E4", Posix::pthread_create_name_np);
	LIB_FUNC("+U1R4WtXvoc", Posix::pthread_detach);
	LIB_FUNC("FJrT5LuUBAU", Posix::pthread_exit);
	LIB_FUNC("EotR8a3ASf4", Posix::pthread_self);
	LIB_FUNC("9vyP6Z7bqzc", Posix::pthread_rename_np);
	LIB_FUNC("lZzFeSxPl08", Posix::pthread_setcancelstate);
	LIB_FUNC("9HzfhdtESio", Posix::pthread_getname_np);
	LIB_FUNC("a2P9wYGeZvc", Posix::pthread_setprio);
	LIB_FUNC("FIs3-UQT9sg", Posix::pthread_getschedparam);
	LIB_FUNC("P41kTWUS3EI", Posix::pthread_getschedparam);
	LIB_FUNC("Xs9hdiD7sAA", Posix::pthread_setschedparam);
	LIB_FUNC("oIRFTjoILbg", Posix::pthread_setschedparam);
	LIB_FUNC("B5GmVDKwpn0", Posix::pthread_yield);
	LIB_FUNC("h9CcP3J0oVM", Posix::pthread_join);
	LIB_FUNC("yH-uQW3LbX0", Posix::pthread_kill);
	LIB_FUNC("wtkt-teR1so", Posix::pthread_attr_init);
	// Firmware 13.60 verified POSIX contracts; libScePosix v1 / libkernel 1.1.
	LIB_FUNC("+Pqub9HZCPo", Posix::pthread_barrier_destroy);
	LIB_FUNC("CawZgCYqXWk", Posix::pthread_barrier_wait);
	LIB_FUNC("ZsXLFtd2jqQ", Posix::pthread_barrier_init);
	LIB_FUNC("xesmlSI-KCI", Posix::pthread_attr_setscope);
	LIB_FUNC("zHchY8ft5pk", Posix::pthread_attr_destroy);
	LIB_FUNC("vQm4fDEsWi8", Posix::pthread_attr_getstack);
	LIB_FUNC("-SrbXpGR1f0", Posix::pthread_attr_setstack);
	LIB_FUNC("2Q0z6rnBrTE", Posix::pthread_attr_setstacksize);
	LIB_FUNC("Ucsu-OK+els", Posix::pthread_attr_get_np);
	LIB_FUNC("RtLRV-pBTTY", Posix::pthread_attr_getschedpolicy);
	LIB_FUNC("JarMIy8kKEY", Posix::pthread_attr_setschedpolicy);
	LIB_FUNC("E+tyo3lp5Lw", Posix::pthread_attr_setdetachstate);
	LIB_FUNC("euKRgm0Vn2M", Posix::pthread_attr_setschedparam);
	LIB_FUNC("7ZlAakEf0Qg", Posix::pthread_attr_setinheritsched);
	LIB_FUNC("0qOtCR-ZHck", Posix::pthread_attr_getstacksize);
	LIB_FUNC("VUT1ZSrHT0I", Posix::pthread_attr_getdetachstate);
	LIB_FUNC("JKyG3SWyA10", Posix::pthread_attr_setguardsize);
	LIB_FUNC("JNkVVsVDmOk", Posix::pthread_attr_getguardsize);
	LIB_FUNC("qlk9pSLsUmM", Posix::pthread_attr_getschedparam);
	LIB_FUNC("FXPWHNk8Of0", Posix::pthread_attr_getschedparam);
	LIB_FUNC("7Xl257M4VNI", LibKernel::PthreadEqual);
	LIB_FUNC("7H0iTOciTLo", Posix::pthread_mutex_lock);
	LIB_FUNC("K-jXhbt2gn4", Posix::pthread_mutex_trylock);
	LIB_FUNC("2Z+PpY6CaJg", Posix::pthread_mutex_unlock);
	LIB_FUNC("ttHNfU+qDBU", Posix::pthread_mutex_init);
	LIB_FUNC("dQHWEsJtoE4", Posix::pthread_mutexattr_init);
	LIB_FUNC("mDmgMOGVUqg", Posix::pthread_mutexattr_settype);
	LIB_FUNC("5txKfcMUAok", Posix::pthread_mutexattr_setprotocol);
	LIB_FUNC("HF7lK46xzjY", Posix::pthread_mutexattr_destroy);
	LIB_FUNC("ltCfaGr2JGE", Posix::pthread_mutex_destroy);
	LIB_FUNC("mkx2fVhNMsg", Posix::pthread_cond_broadcast);
	LIB_FUNC("2MOy+rUfuhQ", Posix::pthread_cond_signal);
	LIB_FUNC("0TyVk4MSLt0", Posix::pthread_cond_init);
	LIB_FUNC("Op8TBGY5KHg", Posix::pthread_cond_wait);
	LIB_FUNC("Z4QosVuAsA0", Posix::pthread_once);
	LIB_FUNC("1471ajPzxh0", Posix::pthread_rwlock_destroy);
	LIB_FUNC("ytQULN-nhL4", Posix::pthread_rwlock_init);
	LIB_FUNC("sIlRvQqsN2Y", Posix::pthread_rwlock_wrlock);
	LIB_FUNC("SFxTMOfuCkE", LibKernel::PthreadRwlockTryrdlock);
	LIB_FUNC("XhWHn6P5R7U", LibKernel::PthreadRwlockTrywrlock);
	LIB_FUNC("mqULNdimTn0", Posix::pthread_key_create);
	LIB_FUNC("6BpEZuDT7YI", Posix::pthread_key_delete);
	LIB_FUNC("WrOLvHU0yQM", Posix::pthread_setspecific);
	LIB_FUNC("0-KXaS70xy4", Posix::pthread_getspecific);
	LIB_FUNC("-quPa4SEJUw", Posix::pthread_getstack);
	LIB_FUNC("YQOfxL4QfeU", LibKernel::Memory::KernelMprotect);
	LIB_FUNC("UqDGjXA5yUM", LibKernel::Memory::KernelMunmap);
	LIB_FUNC("AqBioC2vF3I", LibKernel::read);
	LIB_FUNC("TU-d9PfIHPM", Posix::socket);
	LIB_FUNC("KuOmgKoqCdY", Posix::bind);
	LIB_FUNC("XVL8So3QJUk", Posix::connect);
	LIB_FUNC("TUuiYS2kE8s", Posix::shutdown);
	LIB_FUNC("pxnCmagrtao", Posix::listen);
	LIB_FUNC("3e+4Iv7IJ8U", Posix::accept);
	LIB_FUNC("RenI1lL1WFk", Posix::getsockname);
	LIB_FUNC("TXFFFiNldU8", Posix::getpeername);
	LIB_FUNC("wuCroIGjt2g", LibKernel::open);
	LIB_FUNC("6O8EwYOgH9Y", Posix::getsockopt);
	LIB_FUNC("fFxGkxF2bVo", Posix::setsockopt);
	LIB_FUNC("T8fER+tIGgk", Posix::select);
	LIB_FUNC("fZOeZIOEmLw", Posix::send);
	LIB_FUNC("oBr313PppNE", Posix::sendto);
	LIB_FUNC("Ez8xjo9UF4E", Posix::recv);
	LIB_FUNC("lUk6wrGXyMw", Posix::recvfrom);
	LIB_FUNC("4n51s0zEf0c", Posix::inet_pton);
	LIB_FUNC("5jRCs2axtr4", Posix::inet_ntop);
	LIB_FUNC("cfwBSQyr5Ys", cfwBSQyr5Ys);
}

} // namespace Posix

namespace Coredump {

LIB_VERSION("Coredump", 1, "Coredump", 1, 1);

static uint64_t g_coredump_handler         = 0;
static uint64_t g_coredump_handler_context = 0;

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceCoredumpRegisterCoredumpHandler(uint64_t handler, size_t stack_size,
                                                     uint64_t context) {
	PRINT_NAME();

	g_coredump_handler         = handler;
	g_coredump_handler_context = context;

	LOGF("\t handler = 0x%016" PRIx64 "\n"
	     "\t stack   = 0x%016" PRIx64 "\n"
	     "\t context = 0x%016" PRIx64 "\n",
	     handler, static_cast<uint64_t>(stack_size), context);

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI sceCoredumpUnregisterCoredumpHandler() {
	PRINT_NAME();

	g_coredump_handler         = 0;
	g_coredump_handler_context = 0;

	return OK;
}

} // namespace Coredump

namespace Fiber {

LIB_VERSION("Fiber", 1, "Fiber", 1, 1);

constexpr int32_t FIBER_ERROR_NULL       = -2141650943; /* 0x80590001 */
constexpr int32_t FIBER_ERROR_ALIGNMENT  = -2141650942; /* 0x80590002 */
constexpr int32_t FIBER_ERROR_RANGE      = -2141650941; /* 0x80590003 */
constexpr int32_t FIBER_ERROR_INVALID    = -2141650940; /* 0x80590004 */
constexpr int32_t FIBER_ERROR_PERMISSION = -2141650939; /* 0x80590005 */
constexpr int32_t FIBER_ERROR_STATE      = -2141650938; /* 0x80590006 */

constexpr uint32_t FIBER_MAGIC_START       = 0xdef1649c;
constexpr uint32_t FIBER_MAGIC_END         = 0xb37592a0;
constexpr uint32_t FIBER_OPT_MAGIC         = 0xbb40e64d;
constexpr uint64_t FIBER_STACK_MAGIC       = 0x7149f2ca7149f2ca;
constexpr uint64_t FIBER_CONTEXT_MIN_SIZE  = 512;
constexpr size_t   FIBER_MAX_NAME_LENGTH   = 31;
constexpr uint32_t FIBER_STATE_RUNNING     = 1;
constexpr uint32_t FIBER_STATE_IDLE        = 2;
constexpr uint32_t FIBER_STATE_TERMINATED  = 3;
constexpr uint32_t FIBER_FLAG_SET_FPU_REGS = 0x100;

using FiberEntry = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void (*)(uint64_t arg_on_initialize, uint64_t arg_on_run);

struct FiberOptParam {
	uint32_t magic;
};

struct FiberCpuContext {
	uint64_t rbx;
	uint64_t rbp;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rsp;
	uint64_t rip;
};

static_assert(sizeof(FiberCpuContext) == 80);

struct FiberObject {
	uint32_t        magic_start;
	uint32_t        state;
	FiberEntry      entry;
	uint64_t        arg_on_initialize;
	void*           addr_context;
	uint64_t        size_context;
	char            name[FIBER_MAX_NAME_LENGTH + 1];
	void*           context;
	uint32_t        flags;
	uint32_t        padding;
	void*           context_start;
	void*           context_end;
	FiberCpuContext saved_context;
	uint64_t        arg_on_run;
	uint64_t        arg_on_return;
	bool            context_valid;
	uint32_t        magic_end;
};

static_assert(sizeof(FiberObject) <= 256);

struct FiberInfo {
	uint64_t   size;
	FiberEntry entry;
	uint64_t   arg_on_initialize;
	void*      addr_context;
	uint64_t   size_context;
	char       name[FIBER_MAX_NAME_LENGTH + 1];
	uint64_t   size_context_margin;
	uint8_t    padding[48];
};

static_assert(sizeof(FiberInfo) == 128);

static bool FiberIsValid(const FiberObject* fiber) {
	return fiber != nullptr && fiber->magic_start == FIBER_MAGIC_START &&
	       fiber->magic_end == FIBER_MAGIC_END;
}

struct FiberThreadContext {
	FiberObject*    current_fiber;
	FiberObject*    returned_fiber;
	FiberObject*    pending_idle_fiber;
	FiberCpuContext cpu_context;
};

static const auto g_fiber_context_key = [] {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto key = TlsAlloc();
	if (key == TLS_OUT_OF_INDEXES) {
		std::abort();
	}
#else
	pthread_key_t key;
	if (pthread_key_create(&key, nullptr) != 0) {
		std::abort();
	}
#endif
	return key;
}();

// Native lookup must stay fresh when a fiber resumes on another host thread.
static FiberThreadContext* FiberGetThreadContext() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<FiberThreadContext*>(TlsGetValue(g_fiber_context_key));
#else
	return static_cast<FiberThreadContext*>(pthread_getspecific(g_fiber_context_key));
#endif
}

static void FiberSetThreadContext(FiberThreadContext* context) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (TlsSetValue(g_fiber_context_key, context) == 0) {
#else
	if (pthread_setspecific(g_fiber_context_key, context) != 0) {
#endif
		std::abort();
	}
}

static uint32_t FiberLoadState(const FiberObject* fiber) {
	auto& state = const_cast<uint32_t&>(fiber->state);
	return std::atomic_ref<uint32_t>(state).load(std::memory_order_acquire);
}

static void FiberStoreState(FiberObject* fiber, uint32_t state) {
	std::atomic_ref<uint32_t>(fiber->state).store(state, std::memory_order_release);
}

static void FiberCompleteSwitch(FiberObject* current) {
	auto* context = FiberGetThreadContext();
	context->current_fiber = current;
	// Only publish IDLE after switching away from the departing fiber's stack.
	auto* fiber = context->pending_idle_fiber;
	context->pending_idle_fiber = nullptr;
	if (fiber != nullptr) {
		FiberStoreState(fiber, FIBER_STATE_IDLE);
	}
}

static bool FiberCompareExchangeState(FiberObject* fiber, uint32_t expected, uint32_t desired) {
	return std::atomic_ref<uint32_t>(fiber->state)
	    .compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
	                             std::memory_order_acquire);
}

static void FiberSetContextValid(FiberObject* fiber, bool valid) {
	fiber->context_valid = valid;
	fiber->context       = valid ? &fiber->saved_context : nullptr;
}

#if defined(__x86_64__) || defined(_M_X64)
[[gnu::naked,
  gnu::returns_twice]] static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int FiberSaveContext(FiberCpuContext* /*ctx*/) {
	asm volatile("movq %rdi, %r10\n\t"
	             "movq %rbx, 0(%r10)\n\t"
	             "movq %rbp, 8(%r10)\n\t"
	             "movq %rdi, 16(%r10)\n\t"
	             "movq %rsi, 24(%r10)\n\t"
	             "movq %r12, 32(%r10)\n\t"
	             "movq %r13, 40(%r10)\n\t"
	             "movq %r14, 48(%r10)\n\t"
	             "movq %r15, 56(%r10)\n\t"
	             "leaq 8(%rsp), %r11\n\t"
	             "movq %r11, 64(%r10)\n\t"
	             "movq (%rsp), %r11\n\t"
	             "movq %r11, 72(%r10)\n\t"
	             "xorl %eax, %eax\n\t"
	             "retq\n");
}

[[gnu::naked, gnu::noreturn]] static KYTY_SYSV_ABI void
FiberRestoreContext(FiberCpuContext* /*ctx*/, uint64_t /*ret*/) {
	asm volatile("movq %rdi, %r10\n\t"
	             "movq %rsi, %rax\n\t"
	             "movq 72(%r10), %r11\n\t"
	             "movq 0(%r10), %rbx\n\t"
	             "movq 8(%r10), %rbp\n\t"
	             "movq 16(%r10), %rdi\n\t"
	             "movq 24(%r10), %rsi\n\t"
	             "movq 32(%r10), %r12\n\t"
	             "movq 40(%r10), %r13\n\t"
	             "movq 48(%r10), %r14\n\t"
	             "movq 56(%r10), %r15\n\t"
	             "movq 64(%r10), %rsp\n\t"
	             "jmp *%r11\n");
}
#else
static int FiberSaveContext(FiberCpuContext* ctx) {
	(void)ctx;
	return 0;
}

static void FiberRestoreContext(FiberCpuContext* ctx, uint64_t ret) {
	(void)ctx;
	(void)ret;
	EXIT("Fiber context switching is only implemented on x86_64\n");
}
#endif

[[noreturn]] static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void FiberStartTrampoline(FiberObject* fiber);

[[noreturn]] static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void FiberStartOnGuestStack(FiberObject* fiber) {
	FiberCpuContext ctx {};
	const auto      stack_top = reinterpret_cast<uintptr_t>(fiber->addr_context) +
	                            static_cast<uintptr_t>(fiber->size_context);
	auto            rsp       = (stack_top & ~static_cast<uintptr_t>(0x0f));
	rsp -= sizeof(uint64_t);
	*reinterpret_cast<uint64_t*>(rsp) = 0;

	ctx.rsp = rsp;
	ctx.rip = reinterpret_cast<uint64_t>(&FiberStartTrampoline);
	ctx.rdi = reinterpret_cast<uint64_t>(fiber);

	FiberRestoreContext(&ctx, 1);
}

[[noreturn]] static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void FiberStartTrampoline(FiberObject* fiber) {
	FiberCompleteSwitch(fiber);

	fiber->entry(fiber->arg_on_initialize, fiber->arg_on_run);

	FiberStoreState(fiber, FIBER_STATE_TERMINATED);
	FiberSetContextValid(fiber, false);
	fiber->arg_on_return  = 0;
	auto* context = FiberGetThreadContext();
	context->returned_fiber = fiber;
	context->current_fiber = nullptr;

	FiberRestoreContext(&context->cpu_context, 1);
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberInitialize(FiberObject* fiber, const char* name, FiberEntry entry,
                                      uint64_t arg_on_initialize, void* addr_context,
                                      uint64_t size_context, const FiberOptParam* opt_param,
                                      uint32_t build_version) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (fiber == nullptr || name == nullptr || entry == nullptr) {
		return FIBER_ERROR_NULL;
	}

	if ((reinterpret_cast<uint64_t>(fiber) & 7u) != 0 ||
	    (reinterpret_cast<uint64_t>(addr_context) & 15u) != 0 ||
	    (opt_param != nullptr && (reinterpret_cast<uint64_t>(opt_param) & 7u) != 0)) {
		return FIBER_ERROR_ALIGNMENT;
	}

	if (size_context != 0 && size_context < FIBER_CONTEXT_MIN_SIZE) {
		return FIBER_ERROR_RANGE;
	}

	if ((size_context & 15u) != 0 || (addr_context == nullptr && size_context != 0) ||
	    (addr_context != nullptr && size_context == 0) ||
	    (opt_param != nullptr && opt_param->magic != FIBER_OPT_MAGIC)) {
		return FIBER_ERROR_INVALID;
	}

	std::memset(fiber, 0, sizeof(*fiber));
	std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
	fiber->name[FIBER_MAX_NAME_LENGTH] = '\0';

	fiber->magic_start       = FIBER_MAGIC_START;
	fiber->state             = FIBER_STATE_IDLE;
	fiber->entry             = entry;
	fiber->arg_on_initialize = arg_on_initialize;
	fiber->addr_context      = addr_context;
	fiber->size_context      = size_context;
	fiber->flags             = (build_version >= 0x03500000 ? FIBER_FLAG_SET_FPU_REGS : 0);
	fiber->context_start     = addr_context;
	fiber->context_end =
	    (addr_context != nullptr ? static_cast<uint8_t*>(addr_context) + size_context : nullptr);
	std::memset(&fiber->saved_context, 0, sizeof(fiber->saved_context));
	FiberSetContextValid(fiber, false);
	fiber->magic_end = FIBER_MAGIC_END;

	if (addr_context != nullptr) {
		*static_cast<uint64_t*>(addr_context) = FIBER_STACK_MAGIC;
	}

	LOGF("\t fiber init: %s, entry = 0x%016" PRIx64 ", context = 0x%016" PRIx64 ", size = %" PRIu64
	     "\n",
	     fiber->name, reinterpret_cast<uint64_t>(entry), reinterpret_cast<uint64_t>(addr_context),
	     size_context);

	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberInitializeInternal(FiberObject* fiber, const char* name,
                                              FiberEntry entry, uint64_t arg_on_initialize,
                                              void* addr_context, uint64_t size_context,
                                              const FiberOptParam* opt_param, uint32_t flags,
                                              uint32_t build_version) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	auto ret = FiberInitialize(fiber, name, entry, arg_on_initialize, addr_context, size_context,
	                           opt_param, build_version);
	if (ret == OK && fiber != nullptr) {
		fiber->flags |= flags;
	}
	return ret;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberOptParamInitialize(FiberOptParam* opt_param) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (opt_param == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if ((reinterpret_cast<uint64_t>(opt_param) & 7u) != 0) {
		return FIBER_ERROR_ALIGNMENT;
	}

	std::memset(opt_param, 0, 128);
	opt_param->magic = FIBER_OPT_MAGIC;

	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberFinalize(FiberObject* fiber) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (fiber == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_TERMINATED)) {
		return FIBER_ERROR_STATE;
	}

	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberRun(FiberObject* fiber, uint64_t arg_on_run, uint64_t* arg_on_return) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	if (FiberGetThreadContext() != nullptr) {
		return FIBER_ERROR_PERMISSION;
	}
	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING)) {
		return FIBER_ERROR_STATE;
	}

	fiber->arg_on_run    = arg_on_run;
	fiber->arg_on_return = 0;
	// Register on the host stack; switching fibers only reads the native TLS slot.
	FiberThreadContext context {};
	FiberSetThreadContext(&context);

	if (FiberSaveContext(&context.cpu_context) == 0) {
		if (fiber->context_valid) {
			FiberRestoreContext(&fiber->saved_context, 1);
		}
		FiberStartOnGuestStack(fiber);
	}

	auto* returned_fiber  = (context.returned_fiber != nullptr ? context.returned_fiber : fiber);
	const auto return_value = returned_fiber->arg_on_return;
	const auto return_code =
	    FiberLoadState(returned_fiber) == FIBER_STATE_TERMINATED ? FIBER_ERROR_STATE : OK;
	// Copy results before another thread can acquire and resume the fiber.
	FiberCompleteSwitch(nullptr);
	FiberSetThreadContext(nullptr);

	if (arg_on_return != nullptr) {
		*arg_on_return = return_value;
	}

	return return_code;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberSwitch(FiberObject* fiber, uint64_t arg_on_run,
                                         uint64_t* arg_on_return) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}
	auto* context = FiberGetThreadContext();
	if (context == nullptr) {
		return FIBER_ERROR_PERMISSION;
	}

	if (!FiberCompareExchangeState(fiber, FIBER_STATE_IDLE, FIBER_STATE_RUNNING)) {
		return FIBER_ERROR_STATE;
	}

	auto* caller = context->current_fiber;

	fiber->arg_on_run    = arg_on_run;
	fiber->arg_on_return = 0;

	if (FiberSaveContext(&caller->saved_context) == 0) {
		FiberSetContextValid(caller, true);
		context->pending_idle_fiber = caller;
		if (fiber->context_valid) {
			FiberRestoreContext(&fiber->saved_context, 1);
		}
		FiberStartOnGuestStack(fiber);
	}

	FiberCompleteSwitch(caller);

	if (arg_on_return != nullptr) {
		*arg_on_return = caller->arg_on_run;
	}

	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberGetSelf(FiberObject** fiber) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (fiber == nullptr) {
		return FIBER_ERROR_NULL;
	}

	const auto* context = FiberGetThreadContext();
	*fiber = context != nullptr ? context->current_fiber : nullptr;

	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberReturnToThread(uint64_t arg_on_return, uint64_t* arg_on_run) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	auto* context = FiberGetThreadContext();
	if (context == nullptr) {
		return FIBER_ERROR_PERMISSION;
	}

	auto* fiber          = context->current_fiber;
	fiber->arg_on_return = arg_on_return;

	if (FiberSaveContext(&fiber->saved_context) == 0) {
		FiberSetContextValid(fiber, true);
		context->returned_fiber = fiber;
		context->pending_idle_fiber = fiber;
		FiberRestoreContext(&context->cpu_context, 1);
	}

	FiberCompleteSwitch(fiber);
	if (arg_on_run != nullptr) {
		*arg_on_run = fiber->arg_on_run;
	}

	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberGetInfo(FiberObject* fiber, FiberInfo* fiber_info) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (fiber == nullptr || fiber_info == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}

	std::memset(fiber_info, 0, sizeof(*fiber_info));
	fiber_info->size              = sizeof(*fiber_info);
	fiber_info->entry             = fiber->entry;
	fiber_info->arg_on_initialize = fiber->arg_on_initialize;
	fiber_info->addr_context      = fiber->addr_context;
	fiber_info->size_context      = fiber->size_context;
	std::strncpy(fiber_info->name, fiber->name, FIBER_MAX_NAME_LENGTH);

	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberStartContextSizeCheck(uint32_t flags) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();
	LOGF("\t flags = 0x%08" PRIx32 "\n", flags);
	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberStopContextSizeCheck() {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();
	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberRename(FiberObject* fiber, const char* name) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (fiber == nullptr || name == nullptr) {
		return FIBER_ERROR_NULL;
	}
	if (!FiberIsValid(fiber)) {
		return FIBER_ERROR_INVALID;
	}

	std::strncpy(fiber->name, name, FIBER_MAX_NAME_LENGTH);
	fiber->name[FIBER_MAX_NAME_LENGTH] = '\0';

	return OK;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI FiberGetThreadFramePointerAddress(uint64_t* addr_frame_pointer) {
	if (LibKernel::SharedRuntimeCaller()) { return LibKernel::KERNEL_ERROR_EOPNOTSUPP; }
	PRINT_NAME();

	if (addr_frame_pointer == nullptr) {
		return FIBER_ERROR_NULL;
	}

	*addr_frame_pointer = 0;

	return OK;
}

} // namespace Fiber

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
namespace LibKernel {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static std::atomic<unsigned> g_test_native_pin_direct_signals {0};
static void KYTY_SYSV_ABI TestNativePinSignalHandler(int, void*) { ++g_test_native_pin_direct_signals; }
static std::atomic<uintptr_t> g_test_native_pin_apc_sequence {0};
static std::atomic<uintptr_t> g_test_native_pin_apc_completed {0};
static void TestNativePinApcHandler(void* arg1, void* arg2, void* arg3, PCONTEXT context) {
	SignalApcHandler(arg1, arg2, nullptr, context);
	g_test_native_pin_apc_completed.store(reinterpret_cast<uintptr_t>(arg3), std::memory_order_release);
}
#endif
bool TestRuntimeNativeTargetPinServices(Pthread ended_legacy, void* shared_native_handle) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!GetNtQueueApcThreadEx()) { return false; } // No numeric-ID fallback on unsupported hosts.
	struct RestoreHandlers {
		void* exception[2] {g_exception_handlers[30], g_exception_handlers[31]};
		uint64_t signal[2] {g_sigaction_handlers[30], g_sigaction_handlers[31]};
		int32_t flags[2] {g_sigaction_flags[30], g_sigaction_flags[31]};
		~RestoreHandlers() {
			for (unsigned i = 0; i != 2; ++i) {
				g_exception_handlers[30 + i] = exception[i];
				g_sigaction_handlers[30 + i] = signal[i];
				g_sigaction_flags[30 + i] = flags[i];
			}
		}
	} restore;
	for (unsigned i: {30u, 31u}) {
		g_exception_handlers[i] = reinterpret_cast<void*>(&TestNativePinSignalHandler);
		g_sigaction_handlers[i] = reinterpret_cast<uint64_t>(&TestNativePinSignalHandler);
		g_sigaction_flags[i] = 0;
	}
	const auto before = g_test_native_pin_direct_signals.load();
	if (KernelRaiseException(PthreadSelfOrNull(), 30) != OK || g_test_native_pin_direct_signals.load() != before + 1) {
		return false;
	}
	// Actual terminated native object, not an OpenThread/queue mock. Failed
	// repeated enqueue must leave earlier accepted work intact; a new reservation
	// must roll back. The fixture has not joined/recycled this record yet.
	PthreadQueuePendingSignal(ended_legacy, 30);
	const bool older = KernelRaiseException(ended_legacy, 30) == KERNEL_ERROR_EINVAL &&
	    PthreadHasPendingSignal(ended_legacy, 30);
	const bool newer = KernelRaiseException(ended_legacy, 31) == KERNEL_ERROR_EINVAL &&
	    !PthreadHasPendingSignal(ended_legacy, 31) && PthreadHasPendingSignal(ended_legacy, 30);
	// Deliberately bypass the public sender in this fixture ONLY: an actual
	// special APC runs on a shared native thread with a legacy argument record.
	// The actual-thread check must reject it BEFORE consuming the legacy bit.
	const auto sequence = g_test_native_pin_apc_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	KytyUserApcOption option {};
	option.UserApcFlags = KytyQueueUserApcFlagsSpecialUserApc;
	const auto status = GetNtQueueApcThreadEx()(static_cast<HANDLE>(shared_native_handle), option,
	    TestNativePinApcHandler, ended_legacy, reinterpret_cast<void*>(30), reinterpret_cast<void*>(sequence));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (status == 0 && g_test_native_pin_apc_completed.load(std::memory_order_acquire) != sequence &&
	       std::chrono::steady_clock::now() < deadline) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
	const bool actual_target = status == 0 && g_test_native_pin_apc_completed.load(std::memory_order_acquire) == sequence &&
	    PthreadHasPendingSignal(ended_legacy, 30);
	PthreadTakePendingSignal(ended_legacy, 30); // Fixture cleanup only, after verifying retention.
	return older && newer && actual_target && g_test_native_pin_direct_signals.load() == before + 1;
#else
	return false; // Windows native-handle fixture is not exercised here.
#endif
}
bool TestRuntimeGuestStopServices(Pthread target) {
	const auto handler = g_exception_handlers[30];
	const auto sigaction = g_sigaction_handlers[30];
	const auto flags = g_sigaction_flags[30];
	GuestSigaction action {}, output {};
	output.handler = 0xeeee;
	const auto previous_errno = *Posix::GetErrorAddr();
	const bool rejected = Sigaction(30, &action, &output) == KERNEL_ERROR_EOPNOTSUPP && output.handler == 0xeeee &&
	    Signal(30, 0) == UINT64_MAX && KernelInstallExceptionHandler(30, nullptr) == KERNEL_ERROR_EOPNOTSUPP &&
	    KernelRemoveExceptionHandler(30) == KERNEL_ERROR_EOPNOTSUPP &&
	    KernelRaiseException(target, 30) == KERNEL_ERROR_EOPNOTSUPP && PthreadKill(target, 30) == KERNEL_ERROR_EOPNOTSUPP &&
	    PthreadSuspendUserContext(target) == Posix::POSIX_EOPNOTSUPP &&
	    PthreadGetUserContext(target, nullptr) == Posix::POSIX_EOPNOTSUPP &&
	    PthreadResumeUserContext(target) == Posix::POSIX_EOPNOTSUPP &&
	    Fiber::FiberRun(nullptr, 0, nullptr) == KERNEL_ERROR_EOPNOTSUPP &&
	    Fiber::FiberSwitch(nullptr, 0, nullptr) == KERNEL_ERROR_EOPNOTSUPP &&
	    Fiber::FiberReturnToThread(0, nullptr) == KERNEL_ERROR_EOPNOTSUPP;
	*Posix::GetErrorAddr() = previous_errno;
	PthreadQueuePendingSignal(target, 30);
	if (!rejected || PthreadHasPendingSignal(target, 30) || g_exception_handlers[30] != handler ||
	    g_sigaction_handlers[30] != sigaction || g_sigaction_flags[30] != flags) { return false; }
	// A genuine legacy sender must also be unable to queue/edit a shared target.
	auto* previous = Loader::RuntimeLinker::GetCurrentProgram();
	Loader::Program session;
	Loader::RuntimeLinker::SetCurrentProgram(&session);
	const bool legacy = Sigaction(30, nullptr, &output) == OK &&
	    KernelRaiseException(target, 30) == KERNEL_ERROR_EOPNOTSUPP &&
	    PthreadSuspendUserContext(target) == Posix::POSIX_EOPNOTSUPP;
	Loader::RuntimeLinker::SetCurrentProgram(previous);
	return legacy;
}
} // namespace LibKernel
#endif

int chmod(const char* path, int mode) {
	PRINT_NAME();

	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioInitializeImpl(void* param, int32_t size) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n"
	     "\t size  = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(param), static_cast<uint32_t>(size));

	return OK;
}

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioInitializeParam(void* param) {
	PRINT_NAME();

	LOGF("\t param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(param));
}

constexpr int32_t KERNEL_AIO_STATE_SUBMITTED  = 1;
constexpr int32_t KERNEL_AIO_STATE_PROCESSING = 2;
constexpr int32_t KERNEL_AIO_STATE_COMPLETED  = 3;
constexpr int32_t KERNEL_AIO_STATE_ABORTED    = 4;
constexpr int32_t KERNEL_AIO_STATE_NOTIFIED   = 0x10000;
constexpr int32_t KERNEL_AIO_MAX_QUEUE        = 512;
constexpr int32_t KERNEL_AIO_MAX_REQUESTS     = 128;
constexpr uint32_t KERNEL_AIO_WAIT_AND        = 1;
constexpr uint32_t KERNEL_AIO_WAIT_OR         = 2;

struct KernelAioResult {
	int64_t  return_value;
	uint32_t state;
};

struct KernelAioRwRequest {
	int64_t          offset;
	size_t           nbyte;
	void*            buf;
	KernelAioResult* result;
	int32_t          fd;
};

static std::array<std::atomic_int32_t, KERNEL_AIO_MAX_QUEUE> g_kernel_aio_state = {};
static std::atomic_int32_t                                   g_kernel_aio_next_id {1};

static int32_t kernel_aio_next_id() {
	const auto sequence = g_kernel_aio_next_id.fetch_add(1, std::memory_order_relaxed);
	return ((sequence - 1) % (KERNEL_AIO_MAX_QUEUE - 1)) + 1;
}

static bool kernel_aio_is_valid_id(int32_t id) {
	return id > 0 && id < KERNEL_AIO_MAX_QUEUE;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioSubmitReadCommands(KernelAioRwRequest* req, int32_t size, int32_t prio,
                                              int32_t* id) {
	PRINT_NAME();

	if (req == nullptr || id == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	if (size <= 0 || size > KERNEL_AIO_MAX_REQUESTS) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const auto submit_id = kernel_aio_next_id();
	g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_PROCESSING, std::memory_order_release);

	for (int32_t i = 0; i < size; i++) {
		if (req[i].result == nullptr) {
			g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_ABORTED,
			                                    std::memory_order_release);
			return LibKernel::KERNEL_ERROR_EFAULT;
		}

		req[i].result->state = KERNEL_AIO_STATE_SUBMITTED;

		const auto result =
		    LibKernel::FileSystem::KernelPread(req[i].fd, req[i].buf, req[i].nbyte, req[i].offset);

		req[i].result->return_value = result;
		req[i].result->state = (result < 0 ? KERNEL_AIO_STATE_ABORTED : KERNEL_AIO_STATE_COMPLETED);
	}

	g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_COMPLETED, std::memory_order_release);
	*id = submit_id;

	(void)prio;
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioSubmitWriteCommands(KernelAioRwRequest* req, int32_t size, int32_t prio,
                                               int32_t* id) {
	PRINT_NAME();

	if (req == nullptr || id == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	if (size <= 0 || size > KERNEL_AIO_MAX_REQUESTS) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const auto submit_id = kernel_aio_next_id();
	g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_PROCESSING, std::memory_order_release);

	for (int32_t i = 0; i < size; i++) {
		if (req[i].result == nullptr) {
			g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_ABORTED,
			                                    std::memory_order_release);
			return LibKernel::KERNEL_ERROR_EFAULT;
		}

		req[i].result->state = KERNEL_AIO_STATE_SUBMITTED;

		const auto result =
		    LibKernel::FileSystem::KernelPwrite(req[i].fd, req[i].buf, req[i].nbyte, req[i].offset);

		req[i].result->return_value = result;
		req[i].result->state = (result < 0 ? KERNEL_AIO_STATE_ABORTED : KERNEL_AIO_STATE_COMPLETED);
	}

	g_kernel_aio_state[submit_id].store(KERNEL_AIO_STATE_COMPLETED, std::memory_order_release);
	*id = submit_id;

	(void)prio;
	return OK;
}

static int32_t kernel_aio_get_state(int32_t id, bool notify) {
	if (!kernel_aio_is_valid_id(id)) {
		return LibKernel::KERNEL_ERROR_ESRCH;
	}

	auto current = g_kernel_aio_state[id].load(std::memory_order_acquire);
	while (notify && (current == KERNEL_AIO_STATE_COMPLETED || current == KERNEL_AIO_STATE_ABORTED)) {
		if (g_kernel_aio_state[id].compare_exchange_weak(current, current | KERNEL_AIO_STATE_NOTIFIED,
		                                                std::memory_order_acq_rel)) {
			break;
		}
	}
	return current == 0 ? LibKernel::KERNEL_ERROR_ESRCH : current;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioPollRequest(int32_t id, int32_t* state) {
	PRINT_NAME();

	if (state == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	*state = kernel_aio_get_state(id, true);
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioPollRequests(int32_t* ids, int32_t num, int32_t* states) {
	if (ids == nullptr || states == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (num <= 0 || num > KERNEL_AIO_MAX_REQUESTS) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	for (int32_t i = 0; i < num; i++) {
		KernelAioPollRequest(ids[i], &states[i]);
	}
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioWaitRequests(int32_t* ids, int32_t num, int32_t* states, uint32_t mode,
                                        uint32_t* usec) {
	PRINT_NAME();

	if (ids == nullptr || states == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (num <= 0 || num > KERNEL_AIO_MAX_REQUESTS ||
	    (num != 1 && mode != KERNEL_AIO_WAIT_AND && mode != KERNEL_AIO_WAIT_OR)) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const auto collect_states = [&](bool notify) {
		int32_t finished = 0;
		bool newly_complete = false;
		for (int32_t i = 0; i < num; i++) {
			const auto current = kernel_aio_get_state(ids[i], notify);
			states[i] = current;
			finished += current != KERNEL_AIO_STATE_PROCESSING;
			newly_complete |= current == KERNEL_AIO_STATE_COMPLETED || current == KERNEL_AIO_STATE_ABORTED;
		}
		return finished == num || (mode == KERNEL_AIO_WAIT_OR && newly_complete);
	};
	const auto start = std::chrono::steady_clock::now();
	for (;;) {
		const bool complete = collect_states(false);
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		                         std::chrono::steady_clock::now() - start).count();
		const bool timed_out = usec != nullptr && elapsed >= *usec;
		if (complete || timed_out) {
			// A competing waiter can claim the newly completed request after the first scan.
			if (collect_states(true)) {
				if (usec != nullptr) {
					*usec = elapsed < *usec ? *usec - static_cast<uint32_t>(elapsed) : 0;
				}
				return OK;
			}
			if (timed_out) {
				return LibKernel::KERNEL_ERROR_ETIMEDOUT;
			}
		}
		Common::Thread::SleepMicro(10);
	}
}

int KYTY_SYSV_ABI KernelAioWaitRequest(int32_t id, int32_t* state, uint32_t* usec) {
	return KernelAioWaitRequests(&id, 1, state, KERNEL_AIO_WAIT_AND, usec);
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioDeleteRequest(int32_t id, int32_t* ret) {
	PRINT_NAME();

	if (ret == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}

	if (!kernel_aio_is_valid_id(id)) {
		*ret = LibKernel::KERNEL_ERROR_ESRCH;
		return OK;
	}

	auto current = g_kernel_aio_state[id].load(std::memory_order_acquire);
	for (;;) {
		const auto state = current & ~KERNEL_AIO_STATE_NOTIFIED;
		if (state != KERNEL_AIO_STATE_COMPLETED && state != KERNEL_AIO_STATE_ABORTED) {
			*ret = current == 0 ? LibKernel::KERNEL_ERROR_ESRCH : LibKernel::KERNEL_ERROR_EBUSY;
			break;
		}
		if (g_kernel_aio_state[id].compare_exchange_weak(current, 0, std::memory_order_acq_rel)) {
			*ret = OK;
			break;
		}
	}
	return OK;
}

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelAioDeleteRequests(int32_t* ids, int32_t num, int32_t* rets) {
	if (ids == nullptr || rets == nullptr) {
		return LibKernel::KERNEL_ERROR_EFAULT;
	}
	if (num <= 0 || num > KERNEL_AIO_MAX_REQUESTS) {
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	for (int32_t i = 0; i < num; i++) {
		KernelAioDeleteRequest(ids[i], &rets[i]);
	}
	return OK;
}

namespace FileSystem = LibKernel::FileSystem;
namespace Memory     = LibKernel::Memory;
namespace EventQueue = LibKernel::EventQueue;
namespace EventFlag  = LibKernel::EventFlag;
namespace Semaphore  = LibKernel::Semaphore;

namespace Fiber {

LIB_DEFINE(InitFiber_1) {
	LIB_FUNC("hVYD7Ou2pCQ", Fiber::FiberInitialize);
	LIB_FUNC("7+OJIpko9RY", Fiber::FiberInitializeInternal);
	LIB_FUNC("asjUJJ+aa8s", Fiber::FiberOptParamInitialize);
	LIB_FUNC("JeNX5F-NzQU", Fiber::FiberFinalize);
	LIB_FUNC("a0LLrZWac0M", Fiber::FiberRun);
	LIB_FUNC("PFT2S-tJ7Uk", Fiber::FiberSwitch);
	LIB_FUNC("p+zLIOg27zU", Fiber::FiberGetSelf);
	LIB_FUNC("B0ZX2hx9DMw", Fiber::FiberReturnToThread);
	LIB_FUNC("avfGJ94g36Q", Fiber::FiberRun);
	LIB_FUNC("ZqhZFuzKT6U", Fiber::FiberSwitch);
	LIB_FUNC("uq2Y5BFz0PE", Fiber::FiberGetInfo);
	LIB_FUNC("Lcqty+QNWFc", Fiber::FiberStartContextSizeCheck);
	LIB_FUNC("Kj4nXMpnM8Y", Fiber::FiberStopContextSizeCheck);
	LIB_FUNC("JzyT91ucGDc", Fiber::FiberRename);
	LIB_FUNC("0dy4JtMUcMQ", Fiber::FiberGetThreadFramePointerAddress);
}

} // namespace Fiber

namespace Coredump {

LIB_DEFINE(InitCoredump_1) {
	LIB_FUNC("8zLSfEfW5AU", Coredump::sceCoredumpRegisterCoredumpHandler);
	LIB_FUNC("fFkhOgztiCA", Coredump::sceCoredumpUnregisterCoredumpHandler);
}

} // namespace Coredump

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelDebugOutText(int dbg_channel, const char* text) {
	if (text != nullptr) {
		LOGF("ps5-vsh: sceKernelDebugOutText(%d): %s\n", dbg_channel, text);
		std::fputs(text, stdout);
		std::fflush(stdout);
	}
	return 0;
}

LIB_DEFINE(InitLibKernel_1_FS) {
	LIB_FUNC("MZb0GKT3mo8", ShellCoreFloor::SocketpairObserved);
	LIB_FUNC("PGhQHd-dzv8", FileSystem::KernelMmap);
	LIB_FUNC("1G3lF1Gg1k8", FileSystem::KernelOpen);
	LIB_FUNC("UK2Tl2DWUns", FileSystem::KernelClose);
	LIB_FUNC("8nY19bKoiZk", FileSystem::KernelFcntl);
	LIB_FUNC("MnY19bKoiZk", FileSystem::KernelFcntl);
	LIB_FUNC("9JYNqN6jAKI", KernelDebugOutText);
	LIB_FUNC("Cg4srZ6TKbU", FileSystem::KernelRead);
	LIB_FUNC("QqxBetgJH+g", FileSystem::KernelReadv);
	LIB_FUNC("4wSze92BhLI", FileSystem::KernelWrite);
	LIB_FUNC("kAt6VDbHmro", FileSystem::KernelWritev);
	LIB_FUNC("+r3rMFwItV4", FileSystem::KernelPread);
	LIB_FUNC("yTj62I7kw4s", FileSystem::KernelPreadv);
	LIB_FUNC("nKWi-N2HBV4", FileSystem::KernelPwrite);
	LIB_FUNC("mBd4AfLP+u8", FileSystem::KernelPwritev);
	LIB_FUNC("eV9wAD2riIA", FileSystem::KernelStat);
	LIB_FUNC("kBwCPsYX-m4", FileSystem::KernelFstat);
	LIB_FUNC("VW3TVZiM4-E", FileSystem::KernelFtruncate);
	LIB_FUNC("WlyEA-sLDf0", FileSystem::KernelTruncate);
	LIB_FUNC("AUXVxWeJU-A", FileSystem::KernelUnlink);
	LIB_FUNC("52NcYU9+lEo", FileSystem::KernelRename);
	LIB_FUNC("taRWhTJFTgE", FileSystem::KernelGetdirentries);
	LIB_FUNC("oib76F-12fk", FileSystem::KernelLseek);
	LIB_FUNC("j2AIqSqJP0w", FileSystem::KernelGetdents);
	LIB_FUNC("1-LFLmRFxxM", FileSystem::KernelMkdir);
	LIB_FUNC("naInUjYt3so", FileSystem::KernelRmdir);
	LIB_FUNC("uWyW3v98sU4", FileSystem::KernelCheckReachability);
}

LIB_DEFINE(InitLibKernel_1_Mem) {
	LIB_FUNC("mL8NDH86iQI", Memory::KernelMapNamedFlexibleMemory);
	LIB_FUNC("IWIBBdTHit4", Memory::KernelMapFlexibleMemory);
	LIB_FUNC("DGMG3JshrZU", Memory::KernelSetVirtualRangeName);
	LIB_FUNC("mkgXxsoxWHg", Memory::KernelClearVirtualRangeName);
	// 6xx
	LIB_FUNC("4h6F1LLbTiw", Memory::KernelMapFlexibleMemory);
	LIB_FUNC("cQke9UuBQOk", Memory::KernelMunmap);
	LIB_FUNC("pO96TwzOm5E", Memory::KernelGetDirectMemorySize);
	LIB_FUNC("C0f7TJcbfac", Memory::KernelAvailableDirectMemorySize);
	LIB_FUNC("tZ2yplY8MBY", Memory::KernelGetPageTableStats);
	LIB_FUNC("rTXw65xmLIA", Memory::KernelAllocateDirectMemory);
	LIB_FUNC("B+vc2AO2Zrc", Memory::KernelAllocateMainDirectMemory);
	LIB_FUNC("rVjRvHJ0X6c", Memory::KernelVirtualQuery);
	LIB_FUNC("yDBwVAolDgg", Memory::KernelIsStack);
	LIB_FUNC("7oxv3PPCumo", Memory::KernelReserveVirtualRange);
	LIB_FUNC("L-Q3LEjIbgA", Memory::KernelMapDirectMemory);
	LIB_FUNC("BQQniolj9tQ", Memory::KernelMapDirectMemory2);
	LIB_FUNC("NcaWUxfMNIQ", Memory::KernelMapNamedDirectMemory);
	LIB_FUNC("BohYr-F7-is", Memory::KernelSetPrtAperture);
	LIB_FUNC("L0v2Go5jOuM", Memory::KernelGetPrtAperture);
	LIB_FUNC("hwVSPCmp5tM", Memory::KernelCheckedReleaseDirectMemory);
	LIB_FUNC("MBuItvba6z8", Memory::KernelReleaseDirectMemory);
	LIB_FUNC("jh+8XiK4LeE", Memory::KernelIsAddressSanitizerEnabled);
	LIB_FUNC("WFcfL2lzido", Memory::KernelQueryMemoryProtection);
	LIB_FUNC("BHouLQzh0X0", Memory::KernelDirectMemoryQuery);
	LIB_FUNC("aNz11fnnzi4", Memory::KernelAvailableFlexibleMemorySize);
	LIB_FUNC("n1-v6FgU7MQ", Memory::KernelConfiguredFlexibleMemorySize);
	LIB_FUNC("vSMAm3cxYTY", Memory::KernelMprotect);
	LIB_FUNC("9bfdLIyuwCY", Memory::KernelMtypeprotect);
	LIB_FUNC("2SKEx6bSq-4", Memory::KernelBatchMap);
	LIB_FUNC("kBJzF8x4SyE", Memory::KernelBatchMap2);
	LIB_FUNC("pU-QydtGcGY", Memory::KernelMemoryPoolReserve);
	LIB_FUNC("qCSfqDILlns", Memory::KernelMemoryPoolExpand);
	LIB_FUNC("Vzl66WmfLvk", Memory::KernelMemoryPoolCommit);
	LIB_FUNC("LXo1tpFqJGs", Memory::KernelMemoryPoolDecommit);
	LIB_FUNC("YN878uKRBbE", Memory::KernelMemoryPoolBatch);
	LIB_FUNC("bvD+95Q6asU", Memory::KernelMemoryPoolGetBlockStats);
}

LIB_DEFINE(InitLibKernel_1_Equeue) {
	LIB_FUNC("R-tyYMpYaxY", EventQueue::KernelAddWriteEvent);
	LIB_FUNC("cBGTk8S92XM", EventQueue::KernelDeleteWriteEvent);
	LIB_FUNC("D0OdFMjp46I", EventQueue::KernelCreateEqueue);
	LIB_FUNC("jpFjmgAC5AE", EventQueue::KernelDeleteEqueue);
	LIB_FUNC("fzyMKs9kim0", EventQueue::KernelWaitEqueue);
	LIB_FUNC("vz+pg2zdopI", EventQueue::KernelGetEventUserData);
	LIB_FUNC("mJ7aghmgvfc", EventQueue::KernelGetEventId);
	LIB_FUNC("23CPPI1tyBY", EventQueue::KernelGetEventFilter);
	LIB_FUNC("kwGyyjohI50", EventQueue::KernelGetEventData);
	LIB_FUNC("Q0qr9AyqJSk", EventQueue::KernelGetEventFflags);
	LIB_FUNC("Uu-iDFC9aUc", EventQueue::KernelGetEventError);
	LIB_FUNC("4R6-OvI2cEA", EventQueue::KernelAddUserEvent);
	LIB_FUNC("WDszmSbWuDk", EventQueue::KernelAddUserEventEdge);
	LIB_FUNC("F6e0kwo4cnk", EventQueue::KernelTriggerUserEvent);
	LIB_FUNC("LJDwdSNTnDg", EventQueue::KernelDeleteUserEvent);
	LIB_FUNC("57ZK+ODEXWY", EventQueue::KernelAddTimerEventVsh);
	LIB_FUNC("R74tt43xP6k", EventQueue::KernelAddHRTimerEvent);
	LIB_FUNC("J+LF6LwObXU", EventQueue::KernelDeleteHRTimerEvent);
	LIB_FUNC("bBfz7kMF2Ho", EventQueue::KernelAddAmprEvent);
	LIB_FUNC("vuae5JPNt9A", EventQueue::KernelAddAmprSystemEvent);
	LIB_FUNC("bMmid3pfyjo", EventQueue::KernelDeleteAmprEvent);
	LIB_FUNC("Ij+ryuEClXQ", EventQueue::KernelDeleteAmprSystemEvent);
	LIB_FUNC("nh2IFMgKTv8", LibcInternal::SceKqueue); // kqueue -> kernel equeue
}

LIB_DEFINE(InitLibKernel_1_EventFlag) {
	LIB_FUNC("PZku4ZrXJqg", EventFlag::KernelCancelEventFlag);
	LIB_FUNC("7uhBFWRAS60", EventFlag::KernelClearEventFlag);
	if (LibKernel::SemaphoreProvenance::Enabled()) {
		LIB_FUNC("BpFoboUJoZU", LibKernel::SemaphoreProvenance::CreateEventObserved);
	} else {
		LIB_FUNC("BpFoboUJoZU", EventFlag::KernelCreateEventFlag);
	}
	LIB_FUNC("8mql9OcQnd4", EventFlag::KernelDeleteEventFlag);
	LIB_FUNC("9lvj5DjHZiA", EventFlag::KernelPollEventFlag);
	LIB_FUNC("JTvBflhYazQ", EventFlag::KernelWaitEventFlag);
	LIB_FUNC("IOnSvHzqu6A", EventFlag::KernelSetEventFlag);
}

LIB_DEFINE(InitLibKernel_1_Semaphore) {
	if (LibKernel::SemaphoreProvenance::Enabled()) {
		LIB_FUNC("188x57JYp0g", LibKernel::SemaphoreProvenance::CreateSema);
	} else {
		LIB_FUNC("188x57JYp0g", Semaphore::KernelCreateSema);
	}
	LIB_FUNC("R1Jvn8bSCW8", Semaphore::KernelDeleteSema);
	LIB_FUNC("MwhHNKdBXq8", Semaphore::KernelOpenSema);  // ps5-vsh
	LIB_FUNC("HBkUky2PZPA", Semaphore::KernelCloseSema); // ps5-vsh
	LIB_FUNC("Zxa0VhQVTsk", Semaphore::KernelWaitSema);
	if (LibKernel::SemaphoreProvenance::Enabled()) {
		LIB_FUNC("12wOHk8ywb0", LibKernel::SemaphoreProvenance::PollSema);
	} else {
		LIB_FUNC("12wOHk8ywb0", Semaphore::KernelPollSema);
	}
	LIB_FUNC("4czppHBiriw", Semaphore::KernelSignalSema);
	LIB_FUNC("4DM06U2BNEY", Semaphore::KernelCancelSema);
	LIB_FUNC("GEnUkDZoUwY", Semaphore::PthreadSemInit);
	LIB_FUNC("Vwc+L05e6oE", Semaphore::PthreadSemDestroy);
	LIB_FUNC("C36iRE0F5sE", Semaphore::PthreadSemWait);
	LIB_FUNC("H2a+IN9TP0E", Semaphore::PthreadSemTrywait);
	LIB_FUNC("fjN6NQHhK8k", Semaphore::PthreadSemTimedwait);
	LIB_FUNC("aishVAiFaYM", Semaphore::PthreadSemPost);
	LIB_FUNC("DjpBvGlaWbQ", Semaphore::PthreadSemGetvalue);
}

LIB_DEFINE(InitLibKernel_1_Pthread) {
	LIB_FUNC("cfjAjVTFG6A", LibKernel::PthreadSuspendUserContext);
	LIB_FUNC("YkGOXpJEtO8", LibKernel::PthreadGetUserContext);
	LIB_FUNC("QRdE7dBfNks", LibKernel::PthreadResumeUserContext);
	LIB_FUNC("ZMn3clnAGBA", LibKernel::PthreadSpinInit);
	LIB_FUNC("rCTGkBIHfPY", LibKernel::PthreadSpinTrylock);
	LIB_FUNC("LEfMMCT+SlM", LibKernel::PthreadSpinUnlock);
	LIB_FUNC("IJIggoPZExk", LibKernel::PthreadSpinDestroy);
	LIB_FUNC("pw+70ClLYlY", LibKernel::PthreadSpinLock);
	LIB_FUNC("+F7C-hdk7+E", LibKernel::SigEmptySet);
	LIB_FUNC("VkTAsrZDcJ0", LibKernel::SigFillSet);
	LIB_FUNC("JUimFtKe0Kc", LibKernel::SigAddSet);
	LIB_FUNC("Nd-u09VFSCA", LibKernel::SigDelSet);
	LIB_FUNC("aPcyptbOiZs", LibKernel::sigprocmask);
	LIB_FUNC("JnNl8Xr-z4Y", LibKernel::SigIsMember);
	LIB_FUNC("EZ8h70dtFLg", LibKernel::PsmDiagEZ8h);
	LIB_FUNC("-wUggz2S5yk", LibKernel::MonoDiag_wUgg);
	LIB_FUNC("Y1nEpkCieOY", LibKernel::MonoDiagY1nE);
	LIB_FUNC("9UK1vLZQft4", LibKernel::PthreadMutexLock);
	LIB_FUNC("tn3VlD0hG60", LibKernel::PthreadMutexUnlock);
	LIB_FUNC("2Of0f+3mhhE", LibKernel::PthreadMutexDestroy);
	LIB_FUNC("cmo1RIYva9o", LibKernel::PthreadMutexInit);
	LIB_FUNC("upoVrzMHFeE", LibKernel::PthreadMutexTrylock);
	LIB_FUNC("smWEktiyyG0", LibKernel::PthreadMutexattrDestroy);
	LIB_FUNC("F8bUHwAG284", LibKernel::PthreadMutexattrInit);
	LIB_FUNC("iMp8QpE+XO4", LibKernel::PthreadMutexattrSettype);
	LIB_FUNC("1FGvU0i9saQ", LibKernel::PthreadMutexattrSetprotocol);

	LIB_FUNC("aI+OeCz8xrQ", LibKernel::PthreadSelf);
	LIB_FUNC("EotR8a3ASf4", LibKernel::PthreadSelf);
	LIB_FUNC("6UgtwV+0zb4", LibKernel::PthreadCreate);
	LIB_FUNC("3PtV6p3QNX4", LibKernel::PthreadEqual);
	LIB_FUNC("onNY9Byn-W8", LibKernel::PthreadJoin);
	LIB_FUNC("4qGrR6eoP9Y", LibKernel::PthreadDetach);
	LIB_FUNC("3kg7rT0NQIs", LibKernel::PthreadExit);
	LIB_FUNC("qBDmpCyGssE", LibKernel::PthreadCancel);
	LIB_FUNC("OAmWq+OHSjw", LibKernel::PthreadSetcancelstate);
	LIB_FUNC("sCJd99Phct0", LibKernel::PthreadSetcanceltype);
	LIB_FUNC("How7B8Oet6k", LibKernel::PthreadGetname);
	LIB_FUNC("GBUY7ywdULE", LibKernel::PthreadRename);
	LIB_FUNC("bt3CTBKmGyI", LibKernel::PthreadSetaffinity);
	LIB_FUNC("rcrVFJsQWRY", LibKernel::PthreadGetaffinity);
	LIB_FUNC("1tKyG7RlMJo", LibKernel::PthreadGetprio);
	LIB_FUNC("W0Hpm2X0uPE", LibKernel::PthreadSetprio);
	LIB_FUNC("T72hz6ffq08", LibKernel::PthreadYield);
	LIB_FUNC("6XG4B33N09g", LibKernel::PthreadYield);
	LIB_FUNC("yH-uQW3LbX0", Posix::pthread_kill);
	LIB_FUNC("CBNtXOoef-E", Posix::sched_get_priority_max);
	LIB_FUNC("m0iS6jNsXds", Posix::sched_get_priority_min);
	LIB_FUNC("EI-5-jlq2dE", LibKernel::PthreadGetthreadid);
	// 13.60 K+14da0 (EI-5-jlq2dE) tail-jumps to K+83f0 (3eqs37G74-s).
	// Both return the current guest thread's 32-bit ID; never a host/module ID.
	LIB_FUNC("3eqs37G74-s", LibKernel::PthreadGetthreadid);
#ifdef _WIN32
	LIB_FUNC("Wl2o5hOVZdw", LibKernel::KernelBacktrace);
#endif
	LIB_FUNC("geDaqgH9lTg", LibKernel::PthreadKeyCreate);
	LIB_FUNC("PrdHuuDekhY", LibKernel::PthreadKeyDelete);
	LIB_FUNC("+BzXYkqYeLE", LibKernel::PthreadSetspecific);
	LIB_FUNC("eoht7mQOCmo", LibKernel::PthreadGetspecific);

	LIB_FUNC("62KCwEMmzcM", LibKernel::PthreadAttrDestroy);
	LIB_FUNC("x1X76arYMxU", LibKernel::PthreadAttrGet);
	LIB_FUNC("8+s5BzZjxSg", LibKernel::PthreadAttrGetaffinity);
	LIB_FUNC("nsYoNRywwNg", LibKernel::PthreadAttrInit);
	LIB_FUNC("JaRMy+QcpeU", LibKernel::PthreadAttrGetdetachstate);
	LIB_FUNC("lpMP8HhkBbg", LibKernel::PthreadAttrGetinheritsched);
	LIB_FUNC("-quPa4SEJUw", LibKernel::PthreadAttrGetstack);
	LIB_FUNC("Ru36fiTtJzA", LibKernel::PthreadAttrGetstackaddr);
	LIB_FUNC("-fA+7ZlGDQs", LibKernel::PthreadAttrGetstacksize);
	LIB_FUNC("txHtngJ+eyc", LibKernel::PthreadAttrGetguardsize);
	LIB_FUNC("9RnL-m0+diQ", LibKernel::PthreadAttrGetsolosched);
	LIB_FUNC("Bvn74vj6oLo", LibKernel::PthreadAttrSetstack);
	LIB_FUNC("F+yfmduIBB8", LibKernel::PthreadAttrSetstackaddr);
	LIB_FUNC("UTXzJbWhhTE", LibKernel::PthreadAttrSetstacksize);
	LIB_FUNC("2Q0z6rnBrTE", LibKernel::PthreadAttrSetstacksize);
	LIB_FUNC("-Wreprtu0Qs", LibKernel::PthreadAttrSetdetachstate);
	LIB_FUNC("El+cQ20DynU", LibKernel::PthreadAttrSetguardsize);
	LIB_FUNC("eXbUSpEaTsA", LibKernel::PthreadAttrSetinheritsched);
	LIB_FUNC("DzES9hQF4f4", LibKernel::PthreadAttrSetschedparam);
	LIB_FUNC("4+h9EzwKF4I", LibKernel::PthreadAttrSetschedpolicy);
	LIB_FUNC("Dk6FC-TI+7Q", LibKernel::PthreadAttrSetsolosched);
	LIB_FUNC("3qxgM4ezETA", LibKernel::PthreadAttrSetaffinity);
	LIB_FUNC("FXPWHNk8Of0", LibKernel::PthreadAttrGetschedparam);

	LIB_FUNC("6ULAa0fq4jA", LibKernel::PthreadRwlockInit);
	LIB_FUNC("BB+kb08Tl9A", LibKernel::PthreadRwlockDestroy);
	LIB_FUNC("Ox9i0c7L5w0", LibKernel::PthreadRwlockRdlock);
	LIB_FUNC("iGjsr1WAtI0", LibKernel::PthreadRwlockRdlock);
	LIB_FUNC("+L98PIbGttk", LibKernel::PthreadRwlockUnlock);
	LIB_FUNC("EgmLo6EWgso", LibKernel::PthreadRwlockUnlock);
	LIB_FUNC("mqdNorrB+gI", LibKernel::PthreadRwlockWrlock);
	LIB_FUNC("sIlRvQqsN2Y", LibKernel::PthreadRwlockWrlock);
	LIB_FUNC("bIHoZCTomsI", LibKernel::PthreadRwlockTrywrlock);
	LIB_FUNC("XD3mDeybCnk", LibKernel::PthreadRwlockTryrdlock);
	LIB_FUNC("iPtZRWICjrM", LibKernel::PthreadRwlockTimedrdlock);
	LIB_FUNC("adh--6nIqTk", LibKernel::PthreadRwlockTimedwrlock);
	LIB_FUNC("i2ifZ3fS2fo", LibKernel::PthreadRwlockattrDestroy);
	LIB_FUNC("yOfGg-I1ZII", LibKernel::PthreadRwlockattrInit);
	LIB_FUNC("qsdmgXjqSgk", LibKernel::PthreadRwlockattrDestroy);
	LIB_FUNC("xFebsA4YsFI", LibKernel::PthreadRwlockattrInit);
	LIB_FUNC("h-OifiouBd8", LibKernel::PthreadRwlockattrSettype);

	LIB_FUNC("2Tb92quprl0", LibKernel::PthreadCondInit);
	LIB_FUNC("g+PZd2hiacg", LibKernel::PthreadCondDestroy);
	LIB_FUNC("RXXqi4CtF8w", LibKernel::PthreadCondDestroy);
	LIB_FUNC("WKAXJ4XBPQ4", LibKernel::PthreadCondWait);
	LIB_FUNC("JGgj7Uvrl+A", LibKernel::PthreadCondBroadcast);
	LIB_FUNC("kDh-NfxgMtE", LibKernel::PthreadCondSignal);
	LIB_FUNC("2MOy+rUfuhQ", LibKernel::PthreadCondSignal);
	LIB_FUNC("o69RpYO-Mu0", LibKernel::PthreadCondSignalto);
	LIB_FUNC("BmMjYxmew1w", LibKernel::PthreadCondTimedwait);
	LIB_FUNC("27bAgiJmOh0", Posix::pthread_cond_timedwait);
	LIB_FUNC("K953PF5u6Pc", Posix::pthread_cond_reltimedwait_np);
	LIB_FUNC("m5-2bsNfv7s", LibKernel::PthreadCondattrInit);
	LIB_FUNC("mKoTx03HRWA", Posix::pthread_condattr_init);
	LIB_FUNC("waPcxYiR3WA", LibKernel::PthreadCondattrDestroy);
	LIB_FUNC("dJcuQVn6-Iw", Posix::pthread_condattr_destroy);
	LIB_FUNC("EjllaAqAPZo", Posix::pthread_condattr_setclock);

	LIB_FUNC("wRYVA5Zolso", LibKernel::KernelClockGetres);
	LIB_FUNC("QBi7HCK03hw", LibKernel::KernelClockGettime);
	LIB_FUNC("ejekcaNQNq0", LibKernel::KernelGettimeofday);
	LIB_FUNC("kOcnerypnQA", LibKernel::KernelGettimezone);
	LIB_FUNC("0NTHN1NKONI", LibKernel::KernelConvertLocaltimeToUtc);
	LIB_FUNC("-o5uEDpN+oY", LibKernel::KernelConvertUtcToLocaltime);
	LIB_FUNC("n88vx3C5nW8", Posix::gettimeofday);
	LIB_FUNC("-2IRUCO--PM", LibKernel::KernelReadTsc);
	LIB_FUNC("1j3S3n-tTW4", LibKernel::KernelGetTscFrequency);
	LIB_FUNC("4J2sUJmuHZQ", LibKernel::KernelGetProcessTime);
	LIB_FUNC("BNowx2l588E", LibKernel::KernelGetProcessTimeCounterFrequency);
	LIB_FUNC("fgxnMeTNUtY", LibKernel::KernelGetProcessTimeCounter);

	LIB_FUNC("7H0iTOciTLo", Posix::pthread_mutex_lock);
	LIB_FUNC("2Z+PpY6CaJg", Posix::pthread_mutex_unlock);
	LIB_FUNC("IafI2PxcPnQ", LibKernel::PthreadMutexTimedlock);
	LIB_FUNC("Io9+nTKXZtA", Posix::pthread_mutex_timedlock);
	LIB_FUNC("mkx2fVhNMsg", Posix::pthread_cond_broadcast);
	LIB_FUNC("Op8TBGY5KHg", Posix::pthread_cond_wait);
	LIB_FUNC("14bOACANTBo", Posix::pthread_once);
	LIB_FUNC("Z4QosVuAsA0", Posix::pthread_once);
	LIB_FUNC("Oy6IpwgtYOk", Posix::lseek);
	LIB_FUNC("ezv-RSBNKqI", Posix::pread);
	LIB_FUNC("C2kJ-byS5rM", Posix::pwrite);
	LIB_FUNC("9eMlfusH4sU", Posix::flock);
	LIB_FUNC("mqQMh1zPPT8", Posix::fstat);

	LIB_FUNC("z0dtnPxYgtg", chmod);
	LIB_FUNC("VAzswvTOCzI", FileSystem::KernelUnlink);
	LIB_FUNC("JGMio+21L4c", Posix::mkdir);
	LIB_FUNC("wuCroIGjt2g", LibKernel::open);
	LIB_FUNC("bY-PO6JhzhQ", FileSystem::KernelClose);
	LIB_FUNC("FN4gaPmuFV8", FileSystem::KernelWrite);
}

static void AddLibkernelUnityFunc(Loader::SymbolDatabase* s, const char* nid, uint64_t func,
                                  const std::string& dbg_name) {
	Loader::SymbolResolve sr {};
	sr.name                 = nid;
	sr.library              = "libkernel_unity";
	sr.library_version      = 1;
	sr.module               = "libkernel";
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = Loader::SymbolType::Func;
	s->Add(sr, func, dbg_name);
}

LIB_DEFINE(InitLibKernel_1) {
	LibKernel::SemaphoreProvenance::Initialize();
	LIB_FUNC("04AjkP0jO9U", Posix::UmtxOp);
	InitLibKernel_1_FS(s);
	InitLibKernel_1_Mem(s);
	InitLibKernel_1_Equeue(s);
	InitLibKernel_1_EventFlag(s);
	InitLibKernel_1_Semaphore(s);
	InitLibKernel_1_Pthread(s);
	LibKernelApr::InitLibKernel_1_Apr(s);
	Posix::InitLibKernel_1_Posix(s);
	LibKernelWriteThrottling::InitLibKernelWriteThrottling(s);

	LIB_OBJECT("f7uOxY9mM1U", &LibKernel::g_stack_chk_guard);
	LIB_OBJECT("djxxOmW6-aw", &LibKernel::g_progname);

	LIB_FUNC("1jfXLRVzisc", LibKernel::KernelUsleep);
	LIB_FUNC("QcteRwbsnV0", LibKernel::KernelUsleep);
	LIB_FUNC("QvsZxomvUHs", LibKernel::KernelNanosleep);
	LIB_FUNC("LwG8g3niqwA", LibKernel::KernelDlsym);
	LIB_FUNC("-ZR+hG7aDHw", LibKernel::KernelSleep);
	LIB_FUNC("6c3rCVE-fTU", LibKernel::open);
	LIB_FUNC("6xVpy0Fdq+I", LibKernel::sigprocmask);
	LIB_FUNC("KiJEPEWRyUY", LibKernel::Sigaction);
	LIB_FUNC("dYK287SsmDs", LibKernel::KernelMapFlexibleMemoryInternal2);
	LIB_FUNC("VADc3MNQ3cM", LibKernel::Signal);
	LIB_FUNC("6Z83sYWFlA8", LibKernel::exit);
	LIB_FUNC("8OnWXlgQlvo", LibKernel::KernelRtldThreadAtexitDecrement);
	LIB_FUNC("959qrazPIrg", LibKernel::KernelGetProcParam);
	LIB_FUNC("WB66evu8bsU", LibKernel::KernelGetCompiledSdkVersion);
	LIB_FUNC("tU5e3f9gSiU", LibKernel::KernelIsTrinityMode);
	LIB_FUNC("NH6xARDOVv8", LibKernel::KernelGetOperationMode);
	LIB_FUNC("fTx66l5iWIA", FileSystem::KernelFsync);
	LIB_FUNC("juWbTNM+8hw", Posix::fsync);
	LIB_FUNC("uvT2iYBBnkY", LibKernel::KernelSync);
	LIB_FUNC("HoLVWNanBBc", LibKernel::getpid);
	LIB_FUNC("9BcDykPmo1I", LibKernel::get_error_addr);
	LIB_FUNC("k+AXqu2-eBc", Posix::getpagesize);
	LIB_FUNC("bnZxYgAFeA0", LibKernel::KernelGetSanitizerNewReplaceExternal);
	LIB_FUNC("ca7v6Cxulzs", LibKernel::KernelSetGPO);
	LIB_FUNC("4oXYe9Xmk0Q", LibKernel::KernelGetGPI);
	LIB_FUNC("QtLhuYZf9jg", LibKernel::SceKernelGetBackupRestoreMode);
	LIB_FUNC("a9683+DNj84", LibKernel::SceKernelIsEcoChargeAvailable);
	LIB_FUNC("UtO0OHMCgmI", LibKernel::SceKernelIsDevelopmentMode);
	LIB_FUNC("PKg59uLpHNI", LibKernel::SceKernelIsAssistMode);
	LIB_FUNC("nQ5AHlyJghQ", LibKernel::SceKernelIsCronos);
	LIB_FUNC("D4yla3vx4tY", LibKernel::SceKernelError);
	LIB_FUNC("vhlQXKkQFe0", LibKernel::SceKernelEnablePthreadObjectCheck);
	LIB_FUNC("Z7NoR9m5SVo", LibKernel::SceKernelIccGetCpuInfoBit);
	LIB_FUNC("RP9ImTNDfP8", LibKernel::SceKernelIccSetCpuInfoBit);
	LIB_FUNC("EIZbVQs381s", LibKernel::SysKernelGetIntdevModeForRcmgr);
	LIB_FUNC("6jj29MbyzuI", LibKernel::SysKernelGetManufacturingMode);
	LIB_FUNC("ssmH9nMYO4o", LibKernel::SceKernelGetCpuUsage);
	LIB_FUNC("QuJYZ2KVGGQ", LibKernel::ShmOpen); // shm_open -> -1
	LIB_FUNC("1vDaenmJtyA", LibKernel::SceKernelOpenEventFlag);
	LIB_FUNC("s9-RaxukuzQ", LibKernel::EventFlag::KernelCloseEventFlag);
	LIB_FUNC("0vTn5IDMU9A", LibKernel::SceKernelGetMainSocId);
	LIB_FUNC("i-H8tE6wTqI", LibKernel::SceKernelGetDataTransferMode);
	LIB_FUNC("Yrwoq3bti3c", LibKernel::SceKernelIsPs4Process);
	LIB_FUNC("i3tB6CuvHb4", LibKernel::ScePthreadSetBesteffort);
	LIB_FUNC("mkawd0NA9ts", LibKernel::Sysconf);
	LIB_FUNC("fUJRLEbJOuQ", LibKernel::SceKernelGetProcessName);
	LIB_FUNC("wNeGsNes5ck", LibKernel::SceKernelIccNotifyBootStatus);
	LIB_FUNC("NtOA+0RApBg", LibKernel::SceKernelIccGetUSBPowerState);
	LIB_FUNC("dGFwUPT+BK8", LibKernel::SceKernelAddDynlibEvent);
	LIB_FUNC("DRuBt2pvICk", LibKernel::read);
	LIB_FUNC("AqBioC2vF3I", LibKernel::read);
	LIB_FUNC("f7KBOafysXo", LibKernel::KernelGetModuleInfoFromAddr);
	LIB_FUNC("RpQJJVKTiFM", LibKernel::KernelGetModuleInfoForUnwind);
	LIB_FUNC("crb5j7mkk1c", LibKernel::KernelIsSignalReturn); // _is_signal_return
	LIB_FUNC("Fjc4-n1+y2g", LibKernel::elf_phdr_match_addr);
	LIB_FUNC("FxVZqBAA7ks", LibKernel::write);
	LIB_FUNC("FJmglmTMdr4", LibKernel::getargv);
	LIB_FUNC("kbw4UHHSYy0", LibKernel::pthread_cxa_finalize);
	LIB_FUNC("lLMT9vJAck0", LibKernel::clock_gettime);
	LIB_FUNC("5TgME6AYty4", KernelAioDeleteRequest);
	LIB_FUNC("Ft3EtsZzAoY", KernelAioDeleteRequests);
	LIB_FUNC("HgX7+AORI58", KernelAioSubmitReadCommands);
	LIB_FUNC("2pOuoWoCxdk", KernelAioPollRequest);
	LIB_FUNC("o7O4z3jwKzo", KernelAioPollRequests);
	LIB_FUNC("KOF-oJbQVvc", KernelAioWaitRequest);
	LIB_FUNC("lgK+oIWkJyA", KernelAioWaitRequests);
	LIB_FUNC("XQ8C8y+de+E", KernelAioSubmitWriteCommands);
	LIB_FUNC("nu4a0-arQis", KernelAioInitializeParam);
	LIB_FUNC("il03nluKfMk", LibKernel::KernelRaiseException);
	LIB_FUNC("iKJMWrAumPE", LibKernel::getargc);
	LIB_FUNC("NNtFaKJbPt0", LibKernel::close);
	LIB_FUNC("OMDRKKAZ8I4", LibKernel::KernelDebugRaiseException);
	LIB_FUNC("Ou3iL1abvng", LibKernel::stack_chk_fail);
	LIB_FUNC("p5EcQeEeJAE", LibKernel::KernelRtldSetApplicationHeapAPI);
	LIB_FUNC("pB-yGZ2nQ9o", LibKernel::KernelSetThreadAtexitCount);
	LIB_FUNC("py6L8jiVAN8", LibKernel::KernelGetSanitizerMallocReplaceExternal);
	LIB_FUNC("Qhv5ARAoOEc", LibKernel::KernelRemoveExceptionHandler);
	LIB_FUNC("QKd0qM58Qes", LibKernel::KernelStopUnloadModule);
	LIB_FUNC("rNhWz+lvOMU", LibKernel::KernelSetThreadDtors);
	LIB_FUNC("Tz4RNUCBbGI", LibKernel::KernelRtldThreadAtexitIncrement);
	LIB_FUNC("vNe1w4diLCs", LibKernel::tls_get_addr);
	LIB_FUNC("vYU8P9Td2Zo", KernelAioInitializeImpl);
	LIB_FUNC("WhCc1w3EhSI", LibKernel::KernelSetThreadAtexitReport);
	LIB_FUNC("WkwEd3N7w0Y", LibKernel::KernelInstallExceptionHandler);
	LIB_FUNC("La9uyZv4Kvw", LibKernel::KernelGetAvailableCpumask);
	LIB_FUNC("g0VTBxfJyu0", LibKernel::KernelGetCurrentCpu);
	LIB_FUNC("wzvqT4UqKX8", LibKernel::KernelLoadStartModule);
	LIB_FUNC("Xjoosiw+XPI", LibKernel::KernelUuidCreate);
	LIB_FUNC("DLORcroUqbc", LibKernel::KernelGetOpenPsId);
	LIB_FUNC("zE-wXIZjLoM", LibKernel::KernelDebugRaiseExceptionOnReleaseMode);
	LIB_FUNC("DKWTYfo-jMY", LibKernel::KernelDebugRaiseExceptionWithContext);
	LIB_FUNC("Hc4CaR6JBL0", Posix::KernelSyncOnAddressWait);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || defined(__linux__)
	// These backends have permission-respecting, fault-contained budget copyin/out.
	LIB_FUNC("dFym5b41JLU", Posix::KernelSyncOnAddressWait8);
	LIB_FUNC("XgVs4jRehcY", Posix::KernelSyncOnAddressWait16);
#endif
	LIB_FUNC("3k6kx-zOOSQ", Memory::KernelMlock);
	LIB_FUNC("-YTW+qXc3CQ", LibKernel::KernelInternalMemoryGetModuleSegmentInfo);
	LIB_FUNC("AmJ0mn2l4lM", LibKernel::KernelGetLibkernelTextLocation);
	LIB_FUNC("xQIIfJ860sk", Memory::KernelMunlock);
	LIB_FUNC("B2n8aDorSH4", Posix::KernelSyncOnAddressWait32);
	LIB_FUNC("PZQhiiLXRFs", Posix::KernelSyncOnAddressWait64);
	LIB_FUNC("q2y-wDIVWZA", Posix::KernelSyncOnAddressWake);

	AddLibkernelUnityFunc(s, "Qhv5ARAoOEc",
	                      reinterpret_cast<uint64_t>(LibKernel::KernelRemoveExceptionHandler),
	                      "LibKernel::KernelRemoveExceptionHandler");
	AddLibkernelUnityFunc(s, "il03nluKfMk",
	                      reinterpret_cast<uint64_t>(LibKernel::KernelRaiseException),
	                      "LibKernel::KernelRaiseException");
	AddLibkernelUnityFunc(s, "WkwEd3N7w0Y",
	                      reinterpret_cast<uint64_t>(LibKernel::KernelInstallExceptionHandler),
	                      "LibKernel::KernelInstallExceptionHandler");
}

} // namespace Libs
