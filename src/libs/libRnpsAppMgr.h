#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBRNPSAPPMGR_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBRNPSAPPMGR_H_

#include <cstddef>
#include <string>

namespace Libs::RnpsAppMgr::RnpsAppMgrHost {

// Host-side ShellUI event channel (see libRnpsAppMgr.cpp). Enqueue appends
// to the same queue guest HasEvent/GetEventForShellUI drain.
void   EnqueueShellUIEvent(const std::string& event);
size_t PendingShellUIEvents();
// Pops oldest into *out (false when empty). Lets the resident pump prove
// the full transport -> queue -> consumer circulation.
bool TryConsumeShellUIEvent(std::string* out);

} // namespace Libs::RnpsAppMgr::RnpsAppMgrHost

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBRNPSAPPMGR_H_ */
