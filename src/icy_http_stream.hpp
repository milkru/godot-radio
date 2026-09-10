#pragma once

#include "godot_cpp/classes/http_client.hpp"
#include "godot_cpp/classes/ref_counted.hpp"
#include "godot_cpp/classes/tls_options.hpp"
#include "godot_cpp/classes/thread.hpp"
#include "godot_cpp/core/binder_common.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include "icy_audio_decoder.hpp"

namespace godot {

class IcyHttpStream : public RefCounted {
	GDCLASS(IcyHttpStream, RefCounted);

public:
	enum Result {
  		RESULT_STREAM_FINISHED,
        RESULT_CANT_CONNECT,
        RESULT_CANT_RESOLVE,
        RESULT_CONNECTION_ERROR,
        RESULT_TIMEOUT,
        RESULT_REQUEST_FAILED,
        RESULT_REDIRECT_LIMIT_REACHED,
        RESULT_DISCONNECTED,
        RESULT_CANCELLED,
        RESULT_REDIRECT_FAILED
	};

private:
	enum MetaState {
		AUDIO,
		LENGTH_BYTE,
		METADATA
	};

	struct ParsedUrl {
		String host;
		String request_string;
		int port = 0;
		bool use_tls = false;
		Error error = OK;
	};

	std::mutex decoder_mutex;
	std::mutex client_mutex;
	mutable std::mutex response_state_mutex;

	Ref<HTTPClient> client;
	Ref<TLSOptions> tls_options;
	Ref<Thread> thread;
	Ref<IcyAudioDecoder> audio_decoder;

	String request_string;
	String url;
	String content_type;
	PackedStringArray request_headers;
	PackedStringArray response_headers;
	PackedByteArray metadata_buffer;

	uint64_t timeout_msec = 0;
	uint64_t timeout_start_msec = 0;

	SafeNumeric<int> downloaded;
	int redirections = 0;
	int max_redirections = 8;
	int port = 0;
	int response_code = 0;

	int icy_metaint = 0;
	int audio_bytes_until_meta = 0;
	int meta_bytes_remaining = 0;
	MetaState meta_state = AUDIO;

	SafeFlag thread_done;
	SafeFlag thread_request_quit;
	SafeFlag result_emitted;
	SafeFlag requesting;
	SafeFlag got_response;
	bool use_tls = false;
	bool parse_icy_metadata = false;

	void _defer_connected();
	void _defer_done(int p_status);
	void _defer_data(const PackedByteArray &p_data);
	void _defer_metadata(const String &p_meta);

	void _thread_func();

	static ParsedUrl _parse_url(const String &p_url);
	void _apply_parsed_url(const ParsedUrl &p_parsed);
	Error _request();
	void _reset_icy_state();
	bool _update_connection();
	bool _handle_response(bool *ret_value);
	bool _follow_redirect(const PackedStringArray &p_headers);
	void _process_chunk(const PackedByteArray &p_chunk);

protected:
	static void _bind_methods();

public:
	IcyHttpStream();
	~IcyHttpStream();

	Error request(const String &p_url, real_t p_timeout = 5.0, bool p_parse_metadata = false, const PackedStringArray &p_custom_headers = PackedStringArray());
	void cancel_request();
	bool is_requesting() const;
	String get_content_mime_type() const;
	int get_response_code() const;
	void set_audio_decoder(const Ref<IcyAudioDecoder> p_decoder);
	PackedStringArray get_response_headers() const;
	Dictionary get_response_headers_as_dictionary() const;
	int get_downloaded_bytes() const;
};

}

VARIANT_ENUM_CAST(IcyHttpStream::Result);
