#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <shadowhook.h>

// api il2cpp
struct Api {
  void* h;
  void* (*domain)();
  void** (*assemblies)(void*, size_t*);
  void* (*image)(void*);
  size_t (*classCount)(void*);
  void* (*classAt)(void*, size_t);
  void* (*methods)(void*, void**);
  const char* (*name)(void*);
  int (*argc)(void*);
  void* (*attach)(void*);
  void* (*ptr)(void*);  // opsional
};

static Api il = {};
static std::atomic<bool> started{false};
static std::atomic<JavaVM*> jvm{nullptr};

// jalankan fungsi di thread detached
static bool spawn(void* (*fn)(void*), void* arg) {
  pthread_t t;
  if (pthread_create(&t, nullptr, fn, arg) != 0) return false;
  pthread_detach(t);
  return true;
}

// tunggu kondisi sampai batas detik
template <class F>
static bool waitFor(int secs, F ok) {
  for (int i = 0; i < secs; ++i) {
    if (ok()) return true;
    sleep(1);
  }
  return false;
}

// ---- toast ----
// tiap toast jalan di thread native dengan looper sendiri

static JavaVM* getVm() {
  if (JavaVM* v = jvm.load()) return v;

  // lib dimuat tanpa JNI_OnLoad, cari sendiri
  void* sym = dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
  if (!sym) {
    void* art = shadowhook_dlopen("libart.so");
    if (art) sym = shadowhook_dlsym(art, "JNI_GetCreatedJavaVMs");
    if (art) shadowhook_dlclose(art);
  }
  if (!sym) return nullptr;

  JavaVM* v = nullptr;
  jsize n = 0;
  using Fn = jint (*)(JavaVM**, jsize, jsize*);
  if (reinterpret_cast<Fn>(sym)(&v, 1, &n) != JNI_OK || n < 1) return nullptr;
  jvm.store(v);
  return v;
}

// true jika ada exception jni (dibersihkan)
static bool bad(JNIEnv* env) {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionClear();
  return true;
}

// kembalikan v, atau nullptr jika jni melempar exception
template <class T>
static T chk(JNIEnv* env, T v) {
  return bad(env) ? nullptr : v;
}

// context aplikasi tanpa kelas aplikasi
static jobject appCtx(JNIEnv* env) {
  static const char* const src[2][2] = {
    {"android/app/ActivityThread", "currentApplication"},
    {"android/app/AppGlobals", "getInitialApplication"},
  };
  for (const auto& s : src) {
    jclass cls = chk(env, env->FindClass(s[0]));
    if (!cls) continue;
    jobject app = nullptr;
    jmethodID mid = chk(env, env->GetStaticMethodID(cls, s[1], "()Landroid/app/Application;"));
    if (mid) app = chk(env, env->CallStaticObjectMethod(cls, mid));
    env->DeleteLocalRef(cls);
    if (app) return app;
  }
  return nullptr;
}

// hentikan looper setelah toast selesai
static void* quitLater(void* arg) {
  jobject looper = static_cast<jobject>(arg);
  sleep(4);  // toast pendek ~2 detik + margin

  JavaVM* vm = getVm();
  JNIEnv* env = nullptr;
  if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;

  jmethodID quit = chk(env, env->GetMethodID(env->GetObjectClass(looper), "quit", "()V"));
  if (quit) env->CallVoidMethod(looper, quit);
  bad(env);
  env->DeleteGlobalRef(looper);
  vm->DetachCurrentThread();
  return nullptr;
}

