#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstdint>

namespace Libs::VideoRecordingP {

LIB_VERSION("VideoRecordingP", 1, "VideoRecording", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoRecordingInitialize(const void* param) {
	PRINT_NAME();
	LOGF("\t [ZbyfYeyVajk] sceVideoRecordingInitialize param=%p\n", param);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoRecordingStub_byQbu2Mg3DI(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	(void)a0; (void)a1; (void)a2; (void)a3;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoRecordingStub_taRss8qdZ9Y(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	(void)a0; (void)a1; (void)a2; (void)a3;
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoRecordingStub_iYtbtBx4jvE(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
	PRINT_NAME();
	(void)a0; (void)a1; (void)a2; (void)a3;
	return OK;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI VideoRecordingDefaultStub(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	(void)a1; (void)a2; (void)a3; (void)a4;
	return 0;
}

LIB_DEFINE(InitVideoRecordingP_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("ZbyfYeyVajk", VideoRecordingInitialize);
	LIB_FUNC("byQbu2Mg3DI", VideoRecordingStub_byQbu2Mg3DI);
	LIB_FUNC("taRss8qdZ9Y", VideoRecordingStub_taRss8qdZ9Y);
	LIB_FUNC("iYtbtBx4jvE", VideoRecordingStub_iYtbtBx4jvE);

	LIB_FUNC("QNg8KGixrzg", VideoRecordingDefaultStub);
	LIB_FUNC("X1f4SXr5qQY", VideoRecordingDefaultStub);
	LIB_FUNC("Fc8qxlKINYQ", VideoRecordingDefaultStub);
	LIB_FUNC("PmqSOAzftJg", VideoRecordingDefaultStub);
	LIB_FUNC("sA6+5XdbqMA", VideoRecordingDefaultStub);
	LIB_FUNC("YGRDoLmH2tk", VideoRecordingDefaultStub);
	LIB_FUNC("fZJQzFK4Gv4", VideoRecordingDefaultStub);
	LIB_FUNC("ZvWzS2wTIMc", VideoRecordingDefaultStub);
	LIB_FUNC("iQS6DUtLybE", VideoRecordingDefaultStub);
	LIB_FUNC("7QoTYzVuzto", VideoRecordingDefaultStub);
	LIB_FUNC("KHvkPQJDMLk", VideoRecordingDefaultStub);
	LIB_FUNC("tWoe9IlGAhs", VideoRecordingDefaultStub);
	LIB_FUNC("OOFxrMY+mfI", VideoRecordingDefaultStub);
	LIB_FUNC("KQ34-uHDBto", VideoRecordingDefaultStub);
	LIB_FUNC("26XG0o9fx8Q", VideoRecordingDefaultStub);
	LIB_FUNC("kEfdObutIDg", VideoRecordingDefaultStub);
	LIB_FUNC("qEfkBZEwu7g", VideoRecordingDefaultStub);
	LIB_FUNC("ypNPzNhXzRg", VideoRecordingDefaultStub);
	LIB_FUNC("mopHCpOSZT8", VideoRecordingDefaultStub);
	LIB_FUNC("FDAvHQuI70c", VideoRecordingDefaultStub);
	LIB_FUNC("oryBI-2iyxw", VideoRecordingDefaultStub);
	LIB_FUNC("ufwOTTG2jV4", VideoRecordingDefaultStub);
	LIB_FUNC("zepTKsV7mmE", VideoRecordingDefaultStub);
	LIB_FUNC("BebkD-x+vZc", VideoRecordingDefaultStub);
}

} // namespace Libs::VideoRecordingP
