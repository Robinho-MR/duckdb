//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/no_destructor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

#include <new>
#include <utility>

namespace duckdb {

//! Holds a T whose destructor is never run.
//! Use it for function-local statics: other threads can still be using the object while the process exits and static
//! destructors run, so it must stay alive. It owns inline storage, so nothing is allocated and there is no leak.
//! Prefer a trivially destructible type (e.g. constexpr data) when possible - it needs neither this nor a lock.
//!
//!   static const NoDestructor<case_insensitive_set_t> set(case_insensitive_set_t {"a", "b"});
//!   return *set;
template <class T>
class NoDestructor {
public:
	template <class... ARGS>
	explicit NoDestructor(ARGS &&...args) {
		new (storage) T(std::forward<ARGS>(args)...);
	}
	NoDestructor(const NoDestructor &) = delete;
	NoDestructor &operator=(const NoDestructor &) = delete;

public:
	T &operator*() {
		return *Get();
	}
	const T &operator*() const {
		return *Get();
	}
	T *operator->() {
		return Get();
	}
	const T *operator->() const {
		return Get();
	}
	T *Get() {
		return std::launder(reinterpret_cast<T *>(storage));
	}
	const T *Get() const {
		return std::launder(reinterpret_cast<const T *>(storage));
	}

private:
	alignas(T) char storage[sizeof(T)];
};

} // namespace duckdb

//! Static lifetime checks (-Wexit-time-destructors, -Wglobal-constructors) are enforced for first-party code - see
//! scripts/check_static_lifetimes.py. Use these only where a destructor must run (e.g. thread_local cleanup) or a
//! constructor must run before main (e.g. registration), and say why next to the use.
#if defined(__clang__)
#define DUCKDB_ALLOW_STATIC_LIFETIME_BEGIN                                                                             \
	_Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wexit-time-destructors\"")                   \
	    _Pragma("clang diagnostic ignored \"-Wglobal-constructors\"")
#define DUCKDB_ALLOW_STATIC_LIFETIME_END _Pragma("clang diagnostic pop")
#else
#define DUCKDB_ALLOW_STATIC_LIFETIME_BEGIN
#define DUCKDB_ALLOW_STATIC_LIFETIME_END
#endif
