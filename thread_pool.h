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

	// ��֧�ֿ������壨Ҳ��֧���ƶ����壩
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// �ȴ������̹߳رգ������������߳�
	void shutdown()
	{
		{ // ����is_start���޸ĺ͹����̶߳���is_start�Ķ�ȡҪ����
			Lockgd guard{mtx_};
			is_start_ = false;
		}
		cv_.notify_all();
		// ���������߳�
		for (auto && item : threads_)
			if (item.joinable())
				item.join();
		threads_.clear();
	}

	// ����һ���̳߳�
	void restart(std::size_t thread_num = std::thread::hardware_concurrency())
	{
		// ����ʱ����Ҫ����
		is_start_ = true;
		idle_threads_num_ = thread_num;

		// ���ָ���������̣߳�������vector��
		threads_.reserve(thread_num);
		for (std::size_t i = 0; i < thread_num; ++i)
		{
			//�¼�ѭ������lambda����ύ��thread��
			threads_.emplace_back(std::thread([this]{
				//ѭ�������������ÿ�ζ���ѭ���ڵ��ٽ������������ǶԷ��ٽ�������
				Ulock u_guard{this->mtx_};
				while (1)
				{
					if (!this->tasks_.empty()) //������зǿգ�����ȡ����
					{
						auto task{std::move(this->tasks_.front())};
						this->tasks_.pop();
						this->idle_threads_num_--;
						//ִ������ʱ����
						u_guard.unlock();
						task();
						u_guard.lock();
						this->idle_threads_num_++;
					}
					else if (!this->is_start_) //�̳߳���ֹ���߳��˳�
					{
						break;
					}
					else //�ȴ�������г������񣬻����̳߳���ֹ
					{
						this->cv_.wait(u_guard);
					}
				}
			}));
		}
	}

	// ���һ������
	void execute(std::function<void()> task)
	{
		{
			Lockgd guard{mtx_};
			tasks_.emplace(std::move(task));
		}
		cv_.notify_one();
	}

	// ��ȡ��ǰ�����߳�����
	size_t idleThreadNum()
	{
		std::lock_guard<std::mutex> guard{mtx_};
		return is_start_ ? idle_threads_num_ : 0;
	}		
	// ��ȡ�̳߳����߳�����
	size_t totalThreadNum()
	{
		return threads_.size();
	}	
	// ��ȡ��ǰ��������������
	size_t blockTaskNum()
	{
		std::lock_guard<std::mutex> guard{mtx_};
		return tasks_.size();
	}		
	// �����̳߳��Ƿ�����ִ��
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
