#include "radio_player.hpp"
#include "home_country.hpp"
#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include "godot_cpp/core/math.hpp"
#include "godot_cpp/variant/callable_method_pointer.hpp"
#include "godot_cpp/variant/variant.hpp"

using namespace godot;

void RadioPlayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("play", "url", "custom_headers"), &RadioPlayer::play, DEFVAL(PackedStringArray()));
	ClassDB::bind_method(D_METHOD("stop"), &RadioPlayer::stop);
	ClassDB::bind_static_method("RadioPlayer", D_METHOD("get_home_country"), &RadioPlayer::get_home_country);
	ClassDB::bind_method(D_METHOD("get_state"), &RadioPlayer::get_state);
	ClassDB::bind_method(D_METHOD("get_stream"), &RadioPlayer::get_stream);
	ClassDB::bind_method(D_METHOD("get_http_stream"), &RadioPlayer::get_http_stream);
	ClassDB::bind_method(D_METHOD("get_audio_decoder"), &RadioPlayer::get_audio_decoder);

	ClassDB::bind_method(D_METHOD("set_player_path", "path"), &RadioPlayer::set_player_path);
	ClassDB::bind_method(D_METHOD("get_player_path"), &RadioPlayer::get_player_path);
	ClassDB::bind_method(D_METHOD("set_target_buffer", "seconds"), &RadioPlayer::set_target_buffer);
	ClassDB::bind_method(D_METHOD("get_target_buffer"), &RadioPlayer::get_target_buffer);
	ClassDB::bind_method(D_METHOD("set_reconnect_delay", "seconds"), &RadioPlayer::set_reconnect_delay);
	ClassDB::bind_method(D_METHOD("get_reconnect_delay"), &RadioPlayer::get_reconnect_delay);
	ClassDB::bind_method(D_METHOD("set_connect_timeout", "seconds"), &RadioPlayer::set_connect_timeout);
	ClassDB::bind_method(D_METHOD("get_connect_timeout"), &RadioPlayer::get_connect_timeout);
	ClassDB::bind_method(D_METHOD("set_stall_timeout", "seconds"), &RadioPlayer::set_stall_timeout);
	ClassDB::bind_method(D_METHOD("get_stall_timeout"), &RadioPlayer::get_stall_timeout);
	ClassDB::bind_method(D_METHOD("set_parse_metadata", "enabled"), &RadioPlayer::set_parse_metadata);
	ClassDB::bind_method(D_METHOD("get_parse_metadata"), &RadioPlayer::get_parse_metadata);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "player_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "AudioStreamPlayer,AudioStreamPlayer2D,AudioStreamPlayer3D"), "set_player_path", "get_player_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "target_buffer", PROPERTY_HINT_RANGE, "0.05,5,0.05,or_greater,suffix:s"), "set_target_buffer", "get_target_buffer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reconnect_delay", PROPERTY_HINT_RANGE, "0,60,0.1,or_greater,suffix:s"), "set_reconnect_delay", "get_reconnect_delay");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "connect_timeout", PROPERTY_HINT_RANGE, "0,60,0.1,or_greater,suffix:s"), "set_connect_timeout", "get_connect_timeout");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stall_timeout", PROPERTY_HINT_RANGE, "0,60,0.1,or_greater,suffix:s"), "set_stall_timeout", "get_stall_timeout");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "parse_metadata"), "set_parse_metadata", "get_parse_metadata");

	ADD_SIGNAL(MethodInfo("state_changed", PropertyInfo(Variant::INT, "state", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM, "RadioPlayer.State")));
	ADD_SIGNAL(MethodInfo("metadata_received", PropertyInfo(Variant::STRING, "title")));
	ADD_SIGNAL(MethodInfo("stream_failed", PropertyInfo(Variant::INT, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM, "RadioStream.Result"), PropertyInfo(Variant::INT, "http_code")));

	BIND_ENUM_CONSTANT(STATE_IDLE);
	BIND_ENUM_CONSTANT(STATE_CONNECTING);
	BIND_ENUM_CONSTANT(STATE_BUFFERING);
	BIND_ENUM_CONSTANT(STATE_PLAYING);
	BIND_ENUM_CONSTANT(STATE_RECONNECTING);
	BIND_ENUM_CONSTANT(STATE_FAILED);
}

RadioPlayer::RadioPlayer() {
	http_stream.instantiate();
	http_stream->connect("connection_opened", callable_mp(this, &RadioPlayer::_on_connection_opened));
	http_stream->connect("connection_closed", callable_mp(this, &RadioPlayer::_on_connection_closed));
	http_stream->connect("metadata_received", callable_mp(this, &RadioPlayer::_on_metadata_received));
}

