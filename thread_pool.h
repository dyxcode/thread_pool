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
		// ���캯��������ִ��ʱ�䣬�������
		TaskHandle(BaseInfo base_info) :
			base_info_(std::move(base_info)) { /* none */ }

		// ���캯����������Ҫ������Ϣ
		TaskHandle(BaseInfo base_info, PeriodInfo period_info) :
			base_info_(std::move(base_info)),
			period_info_(std::make_optional(std::move(period_info))) { /* none */ }

		BaseInfo base_info_;							// ʹ��pair�������Ƚ�����
		std::optional<PeriodInfo> period_info_;		// ��¼������Ϣ�����ʱ���ִ�д���
	};

	explicit ThreadPool(std::size_t thread_num);

	~ThreadPool();

	// ��֧�ֿ������壨Ҳ��֧���ƶ����壩
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// ���һ����ͨ����
	template<typename F>
	void execute(F&& task) {
		const TaskHandle& task_handle{std::make_pair(SteadyClock::now(), getTaskIndex())};
		addTask(task_handle, std::forward<F>(task));
	}

	// ���һ����ʱ����
	template<typename F>
	TaskHandle execute(F&& task, TimePoint execute_time) {
		const TaskHandle& task_handle{std::make_pair(std::move(execute_time), getTaskIndex())};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// ���һ����ʱ����
	template<typename F, typename R, typename P>
	TaskHandle execute(F&& task, const Duration<R, P>& delay) {
		const TaskHandle& task_handle{std::make_pair(SteadyClock::now() + delay, getTaskIndex())};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// ���һ����ʱ��������
	template<typename F, typename R, typename P>
	TaskHandle execute(F&& task, TimePoint execute_time, const Duration<R, P>& period, std::size_t cycle_num = 0) {
		TimePoint period_time = std::visit(GetTimePoint(), execute_time) + period;
		const TaskHandle& task_handle{std::make_pair(std::move(execute_time), getTaskIndex()),
										std::make_pair(std::move(period_time), cycle_num)};
		addTask(task_handle, std::forward<F>(task));
		return task_handle;
	}

	// ���һ����ʱ��������
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

	// ���ز�ͬʱ�������£�����ʱ����뵱ǰʱ��Ĳ�ֵ
	struct GetTimeDiff {
		template<typename T> typename T::duration operator()(const T& time) { return time - T::clock::now(); }
	};
	// ���ؾ������͵�ʱ���
	struct GetTimePoint {
		template<typename T> T operator()(const T& time) { return time; }
	};
	// �������������ִ��ʱ�������ʱ��s
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
			// ���ִ��ʱ���뵱ǰʱ���ʱ����
			auto&& lhs_time_diff = std::visit(GetTimeDiff(), lhs.base_info_.first);
			auto&& rhs_time_diff = std::visit(GetTimeDiff(), rhs.base_info_.first);
			// ���ʱ������ȣ���Ƚ�������ţ�һ��������ԽС���ύʱ��Խ��
			if (lhs_time_diff == rhs_time_diff) return lhs.base_info_.second < rhs.base_info_.second;
			// ����ʱ����С������
			else return lhs_time_diff < rhs_time_diff;
		}
	};

	static std::atomic<std::uint64_t> task_num_;	// ��¼������ţ�������������

	bool is_start_;							// �̳߳��Ƿ�ʼ����
	bool is_wait_;							// �Ƿ����̵߳ȴ���ʱ����
	std::mutex mtx_;						// �����������ݳ�Ա�Ļ�����
	std::condition_variable cv_;			// ��������
	std::vector<std::thread> threads_;		// �߳�����
	std::map<TaskHandle, std::function<void()>, TaskCmp> tasks_;	// �������������
};
