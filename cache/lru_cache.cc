//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "cache/lru_cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "rocksdb/terark_namespace.h"

namespace TERARKDB_NAMESPACE {

LRUHandleTable::LRUHandleTable() : list_(nullptr), length_(0), elems_(0) {
  Resize();
}

LRUHandleTable::~LRUHandleTable() {
  ApplyToAllCacheEntries([](LRUHandle* h) {
    if (h->refs == 1) {
      h->Free();
    }
  });
  delete[] list_;
}

LRUHandle* LRUHandleTable::Lookup(const Slice& key, uint32_t hash) {
  return *FindPointer(key, hash);
}

LRUHandle* LRUHandleTable::Insert(LRUHandle* h) {
  LRUHandle** ptr = FindPointer(h->key(), h->hash);
  LRUHandle* old = *ptr;
  h->next_hash = (old == nullptr ? nullptr : old->next_hash);
  *ptr = h;
  if (old == nullptr) {
    ++elems_;
    if (elems_ > length_) {
      // Since each cache entry is fairly large, we aim for a small
      // average linked list length (<= 1).
      Resize();
    }
  }
  return old;
}

LRUHandle* LRUHandleTable::Remove(const Slice& key, uint32_t hash) {
  LRUHandle** ptr = FindPointer(key, hash);
  LRUHandle* result = *ptr;
  if (result != nullptr) {
    *ptr = result->next_hash;
    --elems_;
  }
  return result;
}

LRUHandle** LRUHandleTable::FindPointer(const Slice& key, uint32_t hash) {
  LRUHandle** ptr = &list_[hash & (length_ - 1)];
  while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
    ptr = &(*ptr)->next_hash;
  }
  return ptr;
}

void LRUHandleTable::Resize() {
  uint32_t new_length = 16;
  while (new_length < elems_ * 1.5) {
    new_length *= 2;
  }
  LRUHandle** new_list = new LRUHandle*[new_length];
  memset(new_list, 0, sizeof(new_list[0]) * new_length);
  uint32_t count = 0;
  for (uint32_t i = 0; i < length_; i++) {
    LRUHandle* h = list_[i];
    while (h != nullptr) {
      LRUHandle* next = h->next_hash;
      uint32_t hash = h->hash;
      LRUHandle** ptr = &new_list[hash & (new_length - 1)];
      h->next_hash = *ptr;
      *ptr = h;
      h = next;
      count++;
    }
  }
  assert(elems_ == count);
  delete[] list_;
  list_ = new_list;
  length_ = new_length;
}

template <class CacheMonitor>
LRUCacheShardTemplate<CacheMonitor>::LRUCacheShardTemplate(
    size_t capacity, bool strict_capacity_limit, double high_pri_pool_ratio,
    const typename CacheMonitor::Options& options)
    : CacheMonitor(options),
      capacity_(0),
      strict_capacity_limit_(strict_capacity_limit),
      high_pri_pool_ratio_(high_pri_pool_ratio),
      high_pri_pool_capacity_(0) {
  // Make empty circular linked list
  lru_.next = &lru_;
  lru_.prev = &lru_;
  lru_low_pri_ = &lru_;
  SetCapacity(capacity);
}

template <class CacheMonitor>
LRUCacheShardTemplate<CacheMonitor>::~LRUCacheShardTemplate() {}

template <class CacheMonitor>
bool LRUCacheShardTemplate<CacheMonitor>::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  return e->refs == 0;
}

