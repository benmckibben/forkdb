#include <napi.h>
#include <uv.h>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <memory>
#include <cinttypes>

// Include forkdb headers
#include "../../../../../../projects/forkdb/fkx/fkx.h"
#include "../../../../../../projects/forkdb/store/fkstore.h"

// Forward declarations
class ForkDB : public Napi::ObjectWrap<ForkDB> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  ForkDB(const Napi::CallbackInfo& info);
  ~ForkDB();

  // JavaScript methods
  Napi::Value Open(const Napi::CallbackInfo& info);
  Napi::Value Close(const Napi::CallbackInfo& info);
  Napi::Value Fork(const Napi::CallbackInfo& info);
  Napi::Value Snapshot(const Napi::CallbackInfo& info);
  Napi::Value Discard(const Napi::CallbackInfo& info);
  Napi::Value Reset(const Napi::CallbackInfo& info);
  Napi::Value Info(const Napi::CallbackInfo& info);
  Napi::Value List(const Napi::CallbackInfo& info);
  Napi::Value Promote(const Napi::CallbackInfo& info);
  Napi::Value Undo(const Napi::CallbackInfo& info);
  Napi::Value GC(const Napi::CallbackInfo& info);
  Napi::Value Compact(const Napi::CallbackInfo& info);

 private:
  fkx* db_;
  std::string path_;
  bool is_open_;

  // Helper functions
  Napi::Object FKXInfoToObject(Napi::Env env, const fkx_info& info);
  void CheckClosed();
};

// Module initialization
Napi::Object ForkDB::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(env, "ForkDB", {
    InstanceMethod("open", &ForkDB::Open),
    InstanceMethod("close", &ForkDB::Close),
    InstanceMethod("fork", &ForkDB::Fork),
    InstanceMethod("snapshot", &ForkDB::Snapshot),
    InstanceMethod("discard", &ForkDB::Discard),
    InstanceMethod("reset", &ForkDB::Reset),
    InstanceMethod("info", &ForkDB::Info),
    InstanceMethod("list", &ForkDB::List),
    InstanceMethod("promote", &ForkDB::Promote),
    InstanceMethod("undo", &ForkDB::Undo),
    InstanceMethod("gc", &ForkDB::GC),
    InstanceMethod("compact", &ForkDB::Compact)
  });

  Napi::FunctionReference* constructor = new Napi::FunctionReference();
  *constructor = Napi::Persistent(func);
  env.SetInstanceData(constructor);

  exports.Set(Napi::String::New(env, "ForkDB"), func);
  return exports;
}

ForkDB::ForkDB(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ForkDB>(info), db_(nullptr), is_open_(false) {
  Napi::Env env = info.Env();
  
  if (info.Length() > 0 && info[0].IsString()) {
    path_ = info[0].As<Napi::String>().Utf8Value();
  }
}

ForkDB::~ForkDB() {
  if (is_open_ && db_) {
    fkx_close(db_);
  }
}

void ForkDB::CheckClosed() {
  if (!is_open_ || !db_) {
    throw Napi::Error::New(Env(), "Database is not open");
  }
}

Napi::Value ForkDB::Open(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  
  if (is_open_) {
    throw Napi::Error::New(env, "Database is already open");
  }
  
  std::string path = path_;
  if (info.Length() > 0 && info[0].IsString()) {
    path = info[0].As<Napi::String>().Utf8Value();
  }
  
  if (path.empty()) {
    throw Napi::Error::New(env, "Path is required");
  }
  
  fkx_options options;
  fkx_options_default(&options);
  options.create = 1; // Allow creation
  
  int rc = fkx_open(path.c_str(), &options, &db_);
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to open database: ") + (db_ ? fkx_errmsg(db_) : "Unknown error"));
  }
  
  path_ = path;
  is_open_ = true;
  
  return env.Undefined();
}

Napi::Value ForkDB::Close(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  
  if (is_open_ && db_) {
    fkx_close(db_);
    db_ = nullptr;
    is_open_ = false;
  }
  
  return env.Undefined();
}

Napi::Object ForkDB::FKXInfoToObject(Napi::Env env, const fkx_info& info) {
  Napi::Object obj = Napi::Object::New(env);
  
  obj.Set("name", Napi::String::New(env, info.name));
  
  std::string kind;
  switch (info.kind) {
    case FKX_ROOT: kind = "root"; break;
    case FKX_FORK: kind = "fork"; break;
    case FKX_SNAPSHOT: kind = "snapshot"; break;
    case FKX_BASE: kind = "base"; break;
    case FKX_UNDO: kind = "undo"; break;
    default: kind = "unknown"; break;
  }
  obj.Set("kind", Napi::String::New(env, kind));
  
  obj.Set("readOnly", Napi::Boolean::New(env, info.read_only));
  
  char uid_str[17];
  snprintf(uid_str, sizeof(uid_str), "%016lx", (unsigned long)info.uid);
  obj.Set("uid", Napi::String::New(env, uid_str));
  
  snprintf(uid_str, sizeof(uid_str), "%016" PRIx64, info.origin_uid);
  obj.Set("originUid", Napi::String::New(env, uid_str));
  
  snprintf(uid_str, sizeof(uid_str), "%016" PRIx64, info.base_uid);
  obj.Set("baseUid", Napi::String::New(env, uid_str));
  
  snprintf(uid_str, sizeof(uid_str), "%016" PRIx64, info.owner_uid);
  obj.Set("ownerUid", Napi::String::New(env, uid_str));
  
  obj.Set("state", Napi::Number::New(env, info.state));
  obj.Set("sizePages", Napi::Number::New(env, info.size_pages));
  obj.Set("nUndo", Napi::Number::New(env, info.n_undo));
  obj.Set("openConnections", Napi::Number::New(env, info.open_connections));
  obj.Set("id", Napi::Number::New(env, info.id));
  
  return obj;
}

