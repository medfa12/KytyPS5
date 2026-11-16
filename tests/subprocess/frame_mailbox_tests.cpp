#include "loader/runtimeChildFrames.h"
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace Loader::RuntimeChild;
namespace { void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); } }
int main() try {
	const Key key {104,0,1,GetTickCount64(),GetCurrentProcessId()};
	auto reader = std::make_shared<FrameMailbox>(); FrameMailbox writer, duplicate, missing;
	Check(reader->Create(key) && writer.Open(key), "paired mapping");
	Check(!duplicate.Create(key), "cannot replace a live generation");
	auto wrong = key; ++wrong.sequence; Check(!missing.Open(wrong), "wrong generation cannot attach");
	std::vector<uint8_t> data(128 * 64 * 4, 17);
	Check(!writer.Publish(0,64,37,false,data) && !writer.Publish(128,64,999,false,data) &&
	    !writer.Publish(128,64,37,false,std::span(data).first(data.size()-1)), "invalid dimensions/format/length rejected");
	Check(reader->Revision() == 0,"invalid frames never publish readiness");
	std::atomic_bool done {false}; std::atomic_uint failures {0};
	std::jthread producer([&] {
		for (unsigned n = 1; n <= 2000; ++n) {
			std::fill(data.begin(),data.end(),static_cast<uint8_t>(n));
			if (!writer.Publish(128,64,37,false,data)) ++failures;
		}
		done.store(true);
	});
	FramePixels frame; uint64_t previous = 0; unsigned reads = 0;
	while (!done.load() || reader->Revision() > previous) {
		if (!reader->Read(&frame)) { std::this_thread::yield(); continue; }
		Check(frame.key == key && frame.serial > previous,"monotonic generation-tagged frames");
		Check(std::all_of(frame.pixels.begin(),frame.pixels.end(),[&](auto byte) { return byte == uint8_t(frame.serial); }),"no torn producer/consumer pixels");
		previous = frame.serial; ++reads;
	}
	producer.join(); Check(!failures.load() && reads && previous == 2000,"latest frame survives concurrent replacement");
	SetFrameSource(reader,key,true); const auto revision = FrameSourceRevision();
	SetShellUiFrameStatus(0,6); Check(!GetFrameSource().visible && FrameSourceRevision() != revision,"native Home hides retained app");
	SetShellUiFrameStatus(2,2); Check(GetFrameSource().visible,"masked resume restores app");
	SetShellUiFrameStatus(0,0x20); Check(GetFrameSource().visible,"controller status cannot hide translucent background");
	SetFrameVisibility(wrong,false); RetireFrameSource(wrong); Check(GetFrameSource().visible,"stale generation cannot retire current source");
	RetireFrameSource(key); Check(!GetFrameSource().mailbox,"actual retirement clears source");
	std::cout << "frame mailbox concurrent ownership/validation/native visibility passed\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
