#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <shadowhook.h>

#ifdef MODS_DEBUG
#include <android/api-level.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <cstdarg>
#include <ctime>
#endif

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
  const char* (*className)(void*);  // hanya untuk log
  const char* (*imageName)(void*);  // hanya untuk log
};

static Api il = {};
static std::atomic<bool> started{false};
static std::atomic<JavaVM*> jvm{nullptr};

// build debug (-DMODS_DEBUG=ON): log ke file, toast per tahap, log internal shadowhook
// log: <external files dir>/fbrian.log (Android/data/<paket>/files)
#ifdef MODS_DEBUG
static constexpr bool verbose = true;
static void lg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void lgEx(JNIEnv* env);
#define dbg(msg) toast("debug: " msg)
#else
static constexpr bool verbose = false;
#define lg(...) ((void)0)
#define lgEx(env) ((void)0)
#define lgInit() ((void)0)
#define lgMaps() ((void)0)
#define lgBind() ((void)0)
#define lgImage(img, n) ((void)0)
#define lgProbe(cls, m) ((void)0)
#define lgTarget(name, argc, a) ((void)0)
#define lgMissing() ((void)0)
#define dbg(msg) ((void)0)
#endif

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
  lgEx(env);
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
    lg("appCtx %s.%s = %p", s[0], s[1], static_cast<void*>(app));
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
  if (!lp || !ts || !ctx) {
    lg("toast: gagal lp=%p ts=%p ctx=%p", static_cast<void*>(lp), static_cast<void*>(ts), static_cast<void*>(ctx));
    return;
  }

  jmethodID prep = chk(env, env->GetStaticMethodID(lp, "prepare", "()V"));
  jmethodID cur = chk(env, env->GetStaticMethodID(lp, "myLooper", "()Landroid/os/Looper;"));
  jmethodID loop = chk(env, env->GetStaticMethodID(lp, "loop", "()V"));
  jmethodID make = chk(env, env->GetStaticMethodID(ts, "makeText",
      "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;"));
  jmethodID show = chk(env, env->GetMethodID(ts, "show", "()V"));
  if (!prep || !cur || !loop || !make || !show) {
    lg("toast: method jni tidak ketemu");
    return;
  }

  char buf[128];
  snprintf(buf, sizeof(buf), "[MODS] %s", msg);
  jstring text = chk(env, env->NewStringUTF(buf));
  if (!text) return;

  env->CallStaticVoidMethod(lp, prep);
  if (bad(env)) {
    lg("toast: Looper.prepare gagal");
    return;
  }
  jobject looper = chk(env, env->CallStaticObjectMethod(lp, cur));
  jobject toast = chk(env, env->CallStaticObjectMethod(ts, make, ctx, text, 0));
  if (!looper || !toast) {
    lg("toast: makeText gagal looper=%p toast=%p", static_cast<void*>(looper), static_cast<void*>(toast));
    return;
  }

  env->CallVoidMethod(toast, show);
  if (bad(env)) {
    lg("toast: show gagal");
    return;
  }
  lg("toast: tampil \"%s\"", buf);

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
  } else {
    lg("toast: jvm=%p, attach gagal", static_cast<void*>(vm));
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
  if (!f) lg("sym tidak ada: %s", name);
  return f != nullptr;
}

static bool loadApi() {
  if (!il.h) il.h = dlopen("libil2cpp.so", RTLD_LAZY);
  if (!il.h) {
    lg("dlopen libil2cpp gagal: %s", dlerror());
    return false;
  }

  lgBind();
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

// jumlah target yang ketemu
static int found() {
  int n = 0;
  for (const Hook& h : hooks) n += (h.fn && h.addr) ? 1 : 0;
  return n;
}

// ---- log debug ----
#ifdef MODS_DEBUG

static char logPath[600];

static void lg(const char* fmt, ...) {
  if (!logPath[0]) return;
  static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&mu);
  if (FILE* f = fopen(logPath, "a")) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm t;
    localtime_r(&ts.tv_sec, &t);
    fprintf(f, "%02d:%02d:%02d.%03d [%ld] ", t.tm_hour, t.tm_min, t.tm_sec,
            static_cast<int>(ts.tv_nsec / 1000000), syscall(SYS_gettid));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
  }
  pthread_mutex_unlock(&mu);
}

