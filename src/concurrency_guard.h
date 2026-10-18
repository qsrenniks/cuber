#pragma once

#include <atomic>

#include <godot_cpp/core/error_macros.hpp>

namespace godot {

// Lightweight debug check that a section of code is never entered by two callers at once.
// Never blocks: if a second caller enters while the guard is held, it crashes with an error.
// Also fires on same-thread re-entry.
class ConcurrencyGuard {
	friend class ConcurrencyScope;

	std::atomic_flag busy = ATOMIC_FLAG_INIT;
};

class ConcurrencyScope {
public:
	explicit ConcurrencyScope(ConcurrencyGuard& guard) :
			guard(guard) {
		// test_and_set returns the previous value: true means someone is already inside.
		CRASH_COND_MSG(guard.busy.test_and_set(std::memory_order_acquire), "Concurrent access detected.");
	}

	~ConcurrencyScope() {
		guard.busy.clear(std::memory_order_release);
	}

	ConcurrencyScope(const ConcurrencyScope&) = delete;
	ConcurrencyScope& operator=(const ConcurrencyScope&) = delete;

private:
	ConcurrencyGuard& guard;
};

} // namespace godot

#define CONCURRENCY_GUARD_CONCAT_IMPL(a, b) a##b
#define CONCURRENCY_GUARD_CONCAT(a, b) CONCURRENCY_GUARD_CONCAT_IMPL(a, b)

#define CONCURRENCY_CHECK(guard) ::godot::ConcurrencyScope CONCURRENCY_GUARD_CONCAT(_concurrency_scope_, __LINE__)(guard)
