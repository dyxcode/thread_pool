#include "thread_pool.h"

std::atomic<std::uint64_t> ThreadPool::task_num_{0};

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true),
	thread_waiting_for_delay_(thread_num),		// 该值等于thread_num时表示没有线程正在等待延时任务
	cvs_(thread_num)
{
	while (thread_num--) {
		//事件循环，用lambda打包提交到thread中
		threads_.emplace_back([this, thread_num] {
			Ulock u_guard{this->mtx_};		//循环外加锁，不是每次都对循环内的临界区加锁，而是对非临界区解锁
			while (true) {
				if (!this->is_start_) {		//线程池中止，线程退出
					break;
				} else if (!this->tasks_.empty()) {		//任务队列非空
					auto&& execute_time = this->tasks_.begin()->first.data_->execute_time_;		// 获取第一个任务的执行时间
					if (std::visit(GetDuration(), execute_time).count() <= 0) {					// 如果可以执行
						std::function<void()> task{std::move(this->tasks_.begin()->second)};
						auto && period_info = this->tasks_.begin()->first.data_->period_info_;
						// 如果是周期任务，并且剩余执行次数不止一次
						if (period_info && period_info->second != 1) {
							// 获取任务句柄
							TaskHandle task_handle{std::move(this->tasks_.begin()->first)};
							this->tasks_.erase(this->tasks_.begin());
							u_guard.unlock();
							// 更新句柄信息
							if (task_handle.data_->period_info_->second)
								--task_handle.data_->period_info_->second;
							auto && execute_time = task_handle.data_->execute_time_;
							auto && period_time = task_handle.data_->period_info_->first;
							auto && period = std::visit(GetDuration(), period_time) - std::visit(GetDuration(), execute_time);
							execute_time = period_time;
							period_time = this->addDuration(execute_time, period);
							// 重新添加任务
							this->addTask(task_handle, task);
						} else {	// 其他情况直接删除任务
							auto && task_index = this->tasks_.begin()->first.data_->task_index_;
							this->tasks_indexes_.erase(task_index);
							this->tasks_.erase(this->tasks_.begin());
							u_guard.unlock();
						}
						task();
						u_guard.lock();
					} else if (this->thread_waiting_for_delay_ == this->cvs_.size()) {	// 如果没有线程等待延时任务
						// 构造函数添加等待线程的序号, 析构函数删除等待线程的序号
						ThreadPool::Scheduler::Guard index_guard{&this->scheduler_, thread_num};
						this->thread_waiting_for_delay_ = thread_num;
						this->cvs_[thread_num].wait_for(u_guard, std::visit(GetDuration(), execute_time));
						this->thread_waiting_for_delay_ = this->cvs_.size();
					} else {	// 否则已经有线程等待延时任务了
						ThreadPool::Scheduler::Guard index_guard{&this->scheduler_, thread_num};
						this->cvs_[thread_num].wait(u_guard);
					}
				} else { //等待任务队列出现任务，或者线程池中止
					ThreadPool::Scheduler::Guard index_guard{&this->scheduler_, thread_num};
					this->cvs_[thread_num].wait(u_guard);
				}
			}
		});
	}
}

ThreadPool::~ThreadPool() {
	{ // 对于is_start的修改和工作线程对于is_start的读取要互斥
		Lockgd guard{mtx_};
		is_start_ = false;
	}
	for (auto && item : cvs_)
		item.notify_one();

	for (auto && item : threads_)
		item.join();
}

bool ThreadPool::cancel(const TaskHandle & task_handle) {
	Ulock u_guard{mtx_};
	auto && iter = tasks_indexes_.find(task_handle.data_->task_index_);
	if (iter != tasks_indexes_.end()) {		// 如果找到了任务
		std::size_t notify_thread_index = thread_waiting_for_delay_;
		bool need_notify = (iter->second == tasks_.begin()) && (notify_thread_index != threads_.size());	// 需要唤醒
		tasks_.erase(iter->second);
		tasks_indexes_.erase(iter);
		u_guard.unlock();
		if (need_notify)			// 唤醒等待线程
			cvs_[notify_thread_index].notify_one();
		return true;
	}
	return false;
}

// 获取任务序号
std::uint64_t ThreadPool::getTaskNumber() {
	return task_num_.fetch_add(1, std::memory_order_relaxed);
}

// 构造函数：传入执行时间，任务序号
ThreadPool::TaskHandle::TaskHandle(const TimePoint& execute_time, std::uint64_t task_index) :
	data_(std::make_shared<Data>(execute_time, task_index)) { /* none */ }

// 构造函数：额外需要周期信息
ThreadPool::TaskHandle::TaskHandle(const TimePoint& execute_time, std::uint64_t task_index, const TimePoint& period_time, std::size_t cycle_num) :
	data_(std::make_shared<Data>(execute_time, task_index, std::make_optional<PeriodInfo>(period_time, cycle_num))) { /* none */ }

// 构造函数：实际数据的构造
ThreadPool::TaskHandle::Data::Data(const TimePoint& execute_time, std::uint64_t task_index, std::optional<PeriodInfo> period_info) :
	execute_time_(execute_time), task_index_(task_index), period_info_(std::move(period_info)) { /* none */ }

// 将等待线程的序号放入调度器中
void ThreadPool::Scheduler::put(std::size_t index) {
	index_map_[index] = waiting_thread_indexes_.emplace(waiting_thread_indexes_.end(), index);
}

// 从调度器中应该唤醒的线程
std::size_t ThreadPool::Scheduler::get() {
	return waiting_thread_indexes_.front();
}

// 删除序号
void ThreadPool::Scheduler::remove(std::size_t index) {
	waiting_thread_indexes_.erase(index_map_[index]);
	index_map_.erase(index);
}

// 判断当前是否有线程正在等待
bool ThreadPool::Scheduler::empty() {
	return waiting_thread_indexes_.empty();
}
