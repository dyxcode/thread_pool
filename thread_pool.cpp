#include "thread_pool.h"
#include <iostream>

std::atomic<size_t> ThreadPool::task_num_{0};

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true),
	is_wait_(false)
{
	while (thread_num--) {
		//�¼�ѭ������lambda����ύ��thread��
		threads_.emplace_back([this] {
			//ѭ�������������ÿ�ζ���ѭ���ڵ��ٽ������������ǶԷ��ٽ�������
			Ulock u_guard{this->mtx_};
			while (true) {
				if (!this->tasks_.empty()) { //������зǿ�
					const TimePoint& now = std::chrono::steady_clock::now(); // ��ȡ��ǰʱ��
					const TimePoint& execute_time = std::get<0>(*(this->tasks_.cbegin())); //��ȡ��һ�������ִ��ʱ��
					if (now >= execute_time) {
						auto task{std::move(*(this->tasks_.begin()))};
						this->tasks_.erase(this->tasks_.begin());
						//ִ������ʱ����
						if (std::get<0>(task) == std::get<1>(task)) { // ���������������
							this->task_indexes_.erase(std::get<3>(task));
							u_guard.unlock();
						} else {
							u_guard.unlock();
							auto period = std::get<1>(task) - std::get<0>(task); //��ȡ����
							std::get<0>(task) = std::get<1>(task);               //������һ��ִ��ʱ��
							std::get<1>(task) = std::get<0>(task) + period;		 //�������´�ִ��ʱ��
							this->addTask<true>(task);
						}
						std::get<2>(task)();
						u_guard.lock();
					} else if (!this->is_wait_) {
						this->is_wait_ = true;
						this->cv_.wait_until(u_guard, execute_time);
						this->is_wait_ = false;
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
