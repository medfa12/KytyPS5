// Include in KernelFileSystemTests.cpp's existing test namespace, as with
// NetResolverExTests.inc. TestNetResolverNtoa6() owns Network lifetime: dispatch
// after InitSubsystems but BEFORE InitAll in an isolated fixture process.
// Requires KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS and its existing standard
// array/cstddef/cstdint/cstring/limits/string/thread includes and Net declaration.
namespace NetResolverNtoa6Tests {
namespace Network = Libs::Network;
namespace Net = Network::Net;
struct Api {
  Loader::SymbolDatabase symbols;
  decltype(&Net::NetResolverStartNtoa6) resolve = nullptr;
  decltype(&Net::NetResolverStartNtoaMultipleRecordsEx) records = nullptr;
  decltype(&Net::NetPoolCreate) pool_create = nullptr;
  decltype(&Net::NetPoolDestroy) pool_destroy = nullptr;
  decltype(&Net::NetResolverCreate) create = nullptr;
  decltype(&Net::NetResolverDestroy) destroy = nullptr;
  int* (KYTY_SYSV_ABI *error_addr)() = nullptr;
  template<class F> F Function(const char* nid, const char* name) {
    const auto* exact = symbols.Find({nid, "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
    const auto* fallback = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(exact != nullptr && exact->vaddr != 0 && fallback != nullptr &&
              exact->vaddr == fallback->vaddr && exact->dbg_name == name,
          "Ntoa6 exact/NID binding resolves the production SysV wrapper");
    return reinterpret_cast<F>(exact->vaddr);
  }
  Api() {
    Libs::LibNet::InitNet_1_Net(&symbols);
    resolve = Function<decltype(resolve)>("zl35YNs9jnI", "LibNet::NetResolverStartNtoa6");
    records = Function<decltype(records)>("sT4nBQKUPqM", "LibNet::NetResolverStartNtoaMultipleRecordsEx");
    pool_create = Function<decltype(pool_create)>("dgJBaeJnGpo", "LibNet::NetPoolCreate");
    pool_destroy = Function<decltype(pool_destroy)>("K7RlrTkI-mw", "LibNet::NetPoolDestroy");
    create = Function<decltype(create)>("C4UgDHHPvdw", "LibNet::NetResolverCreate");
    destroy = Function<decltype(destroy)>("kJlYH5uMAWI", "LibNet::NetResolverDestroy");
    error_addr = Function<decltype(error_addr)>("HQOwnfMGipQ", "LibNet::GetNetErrorAddr");
  }
};

struct Guarded {
  std::array<uint8_t, 49> bytes;
  Guarded() { bytes.fill(0xa5); }
  void* Address() { return bytes.data() + 17; } // Deliberately unaligned IPv6 destination.
  void Equal(const uint8_t* address = nullptr) const {
    std::array<uint8_t, 49> expected;
    expected.fill(0xa5);
    if (address != nullptr) { std::memcpy(expected.data() + 17, address, 16); }
    Check(bytes == expected, "Ntoa6 exact 16-byte output/canaries or failure preservation mismatch");
  }
};

void Status(Api& api, int actual, int expected) {
  Check(actual == expected, "Ntoa6 encoded return status");
  Check(*api.error_addr() == (expected < 0 ? expected - Network::NET_ERROR_EPERM + 1 : 0x1234),
        "Ntoa6 publishes Net errno on failure and preserves it on success");
}
void Reject(Api& api, int rid, const char* hostname, int timeout, int retry, int flags, int error) {
  Guarded output;
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, hostname, output.Address(), timeout, retry, flags), error);
  output.Equal();
}
void Literal(Api& api, int rid, const char* hostname, const std::array<uint8_t, 16>& expected, int retry) {
  Guarded output;
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, hostname, output.Address(), 0, retry, 0), 0);
  output.Equal(expected.data());
}
struct Reuse { Api* api; int pool; int victim; bool replace; int replacement = -1; int* main_errno; };
void Retire(void* opaque) {
  auto& reuse = *static_cast<Reuse*>(opaque);
  std::thread worker([&] {
    Check(reuse.api->error_addr() != reuse.main_errno, "Ntoa6 Net errno is thread-local");
    Check(reuse.api->destroy(reuse.victim) == 0, "destroy retained IPv6 resolver");
    if (reuse.replace) {
      reuse.replacement = reuse.api->create("ntoa6-replacement", reuse.pool, 0);
      Check(reuse.replacement == reuse.victim, "force resolver public ID reuse before copyout");
    }
  });
  worker.join(); // Registry locks must not span the retained-operation hook.
}
} // namespace NetResolverNtoa6Tests

