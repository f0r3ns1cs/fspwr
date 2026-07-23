#include "worker.h"

namespace pwr
{
	WorkerQueue::WorkerQueue() : thread_([this] { run(); }) {
	}

	WorkerQueue::~WorkerQueue() {
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
		}
		cv_.notify_all();
		if (thread_.joinable()) thread_.join();
	}

	void WorkerQueue::post(std::function<void()> task) {
		{
			std::lock_guard lock(mutex_);
			if (stopping_) return;
			tasks_.push_back(std::move(task));
		}
		cv_.notify_one();
	}

	WorkerQueue& WorkerQueue::shared() {
		static WorkerQueue inst;
		return inst;
	}

	void WorkerQueue::run() {
		for (;;) {
			std::function<void()> task;
			{
				std::unique_lock lock(mutex_);
				cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
				if (stopping_ && tasks_.empty()) return;
				task = std::move(tasks_.front());
				tasks_.pop_front();
			}
			task();
		}
	}
}
