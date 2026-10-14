#include "radio_stream.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/global_constants.hpp"
#include "godot_cpp/classes/ip.hpp"
#include "godot_cpp/classes/os.hpp"
#include "godot_cpp/classes/time.hpp"
#include "godot_cpp/classes/tls_options.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include "godot_cpp/variant/callable_method_pointer.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include <cstdint>
#include <cstring>
#include <mutex>

using namespace godot;

// strict, so it rejects everything the UTF-8 parser would log an error for
static bool is_valid_utf8(const uint8_t *p_data, int p_size) {
	for (int i = 0; i < p_size;) {
		const uint8_t lead = p_data[i];
		if (lead < 0x80) {
			++i;
			continue;
		}
		// overlong forms, surrogates and code points past U+10FFFF are ruled out by the first continuation byte
		int extra = 0;
		uint8_t low = 0x80;
		uint8_t high = 0xBF;
		if (lead >= 0xC2 && lead <= 0xDF) {
			extra = 1;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			extra = 2;
			low = lead == 0xE0 ? 0xA0 : 0x80;
			high = lead == 0xED ? 0x9F : 0xBF;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			extra = 3;
			low = lead == 0xF0 ? 0x90 : 0x80;
			high = lead == 0xF4 ? 0x8F : 0xBF;
		} else {
			return false;
		}
		if (i + extra >= p_size || p_data[i + 1] < low || p_data[i + 1] > high) { return false; }
		for (int k = 2; k <= extra; ++k) {
			if ((p_data[i + k] & 0xC0) != 0x80) { return false; }
		}
		i += extra + 1;
	}
	return true;
}

// UTF-8, or Latin-1 as some SHOUTcast servers send, where every byte is the code point of the same value.
// Checked up front because the UTF-8 parser logs an error for anything else
static String decode_text(const uint8_t *p_data, int p_size) {
	String text;
	if (is_valid_utf8(p_data, p_size)) {
		text.parse_utf8(reinterpret_cast<const char *>(p_data), p_size);
		return text;
	}
	for (int i = 0; i < p_size; ++i) {
		text += String::chr(p_data[i]);
	}
	return text;
}

RadioStream::RadioStream() {
	tls_options = TLSOptions::client();
	thread.instantiate();
}

RadioStream::~RadioStream() {
	thread_request_quit.set();
	{
		std::lock_guard<std::mutex> lock(client_mutex);
		_close_socket();
	}
	if (thread.is_valid() && thread->is_started()) {
		thread->wait_to_finish();
	}
}

void RadioStream::_bind_methods() {
	ClassDB::bind_method(D_METHOD("request", "url", "connect_timeout", "parse_metadata", "custom_headers"), &RadioStream::request, DEFVAL(DEFAULT_CONNECT_TIMEOUT), DEFVAL(false), DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("cancel_request"), &RadioStream::cancel_request);
	ClassDB::bind_method(D_METHOD("is_requesting"), &RadioStream::is_requesting);
	ClassDB::bind_method(D_METHOD("set_stall_timeout", "seconds"), &RadioStream::set_stall_timeout);
	ClassDB::bind_method(D_METHOD("get_stall_timeout"), &RadioStream::get_stall_timeout);
	ClassDB::bind_method(D_METHOD("get_content_mime_type"), &RadioStream::get_content_mime_type);
	ClassDB::bind_method(D_METHOD("get_response_code"), &RadioStream::get_response_code);
	ClassDB::bind_method(D_METHOD("get_response_headers"), &RadioStream::get_response_headers);
	ClassDB::bind_method(D_METHOD("get_response_headers_as_dictionary"), &RadioStream::get_response_headers_as_dictionary);
	ClassDB::bind_method(D_METHOD("get_downloaded_bytes"), &RadioStream::get_downloaded_bytes);
	ClassDB::bind_method(D_METHOD("set_audio_decoder", "audio_decoder"), &RadioStream::set_audio_decoder);
	ClassDB::bind_method(D_METHOD("get_audio_decoder"), &RadioStream::get_audio_decoder);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stall_timeout", PROPERTY_HINT_RANGE, "0,60,0.1,or_greater,suffix:s"), "set_stall_timeout", "get_stall_timeout");

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
	BIND_ENUM_CONSTANT(RESULT_UNSUPPORTED_FORMAT);
}