void RadioPlayer::_ready() {
	set_process(true);
}

void RadioPlayer::_process(double p_delta) {
	if (state == STATE_RECONNECTING) {
		reconnect_countdown -= p_delta;
		if (reconnect_countdown <= 0.0) {
			_start_request();
		}
		return;
	}

	if (decoder.is_valid()) {
		decoder->process_audio();
	}
}

void RadioPlayer::_exit_tree() {
	stop();
}

Error RadioPlayer::play(const String &p_url, const PackedStringArray &p_custom_headers) {
	Node *player = _get_player();
	ERR_FAIL_NULL_V_MSG(player, ERR_UNCONFIGURED, "RadioPlayer: player_path must point to an AudioStreamPlayer, AudioStreamPlayer2D or AudioStreamPlayer3D.");

	stop();

	// an AudioStreamGenerator already on the player is adopted (buffer_length stays as configured in the
	// inspector); otherwise one is created and assigned
	Ref<AudioStreamGenerator> existing = player->get("stream");
	if (existing.is_valid()) {
		stream = existing;
	} else {
		stream.instantiate();
		stream->set_buffer_length(DEFAULT_BUFFER_LENGTH);
		player->set("stream", stream);
	}

	url = p_url;
	custom_headers = p_custom_headers;
	return _start_request();
}

String RadioPlayer::get_home_country() {
	char code[3];
	get_home_country_code(code);
	return String(code);
}

void RadioPlayer::stop() {
	// cancel_request() reports RESULT_CANCELLED through connection_closed, which _on_connection_closed ignores
	http_stream->cancel_request();
	Node *player = _get_player();
	if (player != nullptr) {
		player->call("stop");
	}
	_detach_decoder();
	_set_state(STATE_IDLE);
}

Error RadioPlayer::_start_request() {
	_detach_decoder();
	const Error err = http_stream->request(url, connect_timeout, parse_metadata, custom_headers);
	if (err != OK) {
		_set_state(STATE_FAILED);
		return err;
	}
	_set_state(STATE_CONNECTING);
	return OK;
}

Node *RadioPlayer::_get_player() const {
	Node *node = get_node_or_null(player_path);
	if (node == nullptr || !node->has_method("get_stream_playback")) {
		return nullptr;
	}
	return node;
}

void RadioPlayer::_set_state(State p_state) {
	if (state == p_state) { return; }
	state = p_state;
	emit_signal("state_changed", state);
}

void RadioPlayer::_detach_decoder() {
	if (decoder.is_null()) { return; }
	decoder->disconnect("stream_info_ready", callable_mp(this, &RadioPlayer::_on_stream_info_ready));
	decoder->disconnect("buffering_started", callable_mp(this, &RadioPlayer::_on_buffering_started));
	decoder->disconnect("buffering_finished", callable_mp(this, &RadioPlayer::_on_buffering_finished));
	decoder->disconnect("stream_error", callable_mp(this, &RadioPlayer::_on_stream_error));
	decoder.unref();
}

void RadioPlayer::_fail(RadioStream::Result p_result) {
	Node *player = _get_player();
	if (player != nullptr) {
		player->call("stop");
	}
	_detach_decoder();
	_set_state(STATE_FAILED);
	emit_signal("stream_failed", p_result, http_stream->get_response_code());
}

void RadioPlayer::_on_connection_opened() {
	// the stream created the decoder for the response's content type before the first body byte arrived
	_detach_decoder();
	decoder = http_stream->get_audio_decoder();
	ERR_FAIL_COND_MSG(decoder.is_null(), "RadioPlayer: the stream opened without a decoder.");

	decoder->connect("stream_info_ready", callable_mp(this, &RadioPlayer::_on_stream_info_ready));
	decoder->connect("buffering_started", callable_mp(this, &RadioPlayer::_on_buffering_started));
	decoder->connect("buffering_finished", callable_mp(this, &RadioPlayer::_on_buffering_finished));
	decoder->connect("stream_error", callable_mp(this, &RadioPlayer::_on_stream_error));
	_set_state(STATE_BUFFERING);
}

void RadioPlayer::_on_connection_closed(int p_result) {
	// stop() and _on_stream_error() have already put the node in the state they wanted; the signal is deferred,
	// so a result from a connection that was cancelled in the meantime can also still arrive here
	if (p_result == RadioStream::RESULT_CANCELLED || state == STATE_IDLE || state == STATE_FAILED) { return; }

	Node *player = _get_player();
	if (player != nullptr) {
		player->call("stop");
	}
	_detach_decoder();

	// the state is settled before the signal so a handler that calls play() again is not overridden
	if (p_result == RadioStream::RESULT_UNSUPPORTED_FORMAT || reconnect_delay <= 0.0) {
		_set_state(STATE_FAILED);
	} else {
		reconnect_countdown = reconnect_delay;
		_set_state(STATE_RECONNECTING);
	}
	emit_signal("stream_failed", p_result, http_stream->get_response_code());
}

