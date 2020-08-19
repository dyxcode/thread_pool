#pragma once

#include <map>
#include <vector>
#include <condition_variable>

class ThreadPool{
	template<typename V, typename R>
	using CDuration = const std::chrono::duration<V, R>;
	using CTimePoint = const std::chrono::steady_clock::time_point;
	// 执行时间，提交时间，周期时间
	using Task_Handle = std::tuple<CTimePoint, CTimePoint, CTimePoint>;
	using Lockgd = std::lock_guard<std::mutex>;
	using Ulock = std::unique_lock<std::mutex>;

public:
	explicit ThreadPool(std::size_t thread_num);

	~ThreadPool();

	// 不支持拷贝语义（也不支持移动语义）
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 添加一个普通任务
	template<typename F>
	void execute(F&& task) {
		CTimePoint& now = std::chrono::steady_clock::now();
		addTask(std::make_tuple(now, now, now), std::forward<F>(task));
	}

	// 添加一个定时任务
	template<typename F, typename V, typename R>
	Task_Handle execute(F&& task, CDuration<V, R>& duration) {
		CTimePoint& execute_time = std::chrono::steady_clock::now() + duration;
		CTimePoint& now = std::chrono::steady_clock::now();
		auto task_handle = make_tuple(execute_time, now, execute_time);
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// 添加一个周期任务
	template<typename F, typename V1, typename R1, typename V2, typename R2>
	Task_Handle execute(F&& task, CDuration<V1, R1>& duration, CDuration<V2, R2>& period) {
		CTimePoint& execute_time = std::chrono::steady_clock::now() + duration;
		CTimePoint& period_time = execute_time + period;
		CTimePoint& now = std::chrono::steady_clock::now();
		auto task_handle = make_tuple(execute_time, now, period_time);
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	bool cancel(const Task_Handle& task_handle);

private:
	template<typename F>
	void addTask(const Task_Handle& task_handle, F&& task) {
		{
			Lockgd guard{mtx_};
			tasks_.emplace(task_handle, std::forward<F>(task));
		}
		cv_.notify_one();
	}

	bool is_start_;
	std::mutex mtx_;
	std::condition_variable cv_;
	std::vector<std::thread> threads_;
	std::map<Task_Handle, std::function<void()>> tasks_;
};
