#include "node_perf.h"
#include "aliased_buffer-inl.h"
#include "env-inl.h"
#include "histogram-inl.h"
#include "memory_tracker-inl.h"
#include "node_buffer.h"
#include "node_external_reference.h"
#include "node_internals.h"
#include "node_process-inl.h"
#include "util-inl.h"

#include <cinttypes>

namespace node {
namespace performance {

using v8::Context;
using v8::DontDelete;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::GCCallbackFlags;
using v8::GCType;
using v8::Int32;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::PropertyAttribute;
using v8::ReadOnly;
using v8::Value;

// Microseconds in a millisecond, as a float.
#define MICROS_PER_MILLIS 1e3
// Nanoseconds in a millisecond, as a float.
#define NANOS_PER_MILLIS 1e6

const uint64_t performance_process_start = PERFORMANCE_NOW();
const double performance_process_start_timestamp =
    GetCurrentTimeInMicroseconds();
uint64_t performance_v8_start;

PerformanceState::PerformanceState(Isolate* isolate,
                                   const PerformanceState::SerializeInfo* info)
    : root(isolate,
           sizeof(performance_state_internal),
           MAYBE_FIELD_PTR(info, root)),
      milestones(isolate,
                 offsetof(performance_state_internal, milestones),
                 NODE_PERFORMANCE_MILESTONE_INVALID,
                 root,
                 MAYBE_FIELD_PTR(info, milestones)),
      observers(isolate,
                offsetof(performance_state_internal, observers),
                NODE_PERFORMANCE_ENTRY_TYPE_INVALID,
                root,
                MAYBE_FIELD_PTR(info, observers)) {
  if (info == nullptr) {
    for (size_t i = 0; i < milestones.Length(); i++) milestones[i] = -1.;
  }
}

PerformanceState::SerializeInfo PerformanceState::Serialize(
    v8::Local<v8::Context> context, v8::SnapshotCreator* creator) {
  SerializeInfo info{root.Serialize(context, creator),
                     milestones.Serialize(context, creator),
                     observers.Serialize(context, creator)};
  return info;
}

void PerformanceState::Deserialize(v8::Local<v8::Context> context) {
  root.Deserialize(context);
  // This is just done to set up the pointers, we will actually reset
  // all the milestones after deserialization.
  milestones.Deserialize(context);
  observers.Deserialize(context);
}

std::ostream& operator<<(std::ostream& o,
                         const PerformanceState::SerializeInfo& i) {
  o << "{\n"
    << "  " << i.root << ",  // root\n"
    << "  " << i.milestones << ",  // milestones\n"
    << "  " << i.observers << ",  // observers\n"
    << "}";
  return o;
}

void PerformanceState::Mark(PerformanceMilestone milestone, uint64_t ts) {
  this->milestones[milestone] = static_cast<double>(ts);
  TRACE_EVENT_INSTANT_WITH_TIMESTAMP0(
      TRACING_CATEGORY_NODE1(bootstrap),
      GetPerformanceMilestoneName(milestone),
      TRACE_EVENT_SCOPE_THREAD, ts / 1000);
}

// Allows specific Node.js lifecycle milestones to be set from JavaScript
void MarkMilestone(const FunctionCallbackInfo<Value>& args) {
  Realm* realm = Realm::GetCurrent(args);
  // TODO(legendecas): Remove this check once the sub-realms are supported.
  CHECK_EQ(realm->kind(), Realm::Kind::kPrincipal);
  Environment* env = realm->env();
  PerformanceMilestone milestone =
      static_cast<PerformanceMilestone>(args[0].As<Int32>()->Value());
  if (milestone != NODE_PERFORMANCE_MILESTONE_INVALID)
    env->performance_state()->Mark(milestone);
}

void SetupPerformanceObservers(const FunctionCallbackInfo<Value>& args) {
  Realm* realm = Realm::GetCurrent(args);
  // TODO(legendecas): Remove this check once the sub-realms are supported.
  CHECK_EQ(realm->kind(), Realm::Kind::kPrincipal);
  CHECK(args[0]->IsFunction());
  realm->set_performance_entry_callback(args[0].As<Function>());
}

// Marks the start of a GC cycle
void MarkGarbageCollectionStart(
    Isolate* isolate,
    GCType type,
    GCCallbackFlags flags,
    void* data) {
  Environment* env = static_cast<Environment*>(data);
  // Prevent gc callback from reentering with different type
  // See https://github.com/nodejs/node/issues/44046
  if (env->performance_state()->current_gc_type != 0) {
    return;
  }
  env->performance_state()->performance_last_gc_start_mark = PERFORMANCE_NOW();
  env->performance_state()->current_gc_type = type;
}

MaybeLocal<Object> GCPerformanceEntryTraits::GetDetails(
    Environment* env,
    const GCPerformanceEntry& entry) {
  Local<Object> obj = Object::New(env->isolate());

  if (!obj->Set(
          env->context(),
          env->kind_string(),
          Integer::NewFromUnsigned(
              env->isolate(),
              entry.details.kind)).IsJust()) {
    return MaybeLocal<Object>();
  }

  if (!obj->Set(
          env->context(),
          env->flags_string(),
          Integer::NewFromUnsigned(
              env->isolate(),
              entry.details.flags)).IsJust()) {
    return MaybeLocal<Object>();
  }

  return obj;
}

// Marks the end of a GC cycle
void MarkGarbageCollectionEnd(
    Isolate* isolate,
    GCType type,
    GCCallbackFlags flags,
    void* data) {
  Environment* env = static_cast<Environment*>(data);
  PerformanceState* state = env->performance_state();
  if (type != state->current_gc_type) {
    return;
  }
  env->performance_state()->current_gc_type = 0;
  // If no one is listening to gc performance entries, do not create them.
  if (LIKELY(!state->observers[NODE_PERFORMANCE_ENTRY_TYPE_GC]))
    return;

  double start_time =
      (state->performance_last_gc_start_mark - env->time_origin()) /
      NANOS_PER_MILLIS;
  double duration = (PERFORMANCE_NOW() / NANOS_PER_MILLIS) -
                    (state->performance_last_gc_start_mark / NANOS_PER_MILLIS);

  std::unique_ptr<GCPerformanceEntry> entry =
      std::make_unique<GCPerformanceEntry>(
          "gc",
          start_time,
          duration,
          GCPerformanceEntry::Details(static_cast<PerformanceGCKind>(type),
                                      static_cast<PerformanceGCFlags>(flags)));

  env->SetImmediate([entry = std::move(entry)](Environment* env) {
    entry->Notify(env);
  }, CallbackFlags::kUnrefed);
}

void GarbageCollectionCleanupHook(void* data) {
  Environment* env = static_cast<Environment*>(data);
  // Reset current_gc_type to 0
  env->performance_state()->current_gc_type = 0;
  env->isolate()->RemoveGCPrologueCallback(MarkGarbageCollectionStart, data);
  env->isolate()->RemoveGCEpilogueCallback(MarkGarbageCollectionEnd, data);
}

static void InstallGarbageCollectionTracking(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  // Reset current_gc_type to 0
  env->performance_state()->current_gc_type = 0;
  env->isolate()->AddGCPrologueCallback(MarkGarbageCollectionStart,
                                        static_cast<void*>(env));
  env->isolate()->AddGCEpilogueCallback(MarkGarbageCollectionEnd,
                                        static_cast<void*>(env));
  env->AddCleanupHook(GarbageCollectionCleanupHook, env);
}

static void RemoveGarbageCollectionTracking(
  const FunctionCallbackInfo<Value> &args) {
  Environment* env = Environment::GetCurrent(args);

  env->RemoveCleanupHook(GarbageCollectionCleanupHook, env);
  GarbageCollectionCleanupHook(env);
}

// Gets the name of a function
inline Local<Value> GetName(Local<Function> fn) {
  Local<Value> val = fn->GetDebugName();
  if (val.IsEmpty() || val->IsUndefined()) {
    Local<Value> boundFunction = fn->GetBoundFunction();
    if (!boundFunction.IsEmpty() && !boundFunction->IsUndefined()) {
      val = GetName(boundFunction.As<Function>());
    }
  }
  return val;
}

// Notify a custom PerformanceEntry to observers
void Notify(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Utf8Value type(env->isolate(), args[0]);
  Local<Value> entry = args[1];
  PerformanceEntryType entry_type = ToPerformanceEntryTypeEnum(*type);
  AliasedUint32Array& observers = env->performance_state()->observers;
  if (entry_type != NODE_PERFORMANCE_ENTRY_TYPE_INVALID &&
      observers[entry_type]) {
    USE(env->performance_entry_callback()->
      Call(env->context(), Undefined(env->isolate()), 1, &entry));
  }
}

// Return idle time of the event loop
void LoopIdleTime(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  uint64_t idle_time = uv_metrics_idle_time(env->event_loop());
  args.GetReturnValue().Set(1.0 * idle_time / 1e6);
}

void CreateELDHistogram(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  int64_t interval = args[0].As<Integer>()->Value();
  CHECK_GT(interval, 0);
  BaseObjectPtr<IntervalHistogram> histogram =
      IntervalHistogram::Create(env, interval, [](Histogram& histogram) {
        uint64_t delta = histogram.RecordDelta();
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                        "delay", delta);
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                      "min", histogram.Min());
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                      "max", histogram.Max());
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                      "mean", histogram.Mean());
        TRACE_COUNTER1(TRACING_CATEGORY_NODE2(perf, event_loop),
                      "stddev", histogram.Stddev());
      }, Histogram::Options { 1000 });
  args.GetReturnValue().Set(histogram->object());
}

