#pragma once

#include <iostream>
#include <vector>
#include <atomic>

#include "macros.h"

namespace Common {
  template<typename T>
  class LFQueue final {
  public:
    explicit LFQueue(std::size_t num_elems) :
        store_(round_up_to_power_of_2(num_elems), T()),
        mask_(store_.size() - 1) {
    }

    auto getNextToWriteTo() noexcept {
      return &store_[next_write_index_.load(std::memory_order_relaxed) & mask_];
    }

    auto updateWriteIndex() noexcept {
      auto current_write_index = next_write_index_.load(std::memory_order_relaxed);
      next_write_index_.store(current_write_index + 1, std::memory_order_relaxed);
      ASSERT(num_elements_ != 0, "Read an invalid element in:" + std::to_string(pthread_self()));
      num_elements_.fetch_add(1, std::memory_order_release);
    }

    auto getNextToRead() const noexcept -> const T * {
      auto current_read_index = next_read_index_.load(std::memory_order_relaxed);
      return (num_elements_.load(std::memory_order_acquire) ? 
              &store_[current_read_index & mask_] : nullptr);
    }

    auto updateReadIndex() noexcept {
      auto current_read_index = next_read_index_.load(std::memory_order_relaxed);
      next_read_index_.store(current_read_index + 1, std::memory_order_relaxed);
      num_elements_.fetch_sub(1, std::memory_order_release);
    }

    auto size() const noexcept {
      return num_elements_.load(std::memory_order_acquire);
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    LFQueue() = delete;
    LFQueue(const LFQueue &) = delete;
    LFQueue(const LFQueue &&) = delete;
    LFQueue &operator=(const LFQueue &) = delete;
    LFQueue &operator=(const LFQueue &&) = delete;

  private:
    static std::size_t round_up_to_power_of_2(std::size_t v) {
      if (v == 0) return 1;
      
      --v;
      v |= v >> 1;
      v |= v >> 2;
      v |= v >> 4;
      v |= v >> 8;
      v |= v >> 16;
      v |= v >> 32;
      ++v;
      
      return v;
    }

    /// Underlying container of data accessed in FIFO order.
    std::vector<T> store_;
    const std::size_t mask_;

    /// Atomic trackers for next index to write new data to and read new data from.
    alignas(64) std::atomic<std::size_t> next_write_index_ = {0};
    alignas(64) std::atomic<std::size_t> next_read_index_ = {0};

    alignas(64) std::atomic<std::size_t> num_elements_ = {0};
  };
}
