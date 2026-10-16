#pragma once

#if defined(__APPLE__) || defined(__linux__) || defined(__HAIKU__)

#include <cerrno>
#include <pthread.h>
#include <signal.h>

namespace lsp::io::detail{

class SigpipeGuard{
public:
	SigpipeGuard() noexcept
	{
		sigemptyset(&m_signal);
		sigaddset(&m_signal, SIGPIPE);
		m_error = pthread_sigmask(SIG_BLOCK, &m_signal, &m_oldMask);
		if(m_error != 0)
			return;
		sigset_t pending;
		m_alreadyPending = sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) == 1;
		m_alreadyBlocked = sigismember(&m_oldMask, SIGPIPE) == 1;
	}

	SigpipeGuard(const SigpipeGuard&) = delete;
	SigpipeGuard& operator=(const SigpipeGuard&) = delete;

	~SigpipeGuard()
	{
		if(m_error != 0)
			return;
		const auto savedError = errno;
		if(m_brokenPipe && !m_alreadyPending && !m_alreadyBlocked)
		{
			// Consume only this write's signal before restoring the caller's mask.
			sigset_t pending;
			if(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
			{
				int signal;
				sigwait(&m_signal, &signal);
			}
		}
		pthread_sigmask(SIG_SETMASK, &m_oldMask, nullptr);
		errno = savedError;
	}

	[[nodiscard]] auto error() const noexcept -> int{ return m_error; }
	void failedWith(int error) noexcept{ m_brokenPipe = m_brokenPipe || error == EPIPE; }

private:
	sigset_t m_signal;
	sigset_t m_oldMask;
	int m_error = 0;
	bool m_alreadyPending = false;
	bool m_alreadyBlocked = false;
	bool m_brokenPipe = false;
};

} // namespace lsp::io::detail

#endif
