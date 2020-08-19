#include "thread_pool.h"

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true)
{
	while (thread_num--) {
		//事件循环，用lambda打包提交到thread中
		threads_.emplace_back([this] {
			//循环外加锁，不是每次都对循环内的临界区加锁，而是对非临界区解锁
			Ulock u_guard{this->mtx_};
			while (true) {
				if (!this->tasks_.empty()) { //任务队列非空
					CTimePoint& now = std::chrono::steady_clock::now();
					CTimePoint& execute_time = std::get<0>(this->tasks_.cbegin()->first);
					if (now >= execute_time) {
						auto task{std::move(*(this->tasks_.begin()))};
						this->tasks_.erase(this->tasks_.begin());
						//执行任务时解锁
						u_guard.unlock();
						if (std::get<0>(task.first) != std::get<2>(task.first)) {
							auto period = std::get<2>(task.first) - std::get<0>(task.first);
							CTimePoint& execute_time = std::get<2>(task.first);
							CTimePoint& period_time = execute_time + period;
							this->addTask(std::make_tuple(execute_time, now, period_time), task.second);
						}
						task.second();
						u_guard.lock();
					} else {
						this->cv_.wait_until(u_guard, execute_time);
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

bool ThreadPool::cancel(const Task_Handle& task_handle) {
	return tasks_.erase(task_handle);
}
