#pragma once

#include <map>
#include <optional>
#include <condition_variable>

namespace std::dyx {

namespace detail {

template<typename Clock>
struct PeriodicTask {
	using PeriodInfo = tuple<function<void()>, Clock::duration, size_t>;

	template<typename T> PeriodicTask(T&& task)
	:task_([task = forward<T>(task)]() mutable -> optional<PeriodInfo> { 
		task();  
		return nullopt; 
	}) { /* none */ }
		
	template<typename T> PeriodicTask(T&& task, const Clock::duration& period, size_t times)
	:task_([task = forward<T>(task), period, times]() mutable -> optional<PeriodInfo> {
		if (times) task();
		if (times > 1) return make_optional<PeriodInfo>(task, period, times - 1);
		return nullopt;
	}) { /* none */ }
	
	PeriodicTask(const PeriodicTask&) = delete;
	PeriodicTask(PeriodicTask&&) = default;

	optional<PeriodInfo> operator()() { return task_(); }

	function<optional<PeriodInfo>()> f_;
};

}  // namespace detail

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
	// ���ȴ��̵߳���ŷ����������
	void put(std::size_t index) {
		index_map_[index] = waiting_thread_indexes_.emplace(waiting_thread_indexes_.end(), index);
	}
	// �ӵ�������Ӧ�û��ѵ��߳�
	std::size_t get() {
		return waiting_thread_indexes_.front();
	}
	// ɾ�����
	void remove(std::size_t index) {
		waiting_thread_indexes_.erase(index_map_[index]);
		index_map_.erase(index);
	}
	// �жϵ�ǰ�Ƿ����߳����ڵȴ�
	bool empty() {
		return waiting_thread_indexes_.empty();
	}
private:
	std::unordered_map<std::size_t, std::list<std::size_t>::iterator> index_map_; // �����ҵ���Ӧ���߳̽��,
	std::list<std::size_t> waiting_thread_indexes_;								// �ȴ������������ϵ��߳����
};

template<typename Clock>
class ThreadPool {
	// ������Ϣ������û�У�
	using PeriodInfo = std::pair<TimePoint, std::size_t>;
	
	template<typename R, typename P> using Duration = std::chrono::duration<R, P>;

public:
	explicit ThreadPool(std::size_t thread_num) 
		: running_(true), waiting_for_delay_(thread_num), cvs_(thread_num) {
		while (thread_num--) {
			threads_.emplace_back([index = thread_num, *this] {
				unique_lock<mutex> guard{pool_mgr_.mtx_};
				while (true) {
					if (running) break;	//�̳߳���ֹ���߳��˳�
					if (tasks_.empty() || waiting_for_delay_ != cvs_.size()) {
						ThreadPool::Scheduler::Guard index_guard{&scheduler_, thread_num};
						cvs_[index].wait(guard);
					} else {		//������зǿ�
						if (tasks_.begin()->first <= Clock::now()) {					// �������ִ��
							auto map_node = tasks_.extract(tasks_.begin());
							guard.unlock();
							optional<PeriodInfo<Clock>> ret = node.value()();
							if (!ret.has_value())
								guard.lock();
							else {
								auto& [task, period, times] = ret.value();
								map_node.key() += period;
								map_node.value() = detail::PeriodicTask(task, period, times);
								guard.lock();
								tasks_.insert(map_node);
							}
						} else {	// ���û���̵߳ȴ���ʱ����
							ThreadPool::Scheduler::Guard index_guard{&this->scheduler_, thread_num};
							waiting_for_delay_ = thread_num;
							cvs_[index].wi(u_guard, std::visit(GetDuration(), execute_time));
							waiting_for_delay_ = cvs_.size();
						}
					}
				}
			});

	}
	}

	~ThreadPool() {
		{ // ����is_start���޸ĺ͹����̶߳���is_start�Ķ�ȡҪ����
			Lockgd guard{mtx_};
			is_start_ = false;
		}
		for (auto && item : cvs_)
			item.notify_one();

		for (auto && item : threads_)
			item.join();
	}

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
	bool cancel(const TaskHandle& task_handle) {
		Ulock u_guard{mtx_};
		auto && iter = tasks_indexes_.find(task_handle.data_->task_index_);
		if (iter != tasks_indexes_.end()) {		// ����ҵ�������
			std::size_t notify_thread_index = thread_waiting_for_delay_;
			bool need_notify = (iter->second == tasks_.begin()) && (notify_thread_index != threads_.size());	// ��Ҫ����
			tasks_.erase(iter->second);
			tasks_indexes_.erase(iter);
			u_guard.unlock();
			if (need_notify)			// ���ѵȴ��߳�
				cvs_[notify_thread_index].notify_one();
			return true;
		}
		return false;
	}

private:
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

private:
	bool running_;								// �̳߳��Ƿ�ʼ����
	size_t waiting_for_delay_;		// �ȴ���ʱ�����߳����
	mutex mtx_;							// �������ݳ�Ա�Ļ�����

	vector<condition_variable> cvs_;			// ��������
	vector<thread> threads_;					// �߳�����
	multimap<typename Clock::time_point, detail::PeriodicTask<Clock>> tasks_;
};

}  // namespace std::dyx
