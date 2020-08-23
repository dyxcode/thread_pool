#include "thread_pool.h"

std::atomic<std::uint64_t> ThreadPool::task_num_{0};

ThreadPool::ThreadPool(std::size_t thread_num) :
	is_start_(true),
	thread_waiting_for_delay_(thread_num),		// ��ֵ����thread_numʱ��ʾû���߳����ڵȴ���ʱ����
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
					if (std::visit(GetDuration(), execute_time).count() <= 0) {					// �������ִ��
						std::function<void()> task{std::move(this->tasks_.begin()->second)};
						auto && period_info = this->tasks_.begin()->first.data_->period_info_;
						// ������������񣬲���ʣ��ִ�д�����ֹһ��
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
						} else {	// �������ֱ��ɾ������
							auto && task_index = this->tasks_.begin()->first.data_->task_index_;
							this->tasks_indexes_.erase(task_index);
							this->tasks_.erase(this->tasks_.begin());
							u_guard.unlock();
						}
						task();
						u_guard.lock();
					} else if (this->thread_waiting_for_delay_ == this->cvs_.size()) {	// ���û���̵߳ȴ���ʱ����
						// ���캯����ӵȴ��̵߳����, ��������ɾ���ȴ��̵߳����
						ThreadPool::Scheduler::Guard index_guard{&this->scheduler_, thread_num};
						this->thread_waiting_for_delay_ = thread_num;
						this->cvs_[thread_num].wait_for(u_guard, std::visit(GetDuration(), execute_time));
						this->thread_waiting_for_delay_ = this->cvs_.size();
					} else {	// �����Ѿ����̵߳ȴ���ʱ������
						ThreadPool::Scheduler::Guard index_guard{&this->scheduler_, thread_num};
						this->cvs_[thread_num].wait(u_guard);
					}
				} else { //�ȴ�������г������񣬻����̳߳���ֹ
					ThreadPool::Scheduler::Guard index_guard{&this->scheduler_, thread_num};
					this->cvs_[thread_num].wait(u_guard);
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
	if (iter != tasks_indexes_.end()) {		// ����ҵ�������
		std::size_t notify_thread_index = thread_waiting_for_delay_;
		bool need_notify = (iter->second == tasks_.begin()) && (notify_thread_index != threads_.size());	// ��Ҫ����
		tasks_.erase(iter->second);
		tasks_indexes_.erase(iter);
		u_guard.unlock();
		if (need_notify)			// ���ѵȴ��߳�
			cvs_[notify_thread_index].notify_one();
		return true;
	}
	return false;
}

// ��ȡ�������
std::uint64_t ThreadPool::getTaskNumber() {
	return task_num_.fetch_add(1, std::memory_order_relaxed);
}

// ���캯��������ִ��ʱ�䣬�������
ThreadPool::TaskHandle::TaskHandle(const TimePoint& execute_time, std::uint64_t task_index) :
	data_(std::make_shared<Data>(execute_time, task_index)) { /* none */ }

// ���캯����������Ҫ������Ϣ
ThreadPool::TaskHandle::TaskHandle(const TimePoint& execute_time, std::uint64_t task_index, const TimePoint& period_time, std::size_t cycle_num) :
	data_(std::make_shared<Data>(execute_time, task_index, std::make_optional<PeriodInfo>(period_time, cycle_num))) { /* none */ }

// ���캯����ʵ�����ݵĹ���
ThreadPool::TaskHandle::Data::Data(const TimePoint& execute_time, std::uint64_t task_index, std::optional<PeriodInfo> period_info) :
	execute_time_(execute_time), task_index_(task_index), period_info_(std::move(period_info)) { /* none */ }

// ���ȴ��̵߳���ŷ����������
void ThreadPool::Scheduler::put(std::size_t index) {
	index_map_[index] = waiting_thread_indexes_.emplace(waiting_thread_indexes_.end(), index);
}

// �ӵ�������Ӧ�û��ѵ��߳�
std::size_t ThreadPool::Scheduler::get() {
	return waiting_thread_indexes_.front();
}

// ɾ�����
void ThreadPool::Scheduler::remove(std::size_t index) {
	waiting_thread_indexes_.erase(index_map_[index]);
	index_map_.erase(index);
}

// �жϵ�ǰ�Ƿ����߳����ڵȴ�
bool ThreadPool::Scheduler::empty() {
	return waiting_thread_indexes_.empty();
}
