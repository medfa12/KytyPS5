#include "graphics/presentation/nativeVideo.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

using namespace Libs::Graphics;
static void Check(bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "NativeVideoTests: %s\n", message); std::abort(); }
}
static NativeVideoFrame Frame(uint64_t generation, int64_t pts) {
	NativeVideoFrame frame;
	frame.rgba = std::make_unique<uint8_t[]>(64);
	frame.bytes = 64; frame.width = 4; frame.height = 4; frame.stride = 16;
	frame.generation = generation; frame.pts = pts; frame.has_pts = true; frame.sequence = 7;
	return frame;
}
int main() {
	auto first = std::make_shared<NativeVideoStream>(100, 1, 2, 1);
	auto second = std::make_shared<NativeVideoStream>(101, 3, 4, 1);
	Check(SelectNativeVideoStream(first), "select owner/context");
	Check(!SelectNativeVideoStream(second), "second live owner cannot replace backdrop");
	Check(first->Publish(Frame(1, 150000)), "publish owned frame");
	auto pending = Frame(1, 183333);
	Check(!first->Publish(std::move(pending)) && pending.rgba, "bounded pending queue preserves rejected pixels");
	auto snapshot = ReadNativeVideoSnapshot();
	const auto unchanged_revision = NativeVideoRevision();
	Check(first->SetRect({}) && NativeVideoRevision() == unchanged_revision && first->IsCurrent(snapshot),
	      "identical Render geometry does not invalidate an in-flight upload");
	Check(snapshot.frame && snapshot.frame->pts == 150000 && !first->Presented().has_pts,
	      "acquiring/uploading frame cannot advance presented PTS");
	Check(first->Acknowledge(snapshot) && first->Presented().pts == 150000, "host presentation acknowledgment");
	Check(!first->Acknowledge(snapshot), "retained redraw is not a second frame acknowledgment");
	Check(first->Publish(std::move(pending)) && !pending.rgba, "acknowledgment frees one pending slot");
	Check(!first->Acknowledge(snapshot), "late acknowledgment cannot overwrite newer frame");
	snapshot = ReadNativeVideoSnapshot();
	const auto recorded_revision = NativeVideoRevision();
	Check(first->Flush(2), "flush establishes new generation");
	Check(!first->ClearAcknowledged(), "flush request is not renderer clear completion");
	AcknowledgeNativeVideoRevision(recorded_revision);
	Check(!first->ClearAcknowledged(), "old frame submitted after flush cannot complete its clear");
	AcknowledgeNativeVideoRevision(NativeVideoRevision());
	Check(first->ClearAcknowledged(), "subsequent presenter commit acknowledges flush boundary");
	Check(!first->IsCurrent(snapshot) && !first->Acknowledge(snapshot) && !first->Presented().has_pts,
	      "flush rejects outstanding upload and clears presented clock");
	Check(snapshot.frame->rgba != nullptr, "in-flight snapshot owns CPU pixels across flush");
	Check(!first->Publish(Frame(1, 200000)) && first->Publish(Frame(2, 0)), "generation rejects stale producer");
	snapshot = ReadNativeVideoSnapshot();
	Check(first->SetRect({.25f, 0, .5f, 1}) && !first->Acknowledge(snapshot), "geometry invalidates stale composition");
	Check(!first->SetRect({0, 0, std::numeric_limits<float>::quiet_NaN(), 1}), "reject NaN geometry");
	Check(!first->SetRect({.75f, 0, .5f, 1}), "reject unsupported out-of-bounds geometry");
	snapshot = ReadNativeVideoSnapshot();
	std::thread retire([first] { first->Retire(); });
	retire.join();
	Check(!first->ClearAcknowledged(), "retirement also requires a later renderer commit");
	Check(!first->IsCurrent(snapshot) && !first->Acknowledge(snapshot) && !first->Publish(Frame(2, 1)),
	      "retirement invalidates all old publications and acknowledgments");
	Check(SelectNativeVideoStream(second), "retired owner releases selection");
	AcknowledgeNativeVideoRevision(NativeVideoRevision());
	Check(first->ClearAcknowledged(), "replacement backdrop commit clears retired stream");
	second->Retire();
	const auto full = FitNativeVideo({}, 1920, 1080, {0, 0, 1280, 720});
	Check(full.x == 0 && full.y == 0 && full.width == 1280 && full.height == 720, "16:9 full viewport");
	const auto fit = FitNativeVideo({}, 640, 480, {0, 0, 1280, 720});
	Check(fit.x == 160 && fit.y == 0 && fit.width == 960 && fit.height == 720, "4:3 aspect fitting");
	const auto top = FitNativeVideo({0, 0, .5f, .5f}, 1920, 1080, {10, 30, 1280, 720});
	Check(top.x == 10 && top.y == 30 && top.width == 640 && top.height == 360, "top-left origin with viewport offset");
	const auto bottom = FitNativeVideo({.5f, .5f, .5f, .5f}, 1920, 1080, {10, 30, 1280, 720});
	Check(bottom.x == 650 && bottom.y == 390, "normalized bounds preserve vertical orientation");
	Check(FitNativeVideo({0, 0, 0, 1}, 16, 9, {0, 0, 1280, 720}).width == 0, "zero bounds hide video");
	std::puts("Native video ownership, flush, acknowledgment and geometry tests passed");
}
