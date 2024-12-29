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

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <folly/Function.h>
#include <folly/Range.h>
#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>

namespace folly {

/**
 * Schedules any number of functions to run at various intervals. E.g.,
 *  在指定的时间间隔运行任意数量的函数
 *
 *   FunctionScheduler fs;
 *
 *   fs.addFunction([&] { LOG(INFO) << "tick..."; }, seconds(1), "ticker");
 *   fs.addFunction(std::bind(&TestClass::doStuff, this), minutes(5), "stuff");
 *   fs.start();
 *   ........
 *   fs.cancelFunction("ticker");
 *   fs.addFunction([&] { LOG(INFO) << "tock..."; }, minutes(3), "tocker");
 *   ........
 *   fs.shutdown();
 *
 *
 * Note: the class uses only one thread - if you want to use more than one
 *       thread, either use multiple FunctionScheduler objects, or check out
 *       ThreadedRepeatingFunctionRunner.h for a much simpler contract of
 *       "run each function periodically in its own thread".
 *  注意：该类仅使用单个线程，若需使用多个线程可创建多个FunctionScheduler对象。
 *
 * start() schedules the functions, while shutdown() terminates further
 * scheduling (after any running function terminates).
 *  start()方法负责调度函数，shutdown()终止后续调度（运行函数完成后）。
 */
class FunctionScheduler {
 public:
  /* 构造函数*/
  FunctionScheduler();

  /**
   * On destruction, ensures that this instance is shutdown prior to deletion.
   *   析构时，确保实例已被shutdown，以防止在删除时发生未知行为。
   *
   * See `shutdown()`.
   */
  ~FunctionScheduler();

  /**
   * By default steady is false, meaning schedules may lag behind overtime.
   * This could be due to long running tasks or time drift because of randomness
   * in thread wakeup time.
   * By setting steady to true, FunctionScheduler will attempt to catch up.
   * i.e. more like a cronjob
   *   默认情况下，steady为false，意味着调度可能落后于实际情况。
   *  由于长时间运行的任务或线程唤醒时间的随机性，可能导致时间漂移，调度滞后。
   *  设置steady为true，FunctionScheduler将尝试追赶。
   *
   * NOTE: it's only safe to set this before calling start()
   */
  void setSteady(bool steady) { steady_ = steady; }

  /*
   * Parameters to control the function interval.
   * 控制函数间隔的参数
   *
   * If isPoisson is true, then use std::poisson_distribution to pick the
   * interval between each invocation of the function.
   *  若isPoisson为true，则使用std::poisson_distribution获取函数调用之间的间隔。
   *
   * If isPoisson is false, then always use the fixed interval specified to
   * addFunction().
   *  若isPoisson为false，则始终使用addFunction()指定的固定间隔。
   */
  struct LatencyDistribution {
    bool isPoisson;
    std::chrono::microseconds poissonMean;

    LatencyDistribution(bool poisson, std::chrono::microseconds mean)
        : isPoisson(poisson), poissonMean(mean) {}
  };

  /**
   * Adds a new function to the FunctionScheduler.
   *   添加新的函数到FunctionScheduler中。
   *
   * Functions will not be run until start() is called.  When start() is
   * called, each function will be run after its specified startDelay.
   * Functions may also be added after start() has been called, in which case
   * startDelay is still honored.
   *   函数不会被运行，直到调用start()。
   *   当调用start()时，每个函数将在其指定的startDelay后运行。
   *   也可以在调用start()后添加函数，此时startDelay仍然有效。
   *
   * Throws an exception on error.  In particular, each function must have a
   * unique name--two functions cannot be added with the same name.
   */
  void addFunction(
      Function<void()>&& cb,
      std::chrono::microseconds interval,
      StringPiece nameID = StringPiece(),
      std::chrono::microseconds startDelay = std::chrono::microseconds(0));

  /*
   * Add a new function to the FunctionScheduler with a specified
   * LatencyDistribution
   * 添加新的函数到FunctionScheduler中，并指定LatencyDistribution
   */
  void addFunction(
      Function<void()>&& cb,
      std::chrono::microseconds interval,
      const LatencyDistribution& latencyDistr,
      StringPiece nameID = StringPiece(),
      std::chrono::microseconds startDelay = std::chrono::microseconds(0));

  /**
   * Adds a new function to the FunctionScheduler to run only once.
   *   添加新的函数到FunctionScheduler中，该函数只运行一次。
   */
  void addFunctionOnce(
      Function<void()>&& cb,
      StringPiece nameID = StringPiece(),
      std::chrono::microseconds startDelay = std::chrono::microseconds(0));

  /**
   * Add a new function to the FunctionScheduler with the time
   * interval being distributed uniformly within the given interval
   * [minInterval, maxInterval].
   *   添加新的函数到FunctionScheduler中，其时间间隔在给定的[minInterval, maxInterval]区间内均匀分布。
   */
  void addFunctionUniformDistribution(
      Function<void()>&& cb,
      std::chrono::microseconds minInterval,
      std::chrono::microseconds maxInterval,
      StringPiece nameID,
      std::chrono::microseconds startDelay);

