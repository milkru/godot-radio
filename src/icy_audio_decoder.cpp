#include "icy_audio_decoder.hpp"
#include "godot_cpp/classes/audio_stream_generator.hpp"
#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/object.hpp"
#include "godot_cpp/variant/utility_functions.hpp"

using namespace godot;

void IcyAudioDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_playback", "playback", "generator"), &IcyAudioDecoder::set_playback, DEFVAL(Ref<AudioStreamGenerator>()));
	ClassDB::bind_method(D_METHOD("set_buffer_thresholds", "target_frames", "max_capacity_frames"), &IcyAudioDecoder::set_buffer_thresholds, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("process_audio"), &IcyAudioDecoder::process_audio);
	ClassDB::bind_method(D_METHOD("get_buffering_progress"), &IcyAudioDecoder::get_buffering_progress);
	ClassDB::bind_method(D_METHOD("is_buffering"), &IcyAudioDecoder::get_is_buffering);
	ClassDB::bind_method(D_METHOD("get_current_target_frames"), &IcyAudioDecoder::get_current_target_frames);
	ClassDB::bind_method(D_METHOD("get_current_max_capacity_frames"), &IcyAudioDecoder::get_current_max_capacity_frames);

	ADD_SIGNAL(MethodInfo("buffering_started"));
	ADD_SIGNAL(MethodInfo("buffering_finished"));
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

void IcyAudioDecoder::set_playback(const Ref<AudioStreamGeneratorPlayback> p_playback, const Ref<AudioStreamGenerator> p_generator) {
	playback = p_playback;

	if (p_generator.is_valid()) {
		max_capacity_buffer_frames = static_cast<int>(p_generator->get_mix_rate() * p_generator->get_buffer_length());
		max_capacity_from_generator = true;
	}
}

void IcyAudioDecoder::set_buffer_thresholds(int p_target_frames, int p_max_capacity_frames) {
	target_buffer_frames = p_target_frames;

	if (p_max_capacity_frames < 0) {
		return;
	}

	if (max_capacity_from_generator && p_max_capacity_frames != max_capacity_buffer_frames) {
		UtilityFunctions::push_warning(
			"IcyAudioDecoder::set_buffer_thresholds: max_capacity_frames (", p_max_capacity_frames,
			") does not match the capacity derived from the AudioStreamGenerator passed to set_playback() (",
			max_capacity_buffer_frames, "). Keeping the generator-derived value.");
		return;
	}

	max_capacity_buffer_frames = p_max_capacity_frames;
}

void IcyAudioDecoder::update_buffering_state() {
	if (playback.is_null()) { return; }

	int frames_needed = playback->get_frames_available();
	int buffered_frames = max_capacity_buffer_frames - frames_needed;

	if (is_buffering) {
		buffering_progress = static_cast<real_t>(buffered_frames) / static_cast<real_t>(target_buffer_frames);
		buffering_progress = MIN(buffering_progress, 1.0f);

		if (buffered_frames >= target_buffer_frames) {
			is_buffering = false;
			buffering_progress = 1.0;
			call_deferred("emit_signal", "buffering_finished");
		}
	} else {
		if (buffered_frames <= 0) {
			is_buffering = true;
			buffering_progress = 0.0f;
			call_deferred("emit_signal", "buffering_started");
		}
	}
}
