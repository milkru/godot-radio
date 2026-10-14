#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "radio_decoder.hpp"
#include "radio_mp3_decoder.hpp"
#include "radio_player.hpp"
#include "radio_stream.hpp"

using namespace godot;

void initialize_gdextension_types(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_ABSTRACT_CLASS(RadioDecoder);
	GDREGISTER_CLASS(RadioMp3Decoder);
	GDREGISTER_CLASS(RadioStream);
	GDREGISTER_CLASS(RadioPlayer);

	RadioDecoder::register_decoder("audio/mpeg", &RadioMp3Decoder::create_instance);
	RadioDecoder::register_decoder("audio/mp3", &RadioMp3Decoder::create_instance);
	RadioDecoder::register_decoder("audio/x-mpeg", &RadioMp3Decoder::create_instance);
	RadioDecoder::register_decoder("audio/mpeg3", &RadioMp3Decoder::create_instance);
}

void uninitialize_gdextension_types(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}

extern "C" {
	// Initialization
	GDExtensionBool GDE_EXPORT godot_radio_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
		GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
		init_obj.register_initializer(initialize_gdextension_types);
		init_obj.register_terminator(uninitialize_gdextension_types);
		init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

		return init_obj.init();
	}
}
