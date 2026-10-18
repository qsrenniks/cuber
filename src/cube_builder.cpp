#include "cube_builder.h"
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>

using namespace godot;

ChunkInfo::ChunkInfo() {
	m_block_types = nullptr;
	m_chunk_dirty = false;
	m_chunk_index = ChunkIndex(0, 0, 0);
}

ChunkInfo::~ChunkInfo() {
	shutdown();
}

void ChunkInfo::initialize(const ChunkIndex& index) {
	m_chunk_index = index;
	const int32_t block_count = chunk_width * chunk_height * chunk_depth;
	m_block_types = memnew_arr(BlockType, block_count);
	ERR_FAIL_NULL_MSG(m_block_types, "Failed to allocate information for a chunk");
	memset(m_block_types, 0, sizeof(BlockType) * block_count);
}

void ChunkInfo::shutdown() {
	if (m_block_types) {
		memdelete_arr(m_block_types);
		m_block_types = nullptr;
	}
}

static bool out_of_bounds(const BlockIndex& index) {
	return index.x < 0 || index.y < 0 || index.z < 0
		|| index.x >= chunk_width || index.y >= chunk_height || index.z >= chunk_depth;
}

void ChunkInfo::set_block_type(const BlockIndex& index, const BlockType& blockType) {
	ERR_FAIL_NULL_MSG(m_block_types, "Block array was null in ChunkInfo");
	ERR_FAIL_COND_MSG(out_of_bounds(index), vformat("Block index [%d %d %d] is outside the chunk", index.x, index.y, index.z));
	const int32_t block_address = convert_index_to_address(index);

	//UtilityFunctions::print(vformat("Setting block %d to %d", block_address, blockType));
	m_block_types[block_address] = blockType;
	m_chunk_dirty = true;
}

static bool is_block_air(const BlockType& type) {
	return type == 0;
}

static const Vector3 face_corners[24] = {
	Vector3(0, 1, 1), Vector3(0, 1, 0), Vector3(1, 1, 0), Vector3(1, 1, 1),
	Vector3(0, 0, 0), Vector3(0, 0, 1), Vector3(1, 0, 1), Vector3(1, 0, 0),
	Vector3(1, 0, 1), Vector3(1, 1, 1), Vector3(1, 1, 0), Vector3(1, 0, 0),
	Vector3(0, 0, 0), Vector3(0, 1, 0), Vector3(0, 1, 1), Vector3(0, 0, 1),
	Vector3(0, 0, 1), Vector3(0, 1, 1), Vector3(1, 1, 1), Vector3(1, 0, 1),
	Vector3(1, 0, 0), Vector3(1, 1, 0), Vector3(0, 1, 0), Vector3(0, 0, 0),
};

static const Vector2 face_uvs[4] = {
	Vector2(0, 1), Vector2(0, 0), Vector2(1, 0), Vector2(1, 1)
};

static const Vector3i face_dirs[6] = {
	Vector3i(0, 1, 0), 
	Vector3i(0, -1, 0), 
	Vector3i(1, 0, 0), 
	Vector3i(-1, 0, 0), 
	Vector3i(0, 0, 1), 
	Vector3i(0, 0, -1), 
};

BlockType ChunkInfo::get_block_at(const BlockIndex& index) {
	const int32_t address = convert_index_to_address(index);
	ERR_FAIL_COND_V_MSG(address < 0 || address >= (chunk_width * chunk_height * chunk_depth), 0, "Tried indexing outside of bounds");
	return m_block_types[address];
}

bool CubeBuilder::is_block_air(const GlobalBlockIndex& global_block_index) const {
	const ChunkIndex& chunk_index = get_chunk_from_global(global_block_index);
	Ref<ChunkInfo> chunk_info = m_chunks.get(chunk_index, nullptr);
	if (!chunk_info.is_valid()) {
		return true;
	}

	const BlockIndex block_index = CubeBuilder::convert_global_index_to_block_index(global_block_index);
	return chunk_info->get_block_at(block_index) == 0;
}

Ref<ArrayMesh> ChunkInfo::rebuild_chunk_geometry(const Ref<CubeBuilder>& builder) {
	if (!m_chunk_dirty)
		return nullptr;

	ERR_FAIL_COND_V_MSG(!builder.is_valid(), nullptr, "Builder was nullptr");

	m_chunk_dirty = false;

	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;

	const GlobalBlockIndex chunk_first_block = Vector3i(m_chunk_index.x * chunk_width, m_chunk_index.y * chunk_height, m_chunk_index.z * chunk_depth);

	const int32_t block_count = chunk_width * chunk_height * chunk_depth;
	for (int32_t i = 0; i < block_count; ++i) {
		const BlockType& block_type = m_block_types[i];
		if (is_block_air(block_type)) {
			continue;
		}

		const BlockIndex block_index = convert_address_to_index(i);
		const Vector3 block_center = Vector3(block_index);
		for (int32_t d = 0; d < BlockDirection::COUNT; ++d) {
			const BlockIndex neighbor_index = block_index + face_dirs[d];
			
			const GlobalBlockIndex global_index = chunk_first_block + neighbor_index;
			if (!builder->is_block_air(global_index)) {
				continue;
			}

			const int32_t base = verts.size();
			Vector3 normal = Vector3(face_dirs[d]);
			for (int32_t c = 0; c < 4; ++c) {
				verts.append(block_center + face_corners[d * 4 + c]);
				normals.append(normal);
				uvs.append(face_uvs[c]);
			}

			indices.append(base);
			indices.append(base + 1);
			indices.append(base + 2);
			indices.append(base);
			indices.append(base + 2);
			indices.append(base + 3);
		}
	}

	if (verts.is_empty())
		return nullptr;

	Ref<ArrayMesh> new_mesh;
	new_mesh.instantiate();
	ERR_FAIL_NULL_V_MSG(new_mesh, nullptr, "Failed to allocate array mesh");

	Array mesh_attributes;
	mesh_attributes.resize(Mesh::ARRAY_MAX);
	mesh_attributes[Mesh::ARRAY_VERTEX] = verts;
	mesh_attributes[Mesh::ARRAY_NORMAL] = normals;
	mesh_attributes[Mesh::ARRAY_TEX_UV] = uvs;
	mesh_attributes[Mesh::ARRAY_INDEX] = indices;

	new_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, mesh_attributes);
	return new_mesh;
}

