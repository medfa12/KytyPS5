#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_FLIPQUEUESELECTION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_FLIPQUEUESELECTION_H_

#include <algorithm>

namespace Libs::Graphics {

// Select complete groups without imposing head-of-line blocking between ports.
// Earlier requests on the same port (including CPU reservations) retain FIFO.
template <typename Request, typename Queue, typename IsReady>
bool IsFlipGroupReady(const Request& owner, const Queue& pending, const Queue& cpu,
                      IsReady is_ready) {
	if (owner.group != owner.id) return false;
	const auto group = owner.id;
	if (std::any_of(cpu.begin(), cpu.end(),
	                [group](const auto& r) { return r.group == group; })) return false;
	for (const auto& request: pending) {
		if (request.group != group) continue;
		if (!is_ready(request)) return false;
		for (const auto* queue: {&pending, &cpu}) {
			if (std::any_of(queue->begin(), queue->end(), [&](const auto& earlier) {
				return earlier.cfg == request.cfg && earlier.group != group && earlier.id < request.id;
			})) return false;
		}
	}
	return true;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_FLIPQUEUESELECTION_H_
