#include "water_simulation.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <cstring>
#include <utility>

using namespace godot;

namespace {

// How many levels are drawn; a full block is the last.
constexpr int32_t FULL = 8;
// Units of water in a full block, and in one drawn level. Matches WaterController.FULL_VOLUME.
constexpr int32_t FULL_VOLUME = 8000;
constexpr int32_t LEVEL_VOLUME = FULL_VOLUME / FULL;

const Vector3i UP(0, 1, 0);
const Vector3i DOWN(0, -1, 0);
// Left, right, forward and back, as WaterController.SIDES has them. The order matters: the first side with a drop
// gets the odd units.
const Vector3i SIDES[4] = { Vector3i(-1, 0, 0), Vector3i(1, 0, 0), Vector3i(0, 0, -1), Vector3i(0, 0, 1) };

// What build_mesh reads a chunk into: the chunk and a block more each way, ordered x, then z, then y.
constexpr int32_t GRID = chunk_width + 2;
constexpr int32_t GRID_LAYER = GRID * GRID;
constexpr int32_t GRID_CELLS = GRID * GRID * GRID;
// Steps through that grid to each of SIDES.
const int32_t SIDE_STEPS[4] = { -1, 1, -GRID, GRID };
// What a block is to water in build_mesh, besides its level: something water shows a face against, or solid.
constexpr uint8_t KIND_OPEN = 0;
constexpr uint8_t KIND_SOLID = 255;
static_assert(chunk_width == chunk_height && chunk_height == chunk_depth, "The water grid assumes cubic chunks.");

// Rounds toward negative infinity, so block -1 lands in chunk -1 rather than chunk 0.
int32_t floor_div(int32_t p_value, int32_t p_divisor) {
	const int32_t quotient = p_value / p_divisor;
	return (p_value % p_divisor != 0 && p_value < 0) ? quotient - 1 : quotient;
}

ChunkIndex chunk_of(const Vector3i &p_position) {
	return ChunkIndex(floor_div(p_position.x, chunk_width), floor_div(p_position.y, chunk_height), floor_div(p_position.z, chunk_depth));
}

// The level drawn for a volume: the nearest eighth, so less than a sixteenth shows nothing.
int32_t level_for_volume(int32_t p_volume) {
	return CLAMP((p_volume + LEVEL_VOLUME / 2) / LEVEL_VOLUME, 0, FULL);
}

// How high water of a level is drawn in its block, as WaterRenderer.surface_height: the level in eighths, except a
// full block, which sits at the surface under anything but more water and fills the block under more water.
float surface_height(int32_t p_level, uint8_t p_above, float p_surface) {
	if (p_level >= FULL) {
		return p_above != KIND_OPEN && p_above != KIND_SOLID ? 1.0f : p_surface;
	}
	return float(p_level) / float(FULL);
}

// A quad centred at p_center, reaching p_across and p_up from it, facing p_normal, as two triangles.
void add_face(LocalVector<Vector3> &r_vertices, LocalVector<Vector3> &r_normals, const Vector3 &p_center, const Vector3 &p_across, const Vector3 &p_up, const Vector3 &p_normal) {
	const Vector3 low_left = p_center - p_across - p_up;
	const Vector3 low_right = p_center + p_across - p_up;
	const Vector3 high_right = p_center + p_across + p_up;
	const Vector3 high_left = p_center - p_across + p_up;
	r_vertices.push_back(low_left);
	r_vertices.push_back(high_right);
	r_vertices.push_back(low_right);
	r_vertices.push_back(low_left);
	r_vertices.push_back(high_left);
	r_vertices.push_back(high_right);
	for (int32_t i = 0; i < 6; ++i) {
		r_normals.push_back(p_normal);
	}
}

template <typename TPacked, typename T>
TPacked to_packed(const LocalVector<T> &p_values) {
	TPacked packed;
	packed.resize(p_values.size());
	memcpy(packed.ptrw(), p_values.ptr(), p_values.size() * sizeof(T));
	return packed;
}

} // namespace

void WaterSimulation::set_cube_builder(const Ref<CubeBuilder> &p_cube_builder) {
	cube_builder = p_cube_builder;
	_forget_cached();
}

