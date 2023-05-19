#pragma once

#include <nan.h>
#include <v8-profiler.h>
#include <unordered_map>

#include "../buffer.hh"

namespace dd {

class WallProfiler : public Nan::ObjectWrap {
 private:
  int samplingInterval = 0;
  v8::CpuProfiler* cpuProfiler = nullptr;
  std::shared_ptr<v8::Global<v8::Value>> labels_;
  std::unordered_map<const v8::CpuProfileNode*, v8::Local<v8::Array>>
      labelSetsByNode;
  bool labelsCaptured = false;

  struct SampleContext {
    std::shared_ptr<v8::Global<v8::Value>> labels;
    uint64_t timestamp;

    // Needed to initialize ring buffer elements
    SampleContext() {}

    SampleContext(std::shared_ptr<v8::Global<v8::Value>> l, uint64_t t)
        : labels(l), timestamp(t) {}

    SampleContext(SampleContext&& rval) {
      labels = std::move(rval.labels);
      timestamp = rval.timestamp;
    }

    SampleContext& operator=(SampleContext&& rval) {
      labels = std::move(rval.labels);
      timestamp = rval.timestamp;
      return *this;
    }
  };

  RingBuffer<SampleContext> contexts;
  uint64_t current_start;  // start of current profiling run
  uint64_t last_start;     // start of last profiling run

  ~WallProfiler();
  void Dispose(v8::Isolate* isolate);

  // A new CPU profiler object will be created each time profiling is started
  // to work around https://bugs.chromium.org/p/v8/issues/detail?id=11051.
  v8::CpuProfiler* GetProfiler();

  void AddLabelSetsByNode(v8::CpuProfile* profile);
  v8::Local<v8::Array> getLabelSetsForNode(const v8::CpuProfileNode* node);
  v8::Local<v8::Array> GetLineNumberTimeProfileChildren(
      const v8::CpuProfileNode* node);
  v8::Local<v8::Object> TranslateLineNumbersTimeProfileNode(
      const v8::CpuProfileNode* parent, const v8::CpuProfileNode* node);
  v8::Local<v8::Value> TranslateLineNumbersTimeProfileRoot(
      const v8::CpuProfileNode* node);
  v8::Local<v8::Value> TranslateTimeProfileNode(const v8::CpuProfileNode* node);
  // TODO: get rid of includeLineInfo
  v8::Local<v8::Value> TranslateTimeProfile(const v8::CpuProfile* profile,
                                            bool includeLineInfo);

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
  void UnsetLabels();
  bool GetLabelsCaptured() {
    bool captured = labelsCaptured;
    labelsCaptured = false;
    return captured;
  }

  void PushContext();
  void StartImpl(v8::Local<v8::String> name, bool includeLines);
  v8::Local<v8::Value> StopImpl(v8::Local<v8::String> name, bool includeLines);

  static NAN_METHOD(New);
  static NAN_METHOD(Dispose);
  static NAN_METHOD(Start);
  static NAN_METHOD(Stop);
  static NAN_METHOD(UnsetLabels);
  static NAN_MODULE_INIT(Init);
  static NAN_GETTER(GetLabels);
  static NAN_SETTER(SetLabels);
  static NAN_GETTER(GetLabelsCaptured);
};

}  // namespace dd
