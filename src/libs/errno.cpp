#include "libs/errno.h"

namespace Libs::Posix {

static thread_local int g_errno = 0;

__attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI int* GetErrorAddr() {
	return &g_errno;
}

} // namespace Libs::Posix
