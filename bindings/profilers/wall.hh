#pragma once

#include <nan.h>
#include <v8-profiler.h>
#include <memory>
#include <unordered_map>
#include <utility>

#include "../buffer.hh"

namespace dd {

using LabelSetsByNode =
    std::unordered_map<const v8::CpuProfileNode*, v8::Local<v8::Array>>;

using ValuePtr = std::shared_ptr<v8::Global<v8::Value>>;

class WallProfiler : public Nan::ObjectWrap {
 private:
  int samplingInterval = 0;
  v8::CpuProfiler* cpuProfiler = nullptr;
  // TODO: Investigate use of v8::Persistent instead of shared_ptr<Global> to
  // avoid heap allocation. Need to figure out the right move/copy semantics in
  // and out of the ring buffer.

  // We're using a pair of shared pointers and an atomic pointer-to-current as
  // a way to ensure signal safety on update.
  ValuePtr labels1;
  ValuePtr labels2;
  std::atomic<ValuePtr*> curLabels;

  struct SampleContext {
    ValuePtr labels;
    int64_t time_from;
    int64_t time_to;

    // Needed to initialize ring buffer elements
    SampleContext() = default;

    SampleContext(const ValuePtr& l, int64_t from, int64_t to) : labels(l), time_from(from), time_to(to) {}
  };

  RingBuffer<SampleContext> contexts;

  ~WallProfiler();
  void Dispose(v8::Isolate* isolate);

  // A new CPU profiler object will be created each time profiling is started
  // to work around https://bugs.chromium.org/p/v8/issues/detail?id=11051.
  v8::CpuProfiler* GetProfiler();

  LabelSetsByNode GetLabelSetsByNode(v8::CpuProfile* profile);

 public:
  /**
   * @param intervalMicros sampling interval, in microseconds
   * @param durationMicros the duration of sampling, in microseconds. This
   * parameter is informative; it is up to the caller to call the Stop method
   * every period. The parameter is used to preallocate data structures that
   * should not be reallocated in async signal safe code.
   */
  explicit WallProfiler(int intervalMicros, int durationMicros);

  v8::Local<v8::Value> GetLabels(v8::Isolate*);
  void SetLabels(v8::Isolate*, v8::Local<v8::Value>);

  void PushContext(int64_t time_from);
  void StartImpl(v8::Local<v8::String> name,
                 bool includeLines,
                 bool withLabels);
  v8::Local<v8::Value> StopImpl(v8::Local<v8::String> name,
                                bool includeLines);

  static NAN_METHOD(New);
  static NAN_METHOD(Dispose);
  static NAN_METHOD(Start);
  static NAN_METHOD(Stop);
  static NAN_MODULE_INIT(Init);
  static NAN_GETTER(GetLabels);
  static NAN_SETTER(SetLabels);
};

}  // namespace dd
