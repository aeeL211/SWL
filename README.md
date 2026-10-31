# SWL

Mod kecil untuk **Stick War: Legacy** yang berjalan di dalam proses game dan memakai
[ShadowHook](https://github.com/bytedance/android-inline-hook) untuk meng-hook method IL2CPP.
Tidak ada offset hardcoded: library memindai `libil2cpp.so` saat runtime, mencocokkan nama
class/method, lalu memasang hook.

> Repo ini hanya berisi library native. Loader/injector dan patcher APK tidak termasuk.

## Fitur

- Membuka Infinity Pack dan menonaktifkan iklan dengan memaksa getter terkait mengembalikan `true`.
- Menyembunyikan iklan (`MayShowAnAd` menjadi `false`).
- Simulasi pembelian: `InitiatePurchase` diarahkan ke `DebugEditorPurchase`.
- Mencegah langganan dicabut: `Revoke` dibuat no-op.
- Dukungan `arm64-v8a` dan `armeabi-v7a`.
- Log opsional ke logcat dengan tag `fbrian`.

## Cara kerja

1. `JNI_OnLoad` diekspor agar ART memakai simbol milik `libfbrian.so`, bukan milik
   `libshadowhook.so`.
2. Constructor memulai thread worker, lalu menunggu `libil2cpp.so` tersedia.
3. Worker mengikat API IL2CPP (`il2cpp_domain_get`, `il2cpp_class_get_methods`, dan lainnya),
   menunggu domain siap, lalu memindai seluruh assembly dan class.
4. Method yang namanya cocok dan tidak kosong dipasang hook lewat ShadowHook.
5. Daftar target ada di array `hooks` pada `main.cpp`.

## Build

Prasyarat:

- CMake 3.20 atau lebih baru
- Android NDK (workflow memakai r26b)
- ShadowHook AAR 2.0.1

```bash
export NDK=$ANDROID_NDK_HOME

curl -fsSLO https://repo1.maven.org/maven2/com/bytedance/android/shadowhook/2.0.1/shadowhook-2.0.1.aar
unzip -q shadowhook-2.0.1.aar -d shadowhook

cmake -S . -B build/arm64-v8a \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  -DSHADOWHOOK_ROOT="$PWD/shadowhook" \
  -DFB_LOG=ON

cmake --build build/arm64-v8a -j"$(nproc)"
```

Hasil build ada di `build/arm64-v8a/lib/arm64-v8a/libfbrian.so`. Untuk 32-bit, ganti ABI
menjadi `armeabi-v7a`.

ShadowHook 2.x membutuhkan `libshadowhook_nothing.so` di samping `libshadowhook.so`.
Workflow GitHub Actions menyalinnya otomatis ke folder artifact.

### GitHub Actions

Setiap push ke `main` atau run manual akan menghasilkan artifact:

- `libfbrian-arm64-v8a`
- `libfbrian-armeabi-v7a`
- `libfbrian-all` (gabungan)

Input `log` mematikan logcat untuk ukuran `.so` yang lebih kecil. Input `shadowhook` memilih
versi AAR; versi 1.1.1 bermasalah (error 12), gunakan 2.0.1 atau 1.0.10.

## Pemasangan

1. Ambil artifact dari GitHub Actions atau build sendiri.
2. Letakkan `libfbrian.so`, `libshadowhook.so`, dan `libshadowhook_nothing.so` (untuk 2.x)
   di folder library yang sama di dalam proses game.
3. Muat `libfbrian.so` dari loader/injector, atau tambahkan `System.loadLibrary("fbrian")`
   pada `UnityPlayerActivity` yang sudah dipatch.
4. Buka game, lalu cek log dengan `adb logcat -s fbrian`.

`libfbrian.so` memiliki `DT_NEEDED` ke `libshadowhook.so`, jadi keduanya harus berada di
folder yang sama. Jika hasil build juga membutuhkan `libc++_shared.so`, sertakan library
tersebut.

## Catatan

- Hanya untuk Stick War: Legacy; nama method bisa berubah di versi game lain.
- Gunakan di perangkat dan akun sendiri. Mod ini dapat melanggar ketentuan layanan game.
- Tidak ada jaminan; gunakan dengan risiko sendiri.
- Belum ada lisensi.
- Terima kasih kepada ShadowHook (ByteDance) dan API IL2CPP.
