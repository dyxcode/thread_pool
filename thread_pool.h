#pragma once

#include <map>
#include <optional>
#include <condition_variable>

namespace std::dyx {

namespace detail {

template<typename Clock>
class PeriodicTask {
	using PeriodInfo = tuple<function<void()>, typename Clock::duration, size_t>;
public:
	template<typename T> PeriodicTask(T&& task)
	:task_([task = forward<T>(task)]() mutable -> optional<PeriodInfo> { 
		task();  
		return nullopt; 
	}) { /* none */ }
		
	template<typename T> PeriodicTask(T&& task, const typename Clock::duration& period, size_t times)
	:task_([task = forward<T>(task), period, times]() mutable -> optional<PeriodInfo> {
		task();
		if (times == 0) return make_optional<PeriodInfo>(move(task), period, times);
		if (times >= 2) return make_optional<PeriodInfo>(move(task), period, times - 1);
		return nullopt;
	}) { /* none */ }

	optional<PeriodInfo> operator()() { return task_(); }
	
	PeriodicTask(const PeriodicTask&) = delete;
	PeriodicTask(PeriodicTask&&) = default;

private:
	function<optional<PeriodInfo>()> task_;
};

template<typename T>
class Scheduler {
public:
	void put(const T& key) {
		map_[key] = waiting_items_.emplace(waiting_items_.end(), key);
	}
	T get() const {
		return waiting_items_.front();
	}
	void remove(const T& key) {
		waiting_items_.erase(map_[key]);
		map_.erase(key);
	}
	bool empty() const {
		return waiting_items_.empty();
	}
private:
	unordered_map<T, typename list<T>::iterator> map_;
	list<T> waiting_items_;
};

}  // namespace detail

template<typename Clock>
class ThreadPool {
	using TimePoint = typename Clock::time_point;
	using Duration = typename Clock::duration;
	using Task = detail::PeriodicTask<Clock>;
public:
	explicit ThreadPool(size_t thread_num) : start_(true), waiting_for_delay_(thread_num), cvs_(thread_num) {
		while (thread_num--) {
			threads_.emplace_back([index = thread_num, this]{
				unique_lock<mutex> guard{this->mtx_};
				while (true) {
					if (!this->start_) break;	//�̳߳���ֹ���߳��˳�
					if (this->tasks_.empty() || this->waiting_for_delay_ != this->cvs_.size()) {
						ThreadGuard index_guard{this->scheduler_, index};
						this->cvs_[index].wait(guard);
					} else {		//������зǿ�
						auto && execute_time = this->tasks_.begin()->first;
						if (execute_time <= Clock::now()) {					// �������ִ��
							auto map_node = this->tasks_.extract(this->tasks_.begin());
							bool has_task = !this->tasks_.empty();
							guard.unlock();
							if (has_task)
								this->cvs_[this->scheduler_.get()].notify_one();
							auto ret = map_node.value()();
							if (!ret.has_value())
								guard.lock();
							else {
								auto& [task, period, times] = ret.value();
								map_node.key() += get<1>(ret.value());
								map_node.value() = detail::PeriodicTask(move(task), period, times);
								guard.lock();
								this->tasks_.insert(map_node);
							}
						} else {	// ���û���̵߳ȴ���ʱ����
							ThreadGuard index_guard{this->scheduler_, index};
							WaitingGuard waiting_guard{this->waiting_for_delay_, index, this->cvs_.size()};
							this->cvs_[index].wait_until(execute_time);
						}
					}
				}
			});
		}
	}

	~ThreadPool() {
		{ // ����is_start���޸ĺ͹����̶߳���is_start�Ķ�ȡҪ����
			lock_guard<mutex> guard{mtx_};
			start_ = false;
		}
		for (auto && item : cvs_)
			item.notify_one();
		for (auto && item : threads_)
			item.join();
	}

	// ��֧�ֿ������壨Ҳ��֧���ƶ����壩
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// ���һ����ʱ����
	template<typename F>
	void execute(F&& task, const TimePoint& execute_time = Clock::now()) {
		addTask(execute_time, Task(forward(task)));
	}

	// ���һ����ʱ����
	template<typename F>
	void execute(F&& task, const Duration& delay) {
		addTask(Clock::now() + delay, Task(forward(task)));
	}

	// ���һ����ʱ��������
	template<typename F>
	void execute(F&& task, const TimePoint& execute_time, const Duration& period, std::size_t times = 0) {
		addTask(execute_time, Task(forward(task), period, times));
	}

	// ���һ����ʱ��������
	template<typename F>
	void execute(F&& task, const Duration& delay, const Duration& period, std::size_t times = 0) {
		addTask(Clock::now() + delay, Task(forward(task), period, times));
	}

private:
	auto addTask(const TimePoint& execute_time, Task&& task) {
		size_t index;
		typename multimap<TimePoint, Task>::iterator iter;
		{
			lock_guard<mutex> guard{mtx_};
			iter = tasks_.emplace(execute_time, move(task));
			if (scheduler_.empty()) return;		// �����ǰû�еȴ��̣߳��Ͳ���Ҫnotify
			// �����ǰ���߳����ڵȴ���ʱ������Ӧ�û�������̣߳�������schedulerָ�����߳�
			index = (waiting_for_delay_ == cvs_.size() ? scheduler_.get() : waiting_for_delay_);
		}
		cvs_[index].notify_one();
		return [execute_time, iter, this]{
			unique_lock<mutex> guard{mtx_};
			auto range = this->tasks_.equal_range(execute_time);
			for (auto it = range.first; it != range.second; ++it) {
				if (it == iter) {
					std::size_t index = this->waiting_for_delay_;
					bool need_notify = (iter == this->tasks_.begin()) && (index != this->cvs_.size());	// ��Ҫ����
					tasks_.erase(iter);
					guard.unlock();
					if (need_notify)			// ���ѵȴ��߳�
						cvs_[index].notify_one();
					return ;
				}
			}
		};
	}

	struct ThreadGuard {
		ThreadGuard(detail::Scheduler<size_t>& scheduler, size_t index) 
			: scheduler_(scheduler), index_(index) { scheduler_.put(index_); }
		~ThreadGuard() { scheduler_.remove(index_); }
		detail::Scheduler<size_t>& scheduler_;
		size_t index_;
	};
	struct WaitingGuard {
		WaitingGuard(size_t& waiting_for_delay, size_t i, size_t n) 
			: waiting_for_delay_(waiting_for_delay), n_(n) { waiting_for_delay = i; }
		~WaitingGuard() { waiting_for_delay_ = n_; }
		size_t& waiting_for_delay_;
		size_t n_;
	};

private:
	bool start_;								// �̳߳��Ƿ�ʼ����
	size_t waiting_for_delay_;		// �ȴ���ʱ�����߳����
	mutex mtx_;							// �������ݳ�Ա�Ļ�����
	multimap<TimePoint, Task> tasks_;

	vector<condition_variable> cvs_;
	vector<thread> threads_;
	detail::Scheduler<size_t> scheduler_;
};

}  // namespace std::dyx