void TestNetResolverNtoa6() {
  using namespace NetResolverNtoa6Tests;
  Api api;
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(-1, reinterpret_cast<const char*>(1), reinterpret_cast<void*>(1), -1, -1, -1),
         Network::NET_ERROR_ENOTINIT);
  Network::Initialize();
  const int pool = api.pool_create("ntoa6", 0x10000, 0);
  const int rid = api.create("ntoa6", pool, 0);
  Check(pool >= 3000 && rid >= 0, "Ntoa6 real pool/resolver lifetime");
  std::array<uint8_t, 16> loopback {};
  loopback[15] = 1;
  Literal(api, rid, "::1", loopback, 0);
  Literal(api, rid, "::1", loopback, 1);
  const std::array<uint8_t, 16> any {};
  Literal(api, rid, "::", any, 0); // All-zero address is valid only when actually parsed.
  std::array<uint8_t, 16> documentation {0x20, 0x01, 0x0d, 0xb8};
  documentation[14] = 0x12; documentation[15] = 0x34;
  Literal(api, rid, "2001:db8::1234", documentation, 0);
  Reject(api, rid, "127.0.0.1", 0, 0, 0, Network::NET_ERROR_RESOLVER_ENORECORD);
  for (int bad: {-1, 32, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
    Reject(api, bad, "::1", 0, 0, 0, Network::NET_ERROR_EBADF);
    *api.error_addr() = 0x1234;
    Status(api, api.resolve(bad, reinterpret_cast<const char*>(1), reinterpret_cast<void*>(1), 0, 0, 0),
           Network::NET_ERROR_EBADF); // Invalid IDs are rejected before caller pointers are read.
  }
  for (int flags: {-1, 2, 0x1000000, 0x2000000, 0x400000}) {
    Reject(api, rid, "::1", 0, 0, flags, Network::NET_ERROR_EINVAL);
  }
  Reject(api, -1, nullptr, -1, 0, 0, Network::NET_ERROR_EINVAL);
  Reject(api, -1, nullptr, 0, 0, 2, Network::NET_ERROR_EINVAL);
  for (int flags: {1, 0x100000, 0x200000, 0x300000}) {
    Reject(api, rid, "::1", 0, 0, flags, Network::NET_ERROR_EOPNOTSUPP);
  }
  Reject(api, rid, "::1", -1, 0, 0, Network::NET_ERROR_EINVAL);
  Reject(api, rid, "::1", 999999, 0, 0, Network::NET_ERROR_EINVAL);
  Reject(api, rid, "::1", 0, -1, 0, Network::NET_ERROR_EINVAL);
  Reject(api, rid, "::1", 1000000, 0, 0, Network::NET_ERROR_EOPNOTSUPP);
  Reject(api, rid, "::1", 0, 2, 0, Network::NET_ERROR_EOPNOTSUPP);
  Reject(api, rid, nullptr, 0, 0, 0, Network::NET_ERROR_EINVAL);
  Reject(api, rid, "", 0, 0, 0, Network::NET_ERROR_EINVAL);
  const std::string long_name(256, 'a');
  Reject(api, rid, long_name.c_str(), 0, 0, 0, Network::NET_ERROR_EINVAL);
  Reject(api, rid, "::1", 0, 0, 0x10000, Network::NET_ERROR_EOPNOTSUPP);
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, "::1", nullptr, 0, 0, 0), Network::NET_ERROR_EINVAL);

  // Real localhost AAAA lookup, compared to the existing family-specific API.
  Net::NetResolverInfoEx records {};
  records.rid6 = -1;
  const int actual = api.records(rid, "localhost", &records, 0, 0, 0x1000000);
  Guarded local;
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, "localhost", local.Address(), 0, 0, 0), actual);
  if (actual == 0) {
    Check(records.records > 0 && records.addrs[0].af == 28, "localhost result is actual IPv6, not IPv4 bytes");
    local.Equal(records.addrs[0].addr);
    Guarded disabled;
    *api.error_addr() = 0x1234;
    Status(api, api.resolve(rid, "localhost", disabled.Address(), 0, 0, 0x10000), 0);
    disabled.Equal(records.addrs[0].addr);
  } else { local.Equal(); } // Honest host no-AAAA error leaves all sixteen bytes untouched.

  for (bool replace: {false, true}) {
    const int victim = api.create("ntoa6-retire", pool, 0);
    Check(victim >= 0, "create retained IPv6 resolver fixture");
    Reuse reuse {&api, pool, victim, replace, -1, api.error_addr()};
    Net::NetResolverRetainedHookForTest(Retire, &reuse);
    Reject(api, victim, "::1", 0, 0, 0, Network::NET_ERROR_EBADF);
    if (reuse.replacement >= 0) {
      Literal(api, reuse.replacement, "::1", loopback, 0);
      Check(api.destroy(reuse.replacement) == 0, "close replacement IPv6 resolver");
    }
  }
  Check(api.destroy(rid) == 0, "destroy primary IPv6 resolver");
  Reject(api, rid, "::1", 0, 0, 0, Network::NET_ERROR_EBADF);
  Check(api.pool_destroy(pool) == 0, "destroy Ntoa6 pool");
  Network::Shutdown();
  Reject(api, rid, "::1", 0, 0, 0, Network::NET_ERROR_ENOTINIT);
}
