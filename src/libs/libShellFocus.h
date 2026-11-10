#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBSHELLFOCUS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBSHELLFOCUS_H_

#include <cstdint>

namespace Libs::ShellFocus::ShellFocusHost {

// Host PS press (SDL GUIDE in window.cpp/hostInput.cpp). The Pad reference
// defines no PS bit in PadData (PS is system-intercepted), so no pad state
// is touched. The press bumps the focus-intent generation counter and, via
// Libs::JscdNet::ShellCoreUtilHost::OnHostPsButtonPressed below, drives the
// guest-visible ShellCoreUtil user-focus/UI-status state that the 13.60
// ShellUI polls (sceShellCoreUtilGetUIStatus fORZmlh1TQo).
void PulsePsButton(uint32_t ms);
// Focus-intent generation counter (bumps per pulse) for shell-observable
// sequencing without guest wiring.
uint64_t FocusIntentGeneration();

} // namespace Libs::ShellFocus::ShellFocusHost

namespace Libs::JscdNet::ShellCoreUtilHost {

// Bridge into the ShellCoreUtil HLE (src/libs/libJscdNet.cpp; ShellUtilSys
// owns the shared UI-status/user-focus state). Applies a host PS press to
// guest-visible state: user focus := Config::GetUserId(), UI status
// VSH_READY set / PS_BUTTON_LOCKED cleared (the unlocked end state of the
// real OpeningPsButtonLocker/UIStatusUpdater path). Called by
// ShellFocusHost::PulsePsButton on every GUIDE down-edge.
void OnHostPsButtonPressed();

} // namespace Libs::JscdNet::ShellCoreUtilHost

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBSHELLFOCUS_H_ */
