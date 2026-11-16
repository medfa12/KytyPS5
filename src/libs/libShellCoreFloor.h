#pragma once
#include "common/abi.h"

namespace Libs::ShellCoreFloor {
// Both libkernel registration paths use this wrapper, retaining raw guest RA.
int KYTY_SYSV_ABI SocketpairObserved(int domain, int type, int protocol, int* output);
}