RadioStream::ParsedUrl RadioStream::_parse_url(const String &p_url) {
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

void RadioStream::_apply_parsed_url(const ParsedUrl &p_parsed) {
	url = p_parsed.host;
	port = p_parsed.port;
	use_tls = p_parsed.use_tls;
	request_string = p_parsed.request_string;
}

// starts resolving the host; _update_connection() connects once the address is in
Error RadioStream::_connect_socket() {
	_close_socket();
	resolve_id = IP::get_singleton()->resolve_hostname_queue_item(url);
	return resolve_id == IP::RESOLVER_INVALID_ID ? ERR_CANT_RESOLVE : OK;
}

RadioStream::Result RadioStream::_closed_result(Error p_read_error) const {
	std::lock_guard<std::mutex> lock(response_state_mutex);
	if (response_code == 0) { return RESULT_REQUEST_FAILED; }
	// end of file is the server closing, how a live stream ends; anything else is a broken connection
	return p_read_error == ERR_FILE_EOF ? RESULT_DISCONNECTED : RESULT_CONNECTION_ERROR;
}

bool RadioStream::_connect_next_address() {
	while (!addresses.is_empty()) {
		const String address = addresses.pop_front();
		tcp.instantiate();
		if (tcp->connect_to_host(address, port) == OK) { return true; }
	}
	return false;
}

void RadioStream::_close_socket() {
	if (resolve_id != IP::RESOLVER_INVALID_ID) {
		IP::get_singleton()->erase_resolve_item(resolve_id);
		resolve_id = IP::RESOLVER_INVALID_ID;
	}
	if (tls.is_valid()) {
		tls->disconnect_from_stream();
		tls.unref();
	}
	if (tcp.is_valid()) {
		tcp->disconnect_from_host();
		tcp.unref();
	}
	header_bytes.clear();
	addresses.clear();
}

void RadioStream::_reset_metadata_state() {
	metadata_demuxer.reset(0);
}

Error RadioStream::request(const String &p_url, real_t p_connect_timeout, bool p_parse_metadata, const PackedStringArray &p_custom_headers) {
	if (requesting.is_set()) { return ERR_BUSY; }

	ParsedUrl parsed = _parse_url(p_url);
	if (parsed.error != OK) { return parsed.error; }

	if (thread->is_started()) {
		thread->wait_to_finish();
	}

	_apply_parsed_url(parsed);

	request_sent = false;
	redirections = 0;
	body_left = -1;
	downloaded.set(0);
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		content_type = "";
		response_code = 0;
		response_headers.clear();
	}

	parse_stream_metadata = p_parse_metadata;
	timeout_msec = static_cast<uint64_t>(p_connect_timeout * MSEC_PER_SEC);
	stall_timeout_msec = static_cast<uint64_t>(stall_timeout * MSEC_PER_SEC);
	timeout_start_msec = Time::get_singleton()->get_ticks_msec();

	request_headers.clear();
	request_headers.push_back("Connection: close");
	// the defaults HTTPClient would send, unless the caller sets them
	bool has_user_agent = false;
	bool has_accept = false;
	for (const String &header : p_custom_headers) {
		has_user_agent = has_user_agent || header.to_lower().begins_with("user-agent:");
		has_accept = has_accept || header.to_lower().begins_with("accept:");
	}
	if (!has_user_agent) {
		// major.minor[.patch].status.build, like the engine's full build version
		const Dictionary info = Engine::get_singleton()->get_version_info();
		String version = String::num_int64(info["major"]) + "." + String::num_int64(info["minor"]);
		if (static_cast<int>(info["patch"]) != 0) {
			version += "." + String::num_int64(info["patch"]);
		}
		version += "." + String(info["status"]) + "." + String(info["build"]);
		request_headers.push_back("User-Agent: GodotEngine/" + version + " (" + OS::get_singleton()->get_name() + ")");
	}
	if (!has_accept) {
		request_headers.push_back("Accept: */*");
	}

	if (p_parse_metadata) {
		request_headers.push_back("Icy-MetaData: 1");
	}
	request_headers.append_array(p_custom_headers);

	_reset_metadata_state();

	requesting.set();
	thread_request_quit.clear();
	result_emitted.clear();

	Error err = thread->start(callable_mp(this, &RadioStream::_thread_func));
	if (err != OK) {
		requesting.clear();
	}
	ERR_FAIL_COND_V_MSG(err != OK, err, "RadioStream: could not start the network thread.");

	return OK;
}

