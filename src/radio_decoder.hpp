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

class RadioDecoder : public RefCounted {
	GDCLASS(RadioDecoder, RefCounted);

public:
	using DecoderFactory = Ref<RadioDecoder> (*)();

protected:
	// what one _decode_frames() call produced. The format describes this batch, not the stream as it was first
	// seen: stations change channels and sample rate between tracks (fallback mounts, ad insertion)
	struct DecodedBatch {
		int frames = 0; // 0 means the codec starved; any positive count is a valid batch
		int channels = 0;
		int sample_rate = 0;
		int bitrate_kbps = 0; // 0 if the codec does not know
	};

private:
	// 128 KiB ~= 8s at 128kbps, ~= 3s at 320kbps. The gate never needs more than byte_threshold (<= 40 KiB at
	// 320kbps) plus a margin for bursty delivery; anything larger only lets playback fall further behind live
	// after a stall, because drop-oldest engages only when the ring is full
	static constexpr size_t RING_CAPACITY = 1 << 17;
	// ~1s at 128kbps. A jitter-buffer policy, not a codec constraint: it also hands the codec a comfortable
	// sync window on the first decode
	static constexpr size_t MIN_BYTE_THRESHOLD = 16384;
	static constexpr int MAX_PROBE_FAILURES = 2;
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
	// a decoded batch that has not been pushed yet (pcm_buffer holds its samples); pending.frames == 0 when none
	DecodedBatch pending;
	PackedVector2Array godot_frames;
	size_t byte_threshold = MIN_BYTE_THRESHOLD;
	bool is_bitrate_hinted = false;
	bool is_stream_info_known = false;
	bool is_decode_gated = true;
	int probe_failures = 0;

	int target_buffer_frames = 11025;
	int max_capacity_buffer_frames = 0;
	real_t buffering_progress = 0.0f;
	bool is_buffering = true;

	// registration must happen during module initialisation (single-threaded); the registry is read-only afterwards.
	static HashMap<String, DecoderFactory> &get_decoder_registry();
	static size_t _threshold_for_bitrate(int p_kbps);

	// main thread only (called from process_audio())
	bool _has_room_for_batch() const;
	void _count_probe_failure();
	// publishes the pending batch's format and emits stream_info_ready
	void _announce_stream_info();
	// converts the pending batch to Vector2 frames and pushes it into the playback
	void _push_pending_batch();
	// emits buffering_started / buffering_finished directly
	void update_buffering_state();

protected:
	Ref<AudioStreamGeneratorPlayback> playback;

	// format of the last announced / last decoded batch; channels are 1 or 2
	int stream_sample_rate = 0;
	int stream_channels = 0;

	// codec hooks, main thread only. Decoders are native: the hooks are plain virtuals, not GDVIRTUAL.
	// Decodes one batch of interleaved float frames into r_interleaved (room for p_max_frames * MAX_CHANNELS
	// floats). Returns frames == 0 when the codec needs more encoded data than is buffered; the first batch
	// that comes back is what announces the stream format, so there is no separate probe
	virtual DecodedBatch _decode_frames(float *r_interleaved, int p_max_frames) = 0;
	// releases codec state so the next _decode_frames() starts from scratch on a fresh stream
	virtual void _reset_codec() = 0;

	// thread-safe; drains the jitter ring (codecs pull their input through this)
	size_t read_encoded(uint8_t *r_out, size_t p_max_bytes);

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
	static Ref<RadioDecoder> create_for_mime_type(const String &p_mime_type);

	// thread-safe, called from the network thread
	void push_encoded_data(const uint8_t *p_data, size_t p_size);
	void push_encoded_data(const PackedByteArray &p_data);
	// main thread only: gate -> decode -> announce format -> push. signals are emitted directly.
	void process_audio();
	// main thread only: drops buffered data and codec state so the decoder can be reused for a new stream
	void reset();
};

} //namespace godot
