extends Node

# Same radio as example.gd, but built from RadioStream and RadioDecoder directly, without
# the RadioPlayer node. Use this when you need your own logic (for example a different
# reconnect policy, or several streams on one node).

const STREAM_URL := "http://puma.streemlion.com:1220/stream"

@export var radio_player: AudioStreamPlayer
@onready var timer: Timer = $Timer

var radio_stream: RadioStream
var radio_decoder: RadioDecoder
var stream: AudioStreamGenerator

func _ready() -> void:
	stream = radio_player.stream
	radio_stream = RadioStream.new()
	# If no data arrives for this many seconds, the connection is closed with RESULT_TIMEOUT.
	# Keep it longer than the decoder's buffer (about 8s), so a short network hiccup does not
	# drop the connection. The connect timeout is a separate value, passed to request().
	radio_stream.stall_timeout = 10.0

	# The stream creates the decoder itself, based on the Content-Type header, and gives it the
	# bitrate from the icy-br header. Here we only need to get it and connect its signals.
	# Alternative: to manually set decoder, use radio_stream.get_content_mime_type(),
	# and call radio_stream.set_audio_decoder(RadioMp3Decoder.new()) before request().
	radio_stream.connection_opened.connect(func():
		radio_decoder = radio_stream.get_audio_decoder()
		radio_decoder.stream_info_ready.connect(on_stream_info_ready)
		radio_decoder.buffering_started.connect(on_buffering_started)
		radio_decoder.buffering_finished.connect(on_buffering_finished)
		radio_decoder.stream_error.connect(func(message: String):
			# the decoder cannot decode the data (usually a wrong Content-Type). Retrying will not help
			push_error(message)
			stop_radio()
		)
	)

	radio_stream.connection_closed.connect(func(result: RadioStream.Result):
		print("Connection closed: ", result, " (HTTP ", radio_stream.get_response_code(), ")")
		# The connection can close before it opened (cannot connect, 404, unsupported format).
		# In that case there is no decoder. Setting radio_decoder to null also stops the timer
		# from calling process_audio() on the old decoder
		radio_decoder = null
		radio_player.stop()
	)

	radio_stream.metadata_received.connect(func(metadata: String):
		print("Metadata: ", metadata)
	)

	# process_audio() moves decoded audio into the generator. It only decodes as much as fits,
	# so it can also be called from _process(). A timer set to a quarter of the generator's
	# buffer_length moves bigger chunks less often
	timer.wait_time = stream.buffer_length / 4.0
	timer.ignore_time_scale = true
	timer.timeout.connect(func():
		if radio_decoder:
			radio_decoder.process_audio()
	)
	timer.start()

	start_radio()

# called after the first frame is decoded. It is called again if the station changes the sample
# rate during the stream (some stations do this between tracks). The generator's mix_rate cannot
# change while playing, so the player is restarted with a new playback for the new rate
func on_stream_info_ready() -> void:
	var rate: int = radio_decoder.get_stream_sample_rate()
	print("Mix rate: %d Hz, channels: %d" % [rate, radio_decoder.get_stream_channels()])
	stream.mix_rate = rate

	# play() creates the playback and sizes its buffer from the generator's mix_rate at that moment,
	# so the player is started only now, after mix_rate is set. stream_paused is set to false first:
	# a playback created while the player is already paused would not be paused, and setting
	# stream_paused = true again would do nothing
	radio_player.stream_paused = false
	radio_player.play()
	# stay paused until enough audio is buffered (see on_buffering_finished)
	radio_player.stream_paused = true

	# give the new playback to the decoder. The frame that triggered this signal is pushed into it right away
	radio_decoder.set_playback(radio_player.get_stream_playback())
	# buffer half a second of audio before playback starts or resumes. Must fit in the generator's buffer_length
	radio_decoder.set_buffer_thresholds(int(stream.mix_rate / 2))

func on_buffering_started() -> void:
	print("Buffering started (underrun detected)")
	radio_player.stream_paused = true

func on_buffering_finished() -> void:
	print("Buffering complete. Unpausing playback.")
	radio_player.stream_paused = false

func start_radio() -> void:
	var request_headers: PackedStringArray = ["User-Agent: Godot/4.5"]
	var err := radio_stream.request(STREAM_URL, 5.0, true, request_headers)
	if err != OK:
		push_error("request() failed: %d" % err)

func stop_radio() -> void:
	radio_stream.cancel_request()

func _input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_accept"):
		if radio_stream.is_requesting():
			stop_radio()
		else:
			start_radio()
