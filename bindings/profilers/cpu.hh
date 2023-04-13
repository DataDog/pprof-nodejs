#pragma once

#include <cstdint>
#include <memory>
#include <atomic>

#include <nan.h>
#include <uv.h>

#include "../code-map.hh"
#include "../cpu-time.hh"
#include "../sample.hh"
#include "../wrap.hh"

namespace dd {

template<typename T, size_t Size>
class RingBuffer
{
public:
  RingBuffer() : size_(0),
                 back_index_(0),
                 front_index_(0) {}

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;

  bool Full() const { return size_ == Size; }
  bool Empty() const { return size_ == 0; }

  T* Reserve() {
    if (Full()) {
      return nullptr;
    }
    return &elements_[back_index_];
  }

  void Push() {
    increment(back_index_);
    ++size_;
  }

  T* Peek() {
    return Full() ? nullptr : &elements_[front_index_];
  }

  void Remove()
  {
    increment(front_index_);
    --size_;
  }

private:
  void increment(size_t &idx) const
  {
    idx = idx + 1 == Size ? 0 : idx + 1;
  }

  std::array<T, Size> elements_;
  size_t size_;
  size_t back_index_;
  size_t front_index_;
};

class CpuProfiler : public Nan::ObjectWrap {
  friend class CodeMap;

 private:
   static constexpr size_t k_sample_buffer_size = 100;

  v8::Isolate* isolate_;
  uv_async_t* async;
  std::shared_ptr<CodeMap> code_map;
  CpuTime cpu_time;
  int64_t unaccounted_cpu_time = 0;
  RingBuffer<RawSample, k_sample_buffer_size> samples_buffer_;
  std::shared_ptr<LabelWrap> labels_;
  double frequency = 0;
  Nan::Global<v8::Array> samples;
  uint64_t start_time;
  uv_sem_t sampler_thread_done;
  uv_thread_t sampler_thread;
  std::atomic_bool sampler_running;
  pthread_t js_thread;
  bool use_signals;
  bool use_sigprof_from_v8;
  int signum;

 public:
  CpuProfiler();
  ~CpuProfiler();

  // Disable copies and moves
  CpuProfiler(const CpuProfiler& other) = delete;
  CpuProfiler(CpuProfiler&& other) = delete;
  CpuProfiler& operator=(const CpuProfiler& other) = delete;
  CpuProfiler& operator=(CpuProfiler&& other) = delete;

  v8::Local<v8::Number> GetFrequency();

  void SetLastSample(std::unique_ptr<Sample> sample);
  Sample* GetLastSample();
  void CaptureSample(v8::Isolate* isolate, void* context = nullptr);
  void CaptureSample2(void *context);
  void SamplerThread(double hz);

  void ProcessSample();
  static void Run(uv_async_t* handle);

  v8::Local<v8::Value> GetLabels();
  void SetLabels(v8::Local<v8::Value>);
  void Start(double hz);
  void Stop();
  void StopAndWaitThread();
  uint32_t GetSampleCount();
  v8::Local<v8::Array> GetSamples();
  v8::Local<v8::Value> GetProfile();

  static NAN_METHOD(New);
  static NAN_GETTER(GetFrequency);
  static NAN_GETTER(GetLabels);
  static NAN_SETTER(SetLabels);
  static NAN_METHOD(Start);
  static NAN_METHOD(Stop);
  static NAN_METHOD(CaptureSample);
  static NAN_METHOD(ProcessSample);
  static NAN_METHOD(GetSamples);
  static NAN_METHOD(GetProfile);

  static NAN_MODULE_INIT(Init);
};

} // namespace dd
