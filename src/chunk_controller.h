#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/classes/node3d.hpp>

namespace godot {

typedef int BlockType;

class ChunkController : public Node3D {
    GDCLASS(ChunkController, Node3D)

private:
	Dictionary m_chunkInfo;

protected:
	static void _bind_methods();

public:
	ChunkController();
	~ChunkController();

	void on_chunk_generated();
};

}