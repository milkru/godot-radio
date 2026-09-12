#pragma once

#include "godot_cpp/classes/ref.hpp"
#include "icy_audio_decoder.hpp"
#include "thirdparty/dr_mp3.h"
#include <cstddef>

namespace godot {

class IcyMp3AudioDecoder : public IcyAudioDecoder {
	GDCLASS(IcyMp3AudioDecoder, IcyAudioDecoder);

private:
	drmp3 mp3;
	bool is_initialized = false;

	static size_t on_read_callback(void *p_user_data, void *p_buffer_out, size_t bytes_to_read);
	// dr_mp3 allocates its 64 KiB input buffer through these so the memory shows up in the engine monitor
	static void *on_malloc(size_t p_size, void *p_user_data);
	static void *on_realloc(void *p_ptr, size_t p_size, void *p_user_data);
	static void on_free(void *p_ptr, void *p_user_data);

protected:
	static void _bind_methods();

	bool _probe_stream() override;
	int _decode_frames(float *r_interleaved, int p_max_frames) override;
	void _reset_codec() override;

public:
	IcyMp3AudioDecoder();
	~IcyMp3AudioDecoder();

	static Ref<IcyAudioDecoder> create_instance();
};

} //namespace godot
