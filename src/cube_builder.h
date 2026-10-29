#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/typed_dictionary.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include "concurrency_guard.h"

namespace godot {

using BlockType = int32_t;
using GlobalBlockIndex = Vector3i;
using ChunkIndex = Vector3i;
using BlockIndex = Vector3i;

class ArrayMesh;

static const int32_t chunk_width = 16;
static const int32_t chunk_height = 16;
static const int32_t chunk_depth = 16;

// the brightest light, in levels; light loses a level for every block it spreads, so it reaches this far
static const int32_t max_light = 15;

enum BlockDirection {
	TOP,
	BOTTOM,
	LEFT, //x
	RIGHT, //-x
	FORWARD, //+z
	BACKWARD, //-z
	COUNT,
};

class CubeBuilder;

class ChunkInfo : public RefCounted {
	GDCLASS(ChunkInfo, RefCounted)
public:
	ChunkInfo();
	~ChunkInfo();

	void initialize(const ChunkIndex& index);
	void shutdown();

	void set_block_type(const BlockIndex& index, const BlockType& block_type);
	BlockType get_block_at(const BlockIndex& index);

	Ref<ArrayMesh> rebuild_chunk_geometry(const Ref<CubeBuilder>& builder);

	// blocks are ordered x first, then z, then y
	void copy_blocks_from(const PackedInt32Array& blocks);

	ChunkIndex get_chunk_index() const { return m_chunk_index; }
	// the sky light and block light at a block in this chunk as of its last rebuild, as (sky, block); full sky and no
	// block light before its first
	Vector2i get_light_at(const BlockIndex& index) const;
	void mark_chunk_dirty() { m_chunk_dirty = true; }
	bool is_chunk_dirty() const { return m_chunk_dirty; }
protected:
	static void _bind_methods();
	static int32_t convert_index_to_address(const BlockIndex& index);
	static BlockIndex convert_address_to_index(const int32_t& address);

	static int32_t get_neighbor_index(const int32_t& index, BlockDirection direction);

	ChunkIndex m_chunk_index;
	BlockType* m_block_types;
	bool m_chunk_dirty;
	// every block's light from the last rebuild, sky light in the high four bits and block light in the low four
	LocalVector<uint8_t> m_light;
};

class CubeBuilder : public Resource {
	GDCLASS(CubeBuilder, Resource)
private:
protected:
	static void _bind_methods();

	TypedDictionary<ChunkIndex, Ref<ChunkInfo>> m_chunks; //index to ChunkInfo

	Ref<ChunkInfo> get_or_create_chunk(const GlobalBlockIndex& index);
	Ref<ChunkInfo> get_chunk(const GlobalBlockIndex& index) const;
	static ChunkIndex get_chunk_from_global(const GlobalBlockIndex& index);

	// call before changing a chunk's blocks; queues it for rebuild_chunk_geometry if it isn't already.
	// urgent chunks are rebuilt on the next call ahead of everything else, whatever its max_chunks
	void mark_chunk_for_rebuild(const Ref<ChunkInfo>& chunk, bool urgent = false);
	// oldest first, so rebuild_chunk_geometry never has to walk every chunk looking for dirty ones
	LocalVector<Ref<ChunkInfo>> m_dirty_chunks;
	// single block edits, like the player mining, so they never wait behind a backlog of streamed chunks
	LocalVector<Ref<ChunkInfo>> m_urgent_chunks;
	ConcurrencyGuard m_grid_info_guard;

	PackedColorArray m_block_colors; //indexed by BlockType
	// the texture array layer for each face of each block type, at block_type * COUNT + direction; -1 for none
	PackedInt32Array m_block_face_layers;
	// the light each block type gives off, 0 to max_light; indexed by BlockType
	PackedInt32Array m_block_light_emission;

	// marks every chunk whose light an edit at global_block_index could change: the ones all round it, and, when
	// the sky reaches the edit, every chunk below those too
	void mark_light_around(const GlobalBlockIndex& global_block_index);
	// block types left out of the chunk meshes, like ones drawn by their own nodes; indexed by BlockType
	LocalVector<uint8_t> m_hidden_block_types;

