#pragma once

/*
 * Makes thread starts fail as when the system has no thread left to give: the
 * test executable's pthread_create stands in for the C library's one.
 * Available with glibc only, where LSP_TEST_THREAD_START_FAILURE is defined.
 */

#if defined(__linux__) && defined(__GLIBC__)

#define LSP_TEST_THREAD_START_FAILURE 1

#include <atomic>
#include <cerrno>
#include <dlfcn.h>
#include <pthread.h>

namespace lsptest{

/*
 * While alive, every thread start after the first `allowed` ones fails.
 */
class ThreadStartFailure{
public:
	explicit ThreadStartFailure(int allowed = 0)
	{
		s_allowed = allowed;
		s_active  = true;
	}

	~ThreadStartFailure()
	{
		s_active = false;
	}

	ThreadStartFailure(const ThreadStartFailure&)            = delete;
	ThreadStartFailure& operator=(const ThreadStartFailure&) = delete;

	static auto failsNext() -> bool
	{
		return s_active && s_allowed.fetch_sub(1) <= 0;
	}

private:
	static inline std::atomic<bool> s_active  = false;
	static inline std::atomic<int>  s_allowed = 0;
};

} // namespace lsptest

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*), void* arg) noexcept
{
	using Create = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
	static const auto next = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));

	if(lsptest::ThreadStartFailure::failsNext())
		return EAGAIN;

	return next(thread, attr, start, arg);
}

#endif
