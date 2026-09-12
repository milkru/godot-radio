#pragma once

#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/classes/ref.hpp"
#include "godot_cpp/classes/ref_counted.hpp"
#include "godot_cpp/core/defs.hpp"
#include "godot_cpp/templates/hash_map.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/string.hpp"

namespace godot {

class IcyAudioDecoder : public RefCounted {
	GDCLASS(IcyAudioDecoder, RefCounted);

public:
	using DecoderFactory = Ref<IcyAudioDecoder> (*)();

private:
	int target_buffer_frames = 11025;
	int max_capacity_buffer_frames = 0;
	real_t buffering_progress = 0.0f;
	bool is_buffering = true;

	// registration must happen during module initialisation (single-threaded); the registry is read-only afterwards.
	static HashMap<String, DecoderFactory> &get_decoder_registry();

protected:
	Ref<AudioStreamGeneratorPlayback> playback;

	// main thread only (called from process_audio()); emits buffering_started / buffering_finished directly
	void update_buffering_state();
	static void _bind_methods();

public:
	void set_buffer_thresholds(int p_target_frames);
	void set_playback(const Ref<AudioStreamGeneratorPlayback> p_playback);
	real_t get_buffering_progress() const;
	bool get_is_buffering() const;
	int get_current_target_frames() const;
	int get_current_max_capacity_frames() const;

	static void register_decoder(const String &p_mime_type, DecoderFactory p_factory);
	static Ref<IcyAudioDecoder> create_for_mime_type(const String &p_mime_type);

	// may be called from any thread; implementations must be thread-safe
	virtual void push_encoded_data(const PackedByteArray &p_data) = 0;
	// main thread only: signals are emitted directly (not deferred) so handlers run synchronously
	virtual void process_audio() = 0;
};

} //namespace godot
