#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>

#include "dobby.h"

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
};

struct Targets {
  void* initiatePurchase;
  void* debugPurchase;
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

static void showToast(const char* msg) {
  if (!gVm) return;
  JNIEnv* env = nullptr;
  bool attached = false;
  jint r = gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (r == JNI_EDETACHED) {
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    attached = true;
  } else if (r != JNI_OK) return;

  jclass thread = env->FindClass("android/app/ActivityThread");
  if (thread) {
    jmethodID cur = env->GetStaticMethodID(thread, "currentApplication",
      "()Landroid/app/Application;");
    jobject app = cur ? env->CallStaticObjectMethod(thread, cur) : nullptr;
    if (app) {
      jclass toastCls = env->FindClass("android/widget/Toast");
      if (toastCls) {
        jmethodID mk = env->GetStaticMethodID(toastCls, "makeText",
          "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
        if (mk) {
          jstring jmsg = env->NewStringUTF(msg);
          jobject toast = env->CallStaticObjectMethod(toastCls, mk, app, jmsg, 0);
          if (toast) {
            jmethodID show = env->GetMethodID(toastCls, "show", "()V");
            if (show) env->CallVoidMethod(toast, show);
          }
          env->DeleteLocalRef(jmsg);
        }
      }
    }
  }
  if (attached) gVm->DetachCurrentThread();
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
  return true;
}

static void* methodPtr(void* method) {
  void* p = *reinterpret_cast<void**>(method);
  if (p) return p;
  return *reinterpret_cast<void**>(reinterpret_cast<char*>(method) + 8);
}

static void scanTargets(void* klass) {
  void* iter = nullptr;
  void* m;
  while ((m = il.methodsOf(klass, &iter))) {
    const char* name = il.methodName(m);
    if (!name) continue;
    int argc = il.paramCount(m);
    void* ptr = methodPtr(m);
    if (!ptr) continue;

#define match(mname, args, field) \
    if (!tgt.field && strcmp(name, mname) == 0 && argc == (args)) { \
      tgt.field = ptr; logI("[+] %s(%d) @ %p", mname, args, ptr); continue; }

    match("InitiatePurchase",          1, initiatePurchase)
    match("DebugEditorPurchase",       1, debugPurchase)
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
  if (!domain) return;

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

static void* origInitiate = nullptr;

static void hookInitiate(void* productId) {
  if (tgt.debugPurchase)
    reinterpret_cast<VoidPtrFn>(tgt.debugPurchase)(productId);
  showToast("MODS: Pembelian berhasil disimulasikan.");
}

static bool retTrue()  { return true;  }
static bool retFalse() { return false; }
static void noop()     {}
static void noopBool(bool) {}

static void installHooks() {
  if (tgt.initiatePurchase) {
    DobbyHook(tgt.initiatePurchase,
              reinterpret_cast<void*>(hookInitiate),
              &origInitiate);
    logI("[Hook] InitiatePurchase OK");
  }
#define hook(field, fn) do { if (tgt.field) { \
    DobbyHook(tgt.field, reinterpret_cast<void*>(fn), nullptr); \
    logI("[Hook] " #field " OK"); } } while (0)

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

  void* domain = il.domainGet();
  if (domain) il.threadAttach(domain);

  findIapClass();
  if (!iapClass) { logE("[Fatal] Class IAP tidak ditemukan"); return nullptr; }

  scanTargets(iapClass);
  installHooks();

  showToast("MODS: Inisialisasi berhasil.");
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
