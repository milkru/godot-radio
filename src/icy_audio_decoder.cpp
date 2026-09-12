#include "icy_audio_decoder.hpp"
#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/math.hpp"
#include "godot_cpp/core/object.hpp"

using namespace godot;

void IcyAudioDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_playback", "playback"), &IcyAudioDecoder::set_playback);
	ClassDB::bind_method(D_METHOD("set_buffer_thresholds", "target_frames"), &IcyAudioDecoder::set_buffer_thresholds);
	ClassDB::bind_method(D_METHOD("process_audio"), &IcyAudioDecoder::process_audio);
	ClassDB::bind_method(D_METHOD("get_buffering_progress"), &IcyAudioDecoder::get_buffering_progress);
	ClassDB::bind_method(D_METHOD("is_buffering"), &IcyAudioDecoder::get_is_buffering);
	ClassDB::bind_method(D_METHOD("get_current_target_frames"), &IcyAudioDecoder::get_current_target_frames);
	ClassDB::bind_method(D_METHOD("get_current_max_capacity_frames"), &IcyAudioDecoder::get_current_max_capacity_frames);
	ClassDB::bind_static_method("IcyAudioDecoder", D_METHOD("create_for_mime_type", "mime_type"), &IcyAudioDecoder::create_for_mime_type);

	ADD_SIGNAL(MethodInfo("buffering_started"));
	ADD_SIGNAL(MethodInfo("buffering_finished"));
}

HashMap<String, IcyAudioDecoder::DecoderFactory> &IcyAudioDecoder::get_decoder_registry() {
	static HashMap<String, DecoderFactory> registry;
	return registry;
}

static String normalize_mime_type(const String &p_mime_type) {
	const int param_idx = p_mime_type.find(";");
	const String base = param_idx == -1 ? p_mime_type : p_mime_type.substr(0, param_idx);
	return base.strip_edges().to_lower();
}

void IcyAudioDecoder::register_decoder(const String &p_mime_type, DecoderFactory p_factory) {
	get_decoder_registry()[normalize_mime_type(p_mime_type)] = p_factory;
}

Ref<IcyAudioDecoder> IcyAudioDecoder::create_for_mime_type(const String &p_mime_type) {
	HashMap<String, DecoderFactory> &registry = get_decoder_registry();
	HashMap<String, DecoderFactory>::Iterator it = registry.find(normalize_mime_type(p_mime_type));

	if (it == registry.end()) {
		return Ref<IcyAudioDecoder>();
	}

	DecoderFactory factory = it->value;
	if (!factory) {
		return Ref<IcyAudioDecoder>();
	}

	return factory();
}

real_t IcyAudioDecoder::get_buffering_progress() const {
	return buffering_progress;
}

bool IcyAudioDecoder::get_is_buffering() const {
	return is_buffering;
}

int IcyAudioDecoder::get_current_target_frames() const {
	return target_buffer_frames;
}

int IcyAudioDecoder::get_current_max_capacity_frames() const {
	return max_capacity_buffer_frames;
}

void IcyAudioDecoder::set_playback(const Ref<AudioStreamGeneratorPlayback> p_playback) {
	playback = p_playback;
	max_capacity_buffer_frames = playback.is_valid() ? playback->get_frames_available() : 0;
}

void IcyAudioDecoder::set_buffer_thresholds(int p_target_frames) {
	target_buffer_frames = p_target_frames;
}

void IcyAudioDecoder::update_buffering_state() {
	if (playback.is_null()) { return; }

	const int frames_available = playback->get_frames_available();
	if (frames_available > max_capacity_buffer_frames) {
		max_capacity_buffer_frames = frames_available;
	}
	const int buffered_frames = max_capacity_buffer_frames - frames_available;

	if (is_buffering) {
		buffering_progress = static_cast<real_t>(buffered_frames) / static_cast<real_t>(target_buffer_frames);
		buffering_progress = MIN(buffering_progress, 1.0f);

		if (buffered_frames >= target_buffer_frames) {
			is_buffering = false;
			buffering_progress = 1.0;
			emit_signal("buffering_finished");
		}
	} else {
		if (buffered_frames <= 0) {
			is_buffering = true;
			buffering_progress = 0.0f;
			emit_signal("buffering_started");
		}
	}
}
