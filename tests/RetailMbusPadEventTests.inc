// Inspect the real MBUS producer's wire record, not a reimplemented encoder.
#include "libs/controller.h"
#include "loader/symbolDatabase.h"

namespace Libs {
void InitPad_1(Loader::SymbolDatabase*);
namespace ShellHits::MbusBoot11 {
void InitShellHitsMbusBoot11_1(Loader::SymbolDatabase*);
}
}
namespace Libs::LibcInternal {
int KYTY_SYSV_ABI SceMbusEventCreate(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
int KYTY_SYSV_ABI SceMbusEventReceive(uint64_t, void*, uint64_t, uint64_t);
}

std::array<uint8_t, 256> TestRetailMbusPadEvent() {
  using namespace Libs::LibcInternal;
  Loader::SymbolDatabase symbols;
  Libs::InitPad_1(&symbols);
  Libs::ShellHits::MbusBoot11::InitShellHitsMbusBoot11_1(&symbols);
  auto resolve = [&](const char* nid) {
    auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr, "production pad/MBUS export registered");
    return symbol->vaddr;
  };
  using Query = int(KYTY_SYSV_ABI *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  using Init = int(KYTY_SYSV_ABI *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  using Open = int(KYTY_SYSV_ABI *)(uint64_t, int, int);
  auto query = reinterpret_cast<Query>(resolve("KRL-S9qBqXw"));
  auto init = reinterpret_cast<Init>(resolve("edYHYROxzx4"));
  auto open = reinterpret_cast<Open>(resolve("d7bXuEBycDI"));
  std::array<uint8_t, 256> condition {}, record {};
  uint32_t total = 0, generation = 0;
  const bool no_foreground = Libs::NoForegroundUser();
  Libs::SetNoForegroundUser(true);
  Check(init(reinterpret_cast<uint64_t>(condition.data()), 256, 0, 0, 0, 0) == 0, "initialize real all-device condition");
  Check(query(reinterpret_cast<uint64_t>(condition.data()), 0, reinterpret_cast<uint64_t>(record.data()),
              1, reinterpret_cast<uint64_t>(&total), reinterpret_cast<uint64_t>(&generation)) == 1 && total == 1,
        "enumerate unassigned production pad");
  uint32_t transport, subtype;
  int32_t owner;
  std::memcpy(&transport, record.data() + 8, 4);
  std::memcpy(&subtype, record.data() + 12, 4);
  std::memcpy(&owner, record.data() + 200, 4);
  Check(transport == 0 && subtype == 22 && owner == -1,
        "retail Bond USB wire identity is MBUS22 with genuinely unassigned owner");
  // The fixture has not initialized controller input. Standard port reaches
  // its real lifecycle error; a VR-right port must fail identity validation.
  Check(open(2, 0, 0) == Libs::Controller::PAD_ERROR_NOT_INITIALIZED, "Bond standard port reaches controller lifecycle");
  Check(open(2, 4, 0) == static_cast<int>(0x80920001u), "VR-right port must not alias DualSense");
  Check(open(3, 0, 0) == static_cast<int>(0x80920003u), "unrelated device remains invalid");
  const int32_t vr_right = 24;
  std::memcpy(condition.data() + 12, &vr_right, 4);
  Check(query(reinterpret_cast<uint64_t>(condition.data()), 0, reinterpret_cast<uint64_t>(record.data()),
              1, reinterpret_cast<uint64_t>(&total), reinterpret_cast<uint64_t>(&generation)) == 0 && total == 0,
        "DualSense excluded from VR-right enumeration");
  Libs::SetNoForegroundUser(no_foreground);
  const int handle = SceMbusEventCreate(0x10001, 0, 0, 0, 0, 0);
  Check(handle > 0, "create MBUS subscriber");
  std::array<uint8_t, 80> event {};
  auto get32 = [&](size_t offset) { uint32_t v; std::memcpy(&v, event.data() + offset, sizeof(v)); return v; };
  auto get64 = [&](size_t offset) { uint64_t v; std::memcpy(&v, event.data() + offset, sizeof(v)); return v; };
  Check(SceMbusEventReceive(handle, event.data(), 1, 0) == 1, "receive queued device-added event");
  Check(get32(0) == 1 && get64(8) == Libs::Mbus::kVirtualPadDeviceId &&
        get32(0x10) == uint32_t(Libs::Mbus::kVirtualPadTransport) &&
        get32(0x14) == uint32_t(Libs::Mbus::kVirtualPadDeviceType),
        "added event device, transport and subtype match enumeration");
  Check(SceMbusEventReceive(handle, event.data(), 1, 0) == 1, "receive queued owner event");
  Check(get32(0) == 0x10 && get64(8) == Libs::Mbus::kVirtualPadDeviceId &&
        get32(0x10) == uint32_t(Libs::Mbus::kVirtualPadTransport) &&
        get32(0x14) == uint32_t(Libs::Mbus::kVirtualPadDeviceType) &&
        get32(0x1c) == uint32_t(Libs::Mbus::VirtualPadOwnerUserId()),
        "owner event retains existing device and actual owner");
  return record;
}