void RadioStream::cancel_request() {
	if (!is_requesting()) { return; }

	thread_request_quit.set();
	{
		std::lock_guard<std::mutex> lock(client_mutex);
		_close_socket();
	}

	if (thread.is_valid() && thread->is_started()) {
		thread->wait_to_finish();
	}

	requesting.clear();
	request_sent = false;
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		response_code = 0;
	}

	if (!result_emitted.is_set()) {
		_finish_with_result(RESULT_CANCELLED);
	}
}

bool RadioStream::is_requesting() const {
	return requesting.is_set();
}

void RadioStream::set_stall_timeout(real_t p_seconds) {
	stall_timeout = MAX(p_seconds, static_cast<real_t>(0));
}

real_t RadioStream::get_stall_timeout() const {
	return stall_timeout;
}

String RadioStream::get_content_mime_type() const {
	std::lock_guard<std::mutex> lock(response_state_mutex);
	return content_type;
}

int RadioStream::get_response_code() const {
	std::lock_guard<std::mutex> lock(response_state_mutex);
	return response_code;
}

void RadioStream::set_audio_decoder(const Ref<RadioDecoder> p_decoder) {
	std::lock_guard<std::mutex> lock(decoder_mutex);
	audio_decoder = p_decoder;
	is_decoder_user_supplied = p_decoder.is_valid();
}

Ref<RadioDecoder> RadioStream::get_audio_decoder() const {
	std::lock_guard<std::mutex> lock(decoder_mutex);
	return audio_decoder;
}

PackedStringArray RadioStream::get_response_headers() const {
	std::lock_guard<std::mutex> lock(response_state_mutex);
	return response_headers;
}

int RadioStream::get_downloaded_bytes() const {
	return downloaded.get();
}

