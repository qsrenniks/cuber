#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/typed_dictionary.hpp>
#include <godot_cpp/templates/local_vector.hpp>
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
	// block types left out of the chunk meshes, like ones drawn by their own nodes; indexed by BlockType
	LocalVector<uint8_t> m_hidden_block_types;

public:
	void set_block(const GlobalBlockIndex& global_block_index, const BlockType& blockType);
	// replaces a whole chunk at once; blocks are ordered x first, then z, then y
	void set_chunk_blocks(const ChunkIndex& chunk_index, const PackedInt32Array& blocks);
	int get_block(const GlobalBlockIndex& global_block_index) const;
	// null when the chunk hasn't been created, which means it's all air
	Ref<ChunkInfo> find_chunk(const ChunkIndex& chunk_index) const;
	void clear_all_blocks();

	void set_block_colors(const PackedColorArray& block_colors);
	PackedColorArray get_block_colors() const;
	Color get_block_color(const BlockType& block_type) const;

	// these block types get no faces in the chunk meshes, and the faces of blocks next to them are drawn as if
	// beside air, so something else can draw them. They're still solid blocks for get_block and is_block_air
	void set_hidden_block_types(const PackedInt32Array& block_types);
	PackedInt32Array get_hidden_block_types() const;
	bool is_block_type_hidden(const BlockType& block_type) const {
		return block_type >= 0 && block_type < (BlockType)m_hidden_block_types.size() && m_hidden_block_types[block_type] != 0;
	}

	bool is_block_air(const GlobalBlockIndex& global_block_index) const;

	static Vector3i get_chunk_dimensions();
	static BlockIndex convert_global_index_to_block_index(const GlobalBlockIndex& global_block_index);

	// rebuilds up to max_chunks dirty chunks (all of them when negative); the rest wait for the next call
	void rebuild_chunk_geometry(int32_t max_chunks = -1);

	CubeBuilder();
	~CubeBuilder();
};

}