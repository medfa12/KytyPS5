// Included once by libKernel.cpp, outside its namespaces, after platform headers.
#include "libs/semaphoreProvenance.h"
#include <limits>
#include <new>
#include <type_traits>

namespace Libs::LibKernel::SemaphoreProvenance {
namespace {
constexpr uint64_t Magic = 0x0031564f52504d53ull; // "SMPROV1\0", little endian
constexpr size_t Banks = 6, PerBank = 24, Count = Banks * PerBank;
constexpr uint64_t NoSlot = UINT64_MAX;
constexpr uint8_t ShellHash[32] = {
    0x68,0x70,0x11,0x3e,0x17,0x63,0x33,0xd9,0x5f,0x12,0x41,0x86,0x4a,0xf3,0x5c,0x20,
    0xac,0x71,0x15,0xc2,0x39,0x48,0xe0,0x02,0xda,0x0e,0xba,0xec,0x48,0x19,0x25,0x0a};
// Every field is a uint64 word except the final 32 raw name bytes. See decoder.
struct Record {
    uint64_t event = 0, begin_sequence = 0, end_sequence = 0, caller = 0;
    uint64_t tid = 0, object = 0, pin_state = 0;
    uint64_t args[6] {};
    uint64_t before[6] {}, after[6] {}; // mask, appId, event, sema, locked, output
    uint64_t result = 0, errno_before = 0, errno_after = 0;
    char name[32] {};
};
struct Slot {
    std::atomic<uint64_t> entry_commit {0}, exit_commit {0};
    Record record {};
};
struct Buffer {
    uint64_t magic = Magic, version = 1, bytes = 39424, pid = 0;
    std::atomic<uint64_t> ready {0}, sequence {0}, pin_state {0};
    uint64_t base = 0, image_size = 0, program_id = 0;
    uint8_t expected_hash[32] {};
    std::atomic<uint64_t> reserved[Banks] {}, dropped[Banks] {};
    uint64_t padding[6] {};
    Slot slots[Count] {};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<Buffer*>::is_always_lock_free);
static_assert(std::atomic_bool::is_always_lock_free);
static_assert(sizeof(Record) == 256 && sizeof(Slot) == 272);
static_assert(offsetof(Buffer, slots) == 256 && sizeof(Buffer) == 39424);
static_assert(sizeof(EventFlag::KernelEventFlag) == 8 && sizeof(Semaphore::KernelSema) == 8);
std::atomic<Buffer*> g_buffer {nullptr};
std::atomic_bool g_attempted {false}; // startup-only, failed/disabled setup never rearms

template<class T> uint64_t Carrier(T value) noexcept {
    if constexpr (std::is_pointer_v<T>) { return reinterpret_cast<uint64_t>(value); }
    else { return static_cast<uint64_t>(value); }
}

// Preserve the real API's guest errno and the host last-error slot around every
// observation operation, including failed reads. No logging or heap operations.
struct PreserveErrors {
    int guest = *Posix::GetErrorAddr();
#if defined(_WIN32)
    DWORD host = GetLastError();
#endif
    ~PreserveErrors() {
        *Posix::GetErrorAddr() = guest;
#if defined(_WIN32)
        SetLastError(host);
#endif
    }
};

bool Read(uint64_t address, void* output, size_t size) noexcept {
#if defined(_WIN32)
    if (address == 0 || address > UINTPTR_MAX - size) { return false; }
    const DWORD saved = GetLastError();
    SIZE_T copied = 0;
    const bool okay = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                       output, size, &copied) != 0 && copied == size;
    SetLastError(saved);
    return okay;
#else
    (void)address; (void)output; (void)size;
    return false;
#endif
}

template<class T> void Field(uint64_t address, uint64_t bit, uint64_t& mask, uint64_t& value) noexcept {
    T local {};
    if (Read(address, &local, sizeof(local))) { value = static_cast<uint64_t>(local); mask |= bit; }
}
void Snapshot(uint64_t object, uint64_t output, uint64_t (&words)[6]) noexcept {
    if (object != 0) {
        Field<uint32_t>(object, 1, words[0], words[1]);
        Field<uint64_t>(object + 0x480, 2, words[0], words[2]);
        Field<uint64_t>(object + 0x4e8, 4, words[0], words[3]);
        Field<uint8_t>(object + 0x4f0, 8, words[0], words[4]);
    }
    if (output != 0) { Field<uint64_t>(output, 16, words[0], words[5]); }
}

// Verify call-site bytes after mapping, against the separately fingerprinted
// backing image. A name or caller address alone never permits object arithmetic.
bool CallSite(uint64_t base, uint64_t caller_rva, uint64_t plt_rva) noexcept {
    uint8_t instruction[5] {};
    if (!Read(base + caller_rva - 5, instruction, sizeof(instruction)) || instruction[0] != 0xe8) { return false; }
    int32_t displacement = 0;
    std::memcpy(&displacement, instruction + 1, 4);
    return static_cast<int64_t>(caller_rva) + displacement == static_cast<int64_t>(plt_rva);
}

class Call {
    Buffer* buffer = nullptr;
    uint64_t index = NoSlot;
public:
    Call(uint64_t event, uint64_t caller, const std::array<uint64_t, 6>& args) noexcept {
        PreserveErrors preserve;
        buffer = g_buffer.load(std::memory_order_acquire);
        if (buffer == nullptr) { return; }
        const auto pin = buffer->pin_state.load(std::memory_order_acquire);
        constexpr uint64_t callers[3] {0x6e690e, 0x6e69ca, 0x6f65ca};
        const bool pinned = pin == 1 && caller == buffer->base + callers[event - 1];
        const size_t bank = static_cast<size_t>(event - 1) + (pinned ? 0 : 3);
        auto next = buffer->reserved[bank].load(std::memory_order_relaxed);
        while (next < PerBank) {
            if (buffer->reserved[bank].compare_exchange_strong(next, next + 1, std::memory_order_relaxed)) {
                index = bank * PerBank + next; break;
            }
        }
        if (index == NoSlot) { buffer->dropped[bank].fetch_add(1, std::memory_order_relaxed); return; }
        auto& slot = buffer->slots[index];
        auto& record = slot.record;
        record.event = event;
        record.begin_sequence = buffer->sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        record.caller = caller;
        record.pin_state = pin;
#if defined(_WIN32)
        record.tid = GetCurrentThreadId();
#endif
        std::copy(args.begin(), args.end(), record.args);
        record.errno_before = static_cast<uint32_t>(*Posix::GetErrorAddr());
        const uint64_t output = event == 3 ? 0 : args[0];
        const uint64_t offset = event == 1 ? 0x480 : 0x4e8;
        if (pinned && event != 3 && output > offset && output - offset <= UINTPTR_MAX - 0x4f1) {
            record.object = output - offset;
        }
        // Poll deliberately has unknown object. Never guess it from a handle,
        // a previous producer, a stack word, or RBX after a C++ prologue.
        Snapshot(record.object, output, record.before);
        if (event != 3) {
            char name[32] {};
            if (Read(args[1], name, sizeof(name))) {
                std::memcpy(record.name, name, sizeof(name));
                record.before[0] |= 32;
            }
        }
        slot.entry_commit.store(index + 1, std::memory_order_release);
    }
    void Finish(int result) noexcept {
        PreserveErrors preserve;
        if (index == NoSlot) { return; }
        auto& slot = buffer->slots[index];
        auto& record = slot.record;
        record.result = static_cast<uint32_t>(result);
        record.errno_after = static_cast<uint32_t>(*Posix::GetErrorAddr());
        Snapshot(record.object, record.event == 3 ? 0 : record.args[0], record.after);
        record.end_sequence = buffer->sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        slot.exit_commit.store(index + 1, std::memory_order_release);
    }
};
} // namespace

