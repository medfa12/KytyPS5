#pragma once

namespace Emulator { struct RunOptions; }
namespace Loader {
// Select the explicitly requested VSH backend or the existing standalone backend.
void ConfigureRuntimeLaunch(const Emulator::RunOptions& options);
void ConfigureVshRuntimeLaunch(const Emulator::RunOptions& options);
void ConfigureVshSubprocessLaunch(const Emulator::RunOptions& options);
// Returns false for ordinary session/standalone code; runtime app callers exit
// through the guest-thread trampoline instead of terminating the emulator.
// This requests owner admission stop, not peer preemption or completed exit.
bool ExitCurrentVshApplication(int code);
}
