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
