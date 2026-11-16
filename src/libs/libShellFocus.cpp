// ps5-vsh ShellFocus: host-callable PS-button pulse + shell-observable focus intent.
// Additive only: touches no existing pad/focus path (PadReadState, PadRead,
// ControllerButtonToPadButton, hostInput mappings all unchanged).
//
// Current host input mapping:
// - PadData.buttons has NO PS bit: platform enum is L3=0x2..SQUARE=0x8000,
//   TOUCH_PAD=0x100000, INTERCEPTED=0x80000000. An earlier
//   draft used bit 0x10000 (Vita analogy); that bit is unassigned on PS5.
// - The host GUIDE button (SDL_GAMEPAD_BUTTON_GUIDE) is dropped to 0 by
//   ControllerButtonToPadButton, so a physical PS press never reaches pad
//   state. PulsePsButton() therefore cannot use a pad bit: it bumps the
//   intent generation counter and calls
//   Libs::JscdNet::ShellCoreUtilHost::OnHostPsButtonPressed(), which sets
//   the guest-visible ShellCoreUtil user focus (Config::GetUserId()) and
//   UI status (VSH_READY set, PS_BUTTON_LOCKED cleared). window.cpp and
//   hostInput.cpp route GUIDE here instead of a pad bit.
// - pad_fill_data() copies state.buttons verbatim, so all mapped buttons
//   reach PadReadState/PadRead guests unchanged.
// - 3 most relevant Focus NIDs from _ProbeLogs/nids.csv (nid,hex,name,library):
//   -9djWj1NU4E,0xFFD7635A3D4D5381,sceShellCoreUtilSetUserFocus,,0,aerolib;ps5rs;ps5rs-cat
//     -> write path: sets which user focus the shell treats as foreground.
//   E4I7uCoWbkM,0x13823BB82A166E43,sceShellCoreUtilGetUserFocus,,0,aerolib;ps5rs;ps5rs-cat
//     -> read path: returns the current user-focus value the shell observes.
//   fORZmlh1TQo,0x7CE4599A58754D0A,sceShellCoreUtilGetUIStatus,,0,aerolib;ps5rs;ps5rs-cat
//     -> reads shell UI status (idle vs overlay e.g. Control Center; write side
//        is sibling atiUTsTFJ3k,...,sceShellCoreUtilSetUIStatus).
//   Owners in src/libs: ShellUtilSys in src/libs/libJscdNet.cpp owns
//   fORZmlh1TQo (GetUIStatus), atiUTsTFJ3k (SetUIStatus), E4I7uCoWbkM
//   (GetUserFocus) and -9djWj1NU4E (SetUserFocus) with shared state;
//   signatures from Sce.Vsh.ShellCoreUtilWrapper (see libJscdNet.cpp).
//   No Focus NID is registered here; this file's guest wiring stays LOCAL_.

#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/controller.h"
#include "libs/libShellFocus.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <thread>

