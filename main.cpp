#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <atomic>

#include "And64InlineHook.hpp"

#define TAG "SWLMods"

static void logWrite(const char* level, const char* fmt, ...)
  __attribute__((format(printf, 2, 3)));

#define logI(...) logWrite("I", __VA_ARGS__)
#define logE(...) logWrite("E", __VA_ARGS__)

struct Il2cpp {
  void*       handle;
  void*     (*domainGet)();
  void**    (*getAssemblies)(void* domain, size_t* count);
  void*     (*imageOf)(void* assembly);
  size_t    (*classCount)(void* image);
  void*     (*classAt)(void* image, size_t idx);
  void*     (*methodsOf)(void* klass, void** iter);
  const char* (*methodName)(void* method);
  int       (*paramCount)(void* method);
  void*     (*threadAttach)(void* domain);
  void*     (*methodGetPointer)(void* method);
};

struct Targets {
  void* initiate;
  void* debug;
  void* hasInfinity;
  void* actualHasInfinity;
  void* hasDisabledAds;
  void* actualHasDisabledAds;
  void* mayShowAd;
  void* revokeWithBool;
  void* revoke;
};

static Il2cpp  il       = {};
static Targets tgt      = {};
static JavaVM* gVm      = nullptr;

static std::atomic<bool> g_started{false};
static std::atomic<bool> g_initToastDone{false};

static JNIEnv* attachEnv() {
  if (!gVm) return nullptr;
  JNIEnv* env = nullptr;
  jint r = gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (r == JNI_EDETACHED) {
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
  } else if (r != JNI_OK) return nullptr;
  return env;
}

static FILE*     g_logFile = nullptr;
static std::mutex g_logMutex;
static char      g_logPath[512] = {0};

static FILE* openLogFile() {
  if (g_logFile) return g_logFile;

  JNIEnv* env = attachEnv();
  if (!env) return nullptr;

  jclass thread = env->FindClass("android/app/ActivityThread");
  if (!thread) return nullptr;

  jmethodID cur = env->GetStaticMethodID(thread, "currentApplication",
    "()Landroid/app/Application;");
  if (!cur) return nullptr;
  jobject app = env->CallStaticObjectMethod(thread, cur);
  if (!app) return nullptr;

  jclass ctxCls = env->FindClass("android/content/Context");
  if (!ctxCls) return nullptr;
  jmethodID getDir = env->GetMethodID(ctxCls, "getExternalFilesDir",
    "(Ljava/lang/String;)Ljava/io/File;");
  if (!getDir) return nullptr;

  jobject dir = env->CallObjectMethod(app, getDir, nullptr);
  if (!dir) return nullptr;

  jclass fileCls = env->FindClass("java/io/File");
  if (!fileCls) return nullptr;
  jmethodID getPath = env->GetMethodID(fileCls, "getAbsolutePath",
    "()Ljava/lang/String;");
  if (!getPath) return nullptr;

  jstring jpath = (jstring)env->CallObjectMethod(dir, getPath);
  if (!jpath) return nullptr;
  const char* cpath = env->GetStringUTFChars(jpath, nullptr);
  if (!cpath) return nullptr;

  snprintf(g_logPath, sizeof(g_logPath), "%s/swlmods.log", cpath);
  env->ReleaseStringUTFChars(jpath, cpath);

  g_logFile = fopen(g_logPath, "a");
  return g_logFile;
}

static void logWrite(const char* level, const char* fmt, ...) {
  std::lock_guard<std::mutex> lk(g_logMutex);

  FILE* f = openLogFile();
  if (!f) return;

  time_t now = time(nullptr);
  struct tm tmBuf;
  localtime_r(&now, &tmBuf);
  char ts[32];
  strftime(ts, sizeof(ts), "%H:%M:%S", &tmBuf);

  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  fprintf(f, "[%s] %s %s\n", ts, level, msg);
  fflush(f);
}

