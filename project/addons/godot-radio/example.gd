extends Node

const STREAM_URL := "http://puma.streemlion.com:1220/stream"
# enums from C++ classes are not dictionaries in GDScript (no .keys()), so the names are listed here for printing
const STATE_NAMES := ["IDLE", "CONNECTING", "BUFFERING", "PLAYING", "RECONNECTING", "FAILED"]

# RadioPlayer handles the connection, the decoder and the startup steps. It only needs a player
# node (AudioStreamPlayer, 2D or 3D) to play through. The RadioStream and RadioDecoder it uses
# are available through get_http_stream() and get_audio_decoder() if you need more control.
# See example2.gd for the same radio built without this node.
@onready var radio: RadioPlayer = $RadioPlayer

func _ready() -> void:
	radio.state_changed.connect(func(state: RadioPlayer.State):
		print("State: ", STATE_NAMES[state])
	)
	radio.metadata_received.connect(func(title: String):
		print("Now playing: ", title)
	)
	radio.stream_failed.connect(func(result: RadioStream.Result, http_code: int):
		print("Stream failed: ", result, " (HTTP ", http_code, ")")
	)
	radio.play(STREAM_URL, ["User-Agent: Godot/4.5"])

func _input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_accept"):
		if radio.get_state() == RadioPlayer.STATE_IDLE:
			radio.play(STREAM_URL, ["User-Agent: Godot/4.5"])
		else:
			radio.stop()
