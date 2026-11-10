#include "libs/processSdk.h"

#include "libs/errno.h"
#include "loader/runtimeLinker.h"

#include <cstring>

namespace Libs::LibKernel {

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetCompiledSdkVersion(uint32_t* out) {
	if (out == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}
	const auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	if (program == nullptr) {
		return KERNEL_ERROR_ESRCH;
	}
	// WB66 reads the immutable process snapshot of ORBI procparam +0x10,
	// not the Prospero SDK at +0x14 or the module containing the caller.
	*out = program->compiled_sdk_version;
	return 0;
}

int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI KernelGetProsperoCompiledSdkVersion(uint32_t* out) {
	if (out == nullptr) { return KERNEL_ERROR_EINVAL; }
	const auto* program = Loader::RuntimeLinker::GetCurrentProgram();
	if (program == nullptr || program->process_abi == Loader::ProcessAbi::Unknown) {
		// No established process snapshot. Do not invent a legacy classification
		// or a successful zero result for an unavailable source.
		return KERNEL_ERROR_ESRCH;
	}
	const bool legacy = program->process_abi == Loader::ProcessAbi::Legacy;
	const uint32_t sdk = legacy ? 0 : program->prospero_compiled_sdk_version;
	// Native writes four bytes directly, including to unaligned destinations.
	std::memcpy(out, &sdk, sizeof(sdk));
	return legacy ? KERNEL_ERROR_EPERM : 0;
}

} // namespace Libs::LibKernel
