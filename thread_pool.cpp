#include "thread_pool.h"

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true)
{
	while (thread_num--) {
		//�¼�ѭ������lambda����ύ��thread��
		threads_.emplace_back(std::thread([this] {
			//ѭ�������������ÿ�ζ���ѭ���ڵ��ٽ������������ǶԷ��ٽ�������
			Ulock u_guard{this->mtx_};
			while (true) {
				if (!this->tasks_.empty()) { //������зǿգ�����ȡ����
					auto task{std::move(this->tasks_.front())};
					this->tasks_.pop();
					//ִ������ʱ����
					u_guard.unlock();
					task();
					u_guard.lock();
				} else if (!this->is_start_ && !this->timer_thread_.joinable()) { //�̳߳���ֹ���߳��˳�
					break;
				} else { //�ȴ�������г������񣬻����̳߳���ֹ
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

// �ȴ������̹߳رգ������������߳�
void ThreadPool::wait() {
	{ // ����is_start���޸ĺ͹����̶߳���is_start�Ķ�ȡҪ����
		Lockgd guard{mtx_};
		if (!is_start_) return;
		is_start_ = false;
	}
	timer_cv_.notify_all();
	timer_thread_.join();

	cv_.notify_all();
	// ���������߳�
	for (auto && item : threads_)
		item.join();
}

// ���һ������
inline void ThreadPool::execute(std::function<void()> task) {
	{
		Lockgd guard{mtx_};
		tasks_.push(std::move(task));
	}
	cv_.notify_one();
}

// ���һ����ʱ����
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