// catat exception jni lalu bersihkan
static void lgEx(JNIEnv* env) {
  jthrowable ex = env->ExceptionOccurred();
  env->ExceptionClear();
  if (!ex) return;

  jclass c = env->FindClass("java/lang/Throwable");
  jmethodID m = c ? env->GetMethodID(c, "toString", "()Ljava/lang/String;") : nullptr;
  jstring s = m ? static_cast<jstring>(env->CallObjectMethod(ex, m)) : nullptr;
  const char* t = s ? env->GetStringUTFChars(s, nullptr) : nullptr;
  lg("jni exception: %s", t ? t : "?");
  if (t) env->ReleaseStringUTFChars(s, t);
  env->ExceptionClear();
}

// folder files eksternal dari Context
static bool dirFromJni(char* out, size_t n) {
  JavaVM* vm = getVm();
  JNIEnv* env = nullptr;
  if (!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;

  bool ok = false;
  if (jobject ctx = appCtx(env)) {
    jmethodID get = chk(env, env->GetMethodID(env->GetObjectClass(ctx), "getExternalFilesDir",
        "(Ljava/lang/String;)Ljava/io/File;"));
    jobject file = get ? chk(env, env->CallObjectMethod(ctx, get, static_cast<jstring>(nullptr))) : nullptr;
    jmethodID path = file ? chk(env, env->GetMethodID(env->GetObjectClass(file), "getAbsolutePath",
        "()Ljava/lang/String;")) : nullptr;
    jstring str = path ? static_cast<jstring>(chk(env, env->CallObjectMethod(file, path))) : nullptr;
    if (str) {
      const char* c = env->GetStringUTFChars(str, nullptr);
      snprintf(out, n, "%s", c);
      env->ReleaseStringUTFChars(str, c);
      ok = true;
    }
  }
  vm->DetachCurrentThread();
  return ok;
}

// fallback: nama paket dari nama proses
static void dirFromProc(char* out, size_t n) {
  char pkg[256] = {};
  if (FILE* f = fopen("/proc/self/cmdline", "r")) {
    size_t r = fread(pkg, 1, sizeof(pkg) - 1, f);
    pkg[r] = 0;
    fclose(f);
  }
  if (char* c = strchr(pkg, ':')) *c = 0;
  snprintf(out, n, "/storage/emulated/0/Android/data/%s/files", pkg);
}

static void mkdirs(const char* path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char* p = tmp + 1; *p; ++p) {
    if (*p != '/') continue;
    *p = 0;
    mkdir(tmp, 0775);
    *p = '/';
  }
  mkdir(tmp, 0775);
}

static void lgInit() {
  bool onLoad = jvm.load() != nullptr;
  char dir[2][512] = {};
  bool viaJni = dirFromJni(dir[0], sizeof(dir[0]));
  dirFromProc(dir[1], sizeof(dir[1]));

  for (int i = viaJni ? 0 : 1; i < 2; ++i) {
    mkdirs(dir[i]);
    snprintf(logPath, sizeof(logPath), "%s/fbrian.log", dir[i]);
    if (FILE* f = fopen(logPath, "w")) {  // mulai baru tiap proses
      fclose(f);
      break;
    }
    logPath[0] = 0;
  }
  if (!logPath[0]) {
    toast("debug: file log gagal dibuat");
    return;
  }

  lg("=== fbrian debug pid=%d arch=%s api=%d JNI_OnLoad=%d dirDariJni=%d", getpid(),
     sizeof(void*) == 8 ? "arm64" : "arm32", android_get_device_api_level(), onLoad, viaJni);
  lg("log: %s", logPath);
}

// daftar lib penting yang termuat
static void lgMaps() {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char line[640];
  while (fgets(line, sizeof(line), f)) {
    if (!strstr(line, "r-xp")) continue;
    if (!strstr(line, "libil2cpp") && !strstr(line, "libunity") && !strstr(line, "libmain") &&
        !strstr(line, "libfbrian") && !strstr(line, "libshadowhook")) continue;
    line[strcspn(line, "\n")] = 0;
    lg("maps: %s", line);
  }
  fclose(f);
}

// fungsi opsional untuk nama class dan image
static void lgBind() {
  bindFn(il.className, "il2cpp_class_get_name");
  bindFn(il.imageName, "il2cpp_image_get_name");
}

static void lgImage(void* img, size_t classes) {
  static int calls = 0;
  if (calls++ >= 300) return;
  lg("image %s classes=%zu", il.imageName ? il.imageName(img) : "?", classes);
}

