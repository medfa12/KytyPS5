// Include inside VirtualMemoryAllocationTests' fixture namespace. Top-level add
// libs/mediaFrameworkEsvmKeys.h. Link mediaFrameworkEsvmKeys.cpp and the production
// mediaFrameworkEsvmCertificate.cpp (+ bcrypt on Windows), then dispatch
// TestMediaFrameworkEsvmKeys() in a DEDICATED process after InitSubsystems.
// Existing thread/semaphore/vector and SymbolDatabase/runtime identity includes
// suffice. Define KYTY_ESVM_KEYS_TESTS on this dedicated test target (including
// mediaFrameworkEsvmKeys.cpp), NEVER on production. Default-denial tests use the
// registered production path. Explicit scoped admission below exists ONLY to
// fixture public messages/lifecycle/Query; it does not assert usable DRM.
// This header has only fixture declarations (no standard-library includes).
#if !defined(KYTY_ESVM_KEYS_TESTS)
#error MediaFrameworkEsvmKeysTests requires the dedicated KYTY_ESVM_KEYS_TESTS fixture build
#endif
#include "fixtures/esvm-certificate/public_certificates.h"
namespace MediaFrameworkEsvmKeysTests {
namespace Mf = Libs::MediaFramework;
constexpr const char* Test = "MediaFrameworkEsvmKeys";
constexpr int Invalid = static_cast<int>(0x81790001u);
constexpr int NotSupported = static_cast<int>(0x8179000au);
constexpr int Failed = static_cast<int>(0x81790fffu);
constexpr int NotInitialized = static_cast<int>(0x8179000du);
constexpr int Full = static_cast<int>(0x81790007u);
constexpr size_t CertificateLimit = 16 * 1024; // Production bound, not a firmware ABI claim.
std::vector<uint8_t> CertificateResponse(const char* base64) {
 constexpr const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 std::vector<uint8_t> bytes;
 uint32_t value = 0; unsigned bits = 0;
 for (const char* c = base64; *c && *c != '='; ++c) {
  const char* digit = std::strchr(alphabet, *c);
  Check(Test, digit != nullptr, "authentic public fixture base64 alphabet");
  value = (value << 6) | static_cast<uint32_t>(digit - alphabet); bits += 6;
  if (bits >= 8) { bits -= 8; bytes.push_back(static_cast<uint8_t>(value >> bits)); }
 }
 return bytes;
}
std::vector<uint8_t> SignedCertificate(const std::vector<uint8_t>& response) {
 // These two independent public vectors have the exact five-byte envelope
 // header 08 05 12 <two-byte length>. No production verifier/trust seam is used
 // to derive the expected byte-for-byte event or storage value.
 Check(Test, response.size() >= 5 && response[0] == 8 && response[1] == 5 && response[2] == 18 &&
             (response[3] & 128) != 0 && response[4] < 128 &&
             size_t(response[3] & 127) + size_t(response[4]) * 128 == response.size() - 5,
       "authentic fixture envelope layout and exact signed-certificate length");
 return {response.begin() + 5, response.end()};
}
struct Api {
 Loader::SymbolDatabase symbols;
 decltype(&Mf::EsvmRequestKeySystemAccess) access;
 decltype(&Mf::EsvmDestroyKeySystemAccess) destroy_access;
 decltype(&Mf::EsvmCreateMediaKeys2) create;
 decltype(&Mf::EsvmDestroyMediaKeys) destroy;
 decltype(&Mf::EsvmGetServiceCertificateRequest) request;
 decltype(&Mf::EsvmSetServiceCertificate) certificate;
 decltype(&Mf::EsvmParseServiceCertificateResponse) response;
 decltype(&Mf::EsvmQueryProvisioning) provisioned;
 decltype(&Mf::EsvmGetProvisioningRequest) provisioning;
 decltype(&Mf::EsvmHandleProvisioningResponse) provisioning_response;
 decltype(&Mf::EsvmRejectMediaKeySession) session;
 decltype(&Mf::EsvmRejectKeysDecrypt) decrypt;
 template<class F> F Function(const char* nid, const char* expression) {
  Loader::SymbolResolve resolve {nid, "MediaFrameworkInterface", 1, "MediaFrameworkInterface", 1, 1,
                                 Loader::SymbolType::Func};
  const auto* record = symbols.FindRegistration(resolve, expression);
  const auto* fallback = symbols.FindByNid(nid, Loader::SymbolType::Func);
  Check(Test, record && fallback && record->vaddr == fallback->vaddr,
        "exact registered NID/version/expression and fallback bind same production function");
  return reinterpret_cast<F>(record->vaddr);
 }
 Api() {
  Mf::InitMediaFrameworkKeys_1(&symbols);
  access = Function<decltype(access)>("Bn34VVQIB-g", "EsvmRequestKeySystemAccess");
  destroy_access = Function<decltype(destroy_access)>("ZagUxqhkHbw", "EsvmDestroyKeySystemAccess");
  create = Function<decltype(create)>("UMuQN3ysX9o", "EsvmCreateMediaKeys2");
  destroy = Function<decltype(destroy)>("3n9Quq9m3Co", "EsvmDestroyMediaKeys");
  request = Function<decltype(request)>("fuhQBPWLMMk", "EsvmGetServiceCertificateRequest");
  certificate = Function<decltype(certificate)>("siSe0IdqHe0", "EsvmSetServiceCertificate");
  response = Function<decltype(response)>("Grm60DSbEYA", "EsvmParseServiceCertificateResponse");
  provisioned = Function<decltype(provisioned)>("O20dcRTe2KE", "EsvmQueryProvisioning");
  provisioning = Function<decltype(provisioning)>("EezlN9RSX7A", "EsvmGetProvisioningRequest");
  provisioning_response = Function<decltype(provisioning_response)>("HunD-fOxkWI", "EsvmHandleProvisioningResponse");
  session = Function<decltype(session)>("Mh8bbtUvx1Y", "EsvmRejectMediaKeySession");
  decrypt = Function<decltype(decrypt)>("AooiBF0kst8", "EsvmRejectKeysDecrypt");
 }
};
struct Completion {
 int calls = 0, handle = -1, result = 0x1234;
 std::vector<int>* order = nullptr;
 void* received_user = nullptr;
 static KYTY_SYSV_ABI void Object(int handle, int result, void* user) {
  auto& out = *static_cast<Completion*>(user);
  ++out.calls; out.handle = handle; out.result = result; out.received_user = user;
 }
 static KYTY_SYSV_ABI void Done(int result, void* user) {
  auto& out = *static_cast<Completion*>(user);
  ++out.calls; out.result = result; out.received_user = user;
  if (out.order) { out.order->push_back(2); }
 }
};
struct QueryCompletion {
 Api* api;
 int keys, calls = 0, result = 0x1234, action = 0, recursive_full = 0;
 bool provisioned = true;
 void* received_user = nullptr;
 uint64_t critical_depth;
 static constexpr uint64_t Cookie = UINT64_C(0xa1593f70cd6824be);
 uint64_t cookie = Cookie;
 explicit QueryCompletion(Api& a, int k): api(&a), keys(k), critical_depth(Common::GetHostLockCriticalCounter()->load()) {}
 static KYTY_SYSV_ABI void Done(bool provisioned, int result, void* user) {
  auto& out = *static_cast<QueryCompletion*>(user);
  Check(Test, out.cookie == Cookie && Common::GetHostLockCriticalCounter()->load() == out.critical_depth,
        "Query's third argument is the original cookie, with no host-critical lock spanning callback");
  ++out.calls; out.provisioned = provisioned; out.result = result; out.received_user = user;
  Check(Test, !provisioned && result == 0, "known absent device credentials is boolean false and separate successful status zero");
  if (out.action == 1) { Check(Test, out.api->destroy(out.keys) == 0, "Query completion reenters keys destruction"); }
  if (out.action == 2) {
   const int accepted = out.api->provisioned(out.keys, Done, &out);
   if (accepted == Full) { ++out.recursive_full; }
   else { Check(Test, accepted == 0, "nested Query accepted or bounded Full only"); }
  }
  if (out.action == 3) { Check(Test, Mf::EsvmUnregisterClient("citroncore") == 0, "Query completion reenters owner unregister"); }
 }
};
struct Events {
 Api* api;
 int keys = 0, calls = 0, action = 0;
 int recursive_full = 0;
 std::vector<int> order;
 std::array<uint8_t, 2> bytes {};
 int certificate_calls = 0;
 std::vector<uint8_t> expected_certificate, certificate_payload, replacement, response;
 std::vector<uint8_t>* poison_input = nullptr;
 uint64_t critical_depth = 0;
 std::binary_semaphore entered {0}, resume {0};
};
static thread_local Events* current = nullptr;
static KYTY_SYSV_ABI void Event(int keys, int event, const void* data, uint64_t bytes) {
 auto& out = *current;
 Check(Test, keys == out.keys && data && (event == 0 || event == 1),
       "native event ABI is keys,event,data,uint64 length; only real certificate protocol events");
 Check(Test, Common::GetHostLockCriticalCounter()->load() == out.critical_depth,
       "no host-critical registry/lifecycle lock spans guest callbacks");
 if (event == 0) {
  Check(Test, bytes == 2, "certificate request is event zero with two bytes");
  std::memcpy(out.bytes.data(), data, 2);
 } else {
  Check(Test, bytes == out.expected_certificate.size() &&
              std::memcmp(data, out.expected_certificate.data(), static_cast<size_t>(bytes)) == 0,
        "event one carries exact authenticated signed certificate, not envelope or reserialization");
  out.certificate_payload.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
  ++out.certificate_calls;
  if (out.poison_input) { std::fill(out.poison_input->begin(), out.poison_input->end(), 0xa5); }
 }
 out.order.push_back(1); ++out.calls;
 // All of these reenter production lifecycle/registry code.
 if (out.action == 1) { Check(Test, out.api->destroy(keys) == 0, "destroy keys from event callback"); }
 if (out.action == 2) { Check(Test, Mf::EsvmUnregisterClient("citroncore") == 0, "unregister from event callback"); }
 if (out.action == 3) { Check(Test, Mf::EsvmTerminate() == 0, "terminate from event callback"); }
 if (out.action == 4) {
  Completion done;
  const int result = out.api->request(keys, Completion::Done, &done);
  if (result == Full) { ++out.recursive_full; Check(Test, done.calls == 0, "queue-full request not accepted"); }
  else { Check(Test, result == 0 && done.calls == 1 && done.result == 0, "bounded nested request completes"); }
 }
 if (out.action == 5) {
  out.entered.release();
  Check(Test, out.resume.try_acquire_for(std::chrono::seconds(5)), "release held callback during concurrent retirement");
 }
 if (out.action == 6) { Check(Test, Mf::ShutdownEsvmService(), "shutdown from callback"); }
 if (out.action == 7) {
  Completion done;
  Check(Test, out.api->certificate(keys, 2, out.replacement.data(), out.replacement.size(), Completion::Done, &done) == 0 &&
              done.calls == 1 && done.result == 0,
        "event reentry replaces both role slots using independently authenticated certificate");
 }
 if (out.action == 8) {
  Completion done;
  const int result = out.api->response(keys, 2, out.response.data(), out.response.size(), Completion::Done, &done);
  if (result == Full) { ++out.recursive_full; Check(Test, done.calls == 0, "full certificate operation not accepted"); }
  else { Check(Test, result == 0 && done.calls == 1 && done.result == 0, "bounded nested Parse completes"); }
 }
 if (event == 1) {
  Check(Test, bytes == out.expected_certificate.size() &&
              std::memcmp(data, out.expected_certificate.data(), static_cast<size_t>(bytes)) == 0,
        "event payload survives input poison, nested replacement, destruction and retirement until callback return");
 }
}
int FixtureAccess(Api& api, const std::vector<uint8_t>& config) {
 Mf::EsvmKeysPublicMessageFixtureScope fixture;
 Completion done;
 Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == 0 &&
             done.calls == 1 && done.result == 0 && done.handle > 0 && done.received_user == &done,
       "TEST-ONLY public-message fixture owns copied configuration; NOT usable DRM admission");
 return done.handle;
}
int Keys(Api& api, int access, Events& events) {
 Completion done;
 Check(Test, api.create(access, true, Event, Completion::Object, &done) == 0 && done.calls == 1 &&
             done.result == 0 && done.handle > 0 && done.handle != access, "create protocol-backed keys through registered ABI");
 events.keys = done.handle;
 return done.handle;
}
void EmptyObjects() {
 const auto resources = Mf::EsvmKeysResourcesForTest();
 Check(Test, resources.accesses == 0 && resources.keys == 0 && resources.configuration_bytes == 0 && resources.pending == 0 &&
             resources.certificate_slots == 0 && resources.certificate_bytes == 0,
        "all protocol object allocations/configurations/certificates/operations released");
}
static KYTY_SYSV_ABI void UnexpectedEvent(int, int, const void*, uint64_t) {
 Check(Test, false, "unsupported access/invalid keys must never publish a data event");
}
void UnsupportedAccess(Api& api, const std::vector<uint8_t>& config, uint32_t count = 1,
                       const char* name = "com.widevine.alpha.citroncore") {
 const auto before = Mf::EsvmKeysResourcesForTest();
 Completion done, no_keys;
 Check(Test, api.access(name, config.data(), count, Completion::Object, &done) == 0 && done.calls == 1 &&
             done.handle == 0 && done.result == NotSupported && done.received_user == &done,
       "default access accepts bounded request then completes exactly once with zero handle/8179000a/original user");
 Check(Test, api.create(done.handle, false, UnexpectedEvent, Completion::Object, &no_keys) == Invalid &&
             no_keys.calls == 0 && api.destroy_access(done.handle) == Invalid,
       "unsupported completion cannot create MediaKeys, destroy an access or emit data");
 const auto after = Mf::EsvmKeysResourcesForTest();
 Check(Test, before.accesses == after.accesses && before.keys == after.keys &&
             before.configuration_bytes == after.configuration_bytes && before.pending == after.pending &&
             before.certificate_slots == after.certificate_slots && before.certificate_bytes == after.certificate_bytes,
       "unsupported access retains no configuration/object/pending operation or certificate, including with existing fixtures");
}
struct DenialReentry {
 Api* api;
 const std::vector<uint8_t>* config;
 int action = 0, calls = 0;
 void* received_user = nullptr;
 uint64_t critical_depth;
 static constexpr uint64_t Cookie = UINT64_C(0xe15f7a934cb2068d);
 uint64_t cookie = Cookie;
 std::binary_semaphore entered {0}, resume {0};
 DenialReentry(Api& a, const std::vector<uint8_t>& c): api(&a), config(&c),
   critical_depth(Common::GetHostLockCriticalCounter()->load()) {}
 static KYTY_SYSV_ABI void Done(int handle, int result, void* user) {
  auto& out = *static_cast<DenialReentry*>(user);
  Check(Test, out.cookie == Cookie && handle == 0 && result == NotSupported && out.calls == 0 &&
              Common::GetHostLockCriticalCounter()->load() == out.critical_depth,
        "denial callback preserves exact ABI/cookie, runs once and holds no host-critical lifecycle/registry lock");
  ++out.calls; out.received_user = user;
  if (out.action == 1) { UnsupportedAccess(*out.api, *out.config); }
  if (out.action == 2) {
   Check(Test, Mf::EsvmUnregisterClient("citroncore") == 0, "denial completion reenters owner unregister");
   Completion untouched;
   Check(Test, out.api->access("com.widevine.alpha.citroncore", out.config->data(), 1, Completion::Object, &untouched) == Failed &&
               untouched.calls == 0, "retired owner cannot accept another request/callback");
   Check(Test, Mf::EsvmRegisterClient("citroncore") == 0, "denial completion reenters new registration epoch");
   UnsupportedAccess(*out.api, *out.config);
  }
  if (out.action == 3) {
   out.entered.release();
   Check(Test, out.resume.try_acquire_for(std::chrono::seconds(5)), "release held denial completion during retirement");
  }
 }
};
void DefaultDenialAndFixtureIsolation(Api& api, const std::vector<uint8_t>& config, const Mf::EsvmInit24& init) {
  // First exercise the actual default path before enabling ANY fixture scope.
  const int before_handle = Mf::AllocateEsvmServiceHandle();
  UnsupportedAccess(api, config);
  Check(Test, Mf::AllocateEsvmServiceHandle() == before_handle + 1,
        "production/default denial does not consume an access or MediaKeys service handle");
  EmptyObjects();
  auto changed = config; changed.back() = 1;
  UnsupportedAccess(api, changed);
  UnsupportedAccess(api, config, 0); // Existing transport accepts zero candidate count, but no provider supports it.
  UnsupportedAccess(api, config, 1, "com.widevine.alpha");
  UnsupportedAccess(api, config, 1, "unrecognized.key.system");
  UnsupportedAccess(api, config, 1, "");
  std::array<char, 257> name; name.fill('a'); name.back() = 0;
  UnsupportedAccess(api, config, 1, name.data());
  name.back() = 'a'; Completion untouched;
  Check(Test, api.access(name.data(), config.data(), 1, Completion::Object, &untouched) == Invalid && untouched.calls == 0,
        "unterminated overlong name fails safely before accepting a callback");
  std::vector<uint8_t> candidates(16 * Mf::EsvmAccessConfigurationBytes, 0);
  std::memcpy(candidates.data(), config.data(), config.size());
  UnsupportedAccess(api, candidates, 2);
  UnsupportedAccess(api, candidates, 16);
  Check(Test, api.access("unrecognized.key.system", reinterpret_cast<const void*>(1), 1, Completion::Object, &untouched) == Invalid &&
              api.access("com.widevine.alpha.citroncore", reinterpret_cast<const void*>(UINTPTR_MAX - 7), 1,
                         Completion::Object, &untouched) == Invalid && untouched.calls == 0,
        "unknown names do not bypass safe bounded configuration copying; overflowing address range rejects");
  for (int action: {1, 2}) {
   DenialReentry out(api, config); out.action = action;
   Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, DenialReentry::Done, &out) == 0 &&
               out.calls == 1 && out.received_user == &out,
         "denial safely reenters access and owner retirement/re-registration without a second outer callback");
   EmptyObjects();
  }
  DenialReentry held(api, config); held.action = 3;
  std::atomic_bool retired {false};
  std::thread caller([&] {
   Loader::RuntimeLinker::SetCurrentProgram(nullptr);
   Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, DenialReentry::Done, &held) == 0,
         "held default denial is accepted without a pretend provider");
  });
  Check(Test, held.entered.try_acquire_for(std::chrono::seconds(5)), "denial callback entered");
  std::thread retire([&] { Check(Test, Mf::ReleaseEsvmOwner(100), "concurrent denial owner retirement"); retired.store(true); });
  uint64_t epoch = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (Mf::GetEsvmClientEpoch(100, &epoch) == 0 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  Check(Test, Mf::GetEsvmClientEpoch(100, &epoch) != 0 && !retired.load(),
        "denial retirement closes admission before waiting for the in-flight callback gate");
  held.resume.release(); caller.join(); retire.join();
  Check(Test, retired.load() && held.calls == 1 && held.received_user == &held, "denial completes once and retirement drains");
  EmptyObjects();
  Check(Test, Mf::EsvmInitialize(&init, false) == 0 && Mf::EsvmRegisterClient("citroncore") == 0,
        "fresh direct owner after default-denial retirement");
  UnsupportedAccess(api, config);

  // Only now exercise fixture admission. Its entire positive branch is compiled
  // out of production; scopes must restore OFF, including nesting and exceptions.
  {
   Mf::EsvmKeysPublicMessageFixtureScope outer;
   {
    Mf::EsvmKeysPublicMessageFixtureScope inner;
    Check(Test, api.destroy_access(FixtureAccess(api, config)) == 0, "nested explicit fixture admission and cleanup");
   }
   Completion done;
   Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == 0 &&
               done.calls == 1 && done.result == 0 && done.handle > 0 && done.received_user == &done &&
               api.destroy_access(done.handle) == 0, "inner scope restores active outer fixture scope, not production capability");
   std::thread isolated([&] {
    Loader::RuntimeLinker::SetCurrentProgram(nullptr);
    UnsupportedAccess(api, config);
   });
   isolated.join();
   UnsupportedAccess(api, changed);
   UnsupportedAccess(api, config, 1, "unrecognized.key.system");
   for (size_t allocation: {size_t {0}, size_t {1}}) {
    Completion failed;
    Mf::EsvmKeysFailNextAllocationForTest(allocation);
    Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &failed) == Failed &&
                failed.calls == 0, "fixture snapshot/object allocation failure never publishes success or a handle");
    EmptyObjects();
   }
  }
  UnsupportedAccess(api, config);
  try {
   Mf::EsvmKeysPublicMessageFixtureScope fixture;
   throw 17;
  } catch (int value) { Check(Test, value == 17, "explicit fixture scope exception test"); }
  UnsupportedAccess(api, config);
  EmptyObjects();
}
void Certificates(Api& api, int keys, Events& events) {
 const auto common_response = CertificateResponse(EsvmCertificateFixture::CommonResponseBase64);
 const auto common = SignedCertificate(common_response);
 const auto staging_response = CertificateResponse(EsvmCertificateFixture::StagingResponseBase64);
 const auto staging = SignedCertificate(staging_response);
 Check(Test, common_response.size() == 716 && common.size() == 711 && staging_response.size() == 714 && staging.size() == 709,
       "independent authentic public Common/Staging vectors, no synthetic positive certificate");
 events.expected_certificate = common;
 auto invoke = [&](auto operation, int role, const void* data, uint64_t bytes, int expected, bool emits) {
  const int before = events.calls;
  events.order.clear();
  Completion done; done.order = &events.order;
  Check(Test, operation(keys, role, data, bytes, Completion::Done, &done) == 0 && done.calls == 1 && done.result == expected &&
              events.calls == before + int(emits) && events.order == (emits ? std::vector<int> {1, 2} : std::vector<int> {2}),
        "accepted certificate operation completes once; only successful Parse emits event one BEFORE completion");
 };
 auto stored = [&](const std::vector<uint8_t>& license, const std::vector<uint8_t>& provisioning, size_t owned_bytes) {
  const auto resources = Mf::EsvmKeysResourcesForTest();
  Check(Test, Mf::EsvmKeysCertificateForTest(keys, 0) == license && Mf::EsvmKeysCertificateForTest(keys, 1) == provisioning &&
              resources.certificate_slots == size_t(!license.empty()) + size_t(!provisioning.empty()) &&
              resources.certificate_bytes == owned_bytes && resources.pending == 0,
        "exact owner/keys role bytes and logical unique authenticated storage accounting");
 };
 stored({}, {}, 0);
 auto caller_raw = common;
 invoke(api.certificate, 0, caller_raw.data(), caller_raw.size(), 0, false);
 std::fill(caller_raw.begin(), caller_raw.end(), 0xa5);
 stored(common, {}, common.size());
 invoke(api.certificate, 1, staging.data(), staging.size(), 0, false);
 stored(common, staging, common.size() + staging.size());
 invoke(api.certificate, 2, common.data(), common.size(), 0, false);
 stored(common, common, common.size());
 events.expected_certificate = staging;
 invoke(api.response, 0, staging_response.data(), staging_response.size(), 0, true);
 stored(staging, common, staging.size() + common.size());
 events.expected_certificate = common;
 invoke(api.response, 1, common_response.data(), common_response.size(), 0, true);
 stored(staging, common, staging.size() + common.size());
 auto caller_response = common_response;
 events.poison_input = &caller_response;
 invoke(api.response, 2, caller_response.data(), caller_response.size(), 0, true);
 events.poison_input = nullptr;
 Check(Test, std::all_of(caller_response.begin(), caller_response.end(), [](auto byte) { return byte == 0xa5; }) &&
             events.certificate_payload == common, "Parse owns authenticated event bytes before guest mutates input envelope");
 stored(common, common, common.size());

 auto tampered_raw = staging; tampered_raw.back() ^= 1;
 auto tampered_response = staging_response; tampered_response.back() ^= 1;
 for (int role: {0, 1, 2}) {
  invoke(api.certificate, role, tampered_raw.data(), tampered_raw.size(), Failed, false);
  invoke(api.response, role, tampered_response.data(), tampered_response.size(), Failed, false);
  stored(common, common, common.size());
 }
 for (auto operation: {api.certificate, api.response}) {
  const auto& valid = operation == api.certificate ? staging : staging_response;
  const auto& wrong_format = operation == api.certificate ? staging_response : staging;
  invoke(operation, 2, wrong_format.data(), wrong_format.size(), Failed, false);
  for (int role: {-1, 3, INT32_MAX}) { invoke(operation, role, valid.data(), valid.size(), Failed, false); }
  invoke(operation, 2, valid.data(), 0, Failed, false);
  invoke(operation, 2, reinterpret_cast<const void*>(1), 8, Failed, false);
  invoke(operation, 2, reinterpret_cast<const void*>(1), CertificateLimit + 1, Failed, false);
  invoke(operation, 2, reinterpret_cast<const void*>(1), UINT64_MAX, Failed, false);
  std::vector<uint8_t> bounded_garbage(CertificateLimit, 0);
  invoke(operation, 2, bounded_garbage.data(), bounded_garbage.size(), Failed, false);
  // Both the bounded input snapshot and immutable certificate allocation fail
  // independently, after operation acceptance and before any role-slot commit.
  for (size_t allocation: {size_t {0}, size_t {1}}) {
   Mf::EsvmKeysFailNextAllocationForTest(allocation);
   invoke(operation, 2, valid.data(), valid.size(), Failed, false);
   stored(common, common, common.size());
  }
  Completion untouched;
  Check(Test, operation(keys, 2, nullptr, valid.size(), Completion::Done, &untouched) == Invalid &&
              operation(keys, 2, valid.data(), valid.size(), nullptr, &untouched) == Invalid &&
              operation(0, 2, valid.data(), valid.size(), Completion::Done, &untouched) == Invalid && untouched.calls == 0,
        "null pointer/completion and zero keys reject transport without accepting a callback");
  stored(common, common, common.size());
 }
 const int before_provisioning = events.calls;
 Completion unsupported;
 Check(Test, api.provisioning_response(keys, common_response.data(), common_response.size(), Completion::Done, &unsupported) == 0 &&
             unsupported.calls == 1 && unsupported.result == Failed && events.calls == before_provisioning,
       "even an authentic certificate envelope is NOT accepted as a provisioning response");
 QueryCompletion query(api, keys);
 Check(Test, api.provisioned(keys, QueryCompletion::Done, &query) == 0 && query.calls == 1 && !query.provisioned &&
             query.result == 0 && query.received_user == &query && events.calls == before_provisioning,
       "authenticated service certificates leave successful device Query false and emit no provisioning event");
 unsupported = {};
 Check(Test, api.provisioning(keys, Completion::Done, &unsupported) == 0 && unsupported.calls == 1 && unsupported.result == Failed &&
             events.calls == before_provisioning, "Request remains unsupported and is never forwarded to boolean Query");
 stored(common, common, common.size());

 events.action = 7; events.replacement = staging;
 invoke(api.response, 2, common_response.data(), common_response.size(), 0, true);
 stored(staging, staging, staging.size());
 events.action = 8; events.response = common_response;
 const int before_nested = events.certificate_calls, before_full = events.recursive_full;
 Completion nested;
 Check(Test, api.response(keys, 2, common_response.data(), common_response.size(), Completion::Done, &nested) == 0 &&
             nested.calls == 1 && nested.result == 0 && events.certificate_calls == before_nested + 8 &&
             events.recursive_full == before_full + 1 && Mf::EsvmKeysResourcesForTest().pending == 0,
       "recursive authenticated Parse uses the shared eight-operation admission bound and fully drains");
 events.action = 0;
 stored(common, common, common.size());
 events.order.clear();
}
} // namespace MediaFrameworkEsvmKeysTests

