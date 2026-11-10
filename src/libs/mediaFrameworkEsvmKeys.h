#pragma once

#include "common/abi.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Loader { class SymbolDatabase; }

namespace Libs::MediaFramework {
// Observed interface ABI, NOT EsvmEngine's private C++ object ABI.
using EsvmObjectCompletion = KYTY_SYSV_ABI void (*)(int32_t handle, int32_t result, void* user);
using EsvmKeysCompletion = KYTY_SYSV_ABI void (*)(int32_t result, void* user);
using EsvmProvisioningCompletion = KYTY_SYSV_ABI void (*)(bool provisioned, int32_t result, void* user);
using EsvmKeysEvent = KYTY_SYSV_ABI void (*)(int32_t keys, int32_t event, const void* data, uint64_t bytes);
inline constexpr size_t EsvmAccessConfigurationBytes = 0x401b8;

// No genuine CDM/session/decrypt provider is currently available. Valid bounded
// requests complete once with (0, 0x8179000a, user), without admitting an access.
int KYTY_SYSV_ABI EsvmRequestKeySystemAccess(const char* name, const void* configurations,
                                           uint32_t count, EsvmObjectCompletion complete, void* user);
int KYTY_SYSV_ABI EsvmDestroyKeySystemAccess(int access);
int KYTY_SYSV_ABI EsvmCreateMediaKeys2(int access, bool option, EsvmKeysEvent event,
                                     EsvmObjectCompletion complete, void* user);
int KYTY_SYSV_ABI EsvmDestroyMediaKeys(int keys);
int KYTY_SYSV_ABI EsvmGetServiceCertificateRequest(int keys, EsvmKeysCompletion complete, void* user);
// Set/Parse authenticate the public service certificate using the fixed firmware
// pin and own role-scoped signed bytes. This is NOT device provisioning/licensing.
int KYTY_SYSV_ABI EsvmSetServiceCertificate(int keys, int role, const void* data, uint64_t bytes,
                                          EsvmKeysCompletion complete, void* user);
int KYTY_SYSV_ABI EsvmParseServiceCertificateResponse(int keys, int role, const void* data, uint64_t bytes,
                                                    EsvmKeysCompletion complete, void* user);
// Query reports known absent device credentials as (false, 0, user), with no
// request/event. Service certificates do not change this device-credential fact.
int KYTY_SYSV_ABI EsvmQueryProvisioning(int keys, EsvmProvisioningCompletion complete, void* user);
int KYTY_SYSV_ABI EsvmGetProvisioningRequest(int keys, EsvmKeysCompletion complete, void* user);
int KYTY_SYSV_ABI EsvmHandleProvisioningResponse(int keys, const void* data, uint64_t bytes,
                                               EsvmKeysCompletion complete, void* user);
// No license sessions are admitted by this protocol-only provider. Rejection
// is synchronous (nonzero transport result); output/callback storage untouched.
int KYTY_SYSV_ABI EsvmRejectMediaKeySession(int keys, int type, const void* callbacks, int* session);
int KYTY_SYSV_ABI EsvmRejectKeysDecrypt(int keys, const void* parameters, void* output, uint64_t* output_bytes);

void InitMediaFrameworkKeys_1(Loader::SymbolDatabase* symbols);

// Clear calls invalidation while holding the framework lifecycle lock. It never
// waits/calls guest code. Drain MUST run after those locks have been released.
// Routine/reentrant drains try/defer and retain discoverable entries. Explicit
// external retirement waits only when no keys/source dispatch gate is held.
void InvalidateEsvmKeysOwner(int pid);
void DrainRetiredEsvmKeys(bool wait = false); // Blocking only outside dispatch scopes.

#if defined(KYTY_ESVM_KEYS_TESTS)
// Host fixture isolation only: absent from production and never a guest export.
// Enables the exact native-default public-message fixture on this thread for
// this scope; nested scopes restore their prior state. Default is OFF. This
// does not provide or assert usable DRM, credentials, licensing or decryption.
class EsvmKeysPublicMessageFixtureScope final {
public:
 EsvmKeysPublicMessageFixtureScope() noexcept;
 ~EsvmKeysPublicMessageFixtureScope();
 EsvmKeysPublicMessageFixtureScope(const EsvmKeysPublicMessageFixtureScope&) = delete;
 EsvmKeysPublicMessageFixtureScope& operator=(const EsvmKeysPublicMessageFixtureScope&) = delete;
private:
 bool previous;
};
#endif

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
struct EsvmKeysResources { size_t owners, accesses, keys, configuration_bytes, pending, certificate_slots, certificate_bytes, retired_owners; };
EsvmKeysResources EsvmKeysResourcesForTest();
void EsvmKeysFailNextAllocationForTest(size_t successful_allocations = 0);
std::vector<uint8_t> EsvmKeysCertificateForTest(int keys, size_t slot);
#endif
} // namespace Libs::MediaFramework
