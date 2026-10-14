#pragma once

#include "godot_cpp/classes/ref.hpp"
#include "radio_decoder.hpp"
#include "thirdparty/dr_mp3.h"
#include <cstdint>

namespace godot {

class RadioMp3Decoder : public RadioDecoder {
	GDCLASS(RadioMp3Decoder, RadioDecoder);

private:
	static constexpr int MP3_HEADER_BYTES = 4;
	// minimp3 recommends ~10 frames (16 KiB) of lookahead to sync reliably; the base's gate guarantees at least
	// this much in the ring before the first decode, so the whole window fits in one pull
	static constexpr int STAGING_CAPACITY = 16384;
	// a frame is decoded only once the header of the frame after it is in view; with less than this staged, a
	// legal frame at the front could be cut off and minimp3 would discard it as junk (and drop its bit reservoir)
	static constexpr int MIN_LOOKAHEAD = DRMP3_MAX_FREE_FORMAT_FRAME_SIZE + MP3_HEADER_BYTES;

	drmp3dec dec;
	uint8_t staging[STAGING_CAPACITY];
	int staged = 0;

protected:
	static void _bind_methods();

	DecodedBatch _decode_frames(float *r_interleaved, int p_max_frames) override;
	void _reset_codec() override;

public:
	RadioMp3Decoder();

	static Ref<RadioDecoder> create_instance();
};

} //namespace godot
