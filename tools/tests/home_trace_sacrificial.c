/* Windows-only debugger lifecycle target. No emulator/firmware dependencies. */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

static HANDLE start_event;
static volatile LONG stopping, churn_stop, churned, foreign_seen;
static volatile LONG64 hits;

__declspec(noinline) void probe0(void) { InterlockedIncrement64(&hits); }
__declspec(noinline) void probe1(void) { InterlockedExchangeAdd64(&hits, 2); }
__declspec(noinline) void probe2(void) { InterlockedExchangeAdd64(&hits, 3); }
__declspec(noinline) void probe3(void) { InterlockedExchangeAdd64(&hits, 4); }
__declspec(noinline) void probe4(void) { InterlockedExchangeAdd64(&hits, 5); }
__declspec(noinline) void probe5(void) { InterlockedExchangeAdd64(&hits, 6); }
__declspec(noinline) void probe6(void) { InterlockedExchangeAdd64(&hits, 7); }
__declspec(noinline) void probe7(void) { InterlockedExchangeAdd64(&hits, 8); }
__declspec(noinline) void probe8(void) { InterlockedExchangeAdd64(&hits, 9); }
__declspec(noinline) void probe9(void) { InterlockedExchangeAdd64(&hits, 10); }
__declspec(noinline) void probe10(void) { InterlockedExchangeAdd64(&hits, 11); }
__declspec(noinline) void probe11(void) { InterlockedExchangeAdd64(&hits, 12); }
__declspec(noinline) void probe12(void) { InterlockedExchangeAdd64(&hits, 13); }

static void all_probes(void) {
    probe0(); probe1(); probe2(); probe3(); probe4(); probe5();
    probe6(); probe7(); probe8(); probe9(); probe10(); probe11(); probe12();
}

static LONG CALLBACK foreign_handler(EXCEPTION_POINTERS *ex) {
    if (ex->ExceptionRecord->ExceptionCode == 0xe0424242) {
        InterlockedIncrement(&foreign_seen);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI failed(EXCEPTION_POINTERS *ex) {
    printf("{\"crash\":%lu,\"tid\":%lu,\"dr7\":%llu}\n",
           ex->ExceptionRecord->ExceptionCode, GetCurrentThreadId(),
           (unsigned long long)ex->ContextRecord->Dr7);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI worker(void *unused) {
    (void)unused;
    WaitForSingleObject(start_event, INFINITE);
    while (!stopping) {
        for (int i = 0; i < 32; ++i) {
            all_probes();
        }
        Sleep(1);
    }
    return 0;
}

static DWORD WINAPI transient(void *unused) {
    (void)unused;
    for (int i = 0; i < 8; ++i) {
        all_probes();
    }
    InterlockedIncrement(&churned);
    return 0;
}

static DWORD WINAPI churn(void *unused) {
    (void)unused;
    WaitForSingleObject(start_event, INFINITE);
    while (!churn_stop) {
        HANDLE batch[4];
        for (int i = 0; i < 4; ++i) batch[i] = CreateThread(NULL, 0, transient, NULL, 0, NULL);
        for (int i = 0; i < 4; ++i) {
            if (batch[i]) { WaitForSingleObject(batch[i], INFINITE); CloseHandle(batch[i]); }
        }
        Sleep(1);
    }
    return 0;
}

static int debug_scan(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 e = {0};
    int active = 0;
    e.dwSize = sizeof(e);
    if (snap == INVALID_HANDLE_VALUE || !Thread32First(snap, &e)) return -1;
    do {
        if (e.th32OwnerProcessID != GetCurrentProcessId() || e.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, e.th32ThreadID);
        if (!h) { active = -1; break; }
        DWORD prior = SuspendThread(h);
        CONTEXT c = {0};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (prior == (DWORD)-1 || !GetThreadContext(h, &c)) active = -1;
        else if (c.Dr7 & 255) ++active;
        if (prior != (DWORD)-1 && ResumeThread(h) != prior + 1) active = -1;
        CloseHandle(h);
        if (active < 0) break;
    } while (Thread32Next(snap, &e));
    CloseHandle(snap);
    return active;
}

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 64;
    if (count < 4 || count > 256) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(failed);
    AddVectoredExceptionHandler(1, foreign_handler);
    start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE *workers = calloc(count, sizeof(HANDLE));
    for (int i = 0; i < count; ++i) workers[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    HANDLE churner = CreateThread(NULL, 0, churn, NULL, 0, NULL);
    SetThreadPriority(churner, THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE sentry = CreateThread(NULL, 0, transient, NULL, CREATE_SUSPENDED, NULL);
    printf("{\"pid\":%lu,\"probes\":[\"%p\",\"%p\",\"%p\",\"%p\",\"%p\",\"%p\",\"%p\",\"%p\",\"%p\",\"%p\",\"%p\",\"%p\",\"%p\"]}\n",
           GetCurrentProcessId(), probe0, probe1, probe2, probe3, probe4, probe5,
           probe6, probe7, probe8, probe9, probe10, probe11, probe12);
    fflush(stdout);
    char command[32];
    if (!fgets(command, sizeof(command), stdin) || command[0] != 'G') return 3;
    SetEvent(start_event);
    RaiseException(0xe0424242, 0, 0, NULL);
    if (!fgets(command, sizeof(command), stdin) || command[0] != 'V') return 4;
    InterlockedExchange(&churn_stop, 1);
    if (WaitForSingleObject(churner, 10000) != WAIT_OBJECT_0) return 5;
    CloseHandle(churner);
    int active = debug_scan();
    DWORD sentry_count = ResumeThread(sentry);
    if (WaitForSingleObject(sentry, 10000) != WAIT_OBJECT_0) return 8;
    CloseHandle(sentry);
    LONG64 before = hits;
    Sleep(2000); /* hot calls must continue to survive well beyond detach */
    LONG64 progress = hits - before;
    RaiseException(0xe0424242, 0, 0, NULL);
    InterlockedExchange(&stopping, 1);
    for (int i = 0; i < count; ++i) {
        if (WaitForSingleObject(workers[i], 10000) != WAIT_OBJECT_0) return 6;
        CloseHandle(workers[i]);
    }
    printf("{\"active_dr_threads\":%d,\"progress\":%llu,\"churned\":%ld,\"foreign_seen\":%ld,\"sentry_suspend_count\":%lu}\n",
           active, (unsigned long long)progress, churned, foreign_seen, sentry_count);
    fflush(stdout);
    free(workers);
    CloseHandle(start_event);
    return active == 0 && progress > 1000 && churned > 0 && foreign_seen == 2 && sentry_count == 1 ? 0 : 7;
}
