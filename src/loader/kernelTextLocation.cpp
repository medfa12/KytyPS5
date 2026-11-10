#include "loader/kernelTextLocation.h"

#include "libs/errno.h"
#include "loader/runtimeLinker.h"

namespace Libs::LibKernel {

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetLibkernelTextLocation(
    uint64_t* /*start*/, uint64_t* /*end*/) {

	const auto* current = Loader::RuntimeLinker::GetCurrentProgram();
	const int error = current != nullptr && current->rt != nullptr
	    ? current->rt->QueryKernelTextLocationError() : Posix::POSIX_ESRCH;
	// 13.60: helper 15fac -> sysctl 11440 -> syscall stub f40 -> errno
	// trampoline d4 (stores errno). Helper 15fb5 reads __error, public 34c5d
	// discards only the return value, not this thread-local errno side effect.
	*Posix::GetErrorAddr() = error;
	return KERNEL_ERROR_UNKNOWN + error;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelInternalMemoryGetModuleSegmentInfo(void* out16) {
	// Match native LEA without C++ null/out-of-object pointer arithmetic. This
	// arithmetic performs no memory access, even for an invalid guest address.
	const auto address = reinterpret_cast<uintptr_t>(out16);
	(void)KernelGetLibkernelTextLocation(reinterpret_cast<uint64_t*>(address),
	                                    reinterpret_cast<uint64_t*>(address + uintptr_t {8}));
	return 0;
}

} // namespace Libs::LibKernel
