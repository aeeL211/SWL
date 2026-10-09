#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>

#include "And64InlineHook.hpp"

#define TAG "SWLMods"
#define logI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define logE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

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
static void*   iapClass = nullptr;
static JavaVM* gVm      = nullptr;

static JNIEnv* attachEnv() {
  if (!gVm) return nullptr;
  JNIEnv* env = nullptr;
  jint r = gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (r == JNI_EDETACHED) {
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
  } else if (r != JNI_OK) return nullptr;
  return env;
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

static void toastOnMain(const char* msg) {
  JNIEnv* env = attachEnv();
  if (!env) return;

  jclass handlerCls = env->FindClass("android/os/Handler");
  if (!handlerCls) return;
  jclass looperCls = env->FindClass("android/os/Looper");
  if (!looperCls) return;

  jmethodID getMain = env->GetStaticMethodID(looperCls, "getMainLooper",
    "()Landroid/os/Looper;");
  if (!getMain) return;
  jobject looper = env->CallStaticObjectMethod(looperCls, getMain);
  if (!looper) return;

  jmethodID ctor = env->GetMethodID(handlerCls, "<init>", "(Landroid/os/Looper;)V");
  if (!ctor) return;
  jobject handler = env->NewObject(handlerCls, ctor, looper);
  if (!handler) return;

  jclass runnableCls = env->FindClass("java/lang/Runnable");
  if (!runnableCls) return;

  jstring jmsg = env->NewStringUTF(msg);

  jclass toastCls = env->FindClass("android/widget/Toast");
  jmethodID makeText = env->GetStaticMethodID(toastCls, "makeText",
    "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");

  jclass thread = env->FindClass("android/app/ActivityThread");
  jmethodID cur = env->GetStaticMethodID(thread, "currentApplication",
    "()Landroid/app/Application;");
  jobject app = env->CallStaticObjectMethod(thread, cur);

  if (!app || !makeText) return;

  jobject toast = env->CallStaticObjectMethod(toastCls, makeText, app, jmsg, 0);
  env->DeleteLocalRef(jmsg);
  if (!toast) return;
  jmethodID show = env->GetMethodID(toastCls, "show", "()V");

  // Bungkus dalam Runnable sederhana pakai Handler.post
  // Karena kita tidak bisa buat Runnable dari native dengan mudah,
  // kita pakai trick: post dengan objek Toast yang sudah dibuat
  // dan panggil show() melalui reflection di main thread.
  // Alternatif lebih sederhana: panggil show() langsung — Toast.show() aman
  // dari thread manapun pada API modern.
  if (show) env->CallVoidMethod(toast, show);
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
  // Filter thunk ARM64: ret (0xd65f03c0) atau b (opcode 000101...)
  if (first == 0xd65f03c0) return false;
  if ((first & 0xfc000000) == 0x14000000) return true; // b (biasanya thunk ke fungsi asli)
  return true;
}

static void scanTargets(void* klass) {
  void* iter = nullptr;
  void* m;
  while ((m = il.methodsOf(klass, &iter))) {
    const char* name = il.methodName(m);
    if (!name) continue;
    int argc = il.paramCount(m);
    void* addr = methodAddr(m);
    if (!looksValid(addr)) continue;

#define match(mname, args, field) \
    if (!tgt.field && strcmp(name, mname) == 0 && argc == (args)) { \
      tgt.field = addr; logI("[+] %s(%d) @ %p", mname, args, addr); continue; }

    match("InitiatePurchase",          1, initiate)
    match("DebugEditorPurchase",       1, debug)
    match("get_HasInfinityPack",       0, hasInfinity)
    match("get_ActualHasInfinityPack", 0, actualHasInfinity)
    match("get_HasDisabledAds",        0, hasDisabledAds)
    match("get_ActualHasDisabledAds",  0, actualHasDisabledAds)
    match("MayShowAnAd",               0, mayShowAd)
    match("Revoke",                    1, revokeWithBool)
    match("Revoke",                    0, revoke)
#undef match
  }
}

static void findIapClass() {
  void* domain = il.domainGet();
  if (!domain) { logE("Domain il2cpp belum siap"); return; }

  size_t count = 0;
  void** asms = il.getAssemblies(domain, &count);
  if (!asms) return;

  for (size_t i = 0; i < count && !iapClass; ++i) {
    void* image = il.imageOf(asms[i]);
    if (!image) continue;
    size_t classes = il.classCount(image);
    for (size_t j = 0; j < classes && !iapClass; ++j) {
      void* klass = il.classAt(image, j);
      if (!klass) continue;

      void* iter = nullptr;
      void* m;
      while ((m = il.methodsOf(klass, &iter))) {
        const char* name = il.methodName(m);
        if (name && strcmp(name, "InitiatePurchase") == 0 && il.paramCount(m) == 1) {
          iapClass = klass;
          logI("[+] Class IAP ditemukan");
          break;
        }
      }
    }
  }
}

typedef void (*VoidPtrFn)(void*);

static bool hookInitiate(void* productId) {
  if (tgt.debug)
    reinterpret_cast<VoidPtrFn>(tgt.debug)(productId);
  toastOnMain("MODS: Pembelian berhasil disimulasikan.");
  return true;
}

static bool retTrue()      { return true;  }
static bool retFalse()     { return false; }
static void noop()         {}
static void noopBool(bool) {}

static void installHooks() {
#define hook(field, fn) do { if (tgt.field) { \
    A64HookFunction(tgt.field, reinterpret_cast<void*>(fn), nullptr); \
    logI("[Hook] " #field " OK"); \
  } else logI("[Hook] " #field " dilewati (tidak ditemukan)"); } while (0)

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

  if (!resolveIl2cpp()) { logE("[Fatal] Gagal resolve il2cpp"); return nullptr; }

  // Tunggu domain il2cpp siap
  void* domain = nullptr;
  for (int i = 0; i < 60; ++i) {
    domain = il.domainGet();
    if (domain) break;
    sleep(1);
  }
  if (!domain) { logE("[Fatal] Domain il2cpp tidak siap"); return nullptr; }
  il.threadAttach(domain);

  findIapClass();
  if (!iapClass) { logE("[Fatal] Class IAP tidak ditemukan"); return nullptr; }

  scanTargets(iapClass);
  installHooks();

  toastOnMain("MODS: Inisialisasi berhasil.");
  logI("[Info] Inisialisasi berhasil.");
  return nullptr;
}

static void startWorker() {
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
  if (!gVm) startWorker();
}
