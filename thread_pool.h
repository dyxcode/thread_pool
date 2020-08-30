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
					if (!this->start_) break;	//线程池中止，线程退出
					if (this->tasks_.empty() || this->waiting_for_delay_ != this->cvs_.size()) {
						ThreadGuard index_guard{this->scheduler_, index};
						this->cvs_[index].wait(guard);
					} else {		//任务队列非空
						auto && execute_time = this->tasks_.begin()->first;
						if (execute_time <= Clock::now()) {					// 如果可以执行
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
						} else {	// 如果没有线程等待延时任务
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
		{ // 对于is_start的修改和工作线程对于is_start的读取要互斥
			lock_guard<mutex> guard{mtx_};
			start_ = false;
		}
		for (auto && item : cvs_)
			item.notify_one();
		for (auto && item : threads_)
			item.join();
	}

	// 不支持拷贝语义（也不支持移动语义）
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 添加一个定时任务
	template<typename F>
	void execute(F&& task, const TimePoint& execute_time = Clock::now()) {
		addTask(execute_time, Task(forward(task)));
	}

	// 添加一个延时任务
	template<typename F>
	void execute(F&& task, const Duration& delay) {
		addTask(Clock::now() + delay, Task(forward(task)));
	}

	// 添加一个定时周期任务
	template<typename F>
	void execute(F&& task, const TimePoint& execute_time, const Duration& period, std::size_t times = 0) {
		addTask(execute_time, Task(forward(task), period, times));
	}

	// 添加一个延时周期任务
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
			if (scheduler_.empty()) return;		// 如果当前没有等待线程，就不需要notify
			// 如果当前有线程正在等待延时任务，则应该唤醒这个线程，否则唤醒scheduler指定的线程
			index = (waiting_for_delay_ == cvs_.size() ? scheduler_.get() : waiting_for_delay_);
		}
		cvs_[index].notify_one();
		return [execute_time, iter, this]{
			unique_lock<mutex> guard{mtx_};
			auto range = this->tasks_.equal_range(execute_time);
			for (auto it = range.first; it != range.second; ++it) {
				if (it == iter) {
					std::size_t index = this->waiting_for_delay_;
					bool need_notify = (iter == this->tasks_.begin()) && (index != this->cvs_.size());	// 需要唤醒
					tasks_.erase(iter);
					guard.unlock();
					if (need_notify)			// 唤醒等待线程
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
	bool start_;								// 线程池是否开始运行
	size_t waiting_for_delay_;		// 等待延时任务线程序号
	mutex mtx_;							// 保护数据成员的互斥锁
	multimap<TimePoint, Task> tasks_;

	vector<condition_variable> cvs_;
	vector<thread> threads_;
	detail::Scheduler<size_t> scheduler_;
};

}  // namespace std::dyx
