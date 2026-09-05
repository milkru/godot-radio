#pragma once

#include "godot_cpp/classes/audio_stream_generator.hpp"
#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/classes/ref_counted.hpp"
#include "godot_cpp/templates/hash_map.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include <functional>
#include <mutex>

namespace godot {

class IcyAudioDecoder: public RefCounted {
	GDCLASS(IcyAudioDecoder, RefCounted);

public:
	using DecoderFactory = std::function<Ref<IcyAudioDecoder>()>;

private:
	int target_buffer_frames = 11025;
	int max_capacity_buffer_frames = 44100;
	real_t buffering_progress = 0.0f;
	bool is_buffering = true;
	bool max_capacity_from_generator = false;

	// mutex only guards against two native modules registering concurrently during engine init, not against script access.
	static HashMap<String, DecoderFactory>& get_decoder_registry();
	static std::mutex decoder_registry_mutex;

protected:
	Ref<AudioStreamGeneratorPlayback> playback;

	void update_buffering_state();
	static void _bind_methods();

public:
	void set_buffer_thresholds(int p_target_frames, int p_max_capacity_frames = -1);
	void set_playback(const Ref<AudioStreamGeneratorPlayback> p_playback, const Ref<AudioStreamGenerator> p_generator = Ref<AudioStreamGenerator>());
	real_t get_buffering_progress() const;
	bool get_is_buffering() const;
	int get_current_target_frames() const;
	int get_current_max_capacity_frames() const;

	static void register_decoder(const String &p_mime_type, const DecoderFactory &p_factory);
	static Ref<IcyAudioDecoder> create_for_mime_type(const String &p_mime_type);

	virtual void push_encoded_data(const PackedByteArray& p_data) = 0;
	virtual void process_audio() = 0;
};

}