void TestMediaFrameworkEsvmKeys() try {
 using namespace MediaFrameworkEsvmKeysTests;
 namespace Fs = Libs::LibKernel::FileSystem;
 using Loader::RuntimeLinker;
 Fs::Initialize();
 struct Restore {
  Loader::Program* previous = RuntimeLinker::GetCurrentProgram();
  ~Restore() { RuntimeLinker::SetCurrentProgram(previous); Fs::Shutdown(); }
 } restore;
 RuntimeLinker::SetCurrentProgram(nullptr);
 Check(Test, Mf::ReleaseEsvmOwner(100), "clear direct owner");
 Api api;
 Completion done;
 QueryCompletion unopened_query(api, 1);
 Check(Test, api.provisioned(1, nullptr, &unopened_query) == Invalid &&
             api.provisioned(1, QueryCompletion::Done, &unopened_query) == NotInitialized && unopened_query.calls == 0,
       "Query preserves null-completion precedence and initialization admission before any callback");
 std::vector<uint8_t> config(Mf::EsvmAccessConfigurationBytes, 0);
 // Independent golden bytes from native Esvm 13.60's configuration record.
 for (const auto& [offset, text]: std::array<std::pair<size_t, const char*>, 7> {{
        {0x20, "cenc"}, {0x30, "webm"}, {0xa4, "audio/mp4"}, {0x200a8, "video/mp4"},
        {0x400b4, "temporary"}, {0x400d4, "persistent-license"}, {0x400f4, "persistent-usage-record"}}}) {
  std::memcpy(config.data() + offset, text, std::strlen(text));
 }
 config[0xa0] = 2; config[0x200a4] = 1; config[0x400a8] = 1; config[0x401b4] = 3;
 Check(Test, api.create(1, false, nullptr, Completion::Object, &done) == Invalid &&
             api.create(1, false, Event, nullptr, &done) == Invalid && api.request(1, nullptr, &done) == Invalid &&
             api.access(nullptr, config.data(), 1, Completion::Object, &done) == Invalid &&
             api.access("com.widevine.alpha.citroncore", nullptr, 1, Completion::Object, &done) == Invalid &&
             api.access("com.widevine.alpha.citroncore", config.data(), 1, nullptr, &done) == Invalid && done.calls == 0,
       "native null callback/pointer precedence before initialization");
 Check(Test, api.request(1, Completion::Done, &done) == NotInitialized &&
             api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == NotInitialized && done.calls == 0,
       "uninitialized service/access rejects without callback or capability advertisement");
 Mf::EsvmInit24 init {24, 0, 0x1fff, 700, 0};
 Check(Test, Mf::EsvmInitialize(&init, false) == 0 && api.request(1, Completion::Done, &done) == Failed &&
             api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == Failed && done.calls == 0,
       "initialized boolean alone is not registered service admission");
 Check(Test, Mf::EsvmRegisterClient("citroncore") == 0, "acquire actual SHM/semaphore registration resources");
 Check(Test, api.request(0, Completion::Done, &done) == Invalid && done.calls == 0 &&
             Mf::EsvmKeysResourcesForTest().keys == 0,
       "zero is not a public/default/lazy MediaKeys object; request cannot mint keys or callbacks");
 Check(Test, api.access(reinterpret_cast<const char*>(1), config.data(), 1, Completion::Object, &done) == Invalid &&
             api.access("com.widevine.alpha.citroncore", reinterpret_cast<void*>(1), 1, Completion::Object, &done) == Invalid &&
             api.access("com.widevine.alpha.citroncore", config.data(), 17, Completion::Object, &done) == Invalid && done.calls == 0,
       "bounded name/config reads and native maximum-count validation");
 Mf::EsvmKeysFailNextAllocationForTest();
 Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == Failed && done.calls == 0,
       "default-denial snapshot allocation failure does not accept a callback or publish a handle");
 EmptyObjects();
 DefaultDenialAndFixtureIsolation(api, config, init);
 Events events {&api}; current = &events;
 events.critical_depth = Common::GetHostLockCriticalCounter()->load();
 int access = FixtureAccess(api, config);
 UnsupportedAccess(api, config); // Scope has reset OFF even though a fixture access still exists.
 done = {};
 Mf::EsvmKeysFailNextAllocationForTest();
 Check(Test, api.create(access, true, Event, Completion::Object, &done) == Failed && done.calls == 0 &&
             Mf::EsvmKeysResourcesForTest().keys == 0, "keys allocation failure does not admit an unbacked object");
 config[0] = 0xff; // Caller storage is not retained by the service.
 const int keys = Keys(api, access, events);
 auto resources = Mf::EsvmKeysResourcesForTest();
 Check(Test, resources.accesses == 1 && resources.keys == 1 && resources.configuration_bytes == config.size(),
       "TEST-ONLY admitted public-message objects own actual configuration/protocol state, not DRM capabilities");
 Completion request; request.order = &events.order;
 Check(Test, api.request(keys, Completion::Done, &request) == 0 && request.calls == 1 && request.result == 0 &&
             events.order == std::vector<int>({1, 2}) && events.bytes == std::array<uint8_t, 2>({8, 4}),
       "native event populates exactly 0804 before completion, using copied configuration");
 done = {};
 Check(Test, api.create(keys, false, Event, Completion::Object, &done) == Invalid &&
             api.request(access, Completion::Done, &done) == Invalid && done.calls == 0,
       "typed handles cannot alias access/keys/local media controls");
 uint8_t response = 0;
 for (auto operation: {api.certificate, api.response}) {
  done = {};
  Check(Test, operation(keys, 0, &response, 1, Completion::Done, &done) == 0 && done.calls == 1 && done.result == Failed,
        "malformed certificate processing completes failure and emits no request/provisioning event");
 }
 QueryCompletion initial_query(api, keys);
 Check(Test, api.provisioned(keys, QueryCompletion::Done, &initial_query) == 0 && initial_query.calls == 1 &&
             !initial_query.provisioned && initial_query.result == 0 && initial_query.received_user == &initial_query,
       "pre-certificate Query returns false/zero/original user through exact three-argument callback");
 Check(Test, api.provisioned(keys, nullptr, &initial_query) == Invalid &&
             api.provisioned(0, QueryCompletion::Done, &initial_query) == Invalid &&
             api.provisioned(access, QueryCompletion::Done, &initial_query) == Invalid && initial_query.calls == 1,
       "null completion, zero and wrong typed handles reject Query without a callback");
 done = {};
 Check(Test, api.provisioning(keys, Completion::Done, &done) == 0 && done.calls == 1 && done.result == Failed,
       "provisioning Request keeps separate two-argument failure completion, no token or fabricated request");
 done = {};
 Check(Test, api.provisioning_response(keys, &response, 1, Completion::Done, &done) == 0 &&
             done.calls == 1 && done.result == Failed, "provisioning response cannot create credential state");
 int session = 0x5a5a5a5a;
 Check(Test, api.session(keys, 0, &response, &session) == Failed && session == 0x5a5a5a5a &&
             api.session(keys, 0, nullptr, &session) == Invalid,
       "no unbacked license/decrypt session is admitted; rejection leaves output unchanged");
 std::array<uint8_t, 48> plaintext; plaintext.fill(0xa5);
 uint64_t output_bytes = UINT64_C(0x5a5a5a5a5a5a5a5a);
 Check(Test, api.decrypt(keys, &response, plaintext.data(), &output_bytes) == Failed &&
             std::all_of(plaintext.begin(), plaintext.end(), [](auto byte) { return byte == 0xa5; }) &&
             output_bytes == UINT64_C(0x5a5a5a5a5a5a5a5a) &&
             api.decrypt(keys, nullptr, plaintext.data(), &output_bytes) == Invalid,
       "direct MediaKeys decrypt explicitly fails with no plaintext/length writes");
 Check(Test, events.calls == 1, "unsupported operations did not fabricate data events");
 Certificates(api, keys, events);
 QueryCompletion nested_query(api, keys); nested_query.action = 2;
 const int before_queries = events.calls;
 Check(Test, api.provisioned(keys, QueryCompletion::Done, &nested_query) == 0 && nested_query.calls == 8 &&
             nested_query.recursive_full == 1 && Mf::EsvmKeysResourcesForTest().pending == 0 && events.calls == before_queries,
       "Query shares bounded pending admission and drains after reentrant queries without emitting data events");
 const int query_destroyed = Keys(api, access, events);
 QueryCompletion destroyed_query(api, query_destroyed); destroyed_query.action = 1;
 Check(Test, api.provisioned(query_destroyed, QueryCompletion::Done, &destroyed_query) == 0 && destroyed_query.calls == 1 &&
             destroyed_query.received_user == &destroyed_query && api.destroy(query_destroyed) == Invalid &&
             Mf::EsvmKeysResourcesForTest().pending == 0,
       "Query retains accepted keys/pending lifetime across destruction in its completion");
 events.keys = keys;
 const auto common_response = CertificateResponse(EsvmCertificateFixture::CommonResponseBase64);
 const int parsed_destroyed = Keys(api, access, events);
 Check(Test, Mf::EsvmKeysCertificateForTest(parsed_destroyed, 0).empty() && Mf::EsvmKeysCertificateForTest(parsed_destroyed, 1).empty(),
       "a second keys object under the same access does not inherit another keys object's certificates");
 events.action = 1; done = {};
 Check(Test, api.response(parsed_destroyed, 2, common_response.data(), common_response.size(), Completion::Done, &done) == 0 &&
             done.calls == 1 && done.result == Failed && Mf::EsvmKeysCertificateForTest(parsed_destroyed, 0).empty() &&
             Mf::EsvmKeysCertificateForTest(keys, 0) == events.expected_certificate && Mf::EsvmKeysResourcesForTest().certificate_slots == 2,
       "Parse event destroys its keys and clears both slots without invalidating event bytes or another keys object's storage");
 events.keys = keys;
 Check(Test, api.destroy_access(access) == 0 && api.destroy_access(access) == Invalid &&
             Mf::EsvmKeysResourcesForTest().configuration_bytes == config.size(),
       "destroyed access handle cannot be reused; existing keys retain their owned parent configuration");
 events.action = 4; events.order.clear();
 const int before_request_full = events.recursive_full;
 done = {};
 Check(Test, api.request(keys, Completion::Done, &done) == 0 && done.calls == 1 && events.recursive_full == before_request_full + 1 &&
             Mf::EsvmKeysResourcesForTest().pending == 0, "reentrant request admission is bounded and fully drains");
 events.action = 1; done = {};
 Check(Test, api.request(keys, Completion::Done, &done) == 0 && done.calls == 1 && done.result == Failed &&
             api.request(keys, Completion::Done, &done) == Invalid && api.destroy(keys) == Invalid,
       "reentrant keys destruction invalidates events and completes accepted request failure");
 Check(Test, api.destroy_access(access) == Invalid, "last keys destruction released the retained access backing");
 EmptyObjects();
 config[0] = 0;
 access = FixtureAccess(api, config);
 int fresh_keys = Keys(api, access, events);
 Check(Test, fresh_keys != keys, "destroyed keys handles are never reused");
 events.action = 2; done = {};
 Check(Test, api.response(fresh_keys, 2, common_response.data(), common_response.size(), Completion::Done, &done) == 0 && done.calls == 0,
       "Parse event unregister cancels completion; authenticated payload remains valid until event return");
 EmptyObjects();
 Check(Test, Mf::EsvmRegisterClient("citroncore") == 0 && api.request(fresh_keys, Completion::Done, &done) == Invalid,
       "registration epoch cannot revive stale keys");
 access = FixtureAccess(api, config); events.action = 0;
 const int query_retired = Keys(api, access, events);
 QueryCompletion retired_query(api, query_retired); retired_query.action = 3;
 Check(Test, api.provisioned(query_retired, QueryCompletion::Done, &retired_query) == 0 && retired_query.calls == 1 &&
             retired_query.received_user == &retired_query, "Query completion may retire owner without retaining credentials or later callbacks");
 EmptyObjects();
 Check(Test, Mf::EsvmRegisterClient("citroncore") == 0 &&
             api.provisioned(query_retired, QueryCompletion::Done, &retired_query) == Invalid && retired_query.calls == 1,
       "fresh registration cannot revive a retired Query handle");
 access = FixtureAccess(api, config);
 events.action = 0;
 for (int i = 0; i < 8; ++i) { Keys(api, access, events); }
 done = {};
 Check(Test, api.create(access, true, Event, Completion::Object, &done) == Full && done.calls == 0,
       "bounded keys capacity is not unbounded successful handles");
 for (int i = 1; i < 8; ++i) { FixtureAccess(api, config); }
 UnsupportedAccess(api, config); // Production policy must not report fixture capacity Full.
 done = {};
 {
  Mf::EsvmKeysPublicMessageFixtureScope fixture;
  Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == Full && done.calls == 0,
        "explicit public-message fixture retains bounded access capacity before acceptance");
 }
 Check(Test, Mf::EsvmTerminate() == 0, "direct teardown"); EmptyObjects();

 std::array<Loader::Program*, 2> owners {};
 std::array<int, 2> pids {};
 std::array<int, 2> owner_keys {};
 for (size_t index = 0; index < owners.size(); ++index) {
  owners[index] = new Loader::Program;
  owners[index]->unique_id = 793000 + static_cast<int>(index);
  owners[index]->application_owner = owners[index];
  pids[index] = Loader::RegisterRuntimeProcessIdentity(owners[index], 793000 + static_cast<uint32_t>(index));
  RuntimeLinker::SetCurrentProgram(owners[index]);
  Check(Test, Mf::EsvmInitialize(&init, false) == 0 && Mf::EsvmRegisterClient("citroncore") == 0, "runtime owner admission");
  events.action = 0;
  UnsupportedAccess(api, config); // Every real runtime owner defaults to denial before fixture admission.
  owner_keys[index] = Keys(api, FixtureAccess(api, config), events);
  UnsupportedAccess(api, config); // A fixture under this owner does not enable later default requests.
  const auto owner_response = CertificateResponse(index == 0 ? EsvmCertificateFixture::CommonResponseBase64 :
                                                             EsvmCertificateFixture::StagingResponseBase64);
  events.expected_certificate = SignedCertificate(owner_response);
  done = {};
  Check(Test, api.response(owner_keys[index], 2, owner_response.data(), owner_response.size(), Completion::Done, &done) == 0 &&
              done.calls == 1 && done.result == 0 &&
              Mf::EsvmKeysCertificateForTest(owner_keys[index], 0) == events.expected_certificate,
        "independent runtime owners retain distinct authenticated role certificates");
 }
 done = {};
 Check(Test, api.request(owner_keys[0], Completion::Done, &done) == Invalid &&
             api.destroy(owner_keys[0]) == Invalid && done.calls == 0, "cross-PID query and destruction isolation");
 QueryCompletion foreign_query(api, owner_keys[0]);
 Check(Test, api.provisioned(owner_keys[0], QueryCompletion::Done, &foreign_query) == Invalid && foreign_query.calls == 0,
       "provisioning Query cannot borrow another owner's typed keys or callback cookie");
 Check(Test, api.certificate(owner_keys[0], 2, common_response.data(), common_response.size(), Completion::Done, &done) == Invalid &&
             api.response(owner_keys[0], 2, common_response.data(), common_response.size(), Completion::Done, &done) == Invalid &&
             Mf::EsvmKeysCertificateForTest(owner_keys[0], 0).empty() && done.calls == 0,
       "cross-owner Set/Parse and test role getter cannot borrow or replace another owner's keys");
 Loader::Program unknown;
 unknown.application_owner = &unknown;
 RuntimeLinker::SetCurrentProgram(&unknown);
 Check(Test, api.request(owner_keys[0], Completion::Done, &done) == Failed &&
             api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == Failed && done.calls == 0,
       "unregistered runtime owner cannot borrow direct PID or protocol objects");
 {
  Mf::EsvmKeysPublicMessageFixtureScope fixture;
  Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == Failed && done.calls == 0,
        "explicit test-only fixture cannot bypass runtime owner identity admission");
 }
 Loader::Program private_module;
 private_module.application_owner = owners[0];
 RuntimeLinker::SetCurrentProgram(&private_module);
 UnsupportedAccess(api, config); // Private modules inherit owner identity, never a supported DRM capability.
 events.keys = owner_keys[0]; events.action = 3;
 events.expected_certificate = SignedCertificate(common_response);
 done = {};
 Check(Test, api.response(owner_keys[0], 2, common_response.data(), common_response.size(), Completion::Done, &done) == 0 && done.calls == 0 &&
             Mf::EsvmKeysResourcesForTest().keys == 1, "private module inherits owner; Parse event termination only retires its role storage");

 // Drain waits for an admitted guest callback without holding lifecycle/registry
 // locks; after retirement returns there can be no event or completion for it.
 RuntimeLinker::SetCurrentProgram(owners[1]);
 events.keys = owner_keys[1]; events.action = 5;
 std::atomic_bool retired {false};
 Completion held;
 std::thread caller([&] {
  RuntimeLinker::SetCurrentProgram(owners[1]); current = &events;
  Check(Test, api.response(owner_keys[1], 2, common_response.data(), common_response.size(), Completion::Done, &held) == 0,
        "held Parse accepted with pinned authenticated event payload");
  RuntimeLinker::SetCurrentProgram(nullptr);
 });
 Check(Test, events.entered.try_acquire_for(std::chrono::seconds(5)), "callback entered");
 std::thread retire([&] { Check(Test, Mf::ReleaseEsvmOwner(pids[1]), "concurrent retirement"); retired.store(true); });
 uint64_t epoch = 0;
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
 while (Mf::GetEsvmClientEpoch(pids[1], &epoch) == 0 && std::chrono::steady_clock::now() < deadline) {
  std::this_thread::yield();
 }
 Check(Test, Mf::GetEsvmClientEpoch(pids[1], &epoch) != 0 && !retired.load(),
       "retirement closes admission before waiting; it cannot finish during callback");
 events.resume.release(); caller.join(); retire.join();
 Check(Test, retired.load() && held.calls == 0, "owner retirement suppresses later completion"); EmptyObjects();
 for (size_t index = 0; index < owners.size(); ++index) {
  Check(Test, Mf::ReleaseEsvmOwner(pids[index]) && Loader::DeactivateRuntimeProcessIdentity(owners[index]), "runtime teardown");
 }
 RuntimeLinker::SetCurrentProgram(nullptr);
 Check(Test, Mf::EsvmInitialize(&init, false) == 0 && Mf::EsvmRegisterClient("citroncore") == 0, "final shutdown owner");
 events.action = 6;
 UnsupportedAccess(api, config);
 fresh_keys = Keys(api, FixtureAccess(api, config), events); done = {};
 Check(Test, api.response(fresh_keys, 2, common_response.data(), common_response.size(), Completion::Done, &done) == 0 &&
             done.calls == 0 && Mf::ShutdownEsvmService(),
       "Parse event shutdown cancels remaining callbacks, preserves event payload and cannot readmit keys");
 EmptyObjects();
 Check(Test, api.request(fresh_keys, Completion::Done, &done) == Failed && done.calls == 0,
       "post-shutdown request cannot recreate protocol resources");
 Check(Test, api.access("com.widevine.alpha.citroncore", config.data(), 1, Completion::Object, &done) == Failed && done.calls == 0,
       "post-shutdown default denial cannot recreate an owner or accept a callback");
 current = nullptr;
} catch (const std::exception& error) {
 Check("MediaFrameworkEsvmKeys", false, error.what());
}
