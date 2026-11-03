#pragma once

#include "cube_builder.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

namespace godot {

class ArrayMesh;

/// The part of the world's water that runs per cell: how it flows over a CubeBuilder's blocks, and the mesh drawn
/// for a chunk of it. WaterController (water_controller.gd) keeps the node, the timing, who simulates and the
/// world's bookkeeping, and describes how water moves; this does the same, cell for cell, without a call into
/// scripts per block. Every cell of water holds up to FULL_VOLUME units, shown as the nearest of eight levels, each
/// its own block type.
class WaterSimulation : public RefCounted {
	GDCLASS(WaterSimulation, RefCounted);

private:
	Ref<CubeBuilder> cube_builder;
	// Takes a block position and says whether its column is loaded. Asked once a column per call, and water beside
	// a column that isn't waits.
	Callable loaded_check;
	// The block type showing each level, shallowest first, and the level each block type shows, 0 for not water.
	PackedInt32Array level_block_types;
	LocalVector<uint8_t> level_of;

	// Cells whose water isn't exactly what their block shows: in between levels, or too little to show at all.
	HashMap<Vector3i, int32_t> volumes;
	// The cells waiting to be looked at, in order, and the same as a set.
	LocalVector<Vector3i> queue;
	HashSet<Vector3i> active;

	// Blocks changed since take_changes, in the order they first changed, with what each was before.
	LocalVector<Vector3i> changed;
	HashMap<Vector3i, BlockType> changed_from;

	// The two blocks the player stands in, which water leaves alone.
	bool has_player_cell = false;
	Vector3i player_cell;

	// Looked up at most once a call: the chunks read, null for one that doesn't exist, and the columns asked about.
	HashMap<ChunkIndex, Ref<ChunkInfo>> chunk_cache;
	HashMap<Vector2i, bool> loaded_cache;
	// The last of each read, since most reads land in the same chunk as the one before. The chunk is held by
	// chunk_cache.
	bool has_last_chunk = false;
	ChunkIndex last_chunk_index;
	ChunkInfo *last_chunk = nullptr;
	bool has_last_column = false;
	Vector2i last_column;
	bool last_column_loaded = false;

	void _forget_cached();
	BlockType _block_at(const Vector3i &p_position);
	void _write_block(const Vector3i &p_position, BlockType p_block_type);
	bool _is_column_loaded(const Vector3i &p_position);
	bool _is_loaded_around(const Vector3i &p_position);
	bool _is_player_in(const Vector3i &p_position) const;
	bool _is_open(const Vector3i &p_position);
	int32_t _volume_at(const Vector3i &p_position);
	bool _has_room(const Vector3i &p_position);
	void _set_volume(const Vector3i &p_position, int32_t p_volume);
	void _wake_around(const Vector3i &p_position);
	void _update(const Vector3i &p_position);

protected:
	static void _bind_methods();

public:
	void set_cube_builder(const Ref<CubeBuilder> &p_cube_builder);
	Ref<CubeBuilder> get_cube_builder() const;
	void set_loaded_check(const Callable &p_loaded_check);
	Callable get_loaded_check() const;
	void set_level_block_types(const PackedInt32Array &p_block_types);
	PackedInt32Array get_level_block_types() const;

	bool is_water(int64_t p_block_type) const;
	int64_t get_level(int64_t p_block_type) const;
	int64_t get_block_for(int64_t p_level) const;

	int64_t get_volume_at(const Vector3i &p_position);
	bool is_open(const Vector3i &p_position);
	int64_t add_water(const Vector3i &p_position, int64_t p_amount);
	int64_t take_water(const Vector3i &p_position, int64_t p_amount);
	void wake(const Vector3i &p_position);
	int64_t get_pending() const;
	// A block changed by anyone else: whatever water was there is replaced by exactly what the block shows.
	void on_block_changed(const Vector3i &p_position, int64_t p_block_type);

	void set_player_cell(const Vector3i &p_cell);
	void clear_player_cell();

	// One flow step over the cells waiting, stopping after max_updates cells or max_millis milliseconds and carrying
	// the rest over. Returns how many cells it looked at.
	int64_t step(int64_t p_max_updates, double p_max_millis);
	// The blocks this has changed in the cube builder since the last call, as x, y, z and block type, four ints
	// each, leaving out any that ended up as they started.
	PackedInt32Array take_changes();

	// The water surfaces in a chunk, in its own coordinates, or null when it holds none. block_kinds gives each block
	// type's drawn level, 0 for what water shows a face against or 255 for solid.
	static Ref<ArrayMesh> build_mesh(const Ref<CubeBuilder> &p_cube_builder, const Vector3i &p_chunk_index, const PackedByteArray &p_block_kinds, float p_surface);
};

} // namespace godot