Ref<CubeBuilder> WaterSimulation::get_cube_builder() const {
	return cube_builder;
}

void WaterSimulation::set_loaded_check(const Callable &p_loaded_check) {
	loaded_check = p_loaded_check;
	_forget_cached();
}

Callable WaterSimulation::get_loaded_check() const {
	return loaded_check;
}

void WaterSimulation::set_level_block_types(const PackedInt32Array &p_block_types) {
	ERR_FAIL_COND_MSG(p_block_types.size() != FULL, vformat("Water needs a block type for each of its %d levels, got %d.", FULL, p_block_types.size()));
	level_block_types = p_block_types;
	level_of.clear();
	for (int32_t i = 0; i < FULL; ++i) {
		const BlockType block_type = p_block_types[i];
		ERR_CONTINUE_MSG(block_type <= 0, vformat("Water level %d can't be shown by block type %d.", i + 1, block_type));
		while (level_of.size() <= uint32_t(block_type)) {
			level_of.push_back(0);
		}
		level_of[block_type] = uint8_t(i + 1);
	}
}

PackedInt32Array WaterSimulation::get_level_block_types() const {
	return level_block_types;
}

bool WaterSimulation::is_water(int64_t p_block_type) const {
	return get_level(p_block_type) > 0;
}

int64_t WaterSimulation::get_level(int64_t p_block_type) const {
	return p_block_type >= 0 && p_block_type < int64_t(level_of.size()) ? level_of[p_block_type] : 0;
}

int64_t WaterSimulation::get_block_for(int64_t p_level) const {
	if (p_level <= 0) {
		return 0;
	}
	ERR_FAIL_COND_V_MSG(level_block_types.size() != FULL, 0, "Water has no block types for its levels.");
	return level_block_types[MIN(p_level, int64_t(FULL)) - 1];
}

void WaterSimulation::_forget_cached() {
	// Between calls chunks come and go, columns load and the world can be cleared.
	chunk_cache.clear();
	loaded_cache.clear();
	has_last_chunk = false;
	last_chunk = nullptr;
	has_last_column = false;
}

BlockType WaterSimulation::_block_at(const Vector3i &p_position) {
	const ChunkIndex index = chunk_of(p_position);
	if (!has_last_chunk || index != last_chunk_index) {
		Ref<ChunkInfo> *cached = chunk_cache.getptr(index);
		if (cached == nullptr) {
			cached = &chunk_cache.insert(index, cube_builder->find_chunk(index))->value;
		}
		has_last_chunk = true;
		last_chunk_index = index;
		last_chunk = cached->ptr();
	}
	if (last_chunk == nullptr) {
		return 0;
	}
	return last_chunk->get_block_at(p_position - index * CubeBuilder::get_chunk_dimensions());
}

void WaterSimulation::_write_block(const Vector3i &p_position, BlockType p_block_type) {
	if (!changed_from.has(p_position)) {
		changed_from.insert(p_position, _block_at(p_position));
		changed.push_back(p_position);
	}
	cube_builder->set_block(p_position, p_block_type);
	// Writing into a chunk that didn't exist creates it.
	chunk_cache.erase(chunk_of(p_position));
	has_last_chunk = false;
	last_chunk = nullptr;
}

bool WaterSimulation::_is_column_loaded(const Vector3i &p_position) {
	if (!loaded_check.is_valid()) {
		return true;
	}
	const Vector2i column(floor_div(p_position.x, chunk_width), floor_div(p_position.z, chunk_depth));
	if (has_last_column && column == last_column) {
		return last_column_loaded;
	}
	const bool *known = loaded_cache.getptr(column);
	bool loaded;
	if (known != nullptr) {
		loaded = *known;
	} else {
		loaded = loaded_check.call(p_position);
		loaded_cache.insert(column, loaded);
	}
	has_last_column = true;
	last_column = column;
	last_column_loaded = loaded;
	return loaded;
}

bool WaterSimulation::_is_loaded_around(const Vector3i &p_position) {
	if (!_is_column_loaded(p_position)) {
		return false;
	}
	for (const Vector3i &side : SIDES) {
		if (!_is_column_loaded(p_position + side)) {
			return false;
		}
	}
	return true;
}

