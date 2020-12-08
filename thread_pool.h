#pragma once

#include <map>
#include <list>
#include <vector>
#include <unordered_map>
#include <functional>
#include <thread>
#include <condition_variable>

namespace dyx {

namespace detail {

template<typename Clock>
class PeriodicTask {
  using Period = typename Clock::duration;
  using TimePoint = typename Clock::time_point;
public:
  template<typename T>
  PeriodicTask(T&& task, const Period& period, int times)
    : task_(std::forward<T>(task)), period_(period), times_(times) { }

  template<typename T, typename C>
  void operator()(T&& map_node, C& container, std::mutex& mtx) {
    if (--times_ > 0) {
      map_node.key() += period_;
      std::lock_guard<std::mutex> guard{mtx};
      container.insert(std::move(map_node));
    }
    if (times_ >= 0) task_();
  }
  TimePoint getEndTime(const TimePoint& execute_time) const {
    return execute_time + period_ * times_;
  }

  PeriodicTask(const PeriodicTask&) = delete;
  PeriodicTask(PeriodicTask&&) = default;

private:
  std::function<void()> task_;
  Period period_;
  int times_;
};

template<typename T>
class HashListQueue {
public:
  void put(const T& key) {
    iter_to_node_[key] = nodes_.emplace(nodes_.end(), key);
  }
  T get() const {
    return nodes_.front();
  }
  void remove(const T& key) {
    nodes_.erase(iter_to_node_[key]);
    iter_to_node_.erase(key);
  }
  bool empty() const {
    return nodes_.empty();
  }
private:
  std::unordered_map<T, typename std::list<T>::iterator> iter_to_node_;
  std::list<T> nodes_;
};

} // namespace detail

template<typename Clock>
class ThreadPool {
  using TimePoint = typename Clock::time_point;
  using Duration = typename Clock::duration;
  using PeriodicTask = detail::PeriodicTask<Clock>;
public:
  explicit ThreadPool(size_t thread_num)
    : stop_(false), waiting_for_delay_task_(thread_num), cvs_(thread_num) {
    while (thread_num--) {
      threads_.emplace_back([index = thread_num, this]{
        std::unique_lock<std::mutex> u_lock{mtx_};
        while (true) {
          if (stop_) break;
          if (tasks_.empty() || waiting_for_delay_task_ != threads_.size()) {
            scheduler_.put(index);
            cvs_[index].wait(u_lock);
            scheduler_.remove(index);
          } else {
            auto && execute_time = tasks_.begin()->first;
            if (execute_time <= Clock::now()) {
              auto map_node = tasks_.extract(tasks_.begin());
              bool still_has_task = !tasks_.empty();
              u_lock.unlock();
              if (still_has_task && !scheduler_.empty())
                cvs_[scheduler_.get()].notify_one();
              PeriodicTask& task = map_node.mapped();
              task(map_node, tasks_, mtx_);
              u_lock.lock();
            } else {
              scheduler_.put(index);
              waiting_for_delay_task_ = index;
              cvs_[index].wait_until(u_lock, execute_time);
              waiting_for_delay_task_ = threads_.size();
              scheduler_.remove(index);
            }
          }
        }
      });
    }
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> guard{mtx_};
      stop_ = true;
    }
    for (auto && item : cvs_)
      item.notify_one();
    for (auto && item : threads_)
      item.join();
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  template<typename F>
  auto execute(F&& task,
               const TimePoint& execute_time = Clock::now(),
               const Duration& period = Duration::zero(),
               int times = 1) {
    return addTask(execute_time,
                   PeriodicTask(std::forward<F>(task), period, times));
  }

  template<typename F>
  auto execute(F&& task,
               const Duration& delay,
               const Duration& period = Duration::zero(),
               int times = 1) {
    return addTask(Clock::now() + delay,
                   PeriodicTask(std::forward<F>(task), period, times));
  }

private:
  auto addTask(const TimePoint& execute_time, PeriodicTask&& task) {
    auto end_time = task.getEndTime(execute_time);
    size_t index;
    typename std::multimap<TimePoint, PeriodicTask>::iterator iter;
    {
      std::lock_guard<std::mutex> guard{mtx_};
      iter = tasks_.emplace(execute_time, std::move(task));
      if (scheduler_.empty())
        index = cvs_.size();
      else
        index = (waiting_for_delay_task_ == cvs_.size() ?
                 scheduler_.get() : waiting_for_delay_task_);
    }
    if (index != cvs_.size())
      cvs_[index].notify_one();
    return [callable = true, end_time, iter, this] () mutable {
      if (callable == false) return;
      std::unique_lock<std::mutex> u_lock{mtx_};
      if (end_time < Clock::now()) return;
      std::size_t index = waiting_for_delay_task_;
      bool need_notify = (iter == tasks_.begin()) && (index != cvs_.size());
      tasks_.erase(iter);
      u_lock.unlock();
      if (need_notify) cvs_[index].notify_one();
      callable = false;
    };
  }

private:
  bool stop_;
  size_t waiting_for_delay_task_;
  std::mutex mtx_;
  std::multimap<TimePoint, PeriodicTask> tasks_;
  std::vector<std::condition_variable> cvs_;
  std::vector<std::thread> threads_;
  detail::HashListQueue<size_t> scheduler_;
};

} // namespace dyx
