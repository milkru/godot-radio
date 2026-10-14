#pragma once

#include "godot_cpp/classes/audio_stream_generator.hpp"
#include "godot_cpp/classes/global_constants.hpp"
#include "godot_cpp/classes/node.hpp"
#include "godot_cpp/classes/ref.hpp"
#include "godot_cpp/core/binder_common.hpp"
#include "godot_cpp/core/defs.hpp"
#include "godot_cpp/variant/node_path.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include "godot_cpp/variant/string.hpp"
#include "icy_audio_decoder.hpp"
#include "icy_http_stream.hpp"

namespace godot {

// Owns one IcyHttpStream, the decoder it creates and the AudioStreamGenerator on the player node, and runs the
// order-sensitive startup sequence (probe -> mix_rate -> play -> playback) plus pause-on-underrun and reconnect.
// The three underlying classes stay public for callers who want the pieces
class IcyRadioPlayer : public Node {
	GDCLASS(IcyRadioPlayer, Node);

public:
	enum State {
		STATE_IDLE,
		STATE_CONNECTING,
		STATE_BUFFERING,
		STATE_PLAYING,
		STATE_RECONNECTING,
		STATE_FAILED
	};

private:
	static constexpr double DEFAULT_TARGET_BUFFER = 0.5;
	static constexpr double DEFAULT_RECONNECT_DELAY = 2.0;
	static constexpr double DEFAULT_CONNECT_TIMEOUT = 5.0;
	static constexpr double DEFAULT_BUFFER_LENGTH = 2.0;

	Ref<IcyHttpStream> http_stream;
	Ref<IcyAudioDecoder> decoder;
	Ref<AudioStreamGenerator> stream;

	NodePath player_path;
	double target_buffer = DEFAULT_TARGET_BUFFER;
	double reconnect_delay = DEFAULT_RECONNECT_DELAY;
	double connect_timeout = DEFAULT_CONNECT_TIMEOUT;
	bool parse_metadata = true;

	String url;
	PackedStringArray custom_headers;
	State state = STATE_IDLE;
	double reconnect_countdown = 0.0;

	// any node with get_stream_playback(): the three player classes share no common base
	Node *_get_player() const;
	void _set_state(State p_state);
	Error _start_request();
	void _detach_decoder();
	void _fail(IcyHttpStream::Result p_result);

	void _on_connection_opened();
	void _on_connection_closed(int p_result);
	void _on_metadata_received(const String &p_metadata);
	void _on_stream_info_ready();
	void _on_buffering_started();
	void _on_buffering_finished();
	void _on_stream_error(const String &p_message);

protected:
	static void _bind_methods();

public:
	IcyRadioPlayer();

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

	Error play(const String &p_url, const PackedStringArray &p_custom_headers = PackedStringArray());
	void stop();
	static String get_home_country();
	State get_state() const;
	Ref<AudioStreamGenerator> get_stream() const;
	Ref<IcyHttpStream> get_http_stream() const;
	Ref<IcyAudioDecoder> get_audio_decoder() const;

	void set_player_path(const NodePath &p_path);
	NodePath get_player_path() const;
	void set_target_buffer(double p_seconds);
	double get_target_buffer() const;
	void set_reconnect_delay(double p_seconds);
	double get_reconnect_delay() const;
	void set_connect_timeout(double p_seconds);
	double get_connect_timeout() const;
	void set_stall_timeout(double p_seconds);
	double get_stall_timeout() const;
	void set_parse_metadata(bool p_enabled);
	bool get_parse_metadata() const;
};

} //namespace godot

VARIANT_ENUM_CAST(IcyRadioPlayer::State);
