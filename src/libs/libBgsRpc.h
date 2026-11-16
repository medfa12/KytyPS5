#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBBGSRPC_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBBGSRPC_H_

#include <cstdint>
#include <string>

namespace Libs::BgsRpc::BgsRpcHost {

// Host-side BGS-RPC loopback channel (see libBgsRpc.cpp). Connect seeds
// login+show messages; Poll pops oldest (empty when none).
uint64_t    Connect(const char* url);
std::string Poll(uint64_t handle);
void        Close(uint64_t handle);

} // namespace Libs::BgsRpc::BgsRpcHost

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBBGSRPC_H_ */
