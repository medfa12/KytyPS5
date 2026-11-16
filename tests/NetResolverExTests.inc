// Include in KernelFileSystemTests.cpp's existing test namespace. Requires its
// Check(bool, const char*), standard array/atomic/cstddef/cstring/limits/string/
// thread includes, libs/network.h, libs/errno.h, loader/symbolDatabase.h, and the
// global Libs::LibNet::InitNet_1_Net declaration. Run TestNetResolverEx() in an
// isolated mode after InitSubsystems but BEFORE InitAll: it owns Network lifetime.
// Requires KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS (same as the other .inc fixtures).
namespace NetResolverExTests {
namespace Network = Libs::Network;
namespace Net = Network::Net;
using Info = Net::NetResolverInfoEx;
using Address = Net::NetResolverAddrEx;
constexpr int Inet6 = 0x01000000, Inet46 = 0x02000000;

struct Api {
  Loader::SymbolDatabase symbols;
  decltype(&Net::NetResolverStartNtoaMultipleRecordsEx) resolve = nullptr;
  decltype(&Net::NetResolverStartNtoaMultipleRecords) resolve4 = nullptr;
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
          "extended resolver exact/NID binding and production wrapper");
    return reinterpret_cast<F>(exact->vaddr);
  }
  Api() {
    Libs::LibNet::InitNet_1_Net(&symbols);
    resolve = Function<decltype(resolve)>("sT4nBQKUPqM", "LibNet::NetResolverStartNtoaMultipleRecordsEx");
    resolve4 = Function<decltype(resolve4)>("RCCY01Xd+58", "LibNet::NetResolverStartNtoaMultipleRecords");
    pool_create = Function<decltype(pool_create)>("dgJBaeJnGpo", "LibNet::NetPoolCreate");
    pool_destroy = Function<decltype(pool_destroy)>("K7RlrTkI-mw", "LibNet::NetPoolDestroy");
    create = Function<decltype(create)>("C4UgDHHPvdw", "LibNet::NetResolverCreate");
    destroy = Function<decltype(destroy)>("kJlYH5uMAWI", "LibNet::NetResolverDestroy");
    error_addr = Function<decltype(error_addr)>("HQOwnfMGipQ", "LibNet::GetNetErrorAddr");
  }
};

struct Guarded {
  std::array<uint8_t, 16> before;
  Info info;
  std::array<uint8_t, 16> after;
  explicit Guarded(int rid6) {
    before.fill(0x5a);
    std::memset(&info, 0xa5, sizeof(info));
    info.rid6 = rid6;
    after.fill(0xc3);
  }
  void Canaries() const {
    Check(std::all_of(before.begin(), before.end(), [](auto b) { return b == 0x5a; }) &&
              std::all_of(after.begin(), after.end(), [](auto b) { return b == 0xc3; }),
          "extended resolver overwrote bytes adjacent to its exact 384-byte output");
  }
  void Equal(const Info& expected) const {
    Canaries();
    Check(std::memcmp(&info, &expected, sizeof(info)) == 0,
          "extended resolver complete ABI bytes, padding, counts or input rid6 mismatch");
  }
};
static_assert(sizeof(Info) == 384 && sizeof(Address) == 32);
static_assert(offsetof(Info, records) == 0x140 && offsetof(Info, rid6) == 0x14c);
static_assert(offsetof(Guarded, info) == 16 && offsetof(Guarded, after) == 400 && sizeof(Guarded) == 416);

void Status(Api& api, int actual, int expected) {
  Check(actual == expected, "extended resolver encoded return status");
  Check(*api.error_addr() == (expected < 0 ? expected - Network::NET_ERROR_EPERM + 1 : 0x1234),
        "extended resolver must update Net errno on error and preserve it on success");
}

void Literal(Api& api, int rid, int rid6, const char* name, int flags, int family) {
  Guarded output(rid6);
  Info expected {};
  expected.rid6 = rid6;
  expected.records = 1;
  expected.addrs[0].af = family;
  if (family == 2) {
    const uint8_t bytes[4] {127, 0, 0, 1};
    std::memcpy(expected.addrs[0].addr, bytes, sizeof(bytes));
    expected.dns4records = 1;
  } else {
    expected.addrs[0].addr[15] = 1;
    expected.dns6records = 1;
  }
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, name, &output.info, 0, 1, flags), 0);
  output.Equal(expected);
}

void Reject(Api& api, int rid, int rid6, const char* hostname, int timeout, int retry, int flags, int error) {
  Guarded output(rid6);
  const Info original = output.info;
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, hostname, &output.info, timeout, retry, flags), error);
  output.Equal(original); // All validation failures precede output initialization.
}

void Empty(Api& api, int rid, int rid6, const char* hostname, int flags, int error) {
  Guarded output(rid6);
  Info expected {};
  expected.rid6 = rid6;
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, hostname, &output.info, 0, 0, flags), error);
  output.Equal(expected); // Valid query with no records must not retain stale counts/addresses.
}