bool Enabled() noexcept { return g_buffer.load(std::memory_order_acquire) != nullptr; }

void Initialize() noexcept {
#if defined(_WIN32) && defined(__clang__) && (defined(_M_X64) || defined(__x86_64__))
    PreserveErrors preserve;
    if (g_attempted.exchange(true)) { return; }
    const char* enabled = std::getenv("KYTY_SEMAPHORE_PROVENANCE");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) { return; }
    try {
        wchar_t local[32768] {};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
        LARGE_INTEGER now {};
        if (length == 0 || length >= 32768 || !QueryPerformanceCounter(&now)) { return; }
        const auto directory = std::filesystem::path(local) / "Temp" / "opencode";
        std::filesystem::create_directories(directory);
        const auto path = directory / ("semaphore-provenance-" + std::to_string(GetCurrentProcessId()) +
                                      "-" + std::to_string(now.QuadPart) + ".bin");
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { return; }
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, sizeof(Buffer), nullptr);
        void* view = mapping == nullptr ? nullptr : MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Buffer));
        if (mapping != nullptr) { CloseHandle(mapping); }
        CloseHandle(file);
        if (view == nullptr) { DeleteFileW(path.c_str()); return; }
        // Process-lifetime mapping. No guest-thread file/flush/teardown work.
        auto* buffer = new (view) Buffer {};
        buffer->pid = GetCurrentProcessId();
        std::memcpy(buffer->expected_hash, ShellHash, sizeof(ShellHash));
        buffer->ready.store(1, std::memory_order_release);
        g_buffer.store(buffer, std::memory_order_release);
    } catch (...) { /* Observation setup failure disables observation only. */ }
