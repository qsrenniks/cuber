#include "test.h"
#include <godot_cpp/godot.hpp>

using namespace godot;

void TestNode::_bind_methods() 
{
	ClassDB::bind_method(D_METHOD("set_chunk_info", "info"), &TestNode::set_chunk_info);
	ClassDB::bind_method(D_METHOD("get_chunk_info"), &TestNode::get_chunk_info);

	GDVIRTUAL_BIND(_test_something_happened);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "chunk_info", PROPERTY_HINT_DICTIONARY_TYPE, "Vector3i;int"), "set_chunk_info", "get_chunk_info");
}

TestNode::TestNode()
{
	
}

TestNode::~TestNode()
{

}

void TestNode::_ready()
{
	UtilityFunctions::print("Hello world!\n");

	test_something_happened();
}

void TestNode::test_something_happened() const
{
	if (GDVIRTUAL_CALL(_test_something_happened))
	{
		return;
	}

	return;
}
