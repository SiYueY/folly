/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <folly/Executor.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/container/F14Map.h>
#include <folly/executors/thread_factory/ThreadFactory.h>

namespace folly {

/***
 *  ThreadedExecutor 线程执行器
 *
 *  An executor for blocking tasks. 阻塞任务的执行器
 *
 *  This executor runs each task in its own thread. It works well for tasks
 *  which mostly sleep, but works poorly for tasks which mostly compute.
 *  此执行器为每个任务创建一个线程。其适用于大部分休眠的任务，但不适用于大部分计算的任务。
 *
 *  For each task given to the executor with `add`, the executor spawns a new
 *  thread for that task, runs the task in that thread, and joins the thread
 *  after the task has completed.
 *  对于给定给执行器的每个任务，执行器为该任务创建一个新线程，在该线程中运行任务，并在任务完成后等待线程。
 *
 *  Spawning and joining task threads are done in the executor's internal
 *  control thread. Calls to `add` put the tasks to be run into a queue, where
 *  the control thread will find them.
 *  控制线程将任务放入队列中，然后控制线程将找到它们并启动它们。
 *
 *  There is currently no limitation on, or throttling of, concurrency.
 *  并发性的限制或限制尚未实现。
 *
 *  This executor is not currently optimized for performance. For example, it
 *  makes no attempt to re-use task threads. Rather, it exists primarily to
 *  offload sleep-heavy tasks from the CPU executor, where they might otherwise
 *  be run.
 *  此执行器当前没有针对性能进行优化。例如，它没有尝试重用任务线程。相反，它主要用于将休眠密集型任务卸载到CPU执行器，
 */
class ThreadedExecutor : public virtual folly::Executor {
 public:
  explicit ThreadedExecutor(
      std::shared_ptr<ThreadFactory> threadFactory = newDefaultThreadFactory());
  ~ThreadedExecutor() override;

  ThreadedExecutor(ThreadedExecutor const&) = delete;
  ThreadedExecutor(ThreadedExecutor&&) = delete;

  ThreadedExecutor& operator=(ThreadedExecutor const&) = delete;
  ThreadedExecutor& operator=(ThreadedExecutor&&) = delete;

  void add(Func func) override;

 private:
  // TODO(ott): Switch to std::variant when available everywhere.
  struct Message {
    enum class Type { Start, Join, StopControl };
    Type type;
    Func startFunc;
    std::thread::id joinTid;
  };

  static std::shared_ptr<ThreadFactory> newDefaultThreadFactory();

  void work(Func& func);
  void control();

  std::shared_ptr<ThreadFactory> threadFactory_;

  std::atomic<bool> stopping_{false};

  UMPSCQueue<Message, /* MayBlock */ true> controlMessages_;
  std::thread controlThread_;

  // Accessed only by the control thread, so no synchronization.
  F14FastMap<std::thread::id, std::thread> running_;
};

} // namespace folly
