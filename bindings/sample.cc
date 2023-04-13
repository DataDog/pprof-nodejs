#include <algorithm>
#include <iterator>
#include <deque>

#include <nan.h>

#include "per-isolate-data.hh"
#include "sample.hh"
#include "location.hh"
#include "code-map.hh"

#include <iostream>

#include <uv.h>
#include <v8config.h>

#ifdef __aarch64__
#define V8_HOST_ARCH_ARM64 1
#else
#define V8_HOST_ARCH_X64 1
#endif

#ifdef __APPLE__
#define V8_OS_DARWIN 1
#elif defined(__linux__)
#define V8_OS_LINUX 1
#endif

namespace dd {

void GetStackSample(v8::Isolate* isolate, void*context, RawSample* sample) {
  v8::SampleInfo sample_info;
  v8::RegisterState register_state;
  if (!context) {
    register_state.pc = nullptr;
    register_state.fp = &register_state;
    register_state.sp = &register_state;
  } else {
    ucontext_t* ucontext = reinterpret_cast<ucontext_t*>(context);
    v8::RegisterState* state = &register_state;
#if !(V8_OS_OPENBSD || \
      (V8_OS_LINUX &&  \
       (V8_HOST_ARCH_PPC || V8_HOST_ARCH_S390 || V8_HOST_ARCH_PPC64)))
  mcontext_t& mcontext = ucontext->uc_mcontext;
#endif
#if V8_OS_LINUX
#if V8_HOST_ARCH_IA32
  state->pc = reinterpret_cast<void*>(mcontext.gregs[REG_EIP]);
  state->sp = reinterpret_cast<void*>(mcontext.gregs[REG_ESP]);
  state->fp = reinterpret_cast<void*>(mcontext.gregs[REG_EBP]);
#elif V8_HOST_ARCH_X64
  state->pc = reinterpret_cast<void*>(mcontext.gregs[REG_RIP]);
  state->sp = reinterpret_cast<void*>(mcontext.gregs[REG_RSP]);
  state->fp = reinterpret_cast<void*>(mcontext.gregs[REG_RBP]);
#elif V8_HOST_ARCH_ARM
#if V8_LIBC_GLIBC && !V8_GLIBC_PREREQ(2, 4)
  // Old GLibc ARM versions used a gregs[] array to access the register
  // values from mcontext_t.
  state->pc = reinterpret_cast<void*>(mcontext.gregs[R15]);
  state->sp = reinterpret_cast<void*>(mcontext.gregs[R13]);
  state->fp = reinterpret_cast<void*>(mcontext.gregs[R11]);
  state->lr = reinterpret_cast<void*>(mcontext.gregs[R14]);
#else
  state->pc = reinterpret_cast<void*>(mcontext.arm_pc);
  state->sp = reinterpret_cast<void*>(mcontext.arm_sp);
  state->fp = reinterpret_cast<void*>(mcontext.arm_fp);
  state->lr = reinterpret_cast<void*>(mcontext.arm_lr);
#endif  // V8_LIBC_GLIBC && !V8_GLIBC_PREREQ(2, 4)
#elif V8_HOST_ARCH_ARM64
  state->pc = reinterpret_cast<void*>(mcontext.pc);
  state->sp = reinterpret_cast<void*>(mcontext.sp);
  // FP is an alias for x29.
  state->fp = reinterpret_cast<void*>(mcontext.regs[29]);
  // LR is an alias for x30.
  state->lr = reinterpret_cast<void*>(mcontext.regs[30]);
#elif V8_HOST_ARCH_MIPS
  state->pc = reinterpret_cast<void*>(mcontext.pc);
  state->sp = reinterpret_cast<void*>(mcontext.gregs[29]);
  state->fp = reinterpret_cast<void*>(mcontext.gregs[30]);
#elif V8_HOST_ARCH_MIPS64
  state->pc = reinterpret_cast<void*>(mcontext.pc);
  state->sp = reinterpret_cast<void*>(mcontext.gregs[29]);
  state->fp = reinterpret_cast<void*>(mcontext.gregs[30]);
#elif V8_HOST_ARCH_LOONG64
  state->pc = reinterpret_cast<void*>(mcontext.__pc);
  state->sp = reinterpret_cast<void*>(mcontext.__gregs[3]);
  state->fp = reinterpret_cast<void*>(mcontext.__gregs[22]);
#elif V8_HOST_ARCH_PPC || V8_HOST_ARCH_PPC64
#if V8_LIBC_GLIBC
  state->pc = reinterpret_cast<void*>(ucontext->uc_mcontext.regs->nip);
  state->sp = reinterpret_cast<void*>(ucontext->uc_mcontext.regs->gpr[PT_R1]);
  state->fp = reinterpret_cast<void*>(ucontext->uc_mcontext.regs->gpr[PT_R31]);
  state->lr = reinterpret_cast<void*>(ucontext->uc_mcontext.regs->link);
#else
  // Some C libraries, notably Musl, define the regs member as a void pointer
  state->pc = reinterpret_cast<void*>(ucontext->uc_mcontext.gp_regs[32]);
  state->sp = reinterpret_cast<void*>(ucontext->uc_mcontext.gp_regs[1]);
  state->fp = reinterpret_cast<void*>(ucontext->uc_mcontext.gp_regs[31]);
  state->lr = reinterpret_cast<void*>(ucontext->uc_mcontext.gp_regs[36]);
#endif
#elif V8_HOST_ARCH_S390
#if V8_TARGET_ARCH_32_BIT
  // 31-bit target will have bit 0 (MSB) of the PSW set to denote addressing
  // mode.  This bit needs to be masked out to resolve actual address.
  state->pc =
      reinterpret_cast<void*>(ucontext->uc_mcontext.psw.addr & 0x7FFFFFFF);
#else
  state->pc = reinterpret_cast<void*>(ucontext->uc_mcontext.psw.addr);
#endif  // V8_TARGET_ARCH_32_BIT
  state->sp = reinterpret_cast<void*>(ucontext->uc_mcontext.gregs[15]);
  state->fp = reinterpret_cast<void*>(ucontext->uc_mcontext.gregs[11]);
  state->lr = reinterpret_cast<void*>(ucontext->uc_mcontext.gregs[14]);
#elif V8_HOST_ARCH_RISCV64
  // Spec CH.25 RISC-V Assembly Programmer’s Handbook
  state->pc = reinterpret_cast<void*>(mcontext.__gregs[REG_PC]);
  state->sp = reinterpret_cast<void*>(mcontext.__gregs[REG_SP]);
  state->fp = reinterpret_cast<void*>(mcontext.__gregs[REG_S0]);
  state->lr = reinterpret_cast<void*>(mcontext.__gregs[REG_RA]);
#endif  // V8_HOST_ARCH_*
#elif V8_OS_IOS

#if V8_TARGET_ARCH_ARM64
  // Building for the iOS device.
  state->pc = reinterpret_cast<void*>(mcontext->__ss.__pc);
  state->sp = reinterpret_cast<void*>(mcontext->__ss.__sp);
  state->fp = reinterpret_cast<void*>(mcontext->__ss.__fp);
#elif V8_TARGET_ARCH_X64
  // Building for the iOS simulator.
  state->pc = reinterpret_cast<void*>(mcontext->__ss.__rip);
  state->sp = reinterpret_cast<void*>(mcontext->__ss.__rsp);
  state->fp = reinterpret_cast<void*>(mcontext->__ss.__rbp);
#else
#error Unexpected iOS target architecture.
#endif  // V8_TARGET_ARCH_ARM64

#elif V8_OS_DARWIN
#if V8_HOST_ARCH_X64
  state->pc = reinterpret_cast<void*>(mcontext->__ss.__rip);
  state->sp = reinterpret_cast<void*>(mcontext->__ss.__rsp);
  state->fp = reinterpret_cast<void*>(mcontext->__ss.__rbp);
#elif V8_HOST_ARCH_IA32
  state->pc = reinterpret_cast<void*>(mcontext->__ss.__eip);
  state->sp = reinterpret_cast<void*>(mcontext->__ss.__esp);
  state->fp = reinterpret_cast<void*>(mcontext->__ss.__ebp);
#elif V8_HOST_ARCH_ARM64
  state->pc =
      reinterpret_cast<void*>(arm_thread_state64_get_pc(mcontext->__ss));
  state->sp =
      reinterpret_cast<void*>(arm_thread_state64_get_sp(mcontext->__ss));
  state->fp =
      reinterpret_cast<void*>(arm_thread_state64_get_fp(mcontext->__ss));
#endif  // V8_HOST_ARCH_*
#elif V8_OS_FREEBSD
#if V8_HOST_ARCH_IA32
  state->pc = reinterpret_cast<void*>(mcontext.mc_eip);
  state->sp = reinterpret_cast<void*>(mcontext.mc_esp);
  state->fp = reinterpret_cast<void*>(mcontext.mc_ebp);
#elif V8_HOST_ARCH_X64
  state->pc = reinterpret_cast<void*>(mcontext.mc_rip);
  state->sp = reinterpret_cast<void*>(mcontext.mc_rsp);
  state->fp = reinterpret_cast<void*>(mcontext.mc_rbp);
#elif V8_HOST_ARCH_ARM
  state->pc = reinterpret_cast<void*>(mcontext.__gregs[_REG_PC]);
  state->sp = reinterpret_cast<void*>(mcontext.__gregs[_REG_SP]);
  state->fp = reinterpret_cast<void*>(mcontext.__gregs[_REG_FP]);
#endif  // V8_HOST_ARCH_*
#elif V8_OS_NETBSD
#if V8_HOST_ARCH_IA32
  state->pc = reinterpret_cast<void*>(mcontext.__gregs[_REG_EIP]);
  state->sp = reinterpret_cast<void*>(mcontext.__gregs[_REG_ESP]);
  state->fp = reinterpret_cast<void*>(mcontext.__gregs[_REG_EBP]);
#elif V8_HOST_ARCH_X64
  state->pc = reinterpret_cast<void*>(mcontext.__gregs[_REG_RIP]);
  state->sp = reinterpret_cast<void*>(mcontext.__gregs[_REG_RSP]);
  state->fp = reinterpret_cast<void*>(mcontext.__gregs[_REG_RBP]);
#endif  // V8_HOST_ARCH_*
#elif V8_OS_OPENBSD
#if V8_HOST_ARCH_IA32
  state->pc = reinterpret_cast<void*>(ucontext->sc_eip);
  state->sp = reinterpret_cast<void*>(ucontext->sc_esp);
  state->fp = reinterpret_cast<void*>(ucontext->sc_ebp);
#elif V8_HOST_ARCH_X64
  state->pc = reinterpret_cast<void*>(ucontext->sc_rip);
  state->sp = reinterpret_cast<void*>(ucontext->sc_rsp);
  state->fp = reinterpret_cast<void*>(ucontext->sc_rbp);
#endif  // V8_HOST_ARCH_*
#elif V8_OS_SOLARIS
  state->pc = reinterpret_cast<void*>(mcontext.gregs[REG_PC]);
  state->sp = reinterpret_cast<void*>(mcontext.gregs[REG_SP]);
  state->fp = reinterpret_cast<void*>(mcontext.gregs[REG_FP]);
#elif V8_OS_QNX
#if V8_HOST_ARCH_IA32
  state->pc = reinterpret_cast<void*>(mcontext.cpu.eip);
  state->sp = reinterpret_cast<void*>(mcontext.cpu.esp);
  state->fp = reinterpret_cast<void*>(mcontext.cpu.ebp);
#elif V8_HOST_ARCH_ARM
  state->pc = reinterpret_cast<void*>(mcontext.cpu.gpr[ARM_REG_PC]);
  state->sp = reinterpret_cast<void*>(mcontext.cpu.gpr[ARM_REG_SP]);
  state->fp = reinterpret_cast<void*>(mcontext.cpu.gpr[ARM_REG_FP]);
#endif  // V8_HOST_ARCH_*
#elif V8_OS_AIX
  state->pc = reinterpret_cast<void*>(mcontext.jmp_context.iar);
  state->sp = reinterpret_cast<void*>(mcontext.jmp_context.gpr[1]);
  state->fp = reinterpret_cast<void*>(mcontext.jmp_context.gpr[31]);
  state->lr = reinterpret_cast<void*>(mcontext.jmp_context.lr);
#endif  // V8_OS
  }

