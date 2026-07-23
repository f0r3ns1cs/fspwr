#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace pwr
{
	class WorkerQueue {
	  public:
		WorkerQueue();
		~WorkerQueue();

		WorkerQueue(const WorkerQueue&) = delete;
		WorkerQueue& operator=(const WorkerQueue&) = delete;

		void post(std::function<void()> task);

		static WorkerQueue& shared();

	  private:
		void run();

		std::mutex mutex_;
		std::condition_variable cv_;
		std::deque<std::function<void()>> tasks_;
		bool stopping_ = false;
		std::thread thread_;
	};
}
