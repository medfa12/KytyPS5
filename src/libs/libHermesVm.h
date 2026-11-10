#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBHERMESVM_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBHERMESVM_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Libs::HermesVm::HermesVmHost {

// Host-side one-shot: create a VM context over host-owned HBC bytes,
// execute the entry bundle, destroy. Returns 0 on clean run, or an
// HERMES_VM_ERROR_* code (see libHermesVm.cpp). Used by the RNPS --game
// path for decrypted bundles; never touches guest memory.
int RunHbcBuffer(const uint8_t* bytecode, uint64_t size);

// RNPS startup params (--rnps-param k=v): surfaced to bundles via
// CSI.params.appconfig + custom keys. Empty = legacy proxy behavior.
void SetRnpsLaunchParams(const std::vector<std::pair<std::string, std::string>>& params);

// RNPS app identity for the TTPool mount path (titleId + manifest fields).
void SetRnpsAppInfo(const std::string& titleId, const std::string& appName,
                    const std::string& appVersion);

// Host-side resident service: same evaluation as RunHbcBuffer, then the
// runtime stays alive pumping live ticks for `seconds` (event listeners
// re-fire each tick) instead of returning immediately.
int RunHbcBufferLive(const uint8_t* bytecode, uint64_t size, uint64_t seconds);

} // namespace Libs::HermesVm::HermesVmHost

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBHERMESVM_H_ */