#endif
}

void Placement(Loader::Elf64* elf, const std::filesystem::path& filename,
               uint64_t base, uint64_t size, int32_t program_id) noexcept {
    auto* buffer = g_buffer.load(std::memory_order_acquire);
    if (buffer == nullptr) { return; }
    PreserveErrors preserve;
    // Called under the existing loader serialization after this mapping is loaded.
    // Metadata below is immutable once a nonzero pin state is published.
    try {
        if (filename.filename() != "SceShellCore.elf") { return; }
        if (buffer->pin_state.load() != 0) { buffer->pin_state.store(3, std::memory_order_release); return; }
        buffer->base = base;
        buffer->image_size = size;
        buffer->program_id = static_cast<uint32_t>(program_id);
        const bool valid = elf != nullptr && base != 0 && size > 0x197ba35 && base <= UINTPTR_MAX - size &&
            elf->MatchesFileSha256(ShellHash, 128ull * 1024 * 1024) &&
            CallSite(base, 0x6e690e, 0x197b1c0) && CallSite(base, 0x6e69ca, 0x197ba30) &&
            CallSite(base, 0x6f65ca, 0x197b3e0);
        buffer->pin_state.store(valid ? 1 : 3, std::memory_order_release);
    } catch (...) { buffer->pin_state.store(3, std::memory_order_release); }
}

void Unloading(int32_t program_id) noexcept {
    auto* buffer = g_buffer.load(std::memory_order_acquire);
    if (buffer != nullptr && buffer->pin_state.load(std::memory_order_acquire) == 1 &&
        buffer->program_id == static_cast<uint32_t>(program_id)) {
        buffer->pin_state.store(2, std::memory_order_release); // never rearm on reuse
    }
}

int __attribute__((noinline, force_align_arg_pointer)) KYTY_SYSV_ABI CreateEventObserved(
    EventFlag::KernelEventFlag* out, const char* name, uint32_t attr, uint64_t pattern, const void* opt) {
    if (!Enabled()) { return EventFlag::KernelCreateEventFlag(out, name, attr, pattern, opt); }
    const uint64_t caller = KYTY_SPP_CALLER;
    Call observation(1, caller, {Carrier(out), Carrier(name), attr, pattern, Carrier(opt), 0});
    const int result = EventFlag::KernelCreateEventFlag(out, name, attr, pattern, opt);
    observation.Finish(result);
    return result;
}
int __attribute__((noinline, force_align_arg_pointer)) KYTY_SYSV_ABI CreateSema(
    Semaphore::KernelSema* out, const char* name, uint32_t attr, int initial, int maximum, void* opt) {
    if (!Enabled()) { return Semaphore::KernelCreateSema(out, name, attr, initial, maximum, opt); }
    const uint64_t caller = KYTY_SPP_CALLER;
    Call observation(2, caller, {Carrier(out), Carrier(name), attr, static_cast<uint32_t>(initial),
                              static_cast<uint32_t>(maximum), Carrier(opt)});
    const int result = Semaphore::KernelCreateSema(out, name, attr, initial, maximum, opt);
    observation.Finish(result);
    return result;
}
int __attribute__((noinline, force_align_arg_pointer)) KYTY_SYSV_ABI PollSema(Semaphore::KernelSema handle, int need) {
    if (!Enabled()) { return Semaphore::KernelPollSema(handle, need); }
    const uint64_t caller = KYTY_SPP_CALLER;
    Call observation(3, caller, {Carrier(handle), static_cast<uint32_t>(need), 0, 0, 0, 0});
    const int result = Semaphore::KernelPollSema(handle, need);
    observation.Finish(result);
    return result;
}
static_assert(std::is_same_v<decltype(&CreateEventObserved), decltype(&EventFlag::KernelCreateEventFlag)>);
static_assert(std::is_same_v<decltype(&CreateSema), decltype(&Semaphore::KernelCreateSema)>);
static_assert(std::is_same_v<decltype(&PollSema), decltype(&Semaphore::KernelPollSema)>);
} // namespace Libs::LibKernel::SemaphoreProvenance
