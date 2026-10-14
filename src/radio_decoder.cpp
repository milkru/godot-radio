#include "radio_decoder.hpp"
#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include "godot_cpp/core/math.hpp"
#include "godot_cpp/core/object.hpp"
#include "godot_cpp/variant/utility_functions.hpp"
#include "godot_cpp/variant/vector2.hpp"
#include <cstring>

using namespace godot;

void RadioDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_playback", "playback"), &RadioDecoder::set_playback);
	ClassDB::bind_method(D_METHOD("set_buffer_thresholds", "target_frames"), &RadioDecoder::set_buffer_thresholds);
	ClassDB::bind_method(D_METHOD("process_audio"), &RadioDecoder::process_audio);
	ClassDB::bind_method(D_METHOD("reset"), &RadioDecoder::reset);
	ClassDB::bind_method(D_METHOD("get_buffering_progress"), &RadioDecoder::get_buffering_progress);
	ClassDB::bind_method(D_METHOD("is_buffering"), &RadioDecoder::get_is_buffering);
	ClassDB::bind_method(D_METHOD("get_current_target_frames"), &RadioDecoder::get_current_target_frames);
	ClassDB::bind_method(D_METHOD("get_current_max_capacity_frames"), &RadioDecoder::get_current_max_capacity_frames);
	ClassDB::bind_method(D_METHOD("set_bitrate_hint", "kbps"), &RadioDecoder::set_bitrate_hint);
	ClassDB::bind_method(D_METHOD("get_stream_sample_rate"), &RadioDecoder::get_stream_sample_rate);
	ClassDB::bind_method(D_METHOD("get_stream_channels"), &RadioDecoder::get_stream_channels);
	ClassDB::bind_static_method("RadioDecoder", D_METHOD("create_for_mime_type", "mime_type"), &RadioDecoder::create_for_mime_type);

	ADD_SIGNAL(MethodInfo("buffering_started"));
	ADD_SIGNAL(MethodInfo("buffering_finished"));
	ADD_SIGNAL(MethodInfo("stream_info_ready"));
	ADD_SIGNAL(MethodInfo("buffer_overrun", PropertyInfo(Variant::INT, "dropped_bytes")));
	ADD_SIGNAL(MethodInfo("stream_error", PropertyInfo(Variant::STRING, "message")));
}

HashMap<String, RadioDecoder::DecoderFactory> &RadioDecoder::get_decoder_registry() {
	static HashMap<String, DecoderFactory> registry;
	return registry;
}

static String normalize_mime_type(const String &p_mime_type) {
	const int param_idx = p_mime_type.find(";");
	const String base = param_idx == -1 ? p_mime_type : p_mime_type.substr(0, param_idx);
	return base.strip_edges().to_lower();
}

void RadioDecoder::register_decoder(const String &p_mime_type, DecoderFactory p_factory) {
	get_decoder_registry()[normalize_mime_type(p_mime_type)] = p_factory;
}

Ref<RadioDecoder> RadioDecoder::create_for_mime_type(const String &p_mime_type) {
	HashMap<String, DecoderFactory> &registry = get_decoder_registry();
	HashMap<String, DecoderFactory>::Iterator it = registry.find(normalize_mime_type(p_mime_type));

	if (it == registry.end()) {
		return Ref<RadioDecoder>();
	}

	DecoderFactory factory = it->value;
	if (!factory) {
		return Ref<RadioDecoder>();
	}

	return factory();
}

real_t RadioDecoder::get_buffering_progress() const {
	return buffering_progress;
}

bool RadioDecoder::get_is_buffering() const {
	return is_buffering;
}

int RadioDecoder::get_current_target_frames() const {
	return target_buffer_frames;
}

int RadioDecoder::get_current_max_capacity_frames() const {
	return max_capacity_buffer_frames;
}

int RadioDecoder::get_stream_sample_rate() const {
	return stream_sample_rate;
}

int RadioDecoder::get_stream_channels() const {
	return stream_channels;
}

void RadioDecoder::set_playback(const Ref<AudioStreamGeneratorPlayback> p_playback) {
	playback = p_playback;
	max_capacity_buffer_frames = playback.is_valid() ? playback->get_frames_available() : 0;
}

void RadioDecoder::set_buffer_thresholds(int p_target_frames) {
	target_buffer_frames = p_target_frames;
}

// one second of encoded audio at the given bitrate, never below the floor
size_t RadioDecoder::_threshold_for_bitrate(int p_kbps) {
	const size_t computed = (static_cast<size_t>(p_kbps) * BITS_PER_KILOBIT) / BITS_PER_BYTE;
	return MAX(computed, MIN_BYTE_THRESHOLD);
}

void RadioDecoder::set_bitrate_hint(int p_kbps) {
	if (p_kbps <= 0) { return; }
	byte_threshold = _threshold_for_bitrate(p_kbps);
	is_bitrate_hinted = true;
}

void RadioDecoder::push_encoded_data(const PackedByteArray &p_data) {
	push_encoded_data(p_data.ptr(), static_cast<size_t>(p_data.size()));
}

void RadioDecoder::push_encoded_data(const uint8_t *p_data, size_t p_size) {
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
		// verbose only: stations that open with a large burst overrun the ring in normal use
		UtilityFunctions::print_verbose("RadioDecoder: jitter buffer overrun, dropping oldest audio.");
		// Runs on the network thread; the signal has to go through the message queue.
		call_deferred("emit_signal", "buffer_overrun", static_cast<int>(dropped_bytes));
	}
}

