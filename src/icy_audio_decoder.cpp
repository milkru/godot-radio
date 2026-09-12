#include "icy_audio_decoder.hpp"
#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include "godot_cpp/core/math.hpp"
#include "godot_cpp/core/object.hpp"
#include "godot_cpp/variant/vector2.hpp"
#include <cstring>

using namespace godot;

void IcyAudioDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_playback", "playback"), &IcyAudioDecoder::set_playback);
	ClassDB::bind_method(D_METHOD("set_buffer_thresholds", "target_frames"), &IcyAudioDecoder::set_buffer_thresholds);
	ClassDB::bind_method(D_METHOD("process_audio"), &IcyAudioDecoder::process_audio);
	ClassDB::bind_method(D_METHOD("reset"), &IcyAudioDecoder::reset);
	ClassDB::bind_method(D_METHOD("get_buffering_progress"), &IcyAudioDecoder::get_buffering_progress);
	ClassDB::bind_method(D_METHOD("is_buffering"), &IcyAudioDecoder::get_is_buffering);
	ClassDB::bind_method(D_METHOD("get_current_target_frames"), &IcyAudioDecoder::get_current_target_frames);
	ClassDB::bind_method(D_METHOD("get_current_max_capacity_frames"), &IcyAudioDecoder::get_current_max_capacity_frames);
	ClassDB::bind_method(D_METHOD("set_bitrate_hint", "kbps"), &IcyAudioDecoder::set_bitrate_hint);
	ClassDB::bind_method(D_METHOD("get_stream_sample_rate"), &IcyAudioDecoder::get_stream_sample_rate);
	ClassDB::bind_method(D_METHOD("get_stream_channels"), &IcyAudioDecoder::get_stream_channels);
	ClassDB::bind_static_method("IcyAudioDecoder", D_METHOD("create_for_mime_type", "mime_type"), &IcyAudioDecoder::create_for_mime_type);

	ADD_SIGNAL(MethodInfo("buffering_started"));
	ADD_SIGNAL(MethodInfo("buffering_finished"));
	ADD_SIGNAL(MethodInfo("stream_info_ready"));
	ADD_SIGNAL(MethodInfo("buffer_overrun", PropertyInfo(Variant::INT, "dropped_bytes")));
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

int IcyAudioDecoder::get_stream_sample_rate() const {
	return stream_sample_rate;
}

int IcyAudioDecoder::get_stream_channels() const {
	return stream_channels;
}

void IcyAudioDecoder::set_playback(const Ref<AudioStreamGeneratorPlayback> p_playback) {
	playback = p_playback;
	max_capacity_buffer_frames = playback.is_valid() ? playback->get_frames_available() : 0;
}

void IcyAudioDecoder::set_buffer_thresholds(int p_target_frames) {
	target_buffer_frames = p_target_frames;
}

void IcyAudioDecoder::set_min_byte_threshold(size_t p_bytes) {
	min_byte_threshold = p_bytes;
	byte_threshold = MAX(byte_threshold, min_byte_threshold);
}

void IcyAudioDecoder::set_bitrate_hint(int p_kbps) {
	if (p_kbps <= 0) { return; }
	// one second of encoded audio at the given bitrate
	size_t computed = (static_cast<size_t>(p_kbps) * BITS_PER_KILOBIT) / BITS_PER_BYTE;
	byte_threshold = MAX(computed, min_byte_threshold);
}

void IcyAudioDecoder::push_encoded_data(const PackedByteArray &p_data) {
	push_encoded_data(p_data.ptr(), static_cast<size_t>(p_data.size()));
}

