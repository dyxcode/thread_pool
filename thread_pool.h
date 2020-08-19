#pragma once

#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <vector>
#include <thread>
#include <future>
#include <memory>

class ThreadPool
{
	using Lockgd = std::lock_guard<std::mutex>;
	using Ulock = std::unique_lock<std::mutex>;

public:
	explicit ThreadPool(std::size_t thread_num)
	{
		restart(thread_num);
	}

	~ThreadPool()
	{
		if (is_start_)
			shutdown();
	}

	// 不支持拷贝语义（也不支持移动语义）
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// 等待所有线程关闭，并销毁所有线程
	void shutdown()
	{
		{ // 对于is_start的修改和工作线程对于is_start的读取要互斥
			Lockgd guard{mtx_};
			is_start_ = false;
		}
		cv_.notify_all();
		// 回收所有线程
		for (auto && item : threads_)
			if (item.joinable())
				item.join();
		threads_.clear();
	}

	// 重启一个线程池
	void restart(std::size_t thread_num = std::thread::hardware_concurrency())
	{
		// 启动时不需要加锁
		is_start_ = true;
		idle_threads_num_ = thread_num;

		// 添加指定数量的线程，保存在vector中
		threads_.reserve(thread_num);
		for (std::size_t i = 0; i < thread_num; ++i)
		{
			//事件循环，用lambda打包提交到thread中
			threads_.emplace_back(std::thread([this]{
				//循环外加锁，不是每次都对循环内的临界区加锁，而是对非临界区解锁
				Ulock u_guard{this->mtx_};
				while (1)
				{
					if (!this->tasks_.empty()) //任务队列非空，则领取任务
					{
						auto task{std::move(this->tasks_.front())};
						this->tasks_.pop();
						this->idle_threads_num_--;
						//执行任务时解锁
						u_guard.unlock();
						task();
						u_guard.lock();
						this->idle_threads_num_++;
					}
					else if (!this->is_start_) //线程池中止，线程退出
					{
						break;
					}
					else //等待任务队列出现任务，或者线程池中止
					{
						this->cv_.wait(u_guard);
					}
				}
			}));
		}
	}

	// 添加一个任务
	void execute(std::function<void()> task)
	{
		{
			Lockgd guard{mtx_};
			tasks_.emplace(std::move(task));
		}
		cv_.notify_one();
	}

	// 获取当前空闲线程数量
	size_t idleThreadNum()
	{
		std::lock_guard<std::mutex> guard{mtx_};
		return is_start_ ? idle_threads_num_ : 0;
	}		
	// 获取线程池中线程数量
	size_t totalThreadNum()
	{
		return threads_.size();
	}	
	// 获取当前阻塞的任务数量
	size_t blockTaskNum()
	{
		std::lock_guard<std::mutex> guard{mtx_};
		return tasks_.size();
	}		
	// 返回线程池是否正在执行
	bool isRunning()
	{
		std::lock_guard<std::mutex> guard{mtx_};
		return is_start_;
	}			

private:
	bool is_start_;
	std::size_t idle_threads_num_;
	std::mutex mtx_;
	std::condition_variable cv_;
	std::vector<std::thread> threads_;
	std::queue<std::function<void()>> tasks_;
};
