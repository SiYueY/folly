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

#include <folly/SharedMutex.h>
#include <folly/ThreadLocal.h>
#include <folly/experimental/observer/detail/Core.h>
#include <folly/observer/Observer-pre.h>

namespace folly {
namespace observer {

/**
 * Observer - a library which lets you create objects which track updates of
 * their dependencies and get re-computed when any of the dependencies changes.
 *  观察者模型：创建具有依赖关系跟踪更新的对象，并在任何依赖项更改时重新计算。
 *
 * Given an Observer, you can get a snapshot of the current version of the
 * object it holds:
 * 对于给定的观察者，可以获取其当前版本对象快照：
 * 
 *   Observer<int> myObserver = ...;
 *   Snapshot<int> mySnapshot = myObserver.getSnapshot();
 * or simply
 *   Snapshot<int> mySnapshot = *myObserver;
 *
 * Snapshot will hold a view of the object, even if object in the Observer
 * gets updated.
 * 快照将保留对象的视图，即使观察者中的对象也会更新。
 *
 * What makes Observer powerful is its ability to track updates to other
 * Observers. Imagine we have two separate Observers A and B which hold
 * integers.
 *
 *   Observer<int> observerA = ...;
 *   Observer<int> observerB = ...;
 *
 * To compute a sum of A and B we can create a new Observer which would track
 * updates to A and B and re-compute the sum only when necessary.
 *
 *   Observer<int> sumObserver = makeObserver(
 *       [observerA, observerB] {
 *         int a = **observerA;
 *         int b = **observerB;
 *         return a + b;
 *       });
 *
 *   int sum = **sumObserver;
 *
 * Notice that a + b will be only called when either a or b is changed. Getting
 * a snapshot from sumObserver won't trigger any re-computation.
 *
 *  当多个线程同时获取Observer快照(涉及获取shared_ptr)时成本较高。
 *  若考虑到getSnapshot()的性能，可考虑使用其他实现，如AtomicObserver、TLObserver等。
 * Getting an Observer snapshot involves acquiring a shared_ptr, which can be
 * expensive, especially if several threads do so concurrently. If the cost of
 * getSnapshot() is noticeable, alternative Observer implementations are
 * available, offering different trade-offs:
 *
 * - If T is a type for which std::atomic<T> is lock-free (all word-sized PODs
 *   for example), AtomicObserver and ReadMostlyAtomicObserver offer the best
 *   performance at no additional memory cost.
 *  若std::atomic<T> 是无锁类型，则AtomicObserver和ReadMostlyAtomicObserver提供最佳性能，无额外内存开销。
 *
 * - TLObserver stores a thread-local snapshot, so that it can be accessed
 *   without synchronization (except when it needs updating). This however can
 *   consume significant amounts of memory by stranding old snapshots in threads
 *   that do not access, and thus refresh, the observer.
 *   TLObserver将线程本地快照存储在线程本地存储中，因此无需同步即可访问(除非需要更新)。
 *   但是线程不活跃的旧快照堆积可能导致内存消耗，并在线程不访问时刷新观察者。
 *
 * - HazptrObserver uses hazard pointers to protect the snapshot, which offer
 *   high read scalability and low cost, but the snapshot should be held as
 *   little as possible and should not cross coroutine suspension points.
 *   HazptrObserver使用Hazard指针保护快照，具有高读取扩展性和低成本，
 *   但快照应尽可能少地保留，并且不应跨协程暂停点。
 *
 * - ReadMostlyTLObserver returns a snapshot that can be used like a regular
 *   shared_ptr. Scalability and cost are comparable to HazptrObserver, but the
 *   snapshots can be held for arbitrary time. Memory cost is a small constant
 *   for each thread that acquires a snapshot.
 *   ReadMostlyTLObserver返回可以像常规shared_ptr一样使用的快照。
 *   扩展性和成本与HazptrObserver相当，但快照可以保留任意时间。
 *   内存成本为每个线程获取快照的常数级别。
 *
 * - CoreCachedObserver can be used if a std::shared_ptr<T> is strictly
 *   required. Read scalability is comparable to the previous options, but cost
 *   is moderately higher. Memory cost is a small constant for each CPU in the
 *   system.
 *   CoreCachedObserver可以用于严格需要std::shared_ptr<T>的场景。
 *   读取扩展性与之前的选项相当，但成本较高。
 *   内存成本为系统每个CPU的常数级别。
 *
 * See ObserverCreator class if you want to wrap any existing subscription API
 * in an Observer object.
 *  若要将现有订阅API封装为观察者对象，参阅ObserverCreator类。
 */
template <typename T>
class Observer;

/**
 * An AtomicObserver provides read-optimized caching for an Observer using
 * `std::atomic`. Reading only requires atomic loads unless the cached value
 * is stale. If the cache needs to be refreshed, a mutex is used to
 * synchronize the update. This avoids creating a shared_ptr for every read.
 *  AtomicObserver使用`std::atomic`对观察者进行read优化的缓存。
 *  Reading只需进行原子加载，除非缓存值已过期。
 * 若缓存需要刷新，则使用互斥锁进行同步更新，以避免为每个read创建shared_ptr。
 *
 * AtomicObserver models CopyConstructible and MoveConstructible. Copying or
 * moving simply invalidates the cache.
 * AtomicObserver模型支持拷贝构造和移动构造。拷贝或移动只会使缓存失效。
 * 
 * AtomicObserver is ideal when there are lots of reads on a trivially-copyable
 * type. if `std::atomic<T>` is not possible but you still want to optimize
 * reads, consider a TLObserver.
 * AtomicObserver适用于在trivially-copyable类型上有大量读取的场景。
 *   若`std::atomic<T>`不可行，但仍需优化读取，则考虑TLObserver。
 * 
 *   Observer<int> observer = ...;
 *   AtomicObserver<int> atomicObserver(observer);
 *   auto value = *atomicObserver;
 */
template <typename T>
class AtomicObserver;

/**
 * A TLObserver provides read-optimized caching for an Observer using
 * thread-local storage. This avoids creating a shared_ptr for every read.
 *
 * The functionality is similar to that of AtomicObserver except it allows types
 * that don't support atomics. If possible, use AtomicObserver instead.
 *
 * TLObserver can consume significant amounts of memory if accessed from many
 * threads. The problem is exacerbated if you chain several TLObservers.
 * Therefore, TLObserver should be used sparingly.
 *
 *   Observer<int> observer = ...;
 *   TLObserver<int> tlObserver(observer);
 *   auto& snapshot = *tlObserver;
 */
template <typename T>
class TLObserver;

/**
 * A ReadMostlyAtomicObserver guarantees that reading is exactly one relaxed
 * atomic load and a read from a thread local bool. Like AtomicObserver, the
 * value is cached using `std::atomic`.  However, there is no version check when
 * reading which means that the cached value may be out-of-date with the
 * Observer value. The cached value will be updated asynchronously in a
 * background thread.
 *
 * When get() is called from makeObserver, the underlying observer is directly
 * snapshotted to ensure dependent observers have current values and capture
 * dependencies.
 *
 * ReadMostlyAtomicObserver is ideal for fastest possible reads on a
 * trivially-copyable type when a slightly out-of-date value will suffice. It is
 * perfect for very frequent reads coupled with very infrequent writes.
 *
 *   Observer<int> observer = ...;
 *   ReadMostlyAtomicObserver<int> atomicObserver(observer);
 *   auto value = *atomicObserver;
 */
template <typename T>
class ReadMostlyAtomicObserver;

template <typename T>
class Snapshot {
 public:
  const T& operator*() const { return *get(); }