  if (register_state.pc != nullptr) {
    sample->pc = register_state.pc;
  }
  isolate->GetStackSample(register_state, sample->stack.data(), RawSample::kMaxFramesCount, &sample_info);

  sample->vm_state = sample_info.vm_state;
  sample->external_callback_entry = sample_info.external_callback_entry;
  sample->frame_count = sample_info.frames_count;
  // printf("[dd] Captured %zu frames: ", sample->frame_count + 1);
  // printf("%p", sample->pc);
  // for(size_t i=0; i<sample->frame_count; ++i) {
  //   printf("|%p", sample->stack[i]);
  // }
  // printf(" VMState: %d, external_callback: %p\n", sample->vm_state, sample->external_callback_entry);
  if (sample_info.vm_state == v8::StateTag::IDLE) {
    sample->frame_count = 0;
  }
}

Sample::Sample(std::shared_ptr<LabelWrap> labels,
               v8::Local<v8::Array> locations,
               uint64_t timestamp,
               int64_t cpu_time)
  : labels_(labels), timestamp_(timestamp), locations_(locations), cpu_time_(cpu_time) {

}

v8::Local<v8::Integer> Sample::GetCpuTime(v8::Isolate* isolate) {
  return v8::Integer::New(isolate, cpu_time_);
}