Napi::Value ForkDB::Fork(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    throw Napi::Error::New(env, "fork requires from and name parameters");
  }
  
  std::string from = info[0].As<Napi::String>().Utf8Value();
  std::string name = info[1].As<Napi::String>().Utf8Value();
  
  int rc = fkx_fork(db_, from.c_str(), name.c_str());
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to fork: ") + fkx_errmsg(db_));
  }
  
  return env.Undefined();
}

Napi::Value ForkDB::Snapshot(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    throw Napi::Error::New(env, "snapshot requires from and name parameters");
  }
  
  std::string from = info[0].As<Napi::String>().Utf8Value();
  std::string name = info[1].As<Napi::String>().Utf8Value();
  
  int rc = fkx_snapshot(db_, from.c_str(), name.c_str());
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to create snapshot: ") + fkx_errmsg(db_));
  }
  
  return env.Undefined();
}

Napi::Value ForkDB::Discard(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  if (info.Length() < 1 || !info[0].IsString()) {
    throw Napi::Error::New(env, "discard requires name parameter");
  }
  
  std::string name = info[0].As<Napi::String>().Utf8Value();
  
  int rc = fkx_discard(db_, name.c_str());
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to discard: ") + fkx_errmsg(db_));
  }
  
  return env.Undefined();
}

Napi::Value ForkDB::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    throw Napi::Error::New(env, "reset requires name and to parameters");
  }
  
  std::string name = info[0].As<Napi::String>().Utf8Value();
  std::string to = info[1].As<Napi::String>().Utf8Value();
  
  int rc = fkx_reset(db_, name.c_str(), to.c_str());
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to reset: ") + fkx_errmsg(db_));
  }
  
  return env.Undefined();
}

Napi::Value ForkDB::Info(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  if (info.Length() < 1 || !info[0].IsString()) {
    throw Napi::Error::New(env, "info requires ref parameter");
  }
  
  std::string ref = info[0].As<Napi::String>().Utf8Value();
  fkx_info info_struct;
  
  int rc = fkx_info_get(db_, ref.c_str(), &info_struct);
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to get info: ") + fkx_errmsg(db_));
  }
  
  return FKXInfoToObject(env, info_struct);
}

Napi::Value ForkDB::List(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  int include_internal = 0;
  if (info.Length() > 0 && info[0].IsBoolean()) {
    include_internal = info[0].As<Napi::Boolean>().Value();
  }
  
  size_t count = 0;
  int rc = fkx_list(db_, include_internal, NULL, 0, &count);
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to list branches: ") + fkx_errmsg(db_));
  }
  
  if (count == 0) {
    return Napi::Array::New(env, 0);
  }
  
  std::unique_ptr<fkx_info[]> infos(new fkx_info[count]);
  size_t actual_count = 0;
  rc = fkx_list(db_, include_internal, infos.get(), count, &actual_count);
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to list branches: ") + fkx_errmsg(db_));
  }
  
  Napi::Array result = Napi::Array::New(env, actual_count);
  for (size_t i = 0; i < actual_count; i++) {
    result.Set(i, FKXInfoToObject(env, infos[i]));
  }
  
  return result;
}

