#pragma once

#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_vector2_array.hpp"
#include "icy_audio_decoder.hpp"
#include "thirdparty/dr_mp3.h"
#include <mutex>

namespace godot {

class IcyMp3AudioDecoder : public IcyAudioDecoder {
	GDCLASS(IcyMp3AudioDecoder, IcyAudioDecoder);

private:
	static constexpr size_t RING_CAPACITY = 1 << 19; // 512 KiB ~= 32s at 128kbps, ~= 13s at 320kbps
	static constexpr int MIN_FRAMES_TO_DECODE = 1024;
	static constexpr int MAX_BATCH_FRAMES = 2048;
	static constexpr int MAX_CHANNELS = 2;

	drmp3 mp3;
	float pcm_buffer[MAX_BATCH_FRAMES * MAX_CHANNELS];
	PackedVector2Array godot_frames;
	std::mutex buffer_mutex;
	uint8_t ring_buffer[RING_CAPACITY];
	size_t ring_read_pos = 0;
	size_t total_buffered_bytes = 0;
	size_t byte_threshold = 16384; // 1s at 128kbps - lowest possible value, which is also dr_mp3's DRMP3_MIN_DATA_CHUNK_SIZE

	int stream_sample_rate = 0;
	int stream_channels = 0;

	bool is_initialized = false;
	bool is_decode_gated = true;

	static size_t on_read_callback(void* p_user_data, void* p_buffer_out, size_t bytes_to_read);
	size_t read_data(void* p_buffer_out, size_t bytes_to_read);

protected:
    static void _bind_methods();

public:
	IcyMp3AudioDecoder();
	~IcyMp3AudioDecoder();

	static Ref<IcyAudioDecoder> create_instance();
	void set_bitrate_hint(int p_kbps);
	int get_stream_sample_rate() const;
	int get_stream_channels() const;

	void push_encoded_data(const PackedByteArray& p_data) override;
	void process_audio() override;
};

}
