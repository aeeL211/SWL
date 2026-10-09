#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>
#include <cstdint>

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
  void** slotInitiate;
  void*  fnDebug;
  void** slotHasInfinity;
  void** slotActualHasInfinity;
  void** slotHasDisabledAds;
  void** slotActualHasDisabledAds;
  void** slotMayShowAd;
  void** slotRevokeWithBool;
  void** slotRevoke;
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

static bool isExecPtr(void* p) {
  if (!p) return false;
  Dl_info info;
  return dladdr(p, &info) != 0;
}

static void** methodSlot(void* m) {
  void** s0 = reinterpret_cast<void**>(m);
  if (isExecPtr(*s0)) return s0;
  void** s8 = reinterpret_cast<void**>(reinterpret_cast<char*>(m) + 8);
  if (isExecPtr(*s8)) return s8;
  return nullptr;
}

static bool patchSlot(void** slot, void* fn) {
  uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~0xFFFULL;
  mprotect(reinterpret_cast<void*>(page), 0x2000,
           PROT_READ | PROT_WRITE | PROT_EXEC);
  *slot = fn;
  __builtin___clear_cache(reinterpret_cast<char*>(slot),
                          reinterpret_cast<char*>(slot) + sizeof(void*));
  return true;
}

static void scanTargets(void* klass) {
  void* iter = nullptr;
  void* m;
  while ((m = il.methodsOf(klass, &iter))) {
    const char* name = il.methodName(m);
    if (!name) continue;
    int argc = il.paramCount(m);
    void** slot = methodSlot(m);
    if (!slot) continue;

#define match(mname, args, field) \
    if (!tgt.field && strcmp(name, mname) == 0 && argc == (args)) { \
      tgt.field = slot; logI("[+] %s(%d) @ %p", mname, args, *slot); continue; }

    match("InitiatePurchase",          1, slotInitiate)
    match("DebugEditorPurchase",       1, fnDebug)
    match("get_HasInfinityPack",       0, slotHasInfinity)
    match("get_ActualHasInfinityPack", 0, slotActualHasInfinity)
    match("get_HasDisabledAds",        0, slotHasDisabledAds)
    match("get_ActualHasDisabledAds",  0, slotActualHasDisabledAds)
    match("MayShowAnAd",               0, slotMayShowAd)
    match("Revoke",                    1, slotRevokeWithBool)
    match("Revoke",                    0, slotRevoke)
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

static void hookInitiate(void* productId) {
  if (tgt.fnDebug)
    reinterpret_cast<VoidPtrFn>(tgt.fnDebug)(productId);
  showToast("MODS: Pembelian berhasil disimulasikan.");
}

static bool retTrue()      { return true;  }
static bool retFalse()     { return false; }
static void noop()         {}
static void noopBool(bool) {}

static void installHooks() {
#define swap(field, fn) do { if (tgt.field) { \
    patchSlot(tgt.field, reinterpret_cast<void*>(fn)); \
    logI("[Swap] " #field " OK"); } } while (0)

  swap(slotInitiate,          hookInitiate);
  swap(slotHasInfinity,       retTrue);
  swap(slotActualHasInfinity, retTrue);
  swap(slotHasDisabledAds,    retTrue);
  swap(slotActualHasDisabledAds, retTrue);
  swap(slotMayShowAd,         retFalse);
  swap(slotRevokeWithBool,    noopBool);
  swap(slotRevoke,            noop);
#undef swap
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