  const T* operator->() const { return get(); }

  const T* get() const { return data_.get(); }

  std::shared_ptr<const T> getShared() const& { return data_; }

  std::shared_ptr<const T> getShared() && { return std::move(data_); }

  /**
   * Return the version of the observed object.
   *  返回被观察对象的版本。
   */
  size_t getVersion() const { return version_; }

 private:
  friend class Observer<T>;

  /* 构造函数 */
  Snapshot(
      const observer_detail::Core& core,
      std::shared_ptr<const T> data,
      size_t version)
      : data_(std::move(data)), version_(version), core_(&core) {
    DCHECK(data_);
  }

  /* 数据*/
  std::shared_ptr<const T> data_;
  /* 版本*/
  size_t version_;
  const observer_detail::Core* core_;
};

/* 回调句柄 */
class CallbackHandle {
 public:
  /* 构造函数 */
  CallbackHandle();
  template <typename T>
  CallbackHandle(Observer<T> observer, Function<void(Snapshot<T>)> callback);
  /* 禁用拷贝构造函数 */
  CallbackHandle(const CallbackHandle&) = delete;
  /* 默认移动构造函数 */
  CallbackHandle(CallbackHandle&&) = default;
  /* 禁用拷贝赋值函数 */
  CallbackHandle& operator=(const CallbackHandle&) = delete;
  CallbackHandle& operator=(CallbackHandle&&) noexcept;
  /* 析构函数 */
  ~CallbackHandle();

