#pragma once

#include <map>
#include <optional>
#include <condition_variable>

namespace std::dyx {

namespace detail {

template<typename Clock>
class PeriodicTask {
	using PeriodInfo = pair<typename Clock::duration, size_t>;
public:
	template<typename T> PeriodicTask(T&& task)
	:task_(forward<T>(task)) { /* none */ }
		
	template<typename T> PeriodicTask(T&& task, const typename Clock::duration& period, size_t times)
	:task_(forward<T>(task)), period_info_(make_optional<PeriodInfo>(period, times)) { /* none */ }

	optional<typename Clock::duration> getPeriod() const {
		if (period_info_.has_value() && period_info_.value().second != 1)
			return period_info_.value().first;
		return nullopt;
	}

	void operator()() {
		task_(); 
		if (period_info_.has_value() && period_info_.value().second > 1)
			--period_info_.value().second;
	}
	
	PeriodicTask(const PeriodicTask&) = delete;
	PeriodicTask(PeriodicTask&&) = default;

private:
	function<void()> task_;
	optional<PeriodInfo> period_info_;
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
	using PeriodicTask = detail::PeriodicTask<Clock>;
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
							if (has_task && !this->scheduler_.empty())
								this->cvs_[this->scheduler_.get()].notify_one();
							PeriodicTask& task = map_node.mapped();
							if (task.getPeriod().has_value()) {
								map_node.key() += task.getPeriod().value();
								this->tasks_.insert(move(map_node));
							}
							task();
							guard.lock();
						} else {	// ���û���̵߳ȴ���ʱ����
							ThreadGuard index_guard{this->scheduler_, index};
							WaitingGuard waiting_guard{this->waiting_for_delay_, index, this->cvs_.size()};
							this->cvs_[index].wait_until(guard, execute_time);
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
	auto execute(F&& task, const TimePoint& execute_time = Clock::now()) {
		return addTask(execute_time, PeriodicTask(forward<F>(task)));
	}

	// ���һ����ʱ����
	template<typename F>
	auto execute(F&& task, const Duration& delay) {
		return addTask(Clock::now() + delay, PeriodicTask(forward<F>(task)));
	}

	// ���һ����ʱ��������
	template<typename F>
	auto execute(F&& task, const TimePoint& execute_time, const Duration& period, std::size_t times = 0) {
		return addTask(execute_time, PeriodicTask(forward<F>(task), period, times));
	}

	// ���һ����ʱ��������
	template<typename F>
	auto execute(F&& task, const Duration& delay, const Duration& period, std::size_t times = 0) {
		return addTask(Clock::now() + delay, PeriodicTask(forward<F>(task), period, times));
	}

private:
	auto addTask(const TimePoint& execute_time, PeriodicTask&& task) {
		size_t index;
		typename multimap<TimePoint, PeriodicTask>::iterator iter;
		{
			lock_guard<mutex> guard{mtx_};
			iter = tasks_.emplace(execute_time, move(task));
			if (scheduler_.empty())// �����ǰû�еȴ��̣߳��Ͳ���Ҫnotify
				index = cvs_.size();
			else // �����ǰ���߳����ڵȴ���ʱ������Ӧ�û�������̣߳�������schedulerָ�����߳�
				index = (waiting_for_delay_ == cvs_.size() ? scheduler_.get() : waiting_for_delay_);
		}
		if (index != cvs_.size())
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
	multimap<TimePoint, PeriodicTask> tasks_;

	vector<condition_variable> cvs_;
	vector<thread> threads_;
	detail::Scheduler<size_t> scheduler_;
};

}  // namespace std::dyx
