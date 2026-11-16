#include "libs/mediaFrameworkEsvmKeys.h"

#include "common/threads.h"
#include "libs/libs.h"
#include "libs/mediaFrameworkEsvm.h"
#include "libs/mediaFrameworkEsvmCertificate.h"
#include "loader/runtimeProcessIdentity.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#if defined(KYTY_ESVM_KEYS_TESTS)
#include <string_view>
#endif
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Libs::MediaFramework {
LIB_VERSION("MediaFrameworkInterface", 1, "MediaFrameworkInterface", 1, 1);
namespace {
constexpr int Invalid = static_cast<int>(0x81790001u);
constexpr int Full = static_cast<int>(0x81790007u);
constexpr int NotSupported = static_cast<int>(0x8179000au);
constexpr int Failed = static_cast<int>(0x81790fffu);
constexpr size_t ObjectLimit = 8;
constexpr size_t PendingLimit = 8;

void Trace(const char* operation, int handle, int result) {
 static const bool enabled = [] {
  const auto* value = std::getenv("KYTY_MEDIA_TRACE");
  return value && std::strcmp(value, "1") == 0;
 }();
 if (!enabled) { return; }
 // Each distinct API keeps its own first-entry evidence. Query polling must
 // not hide a later provisioning request, license session or direct decrypt.
 enum Group : size_t { General, Query, Request, Response, License, Decrypt, Count };
 const auto group = std::strncmp(operation, "query-provisioning-", 19) == 0 ? Query :
                    std::strncmp(operation, "get-provisioning-request-", 25) == 0 ? Request :
                    std::strncmp(operation, "provisioning-response-", 22) == 0 ? Response :
                    std::strncmp(operation, "license-session-", 16) == 0 ? License :
                    std::strncmp(operation, "keys-decrypt-", 13) == 0 ? Decrypt : General;
 static std::array<std::atomic_uint, Count> counts {};
 constexpr std::array<unsigned, Count> limits {128, 16, 16, 16, 16, 16};
 auto& counter = counts[group];
 auto count = counter.load(std::memory_order_relaxed);
 while (count < limits[group]) {
  if (!counter.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) { continue; }
  LOGF("MediaFramework keys: operation=%s pid=%d handle=%d result=%08x\n",
       operation, Loader::GetCurrentGuestProcessId(), handle, static_cast<unsigned>(result));
  break;
 }
}

bool Read(void* out, const void* in, size_t size) {
 if (out == nullptr || in == nullptr) { return false; }
#if defined(_WIN32)
 SIZE_T copied = 0;
 return ReadProcessMemory(GetCurrentProcess(), in, out, size, &copied) && copied == size;
#elif defined(__linux__)
 iovec local {out, size}, remote {const_cast<void*>(in), size};
 return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#else
 return false;
#endif
}
bool ReadName(const char* in, std::array<char, 257>& out) {
 for (size_t index = 0; index < out.size(); ++index) {
  const auto address = reinterpret_cast<uintptr_t>(in);
  if (address == 0 || index > UINTPTR_MAX - address ||
      !Read(&out[index], reinterpret_cast<const void*>(address + index), 1)) { return false; }
  if (out[index] == 0) { return true; }
 }
 return false;
}

// Public-message state for a future genuine provider and isolated host fixtures,
// NOT a usable CDM. Production access admission is unsupported; only the
// compile-time test seam below can currently create an Access. Certificates
// never supply device credentials, license sessions or decryption keys.
struct Access {
 int handle = 0;
 bool live = true;
 std::vector<uint8_t> configuration;
};
struct Keys {
 int handle = 0;
 bool live = true;
 bool option = false; // Observed boolean retained; no unverified CDM semantics.
 EsvmKeysEvent event = nullptr;
 std::shared_ptr<Access> access;
 std::array<uint8_t, 2> request {};
 size_t pending = 0;
 uint64_t requests = 0;
 // License-service and provisioning-service public certificates, not device
 // credentials or provisioned flags. Role 2 atomically assigns both slots.
 std::array<std::shared_ptr<const EsvmVerifiedServiceCertificate>, 2> certificates {};
};
struct Owner {
 int pid;
 uint64_t epoch;
 std::atomic_bool live {true};
 // Serializes callback admission against drain. NOT a registry/lifecycle lock.
 // Recursive so callbacks can destroy/query their own protocol objects. Other
 // threads cannot return from retirement while a callback is still executing.
 std::shared_ptr<std::recursive_mutex> dispatch;
 size_t callbacks = 0; // Protected by dispatch; reentrant drain must defer unlink.
 std::array<std::shared_ptr<Access>, ObjectLimit> accesses {};
 std::array<std::shared_ptr<Keys>, ObjectLimit> keys {};
 std::shared_ptr<Owner> retired_next;
 Owner(int p, uint64_t e, std::shared_ptr<std::recursive_mutex> gate): pid(p), epoch(e), dispatch(std::move(gate)) {}
};
struct Registry {
 std::mutex mutex;
 std::map<int, std::shared_ptr<Owner>> owners;
 std::shared_ptr<Owner> retired;
};
Registry& Service() { static auto* state = new Registry; return *state; }
thread_local size_t fail_allocation_after = SIZE_MAX;
void AllocationPoint() {
 if (fail_allocation_after == SIZE_MAX) { return; }
 if (fail_allocation_after == 0) { fail_allocation_after = SIZE_MAX; throw std::bad_alloc(); }
 --fail_allocation_after;
}
void Retire(Registry& registry, const std::shared_ptr<Owner>& owner) {
 owner->live.store(false);
 owner->retired_next = std::move(registry.retired);
 registry.retired = owner;
}
std::shared_ptr<Owner> GetOwner(int& result) {
 const int pid = Loader::GetCurrentGuestProcessId();
 uint64_t epoch = 0;
 std::shared_ptr<std::recursive_mutex> gate;
 result = GetEsvmClientLease(pid, &epoch, &gate);
 if (result != 0) { return nullptr; }
 // Never acquire framework/identity locks while holding this registry lock:
 // Clear invalidates in framework -> keys-registry order.
 Common::HostLockCriticalGuard critical;
 auto& registry = Service();
 std::lock_guard lock(registry.mutex);
 auto it = registry.owners.find(pid);
 if (it != registry.owners.end() && it->second->epoch != epoch) {
  Retire(registry, it->second);
  registry.owners.erase(it);
  it = registry.owners.end();
 }
 if (it != registry.owners.end()) { return it->second; }
 if (registry.owners.size() >= 64) { result = Full; return nullptr; }
 AllocationPoint();
 auto owner = std::make_shared<Owner>(pid, epoch, std::move(gate));
 registry.owners.emplace(pid, owner);
 return owner;
}
bool Valid(const Owner& owner) {
 uint64_t epoch = 0;
 return owner.live.load() && GetEsvmClientEpoch(owner.pid, &epoch) == 0 && epoch == owner.epoch;
}
int NextHandle() {
 return AllocateEsvmServiceHandle();
}
template<class T> std::shared_ptr<T> Find(const std::array<std::shared_ptr<T>, ObjectLimit>& objects, int handle) {
 for (const auto& object: objects) { if (object && object->live && object->handle == handle) { return object; } }
 return nullptr;
}
template<class T> auto Free(std::array<std::shared_ptr<T>, ObjectLimit>& objects) {
 return std::find(objects.begin(), objects.end(), nullptr);
}
struct DrainOnExit { ~DrainOnExit() { DrainRetiredEsvmServices(); } };
struct Pending {
 Keys& keys;
 explicit Pending(Keys& value): keys(value) { ++keys.pending; }
 ~Pending() { --keys.pending; }
};
struct CallbackScope {
 Owner& owner;
 explicit CallbackScope(Owner& value): owner(value) { ++owner.callbacks; }
 ~CallbackScope() { --owner.callbacks; }
};
void ObjectComplete(Owner& owner, EsvmObjectCompletion complete, int handle, int result, void* user) {
 CallbackScope callback(owner);
 complete(handle, result, user);
}
void OperationComplete(Owner& owner, EsvmKeysCompletion complete, int result, void* user) {
 CallbackScope callback(owner);
 complete(result, user);
}
// Firmware 13.60 be230 constructs SignedMessage and serializes field 1, enum 4.
// Encode the wire tag/value, rather than inventing a certificate or token.
std::array<uint8_t, 2> EncodeCertificateRequest() {
 constexpr unsigned field = 1, varint_wire_type = 0, service_certificate_request = 4;
 return {static_cast<uint8_t>((field << 3) | varint_wire_type), service_certificate_request};
}
#if defined(KYTY_ESVM_KEYS_TESTS)
thread_local bool public_message_fixture_admission = false;
bool IsNativePublicMessageFixtureConfiguration(const std::vector<uint8_t>& configuration) {
 // Recognizes the exact bounded native default for public-message tests ONLY.
 // It does not certify or advertise support for this configuration's DRM,
 // required requirements or persistent session types.
 if (configuration.size() != EsvmAccessConfigurationBytes) { return false; }
 struct Region { size_t offset; std::string_view bytes; };
 constexpr Region regions[] = {
  {0x20, "cenc"}, {0x30, "webm"}, {0xa0, {"\x02", 1}}, {0xa4, "audio/mp4"},
  {0x200a4, {"\x01", 1}}, {0x200a8, "video/mp4"}, {0x400a8, {"\x01", 1}},
  {0x400b4, "temporary"}, {0x400d4, "persistent-license"},
  {0x400f4, "persistent-usage-record"}, {0x401b4, {"\x03", 1}}
 };
 size_t cursor = 0;
 for (const auto& region: regions) {
  if (std::any_of(configuration.begin() + cursor, configuration.begin() + region.offset,
                  [](auto byte) { return byte != 0; }) ||
      !std::equal(region.bytes.begin(), region.bytes.end(), configuration.begin() + region.offset)) { return false; }
  cursor = region.offset + region.bytes.size();
 }
 return std::all_of(configuration.begin() + cursor, configuration.end(), [](auto byte) { return byte == 0; });
}
#endif
int UnsupportedKeysOperation(int handle, EsvmKeysCompletion complete, void* user, const char* operation) {
 int result = 0;
 auto owner = GetOwner(result);
 if (!owner) { return result; }
 EsvmDispatchLock dispatch(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto keys = Find(owner->keys, handle);
 if (!keys) { return Invalid; }
 if (keys->pending >= PendingLimit) { return Full; }
 Pending pending(*keys);
 Trace(operation, handle, Failed);
 // Accepted failure has a completion. No event publishes fictitious bytes.
 if (Valid(*owner)) { OperationComplete(*owner, complete, Failed, user); }
 return 0;
}
void TraceCertificate(const char* operation, int handle, int role, uint64_t bytes, int result, EsvmCertificateResult backend) {
 static const bool enabled = [] { const char* v = std::getenv("KYTY_MEDIA_TRACE"); return v && std::strcmp(v, "1") == 0; }();
 static std::atomic_uint calls {0};
 if (enabled && calls.fetch_add(1, std::memory_order_relaxed) < 64) {
  LOGF("MediaFramework certificate: operation=%s pid=%d handle=%d role=%d bytes=%llu result=%08x backend=%d\n",
       operation, Loader::GetCurrentGuestProcessId(), handle, role, static_cast<unsigned long long>(bytes),
       static_cast<unsigned>(result), static_cast<int>(backend));
 }
}
int CertificateOperation(int handle, int role, const void* data, uint64_t bytes,
                         EsvmKeysCompletion complete, void* user, EsvmCertificateFormat format) {
 const char* operation = format == EsvmCertificateFormat::SignedCertificate ? "set-signed-certificate" : "parse-certificate-response";
 int result = 0; auto owner = GetOwner(result);
 if (!owner) { TraceCertificate(operation, handle, role, bytes, result, EsvmCertificateResult::InvalidInput); return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto keys = Find(owner->keys, handle); if (!keys) { return Invalid; }
 if (keys->pending >= PendingLimit) { return Full; }
 Pending pending(*keys);
 result = Failed;
 auto backend = EsvmCertificateResult::InvalidInput;
 std::shared_ptr<const EsvmVerifiedServiceCertificate> authenticated;
 // No callbacks, registry lock or host-critical guard inside crypto. The owner
 // dispatch gate bounds/reenters lifetime; retirement invalidates nonblockingly.
 if (role >= 0 && role <= 2 && bytes != 0 && bytes <= EsvmCertificateMaxBytes) {
  try {
   AllocationPoint();
   std::vector<uint8_t> copied(static_cast<size_t>(bytes));
   if (Read(copied.data(), data, copied.size())) {
    auto verification = VerifyEsvmServiceCertificate(copied, format);
    backend = verification.result;
    if (backend == EsvmCertificateResult::Ok && verification.certificate) {
     AllocationPoint();
     authenticated = std::make_shared<const EsvmVerifiedServiceCertificate>(std::move(*verification.certificate));
     result = 0;
    }
   }
  } catch (const std::bad_alloc&) { result = Failed; }
 } else if (bytes > EsvmCertificateMaxBytes) { backend = EsvmCertificateResult::LimitExceeded; }
 // Commit only an authenticated immutable value, after all fallible work. No
 // failed replacement, invalid role or partial allocation changes either slot.
 if (!Valid(*owner)) { return 0; } // Owner cancellation owes no retired callback.
 if (!keys->live) { result = Failed; }
 if (result == 0) {
  if (role == 0 || role == 2) { keys->certificates[0] = authenticated; }
  if (role == 1 || role == 2) { keys->certificates[1] = authenticated; }
  if (format == EsvmCertificateFormat::ServiceCertificateResponse) {
   const auto payload = authenticated->SignedCertificate(); // NOT envelope/unverified temporary.
   const auto event = keys->event;
   {
    CallbackScope callback(*owner);
    event(keys->handle, 1, payload.data(), payload.size());
   }
   // The local immutable owner keeps event bytes alive across replacement,
   // keys destruction or owner retirement reentered from that event callback.
   if (!keys->live) { result = Failed; }
  }
 }
 TraceCertificate(operation, handle, role, bytes, result, backend);
 if (Valid(*owner)) { OperationComplete(*owner, complete, result, user); }
 return 0;
}
} // namespace

#if defined(KYTY_ESVM_KEYS_TESTS)
EsvmKeysPublicMessageFixtureScope::EsvmKeysPublicMessageFixtureScope() noexcept:
 previous(public_message_fixture_admission) {
 public_message_fixture_admission = true;
}
EsvmKeysPublicMessageFixtureScope::~EsvmKeysPublicMessageFixtureScope() {
 public_message_fixture_admission = previous;
}
#endif

void InvalidateEsvmKeysOwner(int pid) {
 Common::HostLockCriticalGuard critical;
 auto& registry = Service();
 std::lock_guard lock(registry.mutex);
 const auto it = registry.owners.find(pid);
 if (it == registry.owners.end()) { return; }
 Retire(registry, it->second);
 registry.owners.erase(it);
}
void DrainRetiredEsvmKeys(bool wait) {
 wait = wait && !EsvmDispatchScopeActive();
 std::shared_ptr<Owner> owner;
 {
  Common::HostLockCriticalGuard critical;
  auto& registry = Service();
  std::lock_guard lock(registry.mutex);
  owner = registry.retired;
 }
 while (owner) {
  std::shared_ptr<Owner> next;
  {
   EsvmDispatchLock dispatch(*owner->dispatch, wait);
   const bool released = dispatch.owns_lock() && owner->callbacks == 0;
   if (released) {
     for (auto& keys: owner->keys) {
      if (keys) { keys->live = false; keys->event = nullptr; keys->certificates = {}; keys.reset(); }
     }
    for (auto& access: owner->accesses) { if (access) { access->live = false; access.reset(); } }
   }
   {
    Common::HostLockCriticalGuard critical;
    auto& registry = Service();
    std::lock_guard lock(registry.mutex);
    next = owner->retired_next;
    // Keep a draining owner discoverable until its callback gate is acquired.
    // Otherwise another thread could steal the queue entry and retirement would
    // return while that thread was still waiting for the guest callback. A
     // reentrant/contended try-drain leaves it until an outer exit retries.
     if (released) {
     auto* link = &registry.retired;
     while (*link && *link != owner) { link = &(*link)->retired_next; }
     if (*link) { *link = owner->retired_next; }
    }
   }
  }
  owner = std::move(next);
 }
}

#define KEYS_ABI __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
int KEYS_ABI EsvmRequestKeySystemAccess(const char* name, const void* configurations, uint32_t count,
                                       EsvmObjectCompletion complete, void* user) try {
 Trace("access-enter", 0, 0);
 DrainOnExit drain;
 if (!name || !configurations || !complete) { return Invalid; }
 std::array<char, 257> copied_name {};
 if (count > 16 || !ReadName(name, copied_name)) { return Invalid; }
 int result = 0;
 auto owner = GetOwner(result);
 if (!owner) { return result; }
 EsvmDispatchLock dispatch(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 // Snapshot all bounded input before completing. Recognition of a name/blob
 // is not support: no production CDM, license-session or decrypt backend exists.
 AllocationPoint();
 std::vector<uint8_t> copied_configurations(static_cast<size_t>(count) * EsvmAccessConfigurationBytes);
 const auto address = reinterpret_cast<uintptr_t>(configurations);
 if (copied_configurations.size() > UINTPTR_MAX - address ||
     (!copied_configurations.empty() && !Read(copied_configurations.data(), configurations, copied_configurations.size()))) {
  return Invalid;
 }
 if (!Valid(*owner)) { return Failed; }
#if defined(KYTY_ESVM_KEYS_TESTS)
 if (public_message_fixture_admission && count == 1 &&
     std::strcmp(copied_name.data(), "com.widevine.alpha.citroncore") == 0 &&
     IsNativePublicMessageFixtureConfiguration(copied_configurations)) {
  auto slot = Free(owner->accesses);
  if (slot == owner->accesses.end()) { return Full; }
  AllocationPoint();
  auto access = std::make_shared<Access>();
  access->configuration = std::move(copied_configurations);
  access->handle = NextHandle();
  if (!access->handle || !Valid(*owner)) { return Failed; }
  *slot = access;
  Trace("access-public-message-fixture-only", access->handle, 0);
  ObjectComplete(*owner, complete, access->handle, 0, user);
  return 0;
 }
#endif
 Trace("access-provider-unsupported", 0, NotSupported);
 if (!Valid(*owner)) { return Failed; }
 ObjectComplete(*owner, complete, 0, NotSupported, user);
 return 0;
} catch (const std::bad_alloc&) { return Failed; }

int KEYS_ABI EsvmDestroyKeySystemAccess(int handle) try {
 DrainOnExit drain;
 int result = 0; auto owner = GetOwner(result);
 if (!owner) { return result; }
 EsvmDispatchLock dispatch(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto access = Find(owner->accesses, handle);
 if (!access) { return Invalid; }
 // Keep an existing keys object's parent alive; deleting access is not license
 // revocation and must not silently invalidate its separately owned keys.
 access->live = false;
 for (auto& slot: owner->accesses) { if (slot == access) { slot.reset(); break; } }
 return 0;
} catch (const std::bad_alloc&) { return Failed; }

int KEYS_ABI EsvmCreateMediaKeys2(int handle, bool option, EsvmKeysEvent event,
                                 EsvmObjectCompletion complete, void* user) try {
 Trace("keys-enter", handle, 0);
 DrainOnExit drain;
 if (!event || !complete) { return Invalid; }
 int result = 0; auto owner = GetOwner(result);
 if (!owner) { return result; }
 EsvmDispatchLock dispatch(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto access = Find(owner->accesses, handle);
 if (!access) { return Invalid; }
 auto slot = Free(owner->keys);
 if (slot == owner->keys.end()) { return Full; }
 AllocationPoint();
 auto keys = std::make_shared<Keys>();
 keys->handle = NextHandle();
 keys->access = access;
 keys->option = option;
 keys->event = event; // Function value copied, never a pointer to caller's record.
 keys->request = EncodeCertificateRequest();
 if (!keys->handle || !Valid(*owner)) { return Failed; }
 *slot = keys;
 Trace("keys-admitted", keys->handle, 0);
 ObjectComplete(*owner, complete, keys->handle, 0, user);
 return 0;
} catch (const std::bad_alloc&) { return Failed; }

int KEYS_ABI EsvmDestroyMediaKeys(int handle) try {
 DrainOnExit drain;
 int result = 0; auto owner = GetOwner(result);
 if (!owner) { return result; }
 EsvmDispatchLock dispatch(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto keys = Find(owner->keys, handle);
 if (!keys) { return Invalid; }
 keys->live = false;
 keys->event = nullptr;
 keys->certificates = {};
 for (auto& slot: owner->keys) { if (slot == keys) { slot.reset(); break; } }
 return 0;
} catch (const std::bad_alloc&) { return Failed; }

int KEYS_ABI EsvmGetServiceCertificateRequest(int handle, EsvmKeysCompletion complete, void* user) try {
 Trace("certificate-request-enter", handle, 0);
 DrainOnExit drain;
 if (!complete) { return Invalid; }
 int result = 0; auto owner = GetOwner(result);
 if (!owner) { Trace("certificate-request-owner-rejected", handle, result); return result; }
 EsvmDispatchLock dispatch(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto keys = Find(owner->keys, handle);
 if (!keys) { Trace("certificate-request-handle-rejected", handle, Invalid); return Invalid; }
 if (keys->pending >= PendingLimit) { return Full; }
 Pending pending(*keys);
 const auto event = keys->event;
 ++keys->requests;
 {
  CallbackScope callback(*owner);
  event(keys->handle, 0, keys->request.data(), keys->request.size());
 }
 Trace("certificate-request-event-delivered", handle, 0);
 // Reentry may destroy keys/owner. Owner retirement suppresses all further
 // callbacks; keys-only destruction completes this already accepted request
 // with failure. The retained operation owns its bytes until dispatch returns.
 if (Valid(*owner)) { OperationComplete(*owner, complete, keys->live ? 0 : Failed, user); }
 return 0;
} catch (const std::bad_alloc&) { return Failed; }

int KEYS_ABI EsvmSetServiceCertificate(int keys, int role, const void* data, uint64_t bytes,
                                      EsvmKeysCompletion complete, void* user) try {
 Trace("certificate-set-enter", keys, 0);
 DrainOnExit drain;
 if (!data || !complete) { return Invalid; }
 return CertificateOperation(keys, role, data, bytes, complete, user, EsvmCertificateFormat::SignedCertificate);
} catch (const std::bad_alloc&) { return Failed; }
int KEYS_ABI EsvmParseServiceCertificateResponse(int keys, int role, const void* data, uint64_t bytes,
                                                EsvmKeysCompletion complete, void* user) try {
 Trace("certificate-parse-enter", keys, 0);
 DrainOnExit drain;
 if (!data || !complete) { return Invalid; }
 return CertificateOperation(keys, role, data, bytes, complete, user, EsvmCertificateFormat::ServiceCertificateResponse);
} catch (const std::bad_alloc&) { return Failed; }
int KEYS_ABI EsvmQueryProvisioning(int handle, EsvmProvisioningCompletion complete, void* user) try {
 Trace("query-provisioning-enter", handle, 0);
 DrainOnExit drain;
 if (!complete) { return Invalid; }
 int result = 0; auto owner = GetOwner(result);
 if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto keys = Find(owner->keys, handle); if (!keys) { return Invalid; }
 if (keys->pending >= PendingLimit) { return Full; }
 Pending pending(*keys);
 // This provider has no device credential/session/private-key backend. Known
 // absence is a successful boolean query, NOT unsupported, provisioned, or a
 // claim that service-certificate acceptance initialized any device credential.
 if (Valid(*owner)) {
  Trace("query-provisioning-known-absent", handle, 0);
  CallbackScope callback(*owner);
  complete(false, 0, user); // Verified DIL=bool, ESI=status, RDX=original cookie.
 }
 return 0;
} catch (const std::bad_alloc&) { return Failed; }
int KEYS_ABI EsvmGetProvisioningRequest(int keys, EsvmKeysCompletion complete, void* user) try {
 Trace("get-provisioning-request-enter", keys, 0);
 DrainOnExit drain;
 if (!complete) { return Invalid; }
 return UnsupportedKeysOperation(keys, complete, user, "get-provisioning-request-accepted-unsupported");
} catch (const std::bad_alloc&) { return Failed; }
int KEYS_ABI EsvmHandleProvisioningResponse(int keys, const void* data, uint64_t,
                                           EsvmKeysCompletion complete, void* user) try {
 Trace("provisioning-response-unsupported", keys, Failed);
 DrainOnExit drain;
 if (!data || !complete) { return Invalid; }
 return UnsupportedKeysOperation(keys, complete, user, "provisioning-response-accepted-unsupported"); // NEVER a certificate parser alias.
} catch (const std::bad_alloc&) { return Failed; }
int KEYS_ABI EsvmRejectMediaKeySession(int handle, int, const void* callbacks, int* session) try {
 Trace("license-session-enter", handle, 0);
 DrainOnExit drain;
 // 13.60 interface e2f0 requires both pointers; a nonzero return means it did
 // not accept an asynchronous session operation, so there is no callback owed.
 if (!callbacks || !session) { return Invalid; }
 int result = 0; auto owner = GetOwner(result);
 if (!owner) { return result; }
 EsvmDispatchLock dispatch(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 if (!Find(owner->keys, handle)) { return Invalid; }
 Trace("license-session-rejected", handle, Failed);
 return Failed; // No CDM/license session, no session handle or plaintext output.
} catch (const std::bad_alloc&) { return Failed; }
int KEYS_ABI EsvmRejectKeysDecrypt(int handle, const void* parameters, void* output, uint64_t* output_bytes) try {
 Trace("keys-decrypt-enter", handle, 0);
 DrainOnExit drain;
 // AooiBF0kst8, 13.60 f7f0: three nonnull pointer arguments; synchronous
 // nonzero rejection owes no async callback and must not touch plaintext/output.
 if (!parameters || !output || !output_bytes) { return Invalid; }
 int result = 0; auto owner = GetOwner(result);
 if (!owner) { return result; }
 EsvmDispatchLock dispatch(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 if (!Find(owner->keys, handle)) { return Invalid; }
 Trace("keys-decrypt-rejected", handle, Failed);
 return Failed;
} catch (const std::bad_alloc&) { return Failed; }

LIB_DEFINE(InitMediaFrameworkKeys_1) {
 LIB_FUNC("Bn34VVQIB-g", EsvmRequestKeySystemAccess);
 LIB_FUNC("ZagUxqhkHbw", EsvmDestroyKeySystemAccess);
 LIB_FUNC("UMuQN3ysX9o", EsvmCreateMediaKeys2);
 LIB_FUNC("3n9Quq9m3Co", EsvmDestroyMediaKeys);
 LIB_FUNC("fuhQBPWLMMk", EsvmGetServiceCertificateRequest);
 LIB_FUNC("siSe0IdqHe0", EsvmSetServiceCertificate);
 LIB_FUNC("Grm60DSbEYA", EsvmParseServiceCertificateResponse);
 LIB_FUNC("O20dcRTe2KE", EsvmQueryProvisioning);
 LIB_FUNC("EezlN9RSX7A", EsvmGetProvisioningRequest);
 LIB_FUNC("HunD-fOxkWI", EsvmHandleProvisioningResponse);
 LIB_FUNC("Mh8bbtUvx1Y", EsvmRejectMediaKeySession);
 LIB_FUNC("AooiBF0kst8", EsvmRejectKeysDecrypt);
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void EsvmKeysFailNextAllocationForTest(size_t successful_allocations) { fail_allocation_after = successful_allocations; }
EsvmKeysResources EsvmKeysResourcesForTest() {
  std::array<std::shared_ptr<Owner>, 128> owners {};
  size_t count = 0;
  EsvmKeysResources result {};
 {
  auto& registry = Service(); std::lock_guard lock(registry.mutex);
   for (const auto& [pid, owner]: registry.owners) { if (count < owners.size()) { owners[count++] = owner; } }
   for (auto owner = registry.retired; owner; owner = owner->retired_next) {
    ++result.retired_owners;
    if (count < owners.size()) { owners[count++] = owner; }
   }
  }
 for (const auto& owner: owners) {
  if (!owner) { continue; }
  EsvmDispatchLock dispatch(*owner->dispatch);
  ++result.owners;
  std::array<const Access*, ObjectLimit * 2> seen {};
  size_t seen_count = 0;
  auto account = [&](const std::shared_ptr<Access>& access) {
   if (access && std::find(seen.begin(), seen.begin() + seen_count, access.get()) == seen.begin() + seen_count) {
    seen[seen_count++] = access.get();
    ++result.accesses; result.configuration_bytes += access->configuration.size();
   }
  };
  for (const auto& access: owner->accesses) {
   account(access);
  }
  for (const auto& keys: owner->keys) {
   if (keys) {
    ++result.keys; result.pending += keys->pending; account(keys->access);
    const EsvmVerifiedServiceCertificate* prior = nullptr;
    for (const auto& certificate: keys->certificates) {
     if (certificate) { ++result.certificate_slots;
      if (certificate.get() != prior) { result.certificate_bytes += certificate->SignedCertificate().size(); }
      prior = certificate.get(); }
    }
   }
  }
 }
 return result;
}
std::vector<uint8_t> EsvmKeysCertificateForTest(int handle, size_t slot) {
 DrainOnExit drain;
 int result = 0; auto owner = GetOwner(result); if (!owner || slot >= 2) { return {}; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return {}; }
 const auto keys = Find(owner->keys, handle); if (!keys || !keys->certificates[slot]) { return {}; }
 const auto bytes = keys->certificates[slot]->SignedCertificate();
 return {bytes.begin(), bytes.end()};
}
#endif
} // namespace Libs::MediaFramework