	// the columns of chunks set_column_loaded was told about. Once there are any, a missing chunk in a column
	// that isn't one is ground not loaded yet rather than air
	HashSet<Vector2i> m_loaded_columns;

public:
	void set_block(const GlobalBlockIndex& global_block_index, const BlockType& blockType);
	// replaces a whole chunk at once; blocks are ordered x first, then z, then y
	void set_chunk_blocks(const ChunkIndex& chunk_index, const PackedInt32Array& blocks);
	int get_block(const GlobalBlockIndex& global_block_index) const;
	// null when the chunk hasn't been created, which means it's all air, or not loaded (see is_column_unloaded)
	Ref<ChunkInfo> find_chunk(const ChunkIndex& chunk_index) const;
	void clear_all_blocks();

	// tells the builder every chunk of a column (x, z) has been set, all air or not. Until it's told about any,
	// every missing chunk is air. After, a missing chunk in a column it hasn't been told about is ground that isn't
	// loaded yet: no light comes through it and no faces are drawn against it, so caves at the edge of the loaded
	// world don't light up as if open to the sky. Neighbours are rebuilt when the column arrives.
	void set_column_loaded(const Vector2i& column);
	bool is_column_unloaded(const Vector2i& column) const {
		return !m_loaded_columns.is_empty() && !m_loaded_columns.has(column);
	}

	void set_block_colors(const PackedColorArray& block_colors);
	PackedColorArray get_block_colors() const;
	Color get_block_color(const BlockType& block_type) const;

	// which layer of the chunk material's texture array each block face shows, written into every face's UV2.x:
	// block_type * 6 + direction (top, bottom, +x, -x, +z, -z), -1 for faces drawn in the block's colour alone
	void set_block_face_layers(const PackedInt32Array& face_layers);
	PackedInt32Array get_block_face_layers() const;
	int32_t get_block_face_layer(const BlockType& block_type, int32_t direction) const;

	// how much light each block type gives off, 0 to 15, like a torch's 14. Chunk meshes carry the light at every
	// face corner in CUSTOM0: (sky light 0-15, block light 0-15, corner shading 0-1, 0). Sky light is full in
	// columns open to the sky and fades a level per block sideways into overhangs and tunnels; block light fades
	// a level per block from whatever gives it off. Only air and hidden blocks let light through.
	void set_block_light_emission(const PackedInt32Array& emission);
	PackedInt32Array get_block_light_emission() const;
	int32_t get_block_emission(const BlockType& block_type) const {
		return block_type >= 0 && block_type < m_block_light_emission.size() ? m_block_light_emission[block_type] : 0;
	}
	// light passes through air and through hidden blocks, which draw themselves thin
	bool lets_light_through(const BlockType& block_type) const {
		return block_type == 0 || is_block_type_hidden(block_type);
	}

	// these block types get no faces in the chunk meshes, and the faces of blocks next to them are drawn as if
	// beside air, so something else can draw them. They're still solid blocks for get_block and is_block_air
	void set_hidden_block_types(const PackedInt32Array& block_types);
	PackedInt32Array get_hidden_block_types() const;
	bool is_block_type_hidden(const BlockType& block_type) const {
		return block_type >= 0 && block_type < (BlockType)m_hidden_block_types.size() && m_hidden_block_types[block_type] != 0;
	}

	bool is_block_air(const GlobalBlockIndex& global_block_index) const;

	// the light at a block as the chunk meshes have it, as (sky light, block light), each 0 to 15, for lighting
	// things that aren't blocks, like dropped items, to match. Where no chunk has been built it's open sky.
	Vector2i get_light_at(const GlobalBlockIndex& global_block_index) const;

	static Vector3i get_chunk_dimensions();
	static BlockIndex convert_global_index_to_block_index(const GlobalBlockIndex& global_block_index);

	// rebuilds up to max_chunks dirty chunks (all of them when negative); the rest wait for the next call
	void rebuild_chunk_geometry(int32_t max_chunks = -1);

	CubeBuilder();
	~CubeBuilder();
};

}