void IcyAudioDecoder::push_encoded_data(const uint8_t *p_data, size_t p_size) {
	if (p_data == nullptr || p_size == 0) { return; }

	const uint8_t *src = p_data;
	size_t bytes_to_write = p_size;
	size_t dropped_bytes = 0;

	// single push larger than the whole ring (keep its tail)
	if (bytes_to_write > RING_CAPACITY) {
		dropped_bytes += bytes_to_write - RING_CAPACITY;
		src += bytes_to_write - RING_CAPACITY;
		bytes_to_write = RING_CAPACITY;
	}

	{
		std::lock_guard<std::mutex> lock(buffer_mutex);

		// drop the oldest bytes so playback stays near the live edge instead of replaying a backlog
		size_t free_bytes = RING_CAPACITY - total_buffered_bytes;
		if (bytes_to_write > free_bytes) {
			size_t overflow = bytes_to_write - free_bytes;
			ring_read_pos = (ring_read_pos + overflow) % RING_CAPACITY;
			total_buffered_bytes -= overflow;
			dropped_bytes += overflow;
		}

		size_t write_pos = (ring_read_pos + total_buffered_bytes) % RING_CAPACITY;
		size_t first_part = MIN(bytes_to_write, RING_CAPACITY - write_pos);
		memcpy(ring_buffer + write_pos, src, first_part);
		if (first_part < bytes_to_write) {
			memcpy(ring_buffer, src + first_part, bytes_to_write - first_part);
		}
		total_buffered_bytes += bytes_to_write;
	}

	if (dropped_bytes > 0) {
		// Runs on the network thread; the signal has to go through the message queue.
		WARN_PRINT_ONCE("IcyAudioDecoder: jitter buffer overrun, dropping oldest audio. Is process_audio() being called regularly?");
		call_deferred("emit_signal", "buffer_overrun", static_cast<int>(dropped_bytes));
	}
}

size_t IcyAudioDecoder::read_encoded(uint8_t *r_out, size_t p_max_bytes) {
	std::lock_guard<std::mutex> lock(buffer_mutex);

	size_t bytes_to_copy = MIN(p_max_bytes, total_buffered_bytes);
	if (bytes_to_copy == 0) { return 0; }

	size_t first_part = MIN(bytes_to_copy, RING_CAPACITY - ring_read_pos);
	memcpy(r_out, ring_buffer + ring_read_pos, first_part);
	if (first_part < bytes_to_copy) {
		memcpy(r_out + first_part, ring_buffer, bytes_to_copy - first_part);
	}

	ring_read_pos = (ring_read_pos + bytes_to_copy) % RING_CAPACITY;
	total_buffered_bytes -= bytes_to_copy;

	return bytes_to_copy;
}

void IcyAudioDecoder::reset() {
	{
		std::lock_guard<std::mutex> lock(buffer_mutex);
		ring_read_pos = 0;
		total_buffered_bytes = 0;
	}

	_reset_codec();
	stream_sample_rate = 0;
	stream_channels = 0;
	is_stream_info_known = false;
	is_decode_gated = true;
	is_buffering = true;
	buffering_progress = 0.0f;
}

void IcyAudioDecoder::process_audio() {
	update_buffering_state();

	// stream info is probed before a playback is required, so the caller can size the generator
	// (mix_rate) from stream_info_ready and only then hand over a correctly sized playback
	if (!is_stream_info_known) {
		{
			std::lock_guard<std::mutex> lock(buffer_mutex);
			if (total_buffered_bytes < byte_threshold) { return; }
		}

		if (!_probe_stream()) { return; }
		ERR_FAIL_COND_MSG(stream_channels < 1 || stream_channels > MAX_CHANNELS, "IcyAudioDecoder: unsupported channel count " + itos(stream_channels));

		is_stream_info_known = true;
		// main thread, emitted directly; a handler may call set_playback() here
		emit_signal("stream_info_ready");
	}

	if (playback.is_null()) { return; }

	while (true) {
		if (playback->get_frames_available() < MAX_BATCH_FRAMES) { break; }

		{
			std::lock_guard<std::mutex> lock(buffer_mutex);
			if (is_decode_gated) {
				if (total_buffered_bytes < byte_threshold) { break; }
				is_decode_gated = false;
			}
		}

		int frames_mixed = _decode_frames(pcm_buffer, MAX_BATCH_FRAMES);

		if (frames_mixed < MAX_BATCH_FRAMES) {
			// the codec ran dry (its own buffer and the ring) - rebuffer to byte_threshold before resuming
			is_decode_gated = true;
			if (frames_mixed <= 0) { break; }
		}

		godot_frames.resize(frames_mixed);
		Vector2 *out_ptr = godot_frames.ptrw();
		const float *in_ptr = pcm_buffer;

		if (stream_channels == 2) {
			for (int i = 0; i < frames_mixed; ++i) {
				out_ptr[i] = Vector2(in_ptr[i * 2], in_ptr[i * 2 + 1]);
			}
		} else {
			for (int i = 0; i < frames_mixed; ++i) {
				out_ptr[i] = Vector2(in_ptr[i], in_ptr[i]);
			}
		}

		playback->push_buffer(godot_frames);
	}
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
