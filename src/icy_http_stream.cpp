#include "icy_http_stream.hpp"
#include "godot_cpp/classes/global_constants.hpp"
#include "godot_cpp/classes/http_client.hpp"
#include "godot_cpp/classes/os.hpp"
#include "godot_cpp/classes/time.hpp"
#include "godot_cpp/classes/tls_options.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include "godot_cpp/variant/callable_method_pointer.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include <cstdint>
#include <mutex>

using namespace godot;

IcyHttpStream::IcyHttpStream() {
	client.instantiate();
	client->set_read_chunk_size(READ_CHUNK_SIZE);
	tls_options = TLSOptions::client();
	thread.instantiate();
}

IcyHttpStream::~IcyHttpStream() {
	thread_request_quit.set();
	{
		std::lock_guard<std::mutex> lock(client_mutex);
		client->close();
	}
	if (thread.is_valid() && thread->is_started()) {
		thread->wait_to_finish();
	}
}

void IcyHttpStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("request", "url", "timeout", "parse_metadata", "custom_headers"), &IcyHttpStream::request, DEFVAL(5.0), DEFVAL(false), DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("cancel_request"), &IcyHttpStream::cancel_request);
	ClassDB::bind_method(D_METHOD("is_requesting"), &IcyHttpStream::is_requesting);
	ClassDB::bind_method(D_METHOD("get_content_mime_type"), &IcyHttpStream::get_content_mime_type);
	ClassDB::bind_method(D_METHOD("get_response_code"), &IcyHttpStream::get_response_code);
	ClassDB::bind_method(D_METHOD("get_response_headers"), &IcyHttpStream::get_response_headers);
	ClassDB::bind_method(D_METHOD("get_response_headers_as_dictionary"), &IcyHttpStream::get_response_headers_as_dictionary);
	ClassDB::bind_method(D_METHOD("get_downloaded_bytes"), &IcyHttpStream::get_downloaded_bytes);
	ClassDB::bind_method(D_METHOD("set_audio_decoder", "audio_decoder"), &IcyHttpStream::set_audio_decoder);

	ADD_SIGNAL(MethodInfo("connection_opened"));
	ADD_SIGNAL(MethodInfo("connection_closed", PropertyInfo(Variant::INT, "result")));
	ADD_SIGNAL(MethodInfo("metadata_received", PropertyInfo(Variant::STRING, "metadata")));

	BIND_ENUM_CONSTANT(RESULT_STREAM_FINISHED);
	BIND_ENUM_CONSTANT(RESULT_CANT_CONNECT);
	BIND_ENUM_CONSTANT(RESULT_CANT_RESOLVE);
	BIND_ENUM_CONSTANT(RESULT_CONNECTION_ERROR);
	BIND_ENUM_CONSTANT(RESULT_TIMEOUT);
	BIND_ENUM_CONSTANT(RESULT_REQUEST_FAILED);
	BIND_ENUM_CONSTANT(RESULT_REDIRECT_LIMIT_REACHED);
	BIND_ENUM_CONSTANT(RESULT_DISCONNECTED);
	BIND_ENUM_CONSTANT(RESULT_CANCELLED);
	BIND_ENUM_CONSTANT(RESULT_REDIRECT_FAILED);
}

IcyHttpStream::ParsedUrl IcyHttpStream::_parse_url(const String &p_url) {
	ParsedUrl result;

	String scheme;
	String fragment;
	String parsed_url = p_url;

	int frag_idx = parsed_url.find("#");
	if (frag_idx != -1) {
		fragment = parsed_url.substr(frag_idx + 1);
		parsed_url = parsed_url.substr(0, frag_idx);
	}

	int scheme_idx = parsed_url.find("://");
	if (scheme_idx != -1) {
		scheme = parsed_url.substr(0, scheme_idx + 3);
		parsed_url = parsed_url.substr(scheme_idx + 3);
	} else {
		result.error = ERR_INVALID_PARAMETER;
		return result;
	}

	int path_idx = parsed_url.find("/");
	if (path_idx != -1) {
		result.request_string = parsed_url.substr(path_idx);
		parsed_url = parsed_url.substr(0, path_idx);
	} else {
		result.request_string = "/";
	}

	int port_idx = parsed_url.rfind(":");
	if (port_idx != -1) {
		// check for IPv6 brackets to ensure a colon from inside an IPv6 address won't be detected
		int bracket_idx = parsed_url.rfind("]");
		if (bracket_idx == -1 || port_idx > bracket_idx) {
			result.port = parsed_url.substr(port_idx + 1).to_int();
			result.host = parsed_url.substr(0, port_idx);
		} else {
			result.host = parsed_url;
		}
	} else {
		result.host = parsed_url;
	}

	if (scheme == "https://") {
		result.use_tls = true;
	} else if (scheme != "http://") {
		result.error = ERR_INVALID_PARAMETER;
		return result;
	}

	if (result.port == 0) {
		result.port = result.use_tls ? DEFAULT_HTTPS_PORT : DEFAULT_HTTP_PORT;
	}

	return result;
}

