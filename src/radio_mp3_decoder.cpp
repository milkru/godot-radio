#include "radio_mp3_decoder.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include "godot_cpp/core/memory.hpp"
#include <cstring>

#define DR_MP3_FLOAT_OUTPUT
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "thirdparty/dr_mp3.h"

using namespace godot;

RadioMp3Decoder::RadioMp3Decoder() {
	drmp3dec_init(&dec);
}

Ref<RadioDecoder> RadioMp3Decoder::create_instance() {
	return memnew(RadioMp3Decoder);
}

void RadioMp3Decoder::_bind_methods() {
}

// one MP3 frame per call (<= 1152 PCM frames). drmp3dec_decode_frame() reports in frame_bytes how much input it
// consumed: a decoded frame, or junk it skipped while resyncing (frames == 0), in which case it is called again
RadioDecoder::DecodedBatch RadioMp3Decoder::_decode_frames(float *r_interleaved, int p_max_frames) {
	DecodedBatch batch;
	ERR_FAIL_COND_V_MSG(p_max_frames < DRMP3_MAX_PCM_FRAMES_PER_MP3_FRAME, batch, "RadioMp3Decoder: batch buffer smaller than one MP3 frame");

	while (true) {
		staged += static_cast<int>(read_encoded(staging + staged, static_cast<size_t>(STAGING_CAPACITY - staged)));
		if (staged < MIN_LOOKAHEAD) { return batch; }

		drmp3dec_frame_info info;
		const int frames = drmp3dec_decode_frame(&dec, staging, staged, r_interleaved, &info);
		if (info.frame_bytes <= 0) { return batch; }

		staged -= info.frame_bytes;
		memmove(staging, staging + info.frame_bytes, static_cast<size_t>(staged));

		if (frames > 0) {
			batch.frames = frames;
			batch.channels = info.channels;
			batch.sample_rate = info.sample_rate;
			batch.bitrate_kbps = info.bitrate_kbps;
			return batch;
		}
	}
}

void RadioMp3Decoder::_reset_codec() {
	drmp3dec_init(&dec);
	staged = 0;
}