void BoundedAppend() {
  Guarded output(17);
  output.info = {};
  output.info.rid6 = 17;
  std::array<Address, 12> answers {};
  Info expected {};
  expected.rid6 = 17;
  expected.records = 10;
  expected.dns4records = 6;
  expected.dns6records = 6;
  for (size_t i = 0; i < answers.size(); ++i) {
    // Last two repeat earlier answers. Firmware counts duplicates; it does not
    // globally sort, deduplicate, or count only the ten stored addresses.
    auto& answer = answers[i];
    answer.af = i % 2 == 0 ? 28 : 2;
    answer.addr[0] = 127;
    answer.addr[3] = static_cast<uint8_t>((i % 10) + 1);
    if (answer.af == 28) { answer.addr[15] = static_cast<uint8_t>(i % 10); }
    std::fill(answer.reserved, answer.reserved + 3, 0x7fffffff);
    if (i < 10) {
      expected.addrs[i].af = answer.af;
      std::memcpy(expected.addrs[i].addr, answer.addr, answer.af == 2 ? 4 : 16);
    }
  }
  Net::NetResolverAppendForTest(&output.info, answers.data(), answers.size());
  output.Equal(expected);
  Address unknown {};
  unknown.af = 23; // Host Winsock AF_INET6 must never be mistaken for guest 28.
  Net::NetResolverAppendForTest(&output.info, &unknown, 1);
  output.Equal(expected);
}

void Localhost(Api& api, int rid, int rid6) {
  Guarded v4(rid6), v6(rid6), dual(rid6);
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, "localhost", &v4.info, 0, 0, 0), 0);
  Check(v4.info.records > 0 && v4.info.records <= 10 && v4.info.dns6records == 0,
        "localhost A resolution must use actual host results");
  const int status6 = api.resolve(rid6, "localhost", &v6.info, 0, 0, Inet6);
  Check(status6 == 0 || (status6 < 0 && v6.info.records == 0),
        "localhost AAAA must report real results or a real no-result error");
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, "localhost", &dual.info, 0, 0, Inet46), 0);
  v4.Canaries(); v6.Canaries(); dual.Canaries();
  Check(dual.info.rid6 == rid6 && dual.info.dns4records == v4.info.dns4records &&
            dual.info.dns6records == v6.info.dns6records &&
            dual.info.records == std::min(10, dual.info.dns4records + dual.info.dns6records),
        "INET46 must retain rid6 and combine actual A/AAAA counts");
  int target = 0;
  for (const Info* phase: {&v6.info, &v4.info}) {
    for (int i = 0; i < phase->records && target < 10; ++i, ++target) {
      Check(std::memcmp(&dual.info.addrs[target], &phase->addrs[i], sizeof(Address)) == 0,
            "INET46 synchronous adapter must append AAAA then A in host answer order");
    }
  }
  // DISABLE_IPADDRESS is supported for nonnumeric hostnames; it must not
  // disable real hostname lookup or be aliased to the old IPv4-only API.
  Guarded disabled(rid6);
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, "localhost", &disabled.info, 0, 1, Inet46 | 0x10000), 0);
  disabled.Equal(dual.info);
}

struct Deletion {
  Api* api;
  int pool;
  int victim;
  bool reuse;
  int replacement = -1;
  int* main_errno;
};

void DestroyRetained(void* opaque) {
  auto& deletion = *static_cast<Deletion*>(opaque);
  std::thread worker([&] {
    Check(deletion.api->error_addr() != deletion.main_errno, "Net errno must be thread-local");
    *deletion.api->error_addr() = 0x5678;
    Check(deletion.api->destroy(deletion.victim) == 0, "destroy retained resolver on another thread");
    if (deletion.reuse) {
      deletion.replacement = deletion.api->create("reuse-inflight", deletion.pool, 0);
      Check(deletion.replacement == deletion.victim, "force same public resolver ID reuse");
    }
    Check(*deletion.api->error_addr() == 0x5678, "successful cleanup/create preserves worker errno");
  });
  worker.join(); // The production hook must run after releasing all Net locks.
}

void Lifetime(Api& api, int pool) {
  for (bool secondary: {false, true}) {
    for (bool reuse: {false, true}) {
      const int rid = api.create("retained-primary", pool, 0);
      const int rid6 = api.create("retained-secondary", pool, 0);
      Check(rid >= 0 && rid6 >= 0, "create lifetime fixture resolvers");
      Guarded output(rid6);
      Info expected {};
      expected.rid6 = rid6;
      Deletion deletion {&api, pool, secondary ? rid6 : rid, reuse, -1, api.error_addr()};
      Net::NetResolverRetainedHookForTest(DestroyRetained, &deletion);
      *api.error_addr() = 0x1234;
      Status(api, api.resolve(rid, "127.0.0.1", &output.info, 0, 1, Inet46), Network::NET_ERROR_EBADF);
      output.Equal(expected);
      Check(api.destroy(secondary ? rid : rid6) == 0, "cleanup surviving resolver");
      if (deletion.replacement >= 0) {
        Check(api.destroy(deletion.replacement) == 0, "cleanup replacement without an old result");
      }
    }
  }
}
} // namespace NetResolverExTests