void GetTimeOrigin(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  args.GetReturnValue().Set(
      Number::New(args.GetIsolate(), env->time_origin() / NANOS_PER_MILLIS));
}

void GetTimeOriginTimeStamp(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  args.GetReturnValue().Set(Number::New(
      args.GetIsolate(), env->time_origin_timestamp() / MICROS_PER_MILLIS));
}

void MarkBootstrapComplete(const FunctionCallbackInfo<Value>& args) {
  Realm* realm = Realm::GetCurrent(args);
  CHECK_EQ(realm->kind(), Realm::Kind::kPrincipal);
  realm->env()->performance_state()->Mark(
      performance::NODE_PERFORMANCE_MILESTONE_BOOTSTRAP_COMPLETE);
}

static void CreatePerIsolateProperties(IsolateData* isolate_data,
                                       Local<FunctionTemplate> target) {
  Isolate* isolate = isolate_data->isolate();
  Local<ObjectTemplate> proto = target->PrototypeTemplate();

  HistogramBase::Initialize(isolate_data, proto);

  SetMethod(isolate, proto, "markMilestone", MarkMilestone);
  SetMethod(isolate, proto, "setupObservers", SetupPerformanceObservers);
  SetMethod(isolate,
            proto,
            "installGarbageCollectionTracking",
            InstallGarbageCollectionTracking);
  SetMethod(isolate,
            proto,
            "removeGarbageCollectionTracking",
            RemoveGarbageCollectionTracking);
  SetMethod(isolate, proto, "notify", Notify);
  SetMethod(isolate, proto, "loopIdleTime", LoopIdleTime);
  SetMethod(isolate, proto, "getTimeOrigin", GetTimeOrigin);
  SetMethod(isolate, proto, "getTimeOriginTimestamp", GetTimeOriginTimeStamp);
  SetMethod(isolate, proto, "createELDHistogram", CreateELDHistogram);
  SetMethod(isolate, proto, "markBootstrapComplete", MarkBootstrapComplete);
}

