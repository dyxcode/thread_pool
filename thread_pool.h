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
	// ʱ�����壬��Ϊ��VS��high_resolution_clock����steady_clock��������ʱע�͵�
	using TimePoint = std::variant<SystemClock::time_point, SteadyClock::time_point/*, HighResClock::time_point*/>;

	// ������Ϣ������û�У�
	using PeriodInfo = std::pair<TimePoint, std::size_t>;
	
	template<typename R, typename P> using Duration = std::chrono::duration<R, P>;

public:
	class TaskHandle {
		friend class ThreadPool;
	public:
		TaskHandle(TaskHandle&&) = default;
		TaskHandle(const TaskHandle&) = default;
	private:
		// ���캯��������ִ��ʱ�䣬�������
		TaskHandle(const TimePoint& execute_time, std::uint64_t task_index);
		// ���캯����������Ҫ������Ϣ
		TaskHandle(const TimePoint& execute_time, std::uint64_t task_index, const TimePoint& period_time, std::size_t cycle_num);

		struct Data {
			Data(const TimePoint& execute_time, std::uint64_t task_index, std::optional<PeriodInfo> period_info = std::nullopt);

			TimePoint execute_time_;
			std::uint64_t task_index_;
			std::optional<PeriodInfo> period_info_; // ��¼������Ϣ�����ʱ���ִ�д���
		};
		std::shared_ptr<Data> data_;	// ���⿽������ʡ�ռ�
	};

	explicit ThreadPool(std::size_t thread_num);

	~ThreadPool();

	// ��֧�ֿ������壨Ҳ��֧���ƶ����壩
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// ���һ����ͨ����
	template<typename F>
	void execute(F&& task) {
		TaskHandle task_handle{SteadyClock::now(), getTaskNumber()};
		addTask(std::move(task_handle), std::forward<F>(task));
	}

	// ���һ����ʱ����
	template<typename F>
	TaskHandle execute(F&& task, const TimePoint& execute_time) {
		TaskHandle task_handle{execute_time, getTaskNumber()};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// ���һ����ʱ����
	template<typename F, typename R, typename P>
	TaskHandle execute(F&& task, const Duration<R, P>& delay) {
		TaskHandle task_handle{SteadyClock::now() + delay, getTaskNumber()};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// ���һ����ʱ��������
	template<typename F, typename R, typename P>
	TaskHandle execute(F&& task, const TimePoint& execute_time, const Duration<R, P>& period, std::size_t cycle_num = 0) {
		TaskHandle task_handle{execute_time, getTaskNumber(), addDuration(execute_time, period), cycle_num};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// ���һ����ʱ��������
	template<typename F, typename R1, typename P1 , typename R2, typename P2>
	TaskHandle execute(F&& task, const Duration<R1, P1>& delay, const Duration<R2, P2>& period, std::size_t cycle_num = 0) {
		TaskHandle task_handle{SteadyClock::now() + delay, getTaskNumber(), SteadyClock::now() + delay + period, cycle_num};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// ȡ������
	bool cancel(const TaskHandle& task_handle);

private:
	// ��ȡ�������
	static std::uint64_t getTaskNumber();
	
	template<typename F>
	void addTask(TaskHandle task_handle, F&& task) {
		std::size_t thread_index;
		{
			Lockgd guard{mtx_};
			std::uint64_t index = task_handle.data_->task_index_;
			tasks_indexes_[index] = tasks_.emplace(std::move(task_handle), std::forward<F>(task));
			if (scheduler_.empty()) return;		// �����ǰû�еȴ��̣߳��Ͳ���Ҫnotify
			// �����ǰ���߳����ڵȴ���ʱ������Ӧ�û�������̣߳�������schedulerָ�����߳�
			thread_index = (thread_waiting_for_delay_ == threads_.size() ? scheduler_.get() : thread_waiting_for_delay_);
		}
		cvs_[thread_index].notify_one();
	}

	// ����һ��ʱ��
	template<typename R, typename P>
	static TimePoint addDuration(const TimePoint& time, const Duration<R, P>& duration) {
		TimePoint time_since_epoch;
		if (time.index() == 0) 
			time_since_epoch = SystemClock::time_point(std::chrono::duration_cast<SystemClock::duration>(duration));
		else if (time.index() == 1) 
			time_since_epoch = SteadyClock::time_point(std::chrono::duration_cast<SteadyClock::duration>(duration));
		return std::visit(AddDuration(), time, time_since_epoch);
	}

	// ��ȡ��ͬ����ʱ���Ĳ�ֵ
	struct CmpTimePoint {
		template<typename T1, typename T2>
		bool operator()(const T1& lhs, const T2& rhs) const { 
			return lhs - T1::clock::now() < rhs - T2::clock::now();
		}
		
	};
	// ����һ��ʱ��
	struct AddDuration {
		template<typename T1, typename T2>
		TimePoint operator()(const T1& lhs, const T2& rhs) const {
			return std::chrono::time_point_cast<T1::duration>(lhs + rhs.time_since_epoch()); 
		}
	};
	// �õ����뵱ǰʱ���ʱ��
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
		// ���캯����ӵȴ��̵߳����, ��������ɾ���ȴ��̵߳����
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
		std::unordered_map<std::size_t, std::list<std::size_t>::iterator> index_map_; // �����ҵ���Ӧ���߳̽��,
		std::list<std::size_t> waiting_thread_indexes_;								// �ȴ������������ϵ��߳����
	};

private:
	using TaskContainer = std::multimap<TaskHandle, std::function<void()>, TaskCmp>;
	using TaskIndexMap = std::unordered_map<std::uint64_t, TaskContainer::iterator>;

	TaskIndexMap tasks_indexes_;	// ����������ָ������ĵ�����
	TaskContainer tasks_;			// �������������
	Scheduler scheduler_;			// �̵߳�����

	bool is_start_;								// �̳߳��Ƿ�ʼ����
	std::size_t thread_waiting_for_delay_;		// �ȴ���ʱ�����߳����

	std::vector<std::condition_variable> cvs_;			// ��������
	std::vector<std::thread> threads_;					// �߳�����
	std::mutex mtx_;							// �������ݳ�Ա�Ļ�����

	static std::atomic<std::uint64_t> task_num_;	// ��¼������ţ�������������
};
