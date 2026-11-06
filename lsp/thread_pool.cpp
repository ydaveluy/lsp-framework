#include <algorithm>
#include <lsp/thread_pool.h>

namespace lsp{

ThreadPool::ThreadPool(unsigned int initialThreads, unsigned int maxThreads)
	: m_maxThreads(std::max(maxThreads, 1u))
{
	try
	{
		const auto lock = std::lock_guard(m_mutex);
		initialThreads = std::min(initialThreads, m_maxThreads);
		m_threads.reserve(initialThreads);
		m_waitForNewTasks = true;

		for(std::size_t i = 0; i < initialThreads; ++i)
			addThread();
	}
	catch(...)
	{
		// No destructor runs after a throwing constructor: join the threads that
		// did start, or destroying them would end the process.
		waitUntilFinished();
		throw;
	}
}

ThreadPool::~ThreadPool()
{
	waitUntilFinished();
}

void ThreadPool::waitUntilFinished()
{
	{
		const auto lock = std::lock_guard(m_mutex);
		m_waitForNewTasks = false;
	}

	m_event.notify_all();

	for(auto& t : m_threads)
		t.join();

	{
		const auto lock = std::lock_guard(m_mutex);
		m_threads.clear();
		m_waitForNewTasks = true;
	}

	// Notify threads waiting in addTask
	m_event.notify_all();
}

void ThreadPool::setMaxThreads(unsigned int maxThreads)
{
	const auto lock = std::lock_guard(m_mutex);
	m_maxThreads = std::max(maxThreads, 1u);
}

void ThreadPool::addTask(Task task)
{
	if(!task)
		return;

	auto lock = std::unique_lock(m_mutex);

	if(!m_waitForNewTasks)
		m_event.wait(lock, [this](){ return m_waitForNewTasks; });

	// Only an idle worker would pick the task up now: without one it would wait
	// behind a running task, so grow instead. A thread that cannot start leaves
	// the task to the running ones, as when the pool is full. With none running,
	// no thread would ever run it: the failure is thrown and the task not queued.
	if(m_taskQueue.size() >= m_idleThreads && m_threads.size() < m_maxThreads)
	{
		try
		{
			addThread();
		}
		catch(...)
		{
			if(m_threads.empty())
				throw;
		}
	}

	m_taskQueue.emplace(std::move(task));

	lock.unlock();
	m_event.notify_one();
}

void ThreadPool::addThread()
{
	m_threads.emplace_back([this]()
	{
		while(true)
		{
			Task task;

			{
				auto lock = std::unique_lock(m_mutex);

				if(m_waitForNewTasks && m_taskQueue.empty())
				{
					++m_idleThreads;
					m_event.wait(lock, [this](){ return !m_waitForNewTasks || !m_taskQueue.empty(); });
					--m_idleThreads;
				}

				if(!m_taskQueue.empty())
				{
					task = std::move(m_taskQueue.front());
					m_taskQueue.pop();
				}
			}

			if(!task) // No more tasks in the queue. Thread was notified to exit.
				break;

			std::move(task).run();
		}
	});
}

} // namespace lsp
