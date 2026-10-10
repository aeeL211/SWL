#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <shadowhook.h>

#if !defined(__aarch64__) && !defined(__arm__)
#error "Only arm64-v8a and armeabi-v7a are supported"
#endif

#ifdef FB_LOG
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "fbrian", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "fbrian", __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

#if defined(__arm__)
#define ISA(a) ((reinterpret_cast<uintptr_t>(a) & 1) ? "thumb" : "arm")
#else
#define ISA(a) "a64"
#endif

// il2cpp api
struct Api {
  void* (*domain)();
  void** (*assemblies)(void*, size_t*);
  void* (*image)(void*);
  size_t (*classCount)(void*);
  void* (*classAt)(void*, size_t);
  void* (*methods)(void*, void**);
  const char* (*name)(void*);
  int (*argc)(void*);
  void* (*attach)(void*);
  void* (*ptr)(void*);
};

// one target method and its replacement
struct Hook {
  const char* name;
  int argc;
  void* fn;            // null = lookup only
  bool mark = false;   // marks a class for the revoke scan
  bool revoke = false; // only searched in marked classes
  void* addr = nullptr;
  void* orig = nullptr;
};

static Api api = {};

// replacements
static bool retTrue() { return true; }
static bool retFalse() { return false; }
static void noop() {}

static void* debugAddr();

static bool hookInitiate(void* id) {
  if (void* f = debugAddr()) reinterpret_cast<void (*)(void*)>(f)(id);
  return true;
}

#define FN(f) reinterpret_cast<void*>(f)

static Hook hooks[] = {
  {"DebugEditorPurchase", 1, nullptr},
  {"InitiatePurchase", 1, FN(hookInitiate)},
  {"get_HasInfinityPack", 0, FN(retTrue), true},
  {"get_ActualHasInfinityPack", 0, FN(retTrue)},
  {"get_HasDisabledAds", 0, FN(retTrue), true},
  {"get_ActualHasDisabledAds", 0, FN(retTrue)},
  {"MayShowAnAd", 0, FN(retFalse)},
  {"Revoke", 1, FN(noop), false, true},
  {"Revoke", 0, FN(noop), false, true},
};

static int left = sizeof(hooks) / sizeof(hooks[0]);

static void* debugAddr() { return hooks[0].addr; }

static bool bindApi() {
  void* h = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD);
  if (!h) h = dlopen("libil2cpp.so", RTLD_LAZY);
  if (!h) return false;

  #define BIND(f, sym) \
    api.f = reinterpret_cast<decltype(api.f)>(dlsym(h, sym)); \
    if (!api.f) return false;

  BIND(domain, "il2cpp_domain_get");
  BIND(assemblies, "il2cpp_domain_get_assemblies");
  BIND(image, "il2cpp_assembly_get_image");
  BIND(classCount, "il2cpp_image_get_class_count");
  BIND(classAt, "il2cpp_image_get_class");
  BIND(methods, "il2cpp_class_get_methods");
  BIND(name, "il2cpp_method_get_name");
  BIND(argc, "il2cpp_method_get_param_count");
  BIND(attach, "il2cpp_thread_attach");
  #undef BIND

  // optional, not every il2cpp version has it
  api.ptr = reinterpret_cast<decltype(api.ptr)>(dlsym(h, "il2cpp_method_get_pointer"));
  return true;
}

// keep bit 0 as is, thumb functions need it set
static void* methodAddr(void* m) {
  if (api.ptr) {
    if (void* p = api.ptr(m)) return p;
  }
  void* p = *reinterpret_cast<void**>(m);
  // fallback: second pointer-sized field
  return p ? p : *reinterpret_cast<void**>(static_cast<char*>(m) + sizeof(void*));
}

// skip empty methods (first instruction is a return)
static bool isValid(void* a) {
  Dl_info info;
  if (!a || !dladdr(a, &info) || !info.dli_fbase) return false;
#if defined(__aarch64__)
  return *static_cast<uint32_t*>(a) != 0xd65f03c0;
#else
  auto p = reinterpret_cast<uintptr_t>(a);
  // thumb bx lr / arm bx lr
  if (p & 1) return *reinterpret_cast<uint16_t*>(p - 1) != 0x4770;
  return *reinterpret_cast<uint32_t*>(p) != 0xe12fff1e;
#endif
}

// fill matching hooks of one class, returns true if a marker method exists
static bool scanClass(void* cls, bool revoke) {
  bool marked = false;
  void* it = nullptr;
  while (void* m = api.methods(cls, &it)) {
    const char* n = api.name(m);
    if (!n) continue;
    for (Hook& h : hooks) {
      if (h.revoke != revoke || strcmp(n, h.name)) continue;
      void* a = methodAddr(m);
      if (!isValid(a)) continue;
      if (h.mark) marked = true;
      if (!h.addr && api.argc(m) == h.argc) {
        h.addr = a;
        left--;
      }
    }
  }
  return marked;
}

static void scan() {
  void* dom = api.domain();
  if (!dom) return;

  size_t n = 0;
  void** asms = api.assemblies(dom, &n);
  if (!asms) return;

  for (size_t i = 0; i < n; i++) {
    void* img = api.image(asms[i]);
    if (!img) continue;
    size_t count = api.classCount(img);
    for (size_t j = 0; j < count; j++) {
      void* cls = api.classAt(img, j);
      if (!cls) continue;
      if (scanClass(cls, false)) scanClass(cls, true);
      if (!left) return;
    }
  }
}

static bool initHook() {
  [[maybe_unused]] int e = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
  if (e) LOGE("init failed: %d %s", e, shadowhook_to_errmsg(e));
  return !e;
}

static void installHook(void* addr, void* fn, void** orig, [[maybe_unused]] const char* name) {
  if (shadowhook_hook_func_addr(addr, fn, orig)) {
    LOGI("%s ok at %zx (%s)", name, (size_t)addr, ISA(addr));
    return;
  }
  [[maybe_unused]] int e = shadowhook_get_errno();
  LOGE("%s failed at %zx (%s): %d %s", name, (size_t)addr, ISA(addr), e, shadowhook_to_errmsg(e));
}

static void installAll() {
  for (Hook& h : hooks) {
    if (!h.addr) LOGE("%s(%d) not found", h.name, h.argc);
    else if (h.fn) installHook(h.addr, h.fn, &h.orig, h.name);
  }
}

static void* worker(void*) {
  // init here, not in the constructor: it runs under the linker lock
  if (!initHook()) return nullptr;

  // wait for libil2cpp
  for (int i = 0; i < 120 && !dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD); i++) sleep(1);
  sleep(2);

  if (!bindApi()) {
    LOGE("il2cpp api not found");
    return nullptr;
  }

  // wait for il2cpp domain
  void* dom = nullptr;
  for (int i = 0; i < 60 && !(dom = api.domain()); i++) sleep(1);
  if (!dom) {
    LOGE("il2cpp domain not ready");
    return nullptr;
  }

  api.attach(dom);
  scan();
  installAll();
  return nullptr;
}

__attribute__((constructor))
static void onLoad() {
  pthread_t t;
  if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
}