  /**
   * Add a new function to the FunctionScheduler whose start times are attempted
   * to be scheduled so that they are congruent modulo the interval.
   * Note: The scheduling of the next run time happens right before the function
   * invocation, so the first time a function takes more time than the interval,
   * it will be reinvoked immediately.
   *  添加新的函数到FunctionScheduler中，其启动时间被尝试调度，使得它们与间隔的余数相同。
   */
  void addFunctionConsistentDelay(
      Function<void()>&& cb,
      std::chrono::microseconds interval,
      StringPiece nameID = StringPiece(),
      std::chrono::microseconds startDelay = std::chrono::microseconds(0));

  /**
   * A type alias for function that is called to determine the time
   * interval for the next scheduled run.
   *   类型别名，用于确定下一次调度运行的时间间隔。
   */
  using IntervalDistributionFunc = Function<std::chrono::microseconds()>;
  /**
   * A type alias for function that returns the next run time, given the current
   * run time and the current start time.
   *   类型别名，用于获取下一次运行时间，给定当前运行时间和当前启动时间。
   */
  using NextRunTimeFunc = Function<std::chrono::steady_clock::time_point(
      std::chrono::steady_clock::time_point,
      std::chrono::steady_clock::time_point)>;

  /**
   * Add a new function to the FunctionScheduler. The scheduling interval
   * is determined by the interval distribution functor, which is called
   * every time the next function execution is scheduled. This allows
   * for supporting custom interval distribution algorithms in addition
   * to built in constant interval; and Poisson and jitter distributions
   * (@see FunctionScheduler::addFunction and
   * 添加新的函数到FunctionScheduler中。
   *  调度间隔由间隔分布函数决定，该函数在下一次函数执行调度时被调用。
   *  允许在内置常量间隔之外支持自定义间隔分布算法，以及Poisson和抖动分布。
   * @see FunctionScheduler::addFunctionJitterInterval).
   */
  void addFunctionGenericDistribution(
      Function<void()>&& cb,
      IntervalDistributionFunc&& intervalFunc,
      const std::string& nameID,
      const std::string& intervalDescr,
      std::chrono::microseconds startDelay);

  /**
   * Like addFunctionGenericDistribution, adds a new function to the
   * FunctionScheduler, but the next run time is determined directly by the
   * given functor, rather than by adding an interval.
   *   与addFunctionGenericDistribution类似，但下一次运行时间由给定的functor直接决定，而非通过添加间隔。
   */
  void addFunctionGenericNextRunTimeFunctor(
      Function<void()>&& cb,
      NextRunTimeFunc&& fn,
      const std::string& nameID,
      const std::string& intervalDescr,
      std::chrono::microseconds startDelay);

  /**
   * Cancels the function with the specified name, so it will no longer be run.
   *   取消具有指定名称的函数，使其不再运行。
   * Returns false if no function exists with the specified name.
   */
  bool cancelFunction(StringPiece nameID);
  bool cancelFunctionAndWait(StringPiece nameID);

  /**
   * All functions registered will be canceled.
   *   所有注册的函数都将被取消。
   */
  void cancelAllFunctions();
  void cancelAllFunctionsAndWait();

  /**
   * Resets the specified function's timer.
   * When resetFunctionTimer is called, the specified function's timer will
   * be reset with the same parameters it was passed initially, including
   * its startDelay. If the startDelay was 0, the function will be invoked
   * immediately.
   *   重置指定函数的定时器。
   *  当调用resetFunctionTimer时，将重置指定函数的定时器，其参数与最初传入的参数相同，包括其startDelay。
   *  若startDelay为0，则函数将立即调用。
   * Returns false if no function exists with the specified name.
   */
  bool resetFunctionTimer(StringPiece nameID);

  /**
   * Starts the scheduler.
   * 启动FunctionScheduler。
   * Returns false if the scheduler was already running.
   */
  bool start();

  /**
   * Stops the FunctionScheduler.
   * 停止FunctionScheduler。
   * This method blocks until any running function terminates. It is also called
   * automatically on FunctionScheduler destruction.
   * 该函数会阻塞，直到运行函数完成。
   * 此方法也会在FunctionScheduler析构时自动调用。
   * 
   * This FunctionScheduler may be restarted later by calling start() again.
   *
   * Returns false if the scheduler was not running (in which case this method
   * was a no-op and did not block on any function running). Returns true
   * otherwise.
   *
   * Thread-safe.
   */
  bool shutdown();

  /**
   * Set the name of the worker thread.
   * 设置工作线程的名称。
   */
  void setThreadName(StringPiece threadName);

 private:
  struct RepeatFunc {
    Function<void()> cb;
    NextRunTimeFunc nextRunTimeFunc;
    std::chrono::steady_clock::time_point nextRunTime;
    std::string name;
    std::chrono::microseconds startDelay;
    std::string intervalDescr;
    bool runOnce;

