#pragma once

#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/classes/ref.hpp"
#include "godot_cpp/classes/ref_counted.hpp"
#include "godot_cpp/core/defs.hpp"
#include "godot_cpp/templates/hash_map.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_vector2_array.hpp"
#include "godot_cpp/variant/string.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace godot {

class IcyAudioDecoder : public RefCounted {
	GDCLASS(IcyAudioDecoder, RefCounted);

public:
	using DecoderFactory = Ref<IcyAudioDecoder> (*)();

private:
	static constexpr size_t RING_CAPACITY = 1 << 19; // 512 KiB ~= 32s at 128kbps, ~= 13s at 320kbps
	static constexpr size_t DEFAULT_BYTE_THRESHOLD = 16384; // ~1s at 128kbps
	static constexpr int MAX_BATCH_FRAMES = 2048;
	static constexpr int MAX_CHANNELS = 2;
	static constexpr size_t BITS_PER_KILOBIT = 1000;
	static constexpr size_t BITS_PER_BYTE = 8;

	// shared with the network thread, guarded by buffer_mutex
	std::mutex buffer_mutex;
	uint8_t ring_buffer[RING_CAPACITY];
	size_t ring_read_pos = 0;
	size_t total_buffered_bytes = 0;

	// main thread only
	float pcm_buffer[MAX_BATCH_FRAMES * MAX_CHANNELS];
	PackedVector2Array godot_frames;
	size_t byte_threshold = DEFAULT_BYTE_THRESHOLD;
	size_t min_byte_threshold = DEFAULT_BYTE_THRESHOLD;
	bool is_stream_info_known = false;
	bool is_decode_gated = true;

	int target_buffer_frames = 11025;
	int max_capacity_buffer_frames = 0;
	real_t buffering_progress = 0.0f;
	bool is_buffering = true;

	// registration must happen during module initialisation (single-threaded); the registry is read-only afterwards.
	static HashMap<String, DecoderFactory> &get_decoder_registry();

	// main thread only (called from process_audio()); emits buffering_started / buffering_finished directly
	void update_buffering_state();

protected:
	Ref<AudioStreamGeneratorPlayback> playback;

	// set by _probe_stream(); channels must be 1 or 2
	int stream_sample_rate = 0;
	int stream_channels = 0;

	// codec hooks, main thread only
	// returns true once stream_sample_rate / stream_channels are known; called again next tick otherwise
	virtual bool _probe_stream() = 0;
	// writes up to p_max_frames interleaved float frames; fewer than p_max_frames means the codec starved
	virtual int _decode_frames(float *r_interleaved, int p_max_frames) = 0;
	// releases codec state so _probe_stream() can run again on a fresh stream
	virtual void _reset_codec() = 0;

	// thread-safe; drains the jitter ring (pull-style codecs call this from their read callback)
	size_t read_encoded(uint8_t *r_out, size_t p_max_bytes);
	// smallest amount the codec needs to probe; set_bitrate_hint() never goes below it
	void set_min_byte_threshold(size_t p_bytes);

	static void _bind_methods();

public:
	void set_buffer_thresholds(int p_target_frames);
	void set_playback(const Ref<AudioStreamGeneratorPlayback> p_playback);
	real_t get_buffering_progress() const;
	bool get_is_buffering() const;
	int get_current_target_frames() const;
	int get_current_max_capacity_frames() const;

	void set_bitrate_hint(int p_kbps);
	int get_stream_sample_rate() const;
	int get_stream_channels() const;

	static void register_decoder(const String &p_mime_type, DecoderFactory p_factory);
	static Ref<IcyAudioDecoder> create_for_mime_type(const String &p_mime_type);

	// thread-safe, called from the network thread
	void push_encoded_data(const uint8_t *p_data, size_t p_size);
	void push_encoded_data(const PackedByteArray &p_data);
	// main thread only: gate -> probe -> decode -> push. signals are emitted directly.
	void process_audio();
	// main thread only: drops buffered data and codec state so the decoder can be reused for a new stream
	void reset();
};

} //namespace godot
