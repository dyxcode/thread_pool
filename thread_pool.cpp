#include "thread_pool.h"
#include <iostream>

std::atomic<std::uint64_t> ThreadPool::task_num_{0};

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true),
	thread_waiting_for_delay_(thread_num),
	cvs_(thread_num)
{
	while (thread_num--) {
		//�¼�ѭ������lambda����ύ��thread��
		threads_.emplace_back([this, thread_num] {
			Ulock u_guard{this->mtx_};		//ѭ�������������ÿ�ζ���ѭ���ڵ��ٽ������������ǶԷ��ٽ�������
			while (true) {
				if (!this->is_start_) {		//�̳߳���ֹ���߳��˳�
					break;
				} else if (!this->tasks_.empty()) {		//������зǿ�
					auto&& execute_time = this->tasks_.begin()->first.data_->execute_time_;		// ��ȡ��һ�������ִ��ʱ��
					if (std::visit(GetDuration(), execute_time).count() <= 0) {
						std::function<void()> task{std::move(this->tasks_.begin()->second)};
						// ������������񣬲���ʣ��ִ�д�����ֹһ��
						auto && period_info = this->tasks_.begin()->first.data_->period_info_;
						if (period_info && period_info->second != 1) {
							// ��ȡ������
							TaskHandle task_handle{std::move(this->tasks_.begin()->first)};
							this->tasks_.erase(this->tasks_.begin());
							u_guard.unlock();
							// ���¾����Ϣ
							if (task_handle.data_->period_info_->second)
								--task_handle.data_->period_info_->second;
							auto && execute_time = task_handle.data_->execute_time_;
							auto && period_time = task_handle.data_->period_info_->first;
							auto && period = std::visit(GetDuration(), period_time) - std::visit(GetDuration(), execute_time);
							execute_time = period_time;
							period_time = this->addDuration(execute_time, period);
							// �����������
							this->addTask(task_handle, task);
						} else {
							auto && task_index = this->tasks_.begin()->first.data_->task_index_;
							this->tasks_indexes_.erase(task_index);
							this->tasks_.erase(this->tasks_.begin());
							u_guard.unlock();
						}
						task();
						u_guard.lock();
					} else if (this->thread_waiting_for_delay_ == this->cvs_.size()) {
						this->thread_waiting_for_delay_ = thread_num;
						this->scheduler_.put(thread_num);
						this->cvs_[thread_num].wait_for(u_guard, std::visit(GetDuration(), execute_time));
						this->scheduler_.remove(thread_num);
						this->thread_waiting_for_delay_ = this->cvs_.size();

					} else {
						this->scheduler_.put(thread_num);
						this->cvs_[thread_num].wait(u_guard);
						this->scheduler_.remove(thread_num);
					}
				} else { //�ȴ�������г������񣬻����̳߳���ֹ
					this->scheduler_.put(thread_num);
					this->cvs_[thread_num].wait(u_guard);
					this->scheduler_.remove(thread_num);
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
	for (auto && item : cvs_)
		item.notify_one();
	for (auto && item : threads_)
		item.join();
}

bool ThreadPool::cancel(const TaskHandle & task_handle) {
	Ulock u_guard{mtx_};
	auto && iter = tasks_indexes_.find(task_handle.data_->task_index_);
	if (iter != tasks_indexes_.end()) {
		std::size_t notify_thread = thread_waiting_for_delay_;
		bool need_notify = (iter->second == tasks_.begin()) && (notify_thread != threads_.size());
		tasks_.erase(iter->second);
		tasks_indexes_.erase(iter);
		u_guard.unlock();
		if (need_notify)
			cvs_[notify_thread].notify_one();
		return true;
	}
	return false;
}

std::uint64_t ThreadPool::getTaskNumber() {
	return task_num_.fetch_add(1, std::memory_order_relaxed);
}

void ThreadPool::Scheduler::put(std::size_t index)
{
	index_map_[index] = waiting_thread_indexes_.emplace(waiting_thread_indexes_.end(), index);
}

std::size_t ThreadPool::Scheduler::get()
{
	return waiting_thread_indexes_.front();
}

void ThreadPool::Scheduler::remove(std::size_t index) {
	waiting_thread_indexes_.erase(index_map_[index]);
	index_map_.erase(index);
}

bool ThreadPool::Scheduler::empty() {
	return waiting_thread_indexes_.empty();
}