void IcyHttpStream::_apply_parsed_url(const ParsedUrl &p_parsed) {
	url = p_parsed.host;
	port = p_parsed.port;
	use_tls = p_parsed.use_tls;
	request_string = p_parsed.request_string;
}

Error IcyHttpStream::_connect_socket() {
	return client->connect_to_host(url, port, use_tls ? tls_options : nullptr);
}

void IcyHttpStream::_reset_icy_state() {
	icy_demuxer.reset(0);
}

Error IcyHttpStream::request(const String &p_url, real_t p_timeout, bool p_parse_metadata, const PackedStringArray &p_custom_headers) {
	if (requesting.is_set()) { return ERR_BUSY; }

	ParsedUrl parsed = _parse_url(p_url);
	if (parsed.error != OK) { return parsed.error; }

	if (thread->is_started()) {
		thread->wait_to_finish();
	}

	_apply_parsed_url(parsed);

	request_sent.clear();
	redirections = 0;
	downloaded.set(0);
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		content_type = "";
		response_code = 0;
		response_headers.clear();
	}

	parse_icy_metadata = p_parse_metadata;
	timeout_msec = static_cast<uint64_t>(p_timeout * MSEC_PER_SEC);
	timeout_start_msec = Time::get_singleton()->get_ticks_msec();

	request_headers.clear();
	request_headers.push_back("Connection: close");

	if (p_parse_metadata) {
		request_headers.push_back("Icy-MetaData: 1");
	}
	request_headers.append_array(p_custom_headers);

	_reset_icy_state();

	requesting.set();
	thread_request_quit.clear();
	result_emitted.clear();

	Error err = thread->start(callable_mp(this, &IcyHttpStream::_thread_func));
	if (err != OK) {
		requesting.clear();
	}
	ERR_FAIL_COND_V_MSG(err != OK, err, "IcyHttpStream: could not start the network thread.");

	return OK;
}

void IcyHttpStream::cancel_request() {
	if (!is_requesting()) { return; }

	thread_request_quit.set();
	{
		std::lock_guard<std::mutex> lock(client_mutex);
		client->close();
	}

	if (thread.is_valid() && thread->is_started()) {
		thread->wait_to_finish();
	}

	requesting.clear();
	request_sent.clear();
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		response_code = 0;
	}

	if (!result_emitted.is_set()) {
		_finish_with_result(RESULT_CANCELLED);
	}
}

bool IcyHttpStream::is_requesting() const {
	return requesting.is_set();
}

String IcyHttpStream::get_content_mime_type() const {
	std::lock_guard<std::mutex> lock(response_state_mutex);
	return content_type;
}

int IcyHttpStream::get_response_code() const {
	std::lock_guard<std::mutex> lock(response_state_mutex);
	return response_code;
}

void IcyHttpStream::set_audio_decoder(const Ref<IcyAudioDecoder> p_decoder) {
	std::lock_guard<std::mutex> lock(decoder_mutex);
	audio_decoder = p_decoder;
}

PackedStringArray IcyHttpStream::get_response_headers() const {
	std::lock_guard<std::mutex> lock(response_state_mutex);
	return response_headers;
}

int IcyHttpStream::get_downloaded_bytes() const {
	return downloaded.get();
}

Dictionary IcyHttpStream::get_response_headers_as_dictionary() const {
	std::lock_guard<std::mutex> lock(response_state_mutex);
	Dictionary dict;

	for (int i = 0; i < response_headers.size(); ++i) {
		int sp = response_headers[i].find(":");
		if (sp == -1) { continue; }
		String key = response_headers[i].substr(0, sp).strip_edges().to_lower();
		String value = response_headers[i].substr(sp + 1).strip_edges();
		dict[key] = value;
	}

	return dict;
}

