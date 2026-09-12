#include "icy_mp3_audio_decoder.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/memory.hpp"

#define DR_MP3_FLOAT_OUTPUT
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "thirdparty/dr_mp3.h"

using namespace godot;

IcyMp3AudioDecoder::IcyMp3AudioDecoder() {
	// the smallest pull dr_mp3 will make, so it is also the lowest usable threshold (~1s at 128kbps)
	set_min_byte_threshold(DRMP3_MIN_DATA_CHUNK_SIZE);
}

IcyMp3AudioDecoder::~IcyMp3AudioDecoder() {
	IcyMp3AudioDecoder::_reset_codec();
}

Ref<IcyAudioDecoder> IcyMp3AudioDecoder::create_instance() {
	return memnew(IcyMp3AudioDecoder);
}

void IcyMp3AudioDecoder::_bind_methods() {
}

size_t IcyMp3AudioDecoder::on_read_callback(void *p_user_data, void *p_buffer_out, size_t bytes_to_read) {
	IcyMp3AudioDecoder *decoder = static_cast<IcyMp3AudioDecoder *>(p_user_data);
	return decoder->read_encoded(static_cast<uint8_t *>(p_buffer_out), bytes_to_read);
}

bool IcyMp3AudioDecoder::_probe_stream() {
	if (!drmp3_init(&mp3, on_read_callback, nullptr, nullptr, nullptr, this, nullptr)) {
		return false;
	}

	is_initialized = true;
	stream_sample_rate = static_cast<int>(mp3.sampleRate);
	stream_channels = static_cast<int>(mp3.channels);
	return true;
}

int IcyMp3AudioDecoder::_decode_frames(float *r_interleaved, int p_max_frames) {
	// atEnd latches when the read callback returns 0; the stream is live, so clear it before every pull
	mp3.atEnd = 0;
	return static_cast<int>(drmp3_read_pcm_frames_f32(&mp3, static_cast<drmp3_uint64>(p_max_frames), r_interleaved));
}

void IcyMp3AudioDecoder::_reset_codec() {
	if (is_initialized) {
		drmp3_uninit(&mp3);
		is_initialized = false;
	}
}
