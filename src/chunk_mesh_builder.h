#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/typed_dictionary.hpp>
#include <godot_cpp/variant/vector3i.hpp>

namespace godot {

class ArrayMesh;

using GridInfo = TypedDictionary<Vector3i, int>;

class ChunkMeshBuilder : public MeshInstance3D {
	GDCLASS(ChunkMeshBuilder, MeshInstance3D)

private:
	static constexpr float BLOCK_THRESHOLD = 0.001f;

	void _generate_grid_info(GridInfo grid_info, Node3D *controller) const;

protected:
	static void _bind_methods();

public:
	//controller is the GDScript ChunkController; its constants, noise and get_block_at_index are read through get/call
	static Array generate_mesh_arrays_from_grid_info(const Vector3i &chunk_index, const GridInfo &chunk_info, Node3D *controller);
	static Ref<ArrayMesh> generate_mesh_from_grid_info(const Vector3i &chunk_index, const GridInfo &grid_info, Node3D *controller);

	void generate_chunk_mesh(const Vector3i &chunk_index, const GridInfo &grid_info, Node3D *controller);
	void generate_chunk_grid_info(const GridInfo &grid_info, Node3D *controller);
};

}