bool WaterSimulation::_is_player_in(const Vector3i &p_position) const {
	return has_player_cell && (p_position == player_cell || p_position == player_cell + UP);
}

bool WaterSimulation::_is_open(const Vector3i &p_position) {
	const BlockType block_type = _block_at(p_position);
	return block_type == 0 || is_water(block_type);
}

int32_t WaterSimulation::_volume_at(const Vector3i &p_position) {
	const int32_t *volume = volumes.getptr(p_position);
	if (volume != nullptr) {
		return *volume;
	}
	return int32_t(get_level(_block_at(p_position))) * LEVEL_VOLUME;
}

bool WaterSimulation::_has_room(const Vector3i &p_position) {
	return _is_open(p_position) && _volume_at(p_position) < FULL_VOLUME && !_is_player_in(p_position);
}

void WaterSimulation::_set_volume(const Vector3i &p_position, int32_t p_volume) {
	const int32_t volume = CLAMP(p_volume, 0, FULL_VOLUME);
	const int32_t level = level_for_volume(volume);
	if (volume == level * LEVEL_VOLUME) {
		volumes.erase(p_position);
	} else {
		volumes[p_position] = volume;
	}
	const BlockType block_type = BlockType(get_block_for(level));
	if (_block_at(p_position) != block_type) {
		_write_block(p_position, block_type);
	}
	_wake_around(p_position);
}

void WaterSimulation::_wake_around(const Vector3i &p_position) {
	static const Vector3i AROUND[7] = { Vector3i(), UP, DOWN, SIDES[0], SIDES[1], SIDES[2], SIDES[3] };
	for (const Vector3i &offset : AROUND) {
		const Vector3i at = p_position + offset;
		if (_volume_at(at) > 0) {
			wake(at);
		}
	}
}

void WaterSimulation::_update(const Vector3i &p_position) {
	const int32_t volume = _volume_at(p_position);
	if (volume == 0 || !_is_loaded_around(p_position) || p_position.y <= 0) {
		return;
	}

	// Down first: into air, or topping up water with room.
	const Vector3i below = p_position + DOWN;
	if (_has_room(below)) {
		const int32_t below_volume = _volume_at(below);
		const int32_t moved = MIN(volume, FULL_VOLUME - below_volume);
		_set_volume(below, below_volume + moved);
		_set_volume(p_position, volume - moved);
		return;
	}

	// Then over any edge: a lower neighbour with a drop under it takes everything, like water over a lip.
	Vector3i holes[4];
	int32_t hole_count = 0;
	Vector3i lower[4];
	int32_t lower_count = 0;
	for (const Vector3i &side : SIDES) {
		const Vector3i at = p_position + side;
		if (!_is_open(at) || _is_player_in(at) || _volume_at(at) >= volume) {
			continue;
		}
		if (_has_room(at + DOWN)) {
			holes[hole_count++] = at;
		} else {
			lower[lower_count++] = at;
		}
	}
	if (hole_count > 0) {
		const int32_t share = volume / hole_count;
		int32_t left = volume;
		for (int32_t i = 0; i < hole_count; ++i) {
			const int32_t hole_volume = _volume_at(holes[i]);
			const int32_t moved = MIN(i == 0 ? share + volume % hole_count : share, FULL_VOLUME - hole_volume);
			_set_volume(holes[i], hole_volume + moved);
			left -= moved;
		}
		_set_volume(p_position, left);
		return;
	}

	// Else share out evenly with the lower neighbours, the odd units staying here, so once every neighbour is within
	// a unit nothing moves again.
	if (lower_count == 0) {
		return;
	}
	int32_t total = volume;
	for (int32_t i = 0; i < lower_count; ++i) {
		total += _volume_at(lower[i]);
	}
	const int32_t each = total / (lower_count + 1);
	const int32_t keep = total - each * lower_count;
	bool moves = keep != volume;
	for (int32_t i = 0; i < lower_count && !moves; ++i) {
		moves = _volume_at(lower[i]) != each;
	}
	if (!moves) {
		return;
	}
	for (int32_t i = 0; i < lower_count; ++i) {
		_set_volume(lower[i], each);
	}
	_set_volume(p_position, keep);
}

