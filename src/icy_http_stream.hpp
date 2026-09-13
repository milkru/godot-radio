#pragma once

#include "godot_cpp/classes/global_constants.hpp"
#include "godot_cpp/classes/http_client.hpp"
#include "godot_cpp/classes/ref_counted.hpp"
#include "godot_cpp/classes/thread.hpp"
#include "godot_cpp/classes/tls_options.hpp"
#include "godot_cpp/core/binder_common.hpp"
#include "godot_cpp/core/defs.hpp"
#include "godot_cpp/templates/safe_refcount.hpp"
#include "godot_cpp/variant/dictionary.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include "godot_cpp/variant/string.hpp"
#include "icy_audio_decoder.hpp"
#include "icy_metadata_demuxer.hpp"
#include <cstdint>
#include <mutex>

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
		RESULT_REDIRECT_FAILED,
		RESULT_UNSUPPORTED_FORMAT
	};

private:
	static constexpr int POLL_INTERVAL_USEC = 10000;
	static constexpr int READ_CHUNK_SIZE = 16384;
	static constexpr int MAX_REDIRECTIONS = 8;
	static constexpr int DEFAULT_HTTP_PORT = 80;
	static constexpr int DEFAULT_HTTPS_PORT = 443;
	static constexpr double MSEC_PER_SEC = 1000.0;
	static constexpr real_t DEFAULT_CONNECT_TIMEOUT = 5.0;
	static constexpr real_t DEFAULT_STALL_TIMEOUT = 10.0;

	enum class PollStep {
		KEEP_POLLING,
		STOP
	};

	struct ParsedUrl {
		String host;
		String request_string;
		int port = 0;
		bool use_tls = false;
		Error error = OK;
	};

	mutable std::mutex decoder_mutex;
	std::mutex client_mutex;
	mutable std::mutex response_state_mutex;

	Ref<HTTPClient> client;
	Ref<TLSOptions> tls_options;
	Ref<Thread> thread;
	// guarded by decoder_mutex. The stream creates the decoder itself from the content type unless the caller
	// supplied one through set_audio_decoder()
	Ref<IcyAudioDecoder> audio_decoder;
	bool is_decoder_user_supplied = false;

	// guarded by response_state_mutex; read by the public getters on the main thread
	PackedStringArray response_headers;
	String content_type;
	int response_code = 0;

	SafeNumeric<int> downloaded;
	SafeFlag thread_request_quit;
	SafeFlag result_emitted;
	SafeFlag requesting;

	// main thread only
	real_t stall_timeout = DEFAULT_STALL_TIMEOUT;

	// written by request() before the worker starts, then touched by the worker thread only
	String request_string;
	String url;
	PackedStringArray request_headers;
	IcyMetadataDemuxer icy_demuxer;

	// the connect timeout applies until the response headers are in (redirects included), then the stall timeout
	// takes over and is restarted by every body chunk; 0 disables the one it is copied into
	uint64_t timeout_msec = 0;
	uint64_t timeout_start_msec = 0;
	uint64_t stall_timeout_msec = 0;

	int redirections = 0;
	int port = 0;
	bool use_tls = false;
	bool parse_icy_metadata = false;
	bool request_sent = false;

	// all of these run on the worker thread. Signals are routed through the message queue;
	// audio data goes straight into the (thread-safe) decoder without deferring.
	void _thread_func();
	PollStep _update_connection();
	void _process_chunk(const PackedByteArray &p_chunk);
	void _forward_data_to_decoder(const uint8_t *p_data, int p_size);
	void _emit_connected_deferred();
	void _emit_metadata_deferred(const uint8_t *p_data, int p_size);
	// also called from cancel_request() on the main thread, after the worker has been joined
	void _finish_with_result(int p_status);

	// caller must hold client_mutex
	Error _connect_socket();
	// caller must hold client_mutex; called only while response_code == 0
	PollStep _handle_response();
	// creates the decoder for p_content_type unless the caller supplied one; STOP if no decoder is registered
	PollStep _attach_decoder(const String &p_content_type, int p_bitrate_kbps);
	// caller must hold client_mutex
	PollStep _follow_redirect(const PackedStringArray &p_headers);

	static ParsedUrl _parse_url(const String &p_url);
	void _apply_parsed_url(const ParsedUrl &p_parsed);
	void _reset_icy_state();

protected:
	static void _bind_methods();

public:
	IcyHttpStream();
	~IcyHttpStream();

	Error request(const String &p_url, real_t p_connect_timeout = DEFAULT_CONNECT_TIMEOUT, bool p_parse_metadata = false, const PackedStringArray &p_custom_headers = PackedStringArray());
	void cancel_request();
	bool is_requesting() const;
	// read by request(); takes effect on the next request
	void set_stall_timeout(real_t p_seconds);
	real_t get_stall_timeout() const;
	String get_content_mime_type() const;
	int get_response_code() const;
	// optional: a decoder supplied here (before request()) is used as is, for every connection, and the stream
	// does not apply icy-br to it. Pass null to go back to letting the stream create one per connection
	void set_audio_decoder(const Ref<IcyAudioDecoder> p_decoder);
	// the decoder receiving this connection's data; created by the time connection_opened is emitted
	Ref<IcyAudioDecoder> get_audio_decoder() const;
	PackedStringArray get_response_headers() const;
	Dictionary get_response_headers_as_dictionary() const;
	int get_downloaded_bytes() const;
};

} //namespace godot

VARIANT_ENUM_CAST(IcyHttpStream::Result);
