// Real loopback sockets through the registered POSIX export; no external server.
void TestNetworkPeerName() {
  namespace Net = Libs::Network::Net;
  namespace Posix = Libs::Posix;
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  const auto* symbol = symbols.FindByNid("TXFFFiNldU8", Loader::SymbolType::Func);
  Check(symbol != nullptr, "resolve POSIX getpeername used by Starboard");
  const auto peer = reinterpret_cast<int(KYTY_SYSV_ABI *)(int, void*, uint32_t*)>(symbol->vaddr);
  const int listener = Net::Socket(2, 1, 0);
  const int client = Net::Socket(2, 1, 0);
  Check(listener >= 0 && client >= 0, "create IPv4 stream endpoints");
  std::array<uint8_t, 128> output;
  output.fill(0xa5);
  const auto original = output;
  uint32_t length = output.size();
  Check(peer(client, output.data(), &length) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_ENOTCONN &&
            output == original && length == output.size(),
        "unconnected peer fails without claiming an address");
  std::array<uint8_t, 16> address {16, 2, 0, 0, 127, 0, 0, 1};
  Check(Net::Bind(listener, address.data(), address.size()) == 0 && Net::Listen(listener, 1) == 0,
        "listen on ephemeral loopback port");
  length = address.size();
  Check(Net::Getsockname(listener, address.data(), &length) == 0 && (address[2] || address[3]),
        "obtain assigned loopback port");
  Check(Net::Connect(client, address.data(), address.size()) == 0, "connect loopback stream");
  const int accepted = Net::Accept(listener, nullptr, nullptr);
  Check(accepted >= 0, "accept loopback stream");
  length = output.size();
  *Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
  Check(peer(client, output.data(), &length) == 0 && length == 16 &&
            std::memcmp(output.data(), address.data(), 16) == 0 &&
            std::all_of(output.begin() + 16, output.end(), [](auto b) { return b == 0xa5; }) &&
            *Posix::GetErrorAddr() == Posix::POSIX_EINVAL,
        "Starboard receives BSD family, network-order peer address, exact length and untouched tail");
  std::array<uint8_t, 16> local {};
  length = local.size();
  Check(Net::Getsockname(client, local.data(), &length) == 0, "query client local endpoint");
  length = output.size();
  Check(peer(accepted, output.data(), &length) == 0 && std::memcmp(output.data(), local.data(), 16) == 0,
        "accepted peer is client endpoint, not listener local address");
  for (uint32_t capacity : {0u, 1u, 2u, 7u, 15u}) {
    output.fill(0xa5);
    length = capacity;
    Check(peer(client, output.data(), &length) == 0 && length == 16 &&
              std::memcmp(output.data(), address.data(), capacity) == 0 &&
              std::all_of(output.begin() + capacity, output.end(), [](auto b) { return b == 0xa5; }),
          "short and zero capacities truncate successfully and preserve canary");
  }
  length = output.size();
  Check(peer(client, nullptr, &length) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EFAULT &&
            peer(client, output.data(), nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EFAULT,
        "invalid output pointers fail");
  Check(Net::Send(accepted, "x", 1, 0) == 1, "queue one TCP byte");
  length = output.size();
  char byte = 0;
  Check(peer(client, output.data(), &length) == 0 && Net::Recv(client, &byte, 1, 0) == 1 && byte == 'x',
        "peer query does not consume receive data");
  Check(Net::SocketClose(client) == 0, "close client");
  output = original;
  length = output.size();
  Check(peer(client, output.data(), &length) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EBADF &&
            output == original && length == output.size(), "closed descriptor preserves failed outputs");
  Check(Net::SocketClose(accepted) == 0 && Net::SocketClose(listener) == 0, "close TCP fixture");
  int pair[2] {-1, -1};
  Check(Net::Socketpair(1, 1, 0, pair) == 0, "create Unix socketpair");
  length = output.size();
  Check(peer(pair[0], output.data(), &length) == 0 && length == 16 && output[0] == 16 && output[1] == 1 &&
            std::all_of(output.begin() + 2, output.begin() + 16, [](auto b) { return b == 0; }),
        "socketpair hides host TCP construction address and reports unnamed AF_LOCAL");
  Check(Net::SocketClose(pair[0]) == 0 && Net::SocketClose(pair[1]) == 0, "close socketpair fixture");
}
