#include <godot_cpp/godot.hpp>

#include "cube_builder.h"

using namespace godot;

void init_mod(ModuleInitializationLevel l) {
    if (l == MODULE_INITIALIZATION_LEVEL_SCENE)
    {
        GDREGISTER_CLASS(CubeBuilder);
		GDREGISTER_INTERNAL_CLASS(ChunkInfo);
    }
}
void uninit_mod(ModuleInitializationLevel) {}

extern "C" GDExtensionBool GDE_EXPORT cuber_init(
    GDExtensionInterfaceGetProcAddress p, GDExtensionClassLibraryPtr lib,
    GDExtensionInitialization *init) {
    GDExtensionBinding::InitObject o(p, lib, init);
    o.register_initializer(init_mod);
    o.register_terminator(uninit_mod);
    o.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return o.init();
}
