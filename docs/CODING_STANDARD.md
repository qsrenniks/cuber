# C++ Coding Standard

This standard is taken from the conventions of the Godot engine as they appear in the
`godot-cpp` submodule. It covers `.clang-format`, `.editorconfig`, the pre-commit hooks, and the
code in `include/`, `src/` and `test/src/`. The aim is for code in `src/` to read like Godot code,
so anything copied between this project, godot-cpp and the engine needs no restyling.

When this document and `godot-cpp/.clang-format` disagree, `.clang-format` wins.

---

## 1. Formatting

Formatting is mechanical. Run `clang-format` (v20, the version pinned in godot-cpp's pre-commit)
using `godot-cpp/.clang-format` and don't adjust its output by hand.

| Rule | Value |
| --- | --- |
| Indentation | **Tabs**, 4 columns wide. Never spaces. |
| Continuation indent | 2 tabs (8 columns) |
| Line length | No hard limit (`ColumnLimit: 0`). Aim for about 120 columns, the `.editorconfig` soft limit. |
| Braces | Attached ("K&R"): `if (x) {`, `class Foo {`, `void f() {` |
| Braces on single statements | **Required.** `InsertBraces: true`, so every `if`/`for`/`while` body gets braces. |
| `case` labels | Indented one level inside `switch` |
| Access specifiers | Flush with the `class` keyword (`public:` at column 0 inside a top-level class) |
| Pointer/reference binding | To the **name**: `const String &p_name`, `T *data` |
| Braced init lists | Spaces inside: `{ a, b, c }` |
| Short functions | One line only when defined inline in a class body: `int size() const { return count; }` |
| Constructor initializers | Break after the colon, on the next line, indented 2 tabs |
| Alignment | None. No aligned trailing comments, operands or `\` continuations (alignment causes noisy diffs). |
| Empty lines | None at the start of a block |
| Files | UTF-8, LF line endings, trailing newline, no trailing whitespace |

```cpp
MyNode::MyNode() :
		speed(10.0),
		target(nullptr) {
}

void MyNode::process_item(const Ref<Item> &p_item) {
	switch (p_item->get_kind()) {
		case Item::KIND_A: {
			handle_a(p_item);
		} break;
		default:
			break;
	}
}
```

---

## 2. Naming

| Kind | Style | Example |
| --- | --- | --- |
| Classes, structs, enums, type aliases | `PascalCase` | `ChunkMeshBuilder`, `BlockType` |
| Functions and methods | `snake_case` | `get_or_create_chunk()` |
| Member variables | `snake_case`, **no prefix** | `chunks_dirty`, `block_types` |
| Local variables | `snake_case` | `chunk_index` |
| Function parameters | `p_` + `snake_case` | `p_index`, `p_block_type` |
| Output parameters (written by the callee) | `r_` + `snake_case` | `Variant &r_ret`, `bool &r_is_valid` |
| Constants and `constexpr` values | `SCREAMING_SNAKE_CASE` | `CHUNK_WIDTH`, `FACE_DIRS` |
| Enum values | `SCREAMING_SNAKE_CASE`, prefixed with the enum's meaning | `FLAG_ONE`, `KIND_SOLID` |
| Macros | `SCREAMING_SNAKE_CASE` | `ERR_FAIL_COND` |
| Template parameters | `PascalCase` or a single capital | `T`, `TKey` |
| Files | `snake_case` matching the main class | `chunk_mesh_builder.h` / `.cpp` |

### Method names

- Accessors are `get_x()` / `set_x()`. Boolean queries use `is_x()` / `has_x()`.
- A leading underscore marks **engine virtuals and callbacks** (`_ready`, `_process`,
  `_notification`, `_bind_methods`, `_get_property_list`) and **private helpers**
  (`_resize`, `_generate_grid_info`). Don't give public API methods a leading underscore.
- Names exposed to GDScript through `D_METHOD("name", "arg", ...)` must match the C++ method and
  parameter names, with the `p_` prefix dropped:
  `D_METHOD("set_block", "global_index", "block_type")` ↔ `set_block(..., p_global_index, p_block_type)`.

---

## 3. Files and headers

### Header guards

Use `#pragma once` on the first non-comment line. Don't use `#ifndef` guards; godot-cpp's
`header_guards.py` converts them.

### Include order

Include groups go in this order, with a blank line between groups. Sort alphabetically within
each group:

1. In a `.cpp` file, its own header first: `#include "chunk_mesh_builder.h"`.
2. Other project headers in quotes: `#include "cube_builder.h"`.
3. Godot headers: `#include <godot_cpp/classes/node3d.hpp>`.
4. Standard library headers, only when necessary (see §6): `#include <type_traits>`.

```cpp
#include "chunk_mesh_builder.h"

#include "cube_builder.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <type_traits>
```

- Include what you use. If a header only needs pointers or `Ref<>`s to a class, forward-declare it
  (`class ArrayMesh;`) and include the full header in the `.cpp`.
- Wrap conditional includes in `#if` with a matching comment on `#endif`:
  `#endif // GODOT_VERSION_MINOR >= 4`.

### Namespaces

- Headers declare everything inside `namespace godot { ... }`. Close it with a comment:
  `} // namespace godot`.
- `.cpp` files may use `using namespace godot;` after the includes. **Never put
  `using namespace` in a header.**
- File-local helpers, constants and structs in a `.cpp` go in an anonymous `namespace { ... }`, not
  `static` free functions.

### Extension entry point

`init.cpp` holds class registration (`GDREGISTER_CLASS`) and the `extern "C"` entry symbol, and
nothing else. Register classes in dependency order, base classes first.

---

## 4. Class layout

Order sections like Godot's own classes (see `godot-cpp/test/src/my_test.h`):

```cpp
class ChunkMeshBuilder : public MeshInstance3D {
	GDCLASS(ChunkMeshBuilder, MeshInstance3D);

private:
	static constexpr float BLOCK_THRESHOLD = 0.001f;

	int32_t chunk_size = 16;
	bool dirty = false;

	void _rebuild();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	enum Kind {
		KIND_EMPTY,
		KIND_SOLID,
	};

	void set_chunk_size(int32_t p_size);
	int32_t get_chunk_size() const;

	ChunkMeshBuilder();
	~ChunkMeshBuilder();
};

VARIANT_ENUM_CAST(ChunkMeshBuilder::Kind);
```

- `GDCLASS(Name, Base);` goes on the first line of the body, followed by a semicolon.
- Section order is `private:` (data first, then helpers), `protected:` (the binding and
  notification hooks), then `public:`. Leave out empty sections.
- **Initialize members where they are declared** (`bool dirty = false;`). Use the constructor only
  for values that need computing.
- Mark every method that doesn't modify state `const`.
- Use `override` on overridden virtuals. Don't repeat `virtual` alongside `override`.
- An empty destructor or constructor can be omitted. If you keep one, write `{}` with no blank line
  inside.

---

## 5. Types, parameters and values

### Parameter passing

| Type | Pass as |
| --- | --- |
| Scalars (`int32_t`, `float`, `bool`, enums) | By value: `int32_t p_count` |
| Godot value types (`Vector3i`, `String`, `StringName`, `Array`, `Dictionary`, `Packed*Array`, `Variant`) | `const T &p_x` |
| `Ref<T>` | `const Ref<T> &p_x` |
| `Object`-derived (Nodes) | Raw pointer: `Node3D *p_controller` |
| Values the callee writes | `T &r_x` |

### Aliases and constants

- Use `using Alias = Type;` and not `typedef`.
- Constants are `constexpr`, in `SCREAMING_SNAKE_CASE`. Class-scoped constants are
  `static constexpr`; file-scoped ones go in an anonymous namespace.
- Use fixed-width integers (`int32_t`, `uint32_t`, `int64_t`) when size matters, such as storage,
  serialization and indexing. Godot's `int` in GDScript is 64-bit, so bound methods that take
  integers from scripts may use `int64_t`.
- Use `real_t` for spatial math that should follow Godot's `precision=double` build option.

### Misc

- Write `nullptr`, never `NULL` or `0`.
- Use `auto` sparingly, where the type is obvious from the right-hand side or unwritable (iterators,
  lambdas). godot-cpp uses it fewer than 10 times.
- Prefer functional casts for numeric conversions (`float(x)`, `int32_t(v)`), as godot-cpp does.
  Use `Object::cast_to<T>(obj)` for `Object` hierarchies, never `dynamic_cast`.
- Use `enum` (unscoped, with prefixed values) for anything exposed to Godot, so that
  `BIND_ENUM_CONSTANT` and `VARIANT_ENUM_CAST` work. `enum class` is fine for purely internal enums.

---

## 6. Containers, memory and ownership

### Containers

Use Godot containers, not the STL. godot-cpp includes `<vector>`, `<map>` and `<string>` only once
each.

| Need | Use |
| --- | --- |
| Data crossing into Godot or scripts | `Array`, `TypedArray<T>`, `Dictionary`, `TypedDictionary<K, V>`, `Packed*Array` |
| Internal dynamic array | `LocalVector<T>` (not copy-on-write, fastest) or `Vector<T>` (copy-on-write) |
| Internal map / set | `HashMap<K, V>`, `HashSet<T>`, or `AHashMap` |
| Ordered map | `RBMap<K, V>` |
| Fixed-size block of data | Plain C array, or `LocalVector` sized once |

Don't use `Dictionary`/`TypedDictionary` as internal storage on hot paths. They store `Variant`s
and every access boxes the value. Use `HashMap` internally and convert at the API boundary.

### Allocation

Never use bare `new`/`delete`/`malloc`. Use Godot's allocators so that memory is tracked and
`Object`s are initialized correctly:

| Allocate | Free |
| --- | --- |
| `memnew(T(args))` | `memdelete(ptr)` |
| `memnew_arr(T, n)` | `memdelete_arr(ptr)` |
| `memalloc(bytes)` | `memfree(ptr)` |

### Ownership

- **`RefCounted` / `Resource` subclasses** are always held in `Ref<T>`. Create them with
  `Ref<T> x; x.instantiate();` or `memnew`, and never `memdelete` them. Check validity with
  `x.is_valid()` / `x.is_null()`.
- **`Node`s** belong to the scene tree. After `add_child()`, the parent owns the child. Free nodes
  with `queue_free()`.
- **Plain `Object`s** that aren't in the tree are owned by whoever `memnew`ed them, and that owner
  must `memdelete` them.
- Owning raw pointers to arrays are discouraged. If you need one, free it in exactly one place and
  set the pointer to `nullptr` immediately afterwards. A `LocalVector` usually removes the need.

---

## 7. Error handling

Exceptions are **disabled** (`disable_exceptions=yes` is the godot-cpp default), and so is RTTI in
practice. Don't use `throw`, `try` or `dynamic_cast`. Report problems with Godot's error macros from
`<godot_cpp/core/error_macros.hpp>`. These print to the Godot output with file and line and **return
from the current function**.

| Situation | Macro |
| --- | --- |
| Invalid argument or state, `void` function | `ERR_FAIL_COND_MSG(cond, "msg");` |
| Same, function returns a value | `ERR_FAIL_COND_V_MSG(cond, ret, "msg");` |
| Null pointer or invalid `Ref` | `ERR_FAIL_NULL(ptr);` / `ERR_FAIL_NULL_V(ptr, ret);` |
| Index out of range | `ERR_FAIL_INDEX(idx, size);` / `ERR_FAIL_INDEX_V(idx, size, ret);` |
| Unreachable branch | `ERR_FAIL_MSG("msg");` / `ERR_FAIL_V_MSG(ret, "msg");` |
| Report and keep going | `ERR_PRINT("msg");`, `WARN_PRINT("msg");`, `*_ONCE` variants inside loops |
| Internal invariant (debug builds only) | `DEV_ASSERT(cond);` |
| Unrecoverable corruption | `CRASH_COND_MSG(cond, "msg");` (rare) |

- **Validate at the top of the function, before doing any work.** The check comes before the use:
  `ERR_FAIL_NULL(chunk);` goes *before* `chunk->...`.
- Validate every argument of a method bound to scripts. Scripts can pass anything.
- Messages are full sentences that say what was wrong and, where possible, the offending value:
  `vformat("Block index %s is outside the chunk.", p_index)`.
- Mark rare failure branches in hot code with `unlikely(...)`.

---

## 8. Binding to Godot

- Every `GDCLASS` has a `static void _bind_methods();`. Define it in the `.cpp`, even if it's empty.
- Bind every method in the class's public script API. Static methods use
  `ClassDB::bind_static_method("ClassName", D_METHOD(...), &Class::method);`.
- Properties are declared with `ADD_PROPERTY(PropertyInfo(Variant::TYPE, "name"), "set_name", "get_name");`
  and need both accessors bound. Group related properties with `ADD_GROUP("Label", "prefix_")`.
- Signals: `ADD_SIGNAL(MethodInfo("name", PropertyInfo(...)));`. Signal names are past tense
  `snake_case` (`chunk_generated`).
- Enums: `BIND_ENUM_CONSTANT(VALUE);` in `_bind_methods`, plus `VARIANT_ENUM_CAST(Class::Enum);`
  after the class in the header. Use `BIND_BITFIELD_FLAG` / `BitField<T>` for flag sets.
- Script-overridable virtuals: `GDVIRTUAL*` in the header, `GDVIRTUAL_BIND` in `_bind_methods`,
  and `GDVIRTUAL_CALL` at the call site. Name them with a leading underscore (`_on_block_changed`).
- Prefer typed C++ calls over `get("name")` / `call("name")`, which are string-keyed and
  `Variant`-boxed. If a GDScript object has to be accessed by name, cache the `StringName`s as
  `static` locals or members instead of building them from literals on every call.

---

## 9. Comments

- Use `//` comments with **a space after the slashes**, start them with a capital letter, and end
  them with a period: `// Grows exponentially unless tight is set.`
- `//code();` without a space is reserved for **temporarily disabled code**. clang-format allows it
  for that reason. Don't commit it long-term.
- Document public API with `///` Doxygen-style comments where the intent isn't obvious from the name.
- Comment *why*, not *what*. Bug or issue references go in the comment:
  `// Must take a copy instead of a reference (see GH-31736).`
- Every `#endif` and closing `}` of a namespace gets a trailing comment naming what it closes.
- Write `TODO:` comments with enough context to act on later.

---

## 10. General practices

- Language standard: C++17, which godot-cpp's build passes (`-std=c++17` / `/std:c++17`).
  `.clang-format` parses as C++20, but don't use C++20 features.
- Prefer `if constexpr`, `static_assert` and `<type_traits>` over macros for compile-time logic.
- Keep hot loops free of `Variant`, `String` building and `Dictionary` lookups.
- Mark tiny, hot accessors `_FORCE_INLINE_` only after measuring. Inline definitions in the class
  body are usually enough.
- Use `print_line()` / `UtilityFunctions::print()` for debug output, and remove it before
  committing. Use `print_verbose()` for logs you want to keep.
- One main class per file pair. Small helper types used only by that class may share the file.

---

## Appendix A: Current deviations in `src/`

These were found while writing this standard. Fix them when you next touch these files.

| Location | Deviation | Rule |
| --- | --- | --- |
| `init.cpp`, `chunk_controller.h`, `test.h` | Space indentation, Allman brace in `init_mod` | §1 |
| `cube_builder.h/.cpp`, `chunk_controller.h`, `test.h` | `m_` member prefix, camelCase (`m_blockTypes`, `m_chunkInfo`) | §2: `block_types`, `chunk_info` |
| All files | Parameters lack the `p_` prefix (`index`, `controller`, `info`) | §2 |
| `cube_builder.h`, `test.h` | `const T& x` binds the reference to the type | §1: `const T &p_x` |
| `cube_builder.h` | `static const int32_t chunk_width` | §5: `static constexpr int32_t CHUNK_WIDTH` |
| `cube_builder.h` | Empty `private:` section; data members interleaved with methods | §4 |
| `cube_builder.h` | `BlockType` passed as `const BlockType &` | §5: pass scalars by value |
| `chunk_controller.h` | `typedef int BlockType;` conflicts with `using BlockType = int32_t;` in `cube_builder.h` | §5: one `using` in one shared header |
| `cube_builder.cpp` | `if (m_blockTypes) memdelete_arr(...)` without braces, pointer not reset afterwards | §1, §6 |
| `cube_builder.cpp` | `ERR_FAIL_NULL_MSG(chunk, ...)` placed after `chunk` was already used | §7 |
| `cube_builder.cpp` | Uses `UtilityFunctions` without including `utility_functions.hpp` | §3 |
| `cube_builder.h` | `TypedDictionary` used as internal chunk storage | §6: `HashMap<ChunkIndex, Ref<ChunkInfo>>` |
| `chunk_mesh_builder.cpp` | `#include <vector>` | §6 |
| All headers | Namespace closed with a bare `}`, some files missing a trailing newline | §3, §1 |
| All files | `//comment` with no space | §9 |
| Headers | `GDCLASS(...)` without a trailing semicolon | §4 |

## Appendix B: Tooling

- **Formatting:** copy or symlink `godot-cpp/.clang-format` and `godot-cpp/.editorconfig` to the
  repo root so editors and `clang-format` pick them up for `src/`. Run
  `clang-format -i src/*.cpp src/*.h`.
- **VS Code:** turn on `"editor.formatOnSave": true` with the C/C++ or clangd extension. Both read
  `.clang-format`.
- **Pre-commit (optional):** reuse the `clang-format` hook from `godot-cpp/.pre-commit-config.yaml`,
  pinned to `v20.1.0`.
