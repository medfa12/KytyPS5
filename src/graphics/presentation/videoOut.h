#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_

#include "common/common.h"
// #include "common/subsystems.h"

#include "common/abi.h"
#include "kernel/eventQueue.h"
#include "graphics/presentation/videoOutStatus.h"

#include <memory>

namespace Libs::Graphics {
class CommandBuffer;
class Presenter;
} // namespace Libs::Graphics

namespace Libs::VideoOut {

struct VideoOutBufferAttribute2;
struct VideoOutFlipStatus;
struct VideoOutVblankStatus;
struct VideoOutOutputStatus;
struct VideoOutOutputOptions;
struct VideoOutBuffers;
struct VideoOutColorSettings;

class VideoOutDriver final {
public:
	struct Impl;

	VideoOutDriver(uint32_t width, uint32_t height, Graphics::Presenter& presenter);
	~VideoOutDriver();
	KYTY_CLASS_NO_COPY(VideoOutDriver);

	int  SubmitFlipFromGpu(Graphics::CommandBuffer& buffer, int handle, int index, int flip_mode,
	                       int64_t flip_arg, uint64_t& request_id);
	void PrepareFlip(uint64_t request_id, Graphics::CommandBuffer& buffer);
	void CompleteFlip(uint64_t request_id);
	void SubmitFlipPreparation(uint64_t request_id);
	void WaitForSubmitSlot(int handle);
	void WaitFlipDone(int handle, int index);

	[[nodiscard]] Impl& State() noexcept;

private:
	std::unique_ptr<Impl> m_impl;
};

[[nodiscard]] VideoOutDriver& VideoOutInit(uint32_t width, uint32_t height,
                                           Graphics::Presenter& presenter);
void                          VideoOutShutdown();

// Headless-safe readiness probe: true only after VideoOutInit ran and before
// VideoOutShutdown clears it. Unlike every VideoOut* entry (which EXITs via
// DriverState() when the driver is null), this never exits and is safe to call
// from any thread at any time — e.g. libComposite Flush staging decisions.
[[nodiscard]] bool VideoOutIsInitialized() noexcept;

KYTY_SYSV_ABI int  VideoOutOpen(int user_id, int bus_type, int index, const void* param);
KYTY_SYSV_ABI int  VideoOutClose(int handle);
KYTY_SYSV_ABI int  VideoOutSetFlipMaster(int slave_handle, int master_handle);
KYTY_SYSV_ABI void VideoOutAddBufferAttributeOption(VideoOutBufferAttribute2* attribute,
                                                   uint64_t option);
KYTY_SYSV_ABI void VideoOutSetBufferAttribute2(VideoOutBufferAttribute2* attribute,
                                               uint64_t pixel_format, uint32_t tiling_mode,
                                               uint32_t width, uint32_t height, uint64_t option,
                                               uint32_t dcc_control,
                                               uint64_t dcc_cb_register_clear_color);
KYTY_SYSV_ABI int  VideoOutSetFlipRate(int handle, int rate);
KYTY_SYSV_ABI int  VideoOutAddFlipEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                        void* udata);
KYTY_SYSV_ABI int  VideoOutAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                          void* udata);
KYTY_SYSV_ABI int VideoOutAddPreVblankStartEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                                 void* udata);
KYTY_SYSV_ABI int VideoOutAddOutputModeEvent(LibKernel::EventQueue::KernelEqueue eq, int handle,
                                             void* udata);
KYTY_SYSV_ABI int VideoOutDeleteFlipEvent(LibKernel::EventQueue::KernelEqueue eq, int handle);
KYTY_SYSV_ABI int VideoOutDeleteVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int handle);
// ps5-vsh: system (pipeline-level) vblank events - registered against a bare
// equeue with no video-out handle. PSM's ScePsmVBlankMainThr /
// ScePsmVBlankCheckerThr and ScePssWaitVBlank wait on these every frame.
KYTY_SYSV_ABI int VideoOutSysAddVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int bus,
                                             void* udata);
KYTY_SYSV_ABI int VideoOutSysAddVblankEvent2(LibKernel::EventQueue::KernelEqueue eq,
                                              unsigned pipeline, void* udata);
KYTY_SYSV_ABI int VideoOutSysDeleteVblankEvent(LibKernel::EventQueue::KernelEqueue eq, int bus);
KYTY_SYSV_ABI int VideoOutSysDeleteVblankEvent2(LibKernel::EventQueue::KernelEqueue eq,
                                                 unsigned pipeline);
KYTY_SYSV_ABI int VideoOutDeletePreVblankStartEvent(LibKernel::EventQueue::KernelEqueue eq,
                                                    int                                 handle);
