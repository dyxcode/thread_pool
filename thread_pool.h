#pragma once

#include <map>
#include <vector>
#include <variant>
#include <optional>
#include <atomic>
#include <condition_variable>

class ThreadPool{
	using Lockgd = std::lock_guard<std::mutex>;
	using Ulock = std::unique_lock<std::mutex>;

	using SystemClock = std::chrono::system_clock;
	using SteadyClock = std::chrono::steady_clock;
	using HighResClock = std::chrono::high_resolution_clock;
	// 时间点变体，因为在VS中high_resolution_clock等于steady_clock，所以暂时注释掉
	using TimePoint = std::variant<SystemClock::time_point, SteadyClock::time_point/*, HighResClock::time_point*/>;

	// 周期信息（可能没有）
	using PeriodInfo = std::pair<TimePoint, std::size_t>;
	
	template<typename R, typename P> using Duration = std::chrono::duration<R, P>;

public:
	class TaskHandle {
		friend class ThreadPool;
	public:
		TaskHandle(TaskHandle&&) = default;
		TaskHandle(const TaskHandle&) = default;
	private:
		// 构造函数：传入执行时间，任务序号
		TaskHandle(const TimePoint& execute_time, std::uint64_t task_index);
		// 构造函数：额外需要周期信息
		TaskHandle(const TimePoint& execute_time, std::uint64_t task_index, const TimePoint& period_time, std::size_t cycle_num);

		struct Data {
			Data(const TimePoint& execute_time, std::uint64_t task_index, std::optional<PeriodInfo> period_info = std::nullopt);

			TimePoint execute_time_;
			std::uint64_t task_index_;
			std::optional<PeriodInfo> period_info_; // 记录周期信息，间隔时间和执行次数
		};
		std::shared_ptr<Data> data_;	// 避免拷贝，节省空间
	};

	explicit ThreadPool(std::size_t thread_num);

	~ThreadPool();

	// 不支持拷贝语义（也不支持移动语义）
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 添加一个普通任务
	template<typename F>
	void execute(F&& task) {
		TaskHandle task_handle{SteadyClock::now(), getTaskNumber()};
		addTask(std::move(task_handle), std::forward<F>(task));
	}

	// 添加一个定时任务
	template<typename F>
	TaskHandle execute(F&& task, const TimePoint& execute_time) {
		TaskHandle task_handle{execute_time, getTaskNumber()};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// 添加一个延时任务
	template<typename F, typename R, typename P>
	TaskHandle execute(F&& task, const Duration<R, P>& delay) {
		TaskHandle task_handle{SteadyClock::now() + delay, getTaskNumber()};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// 添加一个定时周期任务
	template<typename F, typename R, typename P>
	TaskHandle execute(F&& task, const TimePoint& execute_time, const Duration<R, P>& period, std::size_t cycle_num = 0) {
		TaskHandle task_handle{execute_time, getTaskNumber(), addDuration(execute_time, period), cycle_num};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// 添加一个延时周期任务
	template<typename F, typename R1, typename P1 , typename R2, typename P2>
	TaskHandle execute(F&& task, const Duration<R1, P1>& delay, const Duration<R2, P2>& period, std::size_t cycle_num = 0) {
		TaskHandle task_handle{SteadyClock::now() + delay, getTaskNumber(), SteadyClock::now() + delay + period, cycle_num};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// 取消任务
	bool cancel(const TaskHandle& task_handle);

private:
	// 获取任务序号
	static std::uint64_t getTaskNumber();
	
	template<typename F>
	void addTask(TaskHandle task_handle, F&& task) {
		std::size_t thread_index;
		{
			Lockgd guard{mtx_};
			std::uint64_t index = task_handle.data_->task_index_;
			tasks_indexes_[index] = tasks_.emplace(std::move(task_handle), std::forward<F>(task));
			if (scheduler_.empty()) return;		// 如果当前没有等待线程，就不需要notify
			// 如果当前有线程正在等待延时任务，则应该唤醒这个线程，否则唤醒scheduler指定的线程
			thread_index = (thread_waiting_for_delay_ == threads_.size() ? scheduler_.get() : thread_waiting_for_delay_);
		}
		cvs_[thread_index].notify_one();
	}

	// 增加一段时长
	template<typename R, typename P>
	static TimePoint addDuration(const TimePoint& time, const Duration<R, P>& duration) {
		TimePoint time_since_epoch;
		if (time.index() == 0) 
			time_since_epoch = SystemClock::time_point(std::chrono::duration_cast<SystemClock::duration>(duration));
		else if (time.index() == 1) 
			time_since_epoch = SteadyClock::time_point(std::chrono::duration_cast<SteadyClock::duration>(duration));
		return std::visit(AddDuration(), time, time_since_epoch);
	}

	// 获取不同类型时间点的差值
	struct CmpTimePoint {
		template<typename T1, typename T2>
		bool operator()(const T1& lhs, const T2& rhs) const { 
			return lhs - T1::clock::now() < rhs - T2::clock::now();
		}
		
	};
	// 增加一段时长
	struct AddDuration {
		template<typename T1, typename T2>
		TimePoint operator()(const T1& lhs, const T2& rhs) const {
			return std::chrono::time_point_cast<T1::duration>(lhs + rhs.time_since_epoch()); 
		}
	};
	// 得到距离当前时间的时长
	struct GetDuration {
		template<typename T> std::chrono::nanoseconds operator()(const T& time_point) const { 
			return time_point - T::clock::now();
		}
	};

	struct TaskCmp {
		bool operator()(const TaskHandle& lhs, const TaskHandle& rhs) const {
			return std::visit(CmpTimePoint(), lhs.data_->execute_time_, rhs.data_->execute_time_);
		}
	};

	class Scheduler {
	public:
		// 构造函数添加等待线程的序号, 析构函数删除等待线程的序号
		struct Guard {
			Guard(Scheduler* scheduler, std::size_t index) : 
				scheduler_(scheduler), index_(index) { scheduler_->put(index_); }
			~Guard() { scheduler_->remove(index_); }
			Scheduler* scheduler_;
			std::size_t index_;
		};
		void put(std::size_t index);
		std::size_t get();
		void remove(std::size_t index);
		bool empty();
	private:
		std::unordered_map<std::size_t, std::list<std::size_t>::iterator> index_map_; // 方便找到对应的线程结点,
		std::list<std::size_t> waiting_thread_indexes_;								// 等待在条件变量上的线程序号
	};

private:
	using TaskContainer = std::multimap<TaskHandle, std::function<void()>, TaskCmp>;
	using TaskIndexMap = std::unordered_map<std::uint64_t, TaskContainer::iterator>;

	TaskIndexMap tasks_indexes_;	// 任务索引到指向任务的迭代器
	TaskContainer tasks_;			// 保存任务的容器
	Scheduler scheduler_;			// 线程调度器

	bool is_start_;								// 线程池是否开始运行
	std::size_t thread_waiting_for_delay_;		// 等待延时任务线程序号

	std::vector<std::condition_variable> cvs_;			// 条件变量
	std::vector<std::thread> threads_;					// 线程容器
	std::mutex mtx_;							// 保护数据成员的互斥锁

	static std::atomic<std::uint64_t> task_num_;	// 记录任务序号，用于生成索引
};