static void showToast(const char* msg) {
  JNIEnv* env = attachEnv();
  if (!env) return;

  jclass thread = env->FindClass("android/app/ActivityThread");
  if (!thread) return;

  jmethodID cur = env->GetStaticMethodID(thread, "currentApplication",
    "()Landroid/app/Application;");
  if (!cur) return;
  jobject app = env->CallStaticObjectMethod(thread, cur);
  if (!app) return;

  jclass toastCls = env->FindClass("android/widget/Toast");
  if (!toastCls) return;
  jmethodID mk = env->GetStaticMethodID(toastCls, "makeText",
    "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
  if (!mk) return;

  jstring jmsg = env->NewStringUTF(msg);
  jobject toast = env->CallStaticObjectMethod(toastCls, mk, app, jmsg, 0);
  env->DeleteLocalRef(jmsg);
  if (!toast) return;

  jmethodID show = env->GetMethodID(toastCls, "show", "()V");
  if (!show) return;
  env->CallVoidMethod(toast, show);
}

static void maybeShowInitToast() {
  if (g_initToastDone.exchange(true)) return;
  showToast("MODS: Inisialisasi berhasil.");
}

static bool resolveIl2cpp() {
  il.handle = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD);
  if (!il.handle) il.handle = dlopen("libil2cpp.so", RTLD_LAZY);
  if (!il.handle) { logE("libil2cpp.so tidak ditemukan"); return false; }

#define bind(field, name) do { \
    il.field = reinterpret_cast<decltype(il.field)>(dlsym(il.handle, name)); \
    if (!il.field) { logE("dlsym gagal: %s", name); return false; } \
  } while (0)

  bind(domainGet,     "il2cpp_domain_get");
  bind(getAssemblies, "il2cpp_domain_get_assemblies");
  bind(imageOf,       "il2cpp_assembly_get_image");
  bind(classCount,    "il2cpp_image_get_class_count");
  bind(classAt,       "il2cpp_image_get_class");
  bind(methodsOf,     "il2cpp_class_get_methods");
  bind(methodName,    "il2cpp_method_get_name");
  bind(paramCount,    "il2cpp_method_get_param_count");
  bind(threadAttach,  "il2cpp_thread_attach");
#undef bind

  il.methodGetPointer = reinterpret_cast<decltype(il.methodGetPointer)>(
    dlsym(il.handle, "il2cpp_method_get_pointer"));
  if (!il.methodGetPointer) logI("il2cpp_method_get_pointer tidak tersedia");
  return true;
}

static void* methodAddr(void* m) {
  if (il.methodGetPointer) {
    void* p = il.methodGetPointer(m);
    if (p) return p;
  }
  void* p0 = *reinterpret_cast<void**>(m);
  if (p0) return p0;
  return *reinterpret_cast<void**>(reinterpret_cast<char*>(m) + 8);
}

static bool looksValid(void* addr) {
  if (!addr) return false;
  Dl_info info;
  if (!dladdr(addr, &info) || !info.dli_fbase) return false;
  uint32_t first = *reinterpret_cast<uint32_t*>(addr);
  if (first == 0xd65f03c0) return false;
  return true;
}