size_t RadioDecoder::read_encoded(uint8_t *r_out, size_t p_max_bytes) {
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

void RadioDecoder::reset() {
	{
		std::lock_guard<std::mutex> lock(buffer_mutex);
		ring_read_pos = 0;
		total_buffered_bytes = 0;
	}

	_reset_codec();
	pending = DecodedBatch();
	stream_sample_rate = 0;
	stream_channels = 0;
	is_stream_info_known = false;
	is_decode_gated = true;
	probe_failures = 0;
	is_buffering = true;
	buffering_progress = 0.0f;
}

void RadioDecoder::process_audio() {
	while (true) {
		if (pending.frames == 0) {
			// the first batch is decoded before a playback is required: its format is announced through
			// stream_info_ready so the caller can size the generator (mix_rate) and only then hand over a
			// correctly sized playback
			if (is_stream_info_known && !_has_room_for_batch()) { break; }

			if (is_decode_gated) {
				std::lock_guard<std::mutex> lock(buffer_mutex);
				if (total_buffered_bytes < byte_threshold) { break; }
			}
			is_decode_gated = false;

			pending = _decode_frames(pcm_buffer, MAX_BATCH_FRAMES);
			if (pending.frames <= 0) {
				pending.frames = 0;
				// the codec ran dry (the ring and whatever it stages itself) - rebuffer to byte_threshold before resuming
				is_decode_gated = true;
				if (!is_stream_info_known) { _count_probe_failure(); }
				break;
			}
			if (pending.channels < 1 || pending.channels > MAX_CHANNELS || pending.frames > MAX_BATCH_FRAMES) {
				const String message = "RadioDecoder: codec returned an invalid batch (" + itos(pending.frames) + " frames, " + itos(pending.channels) + " channels)";
				pending.frames = 0;
				ERR_FAIL_MSG(message);
			}
		}

		// first batch, or the station switched sample rate mid-stream. The generator's mix_rate cannot follow a
		// live playback, so this goes back to the caller through stream_info_ready; a channel change is absorbed
		// below, because the conversion uses the batch's own layout
		if (pending.sample_rate != stream_sample_rate) {
			_announce_stream_info();
		}
		stream_channels = pending.channels;

		// the batch stays pending until a playback with room for it has been set
		if (!_has_room_for_batch()) { break; }
		_push_pending_batch();
	}

	// evaluated after the loop so the state describes the generator as this tick leaves it: an underrun
	// that was refilled above reports buffering_finished now instead of one tick later
	update_buffering_state();
}

bool RadioDecoder::_has_room_for_batch() const {
	return playback.is_valid() && playback->get_frames_available() >= MAX_BATCH_FRAMES;
}

void RadioDecoder::_count_probe_failure() {
	// a failed first decode has eaten byte_threshold bytes; on a mislabelled stream (AAC behind audio/mpeg
	// is common on SHOUTcast v1) this would otherwise repeat silently forever
	++probe_failures;
	if (probe_failures == MAX_PROBE_FAILURES) {
		const String message = "RadioDecoder: could not detect the stream format after " + itos(probe_failures) + " attempts. Is the content type correct?";
		ERR_PRINT(message);
		emit_signal("stream_error", message);
	}
}

void RadioDecoder::_announce_stream_info() {
	stream_sample_rate = pending.sample_rate;
	stream_channels = pending.channels;
	is_stream_info_known = true;
	probe_failures = 0;

	// the first decode had to be gated on the floor; from here on the codec's own bitrate sizes the
	// rebuffer amount unless the caller supplied one (icy-br)
	if (!is_bitrate_hinted && pending.bitrate_kbps > 0) {
		byte_threshold = _threshold_for_bitrate(pending.bitrate_kbps);
	}

	// main thread, emitted directly; a handler is expected to call set_playback() here
	emit_signal("stream_info_ready");
}

void RadioDecoder::_push_pending_batch() {
	godot_frames.resize(pending.frames);
	Vector2 *out_ptr = godot_frames.ptrw();
	const float *in_ptr = pcm_buffer;

	if (pending.channels == 2) {
		for (int i = 0; i < pending.frames; ++i) {
			out_ptr[i] = Vector2(in_ptr[i * 2], in_ptr[i * 2 + 1]);
		}
	} else {
		for (int i = 0; i < pending.frames; ++i) {
			out_ptr[i] = Vector2(in_ptr[i], in_ptr[i]);
		}
	}

	playback->push_buffer(godot_frames);
	pending.frames = 0;
}

void RadioDecoder::update_buffering_state() {
	if (playback.is_null()) { return; }

	const int frames_available = playback->get_frames_available();
	if (frames_available > max_capacity_buffer_frames) {
		max_capacity_buffer_frames = frames_available;
	}
	const int buffered_frames = max_capacity_buffer_frames - frames_available;

	// a target above what the generator can hold would never be reached; the capacity is an observation,
	// so the clamp lives here rather than in the setter
	int target_frames = target_buffer_frames;
	if (target_frames > max_capacity_buffer_frames) {
		WARN_PRINT_ONCE("RadioDecoder: target buffer frames exceed the playback's capacity, clamping. Increase the generator's buffer_length or lower set_buffer_thresholds().");
		target_frames = max_capacity_buffer_frames;
	}
	target_frames = MAX(target_frames, 1);

	if (is_buffering) {
		buffering_progress = static_cast<real_t>(buffered_frames) / static_cast<real_t>(target_frames);
		buffering_progress = MIN(buffering_progress, 1.0f);

		if (buffered_frames >= target_frames) {
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
