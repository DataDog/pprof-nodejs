#include <unordered_map>

#include "per-isolate-data.hh"

namespace dd {

static std::unordered_map<v8::Isolate*, PerIsolateData*> per_isolate_data_;

PerIsolateData::PerIsolateData(v8::Isolate* isolate)
  : isolate_(isolate) {
  per_isolate_data_[isolate] = this;
  node::AddEnvironmentCleanupHook(isolate, [](void* data) {
    auto perIsolateData = static_cast<PerIsolateData*>(data);
    per_isolate_data_.erase(perIsolateData->isolate_);
    delete perIsolateData;
  }, this);
}

PerIsolateData* PerIsolateData::For(v8::Isolate* isolate) {
  auto maybe = per_isolate_data_.find(isolate);
  if (maybe != per_isolate_data_.end()) {
    return maybe->second;
  }

  return new PerIsolateData(isolate);
}

Nan::Global<v8::Function>& PerIsolateData::CpuProfilerConstructor() {
  return cpu_profiler_constructor;
}

Nan::Global<v8::Function>& PerIsolateData::LocationConstructor() {
  return location_constructor;
}

Nan::Global<v8::Function>& PerIsolateData::SampleConstructor() {
  return sample_constructor;
}

} // namespace dd
