#include "loader/runtimeChildProcess.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace Loader::RuntimeChild;
static void Check(bool value, const char* why) { if (!value) { throw std::runtime_error(why); } }
template<class F> static void Until(F f) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!f()) { Check(std::chrono::steady_clock::now() < end, "bounded input mailbox wait"); Sleep(1); }
}
int main(int argc, char** argv) try {
	Check(argc == 2, "input helper path");
	Key key {106,0,1,0x99887766,0x11223344};
	InputMailbox mailbox; Check(mailbox.Create(key), "create isolated input mapping");
	for (unsigned i = 0; i != 100; ++i) { PadReport r; r.time = i; r.epoch = 7; Check(mailbox.Push(r), "nonblocking bounded ring push"); }
	PadReport raw; unsigned read = 0;
	while (mailbox.Pop(&raw)) { Check(raw.time == 36 + read && raw.serial == 37 + read++, "oldest overflow discarded with ordered serials"); }
	Check(read == 64, "exact bounded history");
	PadFocusFilter filter; PadReport held; held.buttons = 0x4000; held.axes[0] = 255; held.axes[4] = 255; held.touch[0].down = 1;
	filter.Focus(true,7,held); raw = held; raw.connected = 1; raw.epoch = 7; raw.serial = 1; raw.buttons |= 0x10000;
	PadReport result; Check(filter.Apply(raw,&result) && !result.buttons && result.axes[0] == 128 && !result.axes[4] && !result.touch[0].down, "held/Guide/axes/touch suppressed at focus entry");
	Check(!filter.Apply(raw,&result), "duplicate report rejected");
	raw.epoch = 6; raw.serial = 2; Check(!filter.Apply(raw,&result), "stale epoch rejected"); raw.epoch = 7;
	raw.buttons = 0; raw.axes = {128,128,128,128,0,0}; raw.touch = {}; Check(filter.Apply(raw,&result), "release re-arms controls");
	raw.serial = 3; raw.buttons = 0x4000; raw.axes[0] = 255; raw.axes[4] = 200; raw.touch[0].down = 1;
	Check(filter.Apply(raw,&result) && result.buttons == 0x4000 && result.axes[0] == 255 && result.axes[4] == 200 && result.touch[0].down, "fresh press after release admitted");
	raw.connected = 0; raw.serial = 4; Check(filter.Apply(raw,&result) && !result.connected && !result.buttons && result.axes[0] == 128, "neutral disconnect");
	filter.Focus(false,8,raw); raw.connected = 1; raw.epoch = 8; raw.serial = 5;
	Check(filter.Apply(raw,&result) && !result.buttons && result.axes[0] == 128, "background ordinary input stays neutral");
	filter.Focus(true,9,PadReport {}); raw.epoch = 9; raw.serial = 6; raw.eligible = 0;
	Check(filter.Apply(raw,&result) && !result.buttons, "OS background input is suppressed");
	raw.eligible = 1; raw.serial = 7;
	Check(filter.Apply(raw,&result) && !result.buttons && result.axes[0] == 128, "held input is not replayed after OS foreground return");
	PadOutput output; output.epoch = 7; output.large_motor = 100;
	Check(mailbox.WriteOutput(output) && mailbox.ReadOutput(&output) && !mailbox.ReadOutput(&output), "bounded latest output consumed once");
	uint64_t ps = 0; Check(mailbox.PulsePs() && mailbox.PulsePs() && mailbox.ReadPs(&ps) && ps == 2 && !mailbox.ReadPs(&ps), "PS intent separate from ordinary reports");
	const auto root = std::filesystem::current_path() / ("input-fixture-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
	std::filesystem::create_directory(root);
	Spawn spawn; spawn.executable = std::filesystem::absolute(argv[1]); spawn.directory = root; spawn.log = root / "input.log"; spawn.key = key;
	spawn.arguments = {L"--probe", L"input"}; uint32_t error = 0; auto p = Process::Create(spawn,&error); Check(bool(p) && !error, "actual brokered child");
	Until([&] { p->Poll(); return p->Entered(); }); Check(p->Capabilities() & BrokeredInput, "real input capability");
	PadReport baseline; baseline.buttons = 0x2000; baseline.axes[2] = 55;
	Check(p->Focus(true,true,9,&baseline), "input focus command carries held baseline atomically"); Until([&] { p->Poll(); return p->FocusAcknowledged() == 9; });
	PadReport r; r.epoch = 9; r.time = 12345; r.connected = 1; r.connections = 2; r.buttons = 0x14000; r.axes = {40,210,50,200,77,88};
	r.touch[0] = {1000,500,3,1,0}; r.accel = {1,2,3}; r.gyro = {4,5,6}; r.orientation = {0.1f,0.2f,0.3f,0.9f};
	Check(p->SendPad(r), "full normalized sample to actual child"); PadOutput reply;
	Until([&] { p->Poll(); Check(!p->Retired(), "input probe stays alive"); return p->ReadPadOutput(&reply); });
	Check(reply.epoch == 9 && reply.large_motor == 77 && reply.small_motor == 88 && reply.red == 3, "child validated motion/touch/axes and routed output");
	Check(p->ReadPs(&ps), "child PS intent arrives at parent only");
	p->Close(false); Until([&] { return p->Poll().retired; }); Check(p->ExitCode() == 0, "input helper actual retirement");
	std::cout << "bounded pad transport/focus/PS/output tests passed\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
