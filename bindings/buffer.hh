#pragma once

namespace dd {
template <class T>
class RingBuffer {
 public:
  explicit RingBuffer(size_t size)
      : buffer(std::make_unique<T[]>(size)),
        capacity_(size),
        size_(0),
        back_index_(0),
        front_index_(0) {}

  bool full() const { return size_ == capacity_; }
  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }

  T& front() { return buffer[front_index_]; }

  const T& front() const { return buffer[front_index_]; }

  template <typename T2>
  void push_back(T2&& t) {
    if (full()) {
      if (empty()) {
        return;
      }
      // overwrite buffer head
      buffer[back_index_] = std::forward<T2>(t);
      increment(back_index_);
      // move buffer head
      front_index_ = back_index_;
    } else {
      buffer[back_index_] = std::forward<T2>(t);
      increment(back_index_);
      ++size_;
    }
  }

  template <typename T2>
  void push_front(T2&& t) {
    if (full()) {
      if (empty()) {
        return;
      }
      // overwrite buffer head
      decrement(front_index_);
      buffer[front_index_] = std::forward<T2>(t);
      // move buffer head
      back_index_ = front_index_;
    } else {
      decrement(front_index_);
      buffer[front_index_] = std::forward<T2>(t);
      ++size_;
    }
  }

  T pop_front() {
    auto idx = front_index_;
    increment(front_index_);
    --size_;
    return std::move(buffer[idx]);
  }

 private:
  void increment(size_t& idx) const {
    idx = idx + 1 == capacity_ ? 0 : idx + 1;
  }
  void decrement(size_t& idx) const {
    idx = idx == 0 ? capacity_ - 1 : idx - 1;
  }

  std::unique_ptr<T[]> buffer;
  size_t capacity_;
  size_t size_;
  size_t back_index_;
  size_t front_index_;
};
}  // namespace dd