int64_t WaterSimulation::get_volume_at(const Vector3i &p_position) {
	ERR_FAIL_COND_V_MSG(cube_builder.is_null(), 0, "WaterSimulation has no cube_builder.");
	_forget_cached();
	return _volume_at(p_position);
}

bool WaterSimulation::is_open(const Vector3i &p_position) {
	ERR_FAIL_COND_V_MSG(cube_builder.is_null(), false, "WaterSimulation has no cube_builder.");
	_forget_cached();
	return _is_open(p_position);
}

int64_t WaterSimulation::add_water(const Vector3i &p_position, int64_t p_amount) {
	ERR_FAIL_COND_V_MSG(cube_builder.is_null(), 0, "WaterSimulation has no cube_builder.");
	_forget_cached();
	if (p_amount <= 0 || !_is_open(p_position)) {
		return 0;
	}
	const int32_t volume = _volume_at(p_position);
	const int32_t moved = int32_t(MIN(p_amount, int64_t(FULL_VOLUME - volume)));
	_set_volume(p_position, volume + moved);
	return moved;
}

int64_t WaterSimulation::take_water(const Vector3i &p_position, int64_t p_amount) {
	ERR_FAIL_COND_V_MSG(cube_builder.is_null(), 0, "WaterSimulation has no cube_builder.");
	_forget_cached();
	const int32_t volume = _volume_at(p_position);
	const int32_t taken = int32_t(MIN(MAX(p_amount, int64_t(0)), int64_t(volume)));
	if (taken > 0) {
		_set_volume(p_position, volume - taken);
	}
	return taken;
}

void WaterSimulation::wake(const Vector3i &p_position) {
	if (active.has(p_position)) {
		return;
	}
	active.insert(p_position);
	queue.push_back(p_position);
}

int64_t WaterSimulation::get_pending() const {
	return queue.size();
}

void WaterSimulation::on_block_changed(const Vector3i &p_position, int64_t p_block_type) {
	ERR_FAIL_COND_MSG(cube_builder.is_null(), "WaterSimulation has no cube_builder.");
	_forget_cached();
	// A player's block replaces whatever water was there; water they place or mine is exactly what it shows.
	volumes.erase(p_position);
	_wake_around(p_position);
	if (is_water(p_block_type)) {
		wake(p_position);
	}
}

void WaterSimulation::set_player_cell(const Vector3i &p_cell) {
	has_player_cell = true;
	player_cell = p_cell;
}

void WaterSimulation::clear_player_cell() {
	has_player_cell = false;
}

int64_t WaterSimulation::step(int64_t p_max_updates, double p_max_millis) {
	ERR_FAIL_COND_V_MSG(cube_builder.is_null(), 0, "WaterSimulation has no cube_builder.");
	_forget_cached();
	Time *time = Time::get_singleton();
	const uint64_t started = time->get_ticks_usec();
	LocalVector<Vector3i> pending = std::move(queue);
	queue = LocalVector<Vector3i>();
	int64_t looked = 0;
	uint32_t next = 0;
	for (; next < pending.size(); ++next) {
		if (looked >= p_max_updates || double(time->get_ticks_usec() - started) / 1000.0 >= p_max_millis) {
			break;
		}
		const Vector3i position = pending[next];
		active.erase(position);
		_update(position);
		++looked;
	}
	if (next < pending.size()) {
		// The rest keep their place at the front of the line, still marked active.
		LocalVector<Vector3i> rest;
		rest.reserve(pending.size() - next + queue.size());
		for (uint32_t i = next; i < pending.size(); ++i) {
			rest.push_back(pending[i]);
		}
		for (uint32_t i = 0; i < queue.size(); ++i) {
			rest.push_back(queue[i]);
		}
		queue = std::move(rest);
	}
	return looked;
}

PackedInt32Array WaterSimulation::take_changes() {
	PackedInt32Array changes;
	if (changed.is_empty()) {
		return changes;
	}
	ERR_FAIL_COND_V_MSG(cube_builder.is_null(), changes, "WaterSimulation has no cube_builder.");
	_forget_cached();
	changes.resize(changed.size() * 4);
	int32_t *write = changes.ptrw();
	int64_t count = 0;
	for (const Vector3i &position : changed) {
		const BlockType block_type = _block_at(position);
		if (block_type == changed_from[position]) {
			continue;
		}
		write[count++] = position.x;
		write[count++] = position.y;
		write[count++] = position.z;
		write[count++] = block_type;
	}
	changes.resize(count);
	changed.clear();
	changed_from.clear();
	return changes;
}

