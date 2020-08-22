#include "thread_pool.h"
#include <iostream>

std::atomic<std::uint64_t> ThreadPool::task_num_{0};

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true),
	is_wait_(false)
{
	while (thread_num--) {
		//�¼�ѭ������lambda����ύ��thread��
		threads_.emplace_back([this] {
			Ulock u_guard{this->mtx_};		//ѭ�������������ÿ�ζ���ѭ���ڵ��ٽ������������ǶԷ��ٽ�������
			while (true) {
				if (!this->is_start_) {		//�̳߳���ֹ���߳��˳�
					break;
				} else if (!this->tasks_.empty()) {		//������зǿ�
					auto&& execute_time = this->tasks_.begin()->first.base_info_.first;		// ��ȡ��һ�������ִ��ʱ��
					auto&& time_diff = std::visit(GetTimeDiff(), execute_time);				// ��ȡִ��ʱ���뵱ǰʱ���ʱ���
					if (time_diff.count() <= 0) {
						std::function<void()> task{std::move(this->tasks_.begin()->second)};
						// ������������񣬲���ʣ��ִ�д�����ֹһ��
						auto && period_info = this->tasks_.begin()->first.period_info_;
						if (period_info && period_info->second != 1) {
							// ��ȡ������
							TaskHandle task_handle{std::move(this->tasks_.begin()->first)};
							this->tasks_.erase(this->tasks_.begin());
							u_guard.unlock();
							// ���¾����Ϣ
							if (task_handle.period_info_->second)
								--task_handle.period_info_->second;
							std::visit(UpdatePeriodInfo(), task_handle.base_info_.first, task_handle.period_info_->first);
							// �����������
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

bool ThreadPool::cancel(const TaskHandle & task_handle) {
	Lockgd guard{mtx_};
	return tasks_.erase(task_handle);
}

std::uint64_t ThreadPool::getTaskIndex()
{
	return task_num_.fetch_add(1, std::memory_order_relaxed);
}