void TestNetResolverEx() {
  using namespace NetResolverExTests;
  Api api;
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(-1, reinterpret_cast<const char*>(1), reinterpret_cast<Info*>(1), -1, -1, -1),
         Network::NET_ERROR_ENOTINIT); // Readiness check must not load guest pointers.
  Network::Initialize();
  const int pool = api.pool_create("resolver-ex", 0x10000, 0);
  const int rid = api.create("resolver-ex-v4", pool, 0);
  const int rid6 = api.create("resolver-ex-v6", pool, 0);
  Check(pool >= 3000 && rid >= 0 && rid6 >= 0, "extended resolver fixture IDs");
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(-1, reinterpret_cast<const char*>(1), reinterpret_cast<Info*>(1), 0, 1, Inet46),
         Network::NET_ERROR_EBADF);
  Literal(api, rid, std::numeric_limits<int>::min(), "127.0.0.1", 0, 2);
  Literal(api, rid, std::numeric_limits<int>::max(), "::1", Inet6, 28);
  Literal(api, rid, rid6, "127.0.0.1", Inet46, 2);
  Literal(api, rid, rid6, "::1", Inet46, 28);
  Empty(api, rid, rid6, "127.0.0.1", Inet6, Network::NET_ERROR_RESOLVER_ENORECORD);
  Empty(api, rid, rid6, "::1", 0, Network::NET_ERROR_RESOLVER_ENORECORD);
  Empty(api, rid, rid6, "127.0.0.1", Inet46 | 0x10000, Network::NET_ERROR_EOPNOTSUPP);
  Empty(api, rid, rid6, "::1", Inet46 | 0x10000, Network::NET_ERROR_EOPNOTSUPP);
  for (int bad: {-1, 32, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
    Reject(api, bad, rid6, "127.0.0.1", 0, 1, Inet46, Network::NET_ERROR_EBADF);
    Reject(api, rid, bad, "127.0.0.1", 0, 1, Inet46, Network::NET_ERROR_EBADF);
  }
  for (int flags: {2, 0x04000000, Inet6 | Inet46, 0x100000, Inet46 | 0x200000}) {
    Reject(api, rid, rid6, "127.0.0.1", 0, 1, flags, Network::NET_ERROR_EINVAL);
  }
  for (int flags: {1, Inet46 | 1, Inet6 | 0x100000, Inet6 | 0x200000}) {
    Reject(api, rid, rid6, "127.0.0.1", 0, 1, flags, Network::NET_ERROR_EOPNOTSUPP);
  }
  Reject(api, rid, rid6, nullptr, 0, 1, Inet46, Network::NET_ERROR_EINVAL);
  Reject(api, rid, rid6, "", 0, 1, Inet46, Network::NET_ERROR_EINVAL);
  const std::string long_name(256, 'a');
  Reject(api, rid, rid6, long_name.c_str(), 0, 1, Inet46, Network::NET_ERROR_EINVAL);
  Reject(api, rid, rid6, "127.0.0.1", -1, 1, Inet46, Network::NET_ERROR_EINVAL);
  Reject(api, rid, rid6, "127.0.0.1", 999999, 1, Inet46, Network::NET_ERROR_EINVAL);
  Reject(api, rid, rid6, "127.0.0.1", 0, -1, Inet46, Network::NET_ERROR_EINVAL);
  Reject(api, rid, rid6, "127.0.0.1", 1000000, 1, Inet46, Network::NET_ERROR_EOPNOTSUPP);
  Reject(api, rid, rid6, "127.0.0.1", 0, 2, Inet46, Network::NET_ERROR_EOPNOTSUPP);
  *api.error_addr() = 0x1234;
  Status(api, api.resolve(rid, "127.0.0.1", nullptr, 0, 1, Inet46), Network::NET_ERROR_EINVAL);
  Reject(api, rid, rid, "127.0.0.1", 0, 1, Inet46, Network::NET_ERROR_RESOLVER_EBUSY);
  const int dead = api.create("dead", pool, 0);
  Check(dead >= 0 && api.destroy(dead) == 0, "prepare destroyed resolver ID");
  Reject(api, dead, rid6, "127.0.0.1", 0, 1, Inet46, Network::NET_ERROR_EBADF);
  Reject(api, rid, dead, "127.0.0.1", 0, 1, Inet46, Network::NET_ERROR_EBADF);
  BoundedAppend();
  Localhost(api, rid, rid6);
  Lifetime(api, pool);
  // Existing IPv4 ABI/export remains byte-for-byte unchanged.
  Net::NetResolverInfo old {};
  Net::NetResolverInfo old_expected {};
  old_expected.addrs[0].addr = 0x0100007fu;
  old_expected.addrs[0].af = 2;
  old_expected.records = old_expected.dns4records = 1;
  Check(api.resolve4(rid, "127.0.0.1", &old, 0, 0, 0) == 0 &&
            std::memcmp(&old, &old_expected, sizeof(old)) == 0, "old IPv4 resolver ABI regression");
  Check(api.destroy(rid6) == 0 && api.destroy(rid) == 0 && api.pool_destroy(pool) == 0,
        "extended resolver fixture cleanup");
  Network::Shutdown();
}
