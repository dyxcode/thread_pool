#pragma once

#include <set>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <condition_variable>

class ThreadPool{
	template<typename V, typename R>
	using CDuration = const std::chrono::duration<V, R>;
	using TimePoint = std::chrono::steady_clock::time_point;
	using Lockgd = std::lock_guard<std::mutex>;
	using Ulock = std::unique_lock<std::mutex>;

	// 执行时间,周期时间,执行函数,句柄索引
	using TaskType = std::tuple<TimePoint, TimePoint, std::function<void()>, std::size_t>;
	struct CompareTask {
		bool operator()(const TaskType& lhs, const TaskType& rhs) {
			return std::less<TimePoint>()(std::get<0>(lhs), std::get<0>(rhs));
		}
	};
	using TaskHandle = std::multiset<TaskType, CompareTask>::iterator;

public:
	explicit ThreadPool(std::size_t thread_num);

	~ThreadPool();

	// 不支持拷贝语义（也不支持移动语义）
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 添加一个普通任务
	template<typename F>
	void execute(F&& task) {
		const TimePoint& now = std::chrono::steady_clock::now();
		addTask<false>(std::make_tuple(now, now, std::forward<F>(task), getTaskIndex()));
	}

	// 添加一个延时任务
	template<typename F, typename V, typename R>
	std::size_t execute(F&& task, CDuration<V, R>& duration) {
		const TimePoint& execute_time = std::chrono::steady_clock::now() + duration;
		std::size_t task_index = getTaskIndex();
		addTask<true>(std::make_tuple(execute_time, execute_time, std::forward<F>(task), task_index));
		return task_index;
	}

	// 添加一个周期任务
	template<typename F, typename V1, typename R1, typename V2, typename R2>
	std::size_t execute(F&& task, CDuration<V1, R1>& duration, CDuration<V2, R2>& period) {
		const TimePoint& execute_time = std::chrono::steady_clock::now() + duration;
		const TimePoint& period_time = execute_time + period;
		std::size_t task_index = getTaskIndex();
		addTask<true>(std::make_tuple(execute_time, period_time, std::forward<F>(task), task_index));
		return task_index;
	}

	bool cancel(std::size_t task_index);

private:
	size_t getTaskIndex();

	template<bool set_index> 
	void addTask(const TaskType& task) {
		{
			Lockgd guard{mtx_};
			task_indexes_[std::get<3>(task)] = tasks_.emplace(task); // 存储任务索引和指向任务的指针
		}
		cv_.notify_one();
	}
	template<> 
	void addTask<false>(const TaskType& task) {
		{
			Lockgd guard{mtx_};
			tasks_.emplace(task);
		}
		cv_.notify_one();
	}

	static std::atomic<size_t> task_num_;	// 记录任务序号，用于生成索引

	bool is_start_;							// 线程池是否开始运行
	bool is_wait_;							// 是否有线程等待延时任务
	std::mutex mtx_;						// 保护以下数据成员的互斥锁
	std::condition_variable cv_;			// 条件变量
	std::vector<std::thread> threads_;		// 线程容器
	std::unordered_map<std::size_t, TaskHandle> task_indexes_;	// 记录任务索引和指向任务的指针
	std::multiset<TaskType, CompareTask> tasks_;				// 保存任务的容器
};
