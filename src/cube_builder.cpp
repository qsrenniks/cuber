#include "cube_builder.h"
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/templates/local_vector.hpp>

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

void ChunkInfo::copy_blocks_from(const PackedInt32Array& blocks) {
	ERR_FAIL_NULL_MSG(m_block_types, "Block array was null in ChunkInfo");
	const int32_t block_count = chunk_width * chunk_height * chunk_depth;
	ERR_FAIL_COND_MSG(blocks.size() != block_count, vformat("Expected %d blocks for a chunk, got %d", block_count, blocks.size()));

	const int32_t* source = blocks.ptr();
	for (int32_t i = 0; i < block_count; ++i) {
		const BlockIndex index(i % chunk_width, i / (chunk_width * chunk_depth), (i / chunk_width) % chunk_depth);
		m_block_types[convert_index_to_address(index)] = source[i];
	}
	m_chunk_dirty = true;
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

template <typename TPacked, typename T>
static TPacked to_packed(const LocalVector<T>& values) {
	TPacked packed;
	packed.resize(values.size());
	memcpy(packed.ptrw(), values.ptr(), values.size() * sizeof(T));
	return packed;
}

Ref<ArrayMesh> ChunkInfo::rebuild_chunk_geometry(const Ref<CubeBuilder>& builder) {
	if (!m_chunk_dirty)
		return nullptr;

	ERR_FAIL_COND_V_MSG(!builder.is_valid(), nullptr, "Builder was nullptr");

	m_chunk_dirty = false;

	// gathered here and copied into packed arrays once at the end, because every append to a
	// packed array is a call into the engine
	LocalVector<Vector3> verts;
	LocalVector<Vector3> normals;
	LocalVector<Vector2> uvs;
	LocalVector<Color> colors;
	LocalVector<int32_t> indices;

	// looked up once here, since finding a chunk goes through a Variant dictionary
	Ref<ChunkInfo> neighbors[BlockDirection::COUNT];
	for (int32_t d = 0; d < BlockDirection::COUNT; ++d) {
		neighbors[d] = builder->find_chunk(m_chunk_index + face_dirs[d]);
	}

	const int32_t block_count = chunk_width * chunk_height * chunk_depth;
	// faces are drawn against air and against hidden blocks, which draw themselves
	auto is_open = [&builder](const BlockType& type) {
		return is_block_air(type) || builder->is_block_type_hidden(type);
	};

	for (int32_t i = 0; i < block_count; ++i) {
		const BlockType& block_type = m_block_types[i];
		if (is_open(block_type)) {
			continue;
		}

		const BlockIndex block_index = convert_address_to_index(i);
		const Vector3 block_center = Vector3(block_index);
		const Color block_color = builder->get_block_color(block_type);
		for (int32_t d = 0; d < BlockDirection::COUNT; ++d) {
			const BlockIndex neighbor_index = block_index + face_dirs[d];

			bool neighbor_is_air;
			if (out_of_bounds(neighbor_index)) {
				// one step out of this chunk always lands in the neighbor on that side
				const Ref<ChunkInfo>& neighbor = neighbors[d];
				if (!neighbor.is_valid() || neighbor->m_block_types == nullptr) {
					neighbor_is_air = true;
				} else {
					const BlockIndex wrapped((neighbor_index.x + chunk_width) % chunk_width, (neighbor_index.y + chunk_height) % chunk_height, (neighbor_index.z + chunk_depth) % chunk_depth);
					neighbor_is_air = is_open(neighbor->m_block_types[convert_index_to_address(wrapped)]);
				}
			} else {
				neighbor_is_air = is_open(m_block_types[convert_index_to_address(neighbor_index)]);
			}
			if (!neighbor_is_air) {
				continue;
			}

			const int32_t base = verts.size();
			Vector3 normal = Vector3(face_dirs[d]);
			for (int32_t c = 0; c < 4; ++c) {
				verts.push_back(block_center + face_corners[d * 4 + c]);
				normals.push_back(normal);
				uvs.push_back(face_uvs[c]);
				colors.push_back(block_color);
			}

			indices.push_back(base);
			indices.push_back(base + 1);
			indices.push_back(base + 2);
			indices.push_back(base);
			indices.push_back(base + 2);
			indices.push_back(base + 3);
		}
	}

	if (verts.is_empty())
		return nullptr;

	Ref<ArrayMesh> new_mesh;
	new_mesh.instantiate();
	ERR_FAIL_NULL_V_MSG(new_mesh, nullptr, "Failed to allocate array mesh");

	Array mesh_attributes;
	mesh_attributes.resize(Mesh::ARRAY_MAX);
	mesh_attributes[Mesh::ARRAY_VERTEX] = to_packed<PackedVector3Array>(verts);
	mesh_attributes[Mesh::ARRAY_NORMAL] = to_packed<PackedVector3Array>(normals);
	mesh_attributes[Mesh::ARRAY_TEX_UV] = to_packed<PackedVector2Array>(uvs);
	mesh_attributes[Mesh::ARRAY_COLOR] = to_packed<PackedColorArray>(colors);
	mesh_attributes[Mesh::ARRAY_INDEX] = to_packed<PackedInt32Array>(indices);

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
}

CubeBuilder::~CubeBuilder() {
}

Ref<ChunkInfo> CubeBuilder::find_chunk(const ChunkIndex& chunk_index) const {
	return m_chunks.get(chunk_index, nullptr);
}

int CubeBuilder::get_block(const GlobalBlockIndex& global_block_index) const {
	Ref<ChunkInfo> chunk = get_chunk(global_block_index);
	if (!chunk.is_valid()) {
		return 0;
	}

	const BlockIndex block_index = convert_global_index_to_block_index(global_block_index);
	return chunk->get_block_at(block_index);
}

void CubeBuilder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_block", "global_block_index", "block_type"), &CubeBuilder::set_block);
	ClassDB::bind_method(D_METHOD("set_chunk_blocks", "chunk_index", "blocks"), &CubeBuilder::set_chunk_blocks);
	ClassDB::bind_method(D_METHOD("rebuild_chunk_geometry", "max_chunks"), &CubeBuilder::rebuild_chunk_geometry, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("clear_all_blocks"), &CubeBuilder::clear_all_blocks);
	ClassDB::bind_static_method("CubeBuilder", D_METHOD("get_chunk_dimensions"), &CubeBuilder::get_chunk_dimensions);
	ClassDB::bind_method(D_METHOD("get_block"), &CubeBuilder::get_block);
	ClassDB::bind_method(D_METHOD("set_block_colors", "block_colors"), &CubeBuilder::set_block_colors);
	ClassDB::bind_method(D_METHOD("get_block_colors"), &CubeBuilder::get_block_colors);
	ClassDB::bind_method(D_METHOD("set_hidden_block_types", "block_types"), &CubeBuilder::set_hidden_block_types);
	ClassDB::bind_method(D_METHOD("get_hidden_block_types"), &CubeBuilder::get_hidden_block_types);

	ADD_SIGNAL(MethodInfo("on_chunk_mesh_ready", 
		PropertyInfo(Variant::VECTOR3I, "chunk_index"),
		PropertyInfo(Variant::OBJECT, "chunk_array_mesh", PROPERTY_HINT_RESOURCE_TYPE, "ArrayMesh")));
}