void IcyHttpStream::_thread_func() {
	Error err;
	{
		std::lock_guard<std::mutex> lock(client_mutex);
		err = _connect_socket();
	}

	if (err != OK) {
		_finish_with_result(RESULT_CANT_CONNECT);
	} else {
		while (!thread_request_quit.is_set()) {
			if (timeout_msec > 0) {
				uint64_t current_ticks = Time::get_singleton()->get_ticks_msec();
				if (current_ticks - timeout_start_msec > timeout_msec) {
					_finish_with_result(RESULT_TIMEOUT);
					break;
				}
			}

			if (_update_connection() == PollStep::STOP) { break; }
			OS::get_singleton()->delay_usec(POLL_INTERVAL_USEC);
		}
	}

	requesting.clear();
}

IcyHttpStream::PollStep IcyHttpStream::_update_connection() {
	PackedByteArray chunk;
	{
		std::lock_guard<std::mutex> lock(client_mutex);
		HTTPClient::Status current_status = client->get_status();

		switch(current_status) {
			case HTTPClient::STATUS_DISCONNECTED: {
				_finish_with_result(RESULT_DISCONNECTED);
				return PollStep::STOP;
			}
			case HTTPClient::STATUS_RESOLVING:
			case HTTPClient::STATUS_CONNECTING:
			case HTTPClient::STATUS_REQUESTING: {
				client->poll();
				return PollStep::KEEP_POLLING;
			}
			case HTTPClient::STATUS_CANT_RESOLVE: {
				_finish_with_result(RESULT_CANT_RESOLVE);
				return PollStep::STOP;
			}
			case HTTPClient::STATUS_CANT_CONNECT: {
				_finish_with_result(RESULT_CANT_CONNECT);
				return PollStep::STOP;
			}
			case HTTPClient::STATUS_CONNECTED: {
				if (request_sent.is_set()) {
					// reached end of chunked transfer or standard stream (unlikely for icecast unless it disconnects)
					_finish_with_result(RESULT_STREAM_FINISHED);
					return PollStep::STOP;
				}

				Error err = client->request(HTTPClient::METHOD_GET, request_string, request_headers);
				if (err != OK) {
					_finish_with_result(RESULT_CONNECTION_ERROR);
					return PollStep::STOP;
				}

				request_sent.set();
				return PollStep::KEEP_POLLING;
			}
			case HTTPClient::STATUS_BODY: {
				bool response_pending;
				{
					std::lock_guard<std::mutex> lock(response_state_mutex);
					response_pending = response_code == 0;
				}
				if (request_sent.is_set() && response_pending) {
					if (_handle_response() == PollStep::STOP) { return PollStep::STOP; }
				}

				client->poll();
				if (client->get_status() != HTTPClient::STATUS_BODY) {
					return PollStep::KEEP_POLLING;
				}

				chunk = client->read_response_body_chunk();
				break;
			}
			case HTTPClient::STATUS_CONNECTION_ERROR:
			case HTTPClient::STATUS_TLS_HANDSHAKE_ERROR: {
				_finish_with_result(RESULT_CONNECTION_ERROR);
				return PollStep::STOP;
			}
		}
	}

	if (chunk.size() > 0) {
		downloaded.add(chunk.size());
		timeout_start_msec = Time::get_singleton()->get_ticks_msec();
		_process_chunk(chunk);
	}

	return PollStep::KEEP_POLLING;
}

IcyHttpStream::PollStep IcyHttpStream::_handle_response() {
	if (!client->has_response()) {
		_finish_with_result(RESULT_REQUEST_FAILED);
		return PollStep::STOP;
	}

	PackedStringArray new_headers = client->get_response_headers();
	int new_response_code = client->get_response_code();
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		response_headers = new_headers;
		response_code = new_response_code;
	}

	const bool is_redirect = new_response_code == 301 || new_response_code == 302 || new_response_code == 303
			|| new_response_code == 307 || new_response_code == 308;
	if (is_redirect) {
		return _follow_redirect(new_headers);
	}

	if (new_response_code < 200 || new_response_code >= 300) {
		_finish_with_result(RESULT_REQUEST_FAILED);
		return PollStep::STOP;
	}

	bool found_content_type = false;
	bool found_icy_metaint = !parse_icy_metadata;

	for (int i = 0; i < new_headers.size(); ++i) {
		String lower_header = new_headers[i].to_lower();

		if (!found_content_type && lower_header.begins_with("content-type:")) {
			PackedStringArray parts = new_headers[i].split(":", false, 1);
			if (parts.size() > 1) {
				std::lock_guard<std::mutex> lock(response_state_mutex);
				content_type = parts[1].strip_edges();
			}
			found_content_type = true;
		}
		else if (!found_icy_metaint && lower_header.begins_with("icy-metaint:")) {
			PackedStringArray parts = new_headers[i].split(":", false, 1);
			if (parts.size() > 1) {
				icy_demuxer.reset(parts[1].to_int());
			}
			found_icy_metaint = true;
		}

		if (found_content_type && found_icy_metaint) { break; }
	}

	_emit_connected_deferred();

	return PollStep::KEEP_POLLING;
}