v8::Local<v8::Value> Sample::GetLabels(v8::Isolate* isolate) {
  if (!labels_) return v8::Undefined(isolate);
  return labels_->handle();
}

v8::Local<v8::Array> Sample::GetLocations(v8::Isolate* isolate) {
  if (locations_.IsEmpty()) {
    return Nan::New<v8::Array>();
  }
  return locations_.Get(isolate);
}

NAN_GETTER(Sample::GetCpuTime) {
  Sample* wrap = Nan::ObjectWrap::Unwrap<Sample>(info.Holder());
  info.GetReturnValue().Set(wrap->GetCpuTime(info.GetIsolate()));
}

NAN_GETTER(Sample::GetLabels) {
  Sample* wrap = Nan::ObjectWrap::Unwrap<Sample>(info.Holder());
  info.GetReturnValue().Set(wrap->GetLabels(info.GetIsolate()));
}

NAN_GETTER(Sample::GetLocations) {
  Sample* wrap = Nan::ObjectWrap::Unwrap<Sample>(info.Holder());
  info.GetReturnValue().Set(wrap->GetLocations(info.GetIsolate()));
}

v8::Local<v8::Object> Sample::ToObject(v8::Isolate* isolate) {
  if (!persistent().IsEmpty()) {
    return handle();
  }

  auto per_isolate = PerIsolateData::For(isolate);
  v8::Local<v8::Function> cons = Nan::New(
      per_isolate->SampleConstructor());
  auto inst = Nan::NewInstance(cons, 0, {}).ToLocalChecked();

  Wrap(inst);

  return handle();
}