Vector3i CubeBuilder::get_chunk_dimensions() {
	return Vector3i(chunk_width, chunk_height, chunk_depth);
}

void CubeBuilder::clear_all_blocks() {
	m_chunks.clear();
	m_dirty_chunks.clear();
	m_urgent_chunks.clear();
}

void CubeBuilder::mark_chunk_for_rebuild(const Ref<ChunkInfo>& chunk, bool urgent) {
	// a chunk can end up in both queues; whichever reaches it second finds it clean and skips it
	if (urgent) {
		m_urgent_chunks.push_back(chunk);
	} else if (!chunk->is_chunk_dirty()) {
		m_dirty_chunks.push_back(chunk);
	}
	chunk->mark_chunk_dirty();
}

void CubeBuilder::set_block_colors(const PackedColorArray& block_colors) {
	CONCURRENCY_CHECK(m_grid_info_guard);
	m_block_colors = block_colors;

	// every existing chunk was meshed with the old colors
	Array values = m_chunks.values();
	for (int32_t i = 0; i < values.size(); i++) {
		const Ref<ChunkInfo>& chunk = values[i];
		if (chunk.is_valid()) {
			mark_chunk_for_rebuild(chunk);
		}
	}
}

void CubeBuilder::set_hidden_block_types(const PackedInt32Array& block_types) {
	CONCURRENCY_CHECK(m_grid_info_guard);
	m_hidden_block_types.clear();
	for (int32_t i = 0; i < block_types.size(); ++i) {
		const BlockType block_type = block_types[i];
		ERR_CONTINUE_MSG(block_type < 0, vformat("Block type %d can't be hidden", block_type));
		if (block_type >= (BlockType)m_hidden_block_types.size()) {
			const uint32_t old_size = m_hidden_block_types.size();
			m_hidden_block_types.resize(block_type + 1);
			for (uint32_t j = old_size; j < m_hidden_block_types.size(); ++j) {
				m_hidden_block_types[j] = 0;
			}
		}
		m_hidden_block_types[block_type] = 1;
	}

	// every existing chunk was meshed with the old set
	Array values = m_chunks.values();
	for (int32_t i = 0; i < values.size(); i++) {
		const Ref<ChunkInfo>& chunk = values[i];
		if (chunk.is_valid()) {
			mark_chunk_for_rebuild(chunk);
		}
	}
}

