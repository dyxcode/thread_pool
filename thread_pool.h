#pragma once

#include <queue>
#include <condition_variable>

class ThreadPool{
	using Lockgd = std::lock_guard<std::mutex>;
	using Ulock = std::unique_lock<std::mutex>;
	using TimePoint = std::chrono::steady_clock::time_point;
	using TimerTask = std::pair<std::function<void()>, TimePoint>;

public:
	explicit ThreadPool(std::size_t thread_num);

	~ThreadPool();

	// 不支持拷贝语义（也不支持移动语义）
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 等待所有线程关闭，并销毁所有线程
	void wait();

	// 添加一个任务
	void execute(std::function<void()> task);

	// 添加一个定时任务
	void execute(std::function<void()> task, std::size_t milliseconds);

private:
	struct Compare_TimerTask {
		bool operator()(const TimerTask& lhs, const TimerTask& rhs) {
			return std::greater<TimePoint>()(lhs.second, rhs.second);
		}
	};

	bool is_start_;
	std::mutex mtx_;
	std::condition_variable cv_;
	std::vector<std::thread> threads_;
	std::queue<std::function<void()>> tasks_;

	std::mutex timer_mtx_;
	std::condition_variable timer_cv_;
	std::thread timer_thread_;
	std::priority_queue<TimerTask, std::deque<TimerTask>, Compare_TimerTask> timer_tasks_;
};