// Call deleter and free

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::EraseUnRefEntries() {
  autovector<LRUHandle*> last_reference_list;
  {
    MutexLock l(&mutex_);
    while (lru_.next != &lru_) {
      LRUHandle* old = lru_.next;
      assert(old->InCache());
      assert(old->refs ==
             1);  // LRU list contains elements which may be evicted
      LRU_Remove(old);
      table_.Remove(old->key(), old->hash);
      old->SetInCache(false);
      Unref(old);
      UsageSub(old);
      last_reference_list.push_back(old);
    }
  }

  for (auto entry : last_reference_list) {
    entry->Free();
  }
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::ApplyToAllCacheEntries(
    void (*callback)(void*, size_t), bool thread_safe) {
  if (thread_safe) {
    mutex_.Lock();
  }
  table_.ApplyToAllCacheEntries(
      [callback](LRUHandle* h) { callback(h->value, h->charge); });
  if (thread_safe) {
    mutex_.Unlock();
  }
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::TEST_GetLRUList(
    LRUHandle** lru, LRUHandle** lru_low_pri) {
  *lru = &lru_;
  *lru_low_pri = lru_low_pri_;
}

template <class CacheMonitor>
size_t LRUCacheShardTemplate<CacheMonitor>::TEST_GetLRUSize() {
  LRUHandle* lru_handle = lru_.next;
  size_t lru_size = 0;
  while (lru_handle != &lru_) {
    lru_size++;
    lru_handle = lru_handle->next;
  }
  return lru_size;
}

template <class CacheMonitor>
double LRUCacheShardTemplate<CacheMonitor>::GetHighPriPoolRatio() {
  MutexLock l(&mutex_);
  return high_pri_pool_ratio_;
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::LRU_Remove(LRUHandle* e) {
  assert(e->next != nullptr);
  assert(e->prev != nullptr);
  if (lru_low_pri_ == e) {
    lru_low_pri_ = e->prev;
  }
  e->next->prev = e->prev;
  e->prev->next = e->next;
  e->prev = e->next = nullptr;
  LRUUsageSub(e);
  if (e->InHighPriPool()) {
    assert(high_pri_pool_usage_ >= e->charge);
    HighPriPoolUsageSub(e);
  }
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::LRU_Insert(LRUHandle* e) {
  assert(e->next == nullptr);
  assert(e->prev == nullptr);
  if (high_pri_pool_ratio_ > 0 && (e->IsHighPri() || e->HasHit())) {
    // Inset "e" to head of LRU list.
    e->next = &lru_;
    e->prev = lru_.prev;
    e->prev->next = e;
    e->next->prev = e;
    e->SetInHighPriPool(true);
    HighPriPoolUsageAdd(e);
    MaintainPoolSize();
  } else {
    // Insert "e" to the head of low-pri pool. Note that when
    // high_pri_pool_ratio is 0, head of low-pri pool is also head of LRU list.
    e->next = lru_low_pri_->next;
    e->prev = lru_low_pri_;
    e->prev->next = e;
    e->next->prev = e;
    e->SetInHighPriPool(false);
    lru_low_pri_ = e;
  }
  LRUUsageAdd(e);
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::MaintainPoolSize() {
  while (high_pri_pool_usage_ > high_pri_pool_capacity_) {
    // Overflow last entry in high-pri pool to low-pri pool.
    lru_low_pri_ = lru_low_pri_->next;
    assert(lru_low_pri_ != &lru_);
    lru_low_pri_->SetInHighPriPool(false);
    HighPriPoolUsageSub(lru_low_pri_);
  }
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::EvictFromLRU(
    size_t charge, autovector<LRUHandle*>* deleted) {
  while (usage_ + charge > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    assert(old->InCache());
    assert(old->refs == 1);  // LRU list contains elements which may be evicted
    LRU_Remove(old);
    table_.Remove(old->key(), old->hash);
    old->SetInCache(false);
    Unref(old);
    UsageSub(old);
    deleted->push_back(old);
  }
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::SetCapacity(size_t capacity) {
  autovector<LRUHandle*> last_reference_list;
  {
    MutexLock l(&mutex_);
    capacity_ = capacity;
    high_pri_pool_capacity_ = capacity_ * high_pri_pool_ratio_;
    EvictFromLRU(0, &last_reference_list);
  }
  // we free the entries here outside of mutex for
  // performance reasons
  for (auto entry : last_reference_list) {
    entry->Free();
  }
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::SetStrictCapacityLimit(
    bool strict_capacity_limit) {
  MutexLock l(&mutex_);
  strict_capacity_limit_ = strict_capacity_limit;
}

template <class CacheMonitor>
Cache::Handle* LRUCacheShardTemplate<CacheMonitor>::Lookup(const Slice& key,
                                                           uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    assert(e->InCache());
    if (e->refs == 1) {
      LRU_Remove(e);
    }
    e->refs++;
    e->SetHit();
  }
  return reinterpret_cast<Cache::Handle*>(e);
}

template <class CacheMonitor>
bool LRUCacheShardTemplate<CacheMonitor>::Ref(Cache::Handle* h) {
  LRUHandle* handle = reinterpret_cast<LRUHandle*>(h);
  MutexLock l(&mutex_);
  if (handle->InCache() && handle->refs == 1) {
    LRU_Remove(handle);
  }
  handle->refs++;
  return true;
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::SetHighPriorityPoolRatio(
    double high_pri_pool_ratio) {
  MutexLock l(&mutex_);
  high_pri_pool_ratio_ = high_pri_pool_ratio;
  high_pri_pool_capacity_ = capacity_ * high_pri_pool_ratio_;
  MaintainPoolSize();
}

template <class CacheMonitor>
bool LRUCacheShardTemplate<CacheMonitor>::Release(Cache::Handle* handle,
                                                  bool force_erase) {
  if (handle == nullptr) {
    return false;
  }
  LRUHandle* e = reinterpret_cast<LRUHandle*>(handle);
  bool last_reference = false;
  {
    MutexLock l(&mutex_);
    last_reference = Unref(e);
    if (last_reference) {
      UsageSub(e);
    }
    if (e->refs == 1 && e->InCache()) {
      // The item is still in cache, and nobody else holds a reference to it
      if (usage_ > capacity_ || force_erase) {
        // the cache is full
        // The LRU list must be empty since the cache is full
        assert(!(usage_ > capacity_) || lru_.next == &lru_);
        // take this opportunity and remove the item
        table_.Remove(e->key(), e->hash);
        e->SetInCache(false);
        Unref(e);
        UsageSub(e);
        last_reference = true;
      } else {
        // put the item on the list to be potentially freed
        LRU_Insert(e);
      }
    }
  }

  // free outside of mutex
  if (last_reference) {
    e->Free();
  }
  return last_reference;
}

template <class CacheMonitor>
Status LRUCacheShardTemplate<CacheMonitor>::Insert(
    const Slice& key, uint32_t hash, void* value, size_t charge,
    void (*deleter)(const Slice& key, void* value), Cache::Handle** handle,
    Cache::Priority priority) {
  // Allocate the memory here outside of the mutex
  // If the cache is full, we'll have to release it
  // It shouldn't happen very often though.
  LRUHandle* e = reinterpret_cast<LRUHandle*>(
      new char[sizeof(LRUHandle) - 1 + key.size()]);
  Status s;
  autovector<LRUHandle*> last_reference_list;

  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->flags = 0;
  e->hash = hash;
  e->refs = (handle == nullptr
                 ? 1
                 : 2);  // One from LRUCache, one for the returned handle
  e->next = e->prev = nullptr;
  e->SetInCache(true);
  e->SetPriority(priority);
  memcpy(e->key_data, key.data(), key.size());

  {
    MutexLock l(&mutex_);

    // Free the space following strict LRU policy until enough space
    // is freed or the lru list is empty
    EvictFromLRU(charge, &last_reference_list);

    if (usage_ - lru_usage_ + charge > capacity_ &&
        (strict_capacity_limit_ || handle == nullptr)) {
      if (handle == nullptr) {
        // Don't insert the entry but still return ok, as if the entry inserted
        // into cache and get evicted immediately.
        last_reference_list.push_back(e);
      } else {
        delete[] reinterpret_cast<char*>(e);
        *handle = nullptr;
        s = Status::Incomplete("Insert failed due to LRU cache being full.");
      }
    } else {
      // insert into the cache
      // note that the cache might get larger than its capacity if not enough
      // space was freed
      LRUHandle* old = table_.Insert(e);
      UsageAdd(e);
      if (old != nullptr) {
        old->SetInCache(false);
        if (Unref(old)) {
          UsageSub(old);
          // old is on LRU because it's in cache and its reference count
          // was just 1 (Unref returned 0)
          LRU_Remove(old);
          last_reference_list.push_back(old);
        }
      }
      if (handle == nullptr) {
        LRU_Insert(e);
      } else {
        *handle = reinterpret_cast<Cache::Handle*>(e);
      }
      s = Status::OK();
    }
  }

  // we free the entries here outside of mutex for
  // performance reasons
  for (auto entry : last_reference_list) {
    entry->Free();
  }

  return s;
}

template <class CacheMonitor>
void LRUCacheShardTemplate<CacheMonitor>::Erase(const Slice& key,
                                                uint32_t hash) {
  LRUHandle* e;
  bool last_reference = false;
  {
    MutexLock l(&mutex_);
    e = table_.Remove(key, hash);
    if (e != nullptr) {
      last_reference = Unref(e);
      if (last_reference && e->InCache()) {
        LRU_Remove(e);
      }
      if (last_reference) {
        UsageSub(e);
      }
      e->SetInCache(false);
    }
  }

  // mutex not held here
  // last_reference will only be true if e != nullptr
  if (last_reference) {
    e->Free();
  }
}

template <class CacheMonitor>
size_t LRUCacheShardTemplate<CacheMonitor>::GetUsage() const {
  MutexLock l(&mutex_);
  return usage_;
}

template <class CacheMonitor>
size_t LRUCacheShardTemplate<CacheMonitor>::GetPinnedUsage() const {
  MutexLock l(&mutex_);
  assert(usage_ >= lru_usage_);
  return usage_ - lru_usage_;
}

template <class CacheMonitor>
std::string LRUCacheShardTemplate<CacheMonitor>::GetPrintableOptions() const {
  const int kBufferSize = 200;
  char buffer[kBufferSize];
  {
    MutexLock l(&mutex_);
    snprintf(buffer, kBufferSize, "    high_pri_pool_ratio: %.3lf\n",
             high_pri_pool_ratio_);
  }
  return std::string(buffer);
}

template <>
LRUCacheBase<LRUCacheDiagnosableShard>::LRUCacheBase(
    size_t capacity, int num_shard_bits, bool strict_capacity_limit,
    double high_pri_pool_ratio,
    const typename LRUCacheDiagnosableShard::MonitorOptions& options,
    std::shared_ptr<MemoryAllocator> allocator)
    : ShardedCache(capacity, num_shard_bits, strict_capacity_limit,
                   std::move(allocator)) {
  num_shards_ = 1 << num_shard_bits;
  shards_ =
      reinterpret_cast<LRUCacheDiagnosableShard*>(port::cacheline_aligned_alloc(
          sizeof(LRUCacheDiagnosableShard) * num_shards_));
  size_t per_shard = (capacity + (num_shards_ - 1)) / num_shards_;
  for (int i = 0; i < num_shards_; i++) {
    new (&shards_[i]) LRUCacheDiagnosableShard(per_shard, strict_capacity_limit,
                                               high_pri_pool_ratio, options);
  }
}

template <class LRUCacheShardType>
LRUCacheBase<LRUCacheShardType>::LRUCacheBase(
    size_t capacity, int num_shard_bits, bool strict_capacity_limit,
    double high_pri_pool_ratio,
    const typename LRUCacheShardType::MonitorOptions& options,
    std::shared_ptr<MemoryAllocator> allocator)
    : ShardedCache(capacity, num_shard_bits, strict_capacity_limit,
                   std::move(allocator)) {
  num_shards_ = 1 << num_shard_bits;
  shards_ = reinterpret_cast<LRUCacheShardType*>(
      port::cacheline_aligned_alloc(sizeof(LRUCacheShardType) * num_shards_));
  size_t per_shard = (capacity + (num_shards_ - 1)) / num_shards_;
  for (int i = 0; i < num_shards_; i++) {
    new (&shards_[i]) LRUCacheShardType(per_shard, strict_capacity_limit,
                                        high_pri_pool_ratio, options);
  }
}

template <class LRUCacheShardType>
std::string LRUCacheBase<LRUCacheShardType>::DumpLRUCacheStatistics() {
  std::string res;
  res.append("Cache Summary: \n");
  res.append("usage: " + std::to_string(GetUsage()) +
             ", pinned_usage: " + std::to_string(GetPinnedUsage()) + "\n");

  for (int i = 0; i < num_shards_; i++) {
    res.append("shard_" + std::to_string(i) + " : \n");
    res.append(shards_[i].DumpDiagnoseInfo());
  }
  return res;
}

#ifdef WITH_DIAGNOSE_CACHE
template <>
const char* LRUCacheBase<LRUCacheDiagnosableShard>::Name() const {
  return "DiagnosableLRUCache";
}
#endif

template <class LRUCacheShardType>
const char* LRUCacheBase<LRUCacheShardType>::Name() const {
  return "LRUCache";
}

template <class LRUCacheShardType>
LRUCacheBase<LRUCacheShardType>::~LRUCacheBase() {
  if (shards_ != nullptr) {
    assert(num_shards_ > 0);
    for (int i = 0; i < num_shards_; i++) {
      shards_[i].~LRUCacheShardType();
    }
    port::cacheline_aligned_free(shards_);
  }
}

template <class LRUCacheShardType>
CacheShard* LRUCacheBase<LRUCacheShardType>::GetShard(int shard) {
  return reinterpret_cast<CacheShard*>(&shards_[shard]);
}

template <class LRUCacheShardType>
const CacheShard* LRUCacheBase<LRUCacheShardType>::GetShard(int shard) const {
  return reinterpret_cast<CacheShard*>(&shards_[shard]);
}

template <class LRUCacheShardType>
void* LRUCacheBase<LRUCacheShardType>::Value(Handle* handle) {
  return reinterpret_cast<const LRUHandle*>(handle)->value;
}

template <class LRUCacheShardType>
size_t LRUCacheBase<LRUCacheShardType>::GetCharge(Handle* handle) const {
  return reinterpret_cast<const LRUHandle*>(handle)->charge;
}

template <class LRUCacheShardType>
uint32_t LRUCacheBase<LRUCacheShardType>::GetHash(Handle* handle) const {
  return reinterpret_cast<const LRUHandle*>(handle)->hash;
}

template <class LRUCacheShardType>
void LRUCacheBase<LRUCacheShardType>::DisownData() {
// Do not drop data if compile with ASAN to suppress leak warning.
#if defined(__clang__)
#if !defined(__has_feature) || !__has_feature(address_sanitizer)
  shards_ = nullptr;
  num_shards_ = 0;
#endif
#else  // __clang__
#ifndef __SANITIZE_ADDRESS__
  shards_ = nullptr;
  num_shards_ = 0;
#endif  // !__SANITIZE_ADDRESS__
#endif  // __clang__
}

template <class LRUCacheShardType>
size_t LRUCacheBase<LRUCacheShardType>::TEST_GetLRUSize() {
  size_t lru_size_of_all_shards = 0;
  for (int i = 0; i < num_shards_; i++) {
    lru_size_of_all_shards += shards_[i].TEST_GetLRUSize();
  }
  return lru_size_of_all_shards;
}

// template <class LRUCacheShardType>
// double LRUCacheBase<LRUCacheShardType>::GetHighPriPoolRatio()

std::shared_ptr<Cache> NewLRUCache(const LRUCacheOptions& cache_opts) {
  return NewLRUCache(cache_opts.capacity, cache_opts.num_shard_bits,
                     cache_opts.strict_capacity_limit,
                     cache_opts.high_pri_pool_ratio,
                     cache_opts.memory_allocator);
}

std::shared_ptr<Cache> NewLRUCache(
    size_t capacity, int num_shard_bits, bool strict_capacity_limit,
    double high_pri_pool_ratio,
    std::shared_ptr<MemoryAllocator> memory_allocator) {
  if (num_shard_bits >= 20) {
    return nullptr;  // the cache cannot be sharded into too many fine pieces
  }
  if (high_pri_pool_ratio < 0.0 || high_pri_pool_ratio > 1.0) {
    // invalid high_pri_pool_ratio
    return nullptr;
  }
  if (num_shard_bits < 0) {
    num_shard_bits = GetDefaultCacheShardBits(capacity);
  }
  return std::make_shared<LRUCache>(
      capacity, num_shard_bits, strict_capacity_limit, high_pri_pool_ratio,
      LRUCacheShard::MonitorOptions{}, std::move(memory_allocator));
}

#ifdef WITH_DIAGNOSE_CACHE
std::shared_ptr<Cache> NewDiagnosableLRUCache(
    const LRUCacheOptions& cache_opts) {
  assert(cache_opts.is_diagnose);
  return NewDiagnosableLRUCache(cache_opts.capacity, cache_opts.num_shard_bits,
                                cache_opts.strict_capacity_limit,
                                cache_opts.high_pri_pool_ratio,
                                cache_opts.memory_allocator, cache_opts.topk);
}

std::shared_ptr<Cache> NewDiagnosableLRUCache(
    size_t capacity, int num_shard_bits, bool strict_capacity_limit,
    double high_pri_pool_ratio,
    std::shared_ptr<MemoryAllocator> memory_allocator, size_t topk) {
  if (num_shard_bits >= 20) {
    return nullptr;  // the cache cannot be sharded into too many fine pieces
  }
  if (high_pri_pool_ratio < 0.0 || high_pri_pool_ratio > 1.0) {
    // invalid high_pri_pool_ratio
    return nullptr;
  }
  if (num_shard_bits < 0) {
    num_shard_bits = GetDefaultCacheShardBits(capacity);
  }
  return std::make_shared<DiagnosableLRUCache>(
      capacity, num_shard_bits, strict_capacity_limit, high_pri_pool_ratio,
      LRUCacheDiagnosableShard::MonitorOptions{topk},
      std::move(memory_allocator));
}

template class LRUCacheShardTemplate<LRUCacheDiagnosableMonitor>;
template class LRUCacheBase<LRUCacheDiagnosableShard>;
#else
std::shared_ptr<Cache> NewDiagnosableLRUCache(
    const LRUCacheOptions& cache_opts) {
  return NewLRUCache(cache_opts.capacity, cache_opts.num_shard_bits,
                     cache_opts.strict_capacity_limit,
                     cache_opts.high_pri_pool_ratio,
                     cache_opts.memory_allocator);
}

std::shared_ptr<Cache> NewDiagnosableLRUCache(
    size_t capacity, int num_shard_bits, bool strict_capacity_limit,
    double high_pri_pool_ratio,
    std::shared_ptr<MemoryAllocator> memory_allocator, size_t /* topk */) {
  return NewLRUCache(capacity, num_shard_bits, strict_capacity_limit,
                     high_pri_pool_ratio, memory_allocator);
}
#endif

template class LRUCacheShardTemplate<LRUCacheNoMonitor>;
template class LRUCacheBase<LRUCacheShard>;

}  // namespace TERARKDB_NAMESPACE
