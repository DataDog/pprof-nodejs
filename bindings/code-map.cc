#include <unordered_map>

#include <node.h>

#include "profilers/cpu.hh"
#include "code-map.hh"

namespace dd {

static std::unordered_map<v8::Isolate*, std::shared_ptr<CodeMap>> code_maps_;

static std::string toString(v8::Local<v8::String> s, v8::Isolate *isolate) {
  std::string str;
  if (!s.IsEmpty()) {
    str.resize(s->Utf8Length(isolate) + 1);
    s->WriteUtf8(isolate, &str[0]);
  }
  return str;
}

CodeMap::CodeMap(v8::Isolate* isolate, CodeEntries entries)
  : CodeEventHandler(isolate),
    code_entries_(entries),
    isolate_(isolate) {}

CodeMap::~CodeMap() {
  Disable();
}

std::shared_ptr<CodeMap> CodeMap::For(v8::Isolate* isolate) {
  auto maybe = code_maps_.find(isolate);
  if (maybe != code_maps_.end()) {
    return maybe->second;
  }

  code_maps_[isolate] = std::make_shared<CodeMap>(isolate);
  return code_maps_[isolate];
}

CodeEntries CodeMap::Entries() {
  return code_entries_;
}

void CodeMap::Enable() {
  if (++refs == 1) {
    CodeEventHandler::Enable();
    isolate_->SetJitCodeEventHandler(v8::kJitCodeEventDefault,
                                    StaticHandleJitEvent);
  }
}

void CodeMap::Disable() {
  if (--refs == 0) {
    CodeEventHandler::Disable();
    isolate_->SetJitCodeEventHandler(v8::kJitCodeEventDefault, nullptr);
    code_entries_.clear();
  }
}

// TODO: unsure of ordering but might need bi-directional merging for script id
void CodeMap::HandleJitEvent(const v8::JitCodeEvent* event) {
  if (event->type == v8::JitCodeEvent::CODE_REMOVED) {
    Remove(reinterpret_cast<uintptr_t>(event->code_start));
    return;
  }

  CodeEntries::iterator it = code_entries_.find(
      reinterpret_cast<uintptr_t>(event->code_start));

  if (it != code_entries_.end() && !event->script.IsEmpty()) {
    it->second->SetScriptId(event->script->GetId());
  }
}

static void printCodeEvent(const v8::JitCodeEvent* event) {
  switch (event->type) {
  case v8::JitCodeEvent::CODE_ADDED:
  fprintf(stderr, "JitCodeEvent: CODE_ADDED, start=%p, size=%zu, name=%.*s\n",
          event->code_start,
          event->code_len,
          (int)event->name.len,
          event->name.str);
    break;

  case v8::JitCodeEvent::CODE_MOVED:
    fprintf(stderr, "JitCodeEvent: CODE_MOVED code_start=%p code_len=%zu new_code_start=%p\n",
           event->code_start,
           event->code_len,
           event->new_code_start);
    break;

  case v8::JitCodeEvent::CODE_REMOVED:
    fprintf(stderr, "JitCodeEvent: CODE_REMOVED code_start=%p code_len=%zu\n",
           event->code_start,
           event->code_len);
    break;
  case v8::JitCodeEvent::CODE_START_LINE_INFO_RECORDING:
    fprintf(stderr, "JitCodeEvent: CODE_START_LINE_INFO_RECORDING\n");
    break;
  case v8::JitCodeEvent::CODE_END_LINE_INFO_RECORDING:
    fprintf(stderr, "JitCodeEvent: CODE_END_LINE_INFO_RECORDING start=%p\n",
           event->code_start);
   case v8::JitCodeEvent::CODE_ADD_LINE_POS_INFO:
    fprintf(stderr, "JitCodeEvent: CODE_ADD_LINE_POS_INFO offset=%zu pos=%zu position=%d\n",
           event->line_info.offset, event->line_info.pos, event->line_info.position_type);

    break;
      default:
    ;  // Ignore.
  }
}

static void printCodeEvent(v8::CodeEvent* event, v8::Isolate* isolate) {
  fprintf(stderr, "CodeEvent: %s start=0x%lx size=%zu previous_start=0x%lx function=%s script=%s\n",
        v8::CodeEvent::GetCodeEventTypeName(event->GetCodeType()),
        event->GetCodeStartAddress(),
        event->GetCodeSize(),
        event->GetPreviousCodeStartAddress(),
        toString(event->GetFunctionName(), isolate).c_str(), toString(event->GetScriptName(), isolate).c_str());
}

void CodeMap::StaticHandleJitEvent(const v8::JitCodeEvent* event) {
  printCodeEvent(event);
  auto code_map = CodeMap::For(event->isolate);
  code_map->HandleJitEvent(event);
}

// TODO: Figure out if additional checks are needed to cleanup expired regions.
// If size of previous is greater than offset of new position, the old record
// must be invalid, clean it up.
void CodeMap::Handle(v8::CodeEvent* code_event) {
  printCodeEvent(code_event, isolate_);
#if NODE_MODULE_VERSION > 79
  if (code_event->GetCodeType() == v8::CodeEventType::kRelocationType) {
    CodeEntries::iterator it = code_entries_.find(
      code_event->GetPreviousCodeStartAddress());
    if (it != code_entries_.end()) {
      code_entries_.erase(it);
      fprintf(stderr, "Removing code at 0x%lx (code_entries_.size=%zu)\n", it->first, code_entries_.size());
    }
  }
#endif

  Add(code_event->GetCodeStartAddress(),
      std::make_shared<CodeEventRecord>(isolate_, code_event));
}

void CodeMap::Add(uintptr_t address, std::shared_ptr<CodeEventRecord> record) {
  auto rng = OverlappingRange(address, address + record->size);
  if (rng.first != rng.second) {
    code_entries_.erase(rng.first, rng.second);
    fprintf(stderr, "Removing overlapping code at 0x%lx (code_entries_.size=%zu)\n", address, code_entries_.size());
  }
  code_entries_.insert(std::make_pair(address, std::move(record)));
  fprintf(stderr, "Inserting code at 0x%lx (code_entries_.size=%zu)\n", address, code_entries_.size());
}

void CodeMap::Remove(uintptr_t address) {
  code_entries_.erase(address);
  fprintf(stderr, "Removing code at 0x%lx (code_entries_.size=%zu)\n", address, code_entries_.size());
}

void CodeMap::Clear() {
  code_entries_.clear();
}

std::shared_ptr<CodeEventRecord> CodeMap::Lookup(uintptr_t address) {
  CodeEntries::iterator it = code_entries_.upper_bound(address);
  if (it == code_entries_.begin()) return nullptr;
  --it;
  uintptr_t start_address = it->first;
  std::shared_ptr<CodeEventRecord> entry = it->second;
  uintptr_t code_end = start_address + entry->size;
  if (address >= code_end) return nullptr;
  return entry;
}

CodeMap::RecordRange CodeMap::OverlappingRange(uintptr_t start_address, uintptr_t end_address) {
  auto first = code_entries_.upper_bound(start_address);
  if (first != code_entries_.begin()) {
    auto p = std::prev(first);
    if (p->first + p->second->size > start_address) {
      first = p;
    }
  }
  auto last = (first == code_entries_.end() || first->first >= end_address) ? first : code_entries_.lower_bound(end_address);
  return {first, last};

}

}; // namespace dd
