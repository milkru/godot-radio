#include "icy_mp3_audio_decoder.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include "godot_cpp/core/math.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/vector2.hpp"
#include <cstring>

#define DR_MP3_FLOAT_OUTPUT
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "thirdparty/dr_mp3.h"

using namespace godot;

IcyMp3AudioDecoder::IcyMp3AudioDecoder() {
	is_initialized = false;
	// ~1s at 128kbps and the smallest pull dr_mp3 will make, so it is also the lowest usable threshold
	byte_threshold = DRMP3_MIN_DATA_CHUNK_SIZE;
}

IcyMp3AudioDecoder::~IcyMp3AudioDecoder() {
	if (is_initialized) {
		drmp3_uninit(&mp3);
	}
}

Ref<IcyAudioDecoder> IcyMp3AudioDecoder::create_instance() {
	return memnew(IcyMp3AudioDecoder);
}

void IcyMp3AudioDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bitrate_hint", "kbps"), &IcyMp3AudioDecoder::set_bitrate_hint);
	ClassDB::bind_method(D_METHOD("get_stream_sample_rate"), &IcyMp3AudioDecoder::get_stream_sample_rate);
	ClassDB::bind_method(D_METHOD("get_stream_channels"), &IcyMp3AudioDecoder::get_stream_channels);

	ADD_SIGNAL(MethodInfo("stream_info_ready"));
	ADD_SIGNAL(MethodInfo("buffer_overrun", PropertyInfo(Variant::INT, "dropped_bytes")));
}

void IcyMp3AudioDecoder::set_bitrate_hint(int p_kbps) {
	if (p_kbps <= 0) { return; }
	// one second of encoded audio at the given bitrate
	size_t computed = (static_cast<size_t>(p_kbps) * BITS_PER_KILOBIT) / BITS_PER_BYTE;
	byte_threshold = MAX(computed, static_cast<size_t>(DRMP3_MIN_DATA_CHUNK_SIZE));
}

int IcyMp3AudioDecoder::get_stream_sample_rate() const {
	return stream_sample_rate;
}

int IcyMp3AudioDecoder::get_stream_channels() const {
	return stream_channels;
}

void IcyMp3AudioDecoder::push_encoded_data(const PackedByteArray &p_data) {
	if (p_data.is_empty()) { return; }

	const uint8_t *src = p_data.ptr();
	size_t bytes_to_write = static_cast<size_t>(p_data.size());
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
		WARN_PRINT_ONCE("IcyMp3AudioDecoder: jitter buffer overrun, dropping oldest audio. Is process_audio() being called regularly?");
		call_deferred("emit_signal", "buffer_overrun", static_cast<int>(dropped_bytes));
	}
}

size_t IcyMp3AudioDecoder::on_read_callback(void *p_user_data, void *p_buffer_out, size_t bytes_to_read) {
	IcyMp3AudioDecoder *decoder = static_cast<IcyMp3AudioDecoder *>(p_user_data);
	return decoder->read_data(p_buffer_out, bytes_to_read);
}

size_t IcyMp3AudioDecoder::read_data(void *p_buffer_out, size_t bytes_to_read) {
	std::lock_guard<std::mutex> lock(buffer_mutex);

	size_t bytes_to_copy = MIN(bytes_to_read, total_buffered_bytes);
	if (bytes_to_copy == 0) { return 0; }

	uint8_t *out_ptr = static_cast<uint8_t *>(p_buffer_out);
	size_t first_part = MIN(bytes_to_copy, RING_CAPACITY - ring_read_pos);
	memcpy(out_ptr, ring_buffer + ring_read_pos, first_part);
	if (first_part < bytes_to_copy) {
		memcpy(out_ptr + first_part, ring_buffer, bytes_to_copy - first_part);
	}

	ring_read_pos = (ring_read_pos + bytes_to_copy) % RING_CAPACITY;
	total_buffered_bytes -= bytes_to_copy;

	return bytes_to_copy;
}

void IcyMp3AudioDecoder::process_audio() {
	update_buffering_state();

	// stream info is probed before a playback is required, so the caller can size the generator
	// (mix_rate) from stream_info_ready and only then hand over a correctly sized playback
	if (!is_initialized) {
		{
			std::lock_guard<std::mutex> lock(buffer_mutex);
			if (total_buffered_bytes < byte_threshold) { return; }
		}

		if (drmp3_init(&mp3, on_read_callback, nullptr, nullptr, nullptr, this, nullptr)) {
			is_initialized = true;
			stream_sample_rate = static_cast<int>(mp3.sampleRate);
			stream_channels = static_cast<int>(mp3.channels);

			// main thread, emitted directly; a handler may call set_playback() here
			emit_signal("stream_info_ready");
		} else {
			return;
		}
	}

	if (playback.is_null()) { return; }

	while (true) {
		int frames_needed = playback->get_frames_available();
		if (frames_needed < MIN_FRAMES_TO_DECODE) { break; }

		int batch_frames = MIN(frames_needed, MAX_BATCH_FRAMES);

		{
			std::lock_guard<std::mutex> lock(buffer_mutex);
			if (is_decode_gated) {
				if (total_buffered_bytes < byte_threshold) { break; }
				is_decode_gated = false;
			} else if (total_buffered_bytes < DRMP3_MIN_DATA_CHUNK_SIZE) {
				is_decode_gated = true;
				break;
			}
		}

		if (mp3.atEnd) {
			mp3.atEnd = 0;
		}

		drmp3_uint64 frames_mixed = drmp3_read_pcm_frames_f32(&mp3, batch_frames, pcm_buffer);

		if (frames_mixed == 0) { break; }

		// CowData keeps the allocation when shrinking or staying the same size, so this only allocates on growth
		godot_frames.resize(frames_mixed);
		Vector2 *out_ptr = godot_frames.ptrw();
		const float *in_ptr = pcm_buffer;

		if (mp3.channels == 2) {
			for (drmp3_uint64 i = 0; i < frames_mixed; ++i) {
				out_ptr[i] = Vector2(in_ptr[i * 2], in_ptr[i * 2 + 1]);
			}
		} else {
			for (drmp3_uint64 i = 0; i < frames_mixed; ++i) {
				out_ptr[i] = Vector2(in_ptr[i], in_ptr[i]);
			}
		}

		playback->push_buffer(godot_frames);

		if (frames_mixed < static_cast<drmp3_uint64>(batch_frames)) { break; }
	}
}
