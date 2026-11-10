#pragma once

#include "kernel/eventFlag.h"
#include "kernel/semaphore.h"
#include <cstdint>
#include <filesystem>

namespace Loader { class Elf64; }
namespace Libs::LibKernel::SemaphoreProvenance {
// Observation only. Initialization/placement are host-loader operations, never
// called from a guest Create/Poll wrapper. No dependency on Memory initialization.
void Initialize() noexcept;
bool Enabled() noexcept;
void Placement(Loader::Elf64* elf, const std::filesystem::path& filename,
               uint64_t base, uint64_t size, int32_t program_id) noexcept;
void Unloading(int32_t program_id) noexcept;

int KYTY_SYSV_ABI CreateEventObserved(EventFlag::KernelEventFlag* out, const char* name,
                            uint32_t attr, uint64_t pattern, const void* opt);
int KYTY_SYSV_ABI CreateSema(Semaphore::KernelSema* out, const char* name,
                           uint32_t attr, int initial, int maximum, void* opt);
int KYTY_SYSV_ABI PollSema(Semaphore::KernelSema handle, int need);
} // namespace Libs::LibKernel::SemaphoreProvenance