Ref<ArrayMesh> WaterSimulation::build_mesh(const Ref<CubeBuilder> &p_cube_builder, const Vector3i &p_chunk_index, const PackedByteArray &p_block_kinds, float p_surface) {
	ERR_FAIL_COND_V_MSG(p_cube_builder.is_null(), Ref<ArrayMesh>(), "Building a water mesh needs a cube builder.");

	// The chunk and those round it, looked up once. A chunk that doesn't exist is all air, so holds no water.
	Ref<ChunkInfo> nearby[27];
	for (int32_t i = 0; i < 27; ++i) {
		const Vector3i offset(i % 3 - 1, (i / 3) % 3 - 1, i / 9 - 1);
		nearby[i] = p_cube_builder->find_chunk(p_chunk_index + offset);
	}
	ChunkInfo *chunk = nearby[13].ptr();
	if (chunk == nullptr) {
		return Ref<ArrayMesh>();
	}

	// Every block of the grid as what it is to water, read once: a lake is nearly all water that shows nothing, and
	// looking each of its neighbours up one at a time is what would take the time.
	const uint8_t *kinds = p_block_kinds.ptr();
	const int64_t kind_count = p_block_kinds.size();
	uint8_t grid[GRID_CELLS];
	for (int32_t y = 0; y < GRID; ++y) {
		const int32_t ly = y - 1;
		const int32_t cy = ly < 0 ? -1 : (ly >= chunk_height ? 1 : 0);
		for (int32_t z = 0; z < GRID; ++z) {
			const int32_t lz = z - 1;
			const int32_t cz = lz < 0 ? -1 : (lz >= chunk_depth ? 1 : 0);
			for (int32_t x = 0; x < GRID; ++x) {
				const int32_t lx = x - 1;
				const int32_t cx = lx < 0 ? -1 : (lx >= chunk_width ? 1 : 0);
				ChunkInfo *from = nearby[(cx + 1) + (cy + 1) * 3 + (cz + 1) * 9].ptr();
				const BlockType block_type = from ? from->get_block_at(BlockIndex(lx - cx * chunk_width, ly - cy * chunk_height, lz - cz * chunk_depth)) : 0;
				grid[x + z * GRID + y * GRID_LAYER] = block_type >= 0 && block_type < kind_count ? kinds[block_type] : KIND_SOLID;
			}
		}
	}

	// Gathered here and copied into packed arrays once at the end, because every append to a packed array is a call
	// into the engine.
	LocalVector<Vector3> vertices;
	LocalVector<Vector3> normals;
	LocalVector<Color> shades;
	for (int32_t y = 0; y < chunk_height; ++y) {
		for (int32_t z = 0; z < chunk_depth; ++z) {
			for (int32_t x = 0; x < chunk_width; ++x) {
				const int32_t at = (x + 1) + (z + 1) * GRID + (y + 1) * GRID_LAYER;
				const uint8_t level = grid[at];
				if (level == KIND_OPEN || level == KIND_SOLID) {
					continue;
				}
				const uint8_t above = grid[at + GRID_LAYER];
				const float height = surface_height(level, above, p_surface);
				const Vector3 base(x, y, z);
				const uint32_t first = vertices.size();
				if (above == KIND_OPEN || above == KIND_SOLID) {
					add_face(vertices, normals, base + Vector3(0.5f, height, 0.5f), Vector3(0.5f, 0, 0), Vector3(0, 0, -0.5f), Vector3(0, 1, 0));
				}
				for (int32_t s = 0; s < 4; ++s) {
					const int32_t beside = at + SIDE_STEPS[s];
					const uint8_t neighbour = grid[beside];
					if (neighbour == KIND_SOLID) {
						continue;
					}
					float from = 0.0f;
					if (neighbour != KIND_OPEN) {
						from = surface_height(neighbour, grid[beside + GRID_LAYER], p_surface);
						if (from >= height) {
							continue;
						}
					}
					const Vector3 normal(SIDES[s]);
					const Vector3 across = normal.cross(Vector3(0, 1, 0)) * 0.5f;
					const Vector3 middle = base + Vector3(0.5f, (from + height) * 0.5f, 0.5f) + normal * 0.5f;
					add_face(vertices, normals, middle, across, Vector3(0, (height - from) * 0.5f, 0), normal);
				}
				if (grid[at - GRID_LAYER] == KIND_OPEN) {
					add_face(vertices, normals, base + Vector3(0.5f, 0, 0.5f), Vector3(0.5f, 0, 0), Vector3(0, 0, 0.5f), Vector3(0, -1, 0));
				}
				// Looked up only for cells with something to show.
				if (vertices.size() > first) {
					const Vector2i light = chunk->get_light_at(BlockIndex(x, y, z));
					const Color shade(light.x / 15.0f, light.y / 15.0f, 0.0f);
					for (uint32_t j = first; j < vertices.size(); ++j) {
						shades.push_back(shade);
					}
				}
			}
		}
	}
	if (vertices.is_empty()) {
		return Ref<ArrayMesh>();
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = to_packed<PackedVector3Array>(vertices);
	arrays[Mesh::ARRAY_NORMAL] = to_packed<PackedVector3Array>(normals);
	arrays[Mesh::ARRAY_COLOR] = to_packed<PackedColorArray>(shades);
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

void WaterSimulation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cube_builder", "cube_builder"), &WaterSimulation::set_cube_builder);
	ClassDB::bind_method(D_METHOD("get_cube_builder"), &WaterSimulation::get_cube_builder);
	ClassDB::bind_method(D_METHOD("set_loaded_check", "loaded_check"), &WaterSimulation::set_loaded_check);
	ClassDB::bind_method(D_METHOD("get_loaded_check"), &WaterSimulation::get_loaded_check);
	ClassDB::bind_method(D_METHOD("set_level_block_types", "block_types"), &WaterSimulation::set_level_block_types);
	ClassDB::bind_method(D_METHOD("get_level_block_types"), &WaterSimulation::get_level_block_types);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "cube_builder", PROPERTY_HINT_RESOURCE_TYPE, "CubeBuilder"), "set_cube_builder", "get_cube_builder");
	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "loaded_check"), "set_loaded_check", "get_loaded_check");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "level_block_types"), "set_level_block_types", "get_level_block_types");

	ClassDB::bind_method(D_METHOD("is_water", "block_type"), &WaterSimulation::is_water);
	ClassDB::bind_method(D_METHOD("get_level", "block_type"), &WaterSimulation::get_level);
	ClassDB::bind_method(D_METHOD("get_block_for", "level"), &WaterSimulation::get_block_for);
	ClassDB::bind_method(D_METHOD("get_volume_at", "position"), &WaterSimulation::get_volume_at);
	ClassDB::bind_method(D_METHOD("is_open", "position"), &WaterSimulation::is_open);
	ClassDB::bind_method(D_METHOD("add_water", "position", "amount"), &WaterSimulation::add_water);
	ClassDB::bind_method(D_METHOD("take_water", "position", "amount"), &WaterSimulation::take_water);
	ClassDB::bind_method(D_METHOD("wake", "position"), &WaterSimulation::wake);
	ClassDB::bind_method(D_METHOD("get_pending"), &WaterSimulation::get_pending);
	ClassDB::bind_method(D_METHOD("on_block_changed", "position", "block_type"), &WaterSimulation::on_block_changed);
	ClassDB::bind_method(D_METHOD("set_player_cell", "cell"), &WaterSimulation::set_player_cell);
	ClassDB::bind_method(D_METHOD("clear_player_cell"), &WaterSimulation::clear_player_cell);
	ClassDB::bind_method(D_METHOD("step", "max_updates", "max_millis"), &WaterSimulation::step);
	ClassDB::bind_method(D_METHOD("take_changes"), &WaterSimulation::take_changes);
	ClassDB::bind_static_method("WaterSimulation", D_METHOD("build_mesh", "cube_builder", "chunk_index", "block_kinds", "surface"), &WaterSimulation::build_mesh);
}
