# Godot Radio

Live internet radio for Godot 4.3+, as a GDExtension. It streams MP3 stations, decodes them as they
arrive and plays them through any Godot audio player node.

## Features

* Plays through `AudioStreamPlayer`, `AudioStreamPlayer2D` or `AudioStreamPlayer3D`
* Downloads on its own thread, over HTTP or HTTPS
* Reads endless streams from servers Godot's `HTTPClient` can't, including SHOUTcast `ICY 200 OK` answers
* Follows redirects, pauses while buffering and reconnects when a station drops
* Reports song titles from the stream metadata
* Full in-editor class documentation

## Usage

Add a `RadioPlayer` node next to an audio player, set its `player_path` to the player and call
`play()`:

```gdscript
@onready var radio: RadioPlayer = $RadioPlayer

func _ready() -> void:
	radio.metadata_received.connect(func(title: String): print("Now playing: ", title))
	radio.play("https://example.com/stream.mp3")
```

`stream_failed` reports why a station stopped. The classes behind the node, `RadioStream` and
`RadioDecoder`, are public and work on their own too. The examples in `project/addons/` show both
ways.

## Building

Clone with submodules, then build from the repository root:

```
scons platform=windows target=template_debug
scons platform=windows target=template_release
```

The libraries land in the addon's `bin/` folder under `project/addons/`.

## License

MIT, see `LICENSE.md`.
