#include "thread_pool.h"
#include <iostream>

std::atomic<size_t> ThreadPool::task_num_{0};

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true),
	is_wait_(false)
{
	while (thread_num--) {
		//事件循环，用lambda打包提交到thread中
		threads_.emplace_back([this] {
			//循环外加锁，不是每次都对循环内的临界区加锁，而是对非临界区解锁
			Ulock u_guard{this->mtx_};
			while (true) {
				if (!this->tasks_.empty()) { //任务队列非空
					const TimePoint& now = std::chrono::steady_clock::now(); // 获取当前时间
					const TimePoint& execute_time = std::get<0>(*(this->tasks_.cbegin())); //获取第一个任务的执行时间
					if (now >= execute_time) {
						auto task{std::move(*(this->tasks_.begin()))};
						this->tasks_.erase(this->tasks_.begin());
						//执行任务时解锁
						if (std::get<0>(task) == std::get<1>(task)) { // 如果不是周期任务
							this->task_indexes_.erase(std::get<3>(task));
							u_guard.unlock();
						} else {
							u_guard.unlock();
							auto period = std::get<1>(task) - std::get<0>(task); //获取周期
							std::get<0>(task) = std::get<1>(task);               //设置下一次执行时间
							std::get<1>(task) = std::get<0>(task) + period;		 //设置下下次执行时间
							this->addTask<true>(task);
						}
						std::get<2>(task)();
						u_guard.lock();
					} else if (!this->is_wait_) {
						this->is_wait_ = true;
						this->cv_.wait_until(u_guard, execute_time);
						this->is_wait_ = false;
					}
				} else if (!this->is_start_) { //线程池中止，线程退出
					break;
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

bool ThreadPool::cancel(std::size_t task_index)
{
	Lockgd guard{mtx_};
	auto task_handle = task_indexes_.find(task_index);
	if (task_handle != task_indexes_.end()) {
		tasks_.erase(task_handle->second);
		task_indexes_.erase(task_handle);
		return true;
	}
	return false;
}

size_t ThreadPool::getTaskIndex()
{
	return static_cast<size_t>(std::chrono::steady_clock::now().time_since_epoch().count() << 8) + (++task_num_ % (1 << 8));
}
