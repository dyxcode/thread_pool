#include "thread_pool.h"

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true)
{
	while (thread_num--) {
		//事件循环，用lambda打包提交到thread中
		threads_.emplace_back(std::thread([this] {
			//循环外加锁，不是每次都对循环内的临界区加锁，而是对非临界区解锁
			Ulock u_guard{this->mtx_};
			while (true) {
				if (!this->tasks_.empty()) { //任务队列非空，则领取任务
					auto task{std::move(this->tasks_.front())};
					this->tasks_.pop();
					//执行任务时解锁
					u_guard.unlock();
					task();
					u_guard.lock();
				} else if (!this->is_start_ && !this->timer_thread_.joinable()) { //线程池中止，线程退出
					break;
				} else { //等待任务队列出现任务，或者线程池中止
					this->cv_.wait(u_guard);
				}
			}
		}));
	}
	// start a timer thread
	timer_thread_ = std::move(std::thread([this] {
		Ulock u_guard{this->timer_mtx_};
		while (true) {
			if (!this->timer_tasks_.empty()) {
				TimePoint now = std::chrono::steady_clock::now();
				if (now >= this->timer_tasks_.top().second) {
					this->execute(std::move(this->timer_tasks_.top().first));
					this->timer_tasks_.pop();
				} else
					this->timer_cv_.wait_until(u_guard, this->timer_tasks_.top().second);
			} else {
				{
					Lockgd guard{this->mtx_};
					if (!this->is_start_) break;
				}
				this->timer_cv_.wait(u_guard);
			}
		}
	}));
}

// 等待所有线程关闭，并销毁所有线程
void ThreadPool::wait() {
	{ // 对于is_start的修改和工作线程对于is_start的读取要互斥
		Lockgd guard{mtx_};
		if (!is_start_) return;
		is_start_ = false;
	}
	timer_cv_.notify_all();
	timer_thread_.join();

	cv_.notify_all();
	// 回收所有线程
	for (auto && item : threads_)
		item.join();
}

// 添加一个任务
inline void ThreadPool::execute(std::function<void()> task) {
	{
		Lockgd guard{mtx_};
		tasks_.push(std::move(task));
	}
	cv_.notify_one();
}

// 添加一个定时任务
inline void ThreadPool::execute(std::function<void()> task, std::size_t milliseconds) {
	TimePoint tp = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	{
		Lockgd guard{timer_mtx_};
		timer_tasks_.push({std::move(task), tp});
	}
	timer_cv_.notify_all();
}

ThreadPool::~ThreadPool() {
	wait();
}
