#pragma once

#include "common/abi.h"

// The guest curl ABI is LP64 even on Windows (long and curl_off_t are 64-bit).
// All callbacks run on the calling guest thread, never on a HostHttp worker.
namespace Libs::RnpsCurl {
#define RNPS_CURL_ABI __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
struct Message {
	int message;
	void* easy;
	union { void* pointer; int result; } data;
};
struct Slist { char* data; Slist* next; };
int RNPS_CURL_ABI GlobalInit(int64_t flags);
void* RNPS_CURL_ABI EasyInit();
void RNPS_CURL_ABI EasyCleanup(void* handle);
int RNPS_CURL_ABI EasySetopt(void* handle, int option, uint64_t value);
int RNPS_CURL_ABI EasyGetinfo(void* handle, int info, void* output);
int RNPS_CURL_ABI EasyPerform(void* handle);
const char* RNPS_CURL_ABI EasyStrerror(int error);
Slist* RNPS_CURL_ABI SlistAppend(Slist* list, const char* text);
void RNPS_CURL_ABI SlistFreeAll(Slist* list);
void* RNPS_CURL_ABI MultiInit();
int RNPS_CURL_ABI MultiCleanup(void* handle);
int RNPS_CURL_ABI MultiSetopt(void* handle, int option, uint64_t value);
int RNPS_CURL_ABI MultiAddHandle(void* multi, void* easy);
int RNPS_CURL_ABI MultiRemoveHandle(void* multi, void* easy);
int RNPS_CURL_ABI MultiPerform(void* multi, int* running);
Message* RNPS_CURL_ABI MultiInfoRead(void* multi, int* remaining);
int RNPS_CURL_ABI MultiPoll(void* multi, void* extra, unsigned count, int timeout, int* ready);
int RNPS_CURL_ABI MultiWakeup(void* multi);
int RNPS_CURL_ABI MultiTimeout(void* multi, int64_t* timeout);
int RNPS_CURL_ABI MultiFdset(void* multi, void* read, void* write, void* except, int* maxfd);
const char* RNPS_CURL_ABI MultiStrerror(int error);
void* RNPS_CURL_ABI ShareInit();
int RNPS_CURL_ABI ShareCleanup(void* share);
int RNPS_CURL_ABI ShareSetopt(void* share, int option, uint64_t value);
const char* RNPS_CURL_ABI PushheaderBynum(void*, size_t);
const char* RNPS_CURL_ABI PushheaderByname(void*, const char*);
} // namespace Libs::RnpsCurl
