#include "thread_pool.h"
#include <iostream>

std::atomic<std::uint64_t> ThreadPool::task_num_{0};

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true),
	is_wait_(false)
{
	while (thread_num--) {
		//事件循环，用lambda打包提交到thread中
		threads_.emplace_back([this] {
			Ulock u_guard{this->mtx_};		//循环外加锁，不是每次都对循环内的临界区加锁，而是对非临界区解锁
			while (true) {
				if (!this->is_start_) {		//线程池中止，线程退出
					break;
				} else if (!this->tasks_.empty()) {		//任务队列非空
					auto&& execute_time = this->tasks_.begin()->first.base_info_.first;		// 获取第一个任务的执行时间
					auto&& time_diff = std::visit(GetTimeDiff(), execute_time);				// 获取执行时间与当前时间的时间差
					if (time_diff.count() <= 0) {
						std::function<void()> task{std::move(this->tasks_.begin()->second)};
						// 如果是周期任务，并且剩余执行次数不止一次
						auto && period_info = this->tasks_.begin()->first.period_info_;
						if (period_info && period_info->second != 1) {
							// 获取任务句柄
							TaskHandle task_handle{std::move(this->tasks_.begin()->first)};
							this->tasks_.erase(this->tasks_.begin());
							u_guard.unlock();
							// 更新句柄信息
							if (task_handle.period_info_->second)
								--task_handle.period_info_->second;
							std::visit(UpdatePeriodInfo(), task_handle.base_info_.first, task_handle.period_info_->first);
							// 重新添加任务
							this->addTask(task_handle, task);
						} else {
							this->tasks_.erase(this->tasks_.begin());
							u_guard.unlock();
						}
						task();
						u_guard.lock();
					} else if (!this->is_wait_) {
						this->is_wait_ = true;
						this->cv_.wait_until(u_guard, std::visit(GetTimePoint(), execute_time));
						this->is_wait_ = false;
					} else {
						this->cv_.wait(u_guard);
					}
				} else { //等待任务队列出现任务，或者线程池中止
					this->cv_.wait(u_guard);
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
	cv_.notify_all();
	// 回收所有线程
	for (auto && item : threads_)
		item.join();
}

bool ThreadPool::cancel(const TaskHandle & task_handle) {
	Lockgd guard{mtx_};
	return tasks_.erase(task_handle);
}

std::uint64_t ThreadPool::getTaskIndex()
{
	return task_num_.fetch_add(1, std::memory_order_relaxed);
}
