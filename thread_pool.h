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

	using PeriodTask = std::pair<TimePoint, std::size_t>;
	// ִ��ʱ��,����ʱ��,ִ�к���,�������
	using TaskType = std::tuple<TimePoint, std::shared_ptr<PeriodTask>, std::function<void()>, std::size_t>;
	struct CompareTask {
		bool operator()(const TaskType& lhs, const TaskType& rhs) {
			return std::less<TimePoint>()(std::get<0>(lhs), std::get<0>(rhs));
		}
	};
	using TaskHandle = std::multiset<TaskType, CompareTask>::iterator;

public:
	explicit ThreadPool(std::size_t thread_num);

	~ThreadPool();

	// ��֧�ֿ������壨Ҳ��֧���ƶ����壩
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// ���һ����ͨ����
	template<typename F>
	void execute(F&& task) {
		const TimePoint& now = std::chrono::steady_clock::now();
		addTask<false>(std::make_tuple(now, nullptr, std::forward<F>(task), getTaskIndex()));
	}

	// ���һ����ʱ����
	template<typename F, typename V, typename R>
	std::size_t execute(F&& task, CDuration<V, R>& duration) {
		const TimePoint& execute_time = std::chrono::steady_clock::now() + duration;
		std::size_t task_index = getTaskIndex();
		addTask<true>(std::make_tuple(execute_time, nullptr, std::forward<F>(task), task_index));
		return task_index;
	}

	// ���һ����ʱ����
	template<typename F, typename C>
	std::size_t execute(F&& task, std::chrono::time_point<C> execute_time) {
		auto duration = execute_time - C::now();
		return execute(std::forward<F>(task), duration);
	}

	// ���һ����ʱ��������
	template<typename F, typename V1, typename R1, typename V2, typename R2>
	std::size_t execute(F&& task, CDuration<V1, R1>& duration, CDuration<V2, R2>& period, std::size_t cycle_num = 0) {
		const TimePoint& execute_time = std::chrono::steady_clock::now() + duration;
		const TimePoint& period_time = execute_time + period;
		std::size_t task_index = getTaskIndex();
		addTask<true>(std::make_tuple(execute_time, std::make_shared<PeriodTask>(period_time, cycle_num), 
						std::forward<F>(task), task_index));
		return task_index;
	}

	// ���һ����ʱ��������
	template<typename F, typename C, typename V, typename R>
	std::size_t execute(F&& task, std::chrono::time_point<C> execute_time, CDuration<V, R>& period, std::size_t cycle_num = 0) {
		auto duration = execute_time - typename C::now();
		return execute(std::forward<F>(task), duration, period, cycle_num);
	}

	bool cancel(std::size_t task_index);

private:
	static size_t getTaskIndex();

	template<bool set_index> 
	void addTask(const TaskType& task) {
		{
			Lockgd guard{mtx_};
			task_indexes_[std::get<3>(task)] = tasks_.emplace(task); // �洢����������ָ�������ָ��
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

	static std::atomic<size_t> task_num_;	// ��¼������ţ�������������

	bool is_start_;							// �̳߳��Ƿ�ʼ����
	bool is_wait_;							// �Ƿ����̵߳ȴ���ʱ����
	std::mutex mtx_;						// �����������ݳ�Ա�Ļ�����
	std::condition_variable cv_;			// ��������
	std::vector<std::thread> threads_;		// �߳�����
	std::unordered_map<std::size_t, TaskHandle> task_indexes_;	// ��¼����������ָ�������ָ��
	std::multiset<TaskType, CompareTask> tasks_;				// �������������
};