    RepeatFunc(
        Function<void()>&& cback,
        IntervalDistributionFunc&& intervalFn,
        const std::string& nameID,
        const std::string& intervalDistDescription,
        std::chrono::microseconds delay,
        bool once)
        : RepeatFunc(
              std::move(cback),
              getNextRunTimeFunc(std::move(intervalFn)),
              nameID,
              intervalDistDescription,
              delay,
              once) {}

    RepeatFunc(
        Function<void()>&& cback,
        NextRunTimeFunc&& nextRunTimeFn,
        const std::string& nameID,
        const std::string& intervalDistDescription,
        std::chrono::microseconds delay,
        bool once)
        : cb(std::move(cback)),
          nextRunTimeFunc(std::move(nextRunTimeFn)),
          nextRunTime(),
          name(nameID),
          startDelay(delay),
          intervalDescr(intervalDistDescription),
          runOnce(once) {}

    static NextRunTimeFunc getNextRunTimeFunc(
        IntervalDistributionFunc&& intervalFn) {
      return [intervalFn_2 = std::move(intervalFn)](
                 std::chrono::steady_clock::time_point /* curNextRunTime */,
                 std::chrono::steady_clock::time_point curTime) mutable {
        return curTime + intervalFn_2();
      };
    }

    std::chrono::steady_clock::time_point getNextRunTime() const {
      return nextRunTime;
    }
    void setNextRunTimeStrict(std::chrono::steady_clock::time_point curTime) {
      nextRunTime = nextRunTimeFunc(nextRunTime, curTime);
    }
    void setNextRunTimeSteady() {
      nextRunTime = nextRunTimeFunc(nextRunTime, nextRunTime);
    }
    void resetNextRunTime(std::chrono::steady_clock::time_point curTime) {
      nextRunTime = curTime + startDelay;
    }
    void cancel() {
      // Simply reset cb to an empty function.
      cb = {};
    }
    bool isValid() const { return bool(cb); }
  };

  struct RunTimeOrder {
    bool operator()(
        const std::unique_ptr<RepeatFunc>& f1,
        const std::unique_ptr<RepeatFunc>& f2) const {
      return f1->getNextRunTime() > f2->getNextRunTime();
    }
  };

  using FunctionHeap = std::vector<std::unique_ptr<RepeatFunc>>;
  using FunctionMap = folly::F14FastMap<StringPiece, RepeatFunc*, Hash>;

  void run();
  void runOneFunction(
      std::unique_lock<std::mutex>& lock,
      std::chrono::steady_clock::time_point now);
  void cancelFunction(const std::unique_lock<std::mutex>& lock, RepeatFunc* it);
  void addFunctionToHeap(
      const std::unique_lock<std::mutex>& lock,
      std::unique_ptr<RepeatFunc> func);

  template <typename RepeatFuncNextRunTimeFunc>
  void addFunctionToHeapChecked(
      Function<void()>&& cb,
      RepeatFuncNextRunTimeFunc&& fn,
      const std::string& nameID,
      const std::string& intervalDescr,
      std::chrono::microseconds startDelay,
      bool runOnce);

  void addFunctionInternal(
      Function<void()>&& cb,
      NextRunTimeFunc&& fn,
      const std::string& nameID,
      const std::string& intervalDescr,
      std::chrono::microseconds startDelay,
      bool runOnce);
  void addFunctionInternal(
      Function<void()>&& cb,
      IntervalDistributionFunc&& fn,
      const std::string& nameID,
      const std::string& intervalDescr,
      std::chrono::microseconds startDelay,
      bool runOnce);

  // Return true if the current function is being canceled
  bool cancelAllFunctionsWithLock(std::unique_lock<std::mutex>& lock);
  bool cancelFunctionWithLock(
      std::unique_lock<std::mutex>& lock, StringPiece nameID);

  std::thread thread_;

  // Mutex to protect our member variables.
  /*   互斥锁，用于保护成员变量 */
  std::mutex mutex_;
  bool running_{false};

  // The functions to run.
  // This is a heap, ordered by next run time.
  /* 堆结构：保存需运行的函数，以下次运行时间为顺序 */
  FunctionHeap functions_;
  FunctionMap functionsMap_;
  RunTimeOrder fnCmp_;

  // The function currently being invoked by the running thread.
  // This is null when the running thread is idle
  /* 当前正在运行的函数。若运行线程空闲则为nullptr */
  RepeatFunc* currentFunction_{nullptr};

  // Condition variable that is signalled whenever a new function is added
  // or when the FunctionScheduler is stopped.
  /* 条件变量，用于通知新增函数或停止FunctionScheduler */
  std::condition_variable runningCondvar_;

  std::string threadName_{"FuncSched"};
  bool steady_{false};
  bool cancellingCurrentFunction_{false};
};

} // namespace folly
