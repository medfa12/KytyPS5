#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstring>

namespace Libs::MetadataReaderWriter {

LIB_VERSION("MetadataReaderWriter", 1, "MetadataReaderWriter", 1, 1);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterInitialize(const void* params) {
	PRINT_NAME();
	LOGF("\t params = %p\n", params);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterFinalize() {
	PRINT_NAME();
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterRegisterParserInfo(const void* parser_info) {
	PRINT_NAME();
	LOGF("\t parser_info = %p\n", parser_info);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterUnregisterParserInfo(uint32_t parser_id) {
	PRINT_NAME();
	LOGF("\t parser_id = %u\n", parser_id);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterRegisterParserInterface(uint32_t parser_id, const void* iface) {
	PRINT_NAME();
	LOGF("\t parser_id = %u iface = %p\n", parser_id, iface);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterGetParserInfoList(void* list, uint32_t* count) {
	PRINT_NAME();
	if (count != nullptr) {
		*count = 0;
	}
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterReadMetadata(const void* req, void* resp) {
	PRINT_NAME();
	LOGF("\t req = %p resp = %p\n", req, resp);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterWriteMetadata(const void* req, void* resp) {
	PRINT_NAME();
	LOGF("\t req = %p resp = %p\n", req, resp);
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterExtractThumbnail(const void* req, void* resp) {
	PRINT_NAME();
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterCreateThumbnail(const void* req, void* resp) {
	PRINT_NAME();
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterCreateThumbnail2(const void* req, void* resp) {
	PRINT_NAME();
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterGetMimeType(const void* req, void* resp) {
	PRINT_NAME();
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterGetMimeTypes(const void* req, void* resp) {
	PRINT_NAME();
	return OK;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterGetNumOfMimeType(const void* req, uint32_t* count) {
	PRINT_NAME();
	if (count != nullptr) {
		*count = 0;
	}
	return OK;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
MetadataReaderWriterDefaultStub(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	return 0;
}

LIB_DEFINE(InitMetadataReaderWriter_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("xM1sJyAZllM", MetadataReaderWriterInitialize);
	LIB_FUNC("T5OWyJevfHY", MetadataReaderWriterFinalize);
	LIB_FUNC("vdaZ6LwHRtk", MetadataReaderWriterRegisterParserInfo);
	LIB_FUNC("fcs3yRIkEVM", MetadataReaderWriterUnregisterParserInfo);
	LIB_FUNC("Te5DnUayCF0", MetadataReaderWriterRegisterParserInterface);
	LIB_FUNC("mqNpIDBdg6Q", MetadataReaderWriterGetParserInfoList);
	LIB_FUNC("P-g2IenG6fs", MetadataReaderWriterReadMetadata);
	LIB_FUNC("WWSL9IG8vkw", MetadataReaderWriterWriteMetadata);
	LIB_FUNC("8zEv3rVpAg0", MetadataReaderWriterExtractThumbnail);
	LIB_FUNC("HPhbUpG3eyU", MetadataReaderWriterCreateThumbnail);
	LIB_FUNC("e+JOJUM2NV0", MetadataReaderWriterCreateThumbnail2);
	LIB_FUNC("M-j0MR0fl1Q", MetadataReaderWriterGetMimeType);
	LIB_FUNC("5XPqMu5BF9I", MetadataReaderWriterGetMimeTypes);
	LIB_FUNC("+k0trJi9U40", MetadataReaderWriterGetNumOfMimeType);

	// General exports
	LIB_FUNC("hKxhdSPJr84", MetadataReaderWriterDefaultStub);
	LIB_FUNC("uUeEONFSa30", MetadataReaderWriterDefaultStub);
	LIB_FUNC("82vJelrS+Fk", MetadataReaderWriterDefaultStub);
	LIB_FUNC("DxrIX+BEzdA", MetadataReaderWriterDefaultStub);
	LIB_FUNC("Ar+SWXuO+B4", MetadataReaderWriterDefaultStub);
	LIB_FUNC("6sO25N6BpIE", MetadataReaderWriterDefaultStub);
	LIB_FUNC("Hi+tZPfxucU", MetadataReaderWriterDefaultStub);
	LIB_FUNC("1twpIQ9LVdk", MetadataReaderWriterDefaultStub);
	LIB_FUNC("gYsaS58fv3Y", MetadataReaderWriterDefaultStub);
	LIB_FUNC("-pyEotv2tkA", MetadataReaderWriterDefaultStub);
	LIB_FUNC("mgcqnroon8s", MetadataReaderWriterDefaultStub);
	LIB_FUNC("Zqy0P4obozI", MetadataReaderWriterDefaultStub);
	LIB_FUNC("H1ACo+TgX+A", MetadataReaderWriterDefaultStub);
	LIB_FUNC("ybFfESYz8VA", MetadataReaderWriterDefaultStub);
	LIB_FUNC("4BR2PtzlXac", MetadataReaderWriterDefaultStub);
	LIB_FUNC("aVw8lQCnuYI", MetadataReaderWriterDefaultStub);
	LIB_FUNC("BL648cXsaOw", MetadataReaderWriterDefaultStub);
	LIB_FUNC("2-w0eKjKQ+8", MetadataReaderWriterDefaultStub);
	LIB_FUNC("-Vcre7XqbY4", MetadataReaderWriterDefaultStub);
	LIB_FUNC("4SvaJGoaPY8", MetadataReaderWriterDefaultStub);
	LIB_FUNC("NhQK-n3UmXE", MetadataReaderWriterDefaultStub);
	LIB_FUNC("qAF3mmR+Bq4", MetadataReaderWriterDefaultStub);
	LIB_FUNC("9wWYRpqWnkE", MetadataReaderWriterDefaultStub);
	LIB_FUNC("EqiT4bJrD3U", MetadataReaderWriterDefaultStub);
	LIB_FUNC("tBIZw5S9Lss", MetadataReaderWriterDefaultStub);
	LIB_FUNC("wEZXaLsC7EM", MetadataReaderWriterDefaultStub);
	LIB_FUNC("3M46lOmQrqQ", MetadataReaderWriterDefaultStub);
	LIB_FUNC("K4SAedM77mg", MetadataReaderWriterDefaultStub);
	LIB_FUNC("Wp7dyH-WjjM", MetadataReaderWriterDefaultStub);
	LIB_FUNC("HulhdZP2sYE", MetadataReaderWriterDefaultStub);
	LIB_FUNC("fJ0X7gqv28M", MetadataReaderWriterDefaultStub);
	LIB_FUNC("KIY3JDiHq5E", MetadataReaderWriterDefaultStub);
	LIB_FUNC("E2TQfgp0rl0", MetadataReaderWriterDefaultStub);
	LIB_FUNC("BcmAcgljv78", MetadataReaderWriterDefaultStub);
	LIB_FUNC("7tZN-6O8+XI", MetadataReaderWriterDefaultStub);
	LIB_FUNC("LmDhtl0kAz4", MetadataReaderWriterDefaultStub);
	LIB_FUNC("BGM3os8bUdE", MetadataReaderWriterDefaultStub);
	LIB_FUNC("Je8FZg0jHdg", MetadataReaderWriterDefaultStub);
	LIB_FUNC("vsiIXtrW0ng", MetadataReaderWriterDefaultStub);
	LIB_FUNC("2n2Y65Orzhs", MetadataReaderWriterDefaultStub);
	LIB_FUNC("6j8lgZFObVo", MetadataReaderWriterDefaultStub);
	LIB_FUNC("17G+g+OmP0k", MetadataReaderWriterDefaultStub);
	LIB_FUNC("ODqPXCqOhJs", MetadataReaderWriterDefaultStub);
	LIB_FUNC("BchENGJOUTk", MetadataReaderWriterDefaultStub);
	LIB_FUNC("YPz6ArouTjc", MetadataReaderWriterDefaultStub);
	LIB_FUNC("GCOzTsybYr0", MetadataReaderWriterDefaultStub);
	LIB_FUNC("ElB1bNKymPg", MetadataReaderWriterDefaultStub);
	LIB_FUNC("ckVz4iaNj4E", MetadataReaderWriterDefaultStub);
	LIB_FUNC("Ly4V1dsyKCA", MetadataReaderWriterDefaultStub);
	LIB_FUNC("YeIbFGmyUVM", MetadataReaderWriterDefaultStub);
	LIB_FUNC("2oFr2hbrkRs", MetadataReaderWriterDefaultStub);
}

} // namespace Libs::MetadataReaderWriter
