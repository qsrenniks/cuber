#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>

namespace godot {

class TestNode : public Node3D {
    GDCLASS(TestNode, Node3D)

private:
	Dictionary m_chunkInfo;

protected:
	static void _bind_methods();

public:
	TestNode();
	~TestNode();

	void set_chunk_info(const Dictionary& info) { m_chunkInfo = info; }
	Dictionary get_chunk_info() const { return m_chunkInfo; }

	GDVIRTUAL0C(_test_something_happened)
	void test_something_happened() const;

	virtual void _ready() override;
};

}