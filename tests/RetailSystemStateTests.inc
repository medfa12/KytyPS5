// Exercises the production export and real named kernel event-flag object.
namespace Libs::LibcInternal {
int KYTY_SYSV_ABI SceSystemStateMgrGetCurrentState();
}

void TestRetailSystemState() {
  using namespace Libs::LibKernel::EventFlag;
  KernelEventFlag flag = nullptr;
  Check(KernelCreateEventFlag(&flag, "SceSystemStateMgrInfo", 0x121, 0, nullptr) == 0,
        "create genuine state flag");
  std::atomic<bool> returned {false};
  std::atomic<int> observed {0};
  int32_t canary = 0x12345678;
  std::thread reader([&] {
    using WithIgnoredArgument = int(KYTY_SYSV_ABI *)(int32_t*);
    auto call = reinterpret_cast<WithIgnoredArgument>(&Libs::LibcInternal::SceSystemStateMgrGetCurrentState);
    observed = call(&canary); // Native no-argument function must ignore stale RDI.
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Check(!returned.load(), "zero state waits rather than fabricating readiness");
  Check(KernelSetEventFlag(flag, 1000) == 0, "publish real working state");
  reader.join();
  Check(observed == 1000 && canary == 0x12345678, "state returned without writing stale RDI");
  Check(KernelClearEventFlag(flag, 0) == 0 && KernelSetEventFlag(flag, 500) == 0,
        "publish changed standby state");
  Check(Libs::LibcInternal::SceSystemStateMgrGetCurrentState() == 500,
        "subsequent query follows changed state, not constant working");
  Check(KernelDeleteEventFlag(flag) == 0, "delete state flag");
}
