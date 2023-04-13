#pragma once

#include <vector>
#include <memory>
#include <cstdint>

#include "code-map.hh"
#include "wrap.hh"

#include <array>
#include <node_object_wrap.h>
#include <v8.h>

namespace dd {

struct RawSample {
  static constexpr size_t kMaxFramesCount = 255;
   using Stack = std::array<void*, kMaxFramesCount>;
   Stack stack;
   size_t frame_count;
   uint64_t timestamp;
   int64_t  cpu_time;
   void* pc;
   void* external_callback_entry;
   v8::StateTag vm_state;
   std::shared_ptr<LabelWrap> labels;
};

class Sample : public Nan::ObjectWrap {
 private:

  std::shared_ptr<LabelWrap> labels_;
  uint64_t timestamp_;
  Nan::Global<v8::Array> locations_;
  int64_t cpu_time_;

 public:
  Sample(std::shared_ptr<LabelWrap> labels,
         v8::Local<v8::Array> locations,
         uint64_t timestamp,
         int64_t cpu_time);

  v8::Local<v8::Integer> GetCpuTime(v8::Isolate* isolate);
  v8::Local<v8::Value> GetLabels(v8::Isolate* isolate);
  v8::Local<v8::Array> GetLocations(v8::Isolate* isolate);

  v8::Local<v8::Object> ToObject(v8::Isolate* isolate);

  static NAN_GETTER(GetCpuTime);
  static NAN_GETTER(GetLabels);
  static NAN_GETTER(GetLocations);

  static NAN_MODULE_INIT(Init);
};

void GetStackSample(v8::Isolate* isolate, void*context, RawSample* sample);
std::unique_ptr<Sample> SymbolizeSample(const RawSample&, const CodeMap& code_map);
} // namespace dd
