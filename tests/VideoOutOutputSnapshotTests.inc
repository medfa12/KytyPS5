// CPU-only tests of the exact tracker and composition used by Swapchain and
// VideoOutDriver. No window, GPU, screenshot or guest execution is required.
#include <atomic>
#include <thread>

namespace {
void SnapshotCheck(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "VideoOutOutputSnapshotTests: %s\n", message);
		std::abort();
	}
}

void TestOutputSnapshots() {
	using namespace Libs::Graphics;
	using namespace Libs::VideoOut;
	PresentationOutputState host;
	VideoOutEndpointOutputState endpoint {1920, 1080, 1};
	VideoOutPortOutputState ports[] = {
	    {false, 256}, {false, 0}, {false, 1}, {false, 2}, {false, 32}};
	auto snapshot = [&](bool initialized = true, bool detect_resolution = true) {
		const auto result = ComposeVideoOutOutputSnapshot(initialized, host.Snapshot(), endpoint, ports,
		                                                   std::size(ports), detect_resolution, 60);
		SnapshotCheck(result.flags == 0, "no HDCP capability in any lifecycle state");
		return result;
	};
	auto output = snapshot(false);
	SnapshotCheck(!output.initialized && !output.connected && !output.usable && !output.port_open,
	              "uninitialized driver has no guest output");
	ports[1].opened = true;
	output = snapshot();
	SnapshotCheck(output.initialized && output.port_open && !output.connected && !output.usable,
	              "opened handle alone is not a connected or usable output");
	host.BeginCreate();
	output = snapshot();
	SnapshotCheck(output.connected && !output.presentation.initialized && !output.usable,
	              "real surface alone is not usable before resources exist");
	host.Created(1280, 720, false, false);
	output = snapshot();
	SnapshotCheck(output.usable && output.bus == 0 && output.guest_width == 1920 &&
	                  output.guest_height == 1080 && output.presentation.drawable_width == 1280 &&
	                  output.presentation.drawable_height == 720 && output.resolution == 1 &&
	                  output.dynamic_range == 1 && output.refresh_rate == 3,
	              "1280x720 host does not change the internal 1920x1080 guest output");
	SnapshotCheck(snapshot(true, false).resolution == 2,
	              "existing guest resolution-detection policy preserved");
	SnapshotCheck(!snapshot(false).usable && !snapshot(false).connected,
	              "ready presenter cannot bypass driver initialization");
	ports[1].opened = false;
	output = snapshot();
	SnapshotCheck(output.connected && output.usable && !output.port_open && output.bus == -1 &&
	                  output.guest_width == 1920 && output.guest_height == 1080 && output.output_mode == 1,
	              "real ready endpoint with zero source ports admits the first frame");
	output = ComposeVideoOutOutputSnapshot(true, host.Snapshot(), endpoint, nullptr, 0, true, 60);
	SnapshotCheck(output.usable && !output.port_open && output.bus == -1 && output.resolution == 1 &&
	                  output.refresh_rate == 3 && output.flags == 0,
	              "no source array is needed to describe the physical endpoint");
	ports[2].opened = true;
	output = snapshot();
	SnapshotCheck(output.usable && !output.port_open && output.bus == -1 &&
	                  output.guest_width == 1920 && output.refresh_rate == 3,
	              "Apple OVERLAY-only source neither creates nor gates output readiness");
	ports[2].opened = false;
	ports[3].opened = ports[4].opened = true;
	SnapshotCheck(snapshot().usable && !snapshot().port_open,
	              "SUB and HMD source bookkeeping does not change a ready MAIN endpoint");
	ports[3].opened = ports[4].opened = false;

	ports[0].opened = true;
	endpoint.SetOutputMode(256, 15);
	output = snapshot();
	SnapshotCheck(output.usable && output.bus == 256 && output.guest_width == 1920 &&
	                  output.refresh_rate == 13,
	              "AGC configuration changes the shared endpoint's mode");
	ports[1].opened = true;
	SnapshotCheck(snapshot().bus == 0 && snapshot().refresh_rate == 13,
	              "opening MAIN selects source diagnostics without resetting the physical mode");
	ports[1].opened = false;
	SnapshotCheck(snapshot().bus == 256, "closing MAIN falls back to live MAIN_SYSTEM");
	ports[0].opened = false;
	output = snapshot();
	SnapshotCheck(output.connected && output.usable && !output.port_open && output.bus == -1 &&
	                  output.output_mode == 15 && output.refresh_rate == 13,
	              "closing the last source retains physical readiness and configured mode");
	ports[1].opened = true;
	SnapshotCheck(snapshot().output_mode == 15, "reopening a source does not reset endpoint mode");
	endpoint.SetOutputMode(0, 1);
	SnapshotCheck(snapshot().refresh_rate == 3, "MAIN can explicitly restore the default mode");
	endpoint.SetOutputMode(1, 15);
	SnapshotCheck(snapshot().refresh_rate == 13, "OVERLAY configuration also targets physical MAIN");
	endpoint.SetOutputMode(2, 1);
	endpoint.SetOutputMode(32, 0x2000c);
	SnapshotCheck(snapshot().output_mode == 15, "SUB/HMD configuration cannot replace MAIN mode");
	endpoint.SetOutputMode(1, 1);
	output = ComposeVideoOutOutputSnapshot(true, host.Snapshot(), endpoint, nullptr, 0, true, 120);
	SnapshotCheck(output.refresh_rate == 13, "configured high vblank frequency remains reflected");

	endpoint.width = 0;
	SnapshotCheck(snapshot().connected && !snapshot().usable && snapshot().resolution == 0 &&
	                  snapshot().refresh_rate == 0,
	              "ready resources and an open source cannot bypass zero endpoint width");
	endpoint.width = 1920;
	endpoint.height = 0;
	SnapshotCheck(!snapshot().usable, "zero endpoint height is unavailable");
	endpoint.height = 1080;
	endpoint.output_mode = 0;
	SnapshotCheck(!snapshot().usable && snapshot().resolution == 0,
	              "missing configured endpoint mode is unavailable");
	endpoint.SetOutputMode(0, 1);
	endpoint.width = 3840;
	endpoint.height = 2160;
	SnapshotCheck(snapshot().usable && snapshot().resolution == 2,
	              "configured guest 4K mode is independent of source and host extents");
	endpoint.width = 1920;
	endpoint.height = 1080;
	host.BeginCreate();
	host.Created(640, 360, false, false);
	output = snapshot();
	SnapshotCheck(output.usable && output.presentation.drawable_width == 640 &&
	                  output.guest_width == 1920 && output.guest_height == 1080 && output.resolution == 1,
	              "resizing the host drawable cannot resize the configured guest endpoint");
	auto unavailable = host.Snapshot();
	unavailable.presentable = false;
	unavailable.availability = PresentationAvailability::ObserverUnavailable;
	output = ComposeVideoOutOutputSnapshot(true, unavailable, endpoint, nullptr, 0, true, 60);
	SnapshotCheck(output.connected && !output.usable, "missing lifecycle observer remains fail-closed");
	unavailable = host.Snapshot();
	unavailable.connected = false;
	output = ComposeVideoOutOutputSnapshot(true, unavailable, endpoint, ports, std::size(ports), true, 60);
	SnapshotCheck(!output.connected && !output.usable, "an open source cannot bypass disconnected output");
	host.SetMinimized(true);
	output = snapshot();
	SnapshotCheck(output.connected && output.port_open && output.presentation.initialized &&
	                  output.presentation.minimized && !output.usable &&
	                  output.presentation.availability == PresentationAvailability::NoDrawable,
	              "minimize suspends presentation without unplugging the guest output");
	host.Presented();
	SnapshotCheck(!snapshot().usable, "a late success cannot override a minimized host");
	host.SetMinimized(false);
	SnapshotCheck(!snapshot().usable, "restore event alone is not successful recovery");
	host.Presented();
	SnapshotCheck(snapshot().usable, "existing retained-frame present validates restoration");
	ports[1].opened = false;

	host.Failed(true, false);
	SnapshotCheck(snapshot().connected && !snapshot().usable &&
	                  snapshot().presentation.availability == PresentationAvailability::AcquireFailed,
	              "acquire failure revokes usability immediately");
	host.BeginRecovery(false);
	host.ResourcesDestroyed();
	host.BeginCreate();
	host.CreationUnavailable(true);
	output = snapshot();
	SnapshotCheck(output.connected && !output.port_open && !output.presentation.initialized &&
	                  !output.usable && output.presentation.drawable_width == 0 &&
	                  output.guest_width == 1920 && output.guest_height == 1080,
	              "zero-extent minimized surface has no swapchain, guest mode remains unchanged");
	host.Presented();
	SnapshotCheck(!snapshot().usable, "no fake success without initialized resources");
	host.BeginCreate();
	host.Created(1280, 0, false, false);
	SnapshotCheck(!snapshot().usable && !snapshot().presentation.initialized,
	              "zero-height resource initialization remains unavailable");
	host.BeginCreate();
	host.Created(1280, 720, true, false);
	SnapshotCheck(!snapshot().usable && snapshot().presentation.initialized,
	              "recreated resources alone do not conceal the previous acquire failure");
	host.Presented();
	SnapshotCheck(snapshot().usable && snapshot().dynamic_range == 1,
	              "successful recovery; HDR support alone does not advertise active HDR");
	host.Failed(false, false);
	SnapshotCheck(snapshot().connected && !snapshot().usable &&
	                  snapshot().presentation.availability == PresentationAvailability::PresentFailed,
	              "present failure is independent of an initialized driver/port");
	host.BeginRecovery(false);
	host.BeginCreate();
	host.CreationUnavailable(false);
	SnapshotCheck(!snapshot().usable && snapshot().connected &&
	                  snapshot().presentation.availability == PresentationAvailability::CreateFailed,
	              "resource acquisition failure remains visible and connected");
	host.BeginCreate();
	host.Created(1280, 720, true, true);
	SnapshotCheck(!snapshot().usable, "repeated creation after failure still needs a good present");
	host.Presented();
	SnapshotCheck(snapshot().usable && snapshot().dynamic_range == 2,
	              "active HDR output is recorded only after actual swapchain selection");
	host.Failed(false, true);
	SnapshotCheck(snapshot().connected && !snapshot().usable &&
	                  snapshot().presentation.availability == PresentationAvailability::SurfaceLost,
	              "lost Vulkan surface suspends presentation, not the attached emulated monitor");
	host.BeginRecovery(true);
	host.BeginCreate();
	SnapshotCheck(snapshot().connected && !snapshot().usable,
	              "recreated surface alone does not restore usability");
	host.Created(1280, 720, false, false);
	host.Presented();
	const auto generation = snapshot().presentation.generation;
	SnapshotCheck(snapshot().usable && generation > 1, "recovered generation is observable");
	host.Shutdown();
	output = snapshot();
	SnapshotCheck(!output.connected && !output.usable && !output.presentation.initialized &&
	                  output.presentation.generation == generation,
	              "presenter teardown clears resources and connection");
	output = ComposeVideoOutOutputSnapshot(false, {}, endpoint, nullptr, 0, true, 60);
	SnapshotCheck(!output.initialized && !output.port_open && !output.connected && !output.usable,
	              "retired driver snapshot is safely empty");
}

