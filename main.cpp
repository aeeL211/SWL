#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <atomic>

#include "And64InlineHook.hpp"

// struktur api il2cpp
struct Il2cppApi {
  void* handle;
  void* (*domainGet)();
  void** (*getAssemblies)(void* domain, size_t* count);
  void* (*imageOf)(void* assembly);
  size_t (*classCount)(void* image);
  void* (*classAt)(void* image, size_t idx);
  void* (*methodsOf)(void* klass, void** iter);
  const char* (*methodName)(void* method);
  int (*paramCount)(void* method);
  void* (*threadAttach)(void* domain);
  void* (*methodGetPointer)(void* method);
};

// struktur target fungsi untuk di-hook
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

static Il2cppApi il2cpp = {};
static Targets targets = {};
static std::atomic<bool> isStarted{false};

// resolve fungsi dari libil2cpp.so
static bool resolveApi() {
  il2cpp.handle = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD);
  if (!il2cpp.handle) il2cpp.handle = dlopen("libil2cpp.so", RTLD_LAZY);
  if (!il2cpp.handle) return false;

  #define BIND_API(field, name) \
    il2cpp.field = reinterpret_cast<decltype(il2cpp.field)>(dlsym(il2cpp.handle, name)); \
    if (!il2cpp.field) return false;

  BIND_API(domainGet, "il2cpp_domain_get");
  BIND_API(getAssemblies, "il2cpp_domain_get_assemblies");
  BIND_API(imageOf, "il2cpp_assembly_get_image");
  BIND_API(classCount, "il2cpp_image_get_class_count");
  BIND_API(classAt, "il2cpp_image_get_class");
  BIND_API(methodsOf, "il2cpp_class_get_methods");
  BIND_API(methodName, "il2cpp_method_get_name");
  BIND_API(paramCount, "il2cpp_method_get_param_count");
  BIND_API(threadAttach, "il2cpp_thread_attach");
  #undef BIND_API

  // opsional, tidak semua versi il2cpp memiliki ini
  il2cpp.methodGetPointer = reinterpret_cast<decltype(il2cpp.methodGetPointer)>(
    dlsym(il2cpp.handle, "il2cpp_method_get_pointer"));

  return true;
}

// dapatkan alamat pointer dari method il2cpp
static void* getMethodAddr(void* method) {
  if (il2cpp.methodGetPointer) {
    void* ptr = il2cpp.methodGetPointer(method);
    if (ptr) return ptr;
  }
  void* fallback = *reinterpret_cast<void**>(method);
  if (fallback) return fallback;
  return *reinterpret_cast<void**>(reinterpret_cast<char*>(method) + 8);
}

// validasi alamat memori
static bool isValidAddr(void* addr) {
  if (!addr) return false;
  Dl_info info;
  if (!dladdr(addr, &info) || !info.dli_fbase) return false;
  if (*reinterpret_cast<uint32_t*>(addr) == 0xd65f03c0) return false;
  return true;
}

