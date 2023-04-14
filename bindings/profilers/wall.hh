#pragma once

#include <nan.h>
#include <v8-profiler.h>
#include "../wrap.hh"

namespace dd {

class WallProfiler : public Nan::ObjectWrap {
 private:
  int samplingInterval = 0;
  v8::CpuProfiler* cpuProfiler = nullptr;
  std::shared_ptr<LabelWrap> labels_;

  struct SampleContext {
    std::shared_ptr<LabelWrap> labels;
    int64_t timestamp; 
  };

  std::vector<SampleContext> contexts_;

  ~WallProfiler();
  void Dispose();

  // A new CPU profiler object will be created each time profiling is started
  // to work around https://bugs.chromium.org/p/v8/issues/detail?id=11051.
  v8::CpuProfiler* GetProfiler();

 public:
  explicit WallProfiler(int interval);

  v8::Local<v8::Value> GetLabels();
  void SetLabels(v8::Local<v8::Value>);

  void PushContext();

  static NAN_METHOD(New);
  static NAN_METHOD(Dispose);
  static NAN_METHOD(Start);
  static NAN_METHOD(Stop);
  static NAN_MODULE_INIT(Init);
  static NAN_GETTER(GetLabels);
  static NAN_SETTER(SetLabels);
};

}  // namespace dd
