#pragma once

#include "godot_cpp/core/math.hpp"
#include <cstdint>
#include <cstring>

namespace godot {

class MetadataDemuxer {
public:
	static constexpr int METADATA_BLOCK_SIZE = 16;
	// the protocol encodes the metadata length in one byte x 16, so this is the hard upper bound
	static constexpr int MAX_METADATA_BYTES = 255 * METADATA_BLOCK_SIZE;

private:
	enum State {
		AUDIO,
		LENGTH_BYTE,
		METADATA
	};

	int metaint = 0;
	int audio_bytes_until_meta = 0;
	int meta_bytes_remaining = 0;
	int metadata_len = 0;
	State state = AUDIO;
	uint8_t metadata_buffer[MAX_METADATA_BYTES];

public:
	// p_metaint <= 0 disables demuxing: feed() passes everything through as audio
	void reset(int p_metaint) {
		metaint = p_metaint;
		audio_bytes_until_meta = p_metaint;
		meta_bytes_remaining = 0;
		metadata_len = 0;
		state = AUDIO;
	}

	// p_audio_fn(const uint8_t *, int) receives audio runs, p_meta_fn(const uint8_t *, int) complete metadata blocks
	template <typename AudioFn, typename MetaFn>
	void feed(const uint8_t *p_data, int p_size, AudioFn &&p_audio_fn, MetaFn &&p_meta_fn) {
		if (metaint <= 0) {
			p_audio_fn(p_data, p_size);
			return;
		}

		int offset = 0;
		while (offset < p_size) {
			if (state == AUDIO) {
				int bytes_to_read = MIN(audio_bytes_until_meta, p_size - offset);
				if (bytes_to_read > 0) {
					p_audio_fn(p_data + offset, bytes_to_read);
					offset += bytes_to_read;
					audio_bytes_until_meta -= bytes_to_read;
				}

				if (audio_bytes_until_meta == 0) {
					state = LENGTH_BYTE;
				}
			}
			else if (state == LENGTH_BYTE) {
				meta_bytes_remaining = p_data[offset] * METADATA_BLOCK_SIZE;
				++offset;

				if (meta_bytes_remaining > 0) {
					state = METADATA;
					metadata_len = 0;
				} else {
					state = AUDIO;
					audio_bytes_until_meta = metaint;
				}
			}
			else if (state == METADATA) {
				int bytes_to_read = MIN(meta_bytes_remaining, p_size - offset);
				memcpy(metadata_buffer + metadata_len, p_data + offset, bytes_to_read);
				offset += bytes_to_read;
				metadata_len += bytes_to_read;
				meta_bytes_remaining -= bytes_to_read;

				if (meta_bytes_remaining == 0) {
					p_meta_fn(metadata_buffer, metadata_len);
					state = AUDIO;
					audio_bytes_until_meta = metaint;
				}
			}
		}
	}
};

} //namespace godot