Dictionary RadioStream::get_response_headers_as_dictionary() const {
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

void RadioStream::_thread_func() {
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

RadioStream::PollStep RadioStream::_update_connection() {
	PackedByteArray chunk;
	Error read_error = OK;
	{
		std::lock_guard<std::mutex> lock(client_mutex);
		// cancel_request() closes the socket while this thread waits for the lock
		if (thread_request_quit.is_set()) { return PollStep::STOP; }
		if (resolve_id != IP::RESOLVER_INVALID_ID) {
			IP *ip = IP::get_singleton();
			IP::ResolverStatus resolve_status = ip->get_resolve_item_status(resolve_id);
			if (resolve_status == IP::RESOLVER_STATUS_WAITING) { return PollStep::KEEP_POLLING; }

			addresses = resolve_status == IP::RESOLVER_STATUS_DONE ? ip->get_resolve_item_addresses(resolve_id) : Array();
			ip->erase_resolve_item(resolve_id);
			resolve_id = IP::RESOLVER_INVALID_ID;
			if (addresses.is_empty()) {
				_finish_with_result(RESULT_CANT_RESOLVE);
				return PollStep::STOP;
			}
			if (!_connect_next_address()) {
				_finish_with_result(RESULT_CANT_CONNECT);
				return PollStep::STOP;
			}
		}

		if (tcp->get_status() == StreamPeerTCP::STATUS_CONNECTING) {
			tcp->poll();
		}
		StreamPeerTCP::Status tcp_status = tcp->get_status();
		if (tcp_status == StreamPeerTCP::STATUS_CONNECTING) { return PollStep::KEEP_POLLING; }
		if (tcp_status == StreamPeerTCP::STATUS_ERROR) {
			// like HTTPClient, a host whose first address can't be reached is tried at the next one
			if (!request_sent && tls.is_null() && _connect_next_address()) { return PollStep::KEEP_POLLING; }
			_finish_with_result(request_sent || tls.is_valid() ? RESULT_CONNECTION_ERROR : RESULT_CANT_CONNECT);
			return PollStep::STOP;
		}

		Ref<StreamPeer> peer = tcp;
		if (use_tls) {
			if (tls.is_null()) {
				tls.instantiate();
				if (tls.is_null() || tls->connect_to_stream(tcp, url, tls_options) != OK) {
					_finish_with_result(RESULT_CONNECTION_ERROR);
					return PollStep::STOP;
				}
			}
			// polled only during the handshake: a poll that finds the server gone drops what it already decrypted,
			// and stations close right after short answers like redirects. Reads drain that data first
			if (tls->get_status() == StreamPeerTLS::STATUS_HANDSHAKING) {
				tls->poll();
			}
			StreamPeerTLS::Status tls_status = tls->get_status();
			if (tls_status == StreamPeerTLS::STATUS_HANDSHAKING) { return PollStep::KEEP_POLLING; }
			if (tls_status != StreamPeerTLS::STATUS_CONNECTED) {
				_finish_with_result(request_sent ? _closed_result(ERR_FILE_EOF) : RESULT_CONNECTION_ERROR);
				return PollStep::STOP;
			}
			peer = tls;
		} else if (tcp_status != StreamPeerTCP::STATUS_CONNECTED) {
			_finish_with_result(_closed_result(ERR_FILE_EOF));
			return PollStep::STOP;
		}

		if (!request_sent) {
			// HTTP/1.0, so the server never answers with chunked encoding
			const bool default_port = port == (use_tls ? DEFAULT_HTTPS_PORT : DEFAULT_HTTP_PORT);
			String head = String("GET ") + request_string + " HTTP/1.0\r\n";
			bool has_host = false;
			for (const String &header : request_headers) {
				has_host = has_host || header.to_lower().begins_with("host:");
				head += header;
				head += "\r\n";
			}
			if (!has_host) {
				head += "Host: " + url + (default_port ? String() : String(":") + String::num_int64(port)) + "\r\n";
			}
			if (peer->put_data((head + "\r\n").to_utf8_buffer()) != OK) {
				_finish_with_result(RESULT_CONNECTION_ERROR);
				return PollStep::STOP;
			}
			request_sent = true;
			return PollStep::KEEP_POLLING;
		}

		// reads only what has arrived: a read that runs into the server closing reports an error and drops the
		// data it got, which would lose the last answer of a server that replies and hangs up
		PackedByteArray data;
		while (data.size() < READ_CHUNK_SIZE) {
			const int available = peer->get_available_bytes();
			Array received = peer->get_partial_data(MIN(MAX(available, 1), READ_CHUNK_SIZE - data.size()));
			read_error = static_cast<Error>(static_cast<int>(received[0]));
			if (read_error != OK) { break; }
			PackedByteArray part = received[1];
			if (part.is_empty()) { break; }
			data.append_array(part);
		}
		if (data.is_empty() && read_error == OK) { return PollStep::KEEP_POLLING; }

		if (!data.is_empty()) {
			bool response_pending;
			{
				std::lock_guard<std::mutex> state_lock(response_state_mutex);
				response_pending = response_code == 0;
			}
			if (!response_pending) {
				chunk = data;
			} else if (_read_response_head(data, chunk) == PollStep::STOP) {
				return PollStep::STOP;
			}
		}
		// a redirect in this pass replaced the socket, so the old one closing is expected
		if (tcp.is_null()) { read_error = OK; }
	}

	if (body_left >= 0 && chunk.size() > body_left) {
		chunk.resize(body_left);
	}
	if (chunk.size() > 0) {
		downloaded.add(chunk.size());
		timeout_start_msec = Time::get_singleton()->get_ticks_msec();
		_process_chunk(chunk);
		if (body_left >= 0) {
			body_left -= chunk.size();
		}
	}
	// a body of known length that has all arrived is the only normal end; a live stream ends when dropped
	if (body_left == 0) {
		_finish_with_result(RESULT_STREAM_FINISHED);
		return PollStep::STOP;
	}
	// the data that came with the error is handled first, so a server's last answer still counts
	if (read_error != OK) {
		_finish_with_result(_closed_result(read_error));
		return PollStep::STOP;
	}

	return PollStep::KEEP_POLLING;
}

RadioStream::PollStep RadioStream::_read_response_head(const PackedByteArray &p_data, PackedByteArray &r_body) {
	header_bytes.append_array(p_data);
	const uint8_t *bytes = header_bytes.ptr();
	const int size = header_bytes.size();
	int head_end = -1;
	int body_start = -1;
	for (int i = 0; i + 1 < size && head_end == -1; ++i) {
		if (bytes[i] != '\n') { continue; }
		if (bytes[i + 1] == '\n') {
			head_end = i;
			body_start = i + 2;
		} else if (bytes[i + 1] == '\r' && i + 2 < size && bytes[i + 2] == '\n') {
			head_end = i;
			body_start = i + 3;
		}
	}
	if (head_end == -1) {
		if (size > MAX_HEADER_SIZE) {
			_finish_with_result(RESULT_REQUEST_FAILED);
			return PollStep::STOP;
		}
		return PollStep::KEEP_POLLING;
	}

	PackedStringArray headers = decode_text(bytes, head_end).replace("\r", "").split("\n", false);
	// "HTTP/1.0 200 OK", or "ICY 200 OK" from SHOUTcast v1
	const int code = headers.is_empty() ? 0 : headers[0].get_slicec(' ', 1).to_int();
	if (!headers.is_empty()) {
		headers.remove_at(0);
	}
	PackedByteArray body = header_bytes.slice(body_start);
	header_bytes.clear();

	if (_handle_response(code, headers) == PollStep::STOP) { return PollStep::STOP; }
	if (code >= 200 && code < 300) {
		r_body = body;
	}
	return PollStep::KEEP_POLLING;
}

RadioStream::PollStep RadioStream::_handle_response(int p_response_code, const PackedStringArray &p_headers) {
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		response_headers = p_headers;
		response_code = p_response_code;
	}

	const bool is_redirect = p_response_code == 301 || p_response_code == 302 || p_response_code == 303
			|| p_response_code == 307 || p_response_code == 308;
	if (is_redirect) {
		return _follow_redirect(p_headers);
	}

	if (p_response_code < 200 || p_response_code >= 300) {
		_finish_with_result(RESULT_REQUEST_FAILED);
		return PollStep::STOP;
	}

	String new_content_type;
	int bitrate_kbps = 0;

	for (int i = 0; i < p_headers.size(); ++i) {
		const int sep = p_headers[i].find(":");
		if (sep == -1) { continue; }
		const String key = p_headers[i].substr(0, sep).strip_edges().to_lower();
		const String value = p_headers[i].substr(sep + 1).strip_edges();

		if (key == "content-type") {
			new_content_type = value;
		} else if (key == "icy-metaint" && parse_stream_metadata) {
			metadata_demuxer.reset(value.to_int());
		} else if (key == "icy-br") {
			bitrate_kbps = value.to_int();
		} else if (key == "content-length") {
			body_left = value.to_int();
		}
	}
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		content_type = new_content_type;
	}

	if (_attach_decoder(new_content_type, bitrate_kbps) == PollStep::STOP) { return PollStep::STOP; }

	// headers are in: from here on the timeout watches for a stalled body instead of a slow connect
	timeout_msec = stall_timeout_msec;
	timeout_start_msec = Time::get_singleton()->get_ticks_msec();

	_emit_connected_deferred();

	return PollStep::KEEP_POLLING;
}

