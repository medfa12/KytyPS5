// Guest Audiodec boundary: real compressed input, partial buffers and reset.
#include "libs/libs.h"
// This executable calls the Audiodec ABI directly; unrelated audio-library
// export registrations must not pull the entire emulator into the test.
#undef LIB_FUNC
#define LIB_FUNC(nid, function) do {} while (false)
#include "libs/libAudio.cpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Libs::LibAudiodec;

static void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "AudiodecAtrac9Tests: %s\n", message);
		std::abort();
	}
}

int main() {
	Check(AudiodecInitLibrary(AUDIODEC_TYPE_AT9) == 0, "library init failed");
	for (int word: {AUDIODEC_WORD_SZ_16BIT, AUDIODEC_WORD_SZ_FLOAT}) {
		AudiodecParamAt9 param {sizeof(param), word, {0xfe, 0x70, 0x0b, 0xf0}};
		AudiodecAt9Info info {sizeof(info)};
		AudiodecAuInfo au {sizeof(au)};
		AudiodecPcmItem pcm {sizeof(pcm)};
		AudiodecCtrl ctrl {&param, &info, &au, &pcm};
		const int handle = AudiodecCreateDecoder(&ctrl, AUDIODEC_TYPE_AT9);
		Check(handle > 0 && info.uiChannel == 1 && info.uiSamplingRate == 48000 &&
		          info.uiSuperFrameSize == 384 && info.uiFramesInSuperFrame == 4 &&
		          info.uiFrameSamples == 256 && info.uiNextFrameSize == 384 && info.uiBitrate == 144000,
		      "config-derived metadata is incorrect");
		// Four mono blocks, each with a nonzero spectral coefficient, and padding.
		constexpr std::array<uint8_t, 36> block {0, 0, 4, 0x20, 4, 0xc0, 0, 0, 0x1f, 0xc0};
		std::array<uint8_t, 384> encoded {};
		for (int frame = 0; frame < 4; ++frame) {
			std::copy(block.begin(), block.end(), encoded.begin() + frame * block.size());
			if (frame != 0) encoded[frame * block.size()] |= 0x80;
		}
		const uint32_t frame_bytes = 256 * audiodec_word_size_bytes(word);
		std::vector<uint8_t> output(frame_bytes + 16, 0xa5), first;
		au.pAuAddr = encoded.data();
		au.uiAuSize = encoded.size() - 1;
		pcm.pPcmAddr = output.data();
		pcm.uiPcmSize = frame_bytes;
		Check(AudiodecDecode(handle, &ctrl) == AUDIODEC_ERROR_INVALID_AU_SIZE &&
		          au.uiAuSize == 0 && pcm.uiPcmSize == 0 && output[0] == 0xa5,
		      "short input consumed data or wrote output");
		au.uiAuSize = encoded.size();
		pcm.uiPcmSize = frame_bytes - 1;
		Check(AudiodecDecode(handle, &ctrl) == AUDIODEC_ERROR_INVALID_PCM_SIZE &&
		          au.uiAuSize == 0 && pcm.uiPcmSize == 0 && info.uiNextFrameSize == 384,
		      "short output advanced decoder state");
		for (int pass = 0; pass < 2; ++pass) {
			if (pass) Check(AudiodecClearContext(handle) == 0, "reset failed");
			size_t offset = 0;
			for (int frame = 0; frame < 4; ++frame) {
				au.pAuAddr = encoded.data() + offset;
				au.uiAuSize = static_cast<uint32_t>(encoded.size() - offset);
				pcm.uiPcmSize = frame_bytes;
				Check(AudiodecDecode(handle, &ctrl) == 0 && pcm.uiPcmSize == frame_bytes &&
				          au.uiAuSize == (frame == 3 ? 276 : 36), "frame decoding/consumption failed");
				offset += au.uiAuSize;
				Check(info.uiNextFrameSize == (frame == 3 ? 384 : 384 - offset),
				      "remaining superframe size is incorrect");
				Check(std::any_of(output.begin(), output.begin() + frame_bytes,
				                  [](uint8_t v) { return v != 0; }), "decoded audio is silent");
				Check(std::all_of(output.begin() + frame_bytes, output.end(),
				                  [](uint8_t v) { return v == 0xa5; }), "output overrun");
				if (frame == 0) {
					if (!pass) first = output;
					else Check(first == output, "reset retained old transform history");
				}
			}
		}
		Check(AudiodecDeleteDecoder(handle) == 0 &&
		          AudiodecDecode(handle, &ctrl) == AUDIODEC_ERROR_INVALID_HANDLE,
		      "deleted decoder still usable");
		param.uiConfigData[0] = 0;
		Check(AudiodecCreateDecoder(&ctrl, AUDIODEC_TYPE_AT9) == AUDIODEC_ERROR_AT9_INVALID_CONFIG_DATA,
		      "invalid config was accepted");
		param.uiConfigData[0] = 0xfe;
		param.iBwPcm = AUDIODEC_WORD_SZ_24BIT;
		Check(AudiodecCreateDecoder(&ctrl, AUDIODEC_TYPE_AT9) == AUDIODEC_ERROR_INVALID_WORD_LENGTH,
		      "unsupported native AT9 output mode was accepted");
	}
	Check(AudiodecTermLibrary(AUDIODEC_TYPE_AT9) == 0, "library term failed");
	std::puts("AudiodecAtrac9Tests: all cases passed");
}