// pindai semua class dan method untuk mencari target
static void scan() {
  void* domain = il2cpp.domainGet();
  if (!domain) return;

  size_t asmCount = 0;
  void** assemblies = il2cpp.getAssemblies(domain, &asmCount);
  if (!assemblies) return;

  for (size_t i = 0; i < asmCount; ++i) {
    void* image = il2cpp.imageOf(assemblies[i]);
    if (!image) continue;

    size_t classCount = il2cpp.classCount(image);
    for (size_t j = 0; j < classCount; ++j) {
      void* klass = il2cpp.classAt(image, j);
      if (!klass) continue;

      bool hasInfMarker = false;
      bool hasAdsMarker = false;
      void* iter = nullptr;
      void* method;

      while ((method = il2cpp.methodsOf(klass, &iter))) {
        const char* name = il2cpp.methodName(method);
        if (!name) continue;

        int argc = il2cpp.paramCount(method);
        void* addr = getMethodAddr(method);
        if (!isValidAddr(addr)) continue;

        if (!strcmp(name, "get_HasInfinityPack")) hasInfMarker = true;
        if (!strcmp(name, "get_HasDisabledAds")) hasAdsMarker = true;

        if (!targets.initiate && !strcmp(name, "InitiatePurchase") && argc == 1)
          targets.initiate = addr;
        else if (!targets.debug && !strcmp(name, "DebugEditorPurchase") && argc == 1)
          targets.debug = addr;
        else if (!targets.hasInfinity && !strcmp(name, "get_HasInfinityPack") && argc == 0)
          targets.hasInfinity = addr;
        else if (!targets.actualHasInfinity && !strcmp(name, "get_ActualHasInfinityPack") && argc == 0)
          targets.actualHasInfinity = addr;
        else if (!targets.hasDisabledAds && !strcmp(name, "get_HasDisabledAds") && argc == 0)
          targets.hasDisabledAds = addr;
        else if (!targets.actualHasDisabledAds && !strcmp(name, "get_ActualHasDisabledAds") && argc == 0)
          targets.actualHasDisabledAds = addr;
        else if (!targets.mayShowAd && !strcmp(name, "MayShowAnAd") && argc == 0)
          targets.mayShowAd = addr;
      }

      // cari fungsi revoke jika marker ditemukan di class ini
      if (hasInfMarker || hasAdsMarker) {
        void* iterRevoke = nullptr;
        while ((method = il2cpp.methodsOf(klass, &iterRevoke))) {
          const char* name = il2cpp.methodName(method);
          if (!name || strcmp(name, "Revoke") != 0) continue;
          
          int argc = il2cpp.paramCount(method);
          void* addr = getMethodAddr(method);
          if (!isValidAddr(addr)) continue;

          if (!targets.revokeWithBool && argc == 1) targets.revokeWithBool = addr;
          if (!targets.revoke && argc == 0) targets.revoke = addr;
        }
      }
    }
  }
}

// fungsi pengganti (hooks)
typedef void (*DebugFn)(void*);

static bool hookInitiate(void* productId) {
  if (targets.debug) {
    reinterpret_cast<DebugFn>(targets.debug)(productId);
  }
  return true;
}

static bool retTrue() { return true; }
static bool retFalse() { return false; }
static void noop() {}
static void noopBool(bool) {}

// pasang semua hook yang ditemukan
static void installHooks() {
  #define HOOK_TARGET(field, fn) \
    if (targets.field) A64HookFunction(targets.field, reinterpret_cast<void*>(fn), nullptr);

  HOOK_TARGET(initiate, hookInitiate);
  HOOK_TARGET(hasInfinity, retTrue);
  HOOK_TARGET(actualHasInfinity, retTrue);
  HOOK_TARGET(hasDisabledAds, retTrue);
  HOOK_TARGET(actualHasDisabledAds, retTrue);
  HOOK_TARGET(mayShowAd, retFalse);
  HOOK_TARGET(revokeWithBool, noopBool);
  HOOK_TARGET(revoke, noop);
  
  #undef HOOK_TARGET
}

// thread utama pekerja
static void* worker(void*) {
  // tunggu libil2cpp dimuat oleh game
  for (int i = 0; i < 120; ++i) {
    if (dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD)) break;
    sleep(1);
  }
  sleep(2);

  if (!resolveApi()) return nullptr;

  // tunggu domain il2cpp siap
  void* domain = nullptr;
  for (int i = 0; i < 60; ++i) {
    domain = il2cpp.domainGet();
    if (domain) break;
    sleep(1);
  }
  
  if (!domain) return nullptr;
  
  il2cpp.threadAttach(domain);
  scan();
  installHooks();

  return nullptr;
}

// mulai thread pekerja hanya sekali
static void start() {
  bool expected = false;
  if (!isStarted.compare_exchange_strong(expected, true)) return;
  
  pthread_t threadId;
  if (pthread_create(&threadId, nullptr, worker, nullptr) == 0) {
    pthread_detach(threadId);
  }
}

// entry point jni
extern "C" jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  start();
  return JNI_VERSION_1_6;
}

// entry point native constructor
__attribute__((constructor))
static void onNativeLoad() {
  start();
}
