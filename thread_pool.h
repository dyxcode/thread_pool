#pragma once

#include <map>
#include <vector>
#include <variant>
#include <optional>
#include <atomic>
#include <type_traits>
#include <condition_variable>

class ThreadPool{
	using Lockgd = std::lock_guard<std::mutex>;
	using Ulock = std::unique_lock<std::mutex>;

	using SystemClock = std::chrono::system_clock;
	using SteadyClock = std::chrono::steady_clock;
	using HighResClock = std::chrono::high_resolution_clock;
	using TimePoint = std::variant<SystemClock::time_point, SteadyClock::time_point/*, HighResClock::time_point*/>;

	template<typename R, typename P> using Duration = std::chrono::duration<R, P>;

	using BaseInfo = std::pair<TimePoint, std::uint64_t>;
	using PeriodInfo = std::pair<TimePoint, std::size_t>;

public:
	class TaskHandle {
		friend class ThreadPool;
		// 构造函数：传入执行时间，任务序号
		TaskHandle(BaseInfo base_info) :
			base_info_(std::move(base_info)) { /* none */ }

		// 构造函数：额外需要周期信息
		TaskHandle(BaseInfo base_info, PeriodInfo period_info) :
			base_info_(std::move(base_info)),
			period_info_(std::make_optional(std::move(period_info))) { /* none */ }

		BaseInfo base_info_;							// 使用pair方便做比较运算
		std::optional<PeriodInfo> period_info_;		// 记录周期信息，间隔时间和执行次数
	};

	explicit ThreadPool(std::size_t thread_num);

	~ThreadPool();

	// 不支持拷贝语义（也不支持移动语义）
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 添加一个普通任务
	template<typename F>
	void execute(F&& task) {
		const TaskHandle& task_handle{std::make_pair(SteadyClock::now(), getTaskIndex())};
		addTask(task_handle, std::forward<F>(task));
	}

	// 添加一个定时任务
	template<typename F>
	TaskHandle execute(F&& task, TimePoint execute_time) {
		const TaskHandle& task_handle{std::make_pair(std::move(execute_time), getTaskIndex())};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// 添加一个延时任务
	template<typename F, typename R, typename P>
	TaskHandle execute(F&& task, const Duration<R, P>& delay) {
		const TaskHandle& task_handle{std::make_pair(SteadyClock::now() + delay, getTaskIndex())};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// 添加一个定时周期任务
	template<typename F, typename R, typename P>
	TaskHandle execute(F&& task, TimePoint execute_time, const Duration<R, P>& period, std::size_t cycle_num = 0) {
		TimePoint period_time = std::visit(GetTimePoint(), execute_time) + period;
		const TaskHandle& task_handle{std::make_pair(std::move(execute_time), getTaskIndex()),
										std::make_pair(std::move(period_time), cycle_num)};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// 添加一个延时周期任务
	template<typename F, typename R1, typename P1 , typename R2, typename P2>
	TaskHandle execute(F&& task, const Duration<R1, P1>& delay, const Duration<R2, P2>& period, std::size_t cycle_num = 0) {
		const TaskHandle& task_handle{std::make_pair(SteadyClock::now() + delay, getTaskIndex()),
										std::make_pair(SteadyClock::now() + delay + period, cycle_num)};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	bool cancel(const TaskHandle& task_handle);

private:
	static std::uint64_t getTaskIndex();

	template<typename F>
	void addTask(const TaskHandle& task_handle, F&& task) {
		{
			Lockgd guard{mtx_};
			tasks_[task_handle] = std::forward<F>(task);
		}
		cv_.notify_one();
	}

	// 返回不同时钟类型下，所给时间点与当前时间的差值
	struct GetTimeDiff {
		template<typename T> typename T::duration operator()(const T& time) { return time - T::clock::now(); }
	};
	// 返回具体类型的时间点
	struct GetTimePoint {
		template<typename T> T operator()(const T& time) { return time; }
	};
	// 更新周期任务的执行时间和周期时间s
	struct UpdatePeriodInfo {
		template<typename T> 
		void operator()(T& execute_time, T& period_time) {
			auto && period = period_time - execute_time;
			execute_time = period_time;
			period_time = execute_time + period;
		}
	};

	struct TaskCmp {
		bool operator()(const TaskHandle& lhs, const TaskHandle& rhs) {
			// 获得执行时间与当前时间的时间间隔
			auto&& lhs_time_diff = std::visit(GetTimeDiff(), lhs.base_info_.first);
			auto&& rhs_time_diff = std::visit(GetTimeDiff(), rhs.base_info_.first);
			// 如果时间间隔相等，则比较任务序号，一般而言序号越小，提交时间越早
			if (lhs_time_diff == rhs_time_diff) return lhs.base_info_.second < rhs.base_info_.second;
			// 否则时间间隔小的优先
			else return lhs_time_diff < rhs_time_diff;
		}
	};

	static std::atomic<std::uint64_t> task_num_;	// 记录任务序号，用于生成索引

	bool is_start_;							// 线程池是否开始运行
	bool is_wait_;							// 是否有线程等待延时任务
	std::mutex mtx_;						// 保护以下数据成员的互斥锁
	std::condition_variable cv_;			// 条件变量
	std::vector<std::thread> threads_;		// 线程容器
	std::map<TaskHandle, std::function<void()>, TaskCmp> tasks_;	// 保存任务的容器
};