// The registry is read-only after init and the new decoder is invisible to everyone until it is published under
// decoder_mutex, so creating it here on the worker thread is safe. The first body byte then lands in a ring that
// exists, instead of being dropped while connection_opened waits in the message queue
RadioStream::PollStep RadioStream::_attach_decoder(const String &p_content_type, int p_bitrate_kbps) {
	{
		std::lock_guard<std::mutex> lock(decoder_mutex);
		if (is_decoder_user_supplied) { return PollStep::KEEP_POLLING; }
	}

	Ref<RadioDecoder> decoder = RadioDecoder::create_for_mime_type(p_content_type);
	if (decoder.is_null()) {
		ERR_PRINT("RadioStream: no decoder registered for content type '" + p_content_type + "'");
		_finish_with_result(RESULT_UNSUPPORTED_FORMAT);
		return PollStep::STOP;
	}
	if (p_bitrate_kbps > 0) {
		decoder->set_bitrate_hint(p_bitrate_kbps);
	}

	std::lock_guard<std::mutex> lock(decoder_mutex);
	audio_decoder = decoder;
	return PollStep::KEEP_POLLING;
}

// KEEP_POLLING once the follow-up request is in flight, STOP if a terminal result was emitted
RadioStream::PollStep RadioStream::_follow_redirect(const PackedStringArray &p_headers) {
	if (redirections >= MAX_REDIRECTIONS) {
		_finish_with_result(RESULT_REDIRECT_LIMIT_REACHED);
		return PollStep::STOP;
	}

	String new_request;
	for (int i = 0; i < p_headers.size(); ++i) {
		if (p_headers[i].to_lower().begins_with("location:")) {
			new_request = p_headers[i].substr(p_headers[i].find(":") + 1).strip_edges();
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

	Error err = _connect_socket();
	if (err != OK) {
		_finish_with_result(RESULT_REDIRECT_FAILED);
		return PollStep::STOP;
	}

	request_sent = false;
	{
		std::lock_guard<std::mutex> lock(response_state_mutex);
		response_code = 0;
		content_type = "";
	}
	downloaded.set(0);
	++redirections;
	body_left = -1;
	_reset_metadata_state();

	return PollStep::KEEP_POLLING;
}

// the demuxer hands out views into p_chunk; nothing is sliced or copied before reaching the ring
void RadioStream::_process_chunk(const PackedByteArray &p_chunk) {
	metadata_demuxer.feed(p_chunk.ptr(), p_chunk.size(),
			[this](const uint8_t *p_data, int p_size) { _forward_data_to_decoder(p_data, p_size); },
			[this](const uint8_t *p_data, int p_size) { _emit_metadata_deferred(p_data, p_size); });
}

void RadioStream::_emit_connected_deferred() {
	call_deferred("emit_signal", "connection_opened");
}

// emits connection_closed on the main thread and asks the worker loop to exit
void RadioStream::_finish_with_result(int p_status) {
	result_emitted.set();
	thread_request_quit.set();
	// cleared before the signal is queued so a connection_closed handler can call request() again
	// without hitting ERR_BUSY; request() joins the still-running worker via wait_to_finish()
	requesting.clear();
	call_deferred("emit_signal", "connection_closed", p_status);
}

// not deferred on purpose: the decoder is thread-safe and audio data must not wait a frame in the message queue
void RadioStream::_forward_data_to_decoder(const uint8_t *p_data, int p_size) {
	std::lock_guard<std::mutex> lock(decoder_mutex);

	if (audio_decoder.is_valid()) {
		audio_decoder->push_encoded_data(p_data, static_cast<size_t>(p_size));
	}
}

// string decoding and cleanup happen here on the worker thread, only the emit is deferred
void RadioStream::_emit_metadata_deferred(const uint8_t *p_data, int p_size) {
	// icecast pads metadata with null bytes. The UTF-8 parser stops at the first, but the Latin-1 fallback would
	// keep them, so they are cut first
	const uint8_t *nul = static_cast<const uint8_t *>(memchr(p_data, 0, p_size));
	if (nul != nullptr) {
		p_size = static_cast<int>(nul - p_data);
	}
	String clean_meta = decode_text(p_data, p_size).strip_edges();
	if (!clean_meta.is_empty()) {
		call_deferred("emit_signal", "metadata_received", clean_meta);
	}
}
