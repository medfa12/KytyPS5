#include "loader/runtimeLinker.h"
#include "loader/npAlternateStartup.h"
#include "loader/runtimeCrtExit.h"
#include "loader/runtimeChildClient.h"
#include "loader/applicationImportBindings.h"
#include "loader/applicationImportData.h"
#include "loader/kernelTextLocation.h"
#include "loader/runtimeProcessIdentity.h"
#include "loader/runtimeStartupBarrier.h"
#include "loader/runtimeAppDb.h"
#include "loader/runtimeNpTransport.h"
#include "libs/runtimeUserService.h"
#include "libs/npNativeTransport.h"
#include <fstream>
#include "libs/errno.h"
#include "libs/composite.h"
#include "loader/processMetadata.h"
#include "libs/semaphoreProvenance.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hostException.h"
#include "common/logging/log.h"
#include "common/platform/sysDbg.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/hostMemory.h"
#include "kernel/eventFlag.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/guestPrintf.h"
#include "libs/libs.h"
#include "loader/elf.h"
#include "loader/appLocalLibraries.h"
#include "loader/gamePatch.h"
#include "loader/guestDiagnosticStore.h"
#include "loader/nativeAllocatorFatal.h"
#include "loader/guestInstructionPatcher.h"
#include "loader/jit.h"
#include "loader/symbolDatabase.h"
#include "loader/x64InstructionEmulator.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <memory>
#include <string>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(_WIN32)
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#else
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <sys/uio.h>
#include <unistd.h>
#endif
#endif

namespace Libs::LibKernel {
void SetProgName(const std::string& name);
} // namespace Libs::LibKernel

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
namespace Libs { void InitLibKernel_1(Loader::SymbolDatabase*); }
namespace Libs::LibC { void InitLibC_1(Loader::SymbolDatabase*); }
namespace Libs::Sysmodule {
bool TestRuntimeGuestRegistry(Loader::RuntimeLinker*, Loader::Program*, Loader::Program*, const void*,
    std::atomic<bool>*, std::atomic<bool>*, std::atomic<unsigned>*, std::string*);
bool TestRuntimeGuestRegistryLatePublication(Loader::RuntimeLinker*, Loader::Program*, Loader::Program*, const void*, std::string*);
}
#endif

namespace Loader {

Program::Program() = default;

Program::~Program() = default;

static void FreeTlsBlock(ThreadLocalStorage::Block* block) {
	if (block == nullptr || block->ptr == nullptr) {
		return;
	}

	if (block->free_func != nullptr) {
		block->free_func(block->ptr);
	} else if (block->vm_alloc) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(reinterpret_cast<uint64_t>(block->ptr),
		                                                  block->alloc_size));
	} else {
		delete[] block->ptr;
	}

	block->ptr        = nullptr;
	block->free_func  = nullptr;
	block->vm_alloc   = false;
	block->alloc_size = 0;
}

static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
	return alignment != 0 ? (value + alignment - 1) & ~(alignment - 1) : value;
}

ThreadLocalStorage::~ThreadLocalStorage() {
	for (auto& [_, block]: tlss) {
		FreeTlsBlock(&block);
	}
}

using atexit_func_t = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void (*)();
using entry_func_t  = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void (*)(EntryParams* params, atexit_func_t atexit_func);
using module_ini_fini_func_t = __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int (*)(size_t args, const void* argp,
                                                     module_func_t func);

enum class BindType { Unknown, Local, Global, Weak };

struct RelocationInfo {
	bool        resolved   = false;
	bool        application_data = false; // Exact audited object, redirected to its owner's RW store.
	BindType    bind       = BindType::Unknown;
	SymbolType  type       = SymbolType::Unknown;
	uint64_t    value      = 0;
	uint64_t    vaddr      = 0;
	uint64_t    base_vaddr = 0;
	std::string name;
	std::string dbg_name;
	bool        bind_self = false;
};

struct StubbedImportRecord {
	uint32_t    index       = 0;
	uint64_t    patch_vaddr = 0;
	uint64_t    thunk_vaddr = 0;
	std::string name;
	SymbolType  type = SymbolType::Unknown;
	BindType    bind = BindType::Unknown;
	std::string program;
	uint32_t    resolve_tries = 0;
	bool        strict_native_dependency = false;
	// ps5-vsh: corruption sentinel (heap-UAF across threads surfaces as
	// baadf00d records crashing FindProgramByFileName). Verified on read;
	// a mismatch skips the call instead of faulting.
	uint64_t magic = 0x9d3c5a7e1f0b8421ull;
};

struct TraceRecord {
	std::string name;
	uint64_t    real_vaddr = 0;
	uint64_t    thunk_vaddr = 0;
};

static std::vector<TraceRecord> g_trace_records;
static std::vector<uint64_t>    g_trace_thunk_pages;
static uint64_t                 g_trace_thunk_offset = 0;
static constexpr uint64_t       TRACE_THUNK_PAGE_SIZE = 4096;
static constexpr uint64_t       TRACE_THUNK_SIZE      = 192;

struct TraceRingEntry {
	uint32_t id = 0;
	uint32_t pad = 0; // ps5-vsh chainSUI149: rsp%16 at record time (mod-16
	                  // parity). Misaligned guest stacks (JIT->jemspace
	                  // chains) fault our big-frame HLEs; the pad names
	                  // every import that arrives off-parity.
	uint64_t regs[6] = {};
	uint64_t ret     = 0; // ps5-vsh chainSUI159: guest return site
	                          // ([rsp] at thunk entry) for crash triage.
};

// Retain a bounded recent-call window: 64 KiB per traced thread, rather than
// 16 MiB. Hundreds of service threads otherwise spend GiB on diagnostics alone.
// Rings still retain exited-thread history for crash reporting.
static constexpr uint64_t TRACE_RING_BITS = 10;
static constexpr uint64_t TRACE_RING_SIZE = (1ull << TRACE_RING_BITS);
static_assert(sizeof(TraceRingEntry) * TRACE_RING_SIZE == 64 * 1024);

struct TraceRing {
	TraceRing() {
		entries = new TraceRingEntry[TRACE_RING_SIZE]();
	}
	~TraceRing() {
		delete[] entries;
	}
	TraceRingEntry* entries = nullptr;
	uint64_t        count   = 0;
};

static thread_local TraceRing* t_trace_ring = nullptr;
static std::mutex              g_trace_registry_mutex;
static std::vector<TraceRing*> g_trace_registry;

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI TraceRecordCall(uint64_t id, const uint64_t* block) {
	if (t_trace_ring == nullptr) {
		t_trace_ring = new TraceRing();
		std::lock_guard lock(g_trace_registry_mutex);
		g_trace_registry.push_back(t_trace_ring);
	}
	auto* ring = t_trace_ring;
	auto& slot = ring->entries[ring->count & (TRACE_RING_SIZE - 1)];
	ring->count++;
	slot.id = static_cast<uint32_t>(id);
#if defined(__x86_64__) || defined(_M_X64)
	{
		uint64_t rrsp = 0;
		asm volatile("movq %%rsp, %0" : "=r"(rrsp));
		slot.pad = static_cast<uint32_t>(rrsp & 15u);
	}
#else
	slot.pad = 0;
#endif
	slot.regs[0] = block[8];
	slot.regs[1] = block[7];
	slot.regs[2] = block[6];
	slot.regs[3] = block[5];
	slot.regs[4] = block[4];
	slot.regs[5] = block[3];
	slot.ret = reinterpret_cast<const uint64_t*>(block)[-1];
}

static uint64_t AllocateTraceThunk(uint64_t record_id, uint64_t real_vaddr) {
	if (g_trace_thunk_pages.empty() ||
	    g_trace_thunk_offset + TRACE_THUNK_SIZE > TRACE_THUNK_PAGE_SIZE) {
		auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, TRACE_THUNK_PAGE_SIZE, Common::VirtualMemory::Mode::ExecuteReadWrite,
		    "trace_thunk");
		EXIT_NOT_IMPLEMENTED(page == 0);
		g_trace_thunk_pages.push_back(page);
		g_trace_thunk_offset = 0;
	}
	auto* code = reinterpret_cast<uint8_t*>(g_trace_thunk_pages.back() + g_trace_thunk_offset);
	g_trace_thunk_offset += TRACE_THUNK_SIZE;

	std::vector<uint8_t> b;
	b.push_back(0x55);                            // push rbp
	b.insert(b.end(), {0x48, 0x89, 0xe5});        // mov rbp, rsp
	b.push_back(0x57);                            // push rdi
	b.push_back(0x56);                            // push rsi
	b.push_back(0x52);                            // push rdx
	b.push_back(0x51);                            // push rcx
	b.insert(b.end(), {0x41, 0x50});              // push r8
	b.insert(b.end(), {0x41, 0x51});              // push r9
	b.push_back(0x50);                            // push rax
	b.insert(b.end(), {0x41, 0x52});              // push r10
	b.insert(b.end(), {0x41, 0x53});              // push r11
	b.insert(b.end(), {0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00}); // sub rsp, 136
	auto movups = [&b](bool store, uint8_t reg, uint8_t disp) {
		if (reg >= 4) {
			b.push_back(0x44); // REX.R for xmm4-7
		}
		b.push_back(0x0f);
		b.push_back(store ? 0x11 : 0x10);
		b.push_back(static_cast<uint8_t>(0x44 + (reg % 4) * 0x08));
		b.push_back(0x24);
		b.push_back(disp);
	};
	for (uint8_t r = 0; r < 8; r++) {
		movups(true, r, static_cast<uint8_t>(r * 16));
	}
	b.insert(b.end(), {0x48, 0xbf});              // mov rdi, record_id
	for (int i = 0; i < 8; i++) {
		b.push_back(static_cast<uint8_t>((record_id >> (i * 8)) & 0xff));
	}
	b.insert(b.end(), {0x48, 0x8d, 0x75, 0xb8});  // lea rsi, [rbp-72]
	b.insert(b.end(), {0x48, 0x8b, 0x45, 0x08});  // mov rax, [rbp+8] (guest ret)
	b.insert(b.end(), {0x48, 0x89, 0x45, 0xb0});  // mov [rbp-80], rax
	const auto logger = reinterpret_cast<uint64_t>(&TraceRecordCall);
	b.insert(b.end(), {0x48, 0xb8});              // mov rax, logger
	for (int i = 0; i < 8; i++) {
		b.push_back(static_cast<uint8_t>((logger >> (i * 8)) & 0xff));
	}
	b.insert(b.end(), {0xff, 0xd0});              // call rax
	for (uint8_t r = 0; r < 8; r++) {
		movups(false, r, static_cast<uint8_t>(r * 16));
	}
	b.insert(b.end(), {0x48, 0x81, 0xc4, 0x88, 0x00, 0x00, 0x00}); // add rsp, 136
	b.insert(b.end(), {0x41, 0x5b});              // pop r11
	b.insert(b.end(), {0x41, 0x5a});              // pop r10
	b.push_back(0x58);                            // pop rax
	b.insert(b.end(), {0x41, 0x59});              // pop r9
	b.insert(b.end(), {0x41, 0x58});              // pop r8
	b.push_back(0x59);                            // pop rcx
	b.push_back(0x5a);                            // pop rdx
	b.push_back(0x5e);                            // pop rsi
	b.push_back(0x5f);                            // pop rdi
	b.push_back(0x5d);                            // pop rbp
	b.insert(b.end(), {0x48, 0xb8});              // mov rax, real
	for (int i = 0; i < 8; i++) {
		b.push_back(static_cast<uint8_t>((real_vaddr >> (i * 8)) & 0xff));
	}
	b.insert(b.end(), {0xff, 0xe0});              // jmp rax
	EXIT_IF(b.size() > TRACE_THUNK_SIZE);
	std::memcpy(code, b.data(), b.size());
	Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(code), b.size());
	return reinterpret_cast<uint64_t>(code);
}

uint64_t TraceWrapImport(uint64_t real_vaddr, const std::string& name) {
	if (name.find("AqBioC2vF3I") != std::string::npos) {
		return real_vaddr;
	}
	for (const auto& r: g_trace_records) {
		if (r.real_vaddr == real_vaddr && r.name == name) {
			return r.thunk_vaddr;
		}
	}
	TraceRecord rec {};
	rec.name        = name;
	rec.real_vaddr  = real_vaddr;
	rec.thunk_vaddr = AllocateTraceThunk(static_cast<uint64_t>(g_trace_records.size()), real_vaddr);
	g_trace_records.push_back(rec);
	return rec.thunk_vaddr;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestTraceImportTarget(uint64_t thunk, uint64_t target) {
	return std::any_of(g_trace_records.begin(), g_trace_records.end(), [=](const TraceRecord& record) {
		return record.thunk_vaddr == thunk && record.real_vaddr == target;
	});
}
#endif

void DumpTraceRings() {
	// Bound both requested output and available history. After wrap, older entries
	// no longer exist and must not be printed repeatedly as if they were retained.
	uint64_t tail_limit = 30000;
	if (const char* value = std::getenv("KYTY_TRACE_DUMP_TAIL"); value != nullptr) {
		char* end = nullptr;
		const auto requested = std::strtoull(value, &end, 10);
		if (end != value && *end == '\0' && requested > 0) {
			tail_limit = std::min<uint64_t>(tail_limit, requested);
		}
	}
	std::lock_guard lock(g_trace_registry_mutex);
	LOGF("--- Trace rings: %llu threads, tail limit %llu ---\n",
	     static_cast<unsigned long long>(g_trace_registry.size()),
	     static_cast<unsigned long long>(tail_limit));
	for (auto* ring: g_trace_registry) {
		if (ring == nullptr || ring->count == 0) {
			continue;
		}
		const uint64_t total = ring->count;
		const uint64_t shown = std::min({total, tail_limit, TRACE_RING_SIZE});
		// ps5-vsh chainSUI149: stack-parity census over the shown window.
		// Modal parity == healthy; anything else arrived misaligned.
		uint64_t parity_count[16] = {};
		for (uint64_t k = total - shown; k < total; k++) {
			parity_count[ring->entries[k & (TRACE_RING_SIZE - 1)].pad & 15u]++;
		}
		uint64_t parity_mode = 0;
		for (uint64_t p = 1; p < 16; p++) {
			if (parity_count[p] > parity_count[parity_mode]) {
				parity_mode = p;
			}
		}
		uint64_t misaligned = 0;
		for (uint64_t p = 0; p < 16; p++) {
			if (p != parity_mode) {
				misaligned += parity_count[p];
			}
		}
		LOGF("--- Trace parity: mode=%llu misaligned=%llu ---\n",
		     (unsigned long long)parity_mode, (unsigned long long)misaligned);
		if (misaligned != 0) {
			uint64_t printed = 0;
			for (uint64_t k = total - 1;
			     k >= total - shown && k < total && printed < 15; k--) {
				const auto& slot = ring->entries[k & (TRACE_RING_SIZE - 1)];
				if ((slot.pad & 15u) == parity_mode) {
					continue;
				}
				const char* name = "?";
				if (slot.id < g_trace_records.size()) {
					name = g_trace_records[slot.id].name.c_str();
				}
				LOGF("TRACE-MISALIGNED %s parity=%llu (%llx, %llx, %llx, %llx, %llx, %llx)\n",
				     name, (unsigned long long)(slot.pad & 15u),
				     (unsigned long long)slot.regs[0], (unsigned long long)slot.regs[1],
				     (unsigned long long)slot.regs[2], (unsigned long long)slot.regs[3],
				     (unsigned long long)slot.regs[4], (unsigned long long)slot.regs[5]);
				printed++;
			}
		}
		LOGF("--- Trace thread ring: %llu calls, last %llu ---\n",
		     static_cast<unsigned long long>(total), static_cast<unsigned long long>(shown));
		for (uint64_t k = total - shown; k < total; k++) {
			const auto& slot = ring->entries[k & (TRACE_RING_SIZE - 1)];
			const char* name = "?";
			if (slot.id < g_trace_records.size()) {
				name = g_trace_records[slot.id].name.c_str();
			}
			LOGF("TRACE %s(%llx, %llx, %llx, %llx, %llx, %llx) ret=%llx\n", name,
			     (unsigned long long)slot.regs[0], (unsigned long long)slot.regs[1],
			     (unsigned long long)slot.regs[2], (unsigned long long)slot.regs[3],
			     (unsigned long long)slot.regs[4], (unsigned long long)slot.regs[5],
			     (unsigned long long)slot.ret);
		}
	}
}

static std::vector<StubbedImportRecord> g_stubbed_imports;
// ps5-vsh: the records vector grows on late (runtime) module loads while
// guest threads concurrently call stubs; a push_back realloc dangles
// in-flight references (baadf00d crash in FindProgramByFileName). One lock
// guards registration, calls, and late-resolve patching.
static std::mutex g_stubbed_imports_mutex;
static std::atomic_uint32_t             g_unresolved_stub_call_log_count {0};
static std::vector<uint64_t>            g_unresolved_stub_thunk_pages;
static uint64_t                         g_unresolved_stub_thunk_offset = 0;
static constexpr uint64_t               UNRESOLVED_STUB_PAGE_SIZE      = 4096;

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t UnresolvedImportStub(uint64_t record_id);

static bool PatchGuestMemory64(uint64_t vaddr, uint64_t value) {
	auto* ptr     = reinterpret_cast<uint64_t*>(vaddr);
	bool  changed = (*ptr != value);
	std::memcpy(ptr, &value, sizeof(value));
	return changed;
}

static uint64_t AllocateUnresolvedImportThunk(uint64_t record_id) {
	constexpr uint64_t thunk_size = 34;

	if (g_unresolved_stub_thunk_pages.empty() ||
	    g_unresolved_stub_thunk_offset + thunk_size > UNRESOLVED_STUB_PAGE_SIZE) {
		auto page = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, UNRESOLVED_STUB_PAGE_SIZE, Common::VirtualMemory::Mode::ExecuteReadWrite,
		    "unresolved_import_thunk");
		EXIT_NOT_IMPLEMENTED(page == 0);
		g_unresolved_stub_thunk_pages.push_back(page);
		g_unresolved_stub_thunk_offset = 0;
	}

	auto* code = reinterpret_cast<uint8_t*>(g_unresolved_stub_thunk_pages.back() +
	                                        g_unresolved_stub_thunk_offset);
	g_unresolved_stub_thunk_offset += thunk_size;

	uint8_t bytes[thunk_size] = {
	    0x48, 0x83, 0xec, 0x08,               // sub rsp, 8
	    0x48, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0, // mov rdi, record_id
	    0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, UnresolvedImportStub
	    0xff, 0xd0,                           // call rax
	    0x48, 0x83, 0xc4, 0x08,               // add rsp, 8
	    0x0f, 0x57, 0xc0,                     // xorps xmm0, xmm0
	    0xc3,                                 // ret
	};
	const auto target = reinterpret_cast<uint64_t>(UnresolvedImportStub);
	std::memcpy(bytes + 6, &record_id, sizeof(record_id));
	std::memcpy(bytes + 16, &target, sizeof(target));
	std::memcpy(code, bytes, sizeof(bytes));
	Common::VirtualMemory::FlushInstructionCache(reinterpret_cast<uint64_t>(code), thunk_size);
	return reinterpret_cast<uint64_t>(code);
}

static uint64_t RegisterStubbedImport(uint32_t index, const Program* program,
                                      const RelocationInfo& ri) {
	std::lock_guard<std::mutex> lock(g_stubbed_imports_mutex);
	for (const auto& record: g_stubbed_imports) {
		if (record.patch_vaddr == ri.vaddr) {
			return record.thunk_vaddr;
		}
	}

	StubbedImportRecord record {};
	record.index       = index;
	record.patch_vaddr = ri.vaddr;
	record.name        = ri.name;
	record.type        = ri.type;
	record.bind        = ri.bind;
	record.program     = Common::PathToString(program->file_name);
	record.strict_native_dependency = program->native_ssl_smoke_dependency;
	g_stubbed_imports.push_back(record);
	const auto record_id                     = g_stubbed_imports.size() - 1;
	const auto thunk                         = AllocateUnresolvedImportThunk(record_id);
	g_stubbed_imports[record_id].thunk_vaddr = thunk;
	return thunk;
}

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI uint64_t UnresolvedImportStub(uint64_t record_id) {
	Common::DebugCounters::Add(Common::DebugCounters::Counter::UnresolvedImportCalls);
	const auto log_index = g_unresolved_stub_call_log_count.fetch_add(1);

	// ps5-vsh: snapshot the record under a SHORT lock; everything below
	// runs unlocked. Holding g_stubbed_imports_mutex across Resolve/Find
	// calls deadlocked against registration paths that take the loader
	// lock first (ABBA: run44 hung with 0 flips).
	struct Snapshot {
		std::string name;
		std::string program;
		uint64_t    patch_vaddr = 0;
		uint32_t    index       = 0;
		SymbolType  type        = SymbolType::Unknown;
		BindType    bind        = BindType::Unknown;
		bool        exists      = false;
		bool        magic_ok    = false;
		bool        do_resolve  = false;
		bool        strict_native_dependency = false;
	};
	Snapshot     snap;
	uint64_t     seen = 0;
	const bool   breadcrumb = log_index < 1024;
	{
		std::lock_guard<std::mutex> lock(g_stubbed_imports_mutex);
		static std::unordered_map<uint64_t, uint64_t> s_logged;
		if (record_id < g_stubbed_imports.size()) {
			auto& r           = g_stubbed_imports[record_id];
			snap.exists       = true;
			snap.name         = r.name;
			snap.program      = r.program;
			snap.patch_vaddr  = r.patch_vaddr;
			snap.index        = r.index;
			snap.type         = r.type;
			snap.bind         = r.bind;
			snap.strict_native_dependency = r.strict_native_dependency;
			snap.magic_ok     = (r.magic == 0x9d3c5a7e1f0b8421ull);
			seen              = ++s_logged[record_id];
			snap.do_resolve   = (r.resolve_tries < 5);
			if (snap.do_resolve) {
				r.resolve_tries++;
			}
		}
		// ps5-vsh: synchronous entry breadcrumb (fopen/fprintf/fclose, no
		// LOGF): identifies the exact record_id when the body below faults.
		// Per-record capped (the old unconditional write was an fopen
		// storm on hot-loop stubs).
		if (breadcrumb && seen <= 2) {
			FILE* bf = nullptr;
			fopen_s(&bf, "_TempData\\stub_ids.txt", "a");
			if (bf != nullptr) {
				std::fprintf(bf, "%llu\n", (unsigned long long)record_id);
				std::fclose(bf);
			}
		}
	}
	const bool loud = breadcrumb && seen <= 2;
	if (!snap.exists) {
		if (loud) {
			printf("Unresolved import stub called: <bad-record>\n");
			LOGF("Unresolved import stub called [%u]: record_id=%" PRIu64 " symbol=<bad-record>\n",
			     log_index, record_id);
		}
		return 0;
	}
	if (snap.strict_native_dependency) {
		EXIT("Native SSL reached an unsupported unresolved import: %s (%s); no late zero-return fallback\n",
		     snap.name.c_str(), snap.program.c_str());
	}
	if (!snap.magic_ok) {
		if (loud) {
			printf("Unresolved import stub called: <corrupt-record %llu>, skipping\n",
			       (unsigned long long)record_id);
			LOGF("Unresolved import stub called [%u]: record_id=%" PRIu64 " CORRUPT, skipped\n",
			     log_index, record_id);
		}
		return 0;
	}
	if (loud) {
		// ps5-vsh: log the guest caller so unknown NIDs can be
		// attributed to a function (caller_rva is eboot-relative).
		uint64_t caller_rva = 0;
		uint64_t mod_base   = 0;
		if (const auto* p = Common::Singleton<RuntimeLinker>::Instance()->FindProgramByFileName(
		        snap.program);
		    p != nullptr) {
			mod_base   = p->base_vaddr;
			caller_rva = reinterpret_cast<uint64_t>(_ReturnAddress()) - p->base_vaddr;
		}
		printf("Unresolved import stub called: %s (caller=0x%016" PRIx64 " got=0x%016" PRIx64
		       " base=0x%016" PRIx64 " rva=0x%08" PRIx64 ")\n",
		       snap.name.c_str(), reinterpret_cast<uint64_t>(_ReturnAddress()),
		       snap.patch_vaddr, mod_base, caller_rva);
		// The generated thunk has no saved-register block. Reading presumed
		// argument slots here can cross the guest stack's guard boundary.
		LOGF("Unresolved import stub called [%u]: patch_vaddr=0x%016" PRIx64
		     " jmprela_index=%" PRIu32 " symbol=%s type=%s bind=%s program=%s\n",
		     log_index, snap.patch_vaddr, snap.index, snap.name.c_str(),
		     magic_enum::enum_name(snap.type), magic_enum::enum_name(snap.bind),
		     snap.program.c_str());
	}
	// ps5-vsh: late-resolve always runs (never starved by call volume);
	// the GOT write is validated as guest memory as before.
	if (snap.do_resolve) {
		auto* rt = Common::Singleton<RuntimeLinker>::Instance();
		if (auto* p = rt->FindProgramByFileName(snap.program); p != nullptr) {
			SymbolRecord out {};
			bool         bind_self = false;
			rt->Resolve(snap.name, snap.type, p, &out, &bind_self);
			if (out.vaddr != 0) {
				if (rt->FindProgramByAddr(snap.patch_vaddr) != nullptr) {
					PatchGuestMemory64(snap.patch_vaddr, out.vaddr);
				}
				LOGF("Late import resolve: %s -> 0x%016" PRIx64 " (%s)\n",
				     snap.name.c_str(), out.vaddr, out.name.c_str());
			}
		}
	}
	return 0;
}

constexpr uint64_t SYSTEM_RESERVED  = 0x800000000u;
constexpr uint64_t CODE_BASE_INCR   = 0x010000000u;
constexpr uint64_t INVALID_OFFSET   = 0x040000000u;
constexpr uint64_t CODE_BASE_OFFSET = 0x100000000u;
constexpr uint64_t INVALID_MEMORY   = SYSTEM_RESERVED + INVALID_OFFSET;

static uint64_t g_desired_base_addr = SYSTEM_RESERVED + CODE_BASE_OFFSET;
static uint64_t g_invalid_memory    = 0;

static Program*              g_tls_main_program        = nullptr;
static thread_local Program* g_tls_cached_main_program = nullptr;
static thread_local uint8_t* g_tls_cached_main_tcb     = nullptr;
// ps5-vsh: multi-program (fake multi-process) support (voldemort parity):
// which loaded program THIS thread's guest code belongs to. Co-elf worker
// threads tag themselves on entry; unset falls back to the main program,
// keeping the single-program path identical.
static thread_local Program* g_tls_current_program = nullptr;
// ps5-vsh: co-hosted mains (--co-elf) each plant a TLS handler stub in
// their own mapping. TlsMainGetAddr routes by the stub return address
// (always inside the caller's SafeCall page). Entries are added at load
// time, before guest threads start; mains are never unloaded.
static std::vector<std::pair<uint64_t, Program*>> g_tls_handler_ranges;
static std::mutex g_tls_handler_ranges_mutex;

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void RunEntry(uint64_t addr, EntryParams* params, atexit_func_t atexit_func,
                                   void* stack_top) {
#if defined(__x86_64__) || defined(_M_X64)
	auto* func = reinterpret_cast<entry_func_t>(addr);

	if (stack_top != nullptr) {
		const auto aligned_stack_top =
		    reinterpret_cast<uintptr_t>(stack_top) & ~static_cast<uintptr_t>(0x0f);
		const auto guest_rsp = aligned_stack_top - 2u * sizeof(uintptr_t);
		const auto guest_rbp = guest_rsp;

		auto* guest_root_frame = reinterpret_cast<uintptr_t*>(guest_rbp);
		guest_root_frame[0]    = 0;
		guest_root_frame[1]    = 0;

#if defined(__APPLE__)
		// Clang on macOS can allocate plain "r" inputs to r12/r13, which the template
		// clobbers before consuming them. Pin the inputs to registers the SysV guest
		// preserves without changing register allocation on Windows or Linux.
		register entry_func_t func_reg asm("rbx")      = func;
		register uintptr_t    guest_rsp_reg asm("r14") = guest_rsp;
		register uintptr_t    guest_rbp_reg asm("r15") = guest_rbp;
#endif

#if defined(__APPLE__)
		asm volatile(
		    "pushq %%r12\n\t"
		    "pushq %%r13\n\t"
		    "movq %%rsp, %%r12\n\t"
		    "movq %%rbp, %%r13\n\t"
		    "movq %[guest_rsp], %%rsp\n\t"
		    "movq %[guest_rbp], %%rbp\n\t"
		    "callq *%[func]\n\t"
		    "movq %%r13, %%rbp\n\t"
		    "movq %%r12, %%rsp\n\t"
		    "popq %%r13\n\t"
		    "popq %%r12\n\t"
		    :
		    : [func] "r"(func_reg), "D"(params),
		      "S"(atexit_func), [guest_rsp] "r"(guest_rsp_reg), [guest_rbp] "r"(guest_rbp_reg)
		    : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
		      "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
		      "xmm13", "xmm14", "xmm15");
#elif KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Windows stack probes use the TEB stack limits during the guest stack switch.
		// bounds, which describe the host stack and are invalid while RSP is in guest memory.
		register entry_func_t func_reg asm("rbx")     = func;
		register uintptr_t    guest_rsp_reg asm("r8") = guest_rsp;
		register uintptr_t    guest_rbp_reg asm("r9") = guest_rbp;
		asm volatile("pushq %%r12\n\t"
		             "pushq %%r13\n\t"
		             "pushq %%r14\n\t"
		             "pushq %%r15\n\t"
		             "movq %%gs:0x08, %%r14\n\t"
		             "movq %%gs:0x10, %%r15\n\t"
		             "xorq %%rcx, %%rcx\n\t"
		             "movq %%rcx, %%gs:0x08\n\t"
		             "movq %%rcx, %%gs:0x10\n\t"
		             "movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             "movq %%r14, %%gs:0x08\n\t"
		             "movq %%r15, %%gs:0x10\n\t"
		             "popq %%r15\n\t"
		             "popq %%r14\n\t"
		             "popq %%r13\n\t"
		             "popq %%r12\n\t"
		             : [guest_rsp] "+r"(guest_rsp_reg), [guest_rbp] "+r"(guest_rbp_reg)
		             : [func] "r"(func_reg), "D"(params), "S"(atexit_func)
		             : "cc", "memory", "rax", "rcx", "rdx", "r10", "r11", "xmm0", "xmm1", "xmm2",
		               "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
		               "xmm12", "xmm13", "xmm14", "xmm15");
#else
		// Clobbers prevent inputs from being allocated to r12/r13.
		asm volatile("movq %%rsp, %%r12\n\t"
		             "movq %%rbp, %%r13\n\t"
		             "movq %[guest_rsp], %%rsp\n\t"
		             "movq %[guest_rbp], %%rbp\n\t"
		             "callq *%[func]\n\t"
		             "movq %%r13, %%rbp\n\t"
		             "movq %%r12, %%rsp\n\t"
		             :
		             : [func] "r"(func), "D"(params),
		               "S"(atexit_func), [guest_rsp] "r"(guest_rsp), [guest_rbp] "r"(guest_rbp)
		             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "r13",
		               "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
		               "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
		return;
	}

	uintptr_t guest_root_frame[2] = {};

#if defined(__APPLE__)
	register entry_func_t func_reg asm("rbx")      = func;
	register uintptr_t    guest_rbp_reg asm("r14") = reinterpret_cast<uintptr_t>(guest_root_frame);
#endif

#if defined(__APPLE__) || KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	asm volatile("pushq %%r12\n\t"
	             "pushq %%r13\n\t"
	             "movq %%rbp, %%r12\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%[func]\n\t"
	             "movq %%r12, %%rbp\n\t"
	             "popq %%r13\n\t"
	             "popq %%r12\n\t"
	             :
#if defined(__APPLE__)
	             : [func] "r"(func_reg), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_rbp_reg)
#else
	             : [func] "r"(func), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
#endif
	             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1",
	               "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
	               "xmm12", "xmm13", "xmm14", "xmm15");
#else
	// Keep inputs out of r12.
	asm volatile("movq %%rbp, %%r12\n\t"
	             "movq %[guest_rbp], %%rbp\n\t"
	             "callq *%[func]\n\t"
	             "movq %%r12, %%rbp\n\t"
	             :
	             : [func] "r"(func), "D"(params),
	               "S"(atexit_func), [guest_rbp] "r"(guest_root_frame)
	             : "cc", "memory", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "xmm0",
	               "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
	               "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#endif
#else
	(void)stack_top;
	reinterpret_cast<entry_func_t>(addr)(params, atexit_func);
#endif
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
struct MainEntryStackTestState {
	bool      called = false;
	uintptr_t rsp    = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t teb_stack_base  = UINTPTR_MAX;
	uintptr_t teb_stack_limit = UINTPTR_MAX;
#endif
};

static __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI void TestMainEntryStackCallback(EntryParams* params,
                                                     atexit_func_t /*atexit_func*/) {
	auto* state = reinterpret_cast<MainEntryStackTestState*>(const_cast<char*>(params->argv[0]));
	asm volatile("pushq %%r15\n\t"
	             "pushq %%r14\n\t"
	             "popq %%r14\n\t"
	             "popq %%r15\n\t"
	             :
	             :
	             : "memory");
	asm volatile("movq %%rsp, %0" : "=r"(state->rsp) : : "memory");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(state->teb_stack_base), "=r"(state->teb_stack_limit)
	             :
	             : "memory");
#endif
	state->called = true;
}

bool TestMainEntryUsesGuestStack() {
	constexpr uint64_t stack_size = 0x10000;
	const auto         stack_base = Libs::LibKernel::Memory::AllocateRuntimeMemory(
	    0, stack_size, Common::VirtualMemory::Mode::ReadWrite, "main_entry_stack_test");
	if (stack_base == 0) {
		return false;
	}

	MainEntryStackTestState state {};
	EntryParams             params {};
	params.argv[0] = reinterpret_cast<const char*>(&state);
	std::memset(reinterpret_cast<void*>(stack_base), 0xcd, stack_size);
	auto* root_frame = reinterpret_cast<const uintptr_t*>(stack_base + stack_size) - 2;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t original_teb_stack_base  = 0;
	uintptr_t original_teb_stack_limit = 0;
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(original_teb_stack_base), "=r"(original_teb_stack_limit)
	             :
	             : "memory");
#endif

	RunEntry(reinterpret_cast<uint64_t>(TestMainEntryStackCallback), &params, nullptr,
	         reinterpret_cast<void*>(stack_base + stack_size));

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uintptr_t restored_teb_stack_base  = 0;
	uintptr_t restored_teb_stack_limit = 0;
	asm volatile("movq %%gs:0x08, %0\n\t"
	             "movq %%gs:0x10, %1\n\t"
	             : "=r"(restored_teb_stack_base), "=r"(restored_teb_stack_limit)
	             :
	             : "memory");
	const bool teb_ok = state.teb_stack_base == 0 && state.teb_stack_limit == 0 &&
	                    restored_teb_stack_base == original_teb_stack_base &&
	                    restored_teb_stack_limit == original_teb_stack_limit;
#else
	constexpr bool teb_ok = true;
#endif

	const bool rsp_ok  = state.rsp >= stack_base && state.rsp < stack_base + stack_size;
	const bool root_ok = root_frame[0] == 0 && root_frame[1] == 0;
	const bool freed   = Libs::LibKernel::Memory::FreeGuestMemory(stack_base, stack_size);
	return state.called && rsp_ok && root_ok && teb_ok && freed;
}

bool TestModuleRelocationUsesWritableHostMapping() {
	constexpr uint64_t page_size = 0x4000;
	constexpr uint64_t value     = 0x4b59545950415443;
	const auto         base      = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0, page_size, Common::VirtualMemory::Mode::ReadWrite, "host_only_patch_test");
	if (base == 0) {
		return false;
	}
	Libs::LibKernel::Memory::SetProgramMemoryProtection(base, page_size,
	                                                    Common::VirtualMemory::Mode::Read);

	Libs::LibKernel::Memory::VirtualQueryInfo before {};
	Libs::LibKernel::Memory::VirtualQueryInfo after {};
	const bool                                before_ok =
	    Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(base), 0, &before,
	                                                sizeof(before)) == 0;
	const bool changed  = PatchGuestMemory64(base, value);
	const bool after_ok = Libs::LibKernel::Memory::KernelVirtualQuery(
	                          reinterpret_cast<const void*>(base), 0, &after, sizeof(after)) == 0;
	const bool value_ok = *reinterpret_cast<const uint64_t*>(base) == value;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION mbi {};
	const bool               host_mode_ok =
	    VirtualQuery(reinterpret_cast<const void*>(base), &mbi, sizeof(mbi)) != 0 &&
	    mbi.Protect == PAGE_READWRITE;
#else
	constexpr bool host_mode_ok = true;
#endif
	const bool freed = Libs::LibKernel::Memory::FreeGuestMemory(base, page_size);

	return before_ok && after_ok && changed && value_ok && host_mode_ok && freed &&
	       before.protection == after.protection;
}
#endif

static uint64_t GetAlignedSize(const Elf64_Phdr* p) {
	return (p->p_align != 0 ? (p->p_memsz + (p->p_align - 1)) & ~(p->p_align - 1) : p->p_memsz);
}

static void DbgDumpSymbols(const std::string& folder, Elf64_Sym* symbols, uint64_t size,
                           const char* names) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + "symbols.txt");

	for (auto* sym = symbols;
	     reinterpret_cast<uint8_t*>(sym) < reinterpret_cast<uint8_t*>(symbols) + size; sym++) {
		f.Printf("----\n");
		f.Printf("st_name = %" PRIu32 ", %s\n", sym->st_name, names + sym->st_name);
		f.Printf("st_info = 0x%02" PRIx8 "\n", sym->st_info);
		f.Printf("st_other = 0x%02" PRIx8 "\n", sym->st_other);
		f.Printf("st_shndx = 0x%04" PRIx16 "\n", sym->st_shndx);
		f.Printf("st_value = 0x%016" PRIx64 "\n", sym->st_value);
		f.Printf("st_size = %" PRIu64 "\n", sym->st_size);
	}

	f.Close();
}

static void DbgDumpRela(const std::string& folder, Elf64_Rela* records, uint64_t size,
                        const char* /*names*/, const char* file_name) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + file_name);

	for (auto* r = records;
	     reinterpret_cast<uint8_t*>(r) < reinterpret_cast<uint8_t*>(records) + size; r++) {
		f.Printf("----\n"
		         "r_offset = 0x%016" PRIx64 "\n"
		         "r_info = 0x%016" PRIx64 "\n"
		         "r_addend = %" PRId64 "\n",
		         r->r_offset, r->r_info, r->r_addend);
	}

	f.Close();
}

static Common::VirtualMemory::Mode GetMode(Elf64_Word flags) {
	switch (flags) {
		case PF_R: return Common::VirtualMemory::Mode::Read;
		case PF_W: return Common::VirtualMemory::Mode::Write;
		case PF_R | PF_W: return Common::VirtualMemory::Mode::ReadWrite;
		case PF_X: return Common::VirtualMemory::Mode::Execute;
		case PF_X | PF_R: return Common::VirtualMemory::Mode::ExecuteRead;
		case PF_X | PF_W: return Common::VirtualMemory::Mode::ExecuteWrite;
		case PF_X | PF_W | PF_R: return Common::VirtualMemory::Mode::ExecuteReadWrite;

		default: return Common::VirtualMemory::Mode::NoAccess;
	}
}

struct GuestStackFrame {
	GuestStackFrame* next;
	uintptr_t        return_address;
};

static bool IsReadableRange(uint64_t addr, uint64_t size);

static bool IsValidGuestCodeAddress(uintptr_t addr) {
	constexpr uintptr_t code_start         = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	constexpr uintptr_t USER_ARENA_START   = 0x1000000000ull;
	constexpr uintptr_t OVERFLOW_CODE_START = 0xe000000000ull;

	if (addr >= code_start && addr < std::min<uintptr_t>(g_desired_base_addr, USER_ARENA_START)) {
		return true;
	}
	if (g_desired_base_addr >= OVERFLOW_CODE_START && addr >= OVERFLOW_CODE_START && addr < g_desired_base_addr) {
		return true;
	}
	return false;
}

static int WalkGuestStack(uint64_t rbp, uint64_t rsp, void** stack, int capacity) {
	constexpr uintptr_t STACK_SIZE = 1024u * 1024u;
	const uintptr_t     code_start = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	const uintptr_t     stack_end  = rsp + STACK_SIZE;
	if (stack == nullptr || capacity <= 0 || rsp == 0 || rbp < rsp || stack_end < rsp ||
	    g_desired_base_addr <= code_start) {
		return 0;
	}

	auto* frame = reinterpret_cast<GuestStackFrame*>(rbp);

	int depth = 0;
	while (depth < capacity) {
		const auto frame_addr = reinterpret_cast<uintptr_t>(frame);
		if (frame_addr < rsp || frame_addr > stack_end - sizeof(GuestStackFrame) ||
		    !IsReadableRange(frame_addr, sizeof(GuestStackFrame)) ||
		    !IsValidGuestCodeAddress(frame->return_address)) {
			break;
		}
		stack[depth++] = reinterpret_cast<void*>(frame->return_address);
		if (reinterpret_cast<uintptr_t>(frame->next) <= frame_addr) {
			break;
		}
		frame = frame->next;
	}
	return depth;
}

// Probe diagnostic ranges without raising another fault.
static bool IsReadableRange(uint64_t addr, uint64_t size) {
	if (addr == 0 || size == 0) {
		return false;
	}

	const uint64_t end = addr + size;
	if (end < addr) {
		return false;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uint64_t current = addr;
	while (current < end) {
		MEMORY_BASIC_INFORMATION mbi {};
		if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) == 0 ||
		    mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
			return false;
		}
		const auto region_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (region_end <= current) {
			return false;
		}
		current = std::min(region_end, end);
	}
#elif defined(__APPLE__)
	// Walk the Mach regions covering the range and require read permission. The fatal
	// report dumps memory behind raw register values, and a fault inside the reporter
	// re-enters the signal handler and wedges the reporting thread.
	uint64_t current = addr;
	while (current < end) {
		mach_vm_address_t              region_addr = current;
		mach_vm_size_t                 region_size = 0;
		vm_region_basic_info_data_64_t info {};
		mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t                    object_name = MACH_PORT_NULL;
		if (mach_vm_region(mach_task_self(), &region_addr, &region_size, VM_REGION_BASIC_INFO_64,
		                   reinterpret_cast<vm_region_info_t>(&info), &count,
		                   &object_name) != KERN_SUCCESS ||
		    region_addr > current || (info.protection & VM_PROT_READ) == 0) {
			return false;
		}
		current = region_addr + region_size;
	}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	const auto page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
	if (page_size == 0) {
		return false;
	}

	for (uint64_t current = addr; current < end;) {
		uint8_t probe = 0;

		iovec local {&probe, sizeof(probe)};
		iovec remote {reinterpret_cast<void*>(current), sizeof(probe)};

		if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) !=
		    static_cast<ssize_t>(sizeof(probe))) {
			return false;
		}

		const uint64_t next = (current & ~(page_size - 1)) + page_size;
		if (next <= current) { // wrapped at the top of the address space
			break;
		}
		current = next;
	}
#else
	(void)end;
#endif
	return true;
}

// ps5-vsh chainSUI235/237: expected-ud2 + pending padding redirects.
// A survive covers TWO paddings: the abort-printer inner ud2 (0x1c13)
// plus the assert-site outer ud2 (e.g. 0x22db4e `call printer; ud2`).
// The setter arms an address match AND a small pending count; the VEH
// consumes one per redirect. Generic shape (ud2 right after a call in
// monosgen) covers unlisted outer sites. Genuine aborts (no survive)
// leave pending==0 and still die.
thread_local uint64_t t_expect_ud2 = 0;
thread_local unsigned t_ud2_pending = 0;
void Ps5VshExpectUd2Redirect(uint64_t ud2addr) { t_expect_ud2 = ud2addr; if (t_ud2_pending < 8) t_ud2_pending += 4; }

// Helper to check if an address was preceded by an x86-64 CALL instruction.
static bool IsCallInstruction(uint64_t ret_addr) {
	if (!IsReadableRange(ret_addr - 8, 9)) {
		return false;
	}
	const auto* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(ret_addr));

	// 5-byte: direct near call (E8 rel32)
	if (p[-5] == 0xE8u) {
		return true;
	}

	// 2-byte: FF /2 (call reg, call [reg]), or FF /4 (jmp reg)
	if (p[-2] == 0xFFu) {
		const uint8_t m = p[-1];
		if ((m & 0xF8u) == 0xD0u || (m & 0xF8u) == 0xE0u || ((m & 0xF8u) == 0x10u && m != 0x14u && m != 0x15u)) {
			return true;
		}
	}

	// 3-byte:
	// a) FF /2 disp8 (FF 50..57 disp8, except SIB 54)
	// b) FF 14 24 (call [rsp])
	// c) REX + FF /2 (call r8..r15 or call [r8..r15])
	if (p[-3] == 0xFFu) {
		const uint8_t m = p[-2];
		if (((m & 0xF8u) == 0x50u && m != 0x54u) || (m == 0x14u && p[-1] == 0x24u)) {
			return true;
		}
	} else if ((p[-3] & 0xF0u) == 0x40u && p[-2] == 0xFFu) {
		const uint8_t m = p[-1];
		if ((m & 0xF8u) == 0xD0u || (m & 0xF8u) == 0xE0u || ((m & 0xF8u) == 0x10u && m != 0x14u && m != 0x15u)) {
			return true;
		}
	}

	// 4-byte:
	// a) REX + FF 50..57 disp8
	// b) FF 54 SIB disp8 (call [rsp + disp8])
	if (p[-4] == 0xFFu && p[-3] == 0x54u) {
		return true;
	}
	if ((p[-4] & 0xF0u) == 0x40u && p[-3] == 0xFFu && (p[-2] & 0xF8u) == 0x50u) {
		return true;
	}

	// 6-byte:
	// a) FF 15 disp32 (call [rip + disp32]) or FF 25 disp32 (jmp [rip + disp32])
	// b) FF 90..97 disp32 (call [reg + disp32], except SIB 94)
	if (p[-6] == 0xFFu) {
		const uint8_t m = p[-5];
		if (m == 0x15u || m == 0x25u || ((m & 0xF8u) == 0x90u && m != 0x94u)) {
			return true;
		}
	}

	// 7-byte:
	// a) REX + FF 15/25/90..97 disp32
	// b) FF 94 SIB disp32 (call [rsp + disp32])
	if (p[-7] == 0xFFu && p[-6] == 0x94u) {
		return true;
	}
	if ((p[-7] & 0xF0u) == 0x40u && p[-6] == 0xFFu) {
		const uint8_t m = p[-5];
		if (m == 0x15u || m == 0x25u || ((m & 0xF8u) == 0x90u && m != 0x94u)) {
			return true;
		}
	}

	// 8-byte: REX + FF 94 SIB disp32
	if ((p[-8] & 0xF0u) == 0x40u && p[-7] == 0xFFu && p[-6] == 0x94u) {
		return true;
	}

	return false;
}

// ps5-vsh: env gates containing unverified guest-code patching.
// Load-time NOP patches default ON (ShellUI path) with opt-out via <name>=0;
// the fault-path nullcall skip defaults OFF and requires =1.
// Host env (std::getenv) is used here: the launcher controls it, and
// runtimeLinker.cpp already reads host env directly (e.g. KYTY_TRACE_DUMP_TAIL).
// (The guest-environment helper Config::GetGuestEnvironment, cf. libKernel.cpp
// TraceUserContext, reflects the guest process env instead.)
static bool Ps5VshLoadPatchOptedOut(const char* name) {
	const char* v = std::getenv(name);
	return v != nullptr && std::strcmp(v, "0") == 0;
}

static bool Ps5VshNullcallSkipEnabled() {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_VSH_NULLCALL_SKIP");
		return v != nullptr && std::strcmp(v, "1") == 0;
	}();
	return enabled;
}

static bool KytyExceptionHandler(const Common::HostException::ExceptionInfo& exception_info) {
	// Native allocator traps must reach the unhandled path regardless of how the
	// host classifies INT45. Do this before every generic recovery/guest signal path.
	if (NativeAllocatorFatal::Contains(exception_info.exception_address)) { return false; }
	const auto* info = &exception_info;
	// ps5-vsh chainSUI186: eboot map-lookup tap site/hits (function scope).
	static std::atomic<uint64_t> s_tapSite {0};
	static std::atomic<uint64_t> s_tapHits {0};
	// ps5-vsh chainSUI224: monosgen 0x35060 entry tap (vmovaps-wall birth
	// hunt). Logs entry rsp/rbp/parity/ret/args; auto-disarms after 24 hits.
	static std::atomic<uint64_t> s_tap2Site {0};
	static std::atomic<uint64_t> s_tap2Hits {0};
	static std::atomic<uint64_t> s_tap3Site {0};
	static std::atomic<uint64_t> s_tap3Hits {0};

	// Resolve GPU page protection before any loader lookup, including guest
	// compatibility probes. A GPU worker can fault while munmap holds the memory
	// operation lock and waits for it; a concurrent loader holds its own lock
	// while waiting for memory. Looking up the fault PC first closes that cycle.
	if (info->type == Common::HostException::ExceptionType::AccessViolation) {
		using CoreAccess = Common::HostException::AccessViolationType;
		using GpuAccess  = Libs::Graphics::PageFaultAccess;
		if (info->access_violation_type == CoreAccess::Read ||
		    info->access_violation_type == CoreAccess::Write) {
			const auto access = info->access_violation_type == CoreAccess::Write
			                        ? GpuAccess::Write : GpuAccess::Read;
			if (Libs::LibKernel::Memory::HandleGpuFault(access, info->access_violation_vaddr)) {
				Common::DebugCounters::Add(Common::DebugCounters::Counter::GpuFaults);
				return true;
			}
			if (const auto length = Libs::Graphics::ShaderRecompiler::IR::SrtWalkerLoadLength(info->exception_address)) {
				Loader::X64InstructionEmulator::SkipLoad(info->native_context, length);
				return true;
			}
		}
	}
	// Guarded copies must still decline genuine bad pointers after GPU handling.
	if (Libs::LibcInternal::Ps5VshInGuardedCopy()) {
		return false;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Windows has no guest FS base. Translate the observed nonzero TCB reads
	// without skipping the following stack-guard comparison/conditional branch.
	// Only an owning guest main can select its TCB; never use a host or global
	// main's FS/TLS for an arbitrary faulting library or co-hosted application.
	if (info->type == Common::HostException::ExceptionType::AccessViolation &&
	    info->access_violation_type == Common::HostException::AccessViolationType::Read &&
	    info->access_violation_vaddr == 0x28 && IsReadableRange(info->exception_address, 9)) {
		const std::span<const uint8_t> instruction(reinterpret_cast<const uint8_t*>(info->exception_address), 9);
		if (X64InstructionEmulator::IsFsTcbRead(instruction)) {
			auto* linker = Common::Singleton<RuntimeLinker>::Instance();
			auto* program = linker ? linker->FindProgramByAddr(info->exception_address) : nullptr;
			if (program && program->elf && !program->elf->IsShared() && program->tls.handler_vaddr) {
				auto* tcb = RuntimeLinker::TlsGetAddr(program) + program->tls.tcb_offset;
				if (X64InstructionEmulator::TryEmulateFsTcbRead(info->native_context, instruction, {tcb, 0x40})) {
					static std::atomic<unsigned> traces {0};
					if (traces.fetch_add(1, std::memory_order_relaxed) < 16)
						LOGF("Guest TLS: translated FS:[0x28] read at RVA %llx\n",
						     static_cast<unsigned long long>(info->exception_address - program->base_vaddr));
					return true;
				}
			}
		}
	}
#endif
	// ps5-vsh chainSUI239: null-call skip. Deterministic Finalizer death:
	// a guest indirect call lands at a near-null target (unresolved/zero
	// import: `call qword ptr [..]` with null GOT, or ret-to-0). [rsp] then
	// holds the post-call return address in guest code. Skipping the call
	// (rax=0, rip=ret, rsp+=8) is deterministic: the call produced no value.
	// Narrow: execute fault + target<64K + ret in a loaded program +
	// byte-verified call insn + cap. No guest-data writes.
	// ps5-vsh: gated behind KYTY_VSH_NULLCALL_SKIP=1 (default OFF); when off,
	// execution falls through to the unhandled path as if the blocks below
	// were absent.
	if (Ps5VshNullcallSkipEnabled() && info->type == Common::HostException::ExceptionType::AccessViolation &&
	    (info->access_violation_type == Common::HostException::AccessViolationType::Execute ||
	     info->access_violation_type == Common::HostException::AccessViolationType::Unknown) &&
	    info->access_violation_vaddr < 0x10000ull) {
		auto* cctx = reinterpret_cast<PCONTEXT>(info->native_context);
		if (cctx != nullptr && IsReadableRange(cctx->Rsp, 8)) {
			auto* linker = Common::Singleton<RuntimeLinker>::Instance();

			// 1. Primary: chainSUI239 null-call skip.
			// When guest code executes `call <target>` and target < 64KB (e.g. null function pointer
			// or uninitialized GOT entry), the CPU pushes the return address to [rsp].
			// If [rsp] is inside a loaded module and preceded by a call instruction, we resume
			// the caller right after the call instruction (rax = 0, rip = cret, rsp += 8).
			uint64_t cret = 0;
			std::memcpy(&cret, reinterpret_cast<const void*>(static_cast<uintptr_t>(cctx->Rsp)), sizeof(cret));
			auto* cprog = (linker != nullptr) ? linker->FindProgramByAddr(cret) : nullptr;
			if (cprog != nullptr && IsCallInstruction(cret)) {
				static std::atomic<uint64_t> s_nullcall {0};
				if (s_nullcall.fetch_add(1, std::memory_order_relaxed) < 4096) {
					cctx->Rax = 0;
					cctx->Rip = cret;
					cctx->Rsp = cctx->Rsp + 8;
					LOGF("ps5-vsh: nullcall skip tgt=%llx ret=%llx(%s+%llx)\n",
					     (unsigned long long)info->access_violation_vaddr,
					     (unsigned long long)cret,
					     Common::PathToString(cprog->file_name.filename()).c_str(),
					     (unsigned long long)(cret - cprog->base_vaddr));
					return true;
				}
			}

			// 2. Fallback: chainSUI240 ret/jmp-to-0 framepop.
			// Reached only when [rsp] was NOT preceded by a call (meaning [rsp] is stale data
			// because a ret popped a null return address, or jmp-to-0 occurred).
			// If the dead frame's RBP linkage is intact ([rbp] = saved RBP, [rbp+8] = caller's return address),
			// resume the caller as if the dead function returned 0, restoring both caller's RBP and RSP.
			if (cctx->Rbp >= 0x10000ull && IsReadableRange(cctx->Rbp, 16)) {
				uint64_t saved_rbp = 0;
				uint64_t fret = 0;
				std::memcpy(&saved_rbp, reinterpret_cast<const void*>(static_cast<uintptr_t>(cctx->Rbp)), sizeof(saved_rbp));
				std::memcpy(&fret, reinterpret_cast<const void*>(static_cast<uintptr_t>(cctx->Rbp + 8)), sizeof(fret));
				auto* fprog = (linker != nullptr) ? linker->FindProgramByAddr(fret) : nullptr;
				if (fprog != nullptr && IsCallInstruction(fret)) {
					static std::atomic<uint64_t> s_framepop {0};
					if (s_framepop.fetch_add(1, std::memory_order_relaxed) < 4096) {
						cctx->Rax = 0;
						cctx->Rip = fret;
						cctx->Rsp = cctx->Rbp + 16;
						cctx->Rbp = saved_rbp;
						LOGF("ps5-vsh: framepop tgt=%llx ret=%llx(%s+%llx)\n",
						     (unsigned long long)info->access_violation_vaddr,
						     (unsigned long long)fret,
						     Common::PathToString(fprog->file_name.filename()).c_str(),
						     (unsigned long long)(fret - fprog->base_vaddr));
						return true;
					}
				}
			}
		}
	}

	// ps5-vsh chainSUI206: SafeSprintf experiment reverted (see log.h);
	// no InSafeSprintf decline.

	// ps5-vsh: image-struct watchpoint. Log the accessor, re-arm, resume.
	if (info->native_code == 0x80000001u &&
	    info->type == Common::HostException::ExceptionType::AccessViolation) {
		const uint64_t page  = Libs::LibcInternal::Ps5VshImageGuardPage();
		const uint64_t page2 = Libs::LibcInternal::Ps5VshIniGuardPage();
		const uint64_t fault = info->access_violation_vaddr;
		const bool     hit =
		    (page != 0 && fault >= page && fault < page + 0x4000u) ||
		    (page2 != 0 && fault >= page2 && fault < page2 + 0x4000u);
		if (hit) {
			const bool is_ini = (page2 != 0 && fault >= page2 && fault < page2 + 0x4000u);
			// ps5-vsh: ini page: writer-hunt only (consumer reads are noise).
			if (is_ini &&
			    info->access_violation_type != Common::HostException::AccessViolationType::Write) {
				Libs::LibcInternal::Ps5VshRearmImageGuard();
				return true;
			}
			const uint64_t n = Libs::LibcInternal::Ps5VshNoteGuardHit();
			uint8_t        what = 9;
			switch (info->access_violation_type) {
				case Common::HostException::AccessViolationType::Read: what = 0; break;
				case Common::HostException::AccessViolationType::Write: what = 1; break;
				case Common::HostException::AccessViolationType::Execute: what = 8; break;
				default: break;
			}
			// ps5-vsh: lock-free record (flushed to the file log from HLE
			// context); keep 4 console lines for live triage.
			Libs::LibcInternal::Ps5VshRecordImageTouch(info->exception_address, fault, what);
			if (n < 4) {
				std::printf("ps5-vsh: image touch #%llu pc=0x%016llx addr=0x%016llx\n",
				            (unsigned long long)n, (unsigned long long)info->exception_address,
				            (unsigned long long)fault);
				std::fflush(stdout);
			}
			Libs::LibcInternal::Ps5VshRearmImageGuard();
			return true;
		}
	}

	// ps5-vsh: dense-heap absorption. Retail jemspace is dense; guest heap
	// writes that overrun our sparse frontier must grow the mapping, not
	// die. Auto-commit a faulting jemspace page when the faulting pc is
	// host code, then retry the instruction. Anything else falls through
	// to diagnostics + terminate below.
	if (info->type == Common::HostException::ExceptionType::AccessViolation &&
	    (info->access_violation_type == Common::HostException::AccessViolationType::Write ||
	     info->access_violation_type == Common::HostException::AccessViolationType::Read)) {
		const uint64_t fault = info->access_violation_vaddr;
		if (fault >= 0x200000000ull && fault < 0x210000000ull) {
			char        mod[MAX_PATH] = "?";
			HMODULE     hm            = nullptr;
			const bool  in_host =
			    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                       reinterpret_cast<LPCSTR>(info->exception_address), &hm) != 0 &&
			    hm == GetModuleHandleW(nullptr);
			if (in_host) {
				constexpr uint64_t kPage = 0x4000u;
				const uint64_t     page  = fault & ~(kPage - 1u);
				const uint64_t     got =
				    Libs::LibKernel::Memory::AllocateRuntimeMemory(
				        page, kPage, Common::VirtualMemory::Mode::ReadWrite,
				        "ps5-vsh-faultgrow", true);
				std::printf("ps5-vsh: faultgrow %llx -> %llx\n",
				            (unsigned long long)page, (unsigned long long)got);
				std::fflush(stdout);
				if (got == page) {
					return true;
				}
			}
		}
	}

	// ps5-vsh chainSUI179+200: slots-node index/count heal (0x16013d).
	// Mono's cache insert reads node[0] as a slot count, but head nodes
	// from reused arenas hold chunk-header pointers; the index register
	// goes stale-insane too. Fix both (index=0, count=0) and RETRY so the
	// insert completes normally into slot 0 (cache cold-start). Narrow:
	// exact fault rva + byte-verified store + insanity gates + cap.
	// Guest-data runtime fixup; fw files untouched.
	if (info->type == Common::HostException::ExceptionType::AccessViolation &&
	    info->access_violation_type ==
	        Common::HostException::AccessViolationType::Write) {
		auto* hlink = Common::Singleton<RuntimeLinker>::Instance();
		auto* hprog = (hlink != nullptr)
		    ? hlink->FindProgramByAddr(info->exception_address)
		    : nullptr;
		if (hprog != nullptr &&
		    Common::PathToString(hprog->file_name.filename()) ==
		        "libmonosgen-2.0.sprx" &&
		    info->exception_address - hprog->base_vaddr == 0x16013du &&
		    IsReadableRange(info->exception_address, 9) &&
		    IsReadableRange(info->rdx, 8)) {
			const auto* hb = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info->exception_address));
			uint64_t cnt = 0;
			std::memcpy(&cnt, reinterpret_cast<const void*>(
			    static_cast<uintptr_t>(info->rdx)), sizeof(cnt));
			const auto cnt32 = static_cast<int32_t>(cnt & 0xffffffffull);
			auto* hctx = reinterpret_cast<PCONTEXT>(info->native_context);
			const int64_t rvid = (hctx != nullptr)
			    ? static_cast<int64_t>(hctx->Rcx) : 0;
			const bool idxbad =
			    hctx != nullptr && (rvid < 0 || rvid > 124);
			const bool cntbad = (cnt32 < 0 || cnt32 > 124);
			static std::atomic<uint64_t> s_healed {0};
			if (hb[0] == 0x48u && hb[1] == 0xC7u && hb[2] == 0x44u &&
			    hb[3] == 0xCAu && hb[4] == 0x18u && (idxbad || cntbad) &&
			    s_healed.fetch_add(1, std::memory_order_relaxed) < 4096) {
				if (cntbad) {
					uint64_t zero = 0;
					std::memcpy(reinterpret_cast<void*>(
					    static_cast<uintptr_t>(info->rdx)), &zero, sizeof(zero));
				}
				if (idxbad) {
					hctx->Rcx = 0;
				}
				LOGF("ps5-vsh: insert-heal1 node=%llx cnt=%llx idxbad=%d\n",
				     (unsigned long long)info->rdx, (unsigned long long)cnt,
				     idxbad ? 1 : 0);
				return true;
			}
		}
	}

	// ps5-vsh chainSUI194+200: second insert site (0x15ed48, r14/rbx).
	// Same shape as 0x16013d with node=r14, index=rbx, entry=r13.
	// Fix index/count and RETRY so the insert completes into slot 0.
	if (info->type == Common::HostException::ExceptionType::AccessViolation &&
	    info->access_violation_type ==
	        Common::HostException::AccessViolationType::Write) {
		auto* hlink2 = Common::Singleton<RuntimeLinker>::Instance();
		auto* hprog2 = (hlink2 != nullptr)
		    ? hlink2->FindProgramByAddr(info->exception_address)
		    : nullptr;
		if (hprog2 != nullptr &&
		    Common::PathToString(hprog2->file_name.filename()) ==
		        "libmonosgen-2.0.sprx" &&
		    info->exception_address - hprog2->base_vaddr == 0x15ed48u &&
		    IsReadableRange(info->exception_address, 9) &&
		    IsReadableRange(info->r14, 8)) {
			const auto* hb2 = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info->exception_address));
			uint64_t cnt2 = 0;
			std::memcpy(&cnt2, reinterpret_cast<const void*>(
			    static_cast<uintptr_t>(info->r14)), sizeof(cnt2));
			const auto cnt322 = static_cast<int32_t>(cnt2 & 0xffffffffull);
			auto* hctx2 = reinterpret_cast<PCONTEXT>(info->native_context);
			const int64_t rvid2 = (hctx2 != nullptr)
			    ? static_cast<int64_t>(hctx2->Rbx) : 0;
			const bool idxbad2 =
			    hctx2 != nullptr && (rvid2 < 0 || rvid2 > 124);
			const bool cntbad2 = (cnt322 < 0 || cnt322 > 124);
			static std::atomic<uint64_t> s_healed2 {0};
			if (hb2[0] == 0x49u && hb2[1] == 0xC7u && hb2[2] == 0x44u &&
			    hb2[3] == 0xDEu && hb2[4] == 0x18u && (idxbad2 || cntbad2) &&
			    s_healed2.fetch_add(1, std::memory_order_relaxed) < 4096) {
				if (cntbad2) {
					uint64_t zero2 = 0;
					std::memcpy(reinterpret_cast<void*>(
					    static_cast<uintptr_t>(info->r14)), &zero2, sizeof(zero2));
				}
				if (idxbad2) {
					hctx2->Rbx = 0;
				}
				LOGF("ps5-vsh: insert-heal2 node=%llx cnt=%llx idxbad=%d\n",
				     (unsigned long long)info->r14, (unsigned long long)cnt2,
				     idxbad2 ? 1 : 0);
				return true;
			}
		}
	}

	// ps5-vsh chainSUI210: lookup-side heal (0xb4df, node=r14). Same stale
	// chunk-header-as-count disease as the insert sites, but on the hash
	// lookup read loop (`cmp [r13],0` walking slots at r14+0x18, bound from
	// [r14] low32). Fix count=0 and skip the inner loop to 0xb510
	// (verified: mov r15d,[rbp-0xa4] / cmp r14,[rbp-0xb8] next-node walk).
	// Zeroed count makes future visits take the jle path. Guest-data
	// runtime fixup; fw files untouched.
	if (info->type == Common::HostException::ExceptionType::AccessViolation &&
	    info->access_violation_type ==
	        Common::HostException::AccessViolationType::Read) {
		auto* hlink3 = Common::Singleton<RuntimeLinker>::Instance();
		auto* hprog3 = (hlink3 != nullptr)
		    ? hlink3->FindProgramByAddr(info->exception_address)
		    : nullptr;
		if (hprog3 != nullptr &&
		    Common::PathToString(hprog3->file_name.filename()) ==
		        "libmonosgen-2.0.sprx" &&
		    info->exception_address - hprog3->base_vaddr == 0xb4dfu &&
		    IsReadableRange(info->exception_address, 5) &&
		    IsReadableRange(info->r14, 8)) {
			const auto* hb3 = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info->exception_address));
			uint64_t cnt3 = 0;
			std::memcpy(&cnt3, reinterpret_cast<const void*>(
			    static_cast<uintptr_t>(info->r14)), sizeof(cnt3));
			const auto cnt323 = static_cast<int32_t>(cnt3 & 0xffffffffull);
			static std::atomic<uint64_t> s_healed3 {0};
			if (hb3[0] == 0x49u && hb3[1] == 0x83u && hb3[2] == 0x7Du &&
			    hb3[3] == 0x00u && hb3[4] == 0x00u &&
			    (cnt323 < 0 || cnt323 > 124) &&
			    !IsReadableRange(info->r13, 8) &&
			    s_healed3.fetch_add(1, std::memory_order_relaxed) < 4096) {
				uint64_t zero3 = 0;
				std::memcpy(reinterpret_cast<void*>(
				    static_cast<uintptr_t>(info->r14)), &zero3, sizeof(zero3));
				auto* hctx3 = reinterpret_cast<PCONTEXT>(info->native_context);
				bool skipped3 = false;
				if (hctx3 != nullptr) {
					const uint64_t guess3 = hprog3->base_vaddr + 0xb510u;
					if (IsReadableRange(guess3, 7)) {
						const auto* gb3 = reinterpret_cast<const uint8_t*>(
						    static_cast<uintptr_t>(guess3));
						if (gb3[0] == 0x44u && gb3[1] == 0x8Bu && gb3[2] == 0xBDu) {
							hctx3->Rip = guess3;
							skipped3 = true;
						}
					}
				}
				LOGF("ps5-vsh: lookup-heal node=%llx cnt=%llx skipped=%d\\n",
				     (unsigned long long)info->r14, (unsigned long long)cnt3,
				     skipped3 ? 1 : 0);
				return true;
			}
		}
	}

	// ps5-vsh chainSUI217: generic tested-bitmap heal (replaces the 0x1ec48e
	// site rule; same fix covers 0x1ea4cd and future siblings). Idiom:
	//   movzx r8d,[rax+2]; cmp [base+r8*4],0; je full-verify
	// The index hashes klass bytes>>16; stale/racing klass words give huge
	// indexes faulting OOB. The je path re-verifies thoroughly, so skipping
	// there is the conservative answer; if the word already changed (racing
	// init landed), rewind to the movzx instead. Matches by byte shape
	// (monosgen-only, index>0x4000 gate, je-target readability check, cap).
	// Guest-context runtime fixup; fw files untouched.
	if (info->type == Common::HostException::ExceptionType::AccessViolation &&
	    info->access_violation_type ==
	        Common::HostException::AccessViolationType::Read) {
		auto* hlink4 = Common::Singleton<RuntimeLinker>::Instance();
		auto* hprog4 = (hlink4 != nullptr)
		    ? hlink4->FindProgramByAddr(info->exception_address)
		    : nullptr;
		if (hprog4 != nullptr &&
		    Common::PathToString(hprog4->file_name.filename()) ==
		        "libmonosgen-2.0.sprx" &&
		    IsReadableRange(info->exception_address, 7)) {
			const auto* hb4 = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info->exception_address));
			const bool idx_is_r8 = (hb4[0] == 0x42u);
			const bool idx_is_r9 = (hb4[0] == 0x43u);
			static std::atomic<uint64_t> s_healed4 {0};
			if ((idx_is_r8 || idx_is_r9) && hb4[1] == 0x83u && hb4[2] == 0x3Cu &&
			    (hb4[3] == 0x87u || hb4[3] == 0x82u || hb4[3] == 0x81u) &&
			    hb4[4] == 0x00u && hb4[5] == 0x74u &&
			    s_healed4.fetch_add(1, std::memory_order_relaxed) < 4096) {
				const uint64_t idx = idx_is_r8 ? (info->r8 & 0xffffu)
				                                 : (info->r9 & 0xffffu);
				if (idx > 0x4000u) {
					const int8_t rel = static_cast<int8_t>(hb4[6]);
					const uint64_t jetarget = info->exception_address + 7 +
					    static_cast<uint64_t>(static_cast<int64_t>(rel));
					uint16_t now = 0;
					bool have = false;
					if (IsReadableRange(info->rax + 2, 2)) {
						std::memcpy(&now, reinterpret_cast<const void*>(
						    static_cast<uintptr_t>(info->rax + 2)), sizeof(now));
						have = true;
					}
					uint64_t target = 0;
					const char* how = "skip";
					if (have && now != static_cast<uint16_t>(idx) &&
					    IsReadableRange(info->exception_address - 5, 3)) {
						const auto* rb4 = reinterpret_cast<const uint8_t*>(
						    static_cast<uintptr_t>(info->exception_address - 5));
						if ((rb4[0] & 0xFBu) == 0x40u && rb4[1] == 0x0Fu &&
						    rb4[2] == 0xB7u) {
							target = info->exception_address - 5;
							how = "rewind";
						}
					}
					if (target == 0 && IsReadableRange(jetarget, 4)) {
						target = jetarget;
					}
					auto* hctx4 = reinterpret_cast<PCONTEXT>(info->native_context);
					bool ok4 = false;
					if (target != 0 && hctx4 != nullptr) {
						hctx4->Rip = target;
						ok4 = true;
					}
					LOGF("ps5-vsh: bitmap-heal %s rva=%llx idx=%llx now=%x done=%d\\n",
					     how,
					     (unsigned long long)(info->exception_address -
					                          hprog4->base_vaddr),
					     (unsigned long long)idx, have ? now : 0xffffu,
					     ok4 ? 1 : 0);
					if (ok4) {
						return true;
					}
				}
			}
		}
	}

	// ps5-vsh chainSUI221: null-log skip (0x5a148 mov rsi,[rbx+0x20],
	// 0x5a17d mov rsi,[rbx+0x30]). Log-args-only reads; with rbx==0 the
	// values log as null instead of faulting. Continues into the real
	// printf (null-safe in our formatter).
	if (info->type == Common::HostException::ExceptionType::AccessViolation &&
	    info->access_violation_type ==
	        Common::HostException::AccessViolationType::Read) {
		auto* llink = Common::Singleton<RuntimeLinker>::Instance();
		auto* lprog = (llink != nullptr)
		    ? llink->FindProgramByAddr(info->exception_address)
		    : nullptr;
		if (lprog != nullptr &&
		    Common::PathToString(lprog->file_name.filename()) ==
		        "libmonosgen-2.0.sprx" &&
		    (info->exception_address - lprog->base_vaddr == 0x5a148u ||
		     info->exception_address - lprog->base_vaddr == 0x5a17du) &&
		    info->rbx == 0 && IsReadableRange(info->exception_address, 4)) {
			const auto* lb = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info->exception_address));
			const uint8_t want = (info->exception_address - lprog->base_vaddr ==
			    0x5a148u) ? 0x20u : 0x30u;
			static std::atomic<uint64_t> s_logskip {0};
			if (lb[0] == 0x48u && lb[1] == 0x8Bu && lb[2] == 0x73u &&
			    lb[3] == want &&
			    s_logskip.fetch_add(1, std::memory_order_relaxed) < 4096) {
				auto* lctx = reinterpret_cast<PCONTEXT>(info->native_context);
				if (lctx != nullptr) {
					lctx->Rsi = 0;
					lctx->Rip = info->exception_address + 4;
					LOGF("ps5-vsh: null-log skip rva=%llx\\n",
					     (unsigned long long)(info->exception_address -
					                          lprog->base_vaddr));
					return true;
				}
			}
		}
	}

	// ps5-vsh chainSUI160: abort-padding redirect (0xebf6e wild-jump fix).
	// Our survivable-assert bypass returns from abort into Sony's
	// `call abort; ud2; int3...` padding at monosgen+0x1c13. Skipping
	// the ud2/int3 strands execution into a fall-through *call* of the
	// NEXT function (0x1c20) with no return address pushed; its epilogue
	// ret then pops stale stack garbage -> deterministic execute fault
	// at 0xebf6e (StrncpyS ret=0x960001c69 is the last trace). Redirect
	// straight back to the assert caller instead (leave;ret emulation
	// via the live rbp below). Validated per-hit (printer
	// asserts fire from many callers; a static code patch would bake in
	// the first S+5), capped; on any doubt fall through to instruction
	// handling (which now refuses UD2). Guest-memory runtime patch: none (context
	// only); fw files untouched.
	if (info->type == Common::HostException::ExceptionType::IllegalInstruction) {
		auto* alink = Common::Singleton<RuntimeLinker>::Instance();
		auto* aprog = (alink != nullptr)
		    ? alink->FindProgramByAddr(info->exception_address)
		    : nullptr;
		if (aprog != nullptr &&
		    Common::PathToString(aprog->file_name.filename()) ==
		        "libmonosgen-2.0.sprx" &&
		    (info->exception_address - aprog->base_vaddr == 0x1c13u ||
		     info->exception_address - aprog->base_vaddr == 0x15e796u ||
		     info->exception_address - aprog->base_vaddr == 0x54f72u ||
		     (t_expect_ud2 != 0 && t_expect_ud2 == info->exception_address) ||
		     (t_ud2_pending > 0 && IsReadableRange(info->exception_address - 5, 7) &&
		      (*(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(info->exception_address - 5))) == 0xE8u))) &&
		    IsReadableRange(info->exception_address, 2)) {
			const auto* ub = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info->exception_address));
			if (ub[0] == 0x0Fu && ub[1] == 0x0Bu) {
			// chainSUI161: leave;ret emulation. abort_path (0x1b70) owns a
			// real frame (push rbp; sub 0xd0); the thunk/HLEs preserve rbp,
			// so [rbp+8] is the assert caller (S+5). The old [rsp]-tier
			// read formatter leftovers (0xebf6e) and data pointers.
			// chainSUI162: nested-wrapper unwind. The assert caller is often
			// itself an abort wrapper (`call abort_path; ud2; int3...` at
			// 0x15f013/0x15f018): landing on its padding repeats the
			// fall-through disease one level up (spurious stack_chk_fail).
			// While the target is padding (ud2/int3 first byte), pop one more
			// frame. Real code never starts with ud2/int3.
			auto* uctx0 = reinterpret_cast<PCONTEXT>(info->native_context);
			uint64_t frbp = (uctx0 != nullptr) ? uctx0->Rbp : 0;
			uint64_t sret = 0;
			bool sret_ok = false;
			unsigned pops = 0;
			auto* rt2 = Common::Singleton<RuntimeLinker>::Instance();
			for (; pops < 16 && frbp != 0 && IsReadableRange(frbp + 8, 8); pops++) {
				uint64_t cand = 0;
				std::memcpy(&cand, reinterpret_cast<const void*>(
				    static_cast<uintptr_t>(frbp + 8)), sizeof(cand));
				if (cand == 0 || rt2 == nullptr ||
				    rt2->FindProgramByAddr(cand) == nullptr ||
				    !IsReadableRange(cand, 2)) {
					break;
				}
				const auto* cb = reinterpret_cast<const uint8_t*>(
				    static_cast<uintptr_t>(cand));
				const bool pad = (cb[0] == 0xCC) ||
				    (cb[0] == 0x0F && cb[1] == 0x0B);
				sret = cand;
				sret_ok = true;
				if (!pad) {
					break;
				}
				// Padding: step to the caller frame (its saved rbp).
				uint64_t next = 0;
				if (!IsReadableRange(frbp, 8)) {
					break;
				}
				std::memcpy(&next, reinterpret_cast<const void*>(
				    static_cast<uintptr_t>(frbp)), sizeof(next));
				if (next <= 0x10000ull || next == frbp) {
					break;
				}
				frbp = next;
				sret_ok = false;
			}
			uint64_t prbp = 0;
			bool prbp_ok = false;
			if (sret_ok && IsReadableRange(frbp, 8)) {
				std::memcpy(&prbp, reinterpret_cast<const void*>(
				    static_cast<uintptr_t>(frbp)), sizeof(prbp));
				prbp_ok = prbp != 0;
			}
			uint8_t after[8] = {};
			if (sret_ok && IsReadableRange(sret, 8)) {
				std::memcpy(after, reinterpret_cast<const void*>(
				    static_cast<uintptr_t>(sret)), sizeof(after));
			}
			static std::atomic<uint64_t> s_abredirect {0};
			const uint64_t rn =
			    s_abredirect.fetch_add(1, std::memory_order_relaxed);
			// chainSUI235: consume a fired expectation (stale matches nothing).
			if (t_ud2_pending > 0) { t_ud2_pending--; }
			if (t_expect_ud2 == info->exception_address) {
				t_expect_ud2 = 0;
			}
			LOGF("ps5-vsh: abort-redirect #%llu site=%llx pops=%u frbp=%llx prbp=%llx sret=%llx%s after=%02x%02x%02x%02x%02x%02x%02x%02x\n",
			     (unsigned long long)rn,
			     (unsigned long long)(info->exception_address - aprog->base_vaddr), pops,
			     (unsigned long long)frbp, (unsigned long long)prbp,
			     (unsigned long long)sret, sret_ok ? "(text)" : "",
			     after[0], after[1], after[2], after[3],
			     after[4], after[5], after[6], after[7]);
			// chainSUI163: restore the caller frame pointer too. sret is the
			// caller's post-call continuation, so rbp must be the caller's
			// rbp ([frbp]), not the dead wrapper frame (run162 died with a
			// stale rbp below rsp, reading dead locals -> NULL deref).
			// chainSUI170: deterministic rax. The wrapper never returns, so rax
			// holds abort leftovers (often 0 or heap-varied); resumed callers
			// deref it (run169: mov rcx,[rax] with rax=0). Point it at a
			// committed guest zero-scratch page: reads yield 0 so callers with
			// a null path limp on deterministically. Writes land in scratch.
			// chainSUI196: per-sret rax. Deref-first sites (0x3bdf3:
			// mov rcx,[rax]) need a MAPPED rax (scratch); null-check-first
			// sites (0x53825: test rax,rax) need NULL rax so the null path
			// is taken (scratch's nonzero address takes the wrong branch).
			// Default keeps scratch (proven for 0x15e46c/0x237ba1/0x54e0f).
			static std::atomic<uint64_t> s_zeroscratch {0};
			uint64_t scratch = s_zeroscratch.load(std::memory_order_acquire);
			if (scratch == 0) {
				const uint64_t got =
				    Libs::LibKernel::Memory::AllocateRuntimeMemory(
				        0, 0x4000, Common::VirtualMemory::Mode::ReadWrite,
				        "ps5-vsh-zeroscratch", false);
				if (got != 0) {
					std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(got)),
					             0, 0x4000);
					s_zeroscratch.store(got, std::memory_order_release);
					scratch = got;
				}
			}
			// chainSUI178: callee-saved recovery for sret=0x3bdf3. Resuming
			// 0x3bdc0 after skipping 0x3b4a0 loses rbx/r14 (0x3b4a0's body
			// clobbered them; its epilogue never ran). But 0x3b4a0's prologue
			// (push rbp/r15/r14/r13/r12/rbx) saved 0x3bdc0's exact values at
			// [frbp-8/-16/-24/-32/-40]. Restore them (narrow: this sret
			// only; other sites keep prior behavior).
			if (rn < 1024 && sret_ok && prbp_ok && uctx0 != nullptr) {
				uctx0->Rip = sret;
				uctx0->Rsp = frbp + 16;
				uctx0->Rbp = prbp;
				if (scratch != 0) {
					uctx0->Rax = scratch;
				}
				// chainSUI186: arm the eboot tap once (see handler above).
				// chainSUI224: arm the monosgen 0x35060 entry tap once too.
				if (s_tap2Site.load(std::memory_order_acquire) == 0 &&
				    s_tap2Hits.load(std::memory_order_acquire) == 0) {
					auto* trt2 = Common::Singleton<RuntimeLinker>::Instance();
					for (auto* prog : trt2->GetPrograms()) {
						if (prog == nullptr ||
						    Common::PathToString(prog->file_name.filename()) !=
						        "libmonosgen-2.0.sprx") {
							continue;
						}
						const uint64_t site2 = prog->base_vaddr + 0x35060u;
						if (IsReadableRange(site2, 1)) {
							auto* sb2 = reinterpret_cast<uint8_t*>(
							    static_cast<uintptr_t>(site2));
							if (sb2[0] == 0x55u) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
								DWORD old2 = 0;
								if (VirtualProtect(sb2, 1, PAGE_EXECUTE_READWRITE, &old2) != 0) {
									sb2[0] = 0xCC;
									VirtualProtect(sb2, 1, old2, &old2);
									FlushInstructionCache(GetCurrentProcess(), sb2, 1);
									s_tap2Site.store(site2, std::memory_order_release);
									LOGF("ps5-vsh: tap2 armed at monosgen+35060\n");
			if (s_tap3Site.load(std::memory_order_acquire) == 0 &&
			    s_tap3Hits.load(std::memory_order_relaxed) == 0) {
				for (auto* prog3 : trt2->GetPrograms()) {
					if (prog3 == nullptr ||
					    Common::PathToString(prog3->file_name.filename()) !=
					        "libmonosgen-2.0.sprx") {
						continue;
					}
					const uint64_t site3 = prog3->base_vaddr + 0x29510u;
					if (IsReadableRange(site3, 1)) {
						auto* sb3 = reinterpret_cast<uint8_t*>(
						    static_cast<uintptr_t>(site3));
						if (sb3[0] == 0x55u) {
						#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
						DWORD old3 = 0;
						if (VirtualProtect(sb3, 1, PAGE_EXECUTE_READWRITE, &old3) != 0) {
							sb3[0] = 0xCC;
							VirtualProtect(sb3, 1, old3, &old3);
							FlushInstructionCache(GetCurrentProcess(), sb3, 1);
							s_tap3Site.store(site3, std::memory_order_release);
							LOGF("ps5-vsh: tap3 armed at monosgen+29510\\n");
							// ps5-vsh chainSUI226: movaps->movups
							// for monosgen+0x35070 spills. The 0x35060 fn is
							// systematically entered misaligned (tap proof); its 8
							// xmm spill stores fault. movups is bit-identical when
							// aligned. Same length: patch the opcode byte only.
							int patched4 = 0;
							for (uint64_t rva = 0x35070u; rva < 0x350a1u; rva++) {
								const uint64_t va = prog3->base_vaddr + rva;
								if (!IsReadableRange(va, 6)) {
									continue;
								}
								auto* pb = reinterpret_cast<uint8_t*>(
								    static_cast<uintptr_t>(va));
								if (pb[0] != 0xC5u || pb[1] != 0xF8u ||
								    (pb[2] != 0x29u && pb[2] != 0x28u) ||
								    (pb[3] & 0xC0u) == 0xC0u) {
									continue;
								}
								DWORD old4 = 0;
								if (VirtualProtect(pb + 2, 1, PAGE_EXECUTE_READWRITE, &old4) != 0) {
									pb[2] = (pb[2] == 0x29u) ? 0x11u : 0x10u;
									VirtualProtect(pb + 2, 1, old4, &old4);
									patched4++;
								}
							}
							FlushInstructionCache(GetCurrentProcess(),
							    reinterpret_cast<LPCVOID>(prog3->base_vaddr + 0x35070u), 0x31);
							LOGF("ps5-vsh: movups patched %d insns at monosgen+35070\n",
							     patched4);
						}
						#endif
						}
					}
					break;
				}
			}
								}
#endif
							}
						}
						break;
					}
				}
				if (s_tapSite.load(std::memory_order_acquire) == 0) {
					auto* trt = Common::Singleton<RuntimeLinker>::Instance();
					for (auto* prog : trt->GetPrograms()) {
						if (prog == nullptr ||
						    Common::PathToString(prog->file_name.filename()) !=
						        "eboot.bin") {
							continue;
						}
						const uint64_t site = prog->base_vaddr + 0x4010f3u;
						if (IsReadableRange(site, 3)) {
							auto* sb = reinterpret_cast<uint8_t*>(
							    static_cast<uintptr_t>(site));
							if (sb[0] == 0x49u && sb[1] == 0x89u && sb[2] == 0xC7u) {
								#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
								DWORD old = 0;
								if (VirtualProtect(sb, 1, PAGE_EXECUTE_READWRITE, &old) != 0) {
									sb[0] = 0xCC;
									VirtualProtect(sb, 1, old, &old);
									FlushInstructionCache(GetCurrentProcess(), sb, 1);
									s_tapSite.store(site, std::memory_order_release);
									LOGF("ps5-vsh: tap armed at eboot+4010f3\n");
								}
								#endif
							}
						}
						break;
					}
				}
				auto* mab = Common::Singleton<RuntimeLinker>::Instance();
				auto* mprog = (mab != nullptr)
				    ? mab->FindProgramByAddr(sret)
				    : nullptr;
				const uint64_t srva =
				    (mprog != nullptr) ? sret - mprog->base_vaddr : 0;
				// chainSUI196+218: shape-aware rax. Deref-first srets need a
				// MAPPED rax (scratch); null-check-first srets need NULL rax
				// so the null path is taken (scratch's nonzero address takes
				// the wrong branch - chainSUI218: 0x53863 test eax,eax took a
				// spurious teardown call with garbage r12). Decode the sret
				// insn instead of allowlisting RVAs. Default keeps scratch.
				{
					bool nullshape = false;
					if (IsReadableRange(sret, 10)) {
						const auto* sb = reinterpret_cast<const uint8_t*>(
						    static_cast<uintptr_t>(sret));
						// chainSUI220: window scan. The null-check is often the SECOND
						// insn (0x53825: mov r14,rax first, then test rax,rax + je).
						// Scan 10 bytes for a null-test on rax/eax followed by a
						// conditional jump (without the branch the value may still
						// be consumed, so a bare test alone is not enough).
						for (int bi = 0; bi + 2 < 10 && !nullshape; bi++) {
							const bool is_test =
							    (sb[bi] == 0x48u && sb[bi + 1] == 0x85u && sb[bi + 2] == 0xC0u) ||
							    (sb[bi] == 0x85u && sb[bi + 1] == 0xC0u) ||
							    (sb[bi] == 0x48u && sb[bi + 1] == 0x83u && sb[bi + 2] == 0xF8u &&
							     bi + 3 < 10 && sb[bi + 3] == 0x00u) ||
							    (sb[bi] == 0x48u && sb[bi + 1] == 0x09u && sb[bi + 2] == 0xC0u);
							if (!is_test) {
								continue;
							}
							for (int bj = bi + 2; bj < 10; bj++) {
								if (sb[bj] == 0x74u || sb[bj] == 0x75u ||
								    (sb[bj] == 0x0Fu && bj + 1 < 10 &&
								     (sb[bj + 1] == 0x84u || sb[bj + 1] == 0x85u))) {
									nullshape = true;
									break;
								}
							}
						}
					}
					if (nullshape) {
						uctx0->Rax = 0;
					}
					LOGF("ps5-vsh: redirect-rax %s srva=%llx sret=%llx\\n",
					     nullshape ? "null" : "scratch",
					     (unsigned long long)srva, (unsigned long long)sret);
				}
				if (((srva == 0x3bdf3u) || (srva == 0x15e46cu) || (srva == 0x5449au) ||
				     (srva == 0x54e0fu) || (srva == 0x2ee2e4u) ||
				     (srva == 0x53825u &&
				      info->exception_address - aprog->base_vaddr == 0x54f72u)) &&
				    IsReadableRange(frbp - 40, 40)) {
					// chainSUI213: 0x3bdf3 slots are 0x3b4a0's prologue saves.
					// chainSUI219: 0x53825 re-enabled, gated on site 0x54f72:
					// that check (0x54520) pushes the same 5 regs, so the
					// slots are valid there too (pops=0 check frame). The
					// chainSUI213 garbage came from an already-poisoned guest
					// rbx (pthread word clobber), now fixed by word guards.
					uint64_t sv[5] = {};
					std::memcpy(sv, reinterpret_cast<const void*>(
					    static_cast<uintptr_t>(frbp - 40)), sizeof(sv));
					// slots: [frbp-40]=rbx [-32]=r12 [-24]=r13 [-16]=r14 [-8]=r15
					uctx0->Rbx = sv[0];
					uctx0->R12 = sv[1];
					uctx0->R13 = sv[2];
					uctx0->R14 = sv[3];
					uctx0->R15 = sv[4];
					LOGF("ps5-vsh: abort-redirect callee-restore rbx=%llx r14=%llx\n",
					     (unsigned long long)sv[0], (unsigned long long)sv[3]);
				} else if (IsReadableRange(frbp - 40, 40)) {
					// chainSUI242: selective de-poison. The aborted callee owned rbx/r12-r15
					// (callee-saved) and died holding garbage, often host-shaped (HLE
					// handouts leak into guest regs). The slots are this frame prologue
					// spills: valid caller values for standard-prologue frames. Narrow:
					// only a host-shaped reg is replaced, and only with a non-host slot
					// (live guest values are never touched).
					const auto hostshape = [](uint64_t v) {
						if (v >= 0x140000000ull && v < 0x143000000ull) {
							return true;
						}
						const uint64_t hi = v >> 32;
						return hi == 0x7FFull || hi >= 0xFF0ull;
					};
					uint64_t dv[5] = {};
					std::memcpy(dv, reinterpret_cast<const void*>(
					    static_cast<uintptr_t>(frbp - 40)), sizeof(dv));
					uint64_t* regs[5] = {&uctx0->Rbx, &uctx0->R12, &uctx0->R13,
					                     &uctx0->R14, &uctx0->R15};
					for (int vi = 0; vi < 5; vi++) {
						if (hostshape(*regs[vi]) && !hostshape(dv[vi])) {
							LOGF("ps5-vsh: de-poison r%llu %llx->%llx sret=%llx\n",
							     (unsigned long long)vi, (unsigned long long)*regs[vi],
							     (unsigned long long)dv[vi], (unsigned long long)srva);
							*regs[vi] = dv[vi];
						}
					}
				}
				return true;
			}
			}
		}
	}

	if (info->type == Common::HostException::ExceptionType::IllegalInstruction &&
	    Loader::X64InstructionEmulator::TryEmulate(info->native_context)) {
		Common::DebugCounters::Add(Common::DebugCounters::Counter::EmulatedInstructions);
		return true;
	}

	// Existing guest INT3 compatibility handling is separate from illegal-
	// instruction emulation. UD2 is not emulated and must remain a fatal boundary.
	// Guest-only:
	// FindProgramByAddr confines the skip to loaded program mappings (host
	// asserts/debugbreaks use int3 too and must still crash loudly).
	// ps5-vsh: layout breakpoint trap (chainSUI122, armed from libC).
	// A 0xCC over monosgen+0xBFD05 (`lea rdi,[rip+disp32]`, the layout
	// assert-prep) lands here with the full register file: log klass,
	// computed size and frame slots, then emulate the lea (keep armed).
	// ps5-vsh chainSUI186: eboot map-lookup result tap (bg -1 wall).
	// int3 armed at eboot+0x4010f3 (`mov r15,rax` after the lookup call).
	// Logs the lookup result; resumes by emulating the 3-byte mov.
	// Persistent (last hit before a crash is the interesting one).
	uint64_t tap2site = s_tap2Site.load(std::memory_order_acquire);
	uint64_t tap3site = s_tap3Site.load(std::memory_order_acquire);
	const bool is_tap2 = (tap2site != 0 && info->exception_address == tap2site);
	const bool is_tap3 = (tap3site != 0 && info->exception_address == tap3site);
	if (info->type == Common::HostException::ExceptionType::Breakpoint &&
	    (is_tap2 || is_tap3)) {
		auto* tctx2 = reinterpret_cast<PCONTEXT>(info->native_context);
		const uint64_t tn = (is_tap3 ? s_tap3Hits.fetch_add(1, std::memory_order_relaxed)
		                                : s_tap2Hits.fetch_add(1, std::memory_order_relaxed));
		if (tctx2 != nullptr && tn < 64) {
			uint64_t tret2 = 0, ta0 = 0, ta1 = 0;
			if (IsReadableRange(tctx2->Rsp, 24)) {
				std::memcpy(&tret2, reinterpret_cast<const void*>(
				    static_cast<uintptr_t>(tctx2->Rsp)), sizeof(tret2));
				std::memcpy(&ta0, reinterpret_cast<const void*>(
				    static_cast<uintptr_t>(tctx2->Rsp + 8)), sizeof(ta0));
				std::memcpy(&ta1, reinterpret_cast<const void*>(
				    static_cast<uintptr_t>(tctx2->Rsp + 16)), sizeof(ta1));
			}
			LOGF("ps5-vsh: %s #%llu rsp=%llx rsp16=%llu rbp=%llx "
			     "ret=%llx a0=%llx a1=%llx",
			     is_tap3 ? "tap3-29510" : "tap2-35060",
			     (unsigned long long)tn,
			     (unsigned long long)tctx2->Rsp,
			     (unsigned long long)(tctx2->Rsp & 15u),
			     (unsigned long long)tctx2->Rbp,
			     (unsigned long long)tret2,
			     (unsigned long long)ta0, (unsigned long long)ta1);
		}
		// Auto-disarm after 24 hits (restore push rbp, re-execute).
		// Before that, emulate the 1-byte push (no guest disarm races).
		if (tctx2 != nullptr && IsReadableRange(info->exception_address, 1)) {
			const auto* eb = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info->exception_address));
			if (eb[0] == 0xCCu && IsReadableRange(tctx2->Rsp - 8, 8)) {
				uint64_t rb = tctx2->Rbp;
				std::memcpy(reinterpret_cast<void*>(
				    static_cast<uintptr_t>(tctx2->Rsp - 8)), &rb, sizeof(rb));
				tctx2->Rsp -= 8;
				tctx2->Rip = info->exception_address + 1;
			}
		}
		const uint64_t tapcap = is_tap3 ? 24u : 64u;
		if (tn + 1 >= tapcap) {
			auto* tb2 = reinterpret_cast<uint8_t*>(
			    static_cast<uintptr_t>(is_tap3 ? tap3site : tap2site));
			#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			DWORD old2 = 0;
			if (VirtualProtect(tb2, 1, PAGE_EXECUTE_READWRITE, &old2) != 0) {
				tb2[0] = 0x55;
				VirtualProtect(tb2, 1, old2, &old2);
				FlushInstructionCache(GetCurrentProcess(), tb2, 1);
			}
			#endif
			if (is_tap3) {
				s_tap3Site.store(0, std::memory_order_release);
			} else {
				s_tap2Site.store(0, std::memory_order_release);
			}
		}
		return true;
	}

	if (info->type == Common::HostException::ExceptionType::Breakpoint &&
	    s_tapSite.load(std::memory_order_acquire) != 0 &&
	    info->exception_address == s_tapSite.load(std::memory_order_acquire)) {
		auto* tctx = reinterpret_cast<PCONTEXT>(info->native_context);
		if (tctx != nullptr && IsReadableRange(info->exception_address, 3)) {
			const auto* tb = reinterpret_cast<const uint8_t*>(
			    static_cast<uintptr_t>(info->exception_address));
			if (tb[0] == 0xCCu && tb[1] == 0x89u && tb[2] == 0xC7u) {
				const uint64_t n = s_tapHits.fetch_add(1, std::memory_order_relaxed);
				const int64_t res = static_cast<int64_t>(tctx->Rax);
				uint64_t tret = 0;
				if (IsReadableRange(tctx->Rsp, 8)) {
					std::memcpy(&tret, reinterpret_cast<const void*>(
					    static_cast<uintptr_t>(tctx->Rsp)), sizeof(tret));
				}
				if (res < 0x10000 || n < 3) {
					LOGF("ps5-vsh: tap-4010f3 #%llu rax=%llx rdi=%llx rsi=%llx r15=%llx ret=%llx\n",
					     (unsigned long long)n,
					     (unsigned long long)tctx->Rax, (unsigned long long)tctx->Rdi,
					     (unsigned long long)tctx->Rsi, (unsigned long long)tctx->R15,
					     (unsigned long long)tret);
				}
				// chainSUI188: substitute garbage results with the fake
				// streambuf (eboot mixes our fake-ios pointers into its map
				// machinery; lookups miss and return wild pointers).
				if (!IsReadableRange(tctx->Rax, 8)) {
					const uint64_t fb =
					    Libs::ShellCoreFloor::Ps5VshFakeBufAddr();
					if (fb != 0 && IsReadableRange(fb, 8)) {
						LOGF("ps5-vsh: tap-4010f3 substitute %llx -> fakebuf %llx\n",
						     (unsigned long long)tctx->Rax,
						     (unsigned long long)fb);
						tctx->Rax = fb;
					}
				}
				tctx->R15 = tctx->Rax;
				tctx->Rip = info->exception_address + 3;
				return true;
			}
		}
	}

	if (info->type == Common::HostException::ExceptionType::Breakpoint) {
		auto* linker = Common::Singleton<RuntimeLinker>::Instance();
		auto* prog = (linker != nullptr)
		    ? linker->FindProgramByAddr(info->exception_address)
		    : nullptr;
		if (prog != nullptr &&
		    Common::PathToString(prog->file_name.filename()) ==
		        "libmonosgen-2.0.sprx") {
			auto* ctx = reinterpret_cast<PCONTEXT>(info->native_context);
			if (ctx != nullptr && IsReadableRange(info->exception_address, 7)) {
				const auto* b = reinterpret_cast<const uint8_t*>(
				    static_cast<uintptr_t>(info->exception_address));
				const uint64_t bp_rva = info->exception_address - prog->base_vaddr;
				if (bp_rva == 0xBFD05u && b[0] == 0xCCu && b[1] == 0x8Du && b[2] == 0x3Du) {
				int32_t disp = 0;
				std::memcpy(&disp, b + 3, sizeof(disp));
				const uint64_t klass = ctx->Rax;
				uint32_t k1c = 0xFFFFFFFFu;
				if (IsReadableRange(klass + 0x1c, 4)) {
				    k1c = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(klass + 0x1c));
				}
				uint64_t snap = 0, f90 = 0, fc0 = 0;
				uint64_t k38 = 0;
				uint32_t k381c = 0xFFFFFFFFu;
				uint32_t cnt94 = 0xFFFFFFFFu, szb4 = 0xFFFFFFFFu;
				uint8_t fl98 = 0xFFu, ba = 0xFFu;
				const uint64_t rbp = ctx->Rbp;
				if (IsReadableRange(rbp - 0x30, 8)) {
				    snap = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(rbp - 0x30));
				}
				if (IsReadableRange(rbp - 0x90, 8)) {
				    f90 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(rbp - 0x90));
				}
				if (IsReadableRange(klass + 0x38, 8)) {
				    k38 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(klass + 0x38));
				    if (k38 >= 0x10000ull && IsReadableRange(k38 + 0x1c, 4)) {
				        k381c = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(k38 + 0x1c));
				    }
				}
				if (IsReadableRange(rbp - 0x94, 4)) {
				    cnt94 = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(rbp - 0x94));
				}
				if (IsReadableRange(rbp - 0xb4, 4)) {
				    szb4 = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(rbp - 0xb4));
				}
				if (IsReadableRange(rbp - 0xc0, 8)) {
				    fc0 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(rbp - 0xc0));
				}
				if (IsReadableRange(rbp - 0x98, 1)) {
				    fl98 = *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(rbp - 0x98));
				}
				if (IsReadableRange(klass + 0xba, 1)) {
				    ba = *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(klass + 0xba));
				}
				LOGF("ps5-vsh: layout-bp klass=0x%016llx r13=0x%016llx r13d=%u k1c=%u ba=0x%02x\n",
				     (unsigned long long)klass, (unsigned long long)ctx->R13,
				     static_cast<unsigned>(ctx->R13 & 0xffffffffull), k1c, ba);
				LOGF("ps5-vsh: layout-bp r14=0x%016llx r12d=%u rbp=0x%016llx snap=%llx f90=%llx cnt94=%u szb4=%u fc0=%llx fl98=0x%02x k38=%llx k381c=0x%x\n",
				     (unsigned long long)ctx->R14, static_cast<unsigned>(ctx->R12 & 0xffffffffull),
				     (unsigned long long)rbp, (unsigned long long)snap, (unsigned long long)f90,
				     cnt94, szb4, (unsigned long long)fc0, fl98,
				     (unsigned long long)k38, k381c);
				Log::Flush();
				ctx->Rdi = info->exception_address + 7 + static_cast<int64_t>(disp);
				ctx->Rip = info->exception_address + 7;
				return true;
				}
				// ps5-vsh: layout-cmp trap (chainSUI124, 0xBF3D0). The check
				// compares k1c against STALE r13d (klass-low32) for ba not in
				// {0x13,0x1e}. Force pass iff r13 holds the klass pointer
				// (stale pattern: a real size never equals a heap address).
				if (bp_rva == 0xBF3D0u && b[0] == 0xCCu && b[1] == 0x39u && b[2] == 0x68u &&
				    b[3] == 0x1Cu) {
				    const uint64_t cklass = ctx->Rax;
				    const uint64_t cr13 = ctx->R13;
				    uint32_t ck1c = 0xFFFFFFFFu;
				    uint64_t kb0 = 0;
				    uint32_t kb0f = 0xFFFFFFFFu;
				    if (IsReadableRange(cklass + 0xb0, 8)) {
				        kb0 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(cklass + 0xb0));
				        if (kb0 >= 0x10000ull && IsReadableRange(kb0 + 8, 4)) {
				            kb0f = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(kb0 + 8));
				        }
				    }
				    if (IsReadableRange(cklass + 0x1c, 4)) {
				        ck1c = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(cklass + 0x1c));
				    }
				    char kname[24] = {};
				    if (IsReadableRange(cklass + 0x40, 8)) {
				        const uint64_t knp = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(cklass + 0x40));
				        if (knp >= 0x10000ull && IsReadableRange(knp, 8)) {
				            int nlen = 0;
				            for (int c = 0; c < 23; c++) {
				                const char ch = *reinterpret_cast<const char*>(static_cast<uintptr_t>(knp) + c);
				                if (ch == '\0') { break; }
				                if (ch < 0x20 || ch >= 0x7f) { nlen = 0; break; }
				                kname[nlen++] = ch;
				            }
				            kname[nlen] = '\0';
				        }
				    }
				    const bool is_rfh = (kname[0] != '\0' && std::strcmp(kname, "RuntimeFieldHandle") == 0);
				    if (is_rfh && ck1c != 0xFFFFFFFFu &&
				        ck1c != static_cast<uint32_t>(cr13 & 0xffffffffull)) {
				        ctx->EFlags = ctx->EFlags | 0x40u;
				        LOGF("ps5-vsh: layout-cmp FORCED-RFH klass=0x%016llx k1c=%u r13d=%u kb0=%llx kb0f=0x%x\n",
				             (unsigned long long)cklass, ck1c,
				             static_cast<unsigned>(cr13 & 0xffffffffull),
				             (unsigned long long)kb0, kb0f);
				        const uint64_t rbp = ctx->Rbp;
				        uint8_t fl98 = 0xFFu, flb5 = 0xFFu, fla4 = 0xFFu, flba = 0xFFu;
				        uint32_t cnt94 = 0xFFFFFFFFu, fcc = 0xFFFFFFFFu, fc4 = 0xFFFFFFFFu, fdc = 0xFFFFFFFFu;
				        uint64_t f90 = 0, fc0 = 0, fd8 = 0;
				        uint64_t f0_type = 0, f0_name = 0, f0_parent = 0, t0_data = 0;
				        uint32_t f0_off = 0xFFFFFFFFu, t0_attrs = 0xFFFFFFFFu;
				        if (IsReadableRange(rbp - 0x98, 1)) {
				            fl98 = *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(rbp - 0x98));
				        }
				        if (IsReadableRange(rbp - 0xb5, 1)) {
				            flb5 = *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(rbp - 0xb5));
				        }
				        if (IsReadableRange(rbp - 0xa4, 1)) {
				            fla4 = *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(rbp - 0xa4));
				        }
				        if (IsReadableRange(cklass + 0xba, 1)) {
				            flba = *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(cklass + 0xba));
				        }
				        if (IsReadableRange(rbp - 0x94, 4)) {
				            cnt94 = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(rbp - 0x94));
				        }
				        if (IsReadableRange(rbp - 0xcc, 4)) {
				            fcc = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(rbp - 0xcc));
				        }
				        if (IsReadableRange(rbp - 0xc4, 4)) {
				            fc4 = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(rbp - 0xc4));
				        }
				        if (IsReadableRange(rbp - 0xdc, 4)) {
				            fdc = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(rbp - 0xdc));
				        }
				        if (IsReadableRange(cklass + 0x90, 8)) {
				            f90 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(cklass + 0x90));
				        }
				        if (IsReadableRange(rbp - 0xc0, 8)) {
				            fc0 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(rbp - 0xc0));
				        }
				        if (IsReadableRange(rbp - 0xd8, 8)) {
				            fd8 = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(rbp - 0xd8));
				        }
				        if (f90 >= 0x10000ull && IsReadableRange(f90, 0x20)) {
				            f0_type = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f90));
				            f0_name = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f90 + 8));
				            f0_parent = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f90 + 0x10));
				            f0_off = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(f90 + 0x18));
				        }
				        if (f0_type >= 0x10000ull && IsReadableRange(f0_type, 16)) {
				            t0_data = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(f0_type));
				            t0_attrs = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(f0_type + 8));
				        }
				        LOGF("ps5-vsh: RFH-STATE fl98=%02x fla4=%02x flb5=%02x ba=%02x cnt94=%u cc=0x%x c4=%u dc=%u f90=%llx fc0=%llx fd8=%llx f0_type=%llx f0_name=%llx f0_parent=%llx f0_off=%u t0_data=%llx t0_attrs=0x%08x\n",
				             fl98, fla4, flb5, flba, cnt94, fcc, fc4, fdc,
				             (unsigned long long)f90, (unsigned long long)fc0, (unsigned long long)fd8,
				             (unsigned long long)f0_type, (unsigned long long)f0_name, (unsigned long long)f0_parent,
				             f0_off, (unsigned long long)t0_data, t0_attrs);
				        // Complete RFH layout in-place so the post-0xBF3D0 field-offset
				        // loop (0xBF420) and valuetype size store (0xBF476) publish
				        // the true 64-bit IntPtr layout (offsets[0]=16, min_align=8,
				        // valuetype_size=24) instead of leaving value@0 and align=1!
				        if (fc0 >= 0x10000ull && IsReadableRange(fc0, 4)) {
				            const uint32_t cur_off0 = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(fc0));
				            if (cur_off0 == 0) {
				                *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(fc0)) = 16u;
				            }
				        }
				        if (f90 >= 0x10000ull && IsReadableRange(f90 + 0x18, 4) && f0_off == 0) {
				            *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(f90 + 0x18)) = 16u;
				        }
				        if (IsReadableRange(cklass + 0x88, 4)) {
				            const uint32_t cur_k88 = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(cklass + 0x88));
				            if (cur_k88 == 0) {
				                *reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(cklass + 0x88)) = 24u;
				            }
				        }
				        if ((ctx->R15 & 0xffull) == 1u) {
				            ctx->R15 = 8u;
				        }
				        ctx->R13 = ck1c;
				    } else if (cr13 == cklass) {
				        ctx->EFlags = ctx->EFlags | 0x40u;
				        LOGF("ps5-vsh: layout-cmp FORCED-PASS klass=0x%016llx k1c=%u r13d=%u kb0=%llx kb0f=0x%x\n",
				             (unsigned long long)cklass, ck1c,
				             static_cast<unsigned>(cr13 & 0xffffffffull),
				                         (unsigned long long)kb0, kb0f);
				    } else {
				        const bool eq = (ck1c != 0xFFFFFFFFu) &&
				            (ck1c == static_cast<uint32_t>(cr13 & 0xffffffffull));
				        if (eq) {
				            ctx->EFlags = ctx->EFlags | 0x40u;
				        } else {
				            ctx->EFlags = ctx->EFlags & ~0x40u;
				        }
				        LOGF("ps5-vsh: layout-cmp honest klass=0x%016llx k1c=%u r13d=%u taken=%d kb0=%llx kb0f=0x%x\n",
				             (unsigned long long)cklass, ck1c,
				             static_cast<unsigned>(cr13 & 0xffffffffull), eq ? 0 : 1,
				                         (unsigned long long)kb0, kb0f);
				    }
				    Log::Flush();
				    ctx->Rip = info->exception_address + 4;
				    return true;
				}
				// ps5-vsh: unlock-PLT trap (chainSUI134, 0x3D5330). Every
				// monosgen pthread_mutex_unlock lands here BEFORE the PLT
				// jump: rdi = mutex, [rsp] = live return site inside the
				// wrapper, rbp chain = live callers (no dead-slot fiction).
				// Log non-.bss mutexes (heap/stack culprits); emulate the
				// `jmp [rip+disp32]` via the GOT. Keep armed.
				if (bp_rva == 0x3D5330u && b[0] == 0xCCu && b[1] == 0x25u) {
				    const bool is_lock = false;
				    int32_t pdisp = 0;
				    std::memcpy(&pdisp, b + 2, sizeof(pdisp));
				    const uint64_t got_ptr =
				        info->exception_address + 6 + static_cast<int64_t>(pdisp);
				    uint64_t target = 0;
				    if (IsReadableRange(got_ptr, 8)) {
				        std::memcpy(&target,
				                    reinterpret_cast<const void*>(
				                        static_cast<uintptr_t>(got_ptr)),
				                    sizeof(target));
				    }
				    const uint64_t urdi = ctx->Rdi;
				    const uint64_t ursp = ctx->Rsp;
				    const uint64_t urbp = ctx->Rbp;
				    uint64_t ret0 = 0, ra1 = 0, ra2 = 0, fp1 = 0;
				    if (IsReadableRange(ursp, 8)) {
				        std::memcpy(&ret0,
				                    reinterpret_cast<const void*>(
				                        static_cast<uintptr_t>(ursp)),
				                    sizeof(ret0));
				    }
				    if (IsReadableRange(urbp, 8)) {
				        std::memcpy(&fp1,
				                    reinterpret_cast<const void*>(
				                        static_cast<uintptr_t>(urbp)),
				                    sizeof(fp1));
				    }
				    if (IsReadableRange(urbp + 8, 8)) {
				        std::memcpy(&ra1,
				                    reinterpret_cast<const void*>(
				                        static_cast<uintptr_t>(urbp + 8)),
				                    sizeof(ra1));
				    }
				    if (fp1 >= 0x10000ull && IsReadableRange(fp1 + 8, 8)) {
				        std::memcpy(&ra2,
				                    reinterpret_cast<const void*>(
				                        static_cast<uintptr_t>(fp1 + 8)),
				                    sizeof(ra2));
				    }
				    // ps5-vsh: the trap is intentional ("keep armed"), but this
				    // diagnostic must not be unbounded: monosgen lands here on
				    // every pthread_mutex_unlock, and in notifPark1 that was
				    // 922,387 traps = 87.9 MiB = 43.8% of a 200 MiB log, each
				    // followed by a forced Log::Flush() - a large
				    // self-inflicted cost in exactly the phase that starves
				    // the guest. Bounded first-64 plus periodic sampling, the
				    // same policy as the other hot-path probes; the counter
				    // keeps advancing so the census stays readable, and the
				    // samples still carry rdi/ret0/ra1/ra2 so an out-of-.bss
				    // mutex is still attributable.
				    static std::atomic<uint64_t> s_utraps {0};
				    const uint64_t un =
				        s_utraps.fetch_add(1, std::memory_order_relaxed);
				    if (un < 64 || (un % 65536ull) == 0) {
				        LOGF("ps5-vsh: %s #%llu mutex=%llx ret0=%llx ra1=%llx ra2=%llx "
				             "tgt=%llx\n",
				             is_lock ? "LPLT" : "UPLT",
				             static_cast<unsigned long long>(un),
				             static_cast<unsigned long long>(urdi),
				             static_cast<unsigned long long>(ret0),
				             static_cast<unsigned long long>(ra1),
				             static_cast<unsigned long long>(ra2),
				             static_cast<unsigned long long>(target));
				        Log::Flush();
				    }
				    if (target != 0 && target != info->exception_address) {
				        ctx->Rip = target;
				        return true;
				    }
				}
			}
		}
	}
	if (info->type == Common::HostException::ExceptionType::Breakpoint &&
	    Common::Singleton<RuntimeLinker>::Instance()->FindProgramByAddr(
	        info->exception_address) != nullptr &&
	    Loader::X64InstructionEmulator::TrySkipInt3(info->native_context)) {
		LOGF("ps5-vsh: skipped guest int3 at 0x%016" PRIx64 " (guest limps on)\n",
		     info->exception_address);
		return true;
	}

	// ps5-vsh chainSUI229: fault-driven null checks. Mono installs a SIGSEGV
	// handler and compiles implicit null checks; dispatch first so managed
	// NullReference rises instead of dying here. Declines when inapplicable.
	if (Libs::LibKernel::Ps5VshTryGuestSigsegv(exception_info)) {
		return true;
	}
	// Report whatever guest context can be read safely before terminating: which guest thread
	// faulted, the register file, the faulting code bytes and the top of its stack.
#if defined(_WIN32)
	// ps5-vsh: host backtrace for crashes inside HLE code. RVAs are
	// executable-relative; resolve offline with llvm-symbolizer.
	// StackWalk64 off the exception context (robust when the stack is
	// smashed and CaptureStackBackTrace yields nothing) with dbghelp
	// symbol names when available.
	{
		const auto image_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
		std::printf("--- Host backtrace (image base 0x%016" PRIx64 ") ---\n", image_base);
		HANDLE process = GetCurrentProcess();
		static std::atomic_bool sym_init {false};
		bool                   have_sym = false;
		{
			bool expected = false;
			if (sym_init.compare_exchange_strong(expected, true)) {
				DWORD opts = SymGetOptions();
				SymSetOptions(opts | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
				have_sym = (SymInitialize(process, nullptr, TRUE) != 0);
				if (!have_sym) {
					sym_init.store(false);
				}
			} else {
				have_sym = true;
			}
		}
		auto* ctx = reinterpret_cast<PCONTEXT>(info->native_context);
		if (ctx != nullptr) {
			STACKFRAME64 frame {};
			frame.AddrPC.Offset    = ctx->Rip;
			frame.AddrPC.Mode      = AddrModeFlat;
			frame.AddrFrame.Offset = ctx->Rbp;
			frame.AddrFrame.Mode   = AddrModeFlat;
			frame.AddrStack.Offset = ctx->Rsp;
			frame.AddrStack.Mode   = AddrModeFlat;
			for (int i = 0; i < 24; i++) {
				if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process,
				                 GetCurrentThread(), &frame, ctx, nullptr,
				                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
					break;
				}
				if (frame.AddrPC.Offset == 0) {
					break;
				}
				char         mod[MAX_PATH] = "?";
				HMODULE      hm            = nullptr;
				const auto*  pc            = reinterpret_cast<const void*>(
                    static_cast<uintptr_t>(frame.AddrPC.Offset));
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				                       reinterpret_cast<LPCSTR>(pc), &hm) != 0 &&
				    hm != nullptr) {
					GetModuleFileNameA(hm, mod, static_cast<DWORD>(sizeof(mod)));
					if (const char* base = std::strrchr(mod, '\\'); base != nullptr) {
						std::memmove(mod, base + 1, std::strlen(base));
					}
				}
				char         symbuf[sizeof(SYMBOL_INFO) + 256] = {};
				auto*        sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
				sym->SizeOfStruct = sizeof(SYMBOL_INFO);
				sym->MaxNameLen   = 255;
				DWORD64        disp = 0;
				const char*    name = "?";
				char           narrow[256] = {};
				if (have_sym && SymFromAddr(process, frame.AddrPC.Offset, &disp, sym) != 0) {
					for (int c = 0; c < 255 && sym->Name[c] != '\0'; c++) {
						narrow[c] = (sym->Name[c] >= 0x20 && sym->Name[c] < 0x7f)
						                ? sym->Name[c]
						                : '.';
					}
					name = narrow;
				}
				std::printf(" [%02d] pc=0x%016" PRIx64 " rva=0x%08" PRIx64 " %s!%s\n", i,
				            static_cast<uint64_t>(frame.AddrPC.Offset),
				            static_cast<uint64_t>(frame.AddrPC.Offset) - image_base, mod,
				            name);
				if (have_sym) {
					(void)disp;
				}
			}
		} else {
			void* pcs[32] = {};
			const auto frames =
			    CaptureStackBackTrace(0, static_cast<DWORD>(std::size(pcs)), pcs, nullptr);
			for (unsigned i = 0; i < frames; i++) {
				char        module_name[MAX_PATH] = "?";
				HMODULE     module                = nullptr;
				const auto* pc                    = reinterpret_cast<const void*>(pcs[i]);
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				                       reinterpret_cast<LPCSTR>(pc), &module) != 0 &&
				    module != nullptr) {
					GetModuleFileNameA(module, module_name,
					                   static_cast<DWORD>(sizeof(module_name)));
					if (const char* base = std::strrchr(module_name, '\\'); base != nullptr) {
						std::memmove(module_name, base + 1, std::strlen(base));
					}
				}
				std::printf(" [%02u] pc=0x%016" PRIx64 " rva=0x%08" PRIx64 " %s\n", i,
				            reinterpret_cast<uint64_t>(pcs[i]),
				            reinterpret_cast<uint64_t>(pcs[i]) - image_base, module_name);
			}
		}
		std::fflush(stdout);
	}
#endif
	{
		char thread_name[64] = "(host thread)";
		if (auto self = Libs::LibKernel::PthreadSelfOrNull(); self != nullptr) {
			if (Libs::LibKernel::PthreadGetname(self, thread_name) != 0) {
				std::snprintf(thread_name, sizeof(thread_name), "(unnamed guest thread)");
			}
		}
		std::printf("--- Guest fault context ---\n");
		std::printf("thread: %s\n", thread_name);
		std::printf("rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64 "\n"
		            "rsi=%016" PRIx64 " rdi=%016" PRIx64 " rbp=%016" PRIx64 " rsp=%016" PRIx64 "\n"
		            "r8 =%016" PRIx64 " r9 =%016" PRIx64 " r10=%016" PRIx64 " r11=%016" PRIx64 "\n"
		            "r12=%016" PRIx64 " r13=%016" PRIx64 " r14=%016" PRIx64 " r15=%016" PRIx64 "\n",
		            info->rax, info->rbx, info->rcx, info->rdx, info->rsi, info->rdi, info->rbp,
		            info->rsp, info->r8, info->r9, info->r10, info->r11, info->r12, info->r13,
		            info->r14, info->r15);
		// ps5-vsh: dump candidate object memory. Faults like call *%rax with
		// rax=0 usually mean an object (rbx/rdi) whose vtable slot is null;
		// showing the first qwords identifies zero-filled vs garbage memory.
		for (const auto [label, addr] : {std::pair<const char*, uint64_t> {"rbx", info->rbx},
		                                 {"rdi", info->rdi},
		                                 {"rsi", info->rsi},
		                                 {"rcx", info->rcx},
                                                 {"r12", info->r12},
                                                 {"r13", info->r13},
                                                 {"r14", info->r14},
                                                 {"r8", info->r8}}) {
			if (addr != 0 && IsReadableRange(addr, 64)) {
				const auto* q = reinterpret_cast<const uint64_t*>(addr);
				std::printf("obj %s @0x%016" PRIx64 ": %016" PRIx64 " %016" PRIx64
				            " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64
				            " %016" PRIx64 " %016" PRIx64 "\n",
				            label, addr, q[0], q[1], q[2], q[3], q[4], q[5], q[6],
				            q[7]);
			} else if (addr != 0) {
				std::printf("obj %s @0x%016" PRIx64 ": <unreadable>\n", label, addr);
			}
		}
		// ps5-vsh (chainSUI144): sniff C-strings behind every integer
		// register. Host-fmt crashes (vsprintf <- ?) hide which literal
		// was formatted and which arg was bad; the strings name both.
		for (const auto [label, addr] : {std::pair<const char*, uint64_t>{"rax", info->rax},
		                                                 {"rbx", info->rbx},
		                                                 {"rcx", info->rcx},
		                                                 {"rdx", info->rdx},
		                                                 {"rsi", info->rsi},
		                                                 {"rdi", info->rdi},
		                                                 {"rbp", info->rbp},
		                                                 {"rsp", info->rsp},
		                                                 {"r8", info->r8},
		                                                 {"r9", info->r9},
		                                                 {"r10", info->r10},
		                                                 {"r11", info->r11},
		                                                 {"r12", info->r12},
		                                                 {"r13", info->r13},
		                                                 {"r14", info->r14},
		                                                 {"r15", info->r15}}) {
			if (addr == 0 || !IsReadableRange(addr, 1)) {
				continue;
			}
			char str[96] = {};
			size_t slen = 0;
			for (; slen < sizeof(str) - 1; slen++) {
				if (!IsReadableRange(addr + slen, 1)) {
					break;
				}
				const char ch = *reinterpret_cast<const char*>(addr + slen);
				if (ch == '\0') {
					break;
				}
				if (ch < 0x20 || ch >= 0x7f) {
					slen = 0;
					break;
				}
				str[slen] = ch;
			}
			if (slen >= 4) {
				std::printf("regstr %s @0x%016llx: \"%s\"\n", label,
				            (unsigned long long)addr, str);
			}
		}
		std::fflush(stdout);
		if (IsReadableRange(info->exception_address - 48, 96)) {
			const auto* code = reinterpret_cast<const uint8_t*>(info->exception_address - 48);
			std::printf("code (pc-48 .. pc+48, fault at byte 48):");
			for (int i = 0; i < 96; i++) {
				std::printf("%s%02x", (i % 16 == 0) ? "\n " : " ", code[i]);
			}
			std::printf("\n");
		}
		if (IsReadableRange(info->rsp, 32 * sizeof(uint64_t))) {
			const auto* stack = reinterpret_cast<const uint64_t*>(info->rsp);
			std::printf("stack:");
			for (int i = 0; i < 32; i++) {
				std::printf("%s %016" PRIx64, (i % 4 == 0) ? "\n " : "", stack[i]);
			}
			std::printf("\n");
		}
		// ps5-vsh: guest rbp-chain walk (JSCd triage): prints return
		// addresses so the dynamic caller chain is exact, not inferred.
		// chainSUI222: also print rbp%16 per frame. Normal frames have
		// rbp%16==0 (entered with rsp%16==8, pushed rbp); a frame with
		// rbp%16==8 was entered misaligned, and the FIRST such frame
		// going outward pinpoints the imbalance birth (recurring -1
		// movaps family). Fatal path only; zero hot-path cost.
		{
			uint64_t rbp = info->rbp;
			std::printf("guest-bp-chain:");
			for (int i = 0; i < 12; i++) {
				if (!IsReadableRange(rbp, 16)) {
					std::printf(" [end @0x%016" PRIx64 "]", rbp);
					break;
				}
				const auto* f = reinterpret_cast<const uint64_t*>(rbp);
				std::printf(" [ret=0x%016" PRIx64 " bp%%16=%llu]", f[1],
				            (unsigned long long)(rbp & 15u));
				const uint64_t next = f[0];
				if (next == 0 || next == rbp) {
					break;
				}
				rbp = next;
			}
			std::printf("\n");
		}
		std::fflush(stdout);
	}
	{
		// ps5-vsh: name the module owning the faulting host PC (ASLR slides
		// bases every run; the raw pc alone is unmappable offline).
		char    fault_module[MAX_PATH] = "?";
		HMODULE fault_hmod             = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       reinterpret_cast<LPCSTR>(info->exception_address),
		                       &fault_hmod) != 0 &&
		    fault_hmod != nullptr) {
			GetModuleFileNameA(fault_hmod, fault_module, static_cast<DWORD>(sizeof(fault_module)));
			if (const char* base = std::strrchr(fault_module, '\\'); base != nullptr) {
				std::memmove(fault_module, base + 1, std::strlen(base));
			}
		}
	std::printf("--- Faulting host module: %s (image base 0x%016" PRIx64 ") ---\n",
	            fault_module, reinterpret_cast<uint64_t>(fault_hmod));
	std::fflush(stdout);
}
LOGF("--- Guest fault regs: rip=%016llx rsp=%016llx rbp=%016llx rdi=%016llx rsi=%016llx "
     "rdx=%016llx rcx=%016llx r8=%016llx r9=%016llx\n",
     (unsigned long long)info->exception_address, (unsigned long long)info->rsp,
     (unsigned long long)info->rbp, (unsigned long long)info->rdi, (unsigned long long)info->rsi,
     (unsigned long long)info->rdx, (unsigned long long)info->rcx, (unsigned long long)info->r8,
     (unsigned long long)info->r9);
// ps5-vsh: the remaining GP registers and the faulting bytes. Needed because the
// reported rip has been seen not to match the statically decoded instruction at
// that VA (libhermes +0x73A84), and rsp can sit inside a host HLE frame (guest
// code called an HLE which called back into guest code).
LOGF("--- Guest fault regs2: rax=%016llx rbx=%016llx r10=%016llx r11=%016llx "
     "r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
     (unsigned long long)info->rax, (unsigned long long)info->rbx, (unsigned long long)info->r10,
     (unsigned long long)info->r11, (unsigned long long)info->r12, (unsigned long long)info->r13,
     (unsigned long long)info->r14, (unsigned long long)info->r15);
if (IsReadableRange(info->exception_address, 16)) {
	const auto* fc = reinterpret_cast<const uint8_t*>(info->exception_address);
	LOGF("--- Guest fault code: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
	     "%02x %02x %02x %02x\n",
	     fc[0], fc[1], fc[2], fc[3], fc[4], fc[5], fc[6], fc[7], fc[8], fc[9], fc[10], fc[11],
	     fc[12], fc[13], fc[14], fc[15]);
}
if (IsReadableRange(info->rsp, 64 * sizeof(uint64_t))) {
	const auto* stack = reinterpret_cast<const uint64_t*>(info->rsp);
	// ps5-vsh: one LOGF for the whole window. With 300 guest threads the
	// per-qword LOGFs used to interleave with TRACE lines and the stack was
	// unreadable exactly when it mattered (rnpherm1).
	char line[64 * 17 + 32];
	int  at = std::snprintf(line, sizeof(line), "--- Guest fault stack64:");
	for (int i = 0; i < 64 && at > 0 && static_cast<size_t>(at) + 20 < sizeof(line); i++) {
		at += std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at), " %016llx",
		                    static_cast<unsigned long long>(stack[i]));
	}
	LOGF("%s\n", line);
}
{
	// ps5-vsh: unlock-free rbp walk so every caller of the faulting guest frame
	// is recoverable offline from the 'place <module> base=' lines. StackTrace()
	// would take RuntimeLinker::m_mutex, which is unsafe from an exception
	// handler that may have interrupted a loader call.
	//
	// Two lines per frame: the chain summary, then the frame's locals/out-slots.
	// The locals matter because JSI/RN pass values through stack slots
	// (rnpherm2: libSceJsi stores the object handle at [rbp-0x70] and the
	// jsi::Value at [rbp-0x100]); without them a backtrace only names frames.
	uint64_t fp = info->rbp;
	char     line[960];
	for (int i = 0; i < 16; i++) {
		if (!IsReadableRange(fp, 2 * sizeof(uint64_t))) {
			break;
		}
		const auto* f    = reinterpret_cast<const uint64_t*>(fp);
		const auto  next = f[0];
		LOGF("--- frame[%d] bp=%016llx ret=%016llx\n", i, (unsigned long long)fp,
		     static_cast<unsigned long long>(f[1]));
		if (IsReadableRange(fp - 0x38, 0x38 + 0x10)) {
			const auto* l = reinterpret_cast<const uint64_t*>(fp - 0x38);
			int         at = std::snprintf(line, sizeof(line), "      L:");
			for (int j = 0; j < 10 && at > 0 && static_cast<size_t>(at) + 20 < sizeof(line); j++) {
				at += std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at),
				                    " %016llx", static_cast<unsigned long long>(l[j]));
			}
			LOGF("%s\n", line);
		}
		if (next == 0 || next == fp || (next & 7u) != 0) {
			break;
		}
		fp = next;
	}
}
	DumpTraceRings();
	// ps5-vsh TEMP-DIAG (chainSUI98): dump the mmap-out arg ring with the
	// crash forensics (the killer call never logs through LOGF).
	Libs::LibcInternal::Ps5VshDumpMmapOut();
	Libs::Ps5VshDumpPrintfEntrySp();
	Libs::LibcInternal::Ps5VshDumpStackWatch();
	Log::Flush(); // crash forensics: regs/stack/tail must survive (no flush on EXIT)
	EXIT("Unhandled host exception: type=%u code=%u pc=0x%016" PRIx64
	     " access=%u address=0x%016" PRIx64 "\n",
	     static_cast<unsigned>(info->type), info->native_code, info->exception_address,
	     static_cast<unsigned>(info->access_violation_type), info->access_violation_vaddr);
}

static void EncodeId64(uint16_t in_id, std::string* out_id) {
	static const char* str = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
	if (in_id < 0x40u) {
		*out_id += str[in_id];
	} else {
		if (in_id < 0x1000u) {
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		} else {
			*out_id += str[static_cast<uint16_t>(in_id >> 12u) & 0x3fu];
			*out_id += str[static_cast<uint16_t>(in_id >> 6u) & 0x3fu];
			*out_id += str[in_id & 0x3fu];
		}
	}
}

template <class T>
static void GetDynDataOs(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = elf->GetDynamicData<T>(dyn->d_un.d_ptr);
	}
}

template <class T>
static void GetDynData(Elf64* elf, uint64_t base_vaddr, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = reinterpret_cast<T>(base_vaddr + dyn->d_un.d_ptr);
	}
}

template <class T>
static void GetDynValue(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = dyn->d_un.d_val;
	}
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestInstallNativeFatalHandler() {
	return Common::HostException::InstallHandler(KytyExceptionHandler);
}
bool TestNativeFatalHandler(uint64_t pc, unsigned access_kind, void* context) {
	Common::HostException::ExceptionInfo info {};
	info.exception_address = pc;
	info.native_context = context;
	info.type = access_kind == 4 ? Common::HostException::ExceptionType::IllegalInstruction :
	                             Common::HostException::ExceptionType::AccessViolation;
	info.access_violation_type = static_cast<Common::HostException::AccessViolationType>(access_kind);
	info.access_violation_vaddr = 0;
	return KytyExceptionHandler(info);
}
#endif

template <class T>
static void GetDynValues(Elf64* elf, T* out, Elf64_Sxword tag) {
	for (const auto* dyn: elf->GetDynList(tag)) {
		out->push_back(dyn->d_un.d_val);
	}
}

template <class T>
static void GetDynPtr(Elf64* elf, T* out, Elf64_Sxword tag) {
	if (const auto* dyn = elf->GetDynValue(tag); dyn != nullptr) {
		*out = dyn->d_un.d_ptr;
	}
}

static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ProgramExitHandler() {
	Common::Singleton<RuntimeLinker>::Instance()->StopAllModules();

	LOGF("exit!!!\n");
}

template <class T>
static void GetDynModules(Elf64* elf, T* out, const char* names, Elf64_Sxword tag) {
	std::vector<uint64_t> needed_modules;
	GetDynValues(elf, &needed_modules, tag);
	for (auto need: needed_modules) {
		ModuleId id {};
		// id.id            = static_cast<int>((need >> 48u) & 0xffffu);
		EncodeId64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version_major = static_cast<int>((need >> 40u) & 0xffu);
		id.version_minor = static_cast<int>((need >> 32u) & 0xffu);
		id.name          = names + (need & 0xffffffff);
		out->push_back(id);
	}
}

template <class T>
static void GetDynLibs(Elf64* elf, T* out, const char* names, Elf64_Sxword tag) {
	std::vector<uint64_t> needed_modules;
	GetDynValues(elf, &needed_modules, tag);
	for (auto need: needed_modules) {
		LibraryId id {};
		// id.id      = static_cast<int>((need >> 48u) & 0xffffu);
		EncodeId64(static_cast<uint16_t>((need >> 48u) & 0xffffu), &id.id);
		id.version = static_cast<int>((need >> 32u) & 0xffffu);
		id.name    = names + (need & 0xffffffff);
		out->push_back(id);
	}
}

static RelocationInfo GetRelocationInfo(Elf64_Rela* r, Program* program) {
	KYTY_PROFILER_FUNCTION();

	RelocationInfo ret;
	// SymbolRecord   sr {};

	auto         type    = r->GetType();
	auto         symbol  = r->GetSymbol();
	Elf64_Sxword addend  = r->r_addend;
	auto*        symbols = program->dynamic_info->symbol_table;
	auto*        names   = program->dynamic_info->str_table;
	ret.base_vaddr       = program->base_vaddr;
	ret.vaddr            = ret.base_vaddr + r->r_offset;
	ret.bind_self        = false;

	switch (type) {
		case R_X86_64_GLOB_DAT:
		case R_X86_64_JUMP_SLOT: addend = 0; [[fallthrough]];
		case R_X86_64_64: {
			auto         sym          = symbols[symbol];
			auto         bind         = sym.GetBind();
			auto         sym_type     = sym.GetType();
			uint64_t     symbol_vaddr = 0;
			SymbolRecord sr {};
			switch (sym_type) {
				case STT_NOTYPE: ret.type = SymbolType::NoType; break;
				case STT_FUNC: ret.type = SymbolType::Func; break;
				case STT_OBJECT: ret.type = SymbolType::Object; break;
				default: EXIT("unknown symbol type: %d\n", (int)sym_type);
			}
			switch (bind) {
				case STB_LOCAL:
					symbol_vaddr = ret.base_vaddr + sym.st_value;
					ret.bind     = BindType::Local;
					break;
				case STB_GLOBAL: ret.bind = BindType::Global; [[fallthrough]];
				case STB_WEAK: {
					ret.bind = (ret.bind == BindType::Unknown ? BindType::Weak : ret.bind);
					ret.name = names + sym.st_name;
					program->rt->Resolve(ret.name, ret.type, program, &sr, &ret.bind_self);
					symbol_vaddr = sr.vaddr;
					if (program->application_owner && ret.type == SymbolType::Object && symbol_vaddr) {
						if (const auto* provider = program->rt->FindProgramByAddr(symbol_vaddr)) {
							// An addend must not disguise a borrowed session/foreign base.
							if (provider->application_owner != program->application_owner) { symbol_vaddr = 0; }
						} else {
							symbol_vaddr = program->rt->ResolveApplicationImportData(program, ret.name, sr, addend, sym.st_size);
							ret.application_data = symbol_vaddr != 0;
							addend = 0; // The exact-object adapter has already bounded/applied it.
						}
					}
				} break;
				default: EXIT("unknown bind: %d\n", (int)bind);
			}
			ret.resolved = (symbol_vaddr != 0);
			ret.value    = (ret.resolved ? symbol_vaddr + addend : 0);
			ret.name     = sr.name;
			ret.dbg_name = sr.dbg_name;
		} break;
		case R_X86_64_RELATIVE:
			ret.value    = ret.base_vaddr + addend;
			ret.resolved = true;
			break;
		case R_X86_64_DTPMOD64:
			ret.value    = reinterpret_cast<uint64_t>(program);
			ret.resolved = true;
			ret.type     = SymbolType::TlsModule;
			ret.bind     = BindType::Local;
			ret.dbg_name = Common::PathToString(program->file_name);
			break;
		default: EXIT("unknown type: %d\n", (int)type);
	}

	return ret;
}

static bool RelocateRecord(uint32_t index, Elf64_Rela* r, Program* program, bool jmprela_table) {
	KYTY_PROFILER_FUNCTION();
	if (program->boot_credential_admission) {
		const auto type = r->GetType();
		const bool symbol = type == R_X86_64_64 || type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT;
		bool valid = symbol || type == R_X86_64_RELATIVE || type == R_X86_64_DTPMOD64;
		valid = valid && r->r_offset <= program->base_size && sizeof(uint64_t) <= program->base_size - r->r_offset;
		if (symbol) { valid = valid && r->GetSymbol() < program->dynamic_info->symbol_table_total_size / sizeof(Elf64_Sym); }
		bool data_target = false;
		const auto* ph = program->elf->GetPhdr();
		for (size_t i = 0; i < program->elf->GetEhdr()->e_phnum; ++i) {
			if (ph[i].p_type == PT_LOAD && !(ph[i].p_flags & PF_X) && r->r_offset >= ph[i].p_vaddr &&
			    r->r_offset - ph[i].p_vaddr <= ph[i].p_memsz && sizeof(uint64_t) <= ph[i].p_memsz - (r->r_offset - ph[i].p_vaddr)) { data_target = true; }
		}
		if (!valid || !data_target) { RevokeBootCredentials(program); return false; }
	}
	if (program->application_owner) {
		if (const auto execution = GetApplicationExecution(program); execution && execution->StopRequested()) { return false; }
		const auto type = r->GetType();
		bool supported = type == R_X86_64_RELATIVE || type == R_X86_64_DTPMOD64;
		if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT) {
			const auto& symbol = program->dynamic_info->symbol_table[r->GetSymbol()];
			supported = (symbol.GetType() == STT_FUNC || symbol.GetType() == STT_OBJECT) &&
			    (symbol.GetBind() == STB_LOCAL || symbol.GetBind() == STB_GLOBAL || symbol.GetBind() == STB_WEAK) &&
			    !(symbol.GetType() == STT_FUNC && type == R_X86_64_64 && r->r_addend != 0);
		}
		if (!supported || r->r_offset > program->base_size || sizeof(uint64_t) > program->base_size - r->r_offset) {
			if (auto execution = GetApplicationExecution(program)) { execution->Fail(GuestFailureReason::InvalidTransition, r->r_offset); }
			return false;
		}
	}

	const auto ri      = GetRelocationInfo(r, program);
	// This codec processes PCM on success; a zero-return dependency thunk can
	// silently corrupt output. The legacy "fail" flag alone only logs misses.
	if (!ri.resolved && ri.bind == BindType::Global && program->fail_if_global_not_resolved &&
	    Common::EqualNoCase(program->file_name.stem().string(), "libSceAudiodecCpuDdp")) {
		EXIT("Required native DDP dependency unresolved: %s\n", ri.name.c_str());
	}
	auto       value   = ri.value;
	bool       stubbed = false;
	if (program->application_owner != nullptr && ri.type == SymbolType::Func &&
	    (ri.bind == BindType::Global || ri.bind == BindType::Weak) &&
	    (r->GetType() == R_X86_64_JUMP_SLOT || r->GetType() == R_X86_64_GLOB_DAT ||
	     (r->GetType() == R_X86_64_64 && r->r_addend == 0))) {
		if (!program->application_import_bindings) {
			program->application_import_bindings = std::make_unique<ApplicationImportBindings>(program);
		}
		const auto& info = *program->dynamic_info;
		const std::string raw_name = info.str_table + info.symbol_table[r->GetSymbol()].st_name;
		value = program->application_import_bindings->Bind(ri.vaddr, raw_name);
		if (!value) {
			if (auto execution = GetApplicationExecution(program)) {
				execution->Fail(GuestFailureReason::BoundaryUnavailable, ri.vaddr);
			}
			value = g_invalid_memory;
		}
	} else if (!ri.resolved) {
		const bool weak = ri.bind == BindType::Weak || !program->fail_if_global_not_resolved;
		if (!weak) {
			LOGF("Stubbed: %s\n",
			     fmt::format("[{:016x}] <- {:016x}, {}, {}, {}, {}", ri.vaddr, ri.value, ri.name,
			                 magic_enum::enum_name(ri.type), magic_enum::enum_name(ri.bind),
			                 ri.dbg_name).c_str());
		}
		if (ri.type == SymbolType::Object) {
			value = g_invalid_memory;
		} else if (program->application_owner != nullptr) {
			// Only typed function imports can use the baseline-SysV deferred
			// gateway. Unknown/data relocations never use legacy zero returns.
			value = g_invalid_memory;
		} else {
			value   = RegisterStubbedImport(index, program, ri);
			stubbed = true;
		}
	} else if (ri.type == SymbolType::Func && value != 0 && !ri.name.empty() && !program->native_ssl_smoke_dependency &&
	           !program->rt->IsNativeSslAddress(value)) {
		value = TraceWrapImport(value, ri.name);
	}

	bool patched = false;
	if (ri.application_data) {
		const auto execution = GetApplicationExecution(program);
		auto lease = execution ? execution->TryAcquire(GuestWorkKind::HostCall) : RuntimeGuestExecution::Lease {};
		// The final data-pointer store, not merely allocation, linearizes with
		// owner stop. This short callback performs no allocation or GPU work.
		if (!lease || !lease.Publish([&] { PatchGuestMemory64(ri.vaddr, value); patched = true; })) { return false; }
	} else { patched = PatchGuestMemory64(ri.vaddr, value); }
	if (!patched && program->application_owner) {
		if (auto execution = GetApplicationExecution(program)) { execution->Fail(GuestFailureReason::InvalidTransition, ri.vaddr); }
	}
	if (patched && stubbed) {
		LOGF("Relocate: unresolved %s import patched to stub [%u] [%016" PRIx64 "] <- %016" PRIx64
		     ", %s, %s, %s, %s\n",
		     jmprela_table ? "PLT" : "non-PLT", index, ri.vaddr, value, ri.name.c_str(),
		     magic_enum::enum_name(ri.type), magic_enum::enum_name(ri.bind),
		     Common::PathToString(program->file_name).c_str());
	}

	if (program->dbg_print_reloc && patched && !ri.bind_self &&
	    (ri.bind == BindType::Global || ri.bind == BindType::Weak ||
	     ri.type == SymbolType::TlsModule)) {
		LOGF("Relocate: %s\n",
		     fmt::format("[{:016x}] <- {:016x}, {}, {}, {}, {}", ri.vaddr, ri.value, ri.name,
		                 magic_enum::enum_name(ri.type), magic_enum::enum_name(ri.bind),
		                 ri.dbg_name).c_str());
	}
	return ri.resolved;
}

static void ForEachRelocation(Program* program, auto&& func) {
	const auto& info = *program->dynamic_info;
	for (uint32_t i = 0; i < info.rela_table_total_size / sizeof(Elf64_Rela); ++i) {
		func(info.rela_table + i, i, false);
	}
	for (uint32_t i = 0; i < info.jmprela_table_size / sizeof(Elf64_Rela); ++i) {
		func(info.jmprela_table + i, i, true);
	}
}

static KYTY_MS_ABI uint8_t* TlsMainGetAddr() {
	EXIT_IF(g_tls_main_program == nullptr);

	// ps5-vsh: the caller is always a per-program SafeCall stub, so the
	// return address identifies the owning main. Capture first: later
	// calls in this frame must not disturb it.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	void* caller = _ReturnAddress();
#else
	void* caller = __builtin_return_address(0);
#endif
	Program* program = g_tls_main_program;
	const auto addr  = reinterpret_cast<uintptr_t>(caller);
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_tls_handler_ranges_mutex);
		for (const auto& [base, owner] : g_tls_handler_ranges) {
			if (addr >= base && addr < base + Jit::SafeCall::GetSize()) {
				program = owner;
				break;
			}
		}
	}

	if (g_tls_cached_main_program == program && g_tls_cached_main_tcb != nullptr) {
		return g_tls_cached_main_tcb;
	}

	g_tls_cached_main_program = program;
	g_tls_cached_main_tcb =
	    RuntimeLinker::TlsGetAddr(program) + program->tls.tcb_offset;
	return g_tls_cached_main_tcb;
}

static void PatchProgram(Program* program, uint64_t address, uint64_t size) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);

	if (size >= 12) {
		// Replace guest stack-canary/errno stores through fs:[0x28] with nops.
		// Windows x64 cannot host guest FS directly, and an unpatched shared-library access faults
		// at address 0x28.
		auto*         start_ptr           = reinterpret_cast<uint8_t*>(address);
		auto*         end_ptr             = start_ptr + size - 12;

		for (auto* ptr = start_ptr; ptr <= end_ptr; ptr++) {
			if (PatchGuestDiagnosticStore(std::span<uint8_t>(ptr, start_ptr + size - ptr))) {
				LOGF("Patch fs:[0x28] store at addr: [%016" PRIx64 "]\n",
				     reinterpret_cast<uint64_t>(ptr));
			}
		}
	}

	// ps5-vsh: default ON with opt-out via KYTY_VSH_NOP_VSYNC=0.
	// NOTE: the 2-byte jne guards below (75 12 / 75 6b) are weak on their own
	// (75 xx matches many short branches); the filename + fixed-RVA scoping is
	// what narrows them. Preceding bytes were not determined from the binary,
	// so no wider guard is asserted here rather than inventing bytes.
	if (!program->boot_credential_admission && program->application_owner == nullptr && !Ps5VshLoadPatchOptedOut("KYTY_VSH_NOP_VSYNC") &&
	    Common::PathToString(program->file_name.filename()) == "eboot.bin") {
		const uint64_t v1 = program->base_vaddr + 0x15499bu;
		if (v1 >= address && v1 + 2 <= address + size) {
			auto* p = reinterpret_cast<uint8_t*>(v1);
			if (p[0] == 0x75u && p[1] == 0x12u) {
				p[0] = 0x90;
				p[1] = 0x90;
				LOGF("ps5-vsh: load-patch neutered eboot WaitBeforeVSync jne at 0x%016" PRIx64 "\n", v1);
			}
		}
		const uint64_t v2 = program->base_vaddr + 0x154a29u;
		if (v2 >= address && v2 + 2 <= address + size) {
			auto* p = reinterpret_cast<uint8_t*>(v2);
			if (p[0] == 0x75u && p[1] == 0x6bu) {
				p[0] = 0x90;
				p[1] = 0x90;
				LOGF("ps5-vsh: load-patch neutered eboot WaitBeforeVSync caller jne at 0x%016" PRIx64 "\n", v2);
			}
		}
	}

	if (!program->elf->IsShared() && program->tls.handler_vaddr != 0 &&
	    size >= Jit::Call9::GetSize()) {
		// Replace:
		//   66 66 66
		//   mov <reg>, qword ptr fs:[0x00]
		// with:
		//   call <handler>
		//   mov <reg>,rax
		//   nop ...
		const uint8_t tls_pattern[5]       = {0x64, 0x48, 0x8B, 0x00, 0x25};
		const uint8_t zero_displacement[4] = {};

		EXIT_IF(Jit::Call9::GetSize() != 9);

		auto* start_ptr = reinterpret_cast<uint8_t*>(address);
		auto* end_ptr   = start_ptr + size - Jit::Call9::GetSize();

		for (auto* ptr = start_ptr; ptr <= end_ptr; ptr++) {
			auto*  inst_ptr     = ptr;
			size_t prefix_count = 0;
			while (prefix_count < 3 && inst_ptr < start_ptr + size && *inst_ptr == 0x66) {
				inst_ptr++;
				prefix_count++;
			}

			if (inst_ptr + Jit::Call9::GetSize() > start_ptr + size) {
				break;
			}

			const uint8_t modrm = inst_ptr[3];
			if (memcmp(inst_ptr, tls_pattern, 3) == 0 && (modrm & 0xc7u) == 0x04u &&
			    inst_ptr[4] == tls_pattern[4] &&
			    memcmp(inst_ptr + 5, zero_displacement, sizeof(zero_displacement)) == 0) {
				LOGF("Patch tls at addr: [%016" PRIx64 "]\n", reinterpret_cast<uint64_t>(inst_ptr));

				const auto reg = (modrm >> 3u) & 7u;
				EXIT_NOT_IMPLEMENTED(reg == 4u);

				// A raw scan can encounter a 0x66 in the preceding instruction, so do not
				// overwrite it. Call9 starts with REX.W to neutralize genuine 0x66
				// prefixes on AMD processors (before it could turn E8 into callw 16bit).
				auto* code = new (inst_ptr) Jit::Call9;
				code->SetFunc(reg == 0
				                  ? program->tls.handler_vaddr
				                  : program->tls.handler_vaddr + Jit::TlsRegStub::GetOffset(reg));
				ptr += prefix_count + Jit::Call9::GetSize() - 1;
			}
		}
	}

	// ps5-vsh: Sce.Vsh.ShellUI.Legacy.dll.sprx BasePlugin::StartFarsightUI null-dereference guard.
	// In BasePlugin.StartFarsightUI(), it calls GameLayerPlugin.ResumeGameScreen().
	// When booting, GameLayerPlugin has not yet finished OnLoaded(), so its m_appCoordinator
	// field is null. ResumeGameScreen() attempts to call m_appCoordinator.Reboot() via
	// `mov rax, [rdi]; call 0xd690` at RVA 0x36235, which faults with NullReferenceException
	// and aborts the entire ShellUI process with PsmFramework::Run() ret=0x1.
	// Nop out the 8-byte invocation (mov rax, [rdi]; call 0xd690) so StartFarsightUI continues
	// directly to ForegroundUserManager.DoFirstLogin(...) and event watch dispatch.
	// ps5-vsh: default ON with opt-out via KYTY_VSH_SKIP_FARSIGHT_NULL=0.
	if (!program->boot_credential_admission && !Ps5VshLoadPatchOptedOut("KYTY_VSH_SKIP_FARSIGHT_NULL") &&
	    program->file_name.filename().string() == "Sce.Vsh.ShellUI.Legacy.dll.sprx") {
		const uint64_t target_vaddr = program->base_vaddr + 0x36235ull;
		if (target_vaddr >= address && target_vaddr + 8ull <= address + size) {
			auto* target_ptr = reinterpret_cast<uint8_t*>(target_vaddr);
			const uint8_t expected[8] = {0x48, 0x8b, 0x07, 0xe8, 0x53, 0x74, 0xfd, 0xff};
			if (std::memcmp(target_ptr, expected, sizeof(expected)) == 0) {
				std::memset(target_ptr, 0x90, sizeof(expected));
				LOGF("ps5-vsh: patched Sce.Vsh.ShellUI.Legacy.dll.sprx StartFarsightUI null-deref at [%016llx]\n",
				     static_cast<unsigned long long>(target_vaddr));
			}
		}
	}
}

uint64_t RuntimeLinker::GetEntry() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		if (p->elf != nullptr && !p->elf->IsShared()) {
			return p->elf->GetEntry() + p->base_vaddr;
		}
	}
	return 0;
}

uint64_t RuntimeLinker::GetProcParam() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);
	if (const auto* current = GetCurrentProgram(); current != nullptr && current->application_owner != nullptr) {
		return current->application_owner->proc_param_vaddr;
	}

	for (const auto* p: m_programs) {
		if (p->elf != nullptr && !p->elf->IsShared()) {
			return p->proc_param_vaddr;
		}
	}
	return 0;
}

void RuntimeLinker::DbgDump(const std::string& folder) {
	KYTY_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		auto folder_str = Common::FixDirectorySlash(folder);
		folder_str += Common::FilenameWithoutDirectory(Common::PathToGenericString(p->file_name));

		EXIT_IF(p->elf == nullptr);

		p->elf->DbgDump(folder_str);

		if (p->dynamic_info != nullptr) {
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table_entry_size != 0 &&
			                     p->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
			EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table_entry_size != 0 &&
			                     p->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->jmprela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->rela_table == nullptr);
			// EXIT_NOT_IMPLEMENTED(p->dynamic_info->symbol_table == nullptr);

			if (p->dynamic_info->symbol_table != nullptr) {
				DbgDumpSymbols(folder_str, p->dynamic_info->symbol_table,
				               p->dynamic_info->symbol_table_total_size,
				               p->dynamic_info->str_table);
			}
			if (p->dynamic_info->jmprela_table != nullptr) {
				DbgDumpRela(folder_str, p->dynamic_info->jmprela_table,
				            p->dynamic_info->jmprela_table_size, p->dynamic_info->str_table,
				            "jmprela_table.txt");
			}
			if (p->dynamic_info->rela_table != nullptr) {
				DbgDumpRela(folder_str, p->dynamic_info->rela_table,
				            p->dynamic_info->rela_table_total_size, p->dynamic_info->str_table,
				            "rela_table.txt");
			}
		}

		if (p->export_symbols != nullptr) {
			p->export_symbols->DbgDump(folder_str, "export_symbols.txt");
		}
		if (p->import_symbols != nullptr) {
			p->import_symbols->DbgDump(folder_str, "import_symbols.txt");
		}
	}
}

void RuntimeLinker::RelocateAll() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);
	CheckStartupMutation();
	if (NativeSslSelected() && !m_ssl_smoke_prepared) { EXIT("Native SSL selection must precede relocation\n"); }

	for (auto* p: m_programs) {
		Relocate(p);
	}
}

std::vector<std::string> RuntimeLinker::GetUnresolvedApplicationImports(const Program* owner) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	std::vector<std::string> missing;
	if (owner == nullptr || owner->application_owner != owner) { return missing; }
	for (auto* program: m_programs) {
		if (program->application_owner != owner || program->dynamic_info == nullptr) { continue; }
		ForEachRelocation(program, [&](Elf64_Rela* r, uint32_t, bool) {
			const auto info = GetRelocationInfo(r, program);
			if (info.bind == BindType::Global && !info.resolved) {
				const auto& symbols = *program->dynamic_info;
				missing.push_back(Common::PathToString(program->file_name.filename()) + ": " +
				                  (symbols.str_table + symbols.symbol_table[r->GetSymbol()].st_name));
			}
		});
	}
	std::sort(missing.begin(), missing.end());
	missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
	return missing;
}

static bool IsNativeKernelProvider(const Program* program) {
	const auto kernel_name = [](const std::string& name) { return Common::ToLower(name).starts_with("libkernel"); };
	if (kernel_name(program->file_name.filename().string())) { return true; }
	if (!program->dynamic_info) { return false; }
	for (const auto& library: program->dynamic_info->export_libs) { if (kernel_name(library.name)) { return true; } }
	for (const auto& module: program->dynamic_info->export_modules) { if (kernel_name(module.name)) { return true; } }
	return false;
}

bool RuntimeLinker::IsApplicationExecutableAddress(const Program* owner, uint64_t address) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	if (!owner || owner->application_owner != owner || !address) { return false; }
	for (const auto* program: m_programs) {
		if (program->application_owner != owner || !program->elf || !program->elf->IsValid() ||
		    IsNativeKernelProvider(program) || address < program->base_vaddr ||
		    address - program->base_vaddr >= program->base_size) { continue; }
		const auto offset = address - program->base_vaddr;
		const auto* segments = program->elf->GetPhdr();
		for (unsigned i = 0; i != program->elf->GetEhdr()->e_phnum; ++i) {
			const auto& segment = segments[i];
			if (segment.p_type == PT_LOAD && (segment.p_flags & PF_X) && segment.p_vaddr <= program->base_size &&
			    segment.p_memsz <= program->base_size - segment.p_vaddr && offset >= segment.p_vaddr &&
			    offset - segment.p_vaddr < segment.p_memsz) { return true; }
		}
	}
	return false;
}

bool RuntimeLinker::PublishApplicationStopText(const Program* owner) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	const auto execution = GetApplicationExecution(owner);
	if (!owner || owner->application_owner != owner || !execution) { return false; }
	auto text = std::make_shared<GuestStopText>();
	text->owner = execution->Owner();
	for (const auto* program: m_programs) {
		if (program->application_owner != owner || !program->elf || !program->elf->IsValid()) { continue; }
		if (IsNativeKernelProvider(program)) { return false; }
		const auto* header = program->elf->GetEhdr();
		const auto* segments = program->elf->GetPhdr();
		for (unsigned i = 0; i != header->e_phnum; ++i) {
			const auto& segment = segments[i];
			if (segment.p_type != PT_LOAD || !(segment.p_flags & PF_X) || !segment.p_memsz) { continue; }
			if (!program->base_vaddr || segment.p_vaddr > program->base_size ||
			    segment.p_memsz > program->base_size - segment.p_vaddr ||
			    program->base_vaddr > UINT64_MAX - segment.p_vaddr ||
			    program->base_vaddr + segment.p_vaddr > UINT64_MAX - segment.p_memsz) { return false; }
			const auto begin = program->base_vaddr + segment.p_vaddr;
			text->ranges.push_back({begin, begin + segment.p_memsz});
		}
	}
	// Private module unload retains its mappings. Publish a fresh immutable copy
	// before entry/late initializers; stoppers read it without loader locks.
	return execution->PublishStopText(std::move(text));
}

std::vector<std::string> RuntimeLinker::GetApplicationHardImportBlockers(const Program* owner) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	std::vector<std::string> blockers;
	if (!owner || owner->application_owner != owner) { return {"invalid application owner"}; }
	const auto execution = GetApplicationExecution(owner);
	if (!execution || execution->StopRequested()) { return {"application execution admission is closed"}; }
	for (auto* program: m_programs) {
		if (program->application_owner != owner || !program->dynamic_info) { continue; }
		const auto reject = [&](const std::string& reason) {
			blockers.push_back(Common::PathToString(program->file_name.filename()) + ": " + reason);
		};
		if (IsNativeKernelProvider(program)) { reject("native kernel provider is unsupported"); }
		ForEachRelocation(program, [&](Elf64_Rela* r, uint32_t, bool) {
			const auto type = r->GetType();
			if (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT &&
			    type != R_X86_64_RELATIVE && type != R_X86_64_DTPMOD64) {
				reject("unsupported relocation type"); return;
			}
			if (r->r_offset > program->base_size || sizeof(uint64_t) > program->base_size - r->r_offset) {
				reject("relocation slot outside private mapping"); return;
			}
			if (type == R_X86_64_RELATIVE || type == R_X86_64_DTPMOD64) { return; }
			const auto& sym = program->dynamic_info->symbol_table[r->GetSymbol()];
			if (sym.GetType() != STT_FUNC && sym.GetType() != STT_OBJECT) { reject("untyped relocation"); return; }
			if (sym.GetBind() == STB_LOCAL) { return; }
			if (sym.GetBind() != STB_GLOBAL && sym.GetBind() != STB_WEAK) { reject("unsupported symbol binding"); return; }
			const std::string name = program->dynamic_info->str_table + sym.st_name;
			const auto slot = program->base_vaddr + r->r_offset;
			if (sym.GetType() == STT_FUNC) {
				uint64_t value = 0;
				if (type == R_X86_64_64 && r->r_addend != 0) { reject(name + ": function addend is unsupported"); return; }
				if (!Libs::Graphics::HostMemoryRangeIsReadable(slot, sizeof(value))) { reject(name + ": unreadable import slot"); return; }
				std::memcpy(&value, reinterpret_cast<const void*>(slot), sizeof(value));
				if (!program->application_import_bindings || !program->application_import_bindings->HasPatchedEntry(slot, name, value)) {
					reject(name + ": immutable function entry not patched");
				}
				return;
			}
			const auto info = GetRelocationInfo(r, program);
			const auto* provider = info.resolved ? FindProgramByAddr(info.value) : nullptr;
			const bool private_data = info.application_data && owner->application_import_data &&
			    owner->application_import_data->ContainsObjectAddress(execution->Owner(), info.value);
			if (!private_data && (!provider || provider->application_owner != owner)) { reject(name + ": data provider is not private to owner"); }
			uint64_t value = 0;
			if (!Libs::Graphics::HostMemoryRangeIsReadable(slot, sizeof(value))) { reject(name + ": unreadable data slot"); return; }
			std::memcpy(&value, reinterpret_cast<const void*>(slot), sizeof(value));
			if (value != info.value) { reject(name + ": data relocation not patched"); }
		});
	}
	std::sort(blockers.begin(), blockers.end());
	blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
	return blockers;
}

std::vector<std::string> RuntimeLinker::GetApplicationImportBlockers(const Program* owner) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	std::vector<std::string> blockers;
	if (!owner || owner->application_owner != owner) { return {"invalid application owner"}; }
	const auto execution = GetApplicationExecution(owner);
	if (!execution || execution->StopRequested()) { return {"application execution admission is closed"}; }
	for (auto* program: m_programs) {
		if (program->application_owner != owner || !program->dynamic_info) { continue; }
		if (program->application_import_bindings) {
			for (auto& reason: program->application_import_bindings->AdmissionBlockers()) {
				blockers.push_back(Common::PathToString(program->file_name.filename()) + ": " + reason);
			}
		}
		ForEachRelocation(program, [&](Elf64_Rela* r, uint32_t, bool) {
			const auto info = GetRelocationInfo(r, program);
			if (info.bind != BindType::Global && info.bind != BindType::Weak) { return; }
			if (info.type == SymbolType::NoType || (!info.resolved && info.type != SymbolType::Func) ||
			    (info.type == SymbolType::Func && r->GetType() == R_X86_64_64 && r->r_addend != 0)) {
				blockers.push_back(Common::PathToString(program->file_name.filename()) + ": " + info.name +
				                   ": unsupported deferred relocation ABI");
			}
		});
	}
	std::sort(blockers.begin(), blockers.end());
	blockers.erase(std::unique(blockers.begin(), blockers.end()), blockers.end());
	return blockers;
}

void RuntimeLinker::RelocateProgram(Program* program) {
	Common::LockGuard lock(m_mutex);
	CheckStartupMutation();
	if (NativeSslSelected() && !m_ssl_smoke_prepared) { EXIT("Native SSL selection must precede relocation\n"); }

	EXIT_IF(program == nullptr);
	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	// ps5-vsh: a late-loaded firmware libSceComposite supersedes the Composite HLE
	// (see Resolve). Imports already bound to HLE are re-resolved against it.
	const auto& new_exports = program->dynamic_info->export_libs;
	if (std::any_of(new_exports.begin(), new_exports.end(),
	                [](const LibraryId& lib) { return lib.name == "libSceComposite"; })) {
		uint32_t rebound = 0;
		for (auto* importer: m_programs) {
			if (importer == program || importer->dynamic_info == nullptr) {
				continue;
			}
			if (program->application_owner != nullptr &&
			    importer->application_owner != program->application_owner) { continue; }
			const auto& info       = *importer->dynamic_info;
			const auto  rela_count = static_cast<uint32_t>(info.rela_table_total_size / sizeof(Elf64_Rela));
			ForEachRelocation(importer, [&](Elf64_Rela* r, uint32_t index, bool jmprela) {
				const auto type = r->GetType();
				if (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT) {
					return;
				}
				const auto& symbol = info.symbol_table[r->GetSymbol()];
				if (symbol.GetBind() != STB_GLOBAL && symbol.GetBind() != STB_WEAK) {
					return;
				}
				const auto ids = Common::Split(std::string(info.str_table + symbol.st_name), '#');
				if (ids.size() != 3) {
					return;
				}
				const auto* lib = FindLibrary(*importer, ids.at(1));
				if (lib == nullptr || lib->name != "libSceComposite") {
					return;
				}
				const auto bit_index = (jmprela ? rela_count : 0) + index;
				importer->rela_bits[bit_index >> 3u] &= static_cast<uint8_t>(~(1u << (bit_index & 7u)));
				rebound++;
			});
		}
		LOGF("ps5-vsh: libSceComposite loaded; %u Composite imports queued for rebinding\n", rebound);
	}

	if (program->application_owner != nullptr) {
		// A private provider may satisfy imports in its own application only.
		// Leave live session/other-application relocation slots untouched.
		for (auto* importer: m_programs) {
			if (importer->application_owner == program->application_owner) { Relocate(importer); }
		}
	} else {
		RelocateAll();
	}
	if (!GamePatch::ApplyPending(program)) {
		EXIT("Failed to apply pending game cheat\n");
	}
}

void RuntimeLinker::UnloadProgram(Program* program) {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);
	CheckStartupMutation();
	if (program && program->boot_credential_admission) {
		LOGF("AppDb: retain admitted boot image until session teardown\n");
		return;
	}
	if (program && program->np_alternate54 && Libs::NpNativeTransport::Installed()) {
		LOGF("NP: retain audited provider until transport retirement\n");
		return;
	}
	if (program && program->application_owner) {
		// Import entries and already copied native function pointers can still
		// reference any private provider. Removing only its GOT slots cannot
		// establish quiescence; shared mappings stay pinned for owner retirement.
		LOGF("VSH runtime: retain private module %d pending owner-safe reclamation\n", program->unique_id);
		return;
	}
	if (m_ssl_smoke_prepared && (m_ssl_smoke_enabled || program == m_ssl_smoke_libc || program == m_ssl_smoke_ssl)) {
		EXIT("Native SSL providers are pinned until process exit\n");
	}

	auto it = std::find(m_programs.begin(), m_programs.end(), program);
	EXIT_IF(it == m_programs.end());
	EXIT_IF(m_native_libc_startup.InvalidateProvider(program->unique_id) == NativeLibcStartup::Result::Invoking);
	m_programs.erase(it);

	for (auto* importer: m_programs) {
		const auto& info = *importer->dynamic_info;
		const auto rela_count =
		    static_cast<uint32_t>(info.rela_table_total_size / sizeof(Elf64_Rela));
		ForEachRelocation(importer, [&](Elf64_Rela* r, uint32_t index, bool jmprela) {
			const auto bit_index = (jmprela ? rela_count : 0) + index;
			auto&      bits      = importer->rela_bits[bit_index >> 3u];
			const auto mask      = 1u << (bit_index & 7u);
			const auto type      = r->GetType();
			if ((bits & mask) == 0 || (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT &&
			                           type != R_X86_64_JUMP_SLOT)) {
				return;
			}
			const auto& symbol = info.symbol_table[r->GetSymbol()];
			if (symbol.GetBind() != STB_GLOBAL && symbol.GetBind() != STB_WEAK) {
				return;
			}
			const auto value =
			    *reinterpret_cast<const uint64_t*>(importer->base_vaddr + r->r_offset);
			const auto address = value - (type == R_X86_64_64 ? r->r_addend : 0);
			if (address < program->base_vaddr ||
			    address >= program->base_vaddr + program->mapped_size) {
				return;
			}
			SymbolType symbol_type;
			switch (symbol.GetType()) {
				case STT_NOTYPE: symbol_type = SymbolType::NoType; break;
				case STT_FUNC: symbol_type = SymbolType::Func; break;
				case STT_OBJECT: symbol_type = SymbolType::Object; break;
				default: return;
			}
			const std::string name = info.str_table + symbol.st_name;
			if (program->export_symbols->FindByNid(name.substr(0, name.find('#')), symbol_type,
			                                       address) != nullptr) {
				bits &= ~mask;
				if (RelocateRecord(index, r, importer, jmprela)) {
					bits |= mask;
				}
			}
		});
	}
	DeleteProgram(program);
}

namespace {
constexpr uint8_t NativeLibcImageHash[32] {
	0x8f, 0xa8, 0x38, 0x79, 0x10, 0x61, 0xe1, 0x9c, 0xa8, 0x1d, 0xdd, 0x49, 0x12, 0x79, 0x8b, 0xfd,
	0x1d, 0x07, 0x4f, 0x81, 0x27, 0xce, 0x8c, 0x5f, 0xb2, 0x5b, 0xb7, 0x08, 0xea, 0xb6, 0x69, 0x35,
};

bool NativeLibcRawExport(const DynamicInfo& info, std::string_view nid, uint64_t rva, uint64_t size) {
	if (!info.symbol_table || !info.str_table || info.symbol_table_entry_size != sizeof(Elf64_Sym) ||
	    info.symbol_table_total_size % sizeof(Elf64_Sym) != 0) { return false; }
	size_t matches = 0;
	for (size_t i = 0; i < info.symbol_table_total_size / sizeof(Elf64_Sym); ++i) {
		const auto& sym = info.symbol_table[i];
		if (sym.st_name >= info.str_table_size) { return false; }
		const char* name = info.str_table + sym.st_name;
		const auto* end = static_cast<const char*>(std::memchr(name, 0, info.str_table_size - sym.st_name));
		if (!end) { return false; }
		const std::string_view raw(name, end - name);
		if (raw.substr(0, raw.find('#')) != nid || sym.st_value == 0) { continue; }
		// Count raw definitions, not the normalized last-wins symbol dictionary.
		if (++matches != 1 || raw != std::string(nid) + "#C#A" || sym.GetBind() != STB_GLOBAL ||
		    sym.GetType() != STT_FUNC || sym.st_shndx == 0 || sym.st_value != rva || sym.st_size != size) { return false; }
	}
	const auto libs = std::count_if(info.export_libs.begin(), info.export_libs.end(), [](const auto& lib) {
		return lib.id == "C";
	});
	const auto mods = std::count_if(info.export_modules.begin(), info.export_modules.end(), [](const auto& mod) {
		return mod.id == "A";
	});
	return matches == 1 && libs == 1 && mods == 1 &&
	    std::any_of(info.export_libs.begin(), info.export_libs.end(), [](const auto& lib) {
		    return lib.id == "C" && lib.name == "libSceLibcInternal" && lib.version == 1;
	    }) && std::any_of(info.export_modules.begin(), info.export_modules.end(), [](const auto& mod) {
		    return mod.id == "A" && mod.name == "libSceLibcInternal" && mod.version_major == 1 && mod.version_minor == 1;
	    });
}

bool NativeLibcSegment(const Program& program, uint64_t rva, uint64_t size, uint32_t flags) {
	if (rva > program.base_size || size > program.base_size - rva ||
	    program.base_size > program.mapped_size || program.base_vaddr > UINT64_MAX - program.mapped_size) { return false; }
	const auto* ph = program.elf->GetPhdr();
	for (size_t i = 0; i < program.elf->GetEhdr()->e_phnum; ++i) {
		if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & flags) == flags && rva >= ph[i].p_vaddr &&
		    rva - ph[i].p_vaddr <= ph[i].p_memsz && size <= ph[i].p_memsz - (rva - ph[i].p_vaddr)) { return true; }
	}
	return false;
}

bool NativeLibcFatalProfile(const Program& program) {
	if (!program.elf || !program.elf->IsShared() || !program.dynamic_info || !program.export_symbols ||
	    program.file_name.filename() != "libSceLibcInternal.sprx" ||
	    program.export_symbols->GetExportPolicy() != SymbolDatabase::ExportPolicy::All ||
	    !NativeLibcRawExport(*program.dynamic_info, "Vla-Z+eXlxo", 0x12eb0, 0xfe) ||
	    !NativeLibcSegment(program, 0x12eb0, 0xfe, PF_X)) { return false; }
	for (const auto& range: NativeAllocatorFatal::ProfileRanges) {
		if (!NativeLibcSegment(program, range.rva, range.size, PF_X)) { return false; }
	}
	if (!program.elf->MatchesFileSha256(NativeLibcImageHash, 4 * 1024 * 1024)) { return false; }
	const auto* free = program.export_symbols->Find(
	    {"Vla-Z+eXlxo", "libSceLibcInternal", 1, "libSceLibcInternal", 1, 1, SymbolType::Func});
	return free && free->vaddr == program.base_vaddr + 0x12eb0 && NativeAllocatorFatal::MatchProfile(
	    {reinterpret_cast<const uint8_t*>(program.base_vaddr), NativeAllocatorFatal::ProfileExtent});
}
} // namespace

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestNativeLibcRawExport(const DynamicInfo& info) {
	return NativeLibcRawExport(info, "h8jq9ee4h5c", NativeLibcStartup::LeafRva, NativeLibcStartup::LeafSize);
}
#endif

void RuntimeLinker::CheckStartupMutation() {
	if (m_ssl_smoke_frozen) { EXIT("Loader mutation attempted during native SSL initialization/isolation\n"); }
	if (NativeSslSelected() && !m_ssl_production_ready && m_primary_startup_entered &&
	    m_ssl_smoke_thread != static_cast<uint64_t>(Common::Thread::GetThreadIdUnique())) {
		EXIT("Concurrent loader mutation rejected during native SSL preparation\n");
	}
	if (m_native_libc_startup.GetPhase() == NativeLibcStartup::Phase::Invoking) {
		EXIT("Loader mutation attempted during native Libc startup\n");
	}
}

void RuntimeLinker::AdmitGuestStartup(bool smoke_initializer) {
	const auto np = NpAlternateStartup::Selected();
	const bool has_alternate = std::any_of(m_programs.begin(), m_programs.end(), [](const auto* p) { return p->np_alternate54; });
	if (np == NpAlternateStartup::Selection::Invalid ||
	    (np == NpAlternateStartup::Selection::Alternate54) != has_alternate) {
		EXIT("NP alternate startup configuration/provider mismatch\n");
	}
	if (m_ssl_smoke_enabled && !smoke_initializer) { EXIT("Unrelated guest startup rejected in isolated native SSL smoke\n"); }
	if (m_ssl_production_enabled && !m_ssl_production_ready && !smoke_initializer) {
		EXIT("Guest startup attempted before native SSL provider initialization\n");
	}
	if (m_native_libc_startup.Enabled() && m_native_libc_startup.GetPhase() != NativeLibcStartup::Phase::Completed) {
		EXIT("Guest startup attempted before native Libc startup completed; fresh session required\n");
	}
	EXIT_IF(m_native_libc_startup.RecordStartupAdmission() == NativeLibcStartup::Result::Invoking);
}

bool RuntimeLinker::ValidateNativeLibcStartup(Program* program) {
	if (!program || !NativeLibcFatalProfile(*program) ||
	    !NativeLibcRawExport(*program->dynamic_info, "h8jq9ee4h5c", NativeLibcStartup::LeafRva, NativeLibcStartup::LeafSize) ||
	    !NativeLibcSegment(*program, NativeLibcStartup::LeafRva, NativeLibcStartup::LeafSize, PF_X) ||
	    !NativeLibcSegment(*program, 0x1b7cc0, 0x2c, PF_W)) { return false; }
	const auto base = program->base_vaddr;
	const auto* bytes = reinterpret_cast<const uint8_t*>(base);
	const auto* entry = program->export_symbols->Find(
	    {"h8jq9ee4h5c", "libSceLibcInternal", 1, "libSceLibcInternal", 1, 1, SymbolType::Func});
	return entry && entry->vaddr == base + NativeLibcStartup::LeafRva &&
	    NativeLibcStartup::MatchesLeafProfile({bytes + NativeLibcStartup::LeafRva, NativeLibcStartup::LeafSize}, 0) &&
	    NativeAllocatorFatal::ContainsProfile(base);
}

void RuntimeLinker::BindNativeLibcStartup(Program* program) {
	if (!m_native_libc_startup.Enabled()) { return; }
	for (const auto& lib: program->dynamic_info->export_libs) {
		if (Common::ToLower(lib.name).starts_with("libkernel")) {
			EXIT("Native Libc startup bridge cannot coexist with a native libkernel startup provider\n");
		}
	}
	const bool candidate = program->file_name.filename() == "libSceLibcInternal.sprx" ||
	    std::any_of(program->dynamic_info->export_libs.begin(), program->dynamic_info->export_libs.end(), [](const auto& lib) {
		    return Common::ToLower(lib.name).find("libcinternal") != std::string::npos;
	    });
	if (!candidate) { return; }
	if (!ValidateNativeLibcStartup(program)) { EXIT("Native Libc startup provider identity/profile rejected\n"); }
	const auto result = m_native_libc_startup.Bind(program->unique_id,
	    reinterpret_cast<NativeLibcStartup::Entry>(program->base_vaddr + NativeLibcStartup::LeafRva), true);
	if (result != NativeLibcStartup::Result::Ok) { EXIT("Native Libc startup binding rejected (%u)\n", unsigned(result)); }
	LOGF("Native Libc startup bound: owner=%d base=0x%016" PRIx64 "\n", program->unique_id, program->base_vaddr);
}

void RuntimeLinker::RunNativeLibcStartup() {
	NativeLibcStartup::PreparedCall call;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		if (!m_native_libc_startup.Enabled()) { return; }
		Program* provider = nullptr;
		for (auto* program: m_programs) {
			if (program->started) { EXIT("Native Libc startup found an already admitted initializer\n"); }
			if (program->unique_id == m_native_libc_startup.Owner()) { provider = program; }
		}
		// Validate again after relocation and game patches, immediately before Prepare.
		if (!ValidateNativeLibcStartup(provider)) { EXIT("Native Libc startup final provider validation failed\n"); }
		const auto result = m_native_libc_startup.Prepare(call);
		if (result != NativeLibcStartup::Result::Ok) { EXIT("Native Libc startup preparation rejected (%u)\n", unsigned(result)); }
	}
	// The audited leaf runs without the loader lock. Invoking closes publication,
	// relocation, initialization and unmap admission until normal return.
	EXIT_IF(call.Invoke() != NativeLibcStartup::Result::Ok);
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		EXIT_IF(m_native_libc_startup.Complete(call) != NativeLibcStartup::Result::Ok);
		LOGF("Native Libc startup completed: owner=%d before module initializers\n", m_native_libc_startup.Owner());
	}
}

void RuntimeLinker::PrepareNativeSslSmoke() {
	Common::LockGuard lock(m_mutex);
	if (m_ssl_smoke_prepared || m_ssl_smoke_frozen) { EXIT("Native SSL preparation cannot be replayed\n"); }
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	EXIT("Native SSL smoke bounded-read admission currently requires Windows\n");
#endif
	auto select = [&](const char* library, const char* filename) {
		Program* found = nullptr;
		for (auto* p: m_programs) {
			if (p->started || p->initializer_returned ||
			    std::any_of(p->rela_bits.begin(), p->rela_bits.end(), [](auto b) { return b != 0; })) {
				EXIT("Native SSL smoke requires a fresh, unrelocated session\n");
			}
			if (Common::EqualNoCase(p->file_name.filename().string(), filename) ||
			    std::any_of(p->dynamic_info->export_libs.begin(), p->dynamic_info->export_libs.end(), [&](const auto& l) {
				    return Common::EqualNoCase(l.name, library);
			    })) {
				if (found) { EXIT("Ambiguous native SSL smoke provider: %s\n", library); }
				found = p;
			}
		}
		if (!found) {
			const auto path = Libs::LibKernel::FileSystem::GetRealFilename(std::string("/common/lib/") + filename);
			if (path.empty() || !Common::File::IsFileExisting(path)) { EXIT("Native SSL smoke provider missing: %s\n", filename); }
			found = LoadProgram(path);
		}
		return found;
	};
	m_ssl_smoke_libc = select("libSceLibcInternal", "libSceLibcInternal.sprx");
	m_ssl_smoke_ssl = select("libSceSsl", "libSceSsl.sprx");
	m_ssl_smoke_libc->native_ssl_smoke_dependency = true;
	m_ssl_smoke_ssl->native_ssl_smoke_dependency = true;
	ValidateNativeSslSmoke(false);
	m_ssl_smoke_prepared = true;
}

bool RuntimeLinker::ResolveNativeSslSmoke(const std::string& name, SymbolType type, Program* importer, SymbolRecord* out) {
	if (!NativeSslSelected() || !importer || !importer->native_ssl_smoke_dependency) { return false; }
	const auto ids = Common::Split(name, '#');
	if (ids.empty()) { return false; }
	const NativeSslSmoke::Export* native = nullptr;
	const NativeSslSmoke::HostContract* host = nullptr;
	if (importer == m_ssl_smoke_ssl) {
		for (const auto& e: NativeSslSmoke::LibcExports) { if (ids[0] == e.nid) { native = &e; break; } }
	}
	for (const auto& h: NativeSslSmoke::HostContracts()) { if (ids[0] == h.nid) { host = &h; break; } }
	if (!native && !host) { return false; }
	if (ids.size() != 3 || type != (native ? native->type : host->type)) { EXIT("Native SSL required import type/identity rejected: %s\n", name.c_str()); }
	const auto& info = *importer->dynamic_info;
	const char* library = native ? "libSceLibcInternal" : "libkernel";
	const auto lib_count = std::count_if(info.import_libs.begin(), info.import_libs.end(), [&](const auto& l) { return l.id == ids[1]; });
	const auto mod_count = std::count_if(info.import_modules.begin(), info.import_modules.end(), [&](const auto& m) { return m.id == ids[2]; });
	if (lib_count != 1 || mod_count != 1 ||
	    !std::any_of(info.import_libs.begin(), info.import_libs.end(), [&](const auto& l) {
		    return l.id == ids[1] && l.name == library && l.version == 1;
	    }) || !std::any_of(info.import_modules.begin(), info.import_modules.end(), [&](const auto& m) {
		    return m.id == ids[2] && m.name == library && m.version_major == 1 && m.version_minor == 1;
	    })) { EXIT("Native SSL required import version/owner rejected: %s\n", name.c_str()); }
	const SymbolResolve query {ids[0], library, 1, library, 1, 1, type};
	const auto* record = native ? m_ssl_smoke_libc->export_symbols->Find(query) : m_symbols->FindRegistration(query, host->registration);
	if (!record || (native && record->vaddr != m_ssl_smoke_libc->base_vaddr + native->rva) ||
	    (host && !NativeSslSmoke::HostMatches(record, *host))) { EXIT("Native SSL required binding rejected: %s\n", name.c_str()); }
	*out = *record;
	return true;
}

bool RuntimeLinker::IsNativeSslAddress(uint64_t address) const {
	return m_ssl_production_enabled && m_ssl_smoke_ssl && address >= m_ssl_smoke_ssl->base_vaddr &&
	    address - m_ssl_smoke_ssl->base_vaddr < m_ssl_smoke_ssl->mapped_size;
}

bool RuntimeLinker::ResolveNativeSslProduction(const std::string& name, SymbolType type, Program* importer, SymbolRecord* out) {
	if (!m_ssl_production_enabled || !importer) { return false; }
	SymbolResolve query {};
	auto policy = NativeSslSmoke::ProductionImport(*importer->dynamic_info, name, type, query);
	// Own definitions use S's audited export IDs, never another module's import metadata.
	const auto ids = Common::Split(name, '#');
	if (importer == m_ssl_smoke_ssl && ids.size() == 3 && ids[1] == "G" && ids[2] == "A") {
		query = {ids[0], "libSceSsl", 1, "libSceSsl", 2, 1, type};
		policy = NativeSslSmoke::PublicExportIdentity(query) ? NativeSslSmoke::ImportPolicy::Accepted : NativeSslSmoke::ImportPolicy::Rejected;
	}
	if (policy == NativeSslSmoke::ImportPolicy::Unrelated) { return false; }
	if (policy != NativeSslSmoke::ImportPolicy::Accepted || !m_ssl_smoke_ssl) {
		EXIT("Native SSL import identity/version rejected: %s (%s)\n", name.c_str(), Common::PathToString(importer->file_name).c_str());
	}
	const auto* record = NativeSslSmoke::FindPublicExport(*m_ssl_smoke_ssl->export_symbols, m_ssl_smoke_ssl->base_vaddr, query);
	if (!record) {
		EXIT("Selected native SSL export missing: %s; HLE fallback prohibited\n", name.c_str());
	}
	*out = *record;
	return true;
}

bool RuntimeLinker::ResolveNativeSslDlsym(Program* handle, std::string_view symbol, std::string_view nid, uint64_t* address) {
	if (!m_ssl_production_enabled) { return false; }
	Common::LockGuard lock(m_mutex);
	const auto* entry = NativeSslSmoke::PublicExport(nid);
	if (!entry) { entry = NativeSslSmoke::PublicExport(symbol); }
	if (handle != m_ssl_smoke_ssl && !entry && !symbol.starts_with("sceSsl") && !symbol.starts_with("_sceSsl")) { return false; }
	*address = 0;
	if (!m_ssl_production_ready) { EXIT("Native SSL dlsym before provider initialization\n"); }
	if (entry) {
		const auto* record = NativeSslSmoke::FindPublicExport(*m_ssl_smoke_ssl->export_symbols, m_ssl_smoke_ssl->base_vaddr,
		    {entry->nid, "libSceSsl", 1, "libSceSsl", 2, 1, entry->type});
		if (!record) { EXIT("Native SSL dlsym provider mismatch\n"); }
		*address = record->vaddr;
	}
	return true;
}

void RuntimeLinker::ValidateNativeSslSmoke(bool relocated) {
	using namespace NativeSslSmoke;
	if (!m_ssl_smoke_libc || !m_ssl_smoke_ssl || !ValidateNativeLibcStartup(m_ssl_smoke_libc) ||
	    m_ssl_smoke_libc->unique_id != m_native_libc_startup.Owner()) { EXIT("Native SSL libc provider/bridge identity rejected\n"); }
	for (auto* provider: {m_ssl_smoke_libc, m_ssl_smoke_ssl}) {
		const bool ssl = provider == m_ssl_smoke_ssl;
		const auto* library = ssl ? "libSceSsl" : "libSceLibcInternal";
		for (auto* other: m_programs) {
			if (other->started || other->initializer_returned) { EXIT("Native SSL smoke found prior initializer admission\n"); }
			if (other != provider && std::any_of(other->dynamic_info->export_libs.begin(), other->dynamic_info->export_libs.end(), [&](const auto& l) {
				return Common::EqualNoCase(l.name, library);
			})) { EXIT("Native SSL duplicate provider publication\n"); }
		}
		if (!provider->elf->IsShared() || provider->dynamic_info->init_vaddr != (ssl ? 0x20u : 0x10u) ||
		    !NativeLibcSegment(*provider, provider->dynamic_info->init_vaddr, 0x100, PF_X)) {
			EXIT("Native SSL DT_INIT identity/bounds rejected\n");
		}
		const std::span<const Export> exports = ssl ? (m_ssl_production_enabled ? std::span<const Export>(SslPublicExports) :
		    std::span<const Export>(SslExports)) : std::span<const Export>(LibcExports);
		for (const auto& e: exports) {
			if (!RawExportMatches(*provider->dynamic_info, e, ssl) ||
			    !NativeLibcSegment(*provider, e.rva, e.size, e.type == SymbolType::Func ? PF_X : PF_R | PF_W)) {
				EXIT("Native SSL raw export/profile rejected: %s\n", e.nid);
			}
		}
	}
	if (m_ssl_smoke_ssl->file_name.filename() != "libSceSsl.sprx" ||
	    m_ssl_smoke_ssl->export_symbols->GetExportPolicy() != (m_ssl_production_enabled ? SymbolDatabase::ExportPolicy::NativeSsl : SymbolDatabase::ExportPolicy::PrivateSslSmoke) ||
	    !m_ssl_smoke_ssl->elf->MatchesFileSha256(SslHash, 4 * 1024 * 1024) ||
	    !NativeLibcSegment(*m_ssl_smoke_ssl, 0x169178, 0x20, PF_R | PF_W) ||
	    !NativeLibcSegment(*m_ssl_smoke_ssl, 0x16a060, 8, PF_R | PF_W) ||
	    !NativeLibcSegment(*m_ssl_smoke_ssl, 0x16a458, 8, PF_R | PF_W) ||
	    !NativeLibcSegment(*m_ssl_smoke_libc, 0x1b42b8, 0x1c8, PF_R | PF_W) ||
	    !NativeLibcSegment(*m_ssl_smoke_libc, 0x1b72b0, 0x300, PF_R | PF_W)) { EXIT("Native SSL image/data profile rejected\n"); }
	for (const auto& h: HostContracts()) {
		const auto* record = m_symbols->FindRegistration({h.nid, "libkernel", 1, "libkernel", 1, 1, h.type}, h.registration);
		if (!HostMatches(record, h)) { EXIT("Native SSL host contract missing/conflicting: %s (%s)\n", h.nid, h.registration); }
		LOGF("Native SSL host contract: %s -> %s @ %016" PRIx64 "\n", h.nid, h.registration, record->vaddr);
	}
	// Both non-PLT and PLT slots are checked, not merely the resolver's claimed result.
	std::array<unsigned, std::size(LibcExports)> seen {};
	for (auto* provider: m_programs) {
		if (!m_ssl_production_enabled && !provider->native_ssl_smoke_dependency) { continue; }
		const auto& info = *provider->dynamic_info;
		ForEachRelocation(provider, [&](Elf64_Rela* relocation, uint32_t, bool) {
			const auto kind = relocation->GetType();
			if (kind != R_X86_64_64 && kind != R_X86_64_GLOB_DAT && kind != R_X86_64_JUMP_SLOT) { return; }
			if (relocation->GetSymbol() >= info.symbol_table_total_size / sizeof(Elf64_Sym)) { EXIT("Native SSL relocation symbol outside table\n"); }
			const auto& symbol = info.symbol_table[relocation->GetSymbol()];
			if (symbol.st_name >= info.str_table_size) { EXIT("Native SSL relocation string outside table\n"); }
			const char* name = info.str_table + symbol.st_name;
			if (!std::memchr(name, 0, info.str_table_size - symbol.st_name)) { EXIT("Native SSL unterminated import\n"); }
			const auto type = symbol.GetType() == STT_FUNC ? SymbolType::Func : symbol.GetType() == STT_OBJECT ? SymbolType::Object : SymbolType::NoType;
			SymbolRecord record {};
			if (!ResolveNativeSslProduction(name, type, provider, &record) && !ResolveNativeSslSmoke(name, type, provider, &record)) { return; }
			if (!NativeLibcSegment(*provider, relocation->r_offset, 8, PF_R | PF_W)) { EXIT("Native SSL GOT outside data segment\n"); }
			if (provider == m_ssl_smoke_ssl) {
				const std::string_view raw(name);
				for (size_t i = 0; i < std::size(LibcExports); ++i) {
					if (raw.substr(0, raw.find('#')) == LibcExports[i].nid) { ++seen[i]; }
				}
			}
			if (relocated) {
				const uint64_t expected = record.vaddr + (kind == R_X86_64_64 ? relocation->r_addend : 0);
				uint64_t actual = 0;
				std::memcpy(&actual, reinterpret_cast<const void*>(provider->base_vaddr + relocation->r_offset), sizeof(actual));
				if (actual != expected) { EXIT("Native SSL actual GOT mismatch: %s\n", name); }
				LOGF("Native SSL binding: owner=%d slot=%" PRIx64 " %s -> %016" PRIx64 "\n", provider->unique_id, relocation->r_offset, name, actual);
			}
		});
	}
	if (std::any_of(seen.begin(), seen.end(), [](auto count) { return count == 0; })) { EXIT("Native SSL required native import absent\n"); }
}

namespace {
// One bounded snapshot per stage; Windows fault-safe copies, never a guessed dereference.
bool SslSmokeRead(uint64_t address, void* out, size_t size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (!address || !size || size > 0x100 || address > UINT64_MAX - size) { return false; }
	SIZE_T transferred = 0;
	return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &transferred) && transferred == size;
#else
	return false;
#endif
}
struct SslSmokeRun {
	RuntimeLinker* rt;
	Program* libc;
	Program* ssl;
	bool production = false;
	NativeSslSmoke::Control control {"1"};
	bool initializers_ok = false;
	uint64_t backing = 0;
	uint64_t backing_size = 0;
};
void SslSmokeObserve(void* opaque, NativeSslSmoke::Phase phase, int id) {
	auto& run = *static_cast<SslSmokeRun*>(opaque);
	if (phase == NativeSslSmoke::Phase::Init) {
		LOGF("Native SSL init returned: id=%08x\n", unsigned(id));
		Log::Flush();
		return;
	}
	const bool retired = phase == NativeSslSmoke::Phase::Term;
	if (!retired) {
		const auto& stats = run.control.GetResult();
		LOGF("Native SSL pre-term stats: result=%08x canaries=%d\n", unsigned(stats.stats_result), stats.canaries_ok);
		for (size_t n = 0; n < stats.before.size(); n += 8) {
			uint64_t before, after;
			std::memcpy(&before, stats.before.data() + n, 8); std::memcpy(&after, stats.after.data() + n, 8);
			LOGF("Native SSL canaries+%02zx: before=%016" PRIx64 " after=%016" PRIx64 "\n", n, before, after);
		}
		for (size_t n = 0; n < stats.stats.size(); n += 8) {
			uint64_t word; std::memcpy(&word, stats.stats.data() + n, 8);
			LOGF("Native SSL public stats+%02zx=%016" PRIx64 "\n", n, word);
		}
	}
	uint64_t root = 0;
	const bool root_ok = SslSmokeRead(run.ssl->base_vaddr + 0x169188, &root, sizeof(root));
	LOGF("Native SSL evidence: retired=%d id=%08x root_available=%d root=%016" PRIx64 "\n", retired, unsigned(id), root_ok, root);
	if (!retired && root_ok && root) {
		std::array<uint8_t,0x60> header {};
		const bool header_ok = SslSmokeRead(root, header.data(), header.size());
		int actual_id = 0;
		std::memcpy(&actual_id, header.data(), sizeof(actual_id));
		LOGF("Native SSL context: available=%d id_matches=%d\n", header_ok, header_ok && actual_id == id);
		if (header_ok && actual_id == id) {
			uint64_t heap = 0;
			std::memcpy(&heap, header.data() + 0x10, 8);
			std::memcpy(&run.backing, header.data() + 0x28, 8);
			std::memcpy(&run.backing_size, header.data() + 0x30, 8);
			LOGF("Native SSL backing: base=%016" PRIx64 " size=%" PRIx64 " heap=%016" PRIx64 "\n", run.backing, run.backing_size, heap);
			for (size_t n = 0; n < header.size(); n += 8) {
				uint64_t word; std::memcpy(&word, header.data() + n, 8);
				LOGF("Native SSL context+%02zx=%016" PRIx64 "\n", n, word);
			}
			std::array<uint8_t,0xb0> allocator {};
			const bool heap_ok = heap <= UINT64_MAX - 0x400 && SslSmokeRead(heap + 0x350, allocator.data(), allocator.size());
			LOGF("Native SSL heap header: available=%d\n", heap_ok);
			if (heap_ok) {
				for (size_t n = 0; n < allocator.size(); n += 8) {
					uint64_t word; std::memcpy(&word, allocator.data() + n, 8);
					LOGF("Native SSL heap+%03zx=%016" PRIx64 "\n", n + 0x350, word);
				}
			}
		}
	}
	if (run.backing) {
		Libs::LibKernel::Memory::VirtualQueryInfo query {};
		const auto result = Libs::LibKernel::Memory::KernelVirtualQuery(reinterpret_cast<const void*>(run.backing), 0, &query, sizeof(query));
		LOGF("Native SSL backing query: retired=%d result=%08x available=%d start=%016" PRIxPTR " end=%016" PRIxPTR "\n",
		     retired, unsigned(result), result == 0, query.start, query.end);
	} else {
		LOGF("Native SSL backing query: retired=%d unavailable (no captured backing)\n", retired);
	}
	Log::Flush();
}
static void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SslSmokeGuestEntry(EntryParams* params, atexit_func_t) {
	auto& run = *reinterpret_cast<SslSmokeRun*>(params);
	for (auto* p: {run.libc, run.ssl}) {
		const int result = run.rt->StartModule(p, 0, nullptr, nullptr);
		LOGF("Native SSL initializer: owner=%d returned=%d result=%08x\n", p->unique_id, p->initializer_returned.load(), unsigned(result));
		Log::Flush();
		if (!p->initializer_returned || p->initializer_result != result || result != 0) { return; }
	}
	run.initializers_ok = true;
	// Fixed native streams must still select the audited static-buffer path.
	for (unsigned stream = 0; stream != 2; ++stream) {
		std::array<uint8_t,0x18> bytes {};
		const uint64_t object = run.libc->base_vaddr + (stream == 0 ? 0x1b42b8 : 0x1b4480);
		uint16_t flags = 0; uint32_t fd = 0; uint64_t begin = 0, end = 0;
		const bool available = SslSmokeRead(object, bytes.data(), bytes.size());
		std::memcpy(&flags, bytes.data(), 2); std::memcpy(&fd, bytes.data() + 4, 4);
		std::memcpy(&begin, bytes.data() + 8, 8); std::memcpy(&end, bytes.data() + 16, 8);
		if (!available || flags != 0x402 || fd != stream + 1 ||
		    begin != run.libc->base_vaddr + 0x1b73b0 + stream * 0x100 || end != begin + 0x100) {
			LOGF("Native SSL stdio admission rejected: stream=%u available=%d\n", stream, available);
			run.initializers_ok = false;
			return;
		}
	}
	// Fresh-only exclusions must hold after real DT_INIT, before the first init.
	for (uint64_t offset: {0x169188ull, 0x16a060ull, 0x16a458ull}) {
		uint64_t value = 0;
		if (!SslSmokeRead(run.ssl->base_vaddr + offset, &value, sizeof(value)) || value != 0) {
			LOGF("Native SSL fresh-state admission rejected at S+%" PRIx64 "\n", offset);
			run.initializers_ok = false;
			return;
		}
	}
	// Production's first actual caller owns the first SSL context. Never run the smoke lifecycle here.
	if (run.production) { return; }
	const auto base = run.ssl->base_vaddr;
	run.control.Run({reinterpret_cast<NativeSslSmoke::Init>(base + NativeSslSmoke::SslExports[0].rva),
	                 reinterpret_cast<NativeSslSmoke::Stats>(base + NativeSslSmoke::SslExports[1].rva),
	                 reinterpret_cast<NativeSslSmoke::Term>(base + NativeSslSmoke::SslExports[2].rva)}, SslSmokeObserve, &run);
}
} // namespace

void RuntimeLinker::RunNativeSslSmoke(void* stack_top) {
	if (!stack_top || !m_ssl_smoke_frozen || m_native_libc_startup.GetPhase() != NativeLibcStartup::Phase::Completed) {
		EXIT("Native SSL primary-stack/bridge admission rejected\n");
	}
	SslSmokeRun run {this, m_ssl_smoke_libc, m_ssl_smoke_ssl};
	RunEntry(reinterpret_cast<uint64_t>(SslSmokeGuestEntry), reinterpret_cast<EntryParams*>(&run), nullptr, stack_top);
	const auto& result = run.control.GetResult();
	LOGF("Native SSL smoke outcome: initializers_ok=%d outcome=%s init_returned=%d id=%08x stats_returned=%d stats_result=%08x canaries=%d term_returned=%d term_result=%08x\n",
	     run.initializers_ok, magic_enum::enum_name(result.outcome).data(), result.init_returned, unsigned(result.id),
	     result.stats_returned, unsigned(result.stats_result), result.canaries_ok, result.term_returned, unsigned(result.term_result));
	if (result.stats_returned) {
		for (size_t n = 0; n < result.stats.size(); n += 8) {
			uint64_t word; std::memcpy(&word, result.stats.data() + n, 8);
			LOGF("Native SSL public stats+%02zx=%016" PRIx64 "\n", n, word);
		}
	}
	const int status = run.initializers_ok && result.outcome == NativeSslSmoke::Outcome::Success ? 0 : 1;
	LOGF("Native SSL isolated session exit=%d (no ShellUI continuation)\n", status);
	Log::Flush();
	std::fflush(nullptr);
	std::quick_exit(status);
}

void RuntimeLinker::RunNativeSslProduction(void* stack_top) {
	if (!stack_top || !m_ssl_smoke_frozen || m_ssl_production_ready ||
	    m_native_libc_startup.GetPhase() != NativeLibcStartup::Phase::Completed) {
		EXIT("Native SSL production primary-stack/bridge admission rejected\n");
	}
	SslSmokeRun run {this, m_ssl_smoke_libc, m_ssl_smoke_ssl, true};
	RunEntry(reinterpret_cast<uint64_t>(SslSmokeGuestEntry), reinterpret_cast<EntryParams*>(&run), nullptr, stack_top);
	Common::LockGuard lock(m_mutex);
	if (!run.initializers_ok || !m_ssl_provider_startup.Completed()) {
		EXIT("Native SSL production provider initialization failed; boot stopped\n");
	}
	m_ssl_production_ready = true;
	m_ssl_smoke_frozen = false; // Ordinary late module loads are now legal; provider lifetime stays pinned.
	LOGF("Native SSL experimental routing ready: real I then S returned zero; continuing normal boot without a test SSL context\n");
}

RuntimeLinker::RuntimeLinker(): m_symbols(std::make_unique<SymbolDatabase>()),
	m_native_libc_startup(NativeSslSmoke::RequestedMode() != NativeSslSmoke::Mode::Disabled ? "1" :
	    (std::getenv("KYTY_NATIVE_LIBC_STARTUP") != nullptr ? std::getenv("KYTY_NATIVE_LIBC_STARTUP") : "")),
	m_ssl_smoke_enabled(NativeSslSmoke::RequestedMode() == NativeSslSmoke::Mode::Smoke),
	m_ssl_production_enabled(NativeSslSmoke::ProductionRequested()) {
	Libs::LibKernel::SemaphoreProvenance::Initialize();
	if (NativeSslSmoke::RequestedMode() == NativeSslSmoke::Mode::Conflict) { EXIT("KYTY_NATIVE_SSL and KYTY_NATIVE_SSL_SMOKE are mutually exclusive\n"); }
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
}

RuntimeLinker::~RuntimeLinker() {
	Clear();
}

Program* RuntimeLinker::LoadProgram(const std::filesystem::path& elf_name, bool new_application) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);
	CheckStartupMutation();
	if (!new_application) {
		const auto* current = GetCurrentProgram();
		if (current != nullptr && current->application_owner != nullptr) {
			const auto execution = GetApplicationExecution(current);
			// Map-only preparation is allowed before explicit execution opt-in.
			// All actual starts/references still reject a missing control below.
			if (execution && execution->StopRequested()) { return nullptr; }
		}
		// Recheck under the mapping lock for the boot session too. Managed
		// dlopen and sysmodule loading may both miss the caller-side lookup;
		// publishing two instances splits a native producer's mutable state.
		// FindProgramByFileName retains process ownership and file identity.
		if (auto* existing = FindProgramByFileName(elf_name)) {
			return current && current->application_owner && IsNativeKernelProvider(existing) ? nullptr : existing;
		}
	}

	static int32_t id_seq = 0;

	LOGF("Loading: %s\n", Common::PathToString(elf_name).c_str());

	auto  program_owner = std::make_unique<Program>();
	auto* program       = program_owner.get();

	program->rt        = this;
	program->file_name = elf_name;
	program->unique_id = ++id_seq;
	const auto* current = GetCurrentProgram();
	program->application_owner = new_application ? program :
	    (current != nullptr ? current->application_owner : nullptr);
	if (program->application_owner && IsNativeKernelProvider(program)) {
		LOGF("VSH runtime: native kernel provider load refused: %s\n", Common::PathToString(elf_name).c_str());
		return nullptr;
	}
	if (new_application) {
		GuestCallBoundaryStatus capability;
		if (!InitializeApplicationExecution(program, &capability)) {
			LOGF("VSH runtime: shared-owner guest recovery unavailable (%u)\n", unsigned(capability));
			return nullptr;
		}
	} else if (program->application_owner != nullptr) {
		const auto execution = GetApplicationExecution(program);
		if (execution && execution->StopRequested()) { return nullptr; }
	}
	// ps5-vsh TEMP: map GOT slots to imports.
	program->dbg_print_reloc = true;

	program->elf = std::make_unique<Elf64>();
	program->elf->Open(elf_name);
	program->authority_metadata = program->elf->GetAuthorityMetadata();
	const auto np_selection = NpAlternateStartup::Selected();
	if (np_selection == NpAlternateStartup::Selection::Invalid) {
		LOGF("NP alternate54: invalid/mixed selection\n");
		return nullptr;
	}
	const bool np_named = Common::EqualNoCase(elf_name.filename().string(), "libSceNpCommon.sprx");
	if (np_named) {
		const bool alternate = np_selection == NpAlternateStartup::Selection::Alternate54;
		const bool duplicate = std::any_of(m_programs.begin(), m_programs.end(), [](const auto* p) {
			return Common::EqualNoCase(p->file_name.filename().string(), "libSceNpCommon.sprx");
		});
		// Both routes pin their own image. In particular, legacy/narrower flags
		// cannot expose the alternate image without its distinct opt-in.
		if (program->application_owner || duplicate || !program->elf->IsValid() || !program->elf->IsShared() ||
		    !program->elf->FreezePlainFile(alternate ? NpAlternateStartup::ImageHash : NpAlternateStartup::OriginalHash,
		                                  NpAlternateStartup::ImageSize)) {
			LOGF("NP provider rejected: image/metadata/ownership/duplicate mismatch\n");
			return nullptr;
		}
		program->np_alternate54 = alternate;
	}

	if (new_application && (!program->elf->IsValid() || program->elf->IsShared())) {
		return nullptr;
	}
	if (program->elf->IsValid()) {
		LoadProgramToMemory(program);
		ParseProgramDynamicInfo(program);
		const auto np_identity = [](const auto& item) {
			return Common::EqualNoCase(item.name, "libSceNpCommon") || Common::EqualNoCase(item.name, "NpCommon");
		};
		const bool np_exports = std::any_of(program->dynamic_info->export_libs.begin(), program->dynamic_info->export_libs.end(), np_identity) ||
		    std::any_of(program->dynamic_info->export_modules.begin(), program->dynamic_info->export_modules.end(), np_identity);
		if (np_exports && !np_named) {
			LOGF("NP provider rejected: renamed/unverified provider\n");
			DeleteProgram(program_owner.release());
			return nullptr;
		}
		if (program->application_owner && IsNativeKernelProvider(program)) {
			LOGF("VSH runtime: native kernel export metadata refused: %s\n", Common::PathToString(elf_name).c_str());
			DeleteProgram(program_owner.release());
			return nullptr;
		}
		if (m_ssl_smoke_prepared && (Common::EqualNoCase(elf_name.filename().string(), "libSceSsl.sprx") ||
		    Common::EqualNoCase(elf_name.filename().string(), "libSceLibcInternal.sprx") ||
		    std::any_of(program->dynamic_info->export_libs.begin(), program->dynamic_info->export_libs.end(), [](const auto& l) {
			    return Common::EqualNoCase(l.name, "libSceSsl") || Common::EqualNoCase(l.name, "libSceLibcInternal");
		    }))) {
			if (program->application_owner != nullptr) {
				LOGF("VSH runtime: private replacement of a pinned session provider is unsupported: %s\n",
				     Common::PathToString(elf_name).c_str());
				DeleteProgram(program_owner.release());
				return nullptr;
			}
			EXIT("Replacing/duplicating a pinned native SSL provider is prohibited\n");
		}
		CreateSymbolDatabase(program);
		// Exact same-open-image identity, raw own export, executable windows and
		// all post-adaptation bytes must agree before any of the eight sites publish.
		if (NativeLibcFatalProfile(*program)) {
			EXIT_IF(!NativeAllocatorFatal::RegisterProfile(program->base_vaddr,
			    {reinterpret_cast<const uint8_t*>(program->base_vaddr), NativeAllocatorFatal::ProfileExtent}));
			LOGF("Native allocator fatal sites registered: %s base=0x%016" PRIx64 " count=%zu\n",
			     Common::PathToString(program->file_name).c_str(), program->base_vaddr, NativeAllocatorFatal::ProfileSites.size());
		}
		BindNativeLibcStartup(program);
	} else {
		EXIT("elf is not valid: %s\n", Common::PathToString(elf_name).c_str());
	}

	m_programs.push_back(program_owner.release());

	if (!program->elf->IsShared()) {
		program->fail_if_global_not_resolved = false;
		if (program->application_owner == nullptr) {
			Libs::LibKernel::SetProgName(elf_name.filename().string());
		}
	}

	if (Common::ToLower(Common::DirectoryWithoutFilename(Common::PathToGenericString(elf_name)))
	        .ends_with("_module/")) {
		program->fail_if_global_not_resolved = false;
	}

	return program;
}

void RuntimeLinker::SaveMainProgram(const std::filesystem::path& elf_name) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	for (const auto* p: m_programs) {
		EXIT_IF(p->elf == nullptr);

		if (!p->elf->IsShared()) {
			p->elf->Save(elf_name);
			break;
		}
	}
}

void RuntimeLinker::SaveProgram(Program* program, const std::filesystem::path& elf_name) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);

	if (auto it = std::find(m_programs.begin(), m_programs.end(), program);
	    it != m_programs.end()) {
		EXIT_IF((*it)->elf == nullptr);

		(*it)->elf->Save(elf_name);
	} else {
		EXIT("program not found");
	}
}

void RuntimeLinker::Execute(const std::filesystem::path& game_patch,
                            const std::vector<std::string>& guest_args,
                            const std::vector<CoElfLaunch>& co_elfs) {
	KYTY_PROFILER_THREAD("Thread_Main");
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		if (m_native_libc_startup.Enabled() && m_primary_startup_entered) {
			EXIT("Native Libc startup requires a single primary Execute per session\n");
		}
		m_primary_startup_entered = true;
		if (NativeSslSelected()) { m_ssl_smoke_thread = static_cast<uint64_t>(Common::Thread::GetThreadIdUnique()); }
	}

	// ps5-vsh: trace this host thread's HLE calls (it runs the main entry;
	// identifies ShellCore main's park when it goes quiet).
	Libs::TraceHostThread(
	    static_cast<uint64_t>(Common::Thread::GetThreadIdUnique()));

	// Predeclared executable cohosts inherit the explicit bootstrap namespace,
	// before libc/SSL/shared initializers can mutate the primary launch's cwd.
	// Shared initializers remain primary-owned; no ownership inference by name.
	namespace Fs = Libs::LibKernel::FileSystem;
	auto primary_fs = Fs::CreateBootstrapFsContext();
	LOGF("guest-fs: primary context=%p root=/ cwd=/\n", static_cast<void*>(primary_fs.get()));
	auto launches = co_elfs;
	for (auto& launch: launches) {
		launch.fs_context = Fs::CloneFsContext(primary_fs);
		LOGF("guest-fs: cohost=%s context=%p root=/ cwd=/\n", launch.argv0.c_str(),
		     static_cast<void*>(launch.fs_context.get()));
	}
	Fs::ScopedGuestFsContext primary_fs_scope(primary_fs);
	Libs::LibKernel::PthreadInitSelfForMainThread();
	auto* main_stack_top = Libs::LibKernel::PthreadCreateMainGuestStack();

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Guest code has no Windows stack probes and may jump over the guard page. Module
	// initializers execute on the host stack too, so grow it before calling any guest code.
	size_t expanded_size = 0;
	while (expanded_size < static_cast<size_t>(768) * 1024) {
		sys_dbg_stack_info_t stack {};
		SysStackUsage(stack);
		*reinterpret_cast<uint32_t*>(stack.guard_addr) = 0;
		expanded_size += stack.guard_size;
	}
#endif

	const bool system_session = !launches.empty() || NativeSslSelected() || m_native_libc_startup.Enabled();
	if (system_session) { PreloadAdjacentPrograms(); }
	if (NativeSslSelected()) {
		if (!game_patch.empty()) { EXIT("Game patches are not admitted for hash-pinned native SSL\n"); }
		PrepareNativeSslSmoke();
	}
	EnsureImportProviders();
	// The runtime automatically loads libc; other PRXs are requested by the application.
	const auto libc_path = Libs::LibKernel::FileSystem::GetRealFilename("/app0/sce_module/libc.prx");
	auto* libc = FindProgramByFileName(libc_path);
	if (libc == nullptr && Common::File::IsFileExisting(libc_path)) { libc = LoadProgram(libc_path); }
	RelocateAll();

	if (!game_patch.empty()) {
		Common::LockGuard lock(m_mutex);
		CheckStartupMutation();
		if (!GamePatch::Apply(game_patch, m_programs.empty() ? nullptr : m_programs.front(),
		                      m_programs)) {
			EXIT("Failed to apply game cheat\n");
		}
	}
	if (NativeSslSelected()) {
		Common::LockGuard lock(m_mutex);
		ValidateNativeSslSmoke(true);
		m_ssl_smoke_thread = static_cast<uint64_t>(Common::Thread::GetThreadIdUnique());
		m_ssl_smoke_frozen = true;
	}
	// Opt-in diagnostic stop after placement/relocation and startup mutations,
	// before native libc, module initializers or cohost guest entries can run.
	const auto startup_barrier = StartupBarrier::FromEnvironment();
	if (startup_barrier != StartupBarrier::Result::Disabled && startup_barrier != StartupBarrier::Result::Released) {
		EXIT("Pre-guest readiness barrier failed (%d); guest startup not entered\n", static_cast<int>(startup_barrier));
	}
	RunNativeLibcStartup();
	if (m_ssl_smoke_enabled) { RunNativeSslSmoke(main_stack_top); }
	if (m_ssl_production_enabled) { RunNativeSslProduction(main_stack_top); }
	// SysCore opens this platform-owned object; it does not create it. Make it
	// available before shared initializers (LoginMgr) and cohost entries run.
	// Only model the external pre-userland prerequisite (0x8): 13.60 SysCore
	// publishes 0x2 at +0xc75 and AgcCompositor waits for 0xa before starting.
	// Do not pre-publish compositor/MBUS/VSH completion as the reference seed does.
	// The creator belongs to the boot session; guest Open calls get ordinary aliases.
	if (!m_programs.empty() && m_programs.front()->file_name.filename() == "SceSysCore.elf") {
		Libs::LibcInternal::BindSysCoreServiceSandbox();
		Libs::LibKernel::EventFlag::KernelEventFlag boot_status = nullptr;
		const int result = Libs::LibKernel::EventFlag::KernelCreateEventFlag(
		    &boot_status, "SceBootStatusFlags", 0x121, 0x8, nullptr);
		if (result != 0) { EXIT("Failed to create SysCore boot status: 0x%08x\n", result); }
		LOGF("syscore bootstrap: SceBootStatusFlags initial=0x8; remaining bits guest-owned\n");
	}
	if (system_session) {
		StartAllModules();
	} else if (libc != nullptr && libc->dynamic_info->init_vaddr != 0) {
		StartModule(libc, 0, nullptr, nullptr);
	}

	// ps5-vsh: launch co-hosted ELF entries now that imports are relocated
	// and modules are started. Each gets its own guest thread; the heap
	// block outlives this frame (guest runs until quick_exit).
	for (const auto& co : launches) {
		auto* launch = new CoElfLaunch(co);
		Common::Thread(
		    [](void* param) {
			    auto* p = static_cast<CoElfLaunch*>(param);
			    // ps5-vsh: tag this thread's TLS + mount scope as
			    // belonging to the co-hosted program (voldemort parity).
			    RuntimeLinker::SetCurrentProgram(p->program);
			    Common::Singleton<RuntimeLinker>::Instance()->RunCoElfEntry(p->entry, p->argv0,
			                                                                 p->args, p->fs_context);
			    delete p;
		    },
		    launch)
		    .Detach();
	}

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Execute: %s\n---\n", "Main");

	if (auto entry = GetEntry(); entry != 0) {
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(m_mutex);
			AdmitGuestStartup();
		}
		// ps5-vsh: guest argv. argv[0] is always the host tag; extra args
		// (e.g. NPXS40038 videoCoreName + buildType) come from --guest-arg.
		// Storage must outlive the guest thread: keep it in a static so the
		// guest stack pointers stay valid until quick_exit.
		static std::vector<std::string> s_guest_arg_storage;
		s_guest_arg_storage = Config::GetGuestArguments();
		if (!guest_args.empty()) {
			s_guest_arg_storage = {"KytyEmu"};
			s_guest_arg_storage.insert(s_guest_arg_storage.end(), guest_args.begin(), guest_args.end());
		}
		const size_t params_size = offsetof(EntryParams, argv) + (s_guest_arg_storage.size() + 2) * sizeof(const char*);
		auto* params = reinterpret_cast<EntryParams*>(
		    (reinterpret_cast<uintptr_t>(main_stack_top) - params_size - 0x100u) & ~static_cast<uintptr_t>(0x0f));
		std::memset(params, 0, params_size);
		for (const auto& argument: s_guest_arg_storage) {
			params->argv[params->argc++] = argument.c_str();
		}
		for (int i = 0; i < params->argc; i++) {
			LOGF("\t guest argv[%d] = %s\n", i,
			     params->argv[i] != nullptr ? params->argv[i] : "<null>");
		}

		LOGF("stack_addr = %" PRIx64 "\n", reinterpret_cast<uint64_t>(params));

		NotifyRuntimeChildEntry();
		RunEntry(entry, params, ProgramExitHandler,
		         reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(params) - 0x1000u));
	}
	// ps5-vsh: daemon entries must never return; a return here means the
	// title's main function exited (silent boot death otherwise).
	LOGF_COLOR(Log::Color::BrightRed, "---\n--- Execute: main entry returned\n---\n");
}

void RuntimeLinker::RunCoElfEntry(uint64_t addr, const std::string& argv0,
                                  const std::vector<std::string>& args,
                                  Libs::LibKernel::FileSystem::GuestFsContextPtr fs_context) {
	KYTY_PROFILER_THREAD("Thread_CoElf");
	EXIT_IF(fs_context == nullptr);
	Libs::LibKernel::FileSystem::ScopedGuestFsContext fs_scope(std::move(fs_context));
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		AdmitGuestStartup();
	}

	Libs::LibKernel::PthreadInitSelfForMainThread();
	// ps5-vsh: distinguish co-elf mains in waiter censuses (all would
	// otherwise read "MainThread").
	Libs::LibKernel::PthreadRename(Libs::LibKernel::PthreadSelf(),
	                               ("CoElf:" + argv0).c_str());
	auto* main_stack_top = Libs::LibKernel::PthreadCreateMainGuestStack();

	const size_t params_size = offsetof(EntryParams, argv) + (args.size() + 3) * sizeof(const char*);
	auto* params = reinterpret_cast<EntryParams*>(
	    (reinterpret_cast<uintptr_t>(main_stack_top) - params_size - 0x100u) & ~static_cast<uintptr_t>(0x0f));
	std::memset(params, 0, params_size);

	// ps5-vsh: one arg store per worker thread (a shared static would race
	// across co-elfs). Heap-leaked like the main argv store: guest stack
	// pointers stay valid until quick_exit.
	thread_local std::vector<std::string>* t_co_arg_storage = nullptr;
	if (t_co_arg_storage == nullptr) {
		t_co_arg_storage = new std::vector<std::string>();
	}
	t_co_arg_storage->clear();
	t_co_arg_storage->push_back(argv0);
	for (const auto& a : args) {
		t_co_arg_storage->push_back(a);
	}
	const size_t extra     = t_co_arg_storage->size();
	params->argc           = static_cast<int>(extra);
	for (size_t i = 0; i < extra; i++) {
		params->argv[i] = (*t_co_arg_storage)[i].c_str();
	}
	for (int i = 0; i < params->argc; i++) {
		LOGF("\t co-elf argv[%d] = %s\n", i,
		     params->argv[i] != nullptr ? params->argv[i] : "<null>");
	}

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- CoElf Entry: %s (0x%016" PRIx64 ")\n---\n",
	           argv0.c_str(), addr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Libs::LibKernel::NoteCoElfThread(argv0, static_cast<uint64_t>(GetCurrentThreadId()));
#endif
	RunEntry(addr, params, ProgramExitHandler,
	         reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(params) - 0x1000u));
	// ps5-vsh: daemon entries must never return (see Execute above).
	LOGF_COLOR(Log::Color::BrightRed, "---\n--- CoElf Entry returned: %s\n---\n",
	           argv0.c_str());
}

RuntimeGuestResult RuntimeLinker::RunApplicationEntry(Program* program, EntryParams* params) {
	EXIT_IF(program == nullptr || program->application_owner != program || params == nullptr);
	EXIT_IF(GetCurrentProgram() != program || program->elf == nullptr || program->elf->IsShared());
	const auto execution = GetApplicationExecution(program);
	if (!execution) {
		return {{GuestCallBoundaryStatus::Unsupported, GuestCallOutcomeTag::NotInvoked, 0}, {}, true};
	}
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		AdmitGuestStartup();
	}
	if (!GetApplicationHardImportBlockers(program).empty() || !PublishApplicationStopText(program)) {
		return {{GuestCallBoundaryStatus::InvalidArgument, GuestCallOutcomeTag::NotInvoked, 0},
		    execution->Fail(GuestFailureReason::InvalidTransition), true};
	}
	const auto entry = program->base_vaddr + program->elf->GetEntry();
	if (!IsApplicationExecutableAddress(program, entry)) {
		return {{GuestCallBoundaryStatus::InvalidArgument, GuestCallOutcomeTag::NotInvoked, 0},
		    execution->Fail(GuestFailureReason::InvalidTransition, entry), true};
	}
	LOGF("VSH runtime: enter app module=%d entry=0x%016" PRIx64 " on existing guest thread\n",
	     program->unique_id, entry);
	const auto exit_callback = RuntimeCrt::PrepareEntryExitCallback(execution);
	if (!exit_callback) {
		return {{GuestCallBoundaryStatus::CodeAllocationFailed, GuestCallOutcomeTag::NotInvoked, 0},
		    execution->Fail(GuestFailureReason::BoundaryUnavailable), true};
	}
	return execution->InvokeEntry(entry, params, exit_callback);
}

void RuntimeLinker::Clear() {
	// EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());

	Common::LockGuard lock(m_mutex);
	CheckStartupMutation();
	if (m_ssl_smoke_prepared) { EXIT("Native SSL providers cannot be cleared/rearmed; process exit required\n"); }
	if (!RetireSourceBoundLoginMgrTransport()) { EXIT("LoginMgr native objects/dispatcher still live; refusing mapping release before native stop/drain/join\n"); }
	if (!RetireSourceBoundNpTransport()) { EXIT("NP native objects/dispatcher still live; refusing mapping release before native stop/drain/join\n"); }
	if (!RetireSourceBoundAppDb()) { EXIT("AppDb native objects/dispatcher still live; refusing mapping release before native stop/drain/join\n"); }
	GamePatch::Clear();

	for (auto* p: m_programs) {
		EXIT_IF(m_native_libc_startup.InvalidateProvider(p->unique_id) == NativeLibcStartup::Result::Invoking);
		DeleteProgram(p);
	}
	m_programs.clear();
	for (const auto page: g_unresolved_stub_thunk_pages) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(page, UNRESOLVED_STUB_PAGE_SIZE));
	}
	g_unresolved_stub_thunk_pages.clear();
	g_unresolved_stub_thunk_offset = 0;
	g_stubbed_imports.clear();
	g_unresolved_stub_call_log_count.store(0);
	if (g_invalid_memory != 0) {
		EXIT_IF(!Libs::LibKernel::Memory::FreeGuestMemory(g_invalid_memory, 4096));
		g_invalid_memory = 0;
	}
	g_tls_main_program        = nullptr;
	g_tls_cached_main_program = nullptr;
	g_tls_cached_main_tcb     = nullptr;
	g_desired_base_addr       = SYSTEM_RESERVED + CODE_BASE_OFFSET;
	m_symbols.reset();
}

void RuntimeLinker::Resolve(const std::string& name, SymbolType type, Program* program,
                            SymbolRecord* out_info, bool* bind_self) {
	KYTY_PROFILER_FUNCTION();

	Common::LockGuard lock(m_mutex);
	const auto candidates = GetVisiblePrograms(program);

	EXIT_IF(out_info == nullptr);

	auto ids = Common::Split(name, '#');

	if (bind_self != nullptr) {
		*bind_self = false;
	}
	if (ResolveNativeSslProduction(name, type, program, out_info)) {
		if (bind_self) { *bind_self = program == m_ssl_smoke_ssl; }
		return;
	}
	if (ResolveNativeSslSmoke(name, type, program, out_info)) { return; }

	// These provider policies expose only explicitly audited native families.
	// Prefer them before HLE even when import version metadata is absent.
	// Filtering at database insertion also covers exact/NID/name fallback,
	// late resolution and KernelDlsym, rather than only changing preference.
	if (type == SymbolType::Func && !ids.empty()) {
		for (auto* p: candidates) {
			if (p->export_symbols != nullptr &&
			    (p->export_symbols->GetExportPolicy() == SymbolDatabase::ExportPolicy::NpOwnership ||
			     p->export_symbols->GetExportPolicy() == SymbolDatabase::ExportPolicy::NpWorkQueue ||
			     p->export_symbols->GetExportPolicy() == SymbolDatabase::ExportPolicy::NpCallout ||
			     p->export_symbols->GetExportPolicy() == SymbolDatabase::ExportPolicy::NpHttpPool ||
			     p->export_symbols->GetExportPolicy() == SymbolDatabase::ExportPolicy::NpEventQueue ||
			     p->export_symbols->GetExportPolicy() == SymbolDatabase::ExportPolicy::NpDispatcher)) {
				if (const auto* rec = p->export_symbols->FindByNid(ids.front(), type); rec != nullptr) {
					*out_info = *rec;
					if (bind_self != nullptr) { *bind_self = p == program; }
					return;
				}
			}
		}
	}

	if (ids.size() == 3) {
		const LibraryId* l = FindLibrary(*program, ids.at(1));
		const ModuleId*  m = FindModule(*program, ids.at(2));

		auto resolve_by_nid = [this, type, &candidates](const std::string& nid, SymbolRecord* out) -> bool {
			EXIT_IF(out == nullptr);

			if (m_symbols != nullptr) {
				if (const auto* rec = m_symbols->FindByNid(nid, type); rec != nullptr) {
					*out = *rec;
					return true;
				}
			}

			for (auto* p: candidates) {
				if (p != nullptr && p->export_symbols != nullptr) {
					if (const auto* rec = p->export_symbols->FindByNid(nid, type); rec != nullptr) {
						*out = *rec;
						return true;
					}
				}
			}

			return false;
		};

		if (l != nullptr && m != nullptr) {
			SymbolResolve sr {};
			sr.name                 = ids.at(0);
			sr.library              = l->name;
			sr.library_version      = l->version;
			sr.module               = m->name;
			sr.module_version_major = m->version_major;
			sr.module_version_minor = m->version_minor;
			sr.type                 = type;

			const SymbolRecord* rec = nullptr;

			// ps5-vsh: a co-loaded firmware library owns its exports; the HLE copy
			// only fills what no loaded module provides.
			//
			// This must be ALL-OR-NOTHING per library. rnpherm3 died in
			// libSceLibcInternal's varargs path because &g_jsiGv[0] (our 24-byte
			// zero array for the Jsi-0.70_v1 object gV349EB96Eg) was passed around
			// as a C++ object whose first word is null (call [0x20]). Sony's real
			// libhermes/libNativeExtensions already had 88 Jsi-0.70_v1 imports
			// bound to the real libSceJsi while 23 were bound to our HLE - the
			// mix, not either side, is what is unsound.
			//
			// Originally scoped to libSceComposite (its InitWithProcessOrder
			// registers the client with AgcCompositor, so mixing HLE entry points
			// with the real library left the compositor without a client).
			//
			// libhermes-0.11_v1 (2026-10-22): libNativeExtensions is the module
			// that CREATES the JS runtime on hardware, and every one of its
			// libhermes imports was binding to a zero stub - 9 distinct NIDs
			// (N7RhzM2vOBc, 0DcgX1p7Zu8, GORZXedSOnU, WIDyseMUXEo, Vo9D2A1RRyw,
			// plus libhermesRemoteInspector's four) came out as
			// "Stubbed: [...] <- 0000000000000000" even though libhermes-0.11.sprx
			// is loaded at 0x10a0000000. Without an HLE for them the only
			// runtime-creation path the guest has was dead, which is why no
			// [RNPS host] line ever appeared in any run. Same lever, same
			// justification as Jsc_v1 below: match the NID, not the versioned
			// library record.
			// RnpsBundle_v1 (2026-10-22): our HLE libRnpsBundle implements the
			// eight entry points libNativeExtensions imports (InitEx, Term,
			// Decrypt, VerifyChain, LoadCrl*, ...) but NOT K9DCgmMthBQ, which
			// _sceRnpsBundleRegistryInit() forwards to. That one import fell
			// through to a zero stub:
			//   Unresolved import stub called [79]: K9DCgmMthBQ[RnpsBundle_v1]
			//   ps5-vsh: RnpsBundleRegistryInit -> 0x0000000000000000
			// HLE symbols still win for the eight we do provide, so co-loading
			// the real module only fills the gap instead of replacing our
			// replay-based decrypt.
			static const char* const k_cohost_libs[] = {"libSceComposite", "Jsi-0.70_v1",
			                                            "Jsc_v1", "libhermes-0.11_v1",
			                                            "RnpsBundle_v1", "libScePsm", "Psm_v1"};
			// ps5-vsh: Psm_v1 joined 2026-10-27. Our HLE Psm library is a
			// single log-only PsmEventPost stub; with HLE winning by default
			// it silently dropped every guest PsmEvent (login/framework
			// events that fill the managed Lx login cache). The real
			// libScePsm.sprx is co-loaded, so let it own the bus.
			//
			// KYTY_REAL_MODULE_WINS widens the preference to every co-loaded
			// library. Both settings are known regressions as of 2026-10-21
			// (rnpwin1 = host stack overflow before OnLaunched with "1";
			// rnpwin2 = host write to 0x10001043a with "objects") because our HLE
			// functions and Sony's objects are mutually dependent. Kept for A/B.
			static const int k_real_module_wins = [] {
				const char* e = std::getenv("KYTY_REAL_MODULE_WINS");
				if (e == nullptr || e[0] == '\0' || e[0] == '0') {
					return 0;
				}
				if (e[0] == 'o' || e[0] == 'O') {
					return 1;
				}
				return 2;
			}();
			const bool real_wins = k_real_module_wins == 2 ||
			                       (k_real_module_wins == 1 && type == SymbolType::Object);
			const bool cohost = std::any_of(
			    std::begin(k_cohost_libs), std::end(k_cohost_libs),
			    [&sr](const char* n) { return sr.library == n; });
			if (cohost || real_wins) {
				for (auto* p: candidates) {
					const auto& libs = p->dynamic_info->export_libs;
					if (p->export_symbols == nullptr ||
					    std::find(libs.begin(), libs.end(), *l) == libs.end()) {
						continue;
					}
					if (const auto* real = p->export_symbols->Find(sr); real != nullptr) {
						rec = real;
						if (bind_self != nullptr) {
							*bind_self = (p == program);
						}
						break;
					}
				}
				// ps5-vsh: inside a co-hosted library, a NID match from the module
				// that exports that library is better than a zero-filled stand-in.
				// The import's module version is not always the one the loaded
				// module advertises: Jsi-0.70_v1 imports ask for Jsi-0.70_v1.1, six
				// objects bind to the real libSceJsi, but gV349EB96Eg /
				// Mat-W4cvnNE / mERZOPXePvQ / CwdZZNHzMRQ / TJLtAXxNzqI miss the
				// exact key and land on libJscdTail's 24-byte zero blobs, which
				// Sony code then dereferences as a live object (rnpherm3:
				// libc call [0x20] through &g_jsiGv[0]; rnppix2: execute at 0).
				// ps5-vsh: inside a co-hosted library, match by NID ALONE, across
				// every loaded module, ignoring the library record and its version.
				// The firmware linker matches on the NID; the library/module names
				// are documentation, and insisting on them made our loader stricter
				// than the hardware. Jsc_v1 is the case that mattered:
				// libNativeExtensions imports m9Y2LTyzlQc from library Jsc_v1
				// (module Jsc_v1.1), but libSceJsc.sprx declares its export library
				// as libSceJsc and carries the NID at 0xc15351. The exact key missed,
				// the name-keyed fallback missed, and the import froze onto our
				// zero-returning JscUnk_m9Y2LTyzlQc instead of libSceJsc+0x25430.
				if (rec == nullptr) {
					for (auto* p: candidates) {
						if (p->export_symbols == nullptr) {
							continue;
						}
						if (const auto* real = p->export_symbols->FindByNid(sr.name, type);
						    real != nullptr) {
							rec = real;
							if (bind_self != nullptr) {
								*bind_self = (p == program);
							}
							break;
						}
					}
				}
			}

			if (rec == nullptr && m_symbols != nullptr) {
				rec = m_symbols->Find(sr);
			}

			if (rec == nullptr) {
				for (auto* p: candidates) {
					const auto& libs = p->dynamic_info->export_libs;
					const auto& modules = p->dynamic_info->export_modules;
					if (p->export_symbols == nullptr ||
					    std::find(libs.begin(), libs.end(), *l) == libs.end() ||
					    std::find(modules.begin(), modules.end(), *m) == modules.end()) {
						continue;
					}
					rec = p->export_symbols->Find(sr);
					if (rec != nullptr) {
						if (bind_self != nullptr) {
							*bind_self = (p == program);
						}
						break;
					}
				}
			}

			if (rec == nullptr) {
				if (resolve_by_nid(sr.name, out_info)) {
					LOGF("PS5 NID fallback: %s -> %s\n", sr.name.c_str(), out_info->name.c_str());
					return;
				}
			}

			// The native compositor still owns registration, queues and objects.
			// Media encoders dispatch host-decoded canvases to GPU-ordered uploads
			// and delegate other metadata IDs to native encoders. Keep these
			// overrides when late Composite loading rebinds PSM.
			if (program->application_owner == nullptr && sr.name == "T6CVkdCDO7o" &&
			    rec != nullptr && type == SymbolType::Func) {
				Libs::Composite::SetNativeCanvasScratch(rec->vaddr);
			}
			if (program->application_owner == nullptr &&
			    (sr.name == "+DMKcAbpcrQ" || sr.name == "yZcGEBwnDAM") &&
			    type == SymbolType::Func && m_symbols != nullptr) {
				if (const auto* media = m_symbols->FindByNid(sr.name, type)) {
					if (rec != nullptr && rec->vaddr != media->vaddr) {
						if (sr.name == "+DMKcAbpcrQ")
							Libs::Composite::SetNativeVirtualCanvasDraw(rec->vaddr);
						else
							Libs::Composite::SetNativeTexturePatch(rec->vaddr);
					}
					rec = media;
					if (bind_self != nullptr) *bind_self = false;
				}
			}
			if (rec != nullptr) {
				*out_info = *rec;
			} else {
				out_info->vaddr    = 0;
				out_info->name     = SymbolDatabase::GenerateName(sr);
				out_info->dbg_name = "";
			}
		} else {
			if (resolve_by_nid(ids.at(0), out_info)) {
				LOGF("PS5 NID fallback: %s -> %s (missing lib/module metadata)\n",
				     ids.at(0).c_str(), out_info->name.c_str());
				return;
			}

			EXIT("l == nullptr || m == nullptr");
		}
	} else {
		out_info->vaddr    = 0;
		out_info->name     = name;
		out_info->dbg_name = "";
	}
}

uint64_t RuntimeLinker::ResolveApplicationImportData(Program* importer, const std::string& raw_name,
    const SymbolRecord& resolved, int64_t addend, uint64_t declared_size) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	const auto execution = GetApplicationExecution(importer);
	auto* owner = importer ? importer->application_owner : nullptr;
	if (!owner || !execution || importer->rt != this || owner->rt != this || !m_symbols ||
	    std::find(m_programs.begin(), m_programs.end(), importer) == m_programs.end() ||
	    std::find(m_programs.begin(), m_programs.end(), owner) == m_programs.end() ||
	    !ApplicationImportData::Supports(*m_symbols, raw_name, resolved, addend, declared_size)) { return 0; }
	auto lease = execution->TryAcquire(GuestWorkKind::HostCall);
	if (!lease || execution->PendingThreadExit()) { return 0; }
	std::unique_ptr<ApplicationImportData> prepared;
	if (!owner->application_import_data) {
		prepared = ApplicationImportData::Create(owner, execution->Owner());
		if (!prepared) { return 0; }
	}
	const auto* data = prepared ? prepared.get() : owner->application_import_data.get();
	const auto value = data->Address(execution->Owner(), *m_symbols, raw_name, resolved, addend, declared_size);
	if (!value) { return 0; }
	uint64_t published = 0;
	lease.Publish([&] {
		if (prepared) { owner->application_import_data = std::move(prepared); }
		published = value;
	});
	return published;
}

uint64_t RuntimeLinker::GetApplicationSanitizerReplacement(ApplicationSanitizerReplacement kind) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	auto* importer = GetCurrentProgram();
	auto* owner = importer ? importer->application_owner : nullptr;
	const auto execution = GetApplicationExecution(importer);
	if (!owner || !execution || importer->rt != this || owner->rt != this ||
	    (kind != ApplicationSanitizerReplacement::Malloc && kind != ApplicationSanitizerReplacement::New) ||
	    std::find(m_programs.begin(), m_programs.end(), importer) == m_programs.end() ||
	    std::find(m_programs.begin(), m_programs.end(), owner) == m_programs.end()) { return 0; }
	auto lease = execution->TryAcquire(GuestWorkKind::HostCall);
	if (!lease || execution->PendingThreadExit()) { return 0; }
	std::unique_ptr<ApplicationImportData> prepared;
	if (!owner->application_import_data) {
		prepared = ApplicationImportData::Create(owner, execution->Owner());
		if (!prepared) { return 0; }
	}
	const auto* data = prepared ? prepared.get() : owner->application_import_data.get();
	const auto value = data->SanitizerTable(execution->Owner(), kind);
	if (!value) { return 0; }
	uint64_t published = 0;
	lease.Publish([&] {
		if (prepared) { owner->application_import_data = std::move(prepared); }
		published = value;
	});
	// A stop racing after this publication is propagated by the matching host
	// return gateway, after this function's locks/leases have finished normally.
	return published;
}

int RuntimeLinker::ResolveApplicationDlsym(Program* handle, const std::string& symbol,
                                         const std::string& nid, void** address) {
	using namespace Libs::LibKernel;
	if (!address) { return KERNEL_ERROR_EFAULT; }
	*address = nullptr;
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	auto* requester = GetCurrentProgram();
	const auto execution = GetApplicationExecution(requester);
	if (!requester || !requester->application_owner || !execution || requester->rt != this) {
		return KERNEL_ERROR_EINVAL;
	}
	auto lease = execution->TryAcquire(GuestWorkKind::HostCall);
	if (!lease || execution->PendingThreadExit()) { return KERNEL_ERROR_ECANCELED; }
	if (!handle || !handle->export_symbols ||
	    std::find(m_programs.begin(), m_programs.end(), handle) == m_programs.end()) { return KERNEL_ERROR_ESRCH; }
	if (handle->application_owner && handle->application_owner != requester->application_owner) {
		return KERNEL_ERROR_EOPNOTSUPP;
	}
	SymbolType type = SymbolType::Unknown;
	const auto find = [&](const Program* program) -> const SymbolRecord* {
		if (!program || !program->export_symbols) { return nullptr; }
		for (auto candidate: {SymbolType::Func, SymbolType::Object, SymbolType::NoType}) {
			if (const auto* record = program->export_symbols->FindByNid(nid, candidate)) {
				type = candidate;
				return record;
			}
		}
		for (auto candidate: {SymbolType::Func, SymbolType::Object, SymbolType::NoType}) {
			if (const auto* record = program->export_symbols->FindByName(symbol, candidate)) {
				type = candidate;
				return record;
			}
		}
		return nullptr;
	};
	const auto* record = find(handle);
	if (!record) {
		for (auto* provider: GetVisiblePrograms(requester)) {
			if (provider != handle && (record = find(provider))) { break; }
		}
	}
	if (!record) { return KERNEL_ERROR_ESRCH; }
	uint64_t value = 0;
	if (type == SymbolType::Func) {
		if (!requester->application_import_bindings) {
			requester->application_import_bindings = std::make_unique<ApplicationImportBindings>(requester);
		}
		// The resolved record's identity wins over the query's spelling (which
		// may be a human name or an already encoded NID). Never return raw HLE.
		const auto separator = record->name.find('[');
		if (separator == std::string::npos) { return KERNEL_ERROR_EOPNOTSUPP; }
		value = requester->application_import_bindings->BindResolved(record->name.substr(0, separator), *record);
	} else if (type == SymbolType::Object) {
		const auto* provider = FindProgramByAddr(record->vaddr);
		if (provider && provider->application_owner == requester->application_owner) { value = record->vaddr; }
		else if (!provider) {
			const auto separator = record->name.find('[');
			if (separator != std::string::npos) {
				value = ResolveApplicationImportData(requester, record->name.substr(0, separator), *record);
			}
		}
	}
	// Ambiguous/foreign native pointers and unaudited HLE remain ordinary lookup
	// errors. Do not poison the owner merely because it probes an optional API.
	if (!value) { return KERNEL_ERROR_EOPNOTSUPP; }
	return lease.Publish([&] { *address = reinterpret_cast<void*>(value); }) ? OK : KERNEL_ERROR_ECANCELED;
}

uint64_t RuntimeLinker::ReadFromElf(Program* program, uint64_t vaddr) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->base_vaddr == 0 || program->base_size == 0);
	EXIT_IF(program->elf == nullptr);

	uint64_t ret = 0;

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t segment_addr      = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size = phdr[i].p_filesz;

			if (vaddr >= segment_addr && vaddr < segment_addr + segment_file_size) {
				program->elf->LoadSegment(reinterpret_cast<uint64_t>(&ret),
				                          phdr[i].p_offset + vaddr - segment_addr, sizeof(ret));
				break;
			}
		}
	}

	return ret;
}

Program* RuntimeLinker::FindProgramById(int32_t id) {
	Common::LockGuard lock(m_mutex);

	// Id 0 is reserved for main program
	if (id == 0 && !m_programs.empty()) {
		if (auto* current = GetCurrentProgram(); current != nullptr && current->application_owner != nullptr) {
			return current->application_owner;
		}
		if (m_programs.front()->export_symbols &&
		    m_programs.front()->export_symbols->GetExportPolicy() == SymbolDatabase::ExportPolicy::PrivateSslSmoke) { return nullptr; }
		return m_programs.front();
	}

	for (auto* p: m_programs) {
		if (p->unique_id == id) {
			// KernelDlsym must fail before its global fallback for this private handle.
			if (p->export_symbols && p->export_symbols->GetExportPolicy() == SymbolDatabase::ExportPolicy::PrivateSslSmoke) { return nullptr; }
			return p;
		}
	}

	return nullptr;
}

void RuntimeLinker::SetCurrentProgram(Program* program) {
	g_tls_current_program = program;
}

Program* RuntimeLinker::GetCurrentProgram() {
	return g_tls_current_program != nullptr ? g_tls_current_program : g_tls_main_program;
}

Program* RuntimeLinker::FindProgramByFileName(const std::filesystem::path& elf_name) {
	Common::LockGuard lock(m_mutex);

	const auto* current = GetCurrentProgram();
	const auto* owner = current != nullptr ? current->application_owner : nullptr;
	const auto fixed_name = Common::FixFilenameSlash(Common::PathToGenericString(elf_name));
	for (auto* p: GetVisiblePrograms(current)) {
		// Symbol visibility includes session providers, but mutable module state
		// belongs to the requesting process. Only the explicitly pinned native
		// SSL/libc providers retain their existing cross-owner reuse policy.
		const bool pinned = m_ssl_smoke_prepared && p->application_owner == nullptr &&
		                    (p == m_ssl_smoke_libc || p == m_ssl_smoke_ssl);
		if (p->application_owner != owner && !pinned) { continue; }
		if (Common::EqualNoCase(Common::FixFilenameSlash(Common::PathToGenericString(p->file_name)),
		                        fixed_name)) {
			return p;
		}
		// Staged native libraries may be hard links (or symlinks) to the same
		// firmware file. Canonical path strings alone cannot recognize hard links.
		// Failed stat/identity queries are a miss, never a basename/hash fallback.
		std::error_code error;
		if (std::filesystem::equivalent(p->file_name, elf_name, error) && !error) { return p; }
	}

	return nullptr;
}

int RuntimeLinker::QueryKernelTextLocationError() {
	Common::HostLockCriticalGuard critical;
	const auto* current = GetCurrentProgram();
	if (current == nullptr || current->rt != this || GetCurrentGuestProcessId() <= 0) {
		return Libs::Posix::POSIX_ESRCH;
	}
	Common::LockGuard lock(m_mutex);
	const auto published = [&](const Program* program) {
		return std::find(m_programs.begin(), m_programs.end(), program) != m_programs.end();
	};
	if (!published(current) || current->base_vaddr == 0 || current->mapped_size == 0) {
		return Libs::Posix::POSIX_ESRCH;
	}
	const auto* owner = current->application_owner;
	if (owner != nullptr && (!published(owner) || owner->rt != this || owner->application_owner != owner ||
	                         owner->base_vaddr == 0 || owner->mapped_size == 0)) {
		return Libs::Posix::POSIX_ESRCH;
	}
	for (const auto* program: m_programs) {
		// Symbol visibility also includes session providers, but that does not
		// establish a native kernel mapping in a runtime application's process.
		// No cross-owner/session borrowing. Null-owner callers retain the existing
		// boot/direct-game namespace; runtime owners must have their own mapping.
		if (program->application_owner != owner || program->rt != this ||
		    (program->base_vaddr == 0 && program->mapped_size == 0)) { continue; }
		const auto kernel_name = [](const std::string& name) {
			const auto lower = Common::ToLower(name);
			return lower == "libkernel" || lower == "libkernel_sys" || lower == "libkernel_web";
		};
		bool candidate = kernel_name(program->file_name.stem().string());
		if (program->dynamic_info != nullptr) {
			for (const auto& module: program->dynamic_info->export_modules) {
				candidate = candidate || kernel_name(module.name);
			}
			for (const auto& library: program->dynamic_info->export_libs) {
				candidate = candidate || kernel_name(library.name);
			}
		}
		// Identification need not certify a text extent: even a single genuine
		// candidate, conflicting metadata, or multiple candidates is unsupported.
		// Never relabel present-but-unsupported as absence, nor guess bounds.
		if (candidate) { return Libs::Posix::POSIX_ENOSYS; }
	}
	return Libs::Posix::POSIX_ENOENT;
}

std::vector<Program*> RuntimeLinker::GetVisiblePrograms(const Program* requester) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	const auto* owner = requester != nullptr ? requester->application_owner : nullptr;
	std::vector<Program*> programs;
	if (owner != nullptr) {
		for (auto* program: m_programs) {
			if (program->application_owner == owner) { programs.push_back(program); }
		}
	}
	for (auto* program: m_programs) {
		if (program->application_owner == nullptr) { programs.push_back(program); }
	}
	return programs;
}

Program* RuntimeLinker::FindProgramByAddr(uint64_t vaddr) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		const auto* ehdr = p->elf->GetEhdr();
		const auto* phdr = p->elf->GetPhdr();

		EXIT_IF(phdr == nullptr || ehdr == nullptr);

		for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
			if (phdr[i].p_memsz != 0 &&
			    (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
				uint64_t segment_addr = phdr[i].p_vaddr + p->base_vaddr;
				uint64_t segment_size = GetAlignedSize(phdr + i);

				if (vaddr >= segment_addr && vaddr < segment_addr + segment_size) {
					return p;
				}
			}
		}
	}

	return nullptr;
}

void RuntimeLinker::StackTrace(uint64_t frame_ptr, uint64_t stack_ptr) {
	void* stack[20];
	int   depth = WalkGuestStack(frame_ptr, stack_ptr, stack, static_cast<int>(std::size(stack)));

	LOGF("Stack trace [thread = %d]:\n", Common::Thread::GetThreadIdUnique());

	for (int i = 0; i < depth; i++) {
		auto  vaddr = reinterpret_cast<uint64_t>(stack[i]);
		auto* p     = FindProgramByAddr(vaddr);
		LOGF("[%d] %016" PRIx64 ", off=%016" PRIx64 ", %s\n", i, vaddr,
		     (p == nullptr ? 0 : vaddr - p->base_vaddr),
		     (p == nullptr
		          ? "???"
		          : Common::FilenameWithoutDirectory(Common::PathToGenericString(p->file_name))
		                .c_str()));
	}
}

static std::string GetProgramModuleName(const Program* program) {
	EXIT_IF(program == nullptr);

	if (program->dynamic_info != nullptr && program->dynamic_info->so_name != nullptr &&
	    program->dynamic_info->so_name[0] != '\0') {
		return std::string(program->dynamic_info->so_name);
	}

	return Common::FilenameWithoutDirectory(Common::PathToGenericString(program->file_name));
}

static bool ModuleStartDependenciesSatisfied(const Program*               program,
                                             const std::vector<Program*>& programs,
                                             const std::vector<Program*>& started) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);

	for (const auto* needed: program->dynamic_info->needed) {
		if (needed == nullptr || needed[0] == '\0') {
			continue;
		}

		const auto needed_name = std::string(needed);

		for (auto* dependency: programs) {
			if (dependency == nullptr || dependency == program || dependency->elf == nullptr ||
			    !dependency->elf->IsShared()) {
				continue;
			}

			const auto dependency_name = GetProgramModuleName(dependency);
			if (Common::EqualNoCase(dependency_name, needed_name) ||
			    Common::EqualNoCase(Common::FilenameWithoutDirectory(
			                            Common::PathToGenericString(dependency->file_name)),
			                        needed_name)) {
				if (std::find(started.begin(), started.end(), dependency) == started.end()) {
					return false;
				}
				break;
			}
		}
	}

	return true;
}

void RuntimeLinker::StartAllModules() {
	Common::LockGuard lock(m_mutex);

	std::vector<Program*> started;
	if (m_ssl_production_ready) {
		started.push_back(m_ssl_smoke_libc);
		started.push_back(m_ssl_smoke_ssl);
	}

	for (;;) {
		bool progressed = false;

		for (auto* p: m_programs) {
			if (p->elf->IsShared() && p->dynamic_info->init_vaddr != 0 &&
			    std::find(started.begin(), started.end(), p) == started.end() &&
			    ModuleStartDependenciesSatisfied(p, m_programs, started)) {
				StartModule(p, 0, nullptr, nullptr);
				started.push_back(p);
				progressed = true;
			}
		}

		if (!progressed) {
			break;
		}
	}

	for (auto* p: m_programs) {
		if (p->elf->IsShared() && p->dynamic_info->init_vaddr != 0 &&
		    std::find(started.begin(), started.end(), p) == started.end()) {
			StartModule(p, 0, nullptr, nullptr);
			started.push_back(p);
		}
	}
}

std::vector<Program*> RuntimeLinker::GetApplicationStartupModules(const Program* owner) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	const auto visible = GetVisiblePrograms(owner);
	std::vector<Program*> started;
	std::vector<Program*> pending;
	std::vector<Program*> ordered;
	for (auto* program: visible) {
		if (program->elf == nullptr || !program->elf->IsShared()) { continue; }
		if (program->started || program->dynamic_info->init_vaddr == 0) {
			started.push_back(program);
		} else if (program->application_owner == owner) {
			pending.push_back(program);
		}
	}
	while (!pending.empty()) {
		auto next = std::find_if(pending.begin(), pending.end(), [&](const auto* program) {
			return ModuleStartDependenciesSatisfied(program, visible, started);
		});
		// Match boot's deterministic cycle fallback, confined to this owner's
		// newly loaded libraries; never start another application's modules.
		if (next == pending.end()) { next = pending.begin(); }
		ordered.push_back(*next);
		started.push_back(*next);
		pending.erase(next);
	}
	return ordered;
}

void RuntimeLinker::StopAllModules() {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		if (p->elf->IsShared() && p->dynamic_info->fini_vaddr != 0) {
			if (m_ssl_production_enabled && p->native_ssl_smoke_dependency) { continue; }
			StopModule(p, 0, nullptr, nullptr);
		}
	}
}

bool RuntimeLinker::PrepareAppLocalPrograms(Program* executable, const std::filesystem::path& root,
                                          AppLocalProgramPolicy policy, std::vector<Program*>* prepared, std::string* error) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	CheckStartupMutation();
	prepared->clear();
	const auto fail = [&](const std::string& message) {
		if (error != nullptr) { *error = message; }
		return false;
	};
	if (executable == nullptr || executable->dynamic_info == nullptr || executable->elf == nullptr ||
	    !executable->elf->IsValid() || executable->elf->IsShared() || root.empty() ||
	    std::find(m_programs.begin(), m_programs.end(), executable) == m_programs.end() ||
	    (executable->application_owner != nullptr && executable->application_owner != executable)) {
		return fail("app-local preparation requires an explicit loaded executable/root");
	}
	struct RestoreProgram {
		Program* previous = RuntimeLinker::GetCurrentProgram();
		~RestoreProgram() { RuntimeLinker::SetCurrentProgram(previous); }
	} restore;
	SetCurrentProgram(executable);
	AppLocalLibraries discovery(root);
	std::vector<Program*> pending {executable};
	constexpr size_t MaxPrograms = 256;
	const auto map = [&](const std::filesystem::path& path) {
		// Validate even a reuse candidate; filenames/extensions alone are not ELF
		// admission. FindProgramByFileName preserves owner/hard-link/pinned policy.
		Elf64 image;
		image.Open(path);
		if (!image.IsValid() || !image.IsShared()) { return fail("invalid/non-shared app-local module: " + path.generic_string()); }
		auto* program = FindProgramByFileName(path);
		if ((program == nullptr || (program->application_owner == executable->application_owner &&
		    std::find(pending.begin(), pending.end(), program) == pending.end())) && pending.size() >= MaxPrograms) {
			return fail("app-local dependency closure exceeds 256 images");
		}
		if (program == nullptr) {
			program = LoadProgram(path);
			if (program == nullptr) { return fail("could not map app-local module: " + path.generic_string()); }
			program->fail_if_global_not_resolved = false;
		}
		// Explicit pinned session SSL/libc reuse remains legal, but its existing
		// session dependency graph and startup state are not this app's to mutate.
		if (program->application_owner != executable->application_owner) { return true; }
		if (std::find(pending.begin(), pending.end(), program) == pending.end()) {
			pending.push_back(program);
			prepared->push_back(program);
		}
		return true;
	};
	if (policy == AppLocalProgramPolicy::AdjacentAndNeeded) {
		std::vector<std::filesystem::path> adjacent;
		if (!discovery.Adjacent(&adjacent, error)) { return false; }
		for (const auto& path: adjacent) { if (!map(path)) { return false; } }
	}
	// Snapshot each importer before mapping: LoadProgram can grow m_programs.
	// Only this executable's discovered closure is inspected, never boot/foreign
	// importers, and every new provider's DT_NEEDED participates transitively.
	for (size_t i = 0; i < pending.size(); ++i) {
		const auto& info = *pending[i]->dynamic_info;
		if (!info.needed_valid) { return fail("invalid app-local DT_NEEDED string table reference"); }
		const auto needed = info.needed;
		for (const auto* name: needed) {
			std::filesystem::path path;
			if (name == nullptr) { return fail("null app-local DT_NEEDED basename"); }
			if (!discovery.Resolve(name, &path, error)) { return false; }
			if (!path.empty() && !map(path)) { return false; }
		}
	}
	return true;
}

static bool IsAdjacentModuleFile(const std::string& name) {
	return AppLocalLibraries::IsModuleFile(name);
}

static bool SkipAdjacentModuleFile(const std::string& name) {
	auto lower = Common::ToLower(name);
	return lower == "eboot.bin" || lower == "libkernel.prx" || lower == "libkernel_sys.prx";
}

void RuntimeLinker::PreloadAdjacentPrograms() {
	Common::LockGuard lock(m_mutex);
	CheckStartupMutation();
	if (m_programs.empty()) {
		return;
	}

	std::vector<std::filesystem::path> module_paths;

	auto is_loaded = [this](const std::filesystem::path& path) {
		auto fixed_path = Common::FixFilenameSlash(Common::PathToGenericString(path));
		for (auto* program: m_programs) {
			if (Common::EqualNoCase(
			        Common::FixFilenameSlash(Common::PathToGenericString(program->file_name)),
			        fixed_path)) {
				return true;
			}
		}
		return false;
	};

	auto add_path = [&module_paths, &is_loaded](const std::filesystem::path& path) {
		if (is_loaded(path)) {
			return;
		}
		for (const auto& p: module_paths) {
			if (Common::EqualNoCase(Common::PathToGenericString(p),
			                        Common::PathToGenericString(path))) {
				return;
			}
		}
		module_paths.push_back(path);
	};

	auto add_dir = [&add_path](const std::filesystem::path& dir) {
		if (!Common::File::IsDirectoryExisting(dir)) {
			return;
		}
		for (const auto& entry: Common::File::GetDirEntries(dir)) {
			if (entry.is_file && IsAdjacentModuleFile(entry.name) &&
			    !SkipAdjacentModuleFile(entry.name)) {
				add_path(dir / entry.name);
			}
		}
	};

	auto root = m_programs.at(0)->file_name.parent_path();
	if (root.empty()) {
		return;
	}

	add_dir(root);
	add_dir(root / "sce_module");
	add_dir(root / "sce_modules");

	for (const auto& path: module_paths) {
		auto* program                        = LoadProgram(path);
		program->fail_if_global_not_resolved = false;
		// ps5-vsh: entries run later via StartAllModules in dependency
		// order (eager starts here broke mono/PsmUtil init ordering).
	}
}

void RuntimeLinker::EnsureImportProviders() {
	Common::LockGuard lock(m_mutex);
	CheckStartupMutation();
	if (NativeSslSelected() && !m_ssl_smoke_prepared) { EXIT("Native SSL selection must precede dependency loading\n"); }
	LOGF("EnsureImportProviders: called, programs=%zu\n", m_programs.size());
	// ps5-vsh: which import library is provided by which firmware module file.
	// The 13.60 chain links against libraries whose module file name does not
	// start with the library name, so a prefix match alone never finds them:
	//   libSceJsc       <- libSceJsc.sprx      (13.4 MB, staged but never
	//                                           placed; m9Y2LTyzlQc returns 0
	//                                           for libSceJsc+0x25430 and the
	//                                           JSI bridge at
	//                                           libNativeExtensions+0x57D90
	//                                           then consumes that null)
	//   libSceJsi-0.70  <- libSceJsi-0.70.sprx
	//   libmonosgen     <- libmonosgen-2.0.sprx
	//
	// NOTE on the keys: these must be the RAW DT_OS_IMPORT_LIB names, which are
	// the ELF's own library names ("libSceJsc", "libmonosgen", ...). They are
	// NOT the "[Jsc_v1][Jsc_v1.1][Func]" form printed in "Relocate:" lines - that
	// is SymbolDatabase::GenerateName(), which strips "libSce" and appends "_vN",
	// so a table keyed on "Jsc_v1" never matches import_libs[i].name and the scan
	// silently reports nothing missing (the original bug). Verified against the
	// 13.60 dump: libNativeExtensions exports DT_OS_EXPORT_LIB id25
	// "libNativeExtensions" and imports id1 "libSceJsc", id11 "libSceJsi-0.70";
	// libSceJsc.sprx exports "libSceJsc"; libmonosgen-2.0.sprx exports
	// "libmonosgen".
	static const struct {
		const char* library;
		const char* module_prefix;
	} k_providers[] = {
	    {"libmonosgen", "libmonosgen"},
	    {"libSceJsc", "libSceJsc"},
	    {"libSceJsi-0.70", "libSceJsi-0.70"},
	    // ShellUI calls BVDkopEoLMk to register NKWeb's PUI internal calls.
	    // Without this provider, startup resolves to a stub and Add Profile
	    // later aborts on WebRunLoop::InitializeNative(int&).
	    {"libSceNKWeb", "libSceNKWeb"},
	    // AudiodecCpu is a dispatcher; codec 0x1007 uses Ddp's exported
	    // function table. A zero-return unresolved decode leaves the caller's
	    // PCM capacity unchanged and publishes uninitialized memory as audio.
	    {"libSceAudiodecCpu", "libSceAudiodecCpu"},
	    {"libSceAudiodecCpuDdp", "libSceAudiodecCpuDdp"},
	};
	auto provider_prefix = [](const std::string& lib) -> const char* {
		for (const auto& pr: k_providers) {
			if (Common::EqualNoCase(lib, pr.library)) {
				return pr.module_prefix;
			}
		}
		return nullptr;
	};
	if (m_programs.empty()) {
		return;
	}
	auto is_loaded = [this](const std::filesystem::path& path) {
		auto fixed_path = Common::FixFilenameSlash(Common::PathToGenericString(path));
		for (auto* program: m_programs) {
			if (program != nullptr &&
			    Common::EqualNoCase(
			        Common::FixFilenameSlash(Common::PathToGenericString(program->file_name)),
			        fixed_path)) {
				return true;
			}
		}
		return false;
	};
	auto provides = [this](const std::string& lib) {
		for (auto* p: m_programs) {
			if (p == nullptr || p->dynamic_info == nullptr) {
				continue;
			}
			for (const auto& e: p->dynamic_info->export_libs) {
				if (Common::EqualNoCase(e.name, lib)) {
					return true;
				}
			}
		}
		return false;
	};
	std::vector<std::filesystem::path> search_dirs;
	std::filesystem::path          title_root;
	if (auto* main0 = m_programs.at(0); main0 != nullptr) {
		auto root = main0->file_name.parent_path();
		if (!root.empty()) {
			title_root = root;
			search_dirs.push_back(root);
			search_dirs.push_back(root / "sce_module");
		}
	}
	for (const char* g: {"/common_ex/lib", "/common/lib", "/system/common/lib"}) {
		auto real = Libs::LibKernel::FileSystem::GetRealFilename(g);
		if (!real.empty()) {
			search_dirs.push_back(real);
		}
	}
	for (int iter = 0; iter < 2; iter++) {
		std::vector<std::string> missing;
		int                      candidates = 0;
		for (auto* p: m_programs) {
			if (p == nullptr || p->dynamic_info == nullptr) {
				continue;
			}
			// ps5-vsh: do not restrict the provider scan to the title root.
			// The importers that need a provider live in /common_ex/lib
			// (libNativeExtensions imports libSceJsc and libSceJsi-0.70), so the
			// old filter meant only libmonosgen - which happens to sit under the
			// title root - ever triggered a provider load, and libSceJsc was
			// never placed at all.
			for (const auto& l: p->dynamic_info->import_libs) {
				if (l.name.empty() || provider_prefix(l.name) == nullptr) {
					continue;
				}
				candidates++;
				bool hop = false;
				std::string via;
				for (auto* q: m_programs) {
					if (q != nullptr && q->dynamic_info != nullptr) {
						for (const auto& e: q->dynamic_info->export_libs) {
							if (Common::EqualNoCase(e.name, l.name)) {
								hop = true;
								via   = Common::PathToString(q->file_name);
								break;
							}
						}
					}
					if (hop) {
						break;
					}
				}
				if (!hop) {
				missing.push_back(l.name);
				} else if (Common::ToLower(l.name) != "libmonosgen") {
					LOGF("EnsureImportProviders: lib %s SATISFIED by %s\n", l.name.c_str(), via.c_str());
				}
			}
		}
		// ps5-vsh: a provider can be imported by several programs (libSceJsc by
		// libNativeExtensions only today, but libSceJsi-0.70 by both
		// libNativeExtensions and libhermes-0.11). Dedupe so each file loads once.
		std::sort(missing.begin(), missing.end());
		missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
		// ps5-vsh: this line is the measurement that the scan actually sees the
		// provider imports. Without it the key mismatch above made the function
		// return at missing.empty() with no evidence at all.
		LOGF("EnsureImportProviders: iter=%d programs=%zu provider_candidates=%d missing=%zu\n",
		     iter, m_programs.size(), candidates, missing.size());
		if (missing.empty()) {
			return;
		}
		bool loaded_any = false;
		for (const auto& lib: missing) {
			std::filesystem::path best;
			for (const auto& dir: search_dirs) {
				if (!Common::File::IsDirectoryExisting(dir)) {
					continue;
				}
				for (const auto& entry: Common::File::GetDirEntries(dir)) {
					if (!entry.is_file) {
						continue;
					}
					auto lower = Common::ToLower(entry.name);
					if (!(lower.ends_with(".sprx") || lower.ends_with(".prx"))) {
						continue;
					}
					auto name_lower = Common::ToLower(entry.name);
					auto lib_lower  = Common::ToLower(provider_prefix(lib));
					// Never substitute a codec plugin for its dispatcher when the
					// exact dispatcher file is absent (their names share a prefix).
					if (lib_lower.starts_with("libsceaudiodeccpu") &&
					    name_lower != lib_lower + ".sprx" && name_lower != lib_lower + ".prx") {
						continue;
					}
					if (name_lower.size() < lib_lower.size() ||
					    name_lower.compare(0, lib_lower.size(), lib_lower) != 0) {
						continue;
					}
					auto cand = dir / entry.name;
					if (is_loaded(cand)) {
						continue;
					}
					if (best.empty() || entry.name.size() < best.filename().string().size()) {
						best = cand;
					}
				}
				if (!best.empty()) {
					break;
				}
			}
			if (!best.empty()) {
				LOGF("EnsureImportProviders: loading %s for lib %s\n",
				     Common::PathToString(best).c_str(), lib.c_str());
				auto* program                        = LoadProgram(best);
				// The selected codec must have real dependencies. In contrast,
				// the dispatcher's tables also reference unrelated optional codecs.
				program->fail_if_global_not_resolved = (lib == "libSceAudiodecCpuDdp");
				loaded_any                           = true;
			} else {
				if (lib == "libSceAudiodecCpu" || lib == "libSceAudiodecCpuDdp") {
					EXIT("Required native audio decoder provider unavailable: %s; refusing zero-return decode fallback\n", lib.c_str());
				}
				LOGF("EnsureImportProviders: no file for lib %s\n", lib.c_str());
			}
		}
		if (!loaded_any) {
			return;
		}
		RelocateAll();
	}
}

namespace {
// A nested initializer may start an idle dependency, but must never wait for
// another in-flight initializer/finalizer (including cross-thread cycles).
thread_local unsigned shared_module_depth = 0;
struct SharedModuleDepth {
	SharedModuleDepth() { ++shared_module_depth; }
	~SharedModuleDepth() { --shared_module_depth; }
};
ModuleCallResult SharedCallResult(RuntimeGuestResult guest) {
	ModuleCallResult result;
	result.guest = guest;
	result.status = guest.Returned() ? ModuleCallStatus::Returned :
	    guest.ThreadExited() ? ModuleCallStatus::ThreadExited :
	    guest.failure ? ModuleCallStatus::Aborted : guest.stopped ? ModuleCallStatus::Stopped :
	    guest.call.tag == GuestCallOutcomeTag::Aborted ? ModuleCallStatus::Aborted : ModuleCallStatus::Unavailable;
	if (guest.call.tag == GuestCallOutcomeTag::Returned) { result.value = static_cast<int32_t>(guest.call.value); }
	return result;
}
}

ModuleCallResult RuntimeLinker::StartModuleChecked(Program* program, size_t args, const void* argp, module_func_t func) {
	const auto execution = GetApplicationExecution(program);
	if (!program || !program->application_owner || !execution) { return {}; }
	if (auto pending = execution->PendingThreadExit()) {
		return SharedCallResult({{GuestCallBoundaryStatus::Ok, GuestCallOutcomeTag::NotInvoked, 0},
		    execution->Failure(), execution->StopRequested(), pending});
	}
	auto lease = execution->TryAcquire(GuestWorkKind::Module);
	if (!lease) { return {ModuleCallStatus::Stopped}; }
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		if (std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end() ||
		    !program->dynamic_info || !program->elf || !program->elf->IsValid() || !program->elf->IsShared()) { return {}; }
		AdmitGuestStartup(); // Existing session/native-SSL admission policy is unchanged.
	}
	const auto hard_blockers = GetApplicationHardImportBlockers(program->application_owner);
	if (!hard_blockers.empty() || !PublishApplicationStopText(program->application_owner)) {
		for (size_t i = 0; i < std::min<size_t>(hard_blockers.size(), 16); ++i) {
			LOGF("VSH runtime: module start hard blocker %s\n", hard_blockers[i].c_str());
		}
		execution->Fail(GuestFailureReason::InvalidTransition);
		return {ModuleCallStatus::Failed};
	}
	if (program->dynamic_info->init_vaddr &&
	    !IsApplicationExecutableAddress(program->application_owner, program->base_vaddr + program->dynamic_info->init_vaddr)) {
		execution->Fail(GuestFailureReason::InvalidTransition, program->dynamic_info->init_vaddr);
		return {ModuleCallStatus::Failed};
	}
	std::unique_lock lock(program->module_mutex);
	while (program->module_state == ModuleExecutionState::Starting || program->module_state == ModuleExecutionState::Stopping) {
		if (shared_module_depth || program->module_thread == std::this_thread::get_id()) { return {ModuleCallStatus::Busy}; }
		if (execution->StopRequested()) { return {ModuleCallStatus::Stopped}; }
		// Stop closes admission independently of this module's completion. Bounded
		// recheck avoids stranding an external waiter behind a failed peer.
		program->module_changed.wait_for(lock, std::chrono::milliseconds(10));
	}
	if (execution->StopRequested()) { return {ModuleCallStatus::Stopped}; }
	if (program->module_state == ModuleExecutionState::Started) { return program->module_start_result; }
	if (program->module_state == ModuleExecutionState::Failed) { return program->module_start_result; }
	if (program->module_state == ModuleExecutionState::Stopped) { return {ModuleCallStatus::Failed}; }
	program->module_state = ModuleExecutionState::Starting;
	program->module_thread = std::this_thread::get_id();
	lock.unlock();
	SharedModuleDepth depth;
	// No loader/module/owner mutex is held across this actual guest invocation.
	auto result = program->dynamic_info->init_vaddr == 0 ?
	    ModuleCallResult {ModuleCallStatus::Returned, 0} :
	    SharedCallResult(execution->InvokeModule(program->base_vaddr + program->dynamic_info->init_vaddr,
	        args, argp, reinterpret_cast<uint64_t>(func)));
	if (result.Returned() && result.value < 0) {
		// A returned negative output is NOT a boundary abort; retain its tag and
		// output, but do not publish an initialized image or successful handle.
		execution->Fail(GuestFailureReason::InitializerRejected, static_cast<uint32_t>(result.value));
	}
	lock.lock();
	program->initializer_result = result.value;
	program->initializer_returned = result.guest.call.tag == GuestCallOutcomeTag::Returned;
	bool published = false;
	if (result.Returned() && result.value >= 0) {
		published = lease.Publish([&] {
			program->module_state = ModuleExecutionState::Started;
			program->module_start_result = result;
			program->started = true; // Publish completion fields before the success flag.
		});
	}
	if (!published) {
		if (result.Returned() && result.value >= 0) {
			result.status = execution->Failure() ? ModuleCallStatus::Aborted : ModuleCallStatus::Stopped;
		}
		program->started = false;
		program->module_state = ModuleExecutionState::Failed;
		// A thread exit belongs only to this actual-thread invocation. The
		// incomplete module stays failed, but never replay an old thread's exit
		// request to another caller waiting for startup completion.
		program->module_start_result = result;
		program->module_start_result.guest.thread_exit = {}; // Also strip exit+stop/failure combinations.
		if (result.ThreadExited()) { program->module_start_result = {ModuleCallStatus::Failed}; }
	}
	program->module_thread = {};
	program->module_changed.notify_all();
	return result;
}

ModuleCallResult RuntimeLinker::StopModuleChecked(Program* program, size_t args, const void* argp, module_func_t func) {
	const auto execution = GetApplicationExecution(program);
	if (!program || !program->application_owner || !execution) { return {}; }
	if (auto pending = execution->PendingThreadExit()) {
		return SharedCallResult({{GuestCallBoundaryStatus::Ok, GuestCallOutcomeTag::NotInvoked, 0},
		    execution->Failure(), execution->StopRequested(), pending});
	}
	auto lease = execution->TryAcquire(GuestWorkKind::Module);
	if (!lease) { return {ModuleCallStatus::Stopped}; } // Fatal cleanup must NOT call guest fini.
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		if (std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end() ||
		    !program->dynamic_info || !program->elf || !program->elf->IsValid() || !program->elf->IsShared()) { return {}; }
		AdmitGuestStartup();
	}
	if (!GetApplicationHardImportBlockers(program->application_owner).empty() ||
	    !PublishApplicationStopText(program->application_owner)) {
		execution->Fail(GuestFailureReason::InvalidTransition);
		return {ModuleCallStatus::Failed};
	}
	if (program->dynamic_info->fini_vaddr &&
	    !IsApplicationExecutableAddress(program->application_owner, program->base_vaddr + program->dynamic_info->fini_vaddr)) {
		execution->Fail(GuestFailureReason::InvalidTransition, program->dynamic_info->fini_vaddr);
		return {ModuleCallStatus::Failed};
	}
	std::unique_lock lock(program->module_mutex);
	while (program->module_state == ModuleExecutionState::Starting || program->module_state == ModuleExecutionState::Stopping) {
		if (shared_module_depth || program->module_thread == std::this_thread::get_id()) { return {ModuleCallStatus::Busy}; }
		if (execution->StopRequested()) { return {ModuleCallStatus::Stopped}; }
		program->module_changed.wait_for(lock, std::chrono::milliseconds(10));
	}
	if (program->module_state != ModuleExecutionState::Started || execution->StopRequested()) { return {ModuleCallStatus::Stopped}; }
	if (program->load_count > 1) { return {ModuleCallStatus::Busy}; }
	program->module_state = ModuleExecutionState::Stopping;
	program->module_thread = std::this_thread::get_id();
	lock.unlock();
	SharedModuleDepth depth;
	auto result = program->dynamic_info->fini_vaddr == 0 ? ModuleCallResult {ModuleCallStatus::Returned, 0} :
	    SharedCallResult(execution->InvokeModule(program->base_vaddr + program->dynamic_info->fini_vaddr,
	        args, argp, reinterpret_cast<uint64_t>(func)));
	lock.lock();
	const bool published = result.Returned() && lease.Publish([&] {
		program->module_state = ModuleExecutionState::Stopped;
	});
	if (!published) {
		program->module_state = ModuleExecutionState::Failed;
		if (result.Returned()) { result.status = ModuleCallStatus::Aborted; }
	}
	program->started = false;
	program->module_thread = {};
	program->module_changed.notify_all();
	// The session PthreadDeleteStaticObjects tail is fatal-on-busy. Quarantine
	// these host records with the pinned mapping instead; owner-safe pthread
	// reclamation belongs to the independently implemented cleanup route.
	return result;
}

int RuntimeLinker::LoadStartModuleReference(Program* program, size_t args, const void* argp, int* output) {
	using namespace Libs::LibKernel;
	if (output) { *output = 0; }
	if (program && !program->application_owner) {
		const auto* requester = GetCurrentProgram();
		if (requester && requester->application_owner) { return KERNEL_ERROR_EPERM; }
		const int result = StartModule(program, args, argp, nullptr);
		std::lock_guard lock(program->module_mutex);
		// Reentrant admission is Busy, not initializer completion. Preserve an
		// actual negative initializer result on every later load attempt.
		if (!program->initializer_returned) { return result < 0 ? result : KERNEL_ERROR_EBUSY; }
		if (output) { *output = program->initializer_result; }
		if (program->initializer_result < 0) { return KERNEL_ERROR_ENOEXEC; }
		if (!program->started || program->load_count == UINT32_MAX) { return KERNEL_ERROR_EBUSY; }
		++program->load_count;
		return program->unique_id;
	}
	const auto execution = GetApplicationExecution(program);
	if (!execution) { return KERNEL_ERROR_ENOSYS; }
	auto lease = execution->TryAcquire(GuestWorkKind::Module);
	if (!lease) { return KERNEL_ERROR_ECANCELED; }
	const auto result = StartModuleChecked(program, args, argp, nullptr);
	if (result.Returned() && output) { *output = result.value; }
	if (result.Returned() && result.value < 0) { return KERNEL_ERROR_ENOEXEC; }
	if (!result.Returned()) {
		return result.status == ModuleCallStatus::Busy ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_EIO;
	}
	std::lock_guard lock(program->module_mutex);
	if (program->module_state != ModuleExecutionState::Started || program->load_count == UINT32_MAX) { return KERNEL_ERROR_EBUSY; }
	if (!lease.Publish([&] { ++program->load_count; })) { return KERNEL_ERROR_ECANCELED; }
	return program->unique_id;
}

void RuntimeLinker::RollbackModuleReference(Program* program) {
	if (!program) { return; }
	if (!program->application_owner) {
		std::lock_guard lock(program->module_mutex);
		if (program->load_count) { --program->load_count; }
		return;
	}
	std::lock_guard lock(program->module_mutex);
	if (program->load_count) { --program->load_count; }
	const auto execution = GetApplicationExecution(program);
	if (execution && execution->StopRequested()) {
		program->started = false;
		program->module_state = ModuleExecutionState::Failed;
		program->module_changed.notify_all();
	}
}

int RuntimeLinker::StopModuleReference(Program* program, size_t args, const void* argp, int* output) {
	using namespace Libs::LibKernel;
	if (output) { *output = 0; }
	const auto execution = GetApplicationExecution(program);
	if (!execution) { return KERNEL_ERROR_ENOSYS; }
	auto lease = execution->TryAcquire(GuestWorkKind::Module);
	if (!lease) { return KERNEL_ERROR_ECANCELED; }
	{
		std::lock_guard lock(program->module_mutex);
		if (!program->load_count) { return KERNEL_ERROR_ESRCH; }
		if (program->load_count > 1) {
			return lease.Publish([&] { --program->load_count; }) ? 0 : KERNEL_ERROR_ECANCELED;
		}
	}
	const auto result = StopModuleChecked(program, args, argp, nullptr);
	std::lock_guard lock(program->module_mutex);
	if (result.Returned() && output) { *output = result.value; }
	if (!result.Returned()) {
		if (execution->StopRequested()) { program->load_count = 0; }
		else if (result.status == ModuleCallStatus::Busy && program->module_state == ModuleExecutionState::Started &&
		         program->load_count > 1) {
			return lease.Publish([&] { --program->load_count; }) ? 0 : KERNEL_ERROR_ECANCELED;
		}
		return result.status == ModuleCallStatus::Busy ? KERNEL_ERROR_EBUSY : KERNEL_ERROR_EIO;
	}
	// Like firmware, finalizer output is informational, even when negative.
	program->load_count = 0;
	return 0;
}

int RuntimeLinker::StartModule(Program* program, size_t args, const void* argp,
                               module_func_t func) {
	if (program && program->application_owner) {
		const auto result = StartModuleChecked(program, args, argp, func);
		return result.Returned() ? result.value : Libs::LibKernel::KERNEL_ERROR_EIO;
	}
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());
		EXIT_IF(program == nullptr);
		EXIT_IF(program->dynamic_info == nullptr);
		EXIT_IF(program->elf == nullptr);
		EXIT_IF(!program->elf->IsShared());
		if (NativeSslSelected() && !m_ssl_production_ready) {
			const unsigned provider = program == m_ssl_smoke_libc ? 0 : program == m_ssl_smoke_ssl ? 1 : 2;
			if (!m_ssl_smoke_frozen || program->started || !m_ssl_provider_startup.Begin(provider,
			    m_ssl_smoke_thread == static_cast<uint64_t>(Common::Thread::GetThreadIdUnique()), !args && !argp && !func)) {
				EXIT("Native SSL initializer admission rejected (replay/order/thread/arguments)\n");
			}
		}

		AdmitGuestStartup(NativeSslSelected() && !m_ssl_production_ready);
	}
	{
		std::unique_lock lock(program->module_mutex);
		while (program->module_state == ModuleExecutionState::Starting) {
			// Never wait on ourselves or a nested initializer dependency cycle.
			if (shared_module_depth || program->module_thread == std::this_thread::get_id()) {
				return Libs::LibKernel::KERNEL_ERROR_EBUSY;
			}
			program->module_changed.wait(lock);
		}
		if (program->module_state == ModuleExecutionState::Stopping || program->module_state == ModuleExecutionState::Stopped) {
			return Libs::LibKernel::KERNEL_ERROR_EBUSY;
		}
		if (program->initializer_returned) { return program->initializer_result; }
		if (program->module_state != ModuleExecutionState::NotStarted) { return Libs::LibKernel::KERNEL_ERROR_EBUSY; }
		program->module_state = ModuleExecutionState::Starting;
		program->module_thread = std::this_thread::get_id();
	}

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Start module: %s\n---\n",
	           Common::PathToString(program->file_name).c_str());

	SharedModuleDepth depth;
	const int result = program->dynamic_info->init_vaddr == 0 ? 0 :
	    reinterpret_cast<module_ini_fini_func_t>(program->dynamic_info->init_vaddr + program->base_vaddr)(args, argp, func);
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		if (NativeSslSelected() && program->native_ssl_smoke_dependency) {
			EXIT_IF(!m_ssl_provider_startup.Complete(result));
		}
	}
	{
		std::lock_guard lock(program->module_mutex);
		program->initializer_result = result;
		program->initializer_returned = true;
		program->started = result >= 0;
		program->module_state = result >= 0 ? ModuleExecutionState::Started : ModuleExecutionState::Failed;
		program->module_thread = {};
		program->module_changed.notify_all();
	}
	return result;
}

int RuntimeLinker::StopModule(Program* program, size_t args, const void* argp, module_func_t func) {
	return StopModule(program, args, argp, func, nullptr);
}

int RuntimeLinker::StopModule(Program* program, size_t args, const void* argp, module_func_t func, bool* finalized) {
	if (finalized) { *finalized = false; }
	if (program && program->application_owner) {
		const auto result = StopModuleChecked(program, args, argp, func);
		if (finalized) { *finalized = result.Returned(); }
		return result.Returned() ? result.value : Libs::LibKernel::KERNEL_ERROR_EIO;
	}
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(m_mutex);
		if (m_ssl_smoke_prepared && (program == m_ssl_smoke_libc || program == m_ssl_smoke_ssl)) {
			EXIT("Stopping a pinned native SSL provider is prohibited\n");
		}
		AdmitGuestStartup();
	}
	EXIT_IF(program == nullptr);
	EXIT_IF(program->dynamic_info == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(!program->elf->IsShared());

	EXIT_IF(std::find(m_programs.begin(), m_programs.end(), program) == m_programs.end());

	{
		std::lock_guard lock(program->module_mutex);
		if (program->module_state != ModuleExecutionState::Started || (finalized && program->load_count > 1)) {
			return Libs::LibKernel::KERNEL_ERROR_EBUSY;
		}
		// Close startup/reference admission before entering the actual finalizer.
		// Reload requires a fresh mapping: native singleton guards need not reset.
		program->module_state = ModuleExecutionState::Stopping;
		program->module_thread = std::this_thread::get_id();
		program->started = false;
	}

	LOGF_COLOR(Log::Color::BrightYellow, "---\n--- Stop module: %s\n---\n",
	           Common::PathToString(program->file_name).c_str());

	SharedModuleDepth depth;
	const int result = program->dynamic_info->fini_vaddr == 0 ? 0 :
	    reinterpret_cast<module_ini_fini_func_t>(program->dynamic_info->fini_vaddr + program->base_vaddr)(args, argp, func);

	Libs::LibKernel::PthreadDeleteStaticObjects(program);
	{
		std::lock_guard lock(program->module_mutex);
		program->module_state = ModuleExecutionState::Stopped;
		program->module_thread = {};
		program->module_changed.notify_all();
	}
	if (finalized) { *finalized = true; }

	return result;
}

uint8_t* RuntimeLinker::TlsGetAddr(Program* program) {
	EXIT_IF(program == nullptr);

	Common::LockGuard lock(program->tls.mutex);

	auto& tls = program->tls.tlss[Common::Thread::GetThreadIdUnique()];

	if (tls.ptr == nullptr) {
		constexpr uint64_t TCB_SIZE  = 0x40;
		constexpr uint64_t TCB_ALIGN = 0x20;

		const auto tcb_offset =
		    program->tls.tcb_offset != 0 ? program->tls.tcb_offset : program->tls.image_size;
		const auto alloc_size = AlignUp(tcb_offset, TCB_ALIGN) + TCB_SIZE;
		tls.ptr        = reinterpret_cast<uint8_t*>(Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    0, alloc_size, Common::VirtualMemory::Mode::ReadWrite, "thread_local_storage"));
		tls.free_func  = nullptr;
		tls.vm_alloc   = true;
		tls.alloc_size = alloc_size;

		EXIT_IF(tls.ptr == nullptr);

		std::memset(tls.ptr, 0, alloc_size);

		if (!program->tls.init_image.empty()) {
			std::memcpy(tls.ptr, program->tls.init_image.data(), program->tls.init_image.size());
		} else {
			std::memcpy(tls.ptr, reinterpret_cast<void*>(program->tls.image_vaddr),
			            program->tls.init_size);
		}

		auto* tcb = reinterpret_cast<uint64_t*>(tls.ptr + tcb_offset);
		tcb[0]    = reinterpret_cast<uint64_t>(tcb);
	}

	return tls.ptr;
}

void RuntimeLinker::DeleteTls(Program* program, int thread_id, bool allow_guest_callbacks) {
	EXIT_IF(program == nullptr);

	if (thread_id == Common::Thread::GetThreadIdUnique() && g_tls_cached_main_program == program) {
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb     = nullptr;
	}

	Common::LockGuard lock(program->tls.mutex);

	if (auto it = program->tls.tlss.find(thread_id); it != program->tls.tlss.end()) {
		if (!allow_guest_callbacks && it->second.free_func != nullptr) {
			// Keep the allocation and its callback identity pinned. Dropping the
			// record would lose ownership; invoking it could reenter a failed guest.
			return;
		}
		FreeTlsBlock(&it->second);
		program->tls.tlss.erase(it);
	}
}

constexpr uint64_t GUEST_PAGE_SIZE = 0x4000;

static uint64_t CalcBaseSize(const Elf64_Ehdr* ehdr, const Elf64_Phdr* phdr) {
	uint64_t base_size = 0;
	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t last_addr = phdr[i].p_vaddr + GetAlignedSize(phdr + i);
			if (last_addr > base_size) {
				base_size = last_addr;
			}
		}
	}
	return base_size;
}

// A module's E, R and RW LOAD segments use flexible memory in 16 KiB pages; GNU_RELRO adds none
// since it lies inside an RW LOAD.
uint64_t CalcFlexibleMemorySize(const Elf64_Phdr* phdr, Elf64_Half phnum) {
	uint64_t size = 0;
	for (Elf64_Half i = 0; i < phnum; i++) {
		const auto flags = phdr[i].p_flags;
		if (phdr[i].p_type == PT_LOAD &&
		    (flags == PF_X || flags == PF_R || flags == (PF_R | PF_W))) {
			size += AlignUp(phdr[i].p_memsz, GUEST_PAGE_SIZE);
		}
	}
	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RuntimeLinker::LoadProgramToMemory(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr || program->base_vaddr != 0 || program->base_size != 0 ||
	        program->elf == nullptr);

	// static uint64_t desired_base_addr = DESIRED_BASE_ADDR;

	bool is_shared   = program->elf->IsShared();
	bool is_next_gen = program->elf->IsNextGen();

	EXIT_NOT_IMPLEMENTED(!is_shared && !is_next_gen);

	const auto* ehdr = program->elf->GetEhdr();
	const auto* phdr = program->elf->GetPhdr();

	EXIT_IF(phdr == nullptr || ehdr == nullptr);
	// Admission loads from one retained same-open-file snapshot, never a later
	// path reopen. Size only selects a candidate; SHA256 + mapped-byte checks
	// below are the authority. No capabilities are inferred from a file name.
	std::vector<uint8_t> credential_source;
	const char* credential_path = std::getenv("KYTY_APPDB_CORE_AUTH_RECORD");
	if (credential_path && *credential_path && !is_shared && !program->application_owner) {
		program->elf->SnapshotPlainFile(35337516, &credential_source);
	}
	if (credential_source.empty() && !is_shared && !program->application_owner) {
		const char* ui_record = std::getenv("KYTY_APPDB_UI_AUTH_RECORD");
		if (ui_record && *ui_record && program->elf->SnapshotPlainFile(21878244, &credential_source)) { credential_path = ui_record; }
	}

	program->base_size = CalcBaseSize(ehdr, phdr);
	EXIT_IF(program->base_size > UINT64_MAX - (GUEST_PAGE_SIZE - 1));
	program->base_size_aligned = AlignUp(program->base_size, GUEST_PAGE_SIZE);

	uint64_t tls_handler_size = is_shared ? 0 : Jit::SafeCall::GetSize();
	EXIT_IF(tls_handler_size > UINT64_MAX - program->base_size_aligned);
	program->mapped_size = program->base_size_aligned + tls_handler_size;
	const bool emulate_amd = Config::AmdCpuEnabled();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const bool protect_memory_faults = Config::RedZoneProtectionEnabled();
#else
	const bool protect_memory_faults = false;
#endif
	const uint64_t tsc_multiplier =
	    Config::GuestTscEnabled() ? Libs::LibKernel::KernelGuestTscMultiplier() : 0;
	const bool patch_guest_instructions =
	    protect_memory_faults || emulate_amd || tsc_multiplier != 0;

	constexpr uint64_t INSTRUCTION_TRAMPOLINE_SIZE = 8u * 1024u * 1024u;
	if (patch_guest_instructions) {
		EXIT_IF(INSTRUCTION_TRAMPOLINE_SIZE > UINT64_MAX - program->mapped_size);
		program->mapped_size += INSTRUCTION_TRAMPOLINE_SIZE;
	}

	// More than 112 co-loaded modules exhaust the initial system code slots.
	// User GPU arenas start at 0x1000000000 and remap their temporarily free
	// holes later. Place overflow modules in a separate high address region,
	// rather than inserting AOT metadata into a live compositor arena.
	constexpr uint64_t USER_ARENA_START    = 0x1000000000ull;
	constexpr uint64_t OVERFLOW_CODE_START = 0xe000000000ull;
	if (g_desired_base_addr < OVERFLOW_CODE_START &&
	    (g_desired_base_addr >= USER_ARENA_START ||
	     program->mapped_size > USER_ARENA_START - g_desired_base_addr)) {
		g_desired_base_addr = OVERFLOW_CODE_START;
	}

	program->base_vaddr = Libs::LibKernel::Memory::AllocateProgramMemory(
	    g_desired_base_addr, program->mapped_size, Common::VirtualMemory::Mode::ExecuteReadWrite,
	    Common::PathToString(program->file_name.filename()).c_str());
	EXIT_IF(program->base_vaddr == 0);
	program->flexible_size = CalcFlexibleMemorySize(phdr, ehdr->e_phnum);
	Libs::LibKernel::Memory::ChargeFlexibleMemory(program->flexible_size);

	if (patch_guest_instructions) {
		const auto trampoline_addr           = program->base_vaddr + program->base_size_aligned;
		program->instruction_trampoline_size = INSTRUCTION_TRAMPOLINE_SIZE;
		RegisterGuestInstructionPatchModule(
		    reinterpret_cast<void*>(program->base_vaddr), program->base_size_aligned,
		    reinterpret_cast<void*>(trampoline_addr), program->instruction_trampoline_size);
	}
	if (!is_shared) {
		program->tls.handler_vaddr =
		    program->base_vaddr + program->base_size_aligned + program->instruction_trampoline_size;
	}

	// ps5-vsh: co-hosted mains share one address space but PS5 ELFs
	// assume the full low va space (ShellCore spans 0x2409000), so pack
	// each program past the previous one's actual mapped end. Verified
	// non-overlapping (Shell 0x090, SysCore 0x091, JSCd 0x092, ...).
	// Placement trace below makes future collisions obvious.
	LOGF("ps5-vsh: place %s base=0x%016" PRIx64 " mapped=0x%016" PRIx64
	     " desired-in=0x%016" PRIx64 "\n",
	     Common::PathToString(program->file_name.filename()).c_str(), program->base_vaddr,
	     program->mapped_size, g_desired_base_addr);
	g_desired_base_addr =
	    AlignUp(program->base_vaddr + program->mapped_size, CODE_BASE_INCR);
	LOGF("ps5-vsh: place desired-out=0x%016" PRIx64 "\n", g_desired_base_addr);

	EXIT_IF(program->base_size_aligned < program->base_size);
	LOGF("base_vaddr             = 0x%016" PRIx64 "\n"
	     "base_size              = 0x%016" PRIx64 "\n"
	     "base_size_aligned      = 0x%016" PRIx64 "\n"
	     "mapped_size            = 0x%016" PRIx64 "\n",
	     program->base_vaddr, program->base_size, program->base_size_aligned, program->mapped_size);
	if (!is_shared) {
		LOGF("tls_handler_size       = 0x%016" PRIx64 "\n", tls_handler_size);
	}

	if (!Common::HostException::InstallHandler(KytyExceptionHandler)) {
		EXIT("Failed to install the required vectored exception handler\n");
	}

	std::vector<std::pair<uint64_t, uint64_t>> executable_segments;
	uint64_t                                   eh_frame_header_addr = 0;
	uint64_t                                   eh_frame_header_size = 0;
	uint64_t                                   proc_param_file_offset = 0;
	uint64_t                                   proc_param_file_size = 0;
	program->process_abi = ReadProcessAbi(ehdr->e_ident[EI_ABIVERSION], is_shared);

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_memsz != 0 && (phdr[i].p_type == PT_LOAD || phdr[i].p_type == PT_OS_RELRO)) {
			uint64_t segment_addr        = phdr[i].p_vaddr + program->base_vaddr;
			uint64_t segment_file_size   = phdr[i].p_filesz;
			uint64_t segment_memory_size = GetAlignedSize(phdr + i);
			auto     mode                = GetMode(phdr[i].p_flags);

			LOGF("[%d] addr        = 0x%016" PRIx64 "\n"
			     "[%d] file_size   = %" PRIu64 "\n"
			     "[%d] memory_size = %" PRIu64 "\n"
			     "[%d] mode        = %s\n",
			     i, segment_addr, i, segment_file_size, i, segment_memory_size, i,
			     magic_enum::enum_name(mode));

			if (credential_source.empty()) {
				program->elf->LoadSegment(segment_addr, phdr[i].p_offset, segment_file_size);
			} else {
				EXIT_IF(phdr[i].p_offset > credential_source.size() || segment_file_size > credential_source.size() - phdr[i].p_offset);
				std::memcpy(reinterpret_cast<void*>(segment_addr), credential_source.data() + phdr[i].p_offset, segment_file_size);
			}

			bool skip_protect = (phdr[i].p_type == PT_LOAD && is_next_gen &&
			                     mode == Common::VirtualMemory::Mode::NoAccess);

			if (Common::VirtualMemory::IsExecute(mode)) {
				if (credential_source.empty()) { PatchProgram(program, segment_addr, segment_memory_size); }
				executable_segments.emplace_back(segment_addr, segment_file_size);
			}

			if (!skip_protect && credential_source.empty()) {
				Libs::LibKernel::Memory::SetProgramMemoryProtection(segment_addr,
				                                                    segment_memory_size, mode);

				if (Common::VirtualMemory::IsExecute(mode)) {
					Common::VirtualMemory::FlushInstructionCache(segment_addr, segment_memory_size);
				}
			}
		}

		if (phdr[i].p_type == PT_TLS) {
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->tls.image_vaddr = phdr[i].p_vaddr + program->base_vaddr;
			program->tls.init_size   = std::min(phdr[i].p_filesz, GetAlignedSize(phdr + i));
			program->tls.image_size  = GetAlignedSize(phdr + i);
			program->tls.tcb_offset  = program->tls.image_size;

			LOGF("tls addr = 0x%016" PRIx64 "\n"
			     "tls init   = %" PRIu64 "\n"
			     "tls size   = %" PRIu64 "\n"
			     "tls offset = %" PRIu64 "\n",
			     program->tls.image_vaddr, program->tls.init_size, program->tls.image_size,
			     program->tls.tcb_offset);
		}

		if (phdr[i].p_type == PT_OS_PROCPARAM) {
			EXIT_IF(program->proc_param_vaddr != 0);
			EXIT_IF(phdr[i].p_vaddr >= program->base_size);

			program->proc_param_vaddr = phdr[i].p_vaddr + program->base_vaddr;
			if (phdr[i].p_memsz >= 0x14 && program->base_size - phdr[i].p_vaddr >= 0x14) {
				proc_param_file_offset = phdr[i].p_offset;
				proc_param_file_size = std::min({phdr[i].p_filesz, phdr[i].p_memsz,
				                                 program->base_size - phdr[i].p_vaddr});
			}
		}

		if (patch_guest_instructions && phdr[i].p_type == PT_GNU_EH_FRAME) {
			eh_frame_header_addr = phdr[i].p_vaddr + program->base_vaddr;
			eh_frame_header_size = phdr[i].p_memsz;
		}
	}

	if (!credential_source.empty()) {
		std::ifstream input(credential_path, std::ios::binary);
		std::array<uint8_t, 136> record {};
		input.read(reinterpret_cast<char*>(record.data()), record.size());
		const bool complete_record = input.gcount() == record.size() && input.peek() == std::char_traits<char>::eof();
		std::vector<BootMappedSegment> mapped;
		for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
			if (phdr[i].p_type == PT_LOAD) {
				mapped.push_back({phdr[i].p_vaddr, {reinterpret_cast<const uint8_t*>(program->base_vaddr + phdr[i].p_vaddr),
				                                    static_cast<size_t>(phdr[i].p_memsz)}});
			}
		}
		EXIT_IF(!complete_record || !AdmitSourceBoundBootCredentials(program, credential_source, mapped, record));
		// Only the checked host ABI transforms in PatchProgram (diagnostic FS
		// stores and TLS reads) apply to this principal. Name-based behavior
		// patches are excluded. Normal relocation/import resolution follows.
		for (Elf64_Half i = 0; i < ehdr->e_phnum; ++i) {
			if (!phdr[i].p_memsz || (phdr[i].p_type != PT_LOAD && phdr[i].p_type != PT_OS_RELRO)) { continue; }
			const auto address = program->base_vaddr + phdr[i].p_vaddr;
			const auto size = GetAlignedSize(phdr + i);
			const auto mode = GetMode(phdr[i].p_flags);
			if (Common::VirtualMemory::IsExecute(mode)) { PatchProgram(program, address, size); }
			if (mode != Common::VirtualMemory::Mode::NoAccess) {
				Libs::LibKernel::Memory::SetProgramMemoryProtection(address, size, mode);
				if (Common::VirtualMemory::IsExecute(mode)) { Common::VirtualMemory::FlushInstructionCache(address, size); }
			}
		}
		LOGF("AppDb: pinned %s admitted; native mailbox policy NOT executed\n",
		     program->boot_credential_admission->IsCoreImage() ? "Core" : "UI");
	}

	// Native exec snapshots SDK fields from executable process parameters.
	// Read its bounded initial image once; later guest writes must not change the
	// process query, and module parameters are not a fallback source.
	if (!is_shared && proc_param_file_size >= 0x14) {
		std::array<uint8_t, 0x18> header {};
		uint32_t read = 0;
		if (credential_source.empty()) {
			program->elf->LoadSegment(reinterpret_cast<uint64_t>(header.data()), proc_param_file_offset,
			                         std::min<uint64_t>(proc_param_file_size, header.size()), &read);
		} else {
			read = static_cast<uint32_t>(std::min<uint64_t>(proc_param_file_size, header.size()));
			EXIT_IF(proc_param_file_offset > credential_source.size() || read > credential_source.size() - proc_param_file_offset);
			std::memcpy(header.data(), credential_source.data() + proc_param_file_offset, read);
		}
		const auto bytes = std::span<const uint8_t>(header).first(read);
		program->compiled_sdk_version = ReadCompiledSdkVersion(bytes);
		if (program->process_abi == ProcessAbi::Prospero) {
			program->prospero_compiled_sdk_version = ReadProsperoCompiledSdkVersion(bytes);
		}
	}

	if (patch_guest_instructions) {
		std::vector<uintptr_t> function_starts;
		const bool             have_function_starts =
		    DecodeEhFrameFunctionStarts(eh_frame_header_addr, eh_frame_header_size,
		                                &function_starts) &&
		    !function_starts.empty();
		const auto module_name = Common::PathToString(program->file_name.filename());
		if (!have_function_starts) {
			Log::WriteToConsoleAndLog(fmt::format(
			    "Guest instruction patches: {} not applied (function boundaries unavailable)\n",
			    module_name));
		}
		GuestInstructionPatchResult totals {};
		for (const auto& [segment_addr, segment_size]: executable_segments) {
			const auto result = PatchGuestInstructions(
			    segment_addr, segment_size, function_starts, protect_memory_faults, emulate_amd,
			    GetGuestInstructionHostFeatures(), tsc_multiplier);
			totals.reciprocal_sqrt += result.reciprocal_sqrt;
			totals.extrq += result.extrq;
			totals.insertq += result.insertq;
			totals.rdpid += result.rdpid;
			totals.clwb += result.clwb;
			totals.rdtsc += result.rdtsc;
			if (protect_memory_faults) {
				LOGF("Windows guest red-zone patching: %s, functions=%" PRIu64 ", red_zone=%" PRIu64
				     ", memory=%" PRIu64 ", patched=%" PRIu64 ", short=%" PRIu64 ", stack=%" PRIu64
				     ", control=%" PRIu64 ", unrelocatable=%" PRIu64 "\n",
				     module_name.c_str(),
				     result.function_count, result.red_zone_function_count,
				     result.memory_instruction_count, result.patched_memory_instruction_count,
				     result.short_memory_instruction_count,
				     result.stack_dependent_memory_instruction_count,
				     result.control_flow_memory_instruction_count,
				     result.unrelocatable_memory_instruction_count);
			}
		}
		if (totals.rdtsc.found != 0) {
			Log::WriteToConsoleAndLog(
			    fmt::format("Guest TSC: {} (RDTSC/RDTSCP: native={}, skipped={})\n", module_name,
			                totals.rdtsc.native, totals.rdtsc.Skipped()));
		}
		if (emulate_amd && have_function_starts) {
			InstructionPatchCounts combined {};
			std::string            details;
			for (const auto& [name, counts]: {std::pair {"VRSQRTPS", totals.reciprocal_sqrt},
			                                  {"EXTRQ", totals.extrq},
			                                  {"INSERTQ", totals.insertq},
			                                  {"RDPID", totals.rdpid},
			                                  {"CLWB", totals.clwb}}) {
				combined += counts;
				if (!details.empty()) details += "; ";
				details += fmt::format("{}: native={}, trapped={}, skipped={}", name, counts.native,
				                       counts.trapped, counts.Skipped());
			}
			const auto  found   = combined.found;
			const auto  skipped = combined.Skipped();
			const char* status  = found == 0         ? "no matching instructions"
			                      : skipped == found ? "not patched"
			                      : skipped != 0     ? "partially patched"
			                                         : "patched";
			Log::WriteToConsoleAndLog(
			    fmt::format("AMD CPU compatibility: {} {} ({})\n", module_name, status, details));
		}
	}

	if (!is_shared) {
		SetupTlsHandler(program);
	}

	Libs::LibKernel::SemaphoreProvenance::Placement(program->elf.get(), program->file_name,
	    program->base_vaddr, program->base_size, program->unique_id);
	LOGF("entry = 0x%016" PRIx64 "\n", program->elf->GetEntry() + program->base_vaddr);
}

void RuntimeLinker::DeleteProgram(Program* p) {
	EXIT_IF(!Libs::UserService::ReleaseNativeUserServiceBackend(p));
	Libs::LibKernel::SemaphoreProvenance::Unloading(p->unique_id);
	auto program = std::unique_ptr<Program>(p);
	NativeAllocatorFatal::Unregister(program->base_vaddr, program->mapped_size);
	if (g_tls_main_program == program.get()) {
		g_tls_main_program = nullptr;
	}
	if (g_tls_cached_main_program == program.get()) {
		g_tls_cached_main_program = nullptr;
		g_tls_cached_main_tcb     = nullptr;
	}
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_tls_handler_ranges_mutex);
		std::erase_if(g_tls_handler_ranges,
		              [&](const auto& range) { return range.second == program.get(); });
	}
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_stubbed_imports_mutex);
		for (auto& record: g_stubbed_imports) {
			if (record.patch_vaddr >= program->base_vaddr &&
			    record.patch_vaddr < program->base_vaddr + program->mapped_size) {
				record.patch_vaddr = 0;
			}
		}
	}

	if (program->base_vaddr != 0 || program->mapped_size != 0) {
		EXIT_IF(program->base_vaddr == 0 || program->mapped_size == 0);
		if (program->instruction_trampoline_size != 0) {
			UnregisterGuestInstructionPatchModule(reinterpret_cast<void*>(program->base_vaddr));
		}
		EXIT_IF(
		    !Libs::LibKernel::Memory::FreeGuestMemory(program->base_vaddr, program->mapped_size));
		Libs::LibKernel::Memory::ReleaseFlexibleMemory(program->flexible_size);
	}
}

void RuntimeLinker::ParseProgramDynamicInfo(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->dynamic_info != nullptr);

	program->dynamic_info = std::make_unique<DynamicInfo>();

	auto* elf = program->elf.get();

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_HASH) && elf->HasDynValue(DT_HASH));
	GetDynDataOs(elf, &program->dynamic_info->hash_table, DT_OS_HASH);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->hash_table, DT_HASH);
	GetDynValue(elf, &program->dynamic_info->hash_table_size, DT_OS_HASHSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRTAB) && elf->HasDynValue(DT_STRTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_STRSZ) && elf->HasDynValue(DT_STRSZ));
	GetDynDataOs(elf, &program->dynamic_info->str_table, DT_OS_STRTAB);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->str_table, DT_STRTAB);
	GetDynValue(elf, &program->dynamic_info->str_table_size, DT_OS_STRSZ);
	GetDynValue(elf, &program->dynamic_info->str_table_size, DT_STRSZ);

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMTAB) && elf->HasDynValue(DT_SYMTAB));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_SYMENT) && elf->HasDynValue(DT_SYMENT));
	GetDynDataOs(elf, &program->dynamic_info->symbol_table, DT_OS_SYMTAB);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->symbol_table, DT_SYMTAB);
	GetDynValue(elf, &program->dynamic_info->symbol_table_total_size, DT_OS_SYMTABSZ);
	GetDynValue(elf, &program->dynamic_info->symbol_table_entry_size, DT_OS_SYMENT);
	GetDynValue(elf, &program->dynamic_info->symbol_table_entry_size, DT_SYMENT);

	GetDynPtr(elf, &program->dynamic_info->init_vaddr, DT_INIT);
	GetDynPtr(elf, &program->dynamic_info->fini_vaddr, DT_FINI);
	GetDynPtr(elf, &program->dynamic_info->init_array_vaddr, DT_INIT_ARRAY);
	GetDynPtr(elf, &program->dynamic_info->fini_array_vaddr, DT_FINI_ARRAY);
	GetDynPtr(elf, &program->dynamic_info->preinit_array_vaddr, DT_PREINIT_ARRAY);
	GetDynValue(elf, &program->dynamic_info->init_array_size, DT_INIT_ARRAYSZ);
	GetDynValue(elf, &program->dynamic_info->fini_array_size, DT_FINI_ARRAYSZ);
	GetDynValue(elf, &program->dynamic_info->preinit_array_size, DT_PREINIT_ARRAYSZ);

	Elf64_Sxword jmprel_type = 0;
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTREL) && elf->HasDynValue(DT_PLTREL));
	GetDynValue(elf, &jmprel_type, DT_OS_PLTREL);
	GetDynValue(elf, &jmprel_type, DT_PLTREL);

	EXIT_NOT_IMPLEMENTED(jmprel_type != DT_RELA);
	if (jmprel_type == DT_RELA) {
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_JMPREL) && elf->HasDynValue(DT_JMPREL));
		EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_PLTRELSZ) && elf->HasDynValue(DT_PLTRELSZ));
		GetDynDataOs(elf, &program->dynamic_info->jmprela_table, DT_OS_JMPREL);
		GetDynData(elf, program->base_vaddr, &program->dynamic_info->jmprela_table, DT_JMPREL);
		GetDynValue(elf, &program->dynamic_info->jmprela_table_size, DT_OS_PLTRELSZ);
		GetDynValue(elf, &program->dynamic_info->jmprela_table_size, DT_PLTRELSZ);
	}

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_RELA) && elf->HasDynValue(DT_RELA));
	GetDynDataOs(elf, &program->dynamic_info->rela_table, DT_OS_RELA);
	GetDynData(elf, program->base_vaddr, &program->dynamic_info->rela_table, DT_RELA);
	GetDynValue(elf, &program->dynamic_info->rela_table_total_size, DT_OS_RELASZ);
	GetDynValue(elf, &program->dynamic_info->rela_table_total_size, DT_RELASZ);
	GetDynValue(elf, &program->dynamic_info->rela_table_entry_size, DT_OS_RELAENT);
	GetDynValue(elf, &program->dynamic_info->rela_table_entry_size, DT_RELAENT);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table_total_size % sizeof(Elf64_Rela) != 0 ||
	                     program->dynamic_info->jmprela_table_size % sizeof(Elf64_Rela) != 0);
	const auto rela_count = program->dynamic_info->rela_table_total_size / sizeof(Elf64_Rela);
	const auto jmprela_count = program->dynamic_info->jmprela_table_size / sizeof(Elf64_Rela);
	program->rela_bits.resize((rela_count + jmprela_count + 7u) / 8u);

	GetDynValue(elf, &program->dynamic_info->relative_count, DT_RELACOUNT);

	GetDynValue(elf, &program->dynamic_info->debug, DT_DEBUG);
	GetDynValue(elf, &program->dynamic_info->flags, DT_FLAGS);
	GetDynValue(elf, &program->dynamic_info->textrel, DT_TEXTREL);

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->debug != 0);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->textrel != 0);

	std::vector<uint64_t> needed;
	GetDynValues(elf, &needed, DT_NEEDED);
	for (auto need: needed) {
		if (program->dynamic_info->str_table == nullptr || need >= program->dynamic_info->str_table_size ||
		    std::memchr(program->dynamic_info->str_table + need, '\0', program->dynamic_info->str_table_size - need) == nullptr) {
			program->dynamic_info->needed_valid = false;
			continue;
		}
		program->dynamic_info->needed.push_back(program->dynamic_info->str_table + need);
	}

	uint64_t so_name = 0;
	GetDynValue(elf, &so_name, DT_SONAME);
	program->dynamic_info->so_name = program->dynamic_info->str_table + so_name;

	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_NEEDED_MODULE) &&
	                     elf->HasDynValue(DT_OS_NEEDED_MODULE_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_MODULE_INFO) &&
	                     elf->HasDynValue(DT_OS_MODULE_INFO_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_IMPORT_LIB) &&
	                     elf->HasDynValue(DT_OS_IMPORT_LIB_1));
	EXIT_NOT_IMPLEMENTED(elf->HasDynValue(DT_OS_EXPORT_LIB) &&
	                     elf->HasDynValue(DT_OS_EXPORT_LIB_1));
	GetDynModules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table,
	              DT_OS_NEEDED_MODULE);
	GetDynModules(elf, &program->dynamic_info->import_modules, program->dynamic_info->str_table,
	              DT_OS_NEEDED_MODULE_1);
	GetDynModules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table,
	              DT_OS_MODULE_INFO);
	GetDynModules(elf, &program->dynamic_info->export_modules, program->dynamic_info->str_table,
	              DT_OS_MODULE_INFO_1);
	GetDynLibs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table,
	           DT_OS_IMPORT_LIB);
	GetDynLibs(elf, &program->dynamic_info->import_libs, program->dynamic_info->str_table,
	           DT_OS_IMPORT_LIB_1);
	GetDynLibs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table,
	           DT_OS_EXPORT_LIB);
	GetDynLibs(elf, &program->dynamic_info->export_libs, program->dynamic_info->str_table,
	           DT_OS_EXPORT_LIB_1);
}

void RuntimeLinker::Relocate(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);

	if (g_invalid_memory == 0) {
		g_invalid_memory = Libs::LibKernel::Memory::AllocateRuntimeMemory(
		    INVALID_MEMORY, 4096, Common::VirtualMemory::Mode::NoAccess, "invalid_memory", true);
		EXIT_NOT_IMPLEMENTED(g_invalid_memory == 0);
	}

	LOGF_COLOR(Log::Color::White, "--- Relocate program: %s ---\n",
	           Common::PathToString(program->file_name).c_str());

	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table_entry_size != sizeof(Elf64_Sym));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table_entry_size != sizeof(Elf64_Rela));
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->jmprela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->rela_table == nullptr);
	EXIT_NOT_IMPLEMENTED(program->dynamic_info->symbol_table == nullptr);

	const auto rela_count = static_cast<uint32_t>(
	    program->dynamic_info->rela_table_total_size / sizeof(Elf64_Rela));
	static const bool timing = [] {
		const char* value = std::getenv("KYTY_RELOCATION_TIMING");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	static uint64_t sequence = 0; // Relocation is serialized by m_mutex.
	const auto serial = timing ? ++sequence : 0;
	const auto started = timing ? std::chrono::steady_clock::now()
	                            : std::chrono::steady_clock::time_point {};
	uint32_t attempted = 0;
	uint32_t resolved = 0;
	if (timing) {
		std::printf("ps5-vsh: relocate-begin #%llu module=%s loaded=%zu\n",
		            static_cast<unsigned long long>(serial),
		            Common::PathToString(program->file_name.filename()).c_str(), program->rt->m_programs.size());
		std::fflush(stdout);
	}

	ForEachRelocation(program, [&](Elf64_Rela* r, uint32_t index, bool jmprela) {
		const auto bit_index = (jmprela ? rela_count : 0) + index;
		if ((program->rela_bits[bit_index >> 3u] & (1u << (bit_index & 7u))) == 0) {
			++attempted;
			if (RelocateRecord(index, r, program, jmprela)) {
				program->rela_bits[bit_index >> 3u] |= 1u << (bit_index & 7u);
				++resolved;
			}
		}
	});
	// Counters only inside the relocation loop; I/O stays at module boundaries.
	if (timing) {
		const auto elapsed = std::chrono::duration<double, std::milli>(
		    std::chrono::steady_clock::now() - started).count();
		std::printf("ps5-vsh: relocate-end #%llu module=%s attempted=%u resolved=%u pending=%u ms=%.3f\n",
		            static_cast<unsigned long long>(serial),
		            Common::PathToString(program->file_name.filename()).c_str(), attempted,
		            resolved, attempted - resolved, elapsed);
		std::fflush(stdout);
	}

	if (program->tls.image_vaddr != 0 && program->tls.init_size != 0 &&
	    program->tls.init_image.empty()) {
		const auto* src = reinterpret_cast<const uint8_t*>(program->tls.image_vaddr);
		program->tls.init_image.assign(src, src + program->tls.init_size);
	}
}

const ModuleId* RuntimeLinker::FindModule(const Program& program, const std::string& id) {
	const auto& import_modules = program.dynamic_info->import_modules;

	if (auto it = std::find_if(import_modules.begin(), import_modules.end(),
	                           [&id](const auto& module) { return module.id == id; });
	    it != import_modules.end()) {
		return &(*it);
	}

	const auto& export_modules = program.dynamic_info->export_modules;

	if (auto it = std::find_if(export_modules.begin(), export_modules.end(),
	                           [&id](const auto& module) { return module.id == id; });
	    it != export_modules.end()) {
		return &(*it);
	}

	return nullptr;
}

const LibraryId* RuntimeLinker::FindLibrary(const Program& program, const std::string& id) {
	const auto& import_libs = program.dynamic_info->import_libs;

	if (auto it = std::find_if(import_libs.begin(), import_libs.end(),
	                           [&id](const auto& lib) { return lib.id == id; });
	    it != import_libs.end()) {
		return &(*it);
	}

	const auto& export_libs = program.dynamic_info->export_libs;

	if (auto it = std::find_if(export_libs.begin(), export_libs.end(),
	                           [&id](const auto& lib) { return lib.id == id; });
	    it != export_libs.end()) {
		return &(*it);
	}

	return nullptr;
}

void RuntimeLinker::CreateSymbolDatabase(Program* program) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(program == nullptr);
	EXIT_IF(program->export_symbols != nullptr);
	EXIT_IF(program->import_symbols != nullptr);

	const char* np_ownership = std::getenv("KYTY_NATIVE_NP_OWNERSHIP");
	const char* np_workqueue = std::getenv("KYTY_NATIVE_NP_WORKQUEUE");
	const char* np_callout = std::getenv("KYTY_NATIVE_NP_CALLOUT");
	const char* np_http_pool = std::getenv("KYTY_NATIVE_NP_HTTP_POOL");
	const char* np_dispatcher = std::getenv("KYTY_NATIVE_NP_DISPATCHER");
	const char* np_event_queue = std::getenv("KYTY_NATIVE_NP_EVENT_QUEUE");
	auto policy = SymbolDatabase::ExportPolicy::All;
	if (program->rt->NativeSslSelected() &&
	    std::any_of(program->dynamic_info->export_libs.begin(), program->dynamic_info->export_libs.end(), [](const auto& lib) {
		    return Common::EqualNoCase(lib.name, "libSceSsl");
	    })) { policy = program->rt->m_ssl_production_enabled ? SymbolDatabase::ExportPolicy::NativeSsl : SymbolDatabase::ExportPolicy::PrivateSslSmoke; }
	if (Common::EqualNoCase(program->file_name.filename().string(), "libSceNpCommon.sprx")) {
		if (program->np_alternate54) {
			policy = SymbolDatabase::ExportPolicy::NpHttpPool;
			LOGF("NP alternate54: immutable reviewed provider selected (no credential grant)\n");
		} else if (np_event_queue != nullptr && std::string_view(np_event_queue) == "1") {
			policy = SymbolDatabase::ExportPolicy::NpEventQueue;
			LOGF("NP regular72: native EventQueue lifecycle selected (no service readiness or credential grant)\n");
		} else if (np_dispatcher != nullptr && std::string_view(np_dispatcher) == "1") {
			policy = SymbolDatabase::ExportPolicy::NpDispatcher;
		} else if (np_http_pool != nullptr && std::string_view(np_http_pool) == "1") {
			policy = SymbolDatabase::ExportPolicy::NpHttpPool;
		} else if (np_callout != nullptr && std::string_view(np_callout) == "1") {
			policy = SymbolDatabase::ExportPolicy::NpCallout;
		} else if (np_workqueue != nullptr && std::string_view(np_workqueue) == "1") {
			policy = SymbolDatabase::ExportPolicy::NpWorkQueue;
		} else if (np_ownership != nullptr && std::string_view(np_ownership) == "1") {
			policy = SymbolDatabase::ExportPolicy::NpOwnership;
		}
	}
	program->export_symbols = std::make_unique<SymbolDatabase>(policy);
	program->import_symbols = std::make_unique<SymbolDatabase>();

	auto syms = [](Program* program, SymbolDatabase* symbols, bool is_export) {
		if (program->dynamic_info->symbol_table == nullptr ||
		    program->dynamic_info->str_table == nullptr) {
			return;
		}

		for (auto* sym = program->dynamic_info->symbol_table;
		     reinterpret_cast<uint8_t*>(sym) <
		     reinterpret_cast<uint8_t*>(program->dynamic_info->symbol_table) +
		         program->dynamic_info->symbol_table_total_size;
		     sym++) {
			std::string id   = std::string(program->dynamic_info->str_table + sym->st_name);
			auto        bind = sym->GetBind();
			auto        type = sym->GetType();
			auto        ids  = Common::Split(id, '#');

			if (ids.size() == 3) {
				const auto* l = FindLibrary(*program, ids.at(1));
				const auto* m = FindModule(*program, ids.at(2));

				if (l != nullptr && m != nullptr && (bind == STB_GLOBAL || bind == STB_WEAK) &&
				    (type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
				    is_export == (sym->st_value != 0)) {
					SymbolResolve sr {};
					sr.name                 = ids.at(0);
					sr.library              = l->name;
					sr.library_version      = l->version;
					sr.module               = m->name;
					sr.module_version_major = m->version_major;
					sr.module_version_minor = m->version_minor;
					switch (type) {
						case STT_NOTYPE: sr.type = SymbolType::NoType; break;
						case STT_FUNC: sr.type = SymbolType::Func; break;
						case STT_OBJECT: sr.type = SymbolType::Object; break;
						default: sr.type = SymbolType::Unknown; break;
					}
					symbols->Add(sr, (is_export ? sym->st_value + program->base_vaddr : 0));
				}
			}
		}
	};

	syms(program, program->export_symbols.get(), true);
	syms(program, program->import_symbols.get(), false);
}

void RuntimeLinker::SetupTlsHandler(Program* program) {
	EXIT_IF(program == nullptr);
	EXIT_IF(program->elf == nullptr);
	EXIT_IF(program->elf->IsShared());
	EXIT_IF(program->tls.handler_vaddr == 0);

	// ps5-vsh: first main keeps the legacy singleton (fast path); extra
	// co-hosted mains register their stub range so TlsMainGetAddr can
	// route by caller. Planting is idempotent per program address.
	if (g_tls_main_program == nullptr) {
		g_tls_main_program = program;
	}
	{
		Common::HostLockCriticalGuard critical;
		std::lock_guard lock(g_tls_handler_ranges_mutex);
		g_tls_handler_ranges.emplace_back(program->tls.handler_vaddr, program);
	}

	auto* code = new (reinterpret_cast<void*>(program->tls.handler_vaddr)) Jit::SafeCall;

	code->SetFunc(TlsMainGetAddr);

	for (uint8_t reg = 1; reg < 8; reg++) {
		if (reg == 4) {
			continue;
		}

		auto* stub = new (reinterpret_cast<void*>(program->tls.handler_vaddr +
		                                          Jit::TlsRegStub::GetOffset(reg))) Jit::TlsRegStub;
		stub->SetFunc(program->tls.handler_vaddr);
		stub->SetOutputReg(reg);
	}

	EXIT_IF(!Libs::LibKernel::Memory::ProtectGuestMemory(program->tls.handler_vaddr,
	                                                     Jit::SafeCall::GetSize(),
	                                                     Common::VirtualMemory::Mode::Execute));
	Common::VirtualMemory::FlushInstructionCache(program->tls.handler_vaddr,
	                                             Jit::SafeCall::GetSize());
}

void RuntimeLinker::DeleteTlss(int thread_id, bool allow_guest_callbacks) {
	Common::LockGuard lock(m_mutex);

	for (auto* p: m_programs) {
		DeleteTls(p, thread_id, allow_guest_callbacks);
	}
}

namespace {
// Origin/ownership is checked separately. A host-readable address alone is
// never sufficient guest heap ABI admission. All arithmetic is half-open.
bool HeapMappedSpan(uint64_t address, uint64_t size, bool writable) {
	if (!address || !size || size > UINT64_MAX - address ||
	    !Libs::Graphics::HostMemoryRangeIsReadable(address, size)) { return false; }
	const auto limit = address + size;
	for (auto cursor = address; cursor < limit;) {
		void* begin = nullptr;
		void* end = nullptr;
		int prot = 0;
		if (Libs::LibKernel::Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(cursor), &begin, &end, &prot) != 0 ||
		    reinterpret_cast<uint64_t>(begin) > cursor || reinterpret_cast<uint64_t>(end) <= cursor ||
		    !(prot & 1) || (prot & 4) || (writable && !(prot & 2))) { return false; }
		cursor = std::min(limit, reinterpret_cast<uint64_t>(end));
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Also reject host-only protection changes (including guard/noaccess pages)
	// that have not altered the tracked guest range. No protection is changed.
	if (writable) {
		for (auto cursor = address; cursor < limit;) {
			MEMORY_BASIC_INFORMATION region {};
			if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) || region.State != MEM_COMMIT ||
			    (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return false; }
			const auto protection = region.Protect & 0xff;
			if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
			    protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) { return false; }
			const auto base = reinterpret_cast<uint64_t>(region.BaseAddress);
			if (base > cursor || region.RegionSize > UINT64_MAX - base || base + region.RegionSize <= cursor) { return false; }
			cursor = std::min(limit, base + region.RegionSize);
		}
	}
#endif
	return true;
}
}

bool RuntimeLinker::IsApplicationHeapSpan(const Program* owner, uint64_t address, uint64_t size, bool writable) {
	// Caller holds m_mutex; mappings stay pinned for the owner's lifetime. Admit
	// only one complete private ELF data segment or the ACTUAL current owned
	// pthread's guest stack, never arbitrary RW/JIT/session/foreign memory.
	if (!owner || owner->application_owner != owner || !address || !size || size > UINT64_MAX - address) { return false; }
	bool owned = false;
	for (const auto* program: m_programs) {
		if (program->application_owner != owner || !program->base_vaddr || !program->elf || !program->elf->IsValid() ||
		    IsNativeKernelProvider(program) || address < program->base_vaddr) { continue; }
		const auto offset = address - program->base_vaddr;
		if (offset >= program->base_size || size > program->base_size - offset) { continue; }
		const auto* segments = program->elf->GetPhdr();
		for (unsigned i = 0; i != program->elf->GetEhdr()->e_phnum; ++i) {
			const auto& segment = segments[i];
			if ((segment.p_type != PT_LOAD && segment.p_type != PT_OS_RELRO) || !(segment.p_flags & PF_R) ||
			    (segment.p_flags & PF_X) || (writable && !(segment.p_flags & PF_W)) ||
			    segment.p_vaddr > program->base_size || segment.p_memsz > program->base_size - segment.p_vaddr ||
			    offset < segment.p_vaddr || offset - segment.p_vaddr >= segment.p_memsz ||
			    size > segment.p_memsz - (offset - segment.p_vaddr)) { continue; }
			owned = true;
			break;
		}
		if (owned) { break; }
	}
	if (!owned) {
		const auto execution = GetApplicationExecution(owner);
		const auto self = Libs::LibKernel::PthreadSelfOrNull();
		uint64_t stack = 0, stack_size = 0;
		if (execution && execution->HasHostCallForCurrentBoundary() && self &&
		    GetApplicationExecution(Libs::LibKernel::PthreadGetOwnerProgram(self)) == execution &&
		    Libs::LibKernel::PthreadGetGuestStack(self, &stack, &stack_size) &&
		    address >= stack && address - stack < stack_size && size <= stack_size - (address - stack)) { owned = true; }
	}
	return owned && HeapMappedSpan(address, size, writable);
}

bool RuntimeLinker::SetApplicationHeapApiFromGuest(void* const api[10]) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	auto* importer = GetCurrentProgram();
	auto* owner = importer ? importer->application_owner : nullptr;
	std::array<void*, 10> snapshot {};
	if (!owner) {
		// Session compatibility is explicit; never reached by a shared caller
		// missing its control. The trusted helper sees only this local copy.
		if (api) {
			if (!Libs::Graphics::HostMemoryRangeIsReadable(reinterpret_cast<uint64_t>(api), sizeof(snapshot))) { return false; }
			std::memcpy(snapshot.data(), api, sizeof(snapshot));
		}
		SetApplicationHeapApi(api ? snapshot.data() : nullptr);
		return true;
	}
	const auto execution = GetApplicationExecution(importer);
	if (!execution) { return false; }
	auto lease = execution->TryAcquire(GuestWorkKind::Callback);
	if (!lease || execution->PendingThreadExit()) { return false; }
	const auto reject = [&](uint64_t detail) {
		execution->Fail(GuestFailureReason::InvalidTransition, detail);
		return false;
	};
	if (importer->rt != this || owner->rt != this || execution->Owner().identity != owner ||
	    std::find(m_programs.begin(), m_programs.end(), importer) == m_programs.end() ||
	    std::find(m_programs.begin(), m_programs.end(), owner) == m_programs.end()) { return reject(0); }
	if (api) {
		if (!IsApplicationHeapSpan(owner, reinterpret_cast<uint64_t>(api), sizeof(snapshot), false)) {
			return reject(reinterpret_cast<uint64_t>(api));
		}
		std::memcpy(snapshot.data(), api, sizeof(snapshot));
		if (!snapshot[0] || !snapshot[1]) { return reject(reinterpret_cast<uint64_t>(api)); }
		for (const auto target: snapshot) {
			if (!target) { continue; }
			void* begin = nullptr;
			void* end = nullptr;
			int prot = 0;
			if (!IsApplicationExecutableAddress(owner, reinterpret_cast<uint64_t>(target)) ||
			    Libs::LibKernel::Memory::KernelQueryMemoryProtection(target, &begin, &end, &prot) != 0 || !(prot & 4) ||
			    reinterpret_cast<uint64_t>(begin) > reinterpret_cast<uint64_t>(target) ||
			    reinterpret_cast<uint64_t>(end) <= reinterpret_cast<uint64_t>(target)) { return reject(reinterpret_cast<uint64_t>(target)); }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			MEMORY_BASIC_INFORMATION region {};
			if (!VirtualQuery(target, &region, sizeof(region)) || region.State != MEM_COMMIT ||
			    (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return reject(reinterpret_cast<uint64_t>(target)); }
			const auto protection = region.Protect & 0xff;
			if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
			    protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
				return reject(reinterpret_cast<uint64_t>(target));
			}
#endif
		}
	}
	// No guest code, allocation or borrowed-vector reads inside publication.
	return lease.Publish([&] {
		owner->application_heap_malloc = reinterpret_cast<application_heap_malloc_func_t>(snapshot[0]);
		owner->application_heap_free = reinterpret_cast<application_heap_free_func_t>(snapshot[1]);
		owner->application_heap_posix_memalign = reinterpret_cast<application_heap_posix_memalign_func_t>(snapshot[6]);
	});
}

bool RuntimeLinker::WriteApplicationHeapTraceInfo(ApplicationHeapTraceInfo* output) {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(m_mutex);
	auto* importer = GetCurrentProgram();
	auto* owner = importer ? importer->application_owner : nullptr;
	const auto execution = GetApplicationExecution(importer);
	if (!owner || !execution) { return false; }
	auto lease = execution->TryAcquire(GuestWorkKind::HostCall);
	if (!lease || execution->PendingThreadExit()) { return false; }
	const auto reject = [&] {
		execution->Fail(GuestFailureReason::InvalidTransition, reinterpret_cast<uint64_t>(output));
		return false;
	};
	if (importer->rt != this || owner->rt != this || execution->Owner().identity != owner ||
	    std::find(m_programs.begin(), m_programs.end(), importer) == m_programs.end() ||
	    std::find(m_programs.begin(), m_programs.end(), owner) == m_programs.end() ||
	    !IsApplicationHeapSpan(owner, reinterpret_cast<uint64_t>(output), sizeof(*output), true)) { return reject(); }
	uint64_t size = 0;
	std::memcpy(&size, output, sizeof(size));
	if (size != sizeof(*output)) { return reject(); } // Void ABI: no partial/fake-success output.
	std::unique_ptr<ApplicationImportData> prepared;
	if (!owner->application_import_data) {
		prepared = ApplicationImportData::Create(owner, execution->Owner());
		if (!prepared) {
			if (!execution->StopRequested()) { return reject(); }
			return false;
		}
	}
	const auto* data = prepared ? prepared.get() : owner->application_import_data.get();
	ApplicationHeapTraceInfo result {};
	if (!data->HeapTraceInfo(execution->Owner(), &result)) { return reject(); }
	return lease.Publish([&] {
		if (prepared) { owner->application_import_data = std::move(prepared); }
		std::memcpy(output, &result, sizeof(result));
	});
}

void RuntimeLinker::SetApplicationHeapApi(void* const api[10]) {
	Common::LockGuard lock(m_mutex);
	const auto* current = GetCurrentProgram();
	auto* owner = current != nullptr ? current->application_owner : nullptr;
	const auto execution = GetApplicationExecution(current);
	auto lease = execution ? execution->TryAcquire(GuestWorkKind::Callback) : RuntimeGuestExecution::Lease {};
	if (owner && !lease) { return; }
	auto& malloc_func = owner != nullptr ? owner->application_heap_malloc : m_application_heap_malloc;
	auto& free_func = owner != nullptr ? owner->application_heap_free : m_application_heap_free;
	auto& memalign_func = owner != nullptr ? owner->application_heap_posix_memalign : m_application_heap_posix_memalign;

	const auto publish = [&] {
		const bool valid = api && api[0] && api[1];
		malloc_func = valid ? reinterpret_cast<application_heap_malloc_func_t>(api[0]) : nullptr;
		free_func = valid ? reinterpret_cast<application_heap_free_func_t>(api[1]) : nullptr;
		memalign_func = valid ? reinterpret_cast<application_heap_posix_memalign_func_t>(api[6]) : nullptr;
	};
	if (execution) { lease.Publish(publish); }
	else { publish(); }
}

namespace {
struct HeapCallbackArguments {
	uint64_t target;
	uint64_t first;
	uint64_t second;
	uint64_t third;
};
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
static_assert(offsetof(HeapCallbackArguments, third) == 24);
// Typed allocator signatures: pointer(uint64_t) or int(void**,uint64_t,uint64_t).
// Only this trivial assembly transition is beneath the guest boundary.
__attribute__((naked)) uint64_t KYTY_GUEST_CALL_BOUNDARY_SYSV HeapCallbackTransition(void*) {
	__asm__ volatile("movq %rdi, %r10\n\t"
	                 "movq 0(%r10), %rax\n\t"
	                 "movq 8(%r10), %rdi\n\t"
	                 "movq 16(%r10), %rsi\n\t"
	                 "movq 24(%r10), %rdx\n\t"
	                 "jmp *%rax\n\t");
}
#endif
RuntimeGuestResult InvokeHeapCallback(RuntimeGuestExecution& execution, HeapCallbackArguments& arguments) {
	auto lease = execution.TryAcquire(GuestWorkKind::Callback);
	if (!lease) { return {{GuestCallBoundaryStatus::Ok, GuestCallOutcomeTag::NotInvoked, 0}, execution.Failure(), true}; }
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
	return execution.Invoke(HeapCallbackTransition, &arguments);
#else
	return {{GuestCallBoundaryStatus::Unsupported, GuestCallOutcomeTag::NotInvoked, 0},
	        execution.Fail(GuestFailureReason::BoundaryUnavailable), execution.StopRequested()};
#endif
}
}

void* RuntimeLinker::ApplicationHeapMemalign(uint64_t alignment, uint64_t size) {
	application_heap_posix_memalign_func_t memalign_func;
	std::shared_ptr<RuntimeGuestExecution> execution;
	{
		Common::LockGuard lock(m_mutex);
		const auto* current = GetCurrentProgram();
		const auto* owner = current != nullptr ? current->application_owner : nullptr;
		execution = GetApplicationExecution(current);
		memalign_func = owner != nullptr ? owner->application_heap_posix_memalign : m_application_heap_posix_memalign;
	}

	if (memalign_func != nullptr) {
		void* ptr = nullptr;
		if (execution) {
			HeapCallbackArguments arguments {reinterpret_cast<uint64_t>(memalign_func),
			    reinterpret_cast<uint64_t>(&ptr), alignment, size};
			const auto result = InvokeHeapCallback(*execution, arguments);
			return result.Returned() && static_cast<int32_t>(result.call.value) == 0 ? ptr : nullptr;
		}
		return memalign_func(&ptr, alignment, size) == 0 ? ptr : nullptr;
	}

	return nullptr;
}

void* RuntimeLinker::ApplicationHeapMalloc(uint64_t size) {
	application_heap_malloc_func_t malloc_func;
	std::shared_ptr<RuntimeGuestExecution> execution;
	{
		Common::LockGuard lock(m_mutex);
		const auto* current = GetCurrentProgram();
		const auto* owner = current != nullptr ? current->application_owner : nullptr;
		execution = GetApplicationExecution(current);
		malloc_func = owner != nullptr ? owner->application_heap_malloc : m_application_heap_malloc;
	}

	if (!malloc_func) { return nullptr; }
	if (execution) {
		HeapCallbackArguments arguments {reinterpret_cast<uint64_t>(malloc_func), size, 0, 0};
		const auto result = InvokeHeapCallback(*execution, arguments);
		return result.Returned() ? reinterpret_cast<void*>(result.call.value) : nullptr;
	}
	return malloc_func(size);
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#include "../../tests/RuntimeApplicationScopeTests.inc"
#include "../../tests/RuntimeProgramFileReuseTests.inc"
#include "../../tests/RuntimeAppLocalProgramsTests.inc"
#include "../../tests/KernelTextLocationTests.inc"
#include "../../tests/RuntimeGuestExecutionTests.inc"
#include "../../tests/ApplicationImportBindingsTests.inc"
#include "../../tests/RuntimeApplicationImportDataTests.inc"
#include "../../tests/RuntimeSanitizerOwnerTests.inc"
#include "../../tests/RuntimeAllocatorBootstrapTests.inc"
#include "../../tests/RuntimeCrtSyncTests.inc"
#endif

} // namespace Loader