  // If callback is currently running, waits until it completes.
  // Callback will never be called after cancel() returns.
  /* 若callback正在运行，则等待其完成.*
   * 若cancel()返回后，callback将不会被调用。*/
  void cancel();

 private:
  struct Context;
  std::shared_ptr<Context> context_;
};

template <typename Observable, typename Traits>
class ObserverCreator;

/* 观察者Observer */
template <typename T>
class Observer {
 public:
  /* 构造函数 */
  explicit Observer(observer_detail::Core::Ptr core);

  /* 获取快照 */
  Snapshot<T> getSnapshot() const;
  Snapshot<T> operator*() const { return getSnapshot(); }

  /**
   * Check if we have a newer version of the observed object than the snapshot.
   * Snapshot should have been originally from this Observer.
   *  检查是否存在比快照更新的被观察对象版本。
   */
  bool needRefresh(const Snapshot<T>& snapshot) const {
    DCHECK_EQ(core_.get(), snapshot.core_);
    return needRefresh(snapshot.getVersion());
  }

  /* 需要刷新 */
  bool needRefresh(size_t version) const {
    return version < core_->getVersionLastChange();
  }

  /**
   * Add a callback to be called when the Observer is updated. The callback
   * will be removed when the returned CallbackHandle is destroyed.
   */
  [[nodiscard]] CallbackHandle addCallback(
      Function<void(Snapshot<T>)> callback) const;

 private:
  template <typename Observable, typename Traits>
  friend class ObserverCreator;

  observer_detail::Core::Ptr core_;
};

template <typename T>
Observer<T> unwrap(Observer<T>);

template <typename T>
Observer<T> unwrapValue(Observer<T>);

template <typename T>
Observer<T> unwrap(Observer<Observer<T>>);

template <typename T>
Observer<T> unwrapValue(Observer<Observer<T>>);

/**
 * makeObserver(...) creates a new Observer<T> object given a functor to
 * compute it. The functor can return T or std::shared_ptr<const T>.
 *
 * makeObserver(...) blocks until the initial version of Observer is computed.
 * If creator functor fails (throws or returns a nullptr) during this first
 * call, the exception is re-thrown by makeObserver(...).
 *
 * For all subsequent updates if creator functor fails (throws or returs a
 * nullptr), the Observer (and all its dependents) is not updated.
 */
template <typename F>
Observer<observer_detail::ResultOf<F>> makeObserver(F&& creator);

template <typename F>
Observer<observer_detail::ResultOfUnwrapSharedPtr<F>> makeObserver(F&& creator);

template <typename F>
Observer<observer_detail::ResultOfUnwrapObserver<F>> makeObserver(F&& creator);

/**
 * The returned Observer will proxy updates from the input observer, but will
 * skip updates that contain the same (according to operator==) value even if
 * the actual object in the update is different.
 */
template <typename T>
Observer<T> makeValueObserver(Observer<T> observer);

/**
 * A more efficient short-cut for makeValueObserver(makeObserver(...)).
 *  更高效的makeValueObserver(makeObserver(...))的快捷方式。
 */
template <typename F>
Observer<observer_detail::ResultOf<F>> makeValueObserver(F&& creator);

template <typename F>
Observer<observer_detail::ResultOfUnwrapSharedPtr<F>> makeValueObserver(
    F&& creator);

/**
 * The returned Observer will never update and always return the passed value.
 */
template <typename T>
Observer<T> makeStaticObserver(T value);

template <typename T>
Observer<std::decay_t<T>> makeStaticObserver(std::shared_ptr<T> value);

/* AtomicObserver */
template <typename T>
class AtomicObserver {
 public:
  /* 构造函数 */
  explicit AtomicObserver(Observer<T> observer);
  AtomicObserver(const AtomicObserver<T>& other);
  AtomicObserver(AtomicObserver<T>&& other) noexcept;