static void runToast(JNIEnv* env, const char* msg) {
  jclass lp = chk(env, env->FindClass("android/os/Looper"));
  jclass ts = chk(env, env->FindClass("android/widget/Toast"));
  jobject ctx = appCtx(env);
  if (!lp || !ts || !ctx) return;

  jmethodID prep = chk(env, env->GetStaticMethodID(lp, "prepare", "()V"));
  jmethodID cur = chk(env, env->GetStaticMethodID(lp, "myLooper", "()Landroid/os/Looper;"));
  jmethodID loop = chk(env, env->GetStaticMethodID(lp, "loop", "()V"));
  jmethodID make = chk(env, env->GetStaticMethodID(ts, "makeText",
      "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;"));
  jmethodID show = chk(env, env->GetMethodID(ts, "show", "()V"));
  if (!prep || !cur || !loop || !make || !show) return;

  char buf[128];
  snprintf(buf, sizeof(buf), "[MODS] %s", msg);
  jstring text = chk(env, env->NewStringUTF(buf));
  if (!text) return;

  env->CallStaticVoidMethod(lp, prep);
  if (bad(env)) return;
  jobject looper = chk(env, env->CallStaticObjectMethod(lp, cur));
  jobject toast = chk(env, env->CallStaticObjectMethod(ts, make, ctx, text, 0));
  if (!looper || !toast) return;

  env->CallVoidMethod(toast, show);
  if (bad(env)) return;

  jobject ref = env->NewGlobalRef(looper);
  if (!spawn(quitLater, ref)) {
    env->DeleteGlobalRef(ref);
    return;
  }
  env->CallStaticVoidMethod(lp, loop);  // kembali setelah quit
  bad(env);
}

static void* toastMain(void* arg) {
  JavaVM* vm = getVm();
  JNIEnv* env = nullptr;
  if (vm && vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
    runToast(env, static_cast<const char*>(arg));
    vm->DetachCurrentThread();
  }
  return nullptr;
}

// msg harus string literal
static void toast(const char* msg) {
  spawn(toastMain, const_cast<char*>(msg));
}

// ---- il2cpp ----

template <class T>
static bool bindFn(T& f, const char* name) {
  f = reinterpret_cast<T>(dlsym(il.h, name));
  return f != nullptr;
}

static bool loadApi() {
  if (!il.h) il.h = dlopen("libil2cpp.so", RTLD_LAZY);
  if (!il.h) return false;

  bindFn(il.ptr, "il2cpp_method_get_pointer");
  return bindFn(il.domain, "il2cpp_domain_get") &&
         bindFn(il.assemblies, "il2cpp_domain_get_assemblies") &&
         bindFn(il.image, "il2cpp_assembly_get_image") &&
         bindFn(il.classCount, "il2cpp_image_get_class_count") &&
         bindFn(il.classAt, "il2cpp_image_get_class") &&
         bindFn(il.methods, "il2cpp_class_get_methods") &&
         bindFn(il.name, "il2cpp_method_get_name") &&
         bindFn(il.argc, "il2cpp_method_get_param_count") &&
         bindFn(il.attach, "il2cpp_thread_attach");
}

// alamat kode method
static void* codeAddr(void* m) {
  void* p = il.ptr ? il.ptr(m) : nullptr;
  if (p) return p;
  void** f = static_cast<void**>(m);  // fallback, aman 32/64-bit
  return f[0] ? f[0] : f[1];
}

static bool validAddr(void* a) {
  Dl_info info;
  if (!a || !dladdr(a, &info) || !info.dli_fbase) return false;
#if defined(__aarch64__)
  // lewati fungsi kosong (RET)
  if (*static_cast<uint32_t*>(a) == 0xd65f03c0) return false;
#endif
  // arm32 tidak dicek, opcode return beragam dan alamat thumb ber-bit-0
  return true;
}

// ---- hook ----

static void* debugFn = nullptr;

static bool buy(void* id) {
  if (debugFn) reinterpret_cast<void (*)(void*)>(debugFn)(id);
  toast("Simulasi pembelian berhasil");
  return true;
}

static bool retTrue() { return true; }
static bool retFalse() { return false; }
static void noop() {}
static void noopBool(bool) {}

template <class T>
static void* fp(T f) { return reinterpret_cast<void*>(f); }

struct Hook {
  const char* name;
  int argc;
  void* fn;  // pengganti, null = hanya dicari
  bool mark = false;  // penanda class target
  bool late = false;  // hanya diambil dari class yang punya penanda
  void* addr = nullptr;
};

static constexpr int kDbgFn = 1;  // indeks DebugEditorPurchase

