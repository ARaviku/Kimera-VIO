/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   ThreadsafeQueue.h
 * @brief  Thread Safe Queue with shutdown/resume functionality.
 * @author Antoni Rosinol
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

#include <glog/logging.h>

#include "kimera-vio/utils/Macros.h"

namespace VIO {

template <typename T>
class ThreadsafeQueue {
 public:
  KIMERA_POINTER_TYPEDEFS(ThreadsafeQueue);
  KIMERA_DELETE_COPY_CONSTRUCTORS(ThreadsafeQueue);
  typedef std::queue<std::shared_ptr<T>> InternalQueue;
  ThreadsafeQueue(const std::string& queue_id);
  virtual ~ThreadsafeQueue() = default;

  /** \brief Push by value. Returns false if the queue has been shutdown.
   * Not optimal, since it will make two move operations.
   * But it does the job: see Item 41 Effective Modern C++
   *
   * Alternatives:
   *  Using both:
   *  - push(const T&)
   *  - with push(T&& ) rvalue to the queue using move semantics.
   * Since there is no type deduction, T&& is NOT a universal
   * reference (typename T is not at the level of the push function).
   * Problem: virtual keyword will make push(T&&) be discarded for
   * push(const T&), non-copyable things will compile-complain.
   *
   */
  virtual bool push(T new_value);

  /** \brief Pop value. Waits for data to be available in the queue.
   * Returns false if the queue has been shutdown.
   */
  // TODO(Toni): add a timer to avoid waiting forever...
  virtual bool popBlocking(T& value);

  /** \brief Pop value. Waits for data to be available in the queue.
   * If the queue has been shutdown, it returns a null shared_ptr.
   */
  virtual std::shared_ptr<T> popBlocking();

  /** \brief Pop without blocking, just checks once if the queue is empty.
   * Returns true if the value could be retrieved, false otherwise.
   */
  virtual bool pop(T& value);

  /** \brief Pop without blocking, just checks once if the queue is empty.
   * Returns a shared_ptr to the value retrieved.
   * If the queue is empty or has been shutdown,
   * it returns a null shared_ptr.
   */
  virtual std::shared_ptr<T> pop();

  /** \brief Swap queue with empty queue if not empty.
   * Returns true if values were retrieved.
   * Returns false if values were not retrieved.
   */
  virtual bool batchPop(InternalQueue* output_queue);

  void shutdown() {
    std::unique_lock<std::mutex> mlock(mutex_);
    // Even if the shared variable is atomic, it must be modified under the
    // mutex in order to correctly publish the modification to the waiting
    // threads.
    shutdown_ = true;
    mlock.unlock();
    data_cond_.notify_all();
  }

  void resume() {
    std::unique_lock<std::mutex> mlock(mutex_);
    // Even if the shared variable is atomic, it must be modified under the
    // mutex in order to correctly publish the modification to the waiting
    // threads.
    shutdown_ = false;
    mlock.unlock();
    data_cond_.notify_all();
  }

  /** \brief Checks if the queue is empty.
   * the state of the queue might change right after this query.
   */
  bool empty() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return data_queue_.empty();
  }

 public:
  std::string queue_id_;

 protected:
  mutable std::mutex mutex_;  //! mutable for empty() and copy-constructor.
  InternalQueue data_queue_;
  std::condition_variable data_cond_;
  std::atomic_bool shutdown_ = {false};  //! flag for signaling queue shutdown.
};

/**
 * @brief The ThreadsafeNullQueue class acts as a placeholder queue, but does
 * nothing. Useful for pipeline modules that do not require a queue.
 */
template <typename T>
class ThreadsafeNullQueue : public ThreadsafeQueue<T> {
 public:
  KIMERA_POINTER_TYPEDEFS(ThreadsafeNullQueue);
  KIMERA_DELETE_COPY_CONSTRUCTORS(ThreadsafeNullQueue);
  ThreadsafeNullQueue(const std::string& queue_id)
      : ThreadsafeQueue<T>(queue_id){};
  virtual ~ThreadsafeNullQueue() override = default;

  //! Do nothing
  // virtual bool push(const T& new_value) override { return true; }
  virtual bool push(T new_value) override { return true; }
  virtual bool popBlocking(T& value) override { return true; }
  virtual std::shared_ptr<T> popBlocking() override { return nullptr; }
  virtual bool pop(T& value) override { return true; }
  virtual std::shared_ptr<T> pop() override { return nullptr; }
};

template <typename T>
ThreadsafeQueue<T>::ThreadsafeQueue(const std::string& queue_id)
    : mutex_(),
      queue_id_(queue_id),
      data_queue_(),
      data_cond_(),
      shutdown_(false) {}

template <typename T>
bool ThreadsafeQueue<T>::push(T new_value) {
  if (shutdown_) return false;  // atomic, no lock needed.
  std::shared_ptr<T> data(std::make_shared<T>(std::move(new_value)));
  std::unique_lock<std::mutex> lk(mutex_);
  size_t queue_size = data_queue_.size();
  VLOG_IF(1, queue_size != 0) << "Queue with id: " << queue_id_
                              << " is getting full, size: " << queue_size;
  data_queue_.push(data);
  lk.unlock();  // Unlock before notify.
  data_cond_.notify_one();
  return true;
}

template <typename T>
bool ThreadsafeQueue<T>::popBlocking(T& value) {
  std::unique_lock<std::mutex> lk(mutex_);
  // Wait until there is data in the queue or shutdown requested.
  data_cond_.wait(lk, [this] { return !data_queue_.empty() || shutdown_; });
  // Return false in case shutdown is requested.
  if (shutdown_) return false;
  value = std::move(*data_queue_.front());
  data_queue_.pop();
  return true;
}

template <typename T>
std::shared_ptr<T> ThreadsafeQueue<T>::popBlocking() {
  std::unique_lock<std::mutex> lk(mutex_);
  data_cond_.wait(lk, [this] { return !data_queue_.empty() || shutdown_; });
  if (shutdown_) return std::shared_ptr<T>(nullptr);
  std::shared_ptr<T> result = data_queue_.front();
  data_queue_.pop();
  return result;
}

template <typename T>
bool ThreadsafeQueue<T>::pop(T& value) {
  if (shutdown_) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (data_queue_.empty()) return false;
  value = std::move(*data_queue_.front());
  data_queue_.pop();
  return true;
}

template <typename T>
std::shared_ptr<T> ThreadsafeQueue<T>::pop() {
  if (shutdown_) return std::shared_ptr<T>(nullptr);
  std::lock_guard<std::mutex> lk(mutex_);
  if (data_queue_.empty()) return std::shared_ptr<T>(nullptr);
  std::shared_ptr<T> result = data_queue_.front();
  data_queue_.pop();
  return result;
}

template <typename T>
bool ThreadsafeQueue<T>::batchPop(InternalQueue* output_queue) {
  if (shutdown_) return false;
  CHECK_NOTNULL(output_queue);
  CHECK(output_queue->empty());
  //*output_queue = InternalQueue();
  std::lock_guard<std::mutex> lk(mutex_);
  if (data_queue_.empty()) {
    return false;
  } else {
    data_queue_.swap(*output_queue);
    return true;
  }
}

}  // namespace VIO
