#include "chunk_mesh_builder.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/noise.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <vector>

using namespace godot;

namespace {

struct ChunkDims {
	int32_t width;
	int32_t height;
	int32_t depth;
};

ChunkDims get_dims(const Node3D *controller) {
	return {
		int32_t(controller->get("chunk_width")),
		int32_t(controller->get("chunk_height")),
		int32_t(controller->get("chunk_depth")),
	};
}

//direction each of the six cube faces points in
const Vector3i FACE_DIRS[6] = {
	Vector3i(0, 1, 0), //top
	Vector3i(0, -1, 0), //bottom
	Vector3i(1, 0, 0), //left
	Vector3i(-1, 0, 0), //right
	Vector3i(0, 0, 1), //forward
	Vector3i(0, 0, -1), //back
};

//four corners for each face in FACE_DIRS, as offsets from the cube
//ordered bottom left, top left, top right, bottom right when looking at the face from outside the cube, which winds it clockwise
const Vector3 FACE_CORNERS[24] = {
	Vector3(0, 1, 1), Vector3(0, 1, 0), Vector3(1, 1, 0), Vector3(1, 1, 1),
	Vector3(0, 0, 0), Vector3(0, 0, 1), Vector3(1, 0, 1), Vector3(1, 0, 0),
	Vector3(1, 0, 1), Vector3(1, 1, 1), Vector3(1, 1, 0), Vector3(1, 0, 0),
	Vector3(0, 0, 0), Vector3(0, 1, 0), Vector3(0, 1, 1), Vector3(0, 0, 1),
	Vector3(0, 0, 1), Vector3(0, 1, 1), Vector3(1, 1, 1), Vector3(1, 0, 1),
	Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0), Vector3(0, 0, 0),
};

//uvs for the four corners of a face, in the same order as FACE_CORNERS
const Vector2 FACE_UVS[4] = { Vector2(0, 1), Vector2(0, 0), Vector2(1, 0), Vector2(1, 1) };

}

void ChunkMeshBuilder::_bind_methods() {
	ClassDB::bind_static_method("ChunkMeshBuilder", D_METHOD("generate_mesh_arrays_from_grid_info", "chunk_index", "chunk_info", "controller"), &ChunkMeshBuilder::generate_mesh_arrays_from_grid_info);
	ClassDB::bind_static_method("ChunkMeshBuilder", D_METHOD("generate_mesh_from_grid_info", "chunk_index", "grid_info", "controller"), &ChunkMeshBuilder::generate_mesh_from_grid_info);
	ClassDB::bind_method(D_METHOD("generate_chunk_mesh", "chunk_index", "grid_info", "controller"), &ChunkMeshBuilder::generate_chunk_mesh);
	ClassDB::bind_method(D_METHOD("generate_chunk_grid_info", "grid_info", "controller"), &ChunkMeshBuilder::generate_chunk_grid_info);
}

void ChunkMeshBuilder::_generate_grid_info(GridInfo grid_info, Node3D *controller) const {
	ERR_FAIL_NULL(controller);
	grid_info.clear();

	const ChunkDims dims = get_dims(controller);
	const Ref<Noise> noise = controller->get("noise");
	const Ref<Noise> surface_noise = controller->get("surface_noise");
	ERR_FAIL_COND(noise.is_null() || surface_noise.is_null());

	const Vector3 chunk_offset = get_global_position();

	for (int32_t i = 0; i < dims.width; i++) {
		for (int32_t k = 0; k < dims.depth; k++) {
			const Vector3 world_sample = chunk_offset + Vector3(i, 0.0, k);
			const float height = 16.0f + surface_noise->get_noise_2d(world_sample.x, world_sample.z);
			for (int32_t l = 0; l < dims.height; l++) {
				const float sample = noise->get_noise_3d(i + chunk_offset.x, l + chunk_offset.y, k + chunk_offset.z);
				const float density = ((height - l) / 8.0f) + sample;
				if (density > BLOCK_THRESHOLD) {
					grid_info[Vector3i(i, l, k)] = 1;
				}
			}
		}
	}
}

