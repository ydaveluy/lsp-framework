#include <lsp/threadpool.h>

namespace lsp{

// ZERO MEANS INLINE, and it is the only answer on a platform that has no
// threads at all: WASI's `pthread_create` is a stub, so a floor of one worker
// made every request fail with "thread constructor failed: Not supported". A
// caller that says zero gets its task run where it submitted it.
ThreadPool::ThreadPool(unsigned int initialThreads, unsigned int maxThreads)
	: m_maxThreads{maxThreads}
{
	const auto lock = std::lock_guard(m_mutex);
	m_threads.reserve(initialThreads);

	for(std::size_t i = 0; i < initialThreads; ++i)
		addThread();
}

ThreadPool::~ThreadPool()
{
	waitUntilFinished();
}

// The queued tasks still run, but new ones are refused rather than made to wait:
// a task submitting a follow-up from a worker would otherwise block on a pool
// that is joining that very worker. Two callers serialize on m_shutdownMutex,
// and the thread vector is taken under m_mutex before it is joined.
void ThreadPool::waitUntilFinished()
{
	const auto shutdownLock = std::lock_guard(m_shutdownMutex);
	auto       threads      = std::vector<std::thread>();

	{
		const auto lock = std::lock_guard(m_mutex);
		m_stopping = true;
		threads.swap(m_threads);
	}

	m_event.notify_all();

	for(auto& t : threads)
		t.join();

	const auto lock = std::lock_guard(m_mutex);
	m_stopping = false;
}

void ThreadPool::addTask(TaskPtr task)
{
	auto lock = std::unique_lock(m_mutex);

	if(m_stopping)
	{
		lock.unlock();
		task->cancel();
		return;
	}

	// No pool at all: run it here, outside the lock, so a task submitting a
	// follow-up is not deadlocked by the mutex it is already under.
	if(m_maxThreads == 0)
	{
		lock.unlock();
		task->execute();
		return;
	}

	m_taskQueue.emplace(std::move(task));

	// A queue longer than the idle workers means this task would wait behind
	// one already running, so grow the pool instead of queueing.
	if(m_taskQueue.size() > m_idleThreads && m_threads.size() < m_maxThreads)
		addThread();

	lock.unlock();
	m_event.notify_one();
}

void ThreadPool::addThread()
{
	m_threads.emplace_back([this]()
	{
		auto lock = std::unique_lock(m_mutex);
		++m_idleThreads;

		while(true)
		{
			m_event.wait(lock, [this](){ return m_stopping || !m_taskQueue.empty(); });

			if(m_taskQueue.empty()) // No more tasks in the queue. Thread was notified to exit.
				break;

			auto task = std::move(m_taskQueue.front());
			m_taskQueue.pop();
			--m_idleThreads;
			lock.unlock();

			task->execute();
			task.reset(); // Run the task's destructor outside of the lock.

			lock.lock();
			++m_idleThreads;
		}

		--m_idleThreads;
	});
}

} // namespace lsp
