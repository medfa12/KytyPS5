#include "graphics/presentation/flipQueueSelection.h"
#include "graphics/presentation/scanoutLayers.h"

#include <gtest/gtest.h>
#include <vector>

namespace {
using namespace Libs::Graphics;
struct Frame {
	bool blank = false;
	bool hdr = false;
};

TEST(ScanoutLayers, SystemOnlyCanReplayAndSelectHdr) {
	ScanoutLayers<Frame> layers;
	Frame system {false, true};
	EXPECT_FALSE(layers.HasFrames());
	EXPECT_EQ(layers.Replace({&system, 256, true}), nullptr);
	EXPECT_TRUE(layers.HasFrames());
	EXPECT_EQ(layers.Ordered()[0].frame, nullptr);
	EXPECT_EQ(layers.Ordered()[2].frame, &system);
	EXPECT_EQ(layers.ColorSource(), &system);
	EXPECT_EQ(layers.Clear(0), nullptr);
	EXPECT_TRUE(layers.HasFrames());
	EXPECT_EQ(layers.Clear(256), &system);
	EXPECT_FALSE(layers.HasFrames());
}

TEST(ScanoutLayers, IndependentReplacementCloseAndCompositionOrder) {
	ScanoutLayers<Frame> layers;
	Frame main, overlay, system, replacement;
	EXPECT_EQ(layers.Replace({&system, 256, true}), nullptr);
	EXPECT_EQ(layers.Replace({&overlay, 1, false}), nullptr);
	EXPECT_EQ(layers.Replace({&main, 0, false}), nullptr);
	EXPECT_EQ(layers.Ordered()[0].frame, &main);
	EXPECT_EQ(layers.Ordered()[1].frame, &overlay);
	EXPECT_EQ(layers.Ordered()[2].frame, &system);
	EXPECT_TRUE(layers.Ordered()[2].premultiplied_alpha);
	EXPECT_EQ(layers.Replace({&replacement, 256, false}), &system);
	EXPECT_FALSE(layers.Ordered()[2].premultiplied_alpha);
	EXPECT_EQ(layers.ColorSource(), &main);
	EXPECT_EQ(layers.Clear(256), &replacement);
	EXPECT_EQ(layers.Clear(256), nullptr);
	EXPECT_EQ(layers.Ordered()[0].frame, &main);
	EXPECT_EQ(layers.Ordered()[1].frame, &overlay);
	EXPECT_EQ(layers.Replace({&system, 256, true}), nullptr);
	EXPECT_EQ(layers.Clear(0), &main);
	EXPECT_EQ(layers.ColorSource(), &system);
}

TEST(ScanoutLayers, SpecialFlipsRetainOwnershipAndColorSpace) {
	ScanoutLayers<Frame> layers;
	Frame system {false, true}, blank {true, false}, black {true, false};
	EXPECT_EQ(layers.Replace({&system, 256, true}), nullptr);
	EXPECT_EQ(layers.Replace({&blank, 256, false}), &system);
	EXPECT_TRUE(layers.HasFrames());
	EXPECT_EQ(layers.ColorSource(), nullptr);
	EXPECT_EQ(layers.Replace({&black, 256, false}), &blank);
	EXPECT_EQ(layers.Clear(256), &black);
	EXPECT_EQ(layers.Replace({&system, 256, true}), nullptr);
	EXPECT_EQ(layers.Replace({&black, 0, false}), nullptr);
	EXPECT_EQ(layers.ColorSource(), nullptr);
}

TEST(ScanoutLayers, HeadsetMirrorCloseDoesNotClearAnotherOwner) {
	ScanoutLayers<Frame> layers;
	Frame main, headset, system, sub;
	EXPECT_EQ(layers.Replace({&main, 0, false}), nullptr);
	EXPECT_EQ(layers.Replace({&system, 256, true}), nullptr);
	EXPECT_EQ(layers.Clear(32), nullptr);
	EXPECT_EQ(layers.Replace({&headset, 32, false}), &main);
	EXPECT_EQ(layers.Clear(0), nullptr);
	EXPECT_EQ(layers.Ordered()[0].frame, &headset);
	EXPECT_EQ(layers.Clear(32), &headset);
	EXPECT_EQ(layers.Ordered()[2].frame, &system);
	// SUB is not plane index 2, and cannot overwrite or close AGC.
	EXPECT_EQ(layers.Replace({&sub, 2, false}), &sub);
	EXPECT_EQ(layers.Clear(2), nullptr);
	EXPECT_EQ(layers.Ordered()[2].frame, &system);
}

struct Request {
	int id, group, cfg;
	bool ready;
};
bool Ready(const Request& owner, const std::vector<Request>& pending,
           const std::vector<Request>& cpu = {}) {
	return IsFlipGroupReady(owner, pending, cpu, [](const auto& r) { return r.ready; });
}

TEST(ScanoutQueue, IndependentPortsCanShareVblankWithoutHeadOfLineBlocking) {
	std::vector<Request> pending {{1, 1, 0, false}, {2, 2, 256, true}, {3, 3, 256, true}};
	EXPECT_FALSE(Ready(pending[0], pending));
	EXPECT_TRUE(Ready(pending[1], pending));
	EXPECT_FALSE(Ready(pending[2], pending));
	pending[0].ready = true;
	EXPECT_TRUE(Ready(pending[0], pending));
	EXPECT_TRUE(Ready(pending[1], pending));
	EXPECT_FALSE(Ready(pending[2], pending));
}

TEST(ScanoutQueue, MasterSlaveCompletionAndCpuReservationsStayAtomic) {
	std::vector<Request> pending {{2, 2, 0, true}, {3, 3, 256, true}};
	std::vector<Request> cpu {{1, 2, 1, false}};
	EXPECT_FALSE(Ready(pending[0], pending, cpu));
	EXPECT_TRUE(Ready(pending[1], pending, cpu));
	pending.push_back(cpu[0]);
	cpu.clear();
	EXPECT_FALSE(Ready(pending[0], pending));
	pending[2].ready = true;
	EXPECT_TRUE(Ready(pending[0], pending));
	EXPECT_FALSE(Ready(pending[2], pending));
	cpu.push_back({0, 4, 0, false});
	EXPECT_FALSE(Ready(pending[0], pending, cpu));
	EXPECT_TRUE(Ready(pending[1], pending, cpu));
}
} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
