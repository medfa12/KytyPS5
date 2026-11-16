void TestNetworkNonblockingConnect() {
  namespace Net = Libs::Network::Net;
  namespace Posix = Libs::Posix;
  const int listener = Net::Socket(2, 1, 0);
  const int client = Net::Socket(2, 1, 0);
  Check(listener >= 0 && client >= 0 && client < 1024, "create nonblocking loopback fixture");
  std::array<uint8_t, 16> address {16, 2, 0, 0, 127, 0, 0, 1};
  uint32_t size = address.size();
  Check(Net::Bind(listener, address.data(), size) == 0 && Net::Listen(listener, 1) == 0 &&
            Net::Getsockname(listener, address.data(), &size) == 0, "listen on actual loopback endpoint");
  const int enabled = 1;
  Check(Net::Setsockopt(client, 0xffff, 0x1200, &enabled, sizeof(enabled)) == 0,
        "enable guest nonblocking socket mode");
  const int connected = Net::Connect(client, address.data(), address.size());
  Check(connected == 0 || (connected == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EINPROGRESS),
        "pending connect is BSD EINPROGRESS, never receive-style EWOULDBLOCK");
  std::array<uint32_t, 32> writable {};
  writable[client / 32] |= uint32_t{1} << (client % 32);
  std::array<int64_t, 2> timeout {2, 0};
  Check(Net::Select(client + 1, nullptr, writable.data(), nullptr, timeout.data()) == 1,
        "pending connection completes through ordinary readiness");
  int error = -1;
  size = sizeof(error);
  Check(Net::Getsockopt(client, 0xffff, 0x1007, &error, &size) == 0 && error == 0,
        "completed nonblocking connection has no socket error");
  const int accepted = Net::Accept(listener, nullptr, nullptr);
  Check(accepted >= 0, "accept completed connection");
  char byte = '!';
  Check(Net::Recv(client, &byte, 1, 0) == -1 &&
            *Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK && byte == '!',
        "empty nonblocking receive retains EWOULDBLOCK and preserves output");
  Check(Net::SocketClose(accepted) == 0 && Net::SocketClose(client) == 0 && Net::SocketClose(listener) == 0,
        "close nonblocking fixture");
}