NAN_MODULE_INIT(Sample::Init) {
  auto class_name = Nan::New<v8::String>("Sample")
      .ToLocalChecked();

  auto tpl = Nan::New<v8::FunctionTemplate>(nullptr);
  tpl->SetClassName(class_name);
  tpl->InstanceTemplate()
      ->SetInternalFieldCount(1);

  // auto proto = tpl->PrototypeTemplate();
  auto proto = tpl->InstanceTemplate();

  Nan::SetAccessor(
      proto,
      Nan::New("cpuTime").ToLocalChecked(),
      GetCpuTime);
  Nan::SetAccessor(
      proto,
      Nan::New("labels").ToLocalChecked(),
      GetLabels);
  Nan::SetAccessor(
      proto,
      Nan::New("locations").ToLocalChecked(),
      GetLocations);

  auto fn = Nan::GetFunction(tpl).ToLocalChecked();
  auto per_isolate = PerIsolateData::For(target->GetIsolate());
  per_isolate->SampleConstructor().Reset(fn);
}

std::unique_ptr<Sample> SymbolizeSample(const RawSample& sample, const CodeMap& code_map) {
  auto isolate = v8::Isolate::GetCurrent();
  auto locations = Nan::New<v8::Array>();

  for(size_t i = sample.frame_count; i > 0; --i) {
    auto record = code_map.Lookup(reinterpret_cast<uintptr_t>(sample.stack[i - 1]));
    if (record) {
      auto location = Location::New(isolate, record);
      Nan::Set(locations, locations->Length(), location->handle()).Check();
    }
  }

  // printf("[dd] Symbolized sample: ");

  auto pc = sample.external_callback_entry ? sample.external_callback_entry : sample.pc;
  auto address = reinterpret_cast<uintptr_t>(pc);
  auto record = code_map.Lookup(address);
  if (record) {
    // printf("%s0x%lx %s(%s)", sample.external_callback_entry ? "[external]" : "", address, record->functionName.c_str(), record->scriptName.c_str());
    auto location = Location::New(isolate, record);
    Nan::Set(locations, locations->Length(), location->handle()).Check();
  }

  for(size_t i = 0; i < sample.frame_count; ++i) {
    auto record = code_map.Lookup(reinterpret_cast<uintptr_t>(sample.stack[i]));
    // if (record) {
    //     printf("|0x%lx %s(%s)", reinterpret_cast<uintptr_t>(sample.stack[i]), record->functionName.c_str(), record->scriptName.c_str());
    // }
  }
  // printf("\n");
  return std::make_unique<Sample>(sample.labels, locations, sample.timestamp, sample.cpu_time);
}
} // namespace dd
