#include "cube_builder.h"
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/templates/local_vector.hpp>

using namespace godot;

ChunkInfo::ChunkInfo() {
	m_block_types = nullptr;
	m_chunk_dirty = false;
	m_geometry_dirty = true;
	m_geometry_version = 0;
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
	mark_chunk_dirty();
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
	mark_chunk_dirty();
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

Vector2i ChunkInfo::get_light_at(const BlockIndex& index) const {
	if (m_light.is_empty() || out_of_bounds(index)) {
		return Vector2i(max_light, 0);
	}
	const uint8_t packed = m_light[convert_index_to_address(index)];
	return Vector2i(packed >> 4, packed & 0x0F);
}

Vector2i CubeBuilder::get_light_at(const GlobalBlockIndex& global_block_index) const {
	Ref<ChunkInfo> chunk = get_chunk(global_block_index);
	if (!chunk.is_valid()) {
		return Vector2i(max_light, 0);
	}
	return chunk->get_light_at(convert_global_index_to_block_index(global_block_index));
}

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

// the region a chunk's light is worked out over: the chunk and max_light blocks all round it, so anything that could
// shine on it is included
static const int32_t light_pad = max_light;
static const int32_t light_dim = chunk_width + 2 * light_pad;
static const int32_t light_cells = light_dim * light_dim * light_dim;
static_assert(chunk_width == chunk_height && chunk_height == chunk_depth, "the light region assumes cubic chunks");
static_assert(light_pad < chunk_width, "the light region reaches only into the chunks next to this one");

// how many layers of chunks a column has at most, from y = 0 up; the sky is looked for this far up too
static const int32_t max_chunk_layers = 16;

// what the light region holds where a neighbouring column isn't loaded yet: not a real block, and solid, so light
// stops at it and faces against it aren't drawn
static const BlockType unloaded_block = -1;

// x, y and z run from 0 to light_dim; the chunk itself starts at light_pad
static inline int32_t region_index(int32_t x, int32_t y, int32_t z) {
	return (y * light_dim + z) * light_dim + x;
}

// the corner shading for how many of a corner's three neighbours block it: none, one, two, or both sides
static const float corner_shading[4] = { 1.0f, 0.8f, 0.62f, 0.45f };

// spreads every level in queue out through the blocks that let light through, a level less per block
static void spread_light(uint8_t* light, const BlockType* types, const Ref<CubeBuilder>& builder, LocalVector<int32_t>& queue) {
	static const int32_t steps[6] = { 1, -1, light_dim, -light_dim, light_dim * light_dim, -light_dim * light_dim };
	for (uint32_t head = 0; head < queue.size(); ++head) {
		const int32_t i = queue[head];
		const uint8_t level = light[i];
		if (level <= 1) {
			continue;
		}
		const int32_t x = i % light_dim;
		const int32_t z = (i / light_dim) % light_dim;
		const int32_t y = i / (light_dim * light_dim);
		const bool inside[6] = { x + 1 < light_dim, x > 0, z + 1 < light_dim, z > 0, y + 1 < light_dim, y > 0 };
		for (int32_t s = 0; s < 6; ++s) {
			if (!inside[s]) {
				continue;
			}
			const int32_t n = i + steps[s];
			if (light[n] >= level - 1 || !builder->lets_light_through(types[n])) {
				continue;
			}
			light[n] = level - 1;
			queue.push_back(n);
		}
	}
}

Ref<ArrayMesh> ChunkInfo::rebuild_chunk_geometry(const Ref<CubeBuilder>& builder) {
	if (!m_chunk_dirty)
		return nullptr;

	ERR_FAIL_COND_V_MSG(!builder.is_valid(), nullptr, "Builder was nullptr");

	m_chunk_dirty = false;
	if (m_geometry_dirty) {
		m_geometry_dirty = false;
		++m_geometry_version;
	}

	// the chunks within light's reach, looked up once, since finding a chunk goes through a Variant dictionary
	Ref<ChunkInfo> nearby_refs[27];
	ChunkInfo* nearby[27];
	// what a missing chunk reads as: air, or solid where its column isn't loaded yet
	BlockType missing[27];
	for (int32_t i = 0; i < 27; ++i) {
		const Vector3i offset(i % 3 - 1, (i / 3) % 3 - 1, i / 9 - 1);
		nearby_refs[i] = offset == Vector3i() ? Ref<ChunkInfo>(this) : builder->find_chunk(m_chunk_index + offset);
		nearby[i] = nearby_refs[i].is_valid() && nearby_refs[i]->m_block_types ? nearby_refs[i].ptr() : nullptr;
		missing[i] = builder->is_column_unloaded(Vector2i(m_chunk_index.x + offset.x, m_chunk_index.z + offset.z)) ? unloaded_block : 0;
	}

	// every block in the light region, read once
	LocalVector<BlockType> types;
	types.resize(light_cells);
	for (int32_t y = 0; y < light_dim; ++y) {
		const int32_t ly = y - light_pad;
		const int32_t cy = ly < 0 ? -1 : (ly >= chunk_height ? 1 : 0);
		for (int32_t z = 0; z < light_dim; ++z) {
			const int32_t lz = z - light_pad;
			const int32_t cz = lz < 0 ? -1 : (lz >= chunk_depth ? 1 : 0);
			for (int32_t x = 0; x < light_dim; ++x) {
				const int32_t lx = x - light_pad;
				const int32_t cx = lx < 0 ? -1 : (lx >= chunk_width ? 1 : 0);
				const int32_t n = (cx + 1) + (cy + 1) * 3 + (cz + 1) * 9;
				const ChunkInfo* chunk = nearby[n];
				types[region_index(x, y, z)] = chunk ? chunk->m_block_types[convert_index_to_address(BlockIndex(lx - cx * chunk_width, ly - cy * chunk_height, lz - cz * chunk_depth))] : missing[n];
			}
		}
	}

	auto is_open = [&builder](const BlockType& type) {
		return builder->lets_light_through(type);
	};

	// whether anything above the region keeps the sky off each of its columns; chunks that don't exist are air
	static const int32_t chunks_above = 16;
	LocalVector<uint8_t> roofed;
	roofed.resize(light_dim * light_dim);
	for (int32_t dz = -1; dz <= 1; ++dz) {
		for (int32_t dx = -1; dx <= 1; ++dx) {
			// ground that isn't loaded yet keeps the sky off, until it arrives and this is rebuilt
			const bool unloaded = builder->is_column_unloaded(Vector2i(m_chunk_index.x + dx, m_chunk_index.z + dz));
			Ref<ChunkInfo> column[chunks_above];
			for (int32_t dy = 1; dy < chunks_above; ++dy) {
				column[dy] = builder->find_chunk(m_chunk_index + Vector3i(dx, dy, dz));
			}
			for (int32_t z = 0; z < light_dim; ++z) {
				const int32_t lz = z - light_pad;
				if ((lz < 0 ? -1 : (lz >= chunk_depth ? 1 : 0)) != dz) {
					continue;
				}
				for (int32_t x = 0; x < light_dim; ++x) {
					const int32_t lx = x - light_pad;
					if ((lx < 0 ? -1 : (lx >= chunk_width ? 1 : 0)) != dx) {
						continue;
					}
					bool covered = unloaded;
					// from just above the region's top, up through every chunk above
					for (int32_t ly = light_dim - light_pad; ly < chunks_above * chunk_height && !covered; ++ly) {
						const Ref<ChunkInfo>& chunk = column[ly / chunk_height];
						if (!chunk.is_valid() || chunk->m_block_types == nullptr) {
							ly += chunk_height - 1 - ly % chunk_height;
							continue;
						}
						const BlockIndex inner(lx - dx * chunk_width, ly % chunk_height, lz - dz * chunk_depth);
						covered = !is_open(chunk->m_block_types[convert_index_to_address(inner)]);
					}
					roofed[z * light_dim + x] = covered;
				}
			}
		}
	}

	// sky light: full straight down every open column until something stops it, then spreading sideways
	LocalVector<uint8_t> sky;
	sky.resize(light_cells);
	memset(sky.ptr(), 0, light_cells);
	LocalVector<int32_t> queue;
	for (int32_t z = 0; z < light_dim; ++z) {
		for (int32_t x = 0; x < light_dim; ++x) {
			if (roofed[z * light_dim + x]) {
				continue;
			}
			for (int32_t y = light_dim - 1; y >= 0; --y) {
				const int32_t i = region_index(x, y, z);
				if (!is_open(types[i])) {
					break;
				}
				sky[i] = max_light;
			}
		}
	}
	// only the edges of the sunlit columns spread anything: above and below a sunlit block are sunlit or closed, so a
	// sunlit block whose open neighbours beside it are all sunlit too can't light anything further. In open country
	// that's most of them, and leaving them out of the queue gives the same light for far less work
	static const int32_t sideways[4] = { 1, -1, light_dim, -light_dim };
	for (int32_t z = 0; z < light_dim; ++z) {
		for (int32_t x = 0; x < light_dim; ++x) {
			const bool inside[4] = { x + 1 < light_dim, x > 0, z + 1 < light_dim, z > 0 };
			for (int32_t y = 0; y < light_dim; ++y) {
				const int32_t i = region_index(x, y, z);
				if (sky[i] != max_light) {
					continue;
				}
				for (int32_t s = 0; s < 4; ++s) {
					const int32_t n = i + sideways[s];
					if (inside[s] && sky[n] != max_light && is_open(types[n])) {
						queue.push_back(i);
						break;
					}
				}
			}
		}
	}
	spread_light(sky.ptr(), types.ptr(), builder, queue);

	// block light: from everything that gives it off
	LocalVector<uint8_t> glow;
	glow.resize(light_cells);
	memset(glow.ptr(), 0, light_cells);
	queue.clear();
	for (int32_t i = 0; i < light_cells; ++i) {
		const int32_t emission = builder->get_block_emission(types[i]);
		if (emission > 0) {
			glow[i] = (uint8_t)MIN(emission, max_light);
			queue.push_back(i);
		}
	}
	spread_light(glow.ptr(), types.ptr(), builder, queue);

	// kept for things that aren't blocks to be lit by, through CubeBuilder::get_light_at
	m_light.resize(chunk_width * chunk_height * chunk_depth);
	for (int32_t ly = 0; ly < chunk_height; ++ly) {
		for (int32_t lz = 0; lz < chunk_depth; ++lz) {
			for (int32_t lx = 0; lx < chunk_width; ++lx) {
				const int32_t i = region_index(lx + light_pad, ly + light_pad, lz + light_pad);
				m_light[convert_index_to_address(BlockIndex(lx, ly, lz))] = uint8_t((sky[i] << 4) | glow[i]);
			}
		}
	}

	// gathered here and copied into packed arrays once at the end, because every append to a
	// packed array is a call into the engine
	LocalVector<Vector3> verts;
	LocalVector<Vector3> normals;
	LocalVector<Vector2> uvs;
	LocalVector<Vector2> layers;
	LocalVector<Color> colors;
	LocalVector<float> lights;
	LocalVector<int32_t> indices;

	for (int32_t ly = 0; ly < chunk_height; ++ly) {
		for (int32_t lz = 0; lz < chunk_depth; ++lz) {
			for (int32_t lx = 0; lx < chunk_width; ++lx) {
				const Vector3i block(lx + light_pad, ly + light_pad, lz + light_pad);
				const BlockType& block_type = types[region_index(block.x, block.y, block.z)];
				// faces are drawn against air and against hidden blocks, which draw themselves
				if (is_open(block_type)) {
					continue;
				}

				const Vector3 block_center = Vector3(lx, ly, lz);
				const Color block_color = builder->get_block_color(block_type);
				for (int32_t d = 0; d < BlockDirection::COUNT; ++d) {
					const Vector3i front = block + face_dirs[d];
					const int32_t front_index = region_index(front.x, front.y, front.z);
					if (!is_open(types[front_index])) {
						continue;
					}

					// the two directions across the face
					int32_t across[2];
					int32_t found = 0;
					for (int32_t axis = 0; axis < 3; ++axis) {
						if (face_dirs[d][axis] == 0) {
							across[found++] = axis;
						}
					}

					const int32_t base = verts.size();
					const Vector3 normal = Vector3(face_dirs[d]);
					const Vector2 layer(builder->get_block_face_layer(block_type, d), 0);
					float shading[4];
					for (int32_t c = 0; c < 4; ++c) {
						const Vector3 corner = face_corners[d * 4 + c];
						verts.push_back(block_center + corner);
						normals.push_back(normal);
						uvs.push_back(face_uvs[c]);
						layers.push_back(layer);
						colors.push_back(block_color);

						// smooth light: the corner averages the block in front of the face with the ones beside and
						// diagonally beside it on that corner's side, and is shaded by however many of those are solid
						Vector3i side1 = front;
						Vector3i side2 = front;
						side1[across[0]] += corner[across[0]] > 0.5f ? 1 : -1;
						side2[across[1]] += corner[across[1]] > 0.5f ? 1 : -1;
						Vector3i diagonal = side1;
						diagonal[across[1]] = side2[across[1]];
						const int32_t i1 = region_index(side1.x, side1.y, side1.z);
						const int32_t i2 = region_index(side2.x, side2.y, side2.z);
						const int32_t i3 = region_index(diagonal.x, diagonal.y, diagonal.z);
						const bool open1 = is_open(types[i1]);
						const bool open2 = is_open(types[i2]);
						// the diagonal is hidden when both sides are solid
						const bool open3 = (open1 || open2) && is_open(types[i3]);
						float sky_sum = sky[front_index];
						float glow_sum = glow[front_index];
						int32_t count = 1;
						if (open1) { sky_sum += sky[i1]; glow_sum += glow[i1]; count++; }
						if (open2) { sky_sum += sky[i2]; glow_sum += glow[i2]; count++; }
						if (open3) { sky_sum += sky[i3]; glow_sum += glow[i3]; count++; }
						const int32_t blocking = (!open1 && !open2) ? 3 : int32_t(!open1) + int32_t(!open2) + int32_t(!is_open(types[i3]));
						shading[c] = corner_shading[blocking];
						lights.push_back(sky_sum / count);
						lights.push_back(glow_sum / count);
						lights.push_back(shading[c]);
						lights.push_back(0.0f);
					}

					// split the quad along whichever diagonal keeps the corner shading from smearing across it
					if (shading[0] + shading[2] >= shading[1] + shading[3]) {
						indices.push_back(base);
						indices.push_back(base + 1);
						indices.push_back(base + 2);
						indices.push_back(base);
						indices.push_back(base + 2);
						indices.push_back(base + 3);
					} else {
						indices.push_back(base + 1);
						indices.push_back(base + 2);
						indices.push_back(base + 3);
						indices.push_back(base + 1);
						indices.push_back(base + 3);
						indices.push_back(base);
					}
				}
			}
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
	mesh_attributes[Mesh::ARRAY_TEX_UV2] = to_packed<PackedVector2Array>(layers);
	mesh_attributes[Mesh::ARRAY_COLOR] = to_packed<PackedColorArray>(colors);
	mesh_attributes[Mesh::ARRAY_CUSTOM0] = to_packed<PackedFloat32Array>(lights);
	mesh_attributes[Mesh::ARRAY_INDEX] = to_packed<PackedInt32Array>(indices);

	const int64_t light_format = int64_t(Mesh::ARRAY_CUSTOM_RGBA_FLOAT) << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT;
	new_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, mesh_attributes, Array(), Dictionary(), BitField<Mesh::ArrayFormat>(light_format));
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
	ClassDB::bind_method(D_METHOD("set_column_loaded", "column"), &CubeBuilder::set_column_loaded);
	ClassDB::bind_static_method("CubeBuilder", D_METHOD("get_chunk_dimensions"), &CubeBuilder::get_chunk_dimensions);
	ClassDB::bind_method(D_METHOD("get_block"), &CubeBuilder::get_block);
	ClassDB::bind_method(D_METHOD("set_block_colors", "block_colors"), &CubeBuilder::set_block_colors);
	ClassDB::bind_method(D_METHOD("get_block_colors"), &CubeBuilder::get_block_colors);
	ClassDB::bind_method(D_METHOD("set_block_face_layers", "face_layers"), &CubeBuilder::set_block_face_layers);
	ClassDB::bind_method(D_METHOD("get_block_face_layers"), &CubeBuilder::get_block_face_layers);
	ClassDB::bind_method(D_METHOD("set_block_light_emission", "emission"), &CubeBuilder::set_block_light_emission);
	ClassDB::bind_method(D_METHOD("get_block_light_emission"), &CubeBuilder::get_block_light_emission);
	ClassDB::bind_method(D_METHOD("get_light_at", "global_block_index"), &CubeBuilder::get_light_at);
	ClassDB::bind_method(D_METHOD("get_chunk_geometry_version", "chunk_index"), &CubeBuilder::get_chunk_geometry_version);
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
	m_loaded_columns.clear();
}

void CubeBuilder::set_column_loaded(const Vector2i& column) {
	if (m_loaded_columns.has(column)) {
		return;
	}
	m_loaded_columns.insert(column);
	// chunks beside it lit and drawn as if it were solid ground; set_chunk_blocks already marks the ones next to its
	// chunks with blocks, which leaves the ones beside its air, like a cliff face standing over it. Only the four
	// columns beside it had faces against it; the ones on its corners only get its light
	for (int32_t dz = -1; dz <= 1; ++dz) {
		for (int32_t dx = -1; dx <= 1; ++dx) {
			if (dx == 0 && dz == 0) {
				continue;
			}
			for (int32_t y = 0; y < max_chunk_layers; ++y) {
				Ref<ChunkInfo> neighbor = m_chunks.get(ChunkIndex(column.x + dx, y, column.y + dz), nullptr);
				if (neighbor.is_valid()) {
					mark_chunk_for_rebuild(neighbor, false, dx == 0 || dz == 0);
				}
			}
		}
	}
}

void CubeBuilder::mark_chunk_for_rebuild(const Ref<ChunkInfo>& chunk, bool urgent, bool geometry) {
	// a chunk can end up in both queues; whichever reaches it second finds it clean and skips it
	if (urgent) {
		m_urgent_chunks.push_back(chunk);
	} else if (!chunk->is_chunk_dirty()) {
		m_dirty_chunks.push_back(chunk);
	}
	chunk->mark_chunk_dirty(geometry);
}

int32_t CubeBuilder::get_chunk_geometry_version(const ChunkIndex& chunk_index) const {
	Ref<ChunkInfo> chunk = m_chunks.get(chunk_index, nullptr);
	return chunk.is_valid() ? chunk->get_geometry_version() : 0;
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

void CubeBuilder::set_block_face_layers(const PackedInt32Array& face_layers) {
	CONCURRENCY_CHECK(m_grid_info_guard);
	m_block_face_layers = face_layers;

	// every existing chunk was meshed with the old layers
	Array values = m_chunks.values();
	for (int32_t i = 0; i < values.size(); i++) {
		const Ref<ChunkInfo>& chunk = values[i];
		if (chunk.is_valid()) {
			mark_chunk_for_rebuild(chunk);
		}
	}
}

void CubeBuilder::set_block_light_emission(const PackedInt32Array& emission) {
	CONCURRENCY_CHECK(m_grid_info_guard);
	m_block_light_emission = emission;
	m_emission_by_type.resize(emission.size());
	for (int32_t i = 0; i < emission.size(); ++i) {
		m_emission_by_type[i] = (uint8_t)CLAMP(emission[i], 0, max_light);
	}

	// every existing chunk was lit with the old emission
	Array values = m_chunks.values();
	for (int32_t i = 0; i < values.size(); i++) {
		const Ref<ChunkInfo>& chunk = values[i];
		if (chunk.is_valid()) {
			mark_chunk_for_rebuild(chunk);
		}
	}
}

PackedInt32Array CubeBuilder::get_block_light_emission() const {
	return m_block_light_emission;
}

void CubeBuilder::mark_light_around(const GlobalBlockIndex& global_block_index) {
	const ChunkIndex center = get_chunk_from_global(global_block_index);
	static const int32_t chunks_above = 16;
	const int32_t lowest_chunk_y = center.y - chunks_above;

	// the blocks whose light changes outright: the edit itself, and, when the sky reaches it, the column below it as
	// far as the first block that stops light, which the sky now lights all the way down, or no longer does
	int32_t lowest = global_block_index.y;
	bool sky_reaches = true;
	for (int32_t y = global_block_index.y + 1; y < (center.y + chunks_above) * chunk_height && sky_reaches; ++y) {
		const GlobalBlockIndex above(global_block_index.x, y, global_block_index.z);
		Ref<ChunkInfo> chunk = get_chunk(above);
		if (!chunk.is_valid()) {
			// a whole chunk of air
			y += chunk_height - 1 - convert_global_index_to_block_index(above).y;
			continue;
		}
		sky_reaches = lets_light_through(chunk->get_block_at(convert_global_index_to_block_index(above)));
	}
	if (sky_reaches) {
		int32_t stop = global_block_index.y - 1;
		while (stop >= lowest_chunk_y * chunk_height) {
			const GlobalBlockIndex below(global_block_index.x, stop, global_block_index.z);
			Ref<ChunkInfo> chunk = get_chunk(below);
			if (!chunk.is_valid()) {
				// a whole chunk of air, or the bottom of the world
				if (stop < 0) {
					break;
				}
				stop -= convert_global_index_to_block_index(below).y + 1;
				continue;
			}
			if (!lets_light_through(chunk->get_block_at(convert_global_index_to_block_index(below)))) {
				break;
			}
			--stop;
		}
		lowest = MIN(stop + 1, global_block_index.y);
	}

	// light spreads from those a level less per block, so it changes nowhere more than max_light blocks away (counting
	// along the axes, the way it spreads). A chunk's faces show the light just outside them, so a chunk counts as
	// reaching a block beyond its edges; chunks further off are lit the same either way. That's usually far fewer
	// than all the chunks round the edit, and every one rebuilt costs a whole relight. Only their light changed,
	// not their faces; set_block marks the chunks whose faces did.
	const int32_t lowest_y = get_chunk_from_global(GlobalBlockIndex(global_block_index.x, lowest - max_light, global_block_index.z)).y;
	for (int32_t cy = MAX(lowest_y, lowest_chunk_y); cy <= center.y + 1; ++cy) {
		for (int32_t dz = -1; dz <= 1; ++dz) {
			for (int32_t dx = -1; dx <= 1; ++dx) {
				const ChunkIndex index(center.x + dx, cy, center.z + dz);
				Ref<ChunkInfo> chunk = m_chunks.get(index, nullptr);
				if (!chunk.is_valid()) {
					continue;
				}
				// the chunk and the blocks just outside it
				const Vector3i first(index.x * chunk_width - 1, index.y * chunk_height - 1, index.z * chunk_depth - 1);
				const Vector3i last(first.x + chunk_width + 1, first.y + chunk_height + 1, first.z + chunk_depth + 1);
				const int32_t gap_x = MAX(0, MAX(first.x - global_block_index.x, global_block_index.x - last.x));
				const int32_t gap_z = MAX(0, MAX(first.z - global_block_index.z, global_block_index.z - last.z));
				const int32_t gap_y = MAX(0, MAX(first.y - global_block_index.y, lowest - last.y));
				if (gap_x + gap_y + gap_z <= max_light) {
					mark_chunk_for_rebuild(chunk, false, false);
				}
			}
		}
	}
}

PackedInt32Array CubeBuilder::get_block_face_layers() const {
	return m_block_face_layers;
}

int32_t CubeBuilder::get_block_face_layer(const BlockType& block_type, int32_t direction) const {
	const int64_t address = (int64_t)block_type * BlockDirection::COUNT + direction;
	if (block_type < 0 || address >= m_block_face_layers.size()) {
		return -1;
	}
	return m_block_face_layers[address];
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
	const BlockType old_type = chunk->get_block_at(block_index);
	mark_chunk_for_rebuild(chunk, true);
	chunk->set_block_type(block_index, block_type);

	// a block that lets light through differently, or gives off different light, relights what's around it
	if (lets_light_through(old_type) != lets_light_through(block_type) || get_block_emission(old_type) != get_block_emission(block_type)) {
		mark_light_around(global_block_index);
	}

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

	// neighbors may have faces against this chunk that are now hidden, or newly exposed, and light from it, or
	// shade, reaches every chunk all round. Only the six beside it share faces with it; the rest only get its light
	for (int32_t i = 0; i < 27; ++i) {
		const Vector3i offset(i % 3 - 1, (i / 3) % 3 - 1, i / 9 - 1);
		Ref<ChunkInfo> neighbor = m_chunks.get(chunk_index + offset, nullptr);
		if (neighbor.is_valid() && neighbor != chunk) {
			mark_chunk_for_rebuild(neighbor, false, ABS(offset.x) + ABS(offset.y) + ABS(offset.z) == 1);
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