KYTY_SYSV_ABI int VideoOutRegisterBuffers2(int handle, int set_index, int buffer_index_start,
                                           const VideoOutBuffers* buffers, int buffer_num,
                                           const VideoOutBufferAttribute2* attribute, int category,
                                           void* option);
KYTY_SYSV_ABI int VideoOutSubmitChangeBufferAttribute2(int handle, int set_index,
                                                       const VideoOutBufferAttribute2* attribute,
                                                       void*                           option);
KYTY_SYSV_ABI int VideoOutUnregisterBuffers(int handle, int set_index);
KYTY_SYSV_ABI int VideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg);
KYTY_SYSV_ABI int VideoOutGetFlipStatus(int handle, VideoOutFlipStatus* status);
KYTY_SYSV_ABI int VideoOutIsFlipPending(int handle);
KYTY_SYSV_ABI int VideoOutGetVblankStatus(int handle, VideoOutVblankStatus* status);
KYTY_SYSV_ABI int VideoOutGetEventId(const LibKernel::EventQueue::KernelEvent* ev);
KYTY_SYSV_ABI int VideoOutGetEventData(const LibKernel::EventQueue::KernelEvent* ev, int64_t* data);
KYTY_SYSV_ABI int VideoOutGetEventCount(const LibKernel::EventQueue::KernelEvent* ev);
KYTY_SYSV_ABI int VideoOutWaitVblank(int handle);
KYTY_SYSV_ABI int VideoOutGetOutputStatus(int handle, VideoOutOutputStatus* status);
KYTY_SYSV_ABI int VideoOutAllowOutputResolutionWqhdDetection(int handle);
KYTY_SYSV_ABI int VideoOutVrrPegToFixedRate(int handle, uint64_t arg1, uint64_t arg2);
KYTY_SYSV_ABI int VideoOutVrrUnpegFromFixedRate(int handle);
KYTY_SYSV_ABI int VideoOutInitializeOutputOptions(VideoOutOutputOptions* options);
KYTY_SYSV_ABI int VideoOutIsOutputSupported(int handle, uint64_t mode,
                                            const VideoOutOutputOptions* options,
                                            void* reserved_ptr, uint64_t reserved);
KYTY_SYSV_ABI int VideoOutConfigureOutput(int handle, uint64_t mode,
                                          const VideoOutOutputOptions* options, void* reserved_ptr,
                                          uint64_t reserved);
KYTY_SYSV_ABI int VideoOutSetWindowModeMargins(int handle, int top, int bottom);
KYTY_SYSV_ABI int VideoOutLatencyControlWaitBeforeInput(int handle);
KYTY_SYSV_ABI int VideoOutLatencyMeasureSetStartPoint(int handle, uint32_t point);
KYTY_SYSV_ABI int VideoOutColorSettingsSetGamma(VideoOutColorSettings* settings, float gamma);
KYTY_SYSV_ABI int VideoOutAdjustColor(int handle, const VideoOutColorSettings* settings);

// PS5 system-library variants (used by SceShellCore; same VideoOut_v1 library).
// Exact native signatures are not documented; extra guest arguments are ignored.
// This entry takes a physical head (MAIN=0, VR=1), not a VideoOutOpen handle.
KYTY_SYSV_ABI int VideoOutSysAddSetModeEvent2(LibKernel::EventQueue::KernelEqueue eq, int head,
                                              void* udata);
KYTY_SYSV_ABI int VideoOutSysSetDimmer(int handle, int dimmer);
KYTY_SYSV_ABI int VideoOutSysGetPipelineStatus(int handle, void* status);
KYTY_SYSV_ABI int VideoOutSysIsUserStatusVr(int user_id);
KYTY_SYSV_ABI int VideoOutSysGetVideoOutModeByBusSpecifier(int bus_specifier, void* mode);
KYTY_SYSV_ABI int VideoOutSysIsModeSupported(const void* mode);
KYTY_SYSV_ABI int VideoOutSysModeIsSupported(const void* mode);
KYTY_SYSV_ABI int VideoOutSysModeIs2(const void* mode);
KYTY_SYSV_ABI int VideoOutSysGetResolutionStatus2(uint32_t pipeline, void* status);
KYTY_SYSV_ABI int VideoOutSysIsUserStatusSystemDefault(int user, uint32_t pipeline,
                                                       uint64_t unused, int reserved,
                                                       uint64_t reserved2);
KYTY_SYSV_ABI int VideoOutSysGetPrivacyMaskStatus(uint64_t* occupied_ids);

} // namespace Libs::VideoOut

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUT_H_ */