PackedInt32Array CubeBuilder::get_hidden_block_types() const {
	PackedInt32Array block_types;
	for (uint32_t i = 0; i < m_hidden_block_types.size(); ++i) {
		if (m_hidden_block_types[i] != 0) {
			block_types.push_back((int32_t)i);
		}
	}
	return block_types;
}

PackedColorArray CubeBuilder::get_block_colors() const {
	return m_block_colors;
}

Color CubeBuilder::get_block_color(const BlockType& block_type) const {
	if (block_type < 0 || block_type >= m_block_colors.size()) {
		// make blocks without a color stand out
		return Color(1, 0, 1);
	}
	return m_block_colors[block_type];
}

BlockIndex CubeBuilder::convert_global_index_to_block_index(const GlobalBlockIndex& global_block_index) {
	const ChunkIndex& chunk_index = get_chunk_from_global(global_block_index);
	const GlobalBlockIndex chunk_first_block = Vector3i(chunk_index.x * chunk_width, chunk_index.y * chunk_height, chunk_index.z * chunk_depth);
	return 	global_block_index - chunk_first_block;
}

static bool block_on_border(const BlockIndex& index) {
	const bool x_on_border = index.x == 0 || index.x == chunk_width - 1;
	const bool y_on_border = index.y == 0 || index.y == chunk_height - 1;
	const bool z_on_border = index.z == 0 || index.z == chunk_depth - 1;
	return x_on_border || y_on_border || z_on_border;
}

void CubeBuilder::set_block(const GlobalBlockIndex& global_block_index, const BlockType& block_type) {
	CONCURRENCY_CHECK(m_grid_info_guard);
	Ref<ChunkInfo> chunk = get_or_create_chunk(global_block_index);
	const BlockIndex block_index = convert_global_index_to_block_index(global_block_index);
	ERR_FAIL_NULL_MSG(chunk, "Could not find chunk");
	mark_chunk_for_rebuild(chunk, true);
	chunk->set_block_type(block_index, block_type);

	if (block_on_border(block_index)) {
		for (int32_t d = 0; d < BlockDirection::COUNT; ++d) {
			const GlobalBlockIndex neighbor_index = global_block_index + face_dirs[d];
			Ref<ChunkInfo> possible_neighbor_chunk = get_chunk(neighbor_index);
			if (possible_neighbor_chunk.is_valid() && possible_neighbor_chunk != chunk) {
				mark_chunk_for_rebuild(possible_neighbor_chunk, true);
			}
		}
	}
}

