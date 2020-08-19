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

	// ��֧�ֿ������壨Ҳ��֧���ƶ����壩
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// �ȴ������̹߳رգ������������߳�
	void wait();

	// ���һ������
	void execute(std::function<void()> task);

	// ���һ����ʱ����
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
