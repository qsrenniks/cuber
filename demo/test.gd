extends Node

@export var cube_builder: CubeBuilder = CubeBuilder.new()

var _rng: RandomNumberGenerator = RandomNumberGenerator.new()

var _timer: Timer = Timer.new()
var _chunk_to_model: Dictionary[Vector3i, MeshInstance3D]
var _iteration: int = 0

func _on_chunk_mesh_ready(chunk_index: Vector3i, mesh: ArrayMesh) -> void:
	var new_instance: MeshInstance3D = _chunk_to_model.get(chunk_index)
	if new_instance == null:
		new_instance = MeshInstance3D.new()
		add_child(new_instance)
		_chunk_to_model.set(chunk_index, new_instance)
	new_instance.mesh = mesh

func clear_grid() -> void:
	cube_builder.clear_all_blocks()
	for node: MeshInstance3D in _chunk_to_model.values():
		node.queue_free()
	_chunk_to_model.clear()

func _regenerate_data() -> void:
	clear_grid()

	for x: int in range(0, 16):
		for y: int in range(0, 16):
			for z: int in range(0, 16):
				var rand_int: int = _rng.randi_range(0, 100)
				if rand_int > 80:
					cube_builder.set_block(Vector3i(x, y, z), 1)
				else:
					cube_builder.set_block(Vector3i(x, y, z), 0)

	cube_builder.rebuild_chunk_geometry()

func _ready() -> void:
	cube_builder.on_chunk_mesh_ready.connect(_on_chunk_mesh_ready)

	add_child(_timer)
	_timer.start(0.1)
	_timer.timeout.connect(_regenerate_data)
	_timer.one_shot = false