void CubeBuilder::set_chunk_blocks(const ChunkIndex& chunk_index, const PackedInt32Array& blocks) {
	CONCURRENCY_CHECK(m_grid_info_guard);

	Ref<ChunkInfo> chunk = m_chunks.get(chunk_index, nullptr);
	if (!chunk.is_valid()) {
		// a missing chunk already reads as air, so an all air one doesn't need creating
		const int32_t* source = blocks.ptr();
		bool has_blocks = false;
		for (int32_t i = 0; i < blocks.size() && !has_blocks; ++i) {
			has_blocks = source[i] != 0;
		}
		if (!has_blocks) {
			return;
		}

		chunk.instantiate();
		chunk->initialize(chunk_index);
		m_chunks.set(chunk_index, chunk);
	}
	mark_chunk_for_rebuild(chunk);
	chunk->copy_blocks_from(blocks);

	// neighbors may have faces against this chunk that are now hidden, or newly exposed
	for (int32_t d = 0; d < BlockDirection::COUNT; ++d) {
		Ref<ChunkInfo> neighbor = m_chunks.get(chunk_index + face_dirs[d], nullptr);
		if (neighbor.is_valid()) {
			mark_chunk_for_rebuild(neighbor);
		}
	}
}

void CubeBuilder::rebuild_chunk_geometry(int32_t max_chunks) {
	if (m_dirty_chunks.is_empty() && m_urgent_chunks.is_empty()) {
		return;
	}

	CONCURRENCY_CHECK(m_grid_info_guard);

	Ref<CubeBuilder> self = Ref<CubeBuilder>(this);

	// every urgent chunk, then this call's share of the rest off the front; the others wait for the next call
	LocalVector<Ref<ChunkInfo>> batch = std::move(m_urgent_chunks);
	m_urgent_chunks = LocalVector<Ref<ChunkInfo>>();
	uint32_t batch_size = m_dirty_chunks.size();
	if (max_chunks >= 0 && uint32_t(max_chunks) < batch_size) {
		batch_size = max_chunks;
	}
	LocalVector<Ref<ChunkInfo>> rest;
	for (uint32_t i = 0; i < m_dirty_chunks.size(); i++) {
		(i < batch_size ? batch : rest).push_back(m_dirty_chunks[i]);
	}
	m_dirty_chunks = std::move(rest);

	for (uint32_t i = 0; i < batch.size(); i++) {
		const Ref<ChunkInfo>& chunk = batch[i];
		if (!chunk->is_chunk_dirty()) {
			continue;
		}
		// only rebuilt chunks are reported; a null mesh means the chunk has no faces left
		Ref<ArrayMesh> new_mesh = chunk->rebuild_chunk_geometry(self);
		emit_signal("on_chunk_mesh_ready", chunk->get_chunk_index(), new_mesh);
	}
}

Ref<ChunkInfo> CubeBuilder::get_chunk(const GlobalBlockIndex& index) const{
	ChunkIndex chunk_index = get_chunk_from_global(index);
	return m_chunks.get(chunk_index, nullptr);
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

// rounds toward negative infinity, so block -1 lands in chunk -1 rather than chunk 0
static int32_t floor_div(int32_t value, int32_t divisor) {
	const int32_t quotient = value / divisor;
	return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

ChunkIndex CubeBuilder::get_chunk_from_global(const GlobalBlockIndex& index) {
	return ChunkIndex(floor_div(index.x, chunk_width), floor_div(index.y, chunk_height), floor_div(index.z, chunk_depth));
}