int32_t ChunkInfo::get_neighbor_index(const int32_t& index, BlockDirection direction)
{
	switch (direction)
	{
	case BlockDirection::TOP:
		return index + (chunk_width * chunk_depth);
	case BlockDirection::BOTTOM:
		return index - (chunk_width * chunk_depth);
	case BlockDirection::LEFT:
		return index + (chunk_depth);
	case BlockDirection::RIGHT:
		return index - (chunk_depth);
	case BlockDirection::FORWARD:
		return index + 1;
	case BlockDirection::BACKWARD:
		return index - 1;
	}

	return -1;
}

void ChunkInfo::_bind_methods() {

}

int32_t ChunkInfo::convert_index_to_address(const BlockIndex& index) {
	return index.z + (index.x * chunk_depth) + (index.y * chunk_width * chunk_depth);
}

BlockIndex ChunkInfo::convert_address_to_index(const int32_t& address) {
	const int32_t z = address % chunk_depth;
	const int32_t x = (address / chunk_depth) % chunk_width;
	const int32_t y = address / (chunk_width * chunk_depth);
	return BlockIndex(x, y, z);
}

CubeBuilder::CubeBuilder() {
	m_chunks_dirty = false;
}

CubeBuilder::~CubeBuilder() {
}

void CubeBuilder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_block", "global_block_index", "block_type"), &CubeBuilder::set_block);
	ClassDB::bind_method(D_METHOD("rebuild_chunk_geometry"), &CubeBuilder::rebuild_chunk_geometry);
	ClassDB::bind_method(D_METHOD("clear_all_blocks"), &CubeBuilder::clear_all_blocks);
	ClassDB::bind_static_method("CubeBuilder", D_METHOD("get_chunk_dimensions"), &CubeBuilder::get_chunk_dimensions);

	ADD_SIGNAL(MethodInfo("on_chunk_mesh_ready", 
		PropertyInfo(Variant::VECTOR3I, "chunk_index"),
		PropertyInfo(Variant::OBJECT, "chunk_array_mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh")));
}

Vector3i CubeBuilder::get_chunk_dimensions() {
	return Vector3i(chunk_width, chunk_height, chunk_depth);
}

void CubeBuilder::clear_all_blocks() {
	m_chunks.clear();
	m_chunks_dirty = true;
}

BlockIndex CubeBuilder::convert_global_index_to_block_index(const GlobalBlockIndex& global_block_index) {
	const ChunkIndex& chunk_index = get_chunk_from_global(global_block_index);
	const GlobalBlockIndex chunk_first_block = Vector3i(chunk_index.x * chunk_width, chunk_index.y * chunk_height, chunk_index.z * chunk_depth);
	return 	global_block_index - chunk_first_block;
}

void CubeBuilder::set_block(const GlobalBlockIndex& global_block_index, const BlockType& block_type) {
	CONCURRENCY_CHECK(m_grid_info_guard);
	Ref<ChunkInfo> chunk = get_or_create_chunk(global_block_index);
	const BlockIndex block_index = convert_global_index_to_block_index(global_block_index);
	ERR_FAIL_NULL_MSG(chunk, "Could not find chunk");
	chunk->set_block_type(block_index, block_type);
	m_chunks_dirty = true;
}

void CubeBuilder::rebuild_chunk_geometry() {
	if (!m_chunks_dirty)
		return;

	CONCURRENCY_CHECK(m_grid_info_guard);
	m_chunks_dirty = false;

	Ref<CubeBuilder> self = Ref<CubeBuilder>(this);

	Array values = m_chunks.values();
	const int32_t value_count = values.size();
	for (int32_t i = 0; i < value_count; i++) {
		const Ref<ChunkInfo>& chunk = values[i];
		if (!chunk.is_valid())
			continue;
		
		Ref<ArrayMesh> new_mesh = chunk->rebuild_chunk_geometry(self);
		emit_signal("on_chunk_mesh_ready", chunk->get_chunk_index(), new_mesh);
	}
}

Ref<ChunkInfo> CubeBuilder::get_or_create_chunk(const GlobalBlockIndex& index) {
	// might be good to create a pool of this information
	ChunkIndex chunk_index = get_chunk_from_global(index);

	Ref<ChunkInfo> chunk = m_chunks.get(chunk_index, nullptr);
	if (chunk == nullptr) {
		chunk.instantiate();
		chunk->initialize(chunk_index);
		m_chunks.set(chunk_index, chunk);
	}

	return chunk;
}

ChunkIndex CubeBuilder::get_chunk_from_global(const GlobalBlockIndex& index) {
	int32_t x = UtilityFunctions::floor(float(index.x) / float(chunk_width));
	int32_t y = UtilityFunctions::floor(float(index.y) / float(chunk_height));
	int32_t z = UtilityFunctions::floor(float(index.z) / float(chunk_depth));
	return ChunkIndex(x, y, z);
}