  /* 重载赋值运算符 */
  AtomicObserver<T>& operator=(const AtomicObserver<T>& other);
  AtomicObserver<T>& operator=(AtomicObserver<T>&& other) noexcept;
  AtomicObserver<T>& operator=(Observer<T> observer);

  /* 获取AtomicObserver */
  T get() const;
  T operator*() const { return get(); }

  Observer<T> getUnderlyingObserver() const { return observer_; }

 private:
  mutable std::atomic<T> cachedValue_{};
  mutable std::atomic<size_t> cachedVersion_{};
  mutable SharedMutex refreshLock_;
  Observer<T> observer_;
};

template <typename T>
class TLObserver {
 public:
  explicit TLObserver(Observer<T> observer);
  TLObserver(const TLObserver<T>& other);
  TLObserver(TLObserver<T>&& other) noexcept;

  const Snapshot<T>& getSnapshotRef() const;
  const Snapshot<T>& operator*() const { return getSnapshotRef(); }

  Observer<T> getUnderlyingObserver() const { return observer_; }

 private:
  Observer<T> observer_;
  mutable ThreadLocalPtr<Snapshot<T>> snapshot_;
};

template <typename T>
class ReadMostlyAtomicObserver {
 public:
  explicit ReadMostlyAtomicObserver(Observer<T> observer);
  ReadMostlyAtomicObserver(const ReadMostlyAtomicObserver<T>&) = delete;
  ReadMostlyAtomicObserver<T>& operator=(const ReadMostlyAtomicObserver<T>&) =
      delete;

  T get() const;
  T operator*() const { return get(); }

  Observer<T> getUnderlyingObserver() const { return observer_; }

 private:
  Observer<T> observer_;
  std::atomic<T> cachedValue_{};
  CallbackHandle callback_;
};

/**
 * Same as makeObserver(...), but creates AtomicObserver.
 */
template <typename T>
AtomicObserver<T> makeAtomicObserver(Observer<T> observer) {
  return AtomicObserver<T>(std::move(observer));
}

template <typename F>
auto makeAtomicObserver(F&& creator) {
  return makeAtomicObserver(makeObserver(std::forward<F>(creator)));
}

/**
 * Same as makeObserver(...), but creates TLObserver.
 */
template <typename T>
TLObserver<T> makeTLObserver(Observer<T> observer) {
  return TLObserver<T>(std::move(observer));
}

template <typename F>
auto makeTLObserver(F&& creator) {
  return makeTLObserver(makeObserver(std::forward<F>(creator)));
}

/**
 * Same as makeObserver(...), but creates ReadMostlyAtomicObserver.
 */
template <typename T>
ReadMostlyAtomicObserver<T> makeReadMostlyAtomicObserver(Observer<T> observer) {
  return ReadMostlyAtomicObserver<T>(std::move(observer));
}

template <typename F>
auto makeReadMostlyAtomicObserver(F&& creator) {
  return makeReadMostlyAtomicObserver(makeObserver(std::forward<F>(creator)));
}

template <typename T, bool CacheInThreadLocal>
struct ObserverTraits {};

template <typename T>
struct ObserverTraits<T, false> {
  using type = Observer<T>;
};

template <typename T>
struct ObserverTraits<T, true> {
  using type = TLObserver<T>;
};

template <typename T, bool CacheInThreadLocal>
using ObserverT = typename ObserverTraits<T, CacheInThreadLocal>::type;
} // namespace observer
} // namespace folly

#include <folly/observer/Observer-inl.h>