void CreatePerContextProperties(Local<Object> target,
                                Local<Value> unused,
                                Local<Context> context,
                                void* priv) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();
  PerformanceState* state = env->performance_state();

  target->Set(context,
              FIXED_ONE_BYTE_STRING(isolate, "observerCounts"),
              state->observers.GetJSArray()).Check();
  target->Set(context,
              FIXED_ONE_BYTE_STRING(isolate, "milestones"),
              state->milestones.GetJSArray()).Check();

  Local<Object> constants = Object::New(isolate);

  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_MAJOR);
  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_MINOR);
  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_INCREMENTAL);
  NODE_DEFINE_CONSTANT(constants, NODE_PERFORMANCE_GC_WEAKCB);

  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_NO);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_FORCED);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY);
  NODE_DEFINE_CONSTANT(
    constants, NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE);

#define V(name, _)                                                            \
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NODE_PERFORMANCE_ENTRY_TYPE_##name);
  NODE_PERFORMANCE_ENTRY_TYPES(V)
#undef V

#define V(name, _)                                                            \
  NODE_DEFINE_HIDDEN_CONSTANT(constants, NODE_PERFORMANCE_MILESTONE_##name);
  NODE_PERFORMANCE_MILESTONES(V)
#undef V

  PropertyAttribute attr =
      static_cast<PropertyAttribute>(ReadOnly | DontDelete);

  target->DefineOwnProperty(context, env->constants_string(), constants, attr)
      .ToChecked();
}

void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(MarkMilestone);
  registry->Register(SetupPerformanceObservers);
  registry->Register(InstallGarbageCollectionTracking);
  registry->Register(RemoveGarbageCollectionTracking);
  registry->Register(Notify);
  registry->Register(LoopIdleTime);
  registry->Register(GetTimeOrigin);
  registry->Register(GetTimeOriginTimeStamp);
  registry->Register(CreateELDHistogram);
  registry->Register(MarkBootstrapComplete);
  HistogramBase::RegisterExternalReferences(registry);
  IntervalHistogram::RegisterExternalReferences(registry);
}
}  // namespace performance
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(
    performance, node::performance::CreatePerContextProperties)
NODE_BINDING_PER_ISOLATE_INIT(performance,
                              node::performance::CreatePerIsolateProperties)
NODE_BINDING_EXTERNAL_REFERENCE(performance,
                                node::performance::RegisterExternalReferences)
