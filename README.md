<p align="center" width="100%"><img src="project/radio.svg"></p>

# IcyRadioStreamer

A GDExtension plugin for receiving IceCast streams inside the Godot 4.x game engine.

## Features

* Modular architecture designed to support additional media decoders in the future
* HTTP streaming performed on a dedicated thread
* Lightweight implementation that can operate entirely outside the SceneTree (See `example2.gd`)
* Compatible with any Godot audio playback node, including 2D, 3D, and non-positional audio players
* Full in-editor documentation

## Current Support

At present, the plugin includes an MP3 decoder implementation. Support for additional audio formats is planned.

## Getting Started

Add an `IcyRadioPlayer` node next to an `AudioStreamPlayer` (2D and 3D players work too), set its
`player_path` to the player and call `play()`:

```gdscript
@onready var radio: IcyRadioPlayer = $IcyRadioPlayer

func _ready() -> void:
	radio.metadata_received.connect(func(title: String): print("Now playing: ", title))
	radio.play("http://example.com:8000/stream")
```

The node picks the decoder from the server's content type, sets the player's `AudioStreamGenerator`
to the stream's sample rate, pauses while buffering and reconnects when the connection is lost. The
classes it uses (`IcyHttpStream`, `IcyAudioDecoder`) are public and can be used on their own.

The full example is in `project/addons/icy-radio-streamer/example.gd`. `example2.gd` in the same
folder builds the same radio from `IcyHttpStream` and `IcyAudioDecoder` directly, without the node.

## Overview

IcyRadioStreamer downloads and decodes IceCast audio streams and exposes the resulting raw audio data for playback through Godot's default audio system, allowing you to route streamed audio to any suitable audio player node.