// KEEP_POLLING once the follow-up request is in flight, STOP if a terminal result was emitted
IcyHttpStream::PollStep IcyHttpStream::_follow_redirect(const PackedStringArray &p_headers) {
	if (redirections >= MAX_REDIRECTIONS) {
		_finish_with_result(RESULT_REDIRECT_LIMIT_REACHED);
		return PollStep::STOP;
	}

	const String location_prefix = "location: ";
	String new_request;
	for (int i = 0; i < p_headers.size(); ++i) {
		if (p_headers[i].to_lower().begins_with(location_prefix)) {
			new_request = p_headers[i].substr(location_prefix.length()).strip_edges();
		}
	}

	if (new_request.is_empty()) {
		_finish_with_result(RESULT_REDIRECT_FAILED);
		return PollStep::STOP;
	}

	if (new_request.begins_with("http")) {
		ParsedUrl parsed = _parse_url(new_request);
		if (parsed.error != OK) {
			_finish_with_result(RESULT_REDIRECT_FAILED);
			return PollStep::STOP;
		}
		_apply_parsed_url(parsed);
	} else {
		request_string = new_request;
	}

	client->close();
	Error err = _connect_socket();
	if (err != OK) {
		_finish_with_result(RESULT_REDIRECT_FAILED);
		return PollStep::STOP;
	}

	request_sent.clear();
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		response_code = 0;
		content_type = "";
	}
	downloaded.set(0);
	++redirections;
	_reset_icy_state();

	return PollStep::KEEP_POLLING;
}

// the demuxer hands out views into p_chunk; nothing is sliced or copied before reaching the ring
void IcyHttpStream::_process_chunk(const PackedByteArray &p_chunk) {
	icy_demuxer.feed(p_chunk.ptr(), p_chunk.size(),
			[this](const uint8_t *p_data, int p_size) { _forward_data_to_decoder(p_data, p_size); },
			[this](const uint8_t *p_data, int p_size) { _emit_metadata_deferred(p_data, p_size); });
}

void IcyHttpStream::_emit_connected_deferred() {
	call_deferred("emit_signal", "connection_opened");
}

// emits connection_closed on the main thread and asks the worker loop to exit
void IcyHttpStream::_finish_with_result(int p_status) {
	result_emitted.set();
	thread_request_quit.set();
	// cleared before the signal is queued so a connection_closed handler can call request() again
	// without hitting ERR_BUSY; request() joins the still-running worker via wait_to_finish()
	requesting.clear();
	call_deferred("emit_signal", "connection_closed", p_status);
}

// not deferred on purpose: the decoder is thread-safe and audio data must not wait a frame in the message queue
void IcyHttpStream::_forward_data_to_decoder(const uint8_t *p_data, int p_size) {
	std::lock_guard<std::mutex> lock(decoder_mutex);

	if (audio_decoder.is_valid()) {
		audio_decoder->push_encoded_data(p_data, static_cast<size_t>(p_size));
	}
}

// string decoding and cleanup happen here on the worker thread, only the emit is deferred
void IcyHttpStream::_emit_metadata_deferred(const uint8_t *p_data, int p_size) {
	String meta_str;
	if (meta_str.parse_utf8(reinterpret_cast<const char *>(p_data), p_size) != OK) {
		// some SHOUTcast v1 servers send Latin-1; every byte maps to the code point of the same value
		meta_str = String();
		for (int i = 0; i < p_size; ++i) {
			meta_str += String::chr(p_data[i]);
		}
	}
	// icecast pads metadata with null bytes, strip before emitting
	String clean_meta = meta_str.strip_edges().replace(String::chr(0), "");
	if (!clean_meta.is_empty()) {
		call_deferred("emit_signal", "metadata_received", clean_meta);
	}
}