Array ChunkMeshBuilder::generate_mesh_arrays_from_grid_info(const Vector3i &chunk_index, const GridInfo &chunk_info, Node3D *controller) {
	ERR_FAIL_NULL_V(controller, Array());

	const ChunkDims dims = get_dims(controller);
	const Vector3i global_index = chunk_index * Vector3i(dims.width, dims.height, dims.depth);

	//copy the chunk into a flat array once, so lookups inside the chunk skip both the dictionary and the controller
	auto in_chunk = [&](const Vector3i &p) {
		return p.x >= 0 && p.x < dims.width && p.y >= 0 && p.y < dims.height && p.z >= 0 && p.z < dims.depth;
	};
	auto flat_index = [&](const Vector3i &p) {
		return (size_t(p.x) * dims.height + p.y) * dims.depth + p.z;
	};

	std::vector<int32_t> blocks(size_t(dims.width) * dims.height * dims.depth, 0);
	const Array keys = chunk_info.keys();
	const Array values = chunk_info.values();
	for (int64_t i = 0; i < keys.size(); i++) {
		const Vector3i p = keys[i];
		if (in_chunk(p)) {
			blocks[flat_index(p)] = values[i];
		}
	}

	//this is the only neighbour lookup, so it is the one place to change to look into other chunks
	//only faces on the chunk border go through the controller
	auto block_at = [&](const Vector3i &p) -> int32_t {
		if (in_chunk(p)) {
			return blocks[flat_index(p)];
		}
		return controller->call("get_block_at_index", global_index + p);
	};

	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;

	for (int32_t x = 0; x < dims.width; x++) {
		for (int32_t z = 0; z < dims.depth; z++) {
			for (int32_t y = 0; y < dims.height; y++) {
				const Vector3i block_index(x, y, z);
				if (blocks[flat_index(block_index)] == 0) {
					continue;
				}

				const Vector3 origin(block_index);
				for (int f = 0; f < 6; f++) {
					const Vector3i dir = FACE_DIRS[f];
					//a face against another block can never be seen
					if (block_at(block_index + dir) != 0) {
						continue;
					}

					//four verts per face, shared by its two triangles through the indices
					const int32_t base = int32_t(verts.size());
					const Vector3 normal(dir);
					for (int c = 0; c < 4; c++) {
						verts.push_back(origin + FACE_CORNERS[f * 4 + c]);
						normals.push_back(normal);
						uvs.push_back(FACE_UVS[c]);
					}

					indices.push_back(base);
					indices.push_back(base + 1);
					indices.push_back(base + 2);
					indices.push_back(base);
					indices.push_back(base + 2);
					indices.push_back(base + 3);
				}
			}
		}
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;

	return arrays;
}

Ref<ArrayMesh> ChunkMeshBuilder::generate_mesh_from_grid_info(const Vector3i &chunk_index, const GridInfo &grid_info, Node3D *controller) {
	Ref<ArrayMesh> nmesh;
	nmesh.instantiate();

	const Array arrays = generate_mesh_arrays_from_grid_info(chunk_index, grid_info, controller);
	const PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
	//a surface can't be created without any verts, so an empty chunk gets a mesh with no surfaces
	if (!verts.is_empty()) {
		nmesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	}

	return nmesh;
}

void ChunkMeshBuilder::generate_chunk_mesh(const Vector3i &chunk_index, const GridInfo &grid_info, Node3D *controller) {
	set_mesh(generate_mesh_from_grid_info(chunk_index, grid_info, controller));
}

void ChunkMeshBuilder::generate_chunk_grid_info(const GridInfo &grid_info, Node3D *controller) {
	_generate_grid_info(grid_info, controller);
}