namespace Libs {
namespace ShellFocus {

LIB_VERSION("ShellFocus", 1, "ShellFocus", 1, 1);

// ps5-vsh: the host mapping exposes no PS-button bit in PadData.
// GUIDE input is handled separately from ordinary controller state,
// so pulsing a pad bit would fake input the shell cannot interpret as PS.
// PulsePsButton therefore leaves pad state untouched: it records intent
// (generation counter + log) and drives the ShellCoreUtil state in
// libJscdNet.cpp. Local to this file on purpose:
// controller.h is intentionally untouched (additive-only scope).
constexpr uint32_t kPadButtonPsUnused = 0x00010000u; // NOT a real PS bit

namespace {

// Shell-observable focus intent: bumped on every pulse; future guest focus
// stubs (LOCAL_ below) expose it without knowing ShellCoreUtil layouts.
std::atomic<uint64_t> g_focus_intent_gen {0};
std::atomic<uint32_t> g_last_pulse_ms {0};

// Upper bound so a host caller can never wedge its thread in sleep.
constexpr uint32_t kMaxPulseMs = 5000;

} // namespace

namespace ShellFocusHost {

// Host API (non-static; host callers extern-declare
// Libs::ShellFocus::ShellFocusHost::PulsePsButton).
// Records PS-button focus intent (generation bump + duration) and applies
// the press to guest-visible ShellCoreUtil state. Intentionally pad-free:
// the Pad reference defines no PS bit, so there is no correct bit to set;
// the shell-observable part is the ShellCoreUtil transition below.
void PulsePsButton(uint32_t ms) {
	if (ms > kMaxPulseMs) {
		ms = kMaxPulseMs;
	}
	// ps5-vsh: no pad bit is set, because no PadData bit means PS. The
	// generation bump is host-side sequencing; the guest-visible part is
	// the ShellCoreUtil user-focus/UI-status unlock in libJscdNet.cpp
	// (transitions are logged there by ApplyUiStatus/StoreUserFocus).
	(void)kPadButtonPsUnused;
	g_focus_intent_gen.fetch_add(1, std::memory_order_relaxed);
	g_last_pulse_ms.store(ms, std::memory_order_relaxed);
	LOGF("\t shell-focus PS pulse intent ms = %u (no pad bit: PS is system-level)\n",
	     static_cast<unsigned>(ms));
	JscdNet::ShellCoreUtilHost::OnHostPsButtonPressed();
}

uint64_t FocusIntentGeneration() {
	return g_focus_intent_gen.load(std::memory_order_relaxed);
}

} // namespace ShellFocusHost

// Guest-facing stubs for future wiring. Placeholder LOCAL_ keys only: the real
// Focus NIDs above are owned by ShellUtilSys in libJscdNet.cpp, so nothing
// guest-real is claimed here. All KYTY_SYSV_ABI, PRINT_NAME+LOGF, null-safe
// (scalar args/returns: no guest pointers involved).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellFocusPulse(uint32_t ms) {
	PRINT_NAME();
	if (ms > kMaxPulseMs) {
		ms = kMaxPulseMs;
	}
	LOGF("\t ms = %u\n", static_cast<unsigned>(ms));
	// ps5-vsh: intent-only (no pad bit exists for PS): record generation +
	// duration, never touch pad state. No sleep on the guest thread.
	g_focus_intent_gen.fetch_add(1, std::memory_order_relaxed);
	g_last_pulse_ms.store(ms, std::memory_order_relaxed);
	(void)kPadButtonPsUnused;
	return 0;
}

static uint64_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellFocusIntent() {
	PRINT_NAME();
	const uint64_t gen = g_focus_intent_gen.load(std::memory_order_relaxed);
	LOGF("\t gen = %" PRIu64 "\n", gen);
	return gen;
}

static uint32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ShellFocusLastPulse() {
	PRINT_NAME();
	const uint32_t ms = g_last_pulse_ms.load(std::memory_order_relaxed);
	LOGF("\t ms = %u\n", static_cast<unsigned>(ms));
	return ms;
}

LIB_DEFINE(InitShellFocus_1) {
	LIB_FUNC("LOCAL_ShellFocusPulse", ShellFocusPulse);       // LOCAL
	LIB_FUNC("LOCAL_ShellFocusIntent", ShellFocusIntent);     // LOCAL
	LIB_FUNC("LOCAL_ShellFocusLastPulse", ShellFocusLastPulse); // LOCAL
}

} // namespace ShellFocus
} // namespace Libs

// Orchestrator registration snippet for src/libs/libs.cpp (add exactly this):
//
//   namespace ShellFocus {
//   LIB_DEFINE(InitShellFocus_1);
//   } // namespace ShellFocus
//
// ... and inside InitAll(Loader::SymbolDatabase* s), alongside the other
// ps5-vsh entries:
//
//   ShellFocus::InitShellFocus_1(s);
