#include "common/abi.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("TextToSpeech2", 1, "TextToSpeech2", 1, 1);

namespace TextToSpeech2 {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI TextToSpeech2GetSpeechStatus() {
	PRINT_NAME();

	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI TextToSpeech2Cancel() {
	PRINT_NAME();

	return OK;
}

} // namespace TextToSpeech2

LIB_DEFINE(InitTextToSpeech2_1) {
	LIB_FUNC("08JSg9p6bgQ", TextToSpeech2::TextToSpeech2GetSpeechStatus);
	LIB_FUNC("2jiIxUmcsGo", TextToSpeech2::TextToSpeech2Cancel);
}

} // namespace Libs