// catat method yang namanya mirip target, untuk cek nama yang berubah
static void lgProbe(void* cls, void* m) {
  static int calls = 0;
  const char* n = il.name(m);
  if (!n || calls >= 300) return;

  static const char* const keys[] = {"Purchase", "Infinity", "DisabledAds", "Revoke", "ShowAnAd"};
  for (const char* k : keys) {
    if (!strstr(n, k)) continue;
    ++calls;
    lg("probe: %s::%s argc=%d", il.className ? il.className(cls) : "?", n, il.argc(m));
    return;
  }
}

static void lgTarget(const char* name, int argc, void* a) {
  Dl_info i;
  if (dladdr(a, &i) && i.dli_fbase) {
    lg("target %s/%d @%p (%s+0x%zx)", name, argc, a, i.dli_fname,
       reinterpret_cast<uintptr_t>(a) - reinterpret_cast<uintptr_t>(i.dli_fbase));
  } else {
    lg("target %s/%d @%p", name, argc, a);
  }
}

static void lgMissing() {
  for (const Hook& h : hooks) {
    if (h.fn && !h.addr) lg("tidak ketemu: %s/%d", h.name, h.argc);
  }
}

#endif  // MODS_DEBUG

// ---- scan ----

// cocokkan method dengan tabel, true jika method penanda
static bool take(void* m, bool late) {
  const char* name = il.name(m);
  if (!name) return false;

  for (Hook& h : hooks) {
    if (h.late != late || strcmp(h.name, name) != 0 || h.argc != il.argc(m)) continue;
    void* a = codeAddr(m);
    if (!validAddr(a)) {
      lg("tolak %s/%d @%p", name, h.argc, a);
      return false;
    }
    if (!h.addr) {
      h.addr = a;
      lgTarget(h.name, h.argc, a);
    }
    return h.mark;
  }
  return false;
}

static void scanClass(void* cls) {
  void* it = nullptr;
  bool mark = false;
  while (void* m = il.methods(cls, &it)) {
    mark |= take(m, false);
    lgProbe(cls, m);
  }
  if (!mark) return;

  // Revoke hanya diambil dari class penanda
  it = nullptr;
  while (void* m = il.methods(cls, &it)) take(m, true);
}

static void scan(void* dom) {
  size_t n = 0;
  void** as = il.assemblies(dom, &n);
  lg("scan: assemblies=%zu", n);
  for (size_t i = 0; as && i < n; ++i) {
    void* img = il.image(as[i]);
    if (!img) continue;
    size_t cn = il.classCount(img);
    lgImage(img, cn);
    for (size_t j = 0; j < cn; ++j) {
      if (void* cls = il.classAt(img, j)) scanClass(cls);
    }
  }
  lg("scan: ketemu=%d", found());
}

// pasang hook (mode unique, orig tidak dipakai), return jumlah berhasil
// alamat thumb dari il2cpp sudah ber-bit-0, shadowhook membacanya sendiri
// target il2cpp yang digabung ke satu alamat: hook kedua ditolak, wajar
static int install() {
  int n = 0;
  for (const Hook& h : hooks) {
    if (!h.fn || !h.addr) continue;
    void* stub = shadowhook_hook_func_addr(h.addr, h.fn, nullptr);
    lg("hook %s/%d @%p -> %s", h.name, h.argc, h.addr,
       stub ? "ok" : shadowhook_to_errmsg(shadowhook_get_errno()));
    if (stub) ++n;
  }
  return n;
}

// ---- entry ----

static void* worker(void*) {
  lgInit();
  lgMaps();

  int r = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, verbose);
  lg("shadowhook_init=%d (%s) jvm=%p", r, shadowhook_to_errmsg(r), static_cast<void*>(getVm()));
  if (r != 0) {
    dbg("shadowhook init gagal");
    return nullptr;
  }
  dbg("shadowhook siap");

  // tunggu libil2cpp dimuat
  bool lib = waitFor(120, [] { return (il.h = dlopen("libil2cpp.so", RTLD_LAZY | RTLD_NOLOAD)) != nullptr; });
  lg("libil2cpp termuat=%d handle=%p", lib, il.h);
  if (!lib) dbg("libil2cpp belum termuat");
  sleep(2);
  lgMaps();
  if (!loadApi()) {
    dbg("api il2cpp gagal");
    return nullptr;
  }

  // tunggu domain siap
  void* dom = nullptr;
  bool domOk = waitFor(60, [&] { return (dom = il.domain()) != nullptr; });
  lg("domain=%p", dom);
  if (!domOk) {
    dbg("domain il2cpp gagal");
    return nullptr;
  }

  // scan ulang sampai target ketemu, assembly bisa belum siap
  il.attach(dom);
  waitFor(30, [&] { scan(dom); return found() > 0; });
  lgMissing();
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