static void scanAllTargets() {
  void* domain = il.domainGet();
  if (!domain) return;

  size_t count = 0;
  void** asms = il.getAssemblies(domain, &count);
  if (!asms) return;

  for (size_t i = 0; i < count; ++i) {
    void* image = il.imageOf(asms[i]);
    if (!image) continue;
    size_t classes = il.classCount(image);
    for (size_t j = 0; j < classes; ++j) {
      void* klass = il.classAt(image, j);
      if (!klass) continue;

      bool hasInfMarker = false;
      bool hasAdsMarker = false;

      void* iter = nullptr;
      void* m;
      while ((m = il.methodsOf(klass, &iter))) {
        const char* name = il.methodName(m);
        if (!name) continue;
        int argc = il.paramCount(m);
        void* addr = methodAddr(m);
        if (!looksValid(addr)) continue;

        if (!strcmp(name, "get_HasInfinityPack")) hasInfMarker = true;
        if (!strcmp(name, "get_HasDisabledAds"))  hasAdsMarker = true;

        if (!tgt.initiate && !strcmp(name, "InitiatePurchase") && argc == 1)
          tgt.initiate = addr;
        else if (!tgt.debug && !strcmp(name, "DebugEditorPurchase") && argc == 1)
          tgt.debug = addr;
        else if (!tgt.hasInfinity && !strcmp(name, "get_HasInfinityPack") && argc == 0)
          tgt.hasInfinity = addr;
        else if (!tgt.actualHasInfinity && !strcmp(name, "get_ActualHasInfinityPack") && argc == 0)
          tgt.actualHasInfinity = addr;
        else if (!tgt.hasDisabledAds && !strcmp(name, "get_HasDisabledAds") && argc == 0)
          tgt.hasDisabledAds = addr;
        else if (!tgt.actualHasDisabledAds && !strcmp(name, "get_ActualHasDisabledAds") && argc == 0)
          tgt.actualHasDisabledAds = addr;
        else if (!tgt.mayShowAd && !strcmp(name, "MayShowAnAd") && argc == 0)
          tgt.mayShowAd = addr;
      }

      if (hasInfMarker || hasAdsMarker) {
        void* iter2 = nullptr;
        while ((m = il.methodsOf(klass, &iter2))) {
          const char* name = il.methodName(m);
          if (!name || strcmp(name, "Revoke") != 0) continue;
          int argc = il.paramCount(m);
          void* addr = methodAddr(m);
          if (!looksValid(addr)) continue;

          if (!tgt.revokeWithBool && argc == 1) tgt.revokeWithBool = addr;
          if (!tgt.revoke && argc == 0)         tgt.revoke = addr;
        }
      }
    }
  }

  logI("[+] initiate             @ %p", tgt.initiate);
  logI("[+] debug                @ %p", tgt.debug);
  logI("[+] hasInfinity          @ %p", tgt.hasInfinity);
  logI("[+] actualHasInfinity    @ %p", tgt.actualHasInfinity);
  logI("[+] hasDisabledAds       @ %p", tgt.hasDisabledAds);
  logI("[+] actualHasDisabledAds @ %p", tgt.actualHasDisabledAds);
  logI("[+] mayShowAd            @ %p", tgt.mayShowAd);
  logI("[+] revokeWithBool       @ %p", tgt.revokeWithBool);
  logI("[+] revoke               @ %p", tgt.revoke);
}

typedef void (*VoidPtrFn)(void*);

static bool hookInitiate(void* productId) {
  maybeShowInitToast();
  if (tgt.debug)
    reinterpret_cast<VoidPtrFn>(tgt.debug)(productId);
  showToast("MODS: Pembelian berhasil disimulasikan.");
  return true;
}

static bool retTrue()      { maybeShowInitToast(); return true;  }
static bool retFalse()     { maybeShowInitToast(); return false; }
static void noop()         { maybeShowInitToast(); }
static void noopBool(bool) { maybeShowInitToast(); }

static void installHooks() {
#define hook(field, fn) do { if (tgt.field) { \
    A64HookFunction(tgt.field, reinterpret_cast<void*>(fn), nullptr); \
    logI("[Hook] " #field " OK"); \
  } else logI("[Hook] " #field " dilewati"); } while (0)

  hook(initiate,             hookInitiate);
  hook(hasInfinity,          retTrue);
  hook(actualHasInfinity,    retTrue);
  hook(hasDisabledAds,       retTrue);
  hook(actualHasDisabledAds, retTrue);
  hook(mayShowAd,            retFalse);
  hook(revokeWithBool,       noopBool);
  hook(revoke,               noop);
#undef hook
}

static void* worker(void*) {
  for (int i = 0; i < 120; ++i) {
    if (dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD)) break;
    sleep(1);
  }
  sleep(2);

  logI("Memulai inisialisasi, log: %s", g_logPath[0] ? g_logPath : "(belum tersedia)");

  if (!resolveIl2cpp()) { logE("[Fatal] Gagal resolve il2cpp"); return nullptr; }

  void* domain = nullptr;
  for (int i = 0; i < 60; ++i) {
    domain = il.domainGet();
    if (domain) break;
    sleep(1);
  }
  if (!domain) { logE("[Fatal] Domain il2cpp tidak siap"); return nullptr; }
  il.threadAttach(domain);

  scanAllTargets();
  installHooks();

  logI("[Info] Inisialisasi selesai.");
  return nullptr;
}

static void startWorker() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;
  pthread_t t;
  if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
}

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
  gVm = vm;
  startWorker();
  return JNI_VERSION_1_6;
}

__attribute__((constructor))
static void onLoad() {
  startWorker();
}