void TestOutputSnapshotConcurrency() {
	using namespace Libs::Graphics;
	using namespace Libs::VideoOut;
	PresentationOutputState host;
	const VideoOutEndpointOutputState endpoint {1920, 1080, 1};
	std::atomic<bool> done {false};
	std::thread writer([&] {
		for (unsigned i = 0; i < 1000; ++i) {
			host.BeginCreate();
			host.Created(1280, 720, false, false);
			host.Failed(true, false);
			host.BeginRecovery(false);
			host.BeginCreate();
			host.Created(1920, 1080, true, true);
			host.Presented();
			host.SetMinimized(true);
			host.SetMinimized(false);
			host.Presented();
			host.Shutdown();
		}
		done.store(true, std::memory_order_release);
	});
	uint64_t generation = 0;
	do {
		const auto state = host.Snapshot();
		SnapshotCheck(!state.presentable || (state.connected && state.initialized && !state.minimized &&
		                                        state.drawable_width != 0 && state.drawable_height != 0),
		              "query cannot tear connection/resource/presentation fields");
		SnapshotCheck(!state.initialized ||
		                  ((state.drawable_width == 1280 && state.drawable_height == 720) ||
		                   (state.drawable_width == 1920 && state.drawable_height == 1080)),
		              "query cannot tear a resource extent");
		SnapshotCheck(state.generation >= generation, "resource generation cannot go backwards");
		const auto output = ComposeVideoOutOutputSnapshot(true, state, endpoint, nullptr, 0, true, 60);
		SnapshotCheck(output.usable == (state.connected && state.initialized && state.presentable) &&
		                  !output.port_open && output.bus == -1 && output.flags == 0 &&
		                  output.guest_width == 1920 && output.guest_height == 1080,
		              "no-source composition follows the exact observed lifecycle without tearing guest mode");
		generation = state.generation;
	} while (!done.load(std::memory_order_acquire));
	writer.join();
}
} // namespace