Napi::Value ForkDB::Promote(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  if (info.Length() < 1 || !info[0].IsString()) {
    throw Napi::Error::New(env, "promote requires name parameter");
  }
  
  std::string name = info[0].As<Napi::String>().Utf8Value();
  fkx_promote_opts opts = {};
  opts.into = nullptr;
  opts.dry_run = 0;
  
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object options = info[1].As<Napi::Object>();
    if (options.Has("into") && options.Get("into").IsString()) {
      std::string into = options.Get("into").As<Napi::String>().Utf8Value();
      opts.into = into.c_str();
    }
    if (options.Has("dryRun") && options.Get("dryRun").IsBoolean()) {
      opts.dry_run = options.Get("dryRun").As<Napi::Boolean>().Value();
    }
  }
  
  fkx_promote_result result = {};
  result.method = FKX_METHOD_NONE;
  result.dry_run = 0;
  result.conflicts = nullptr;
  result.nconflicts = 0;
  result.refusal = nullptr;
  result.undo_recorded = 0;
  int rc = fkx_promote(db_, name.c_str(), &opts, &result);
  
  Napi::Object result_obj = Napi::Object::New(env);
  
  std::string method;
  switch (result.method) {
    case FKX_METHOD_NONE: method = "none"; break;
    case FKX_METHOD_UP_TO_DATE: method = "up-to-date"; break;
    case FKX_METHOD_FAST_FORWARD: method = "fast-forward"; break;
    case FKX_METHOD_REPLAY: method = "replay"; break;
    default: method = "unknown"; break;
  }
  result_obj.Set("method", Napi::String::New(env, method));
  result_obj.Set("dryRun", Napi::Boolean::New(env, result.dry_run));
  result_obj.Set("undoRecorded", Napi::Boolean::New(env, result.undo_recorded));
  
  if (result.refusal) {
    result_obj.Set("refusal", Napi::String::New(env, result.refusal));
  }
  
  if (result.nconflicts > 0) {
    Napi::Array conflicts = Napi::Array::New(env, result.nconflicts);
    for (size_t i = 0; i < result.nconflicts; i++) {
      Napi::Object conflict = Napi::Object::New(env);
      conflict.Set("table", result.conflicts[i].table ? Napi::String::New(env, result.conflicts[i].table) : env.Null());
      conflict.Set("kind", Napi::Number::New(env, result.conflicts[i].kind));
      conflict.Set("op", Napi::Number::New(env, result.conflicts[i].op));
      conflict.Set("key", result.conflicts[i].key ? Napi::String::New(env, result.conflicts[i].key) : env.Null());
      conflicts.Set(i, conflict);
    }
    result_obj.Set("conflicts", conflicts);
  }
  
  fkx_promote_result_free(&result);
  
  if (rc != FKX_OK && rc != FKX_ECONFLICT) {
    throw Napi::Error::New(env, std::string("Failed to promote: ") + fkx_errmsg(db_));
  }
  
  return result_obj;
}

Napi::Value ForkDB::Undo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  if (info.Length() < 1 || !info[0].IsString()) {
    throw Napi::Error::New(env, "undo requires name parameter");
  }
  
  std::string name = info[0].As<Napi::String>().Utf8Value();
  fkx_promote_result result = {};
  result.method = FKX_METHOD_NONE;
  result.dry_run = 0;
  result.conflicts = nullptr;
  result.nconflicts = 0;
  result.refusal = nullptr;
  result.undo_recorded = 0;
  
  int rc = fkx_undo(db_, name.c_str(), &result);
  
  Napi::Object result_obj = Napi::Object::New(env);
  
  std::string method;
  switch (result.method) {
    case FKX_METHOD_NONE: method = "none"; break;
    case FKX_METHOD_UP_TO_DATE: method = "up-to-date"; break;
    case FKX_METHOD_FAST_FORWARD: method = "fast-forward"; break;
    case FKX_METHOD_REPLAY: method = "replay"; break;
    default: method = "unknown"; break;
  }
  result_obj.Set("method", Napi::String::New(env, method));
  
  if (result.refusal) {
    result_obj.Set("refusal", Napi::String::New(env, result.refusal));
  }
  
  fkx_promote_result_free(&result);
  
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to undo: ") + fkx_errmsg(db_));
  }
  
  return result_obj;
}

Napi::Value ForkDB::GC(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  fkx_gc_report report = {};
  report.orphan_bases = 0;
  report.orphan_undo = 0;
  report.unlisted_undo = 0;
  report.over_retention = 0;
  int rc = fkx_gc(db_, &report);
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to gc: ") + fkx_errmsg(db_));
  }
  
  Napi::Object result = Napi::Object::New(env);
  result.Set("orphanBases", Napi::Number::New(env, report.orphan_bases));
  result.Set("orphanUndo", Napi::Number::New(env, report.orphan_undo));
  result.Set("unlistedUndo", Napi::Number::New(env, report.unlisted_undo));
  result.Set("overRetention", Napi::Number::New(env, report.over_retention));
  
  uint32_t total = report.orphan_bases + report.orphan_undo + report.unlisted_undo + report.over_retention;
  result.Set("total", Napi::Number::New(env, total));
  
  return result;
}

Napi::Value ForkDB::Compact(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  CheckClosed();
  
  uint64_t min_bytes = 0;
  if (info.Length() > 0 && info[0].IsNumber()) {
    min_bytes = info[0].As<Napi::Number>().Int64Value();
  }
  
  int did = 0;
  int rc = fkx_compact_if_needed(db_, min_bytes, &did);
  if (rc != FKX_OK) {
    throw Napi::Error::New(env, std::string("Failed to compact: ") + fkx_errmsg(db_));
  }
  
  return Napi::Boolean::New(env, did);
}

// Module entry point
Napi::Object Init(Napi::Env env, Napi::Object exports) {
  return ForkDB::Init(env, exports);
}

NODE_API_MODULE(forkdb, Init)