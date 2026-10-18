#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/typed_dictionary.hpp>
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

	ChunkIndex get_chunk_index() const { return m_chunk_index; }
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
	static ChunkIndex get_chunk_from_global(const GlobalBlockIndex& index);

	bool m_chunks_dirty;
	ConcurrencyGuard m_grid_info_guard;

public:
	void set_block(const GlobalBlockIndex& global_block_index, const BlockType& blockType);
	void clear_all_blocks();

	bool is_block_air(const GlobalBlockIndex& global_block_index) const;

	static Vector3i get_chunk_dimensions();
	static BlockIndex convert_global_index_to_block_index(const GlobalBlockIndex& global_block_index);

	void rebuild_chunk_geometry();

	CubeBuilder();
	~CubeBuilder();
};

}