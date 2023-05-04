#include <mutex>
#include <unordered_map>
#include <utility>

#include "per-isolate-data.hh"

namespace dd {

static std::unordered_map<v8::Isolate*, PerIsolateData> per_isolate_data_;
static std::mutex mutex;

PerIsolateData* PerIsolateData::For(v8::Isolate* isolate) {
  const std::lock_guard<std::mutex> lock(mutex);
  auto maybe = per_isolate_data_.find(isolate);
  if (maybe != per_isolate_data_.end()) {
    return &maybe->second;
  }

  per_isolate_data_.emplace(std::make_pair(isolate, PerIsolateData()));

  auto pair = per_isolate_data_.find(isolate);
  auto perIsolateData = &pair->second;

  node::AddEnvironmentCleanupHook(
      isolate,
      [](void* data) {
        const std::lock_guard<std::mutex> lock(mutex);
        per_isolate_data_.erase(static_cast<v8::Isolate*>(data));
      },
      isolate);

  return perIsolateData;
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

Nan::Global<v8::Function>& PerIsolateData::WallProfilerConstructor() {
  return wall_profiler_constructor;
}

std::shared_ptr<HeapProfilerState>& PerIsolateData::GetHeapProfilerState() {
  return heap_profiler_state;
}

}  // namespace dd