static Hook hooks[] = {
  {"InitiatePurchase", 1, fp(buy)},
  {"DebugEditorPurchase", 1, nullptr},
  {"get_HasInfinityPack", 0, fp(retTrue), true},
  {"get_ActualHasInfinityPack", 0, fp(retTrue)},
  {"get_HasDisabledAds", 0, fp(retTrue), true},
  {"get_ActualHasDisabledAds", 0, fp(retTrue)},
  {"MayShowAnAd", 0, fp(retFalse)},
  {"Revoke", 1, fp(noopBool), false, true},
  {"Revoke", 0, fp(noop), false, true},
};

// cocokkan method dengan tabel, true jika method penanda
static bool take(void* m, bool late) {
  const char* name = il.name(m);
  if (!name) return false;

  for (Hook& h : hooks) {
    if (h.late != late || strcmp(h.name, name) != 0 || h.argc != il.argc(m)) continue;
    void* a = codeAddr(m);
    if (!validAddr(a)) return false;
    if (!h.addr) h.addr = a;
    return h.mark;
  }
  return false;
}

static void scanClass(void* cls) {
  void* it = nullptr;
  bool mark = false;
  while (void* m = il.methods(cls, &it)) mark |= take(m, false);
  if (!mark) return;

  // Revoke hanya diambil dari class penanda
  it = nullptr;
  while (void* m = il.methods(cls, &it)) take(m, true);
}

static void scan(void* dom) {
  size_t n = 0;
  void** as = il.assemblies(dom, &n);
  for (size_t i = 0; as && i < n; ++i) {
    void* img = il.image(as[i]);
    if (!img) continue;
    size_t cn = il.classCount(img);
    for (size_t j = 0; j < cn; ++j) {
      if (void* cls = il.classAt(img, j)) scanClass(cls);
    }
  }
}

// pasang hook (mode unique, orig tidak dipakai), return jumlah berhasil
// alamat thumb dari il2cpp sudah ber-bit-0, shadowhook membacanya sendiri
// target il2cpp yang digabung ke satu alamat: hook kedua ditolak, wajar
static int install() {
  int n = 0;
  for (const Hook& h : hooks) {
    if (h.fn && h.addr && shadowhook_hook_func_addr(h.addr, h.fn, nullptr)) ++n;
  }
  return n;
}

// ---- entry ----

// build debug (-DMODS_DEBUG=ON): toast per tahap dan log internal shadowhook
#ifdef MODS_DEBUG
static constexpr bool verbose = true;
#define dbg(msg) toast("debug: " msg)
#else
static constexpr bool verbose = false;
#define dbg(msg) ((void)0)
#endif

// jumlah target yang ketemu
static int found() {
  int n = 0;
  for (const Hook& h : hooks) n += (h.fn && h.addr) ? 1 : 0;
  return n;
}

static void* worker(void*) {
  if (shadowhook_init(SHADOWHOOK_MODE_UNIQUE, verbose) != 0) {
    dbg("shadowhook init gagal");
    return nullptr;
  }
  dbg("shadowhook siap");

  // tunggu libil2cpp dimuat
  if (!waitFor(120, [] { return (il.h = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD)) != nullptr; })) {
    dbg("libil2cpp belum termuat");
  }
  sleep(2);
  if (!loadApi()) {
    dbg("api il2cpp gagal");
    return nullptr;
  }

  // tunggu domain siap
  void* dom = nullptr;
  if (!waitFor(60, [&] { return (dom = il.domain()) != nullptr; })) {
    dbg("domain il2cpp gagal");
    return nullptr;
  }

  // scan ulang sampai target ketemu, assembly bisa belum siap
  il.attach(dom);
  waitFor(30, [&] { scan(dom); return found() > 0; });
  if (found() == 0) {
    dbg("target tidak ditemukan");
    return nullptr;
  }

  debugFn = hooks[kDbgFn].addr;
  if (install() > 0) {
    toast("Inisialisasi berhasil");
  } else {
    dbg("hook gagal dipasang");
  }
  return nullptr;
}

static void start() {
  if (!started.exchange(true)) spawn(worker, nullptr);
}

// visibility default karena build memakai -fvisibility=hidden
extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM* vm, void*) {
  jvm.store(vm);
  start();
  return JNI_VERSION_1_6;
}

__attribute__((constructor))
static void onLoad() {
  start();
}
