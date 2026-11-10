// SPDX-License-Identifier: GPL-2.0-or-later
#include "loader/applicationImportData.h"

#include "common/stringUtils.h"
#include "kernel/memory.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <new>

namespace Loader {
namespace {
struct Storage {
	// Legacy emulator policy, NOT a claim of a randomized firmware canary.
	// libKernel.cpp: uint64_t g_stack_chk_guard = 0xDeadBeef00000007.
	uint64_t stack_guard = 0xDeadBeef00000007ULL;
	uint64_t program_name = 0; // guest pointer cell, not the host g_progname/buffer
	// libC.cpp has distinct uint32_t = 1 objects in these two namespaces.
	uint32_t libc_need = 1;
	uint32_t internal_need = 1;
	char name[ApplicationImportData::ProgramNameCapacity] {};
	ApplicationSanitizerMallocTable sanitizer_malloc;
	ApplicationSanitizerNewTable sanitizer_new;
	uint64_t heap_trace_mask = 0;
	uint64_t heap_trace_table[ApplicationImportData::HeapTraceTableCount] {};
};
static_assert(sizeof(Storage) <= ApplicationImportData::MappingSize);
static_assert(sizeof(uint64_t) == 8 && sizeof(uint32_t) == 4);
struct Object {
	const char* nid;
	const char* library;
	const char* expression;
	uint64_t offset;
	uint64_t size;
};
constexpr std::array Objects {
	Object {"f7uOxY9mM1U", "libkernel", "&LibKernel::g_stack_chk_guard", offsetof(Storage, stack_guard), 8},
	Object {"djxxOmW6-aw", "libkernel", "&LibKernel::g_progname", offsetof(Storage, program_name), 8},
	Object {"P330P3dFF68", "libc", "&LibC::g_need_flag", offsetof(Storage, libc_need), 4},
	Object {"ZT4ODD2Ts9o", "LibcInternal", "&LibcInternal::g_need_flag", offsetof(Storage, internal_need), 4},
};
const Object* Select(const SymbolDatabase& registrations, const std::string& raw_name,
                     const SymbolRecord& resolved, int64_t addend, uint64_t declared_size) {
	const auto nid = raw_name.substr(0, raw_name.find('#'));
	for (const auto& object: Objects) {
		if (nid != object.nid || addend < 0 || static_cast<uint64_t>(addend) >= object.size ||
		    (declared_size != 0 && declared_size != object.size)) { continue; }
		const SymbolResolve identity {object.nid, object.library, 1, object.library, 1, 1, SymbolType::Object};
		// FindRegistration rejects overwritten/conflicting exact identities. NID
		// or an address match alone is insufficient, as is a Func/NoType record.
		const auto* registered = registrations.FindRegistration(identity, object.expression);
		if (registered && registered->vaddr == resolved.vaddr && registered->name == resolved.name &&
		    registered->dbg_name == resolved.dbg_name) { return &object; }
	}
	return nullptr;
}
bool SameLifetime(GuestCallOwner first, GuestCallOwner second) noexcept {
	return first.identity && first.token && first.identity == second.identity && first.token == second.token;
}
}

std::unique_ptr<ApplicationImportData> ApplicationImportData::Create(const Program* owner, GuestCallOwner lifetime) noexcept {
	if (!owner || owner->application_owner != owner || lifetime.identity != owner || !lifetime.token) { return {}; }
	const auto execution = GetApplicationExecution(owner);
	if (!execution || !SameLifetime(execution->Owner(), lifetime) || execution->StopRequested()) { return {}; }
	try {
		auto result = std::unique_ptr<ApplicationImportData>(new ApplicationImportData(lifetime));
		const auto name = Common::PathToString(owner->file_name.filename());
		result->mapping_ = Libs::LibKernel::Memory::AllocateRuntimeMemory(0, MappingSize,
		    Common::VirtualMemory::Mode::ReadWrite, "application_import_data");
		if (!result->mapping_) { return {}; }
		auto* storage = new (reinterpret_cast<void*>(result->mapping_)) Storage {};
		storage->program_name = result->mapping_ + offsetof(Storage, name);
		std::memcpy(storage->name, name.data(), std::min(name.size(), ProgramNameCapacity - 1));
		return result;
	} catch (...) { return {}; }
}

ApplicationImportData::~ApplicationImportData() {
	// Same final quiescence requirement as Program/private-module destruction.
	// Stop alone never releases/reuses these copied guest data pointers.
	if (mapping_ && !Libs::LibKernel::Memory::FreeGuestMemory(mapping_, MappingSize)) { std::terminate(); }
}

bool ApplicationImportData::Supports(const SymbolDatabase& registrations, const std::string& raw_name,
    const SymbolRecord& resolved, int64_t addend, uint64_t declared_size) {
	return Select(registrations, raw_name, resolved, addend, declared_size) != nullptr;
}

uint64_t ApplicationImportData::Address(GuestCallOwner lifetime, const SymbolDatabase& registrations,
    const std::string& raw_name, const SymbolRecord& resolved, int64_t addend, uint64_t declared_size) const {
	if (!SameLifetime(lifetime_, lifetime) || !mapping_) { return 0; }
	const auto* object = Select(registrations, raw_name, resolved, addend, declared_size);
	return object ? mapping_ + object->offset + static_cast<uint64_t>(addend) : 0;
}

bool ApplicationImportData::ContainsObjectAddress(GuestCallOwner lifetime, uint64_t address) const noexcept {
	if (!SameLifetime(lifetime_, lifetime) || !mapping_ || address < mapping_) { return false; }
	const auto offset = address - mapping_;
	for (const auto& object: Objects) {
		if (offset >= object.offset && offset - object.offset < object.size) { return true; }
	}
	return false;
}

uint64_t ApplicationImportData::SanitizerTable(GuestCallOwner lifetime, ApplicationSanitizerReplacement kind) const noexcept {
	if (!SameLifetime(lifetime_, lifetime) || !mapping_) { return 0; }
	switch (kind) {
		case ApplicationSanitizerReplacement::Malloc: return mapping_ + offsetof(Storage, sanitizer_malloc);
		case ApplicationSanitizerReplacement::New: return mapping_ + offsetof(Storage, sanitizer_new);
	}
	return 0;
}

bool ApplicationImportData::HeapTraceInfo(GuestCallOwner lifetime, ApplicationHeapTraceInfo* output) const noexcept {
	if (!output || !SameLifetime(lifetime_, lifetime) || !mapping_) { return false; }
	*output = ApplicationHeapTraceInfo {};
	output->mask = mapping_ + offsetof(Storage, heap_trace_mask);
	output->table = mapping_ + offsetof(Storage, heap_trace_table);
	return true;
}
} // namespace Loader
