#include "thread_pool.h"

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true)
{
	while (thread_num--) {
		//�¼�ѭ������lambda����ύ��thread��
		threads_.emplace_back([this] {
			//ѭ�������������ÿ�ζ���ѭ���ڵ��ٽ������������ǶԷ��ٽ�������
			Ulock u_guard{this->mtx_};
			while (true) {
				if (!this->tasks_.empty()) { //������зǿ�
					CTimePoint& now = std::chrono::steady_clock::now();
					CTimePoint& execute_time = std::get<0>(this->tasks_.cbegin()->first);
					if (now >= execute_time) {
						auto task{std::move(*(this->tasks_.begin()))};
						this->tasks_.erase(this->tasks_.begin());
						//ִ������ʱ����
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
				} else if (!this->is_start_) { //�̳߳���ֹ���߳��˳�
					break;
				} else { //�ȴ�������г������񣬻����̳߳���ֹ
					this->cv_.wait(u_guard);
				}
			}
		});
	}
}

ThreadPool::~ThreadPool() {
	{ // ����is_start���޸ĺ͹����̶߳���is_start�Ķ�ȡҪ����
		Lockgd guard{mtx_};
		is_start_ = false;
	}
	cv_.notify_all();
	// ���������߳�
	for (auto && item : threads_)
		item.join();
}

bool ThreadPool::cancel(const Task_Handle& task_handle) {
	return tasks_.erase(task_handle);
}
