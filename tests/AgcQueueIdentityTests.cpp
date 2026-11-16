#include "libs/agcQueueIdentity.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

using namespace Libs::Graphics::AgcQueue;

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "AgcQueueIdentityTests: failed: %s\n", message);
		std::abort();
	}
}

void TestCompositorQueueCarriesEvent8() {
	Check(IsGraphicsCreateQueueId(COMPOSITOR_SYSTEM_QUEUE_ID),
	      "compositor queue id 3 is not on the graphics creation path");
	Check(RouteForQueue(COMPOSITOR_SYSTEM_QUEUE_ID) == Route::Graphics,
	      "compositor queue id 3 is not routed to the graphics processor");
	Check(GraphicsEopEventIdForQueue(3) == 8, "compositor queue id 3 does not raise event 8");
}

void TestRegularGraphicsQueueKeepsEvent0() {
	// Zeroed (legacy) contexts and every unverified graphics id keep interrupt source 0.
	for (uint32_t id: {0u, 1u, 2u, 4u, 5u, 8u, 0x1fu}) {
		Check(RouteForQueue(id) == Route::Graphics, "graphics id not routed to graphics");
		Check(GraphicsEopEventIdForQueue(id) == 0, "unverified graphics id changed event id");
	}
}

void TestComputeRoutingUnchanged() {
	Check(!IsGraphicsCreateQueueId(0x20), "compute id 0x20 treated as graphics creation");
	Check(RouteForQueue(0x1f) == Route::Graphics, "0x1f left graphics routing");
	Check(RouteForQueue(0x20) == Route::Compute, "0x20 not routed to compute");
	Check(RouteForQueue(0x57) == Route::Compute, "0x57 not routed to compute");
	Check(RouteForQueue(0x58) == Route::Graphics, "0x58 routing changed from previous behavior");
}

void TestContextLayout() {
	Check(CONTEXT_QUEUE_ID_OFFSET == 4, "queue id must be read from context +4");
	Check(CONTEXT_HEADER_OFFSET == 0 && CONTEXT_HEADER_VALUE == 0x38,
	      "graphics context header differs from libSceAgcDriver");
}

} // namespace

int main() {
	TestCompositorQueueCarriesEvent8();
	TestRegularGraphicsQueueKeepsEvent0();
	TestComputeRoutingUnchanged();
	TestContextLayout();
	std::puts("AgcQueueIdentityTests: all cases passed");
	return 0;
}