// "StreamTitle='Artist - Title';StreamUrl='...';" -> "Artist - Title"; anything else is passed through as is
static String extract_stream_title(const String &p_metadata) {
	const String key = "StreamTitle='";
	int start = p_metadata.find(key);
	if (start == -1) { return p_metadata; }
	start += key.length();

	int end = p_metadata.find("';", start);
	if (end == -1) { end = p_metadata.rfind("'"); }
	if (end < start) { return p_metadata.substr(start); }
	return p_metadata.substr(start, end - start);
}

void RadioPlayer::_on_metadata_received(const String &p_metadata) {
	emit_signal("metadata_received", extract_stream_title(p_metadata));
}

// the startup sequence every consumer used to reproduce by hand. Also runs again when the station changes
// sample rate mid-stream: play() then replaces the playback with one sized for the new mix_rate
void RadioPlayer::_on_stream_info_ready() {
	Node *player = _get_player();
	ERR_FAIL_NULL_MSG(player, "RadioPlayer: the player node went away while streaming.");

	stream->set_mix_rate(static_cast<float>(decoder->get_stream_sample_rate()));

	// play() instantiates the playback and sizes its ring from the generator's mix_rate at that moment, so the
	// player is started only now that mix_rate is final. stream_paused is reset first: a playback created while
	// the player is already flagged as paused would not be paused, and pausing it again would be a no-op
	player->set("stream_paused", false);
	player->call("play");
	// paused until enough audio has been buffered (see _on_buffering_finished)
	player->set("stream_paused", true);

	Ref<AudioStreamGeneratorPlayback> playback = player->call("get_stream_playback");
	ERR_FAIL_COND_MSG(playback.is_null(), "RadioPlayer: the player did not produce an AudioStreamGeneratorPlayback. Is its stream an AudioStreamGenerator?");

	decoder->set_playback(playback);
	decoder->set_buffer_thresholds(static_cast<int>(stream->get_mix_rate() * target_buffer));
	_set_state(STATE_BUFFERING);
}

void RadioPlayer::_on_buffering_started() {
	Node *player = _get_player();
	if (player != nullptr) {
		player->set("stream_paused", true);
	}
	_set_state(STATE_BUFFERING);
}

void RadioPlayer::_on_buffering_finished() {
	Node *player = _get_player();
	if (player != nullptr) {
		player->set("stream_paused", false);
	}
	_set_state(STATE_PLAYING);
}

void RadioPlayer::_on_stream_error(const String &p_message) {
	// the decoder cannot make sense of the data (mislabelled content type); reconnecting would not help
	http_stream->cancel_request();
	_fail(RadioStream::RESULT_UNSUPPORTED_FORMAT);
}

RadioPlayer::State RadioPlayer::get_state() const {
	return state;
}

Ref<AudioStreamGenerator> RadioPlayer::get_stream() const {
	return stream;
}

Ref<RadioStream> RadioPlayer::get_http_stream() const {
	return http_stream;
}

Ref<RadioDecoder> RadioPlayer::get_audio_decoder() const {
	return decoder;
}

void RadioPlayer::set_player_path(const NodePath &p_path) {
	player_path = p_path;
}

NodePath RadioPlayer::get_player_path() const {
	return player_path;
}

void RadioPlayer::set_target_buffer(double p_seconds) {
	target_buffer = MAX(p_seconds, 0.0);
}

double RadioPlayer::get_target_buffer() const {
	return target_buffer;
}

void RadioPlayer::set_reconnect_delay(double p_seconds) {
	reconnect_delay = MAX(p_seconds, 0.0);
}

double RadioPlayer::get_reconnect_delay() const {
	return reconnect_delay;
}

void RadioPlayer::set_connect_timeout(double p_seconds) {
	connect_timeout = MAX(p_seconds, 0.0);
}

double RadioPlayer::get_connect_timeout() const {
	return connect_timeout;
}

void RadioPlayer::set_stall_timeout(double p_seconds) {
	http_stream->set_stall_timeout(static_cast<real_t>(p_seconds));
}

double RadioPlayer::get_stall_timeout() const {
	return http_stream->get_stall_timeout();
}

void RadioPlayer::set_parse_metadata(bool p_enabled) {
	parse_metadata = p_enabled;
}

bool RadioPlayer::get_parse_metadata() const {
	return parse_metadata;
}
