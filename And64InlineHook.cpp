#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#if defined(__aarch64__)
#include "And64InlineHook.hpp"
#define A64_MAX_INSTRUCTIONS 5
#define A64_MAX_REFERENCES (A64_MAX_INSTRUCTIONS * 2)
#define A64_NOP 0xd503201fu
#define A64_JNIEXPORT __attribute__((visibility("default")))
#define A64_LOGE(...) ((void)0)
#define A64_LOGI(...) ((void)0)
typedef uint32_t *__restrict *__restrict instruction;
struct context {
  struct fix_info {
    uint32_t *bp;
    uint32_t ls;
    uint32_t ad;
  };
  struct insns_info {
    union {
      uint64_t insu;
      int64_t ins;
      void *insp;
    };
    fix_info fmap[A64_MAX_REFERENCES];
  };
  int64_t basep;
  int64_t endp;
  insns_info dat[A64_MAX_INSTRUCTIONS];
public:
  inline bool is_in_fixing_range(const int64_t absolute_addr) {
    return absolute_addr >= this->basep && absolute_addr < this->endp;
  }
  inline intptr_t get_ref_ins_index(const int64_t absolute_addr) {
    return static_cast<intptr_t>((absolute_addr - this->basep) / sizeof(uint32_t));
  }
  inline intptr_t get_and_set_current_index(uint32_t *__restrict inp, uint32_t *__restrict outp) {
    intptr_t current_idx = this->get_ref_ins_index(reinterpret_cast<int64_t>(inp));
    this->dat[current_idx].insp = outp;
    return current_idx;
  }
  inline void reset_current_ins(const intptr_t idx, uint32_t *__restrict outp) {
    this->dat[idx].insp = outp;
  }
  void insert_fix_map(const intptr_t idx, uint32_t *bp, uint32_t ls = 0u, uint32_t ad = 0xffffffffu) {
    for (auto &f : this->dat[idx].fmap) {
      if (f.bp == NULL) {
        f.bp = bp;
        f.ls = ls;
        f.ad = ad;
        return;
      }
    }
  }
  void process_fix_map(const intptr_t idx) {
    for (auto &f : this->dat[idx].fmap) {
      if (f.bp == NULL) break;
      *(f.bp) = *(f.bp) | (((int32_t(this->dat[idx].ins - reinterpret_cast<int64_t>(f.bp)) >> 2) << f.ls) & f.ad);
      f.bp = NULL;
    }
  }
};
#define __intval(p) reinterpret_cast<intptr_t>(p)
#define __uintval(p) reinterpret_cast<uintptr_t>(p)
#define __ptr(p) reinterpret_cast<void *>(p)
#define __page_size 4096
#define __page_align(n) __align_up(static_cast<uintptr_t>(n), __page_size)
#define __ptr_align(x) __ptr(__align_down(reinterpret_cast<uintptr_t>(x), __page_size))
#define __align_up(x, n) (((x) + ((n) - 1)) & ~((n) - 1))
#define __align_down(x, n) ((x) & -(n))
#define __countof(x) static_cast<intptr_t>(sizeof(x) / sizeof((x)[0]))
#define __atomic_increase(p) __sync_add_and_fetch(p, 1)
#define __sync_cmpswap(p, v, n) __sync_bool_compare_and_swap(p, v, n)
#define __predict_true(exp) __builtin_expect((exp) != 0, 1)
#define __flush_cache(c, n) __builtin___clear_cache(reinterpret_cast<char *>(c), reinterpret_cast<char *>(c) + n)
#define __make_rwx(p, n) ::mprotect(__ptr_align(p), \
  __page_align(__uintval(p) + n) != __page_align(__uintval(p)) ? __page_align(n) + __page_size : __page_align(n), \
  PROT_READ | PROT_WRITE | PROT_EXEC)
static bool __fix_branch_imm(instruction inpp, instruction outpp, context *ctxp) {
  static constexpr uint32_t mbits = 6u;
  static constexpr uint32_t mask = 0xfc000000u;
  static constexpr uint32_t rmask = 0x03ffffffu;
  static constexpr uint32_t op_b = 0x14000000u;
  static constexpr uint32_t op_bl = 0x94000000u;
  const uint32_t ins = *(*inpp);
  const uint32_t opc = ins & mask;
  switch (opc) {
    case op_b:
    case op_bl: {
      intptr_t current_idx = ctxp->get_and_set_current_index(*inpp, *outpp);
      int64_t absolute_addr = reinterpret_cast<int64_t>(*inpp) + (static_cast<int32_t>(ins << mbits) >> (mbits - 2u));
      int64_t new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp)) >> 2;
      bool special_fix_type = ctxp->is_in_fixing_range(absolute_addr);
      if (!special_fix_type && llabs(new_pc_offset) >= (rmask >> 1)) {
        bool b_aligned = (reinterpret_cast<uint64_t>(*outpp + 2) & 7u) == 0u;
        if (opc == op_b) {
          if (b_aligned != true) {
            (*outpp)[0] = A64_NOP;
            ctxp->reset_current_ins(current_idx, ++(*outpp));
          }
          (*outpp)[0] = 0x58000051u;
          (*outpp)[1] = 0xd61f0220u;
          memcpy(*outpp + 2, &absolute_addr, sizeof(absolute_addr));
          *outpp += 4;
        } else {
          if (b_aligned == true) {
            (*outpp)[0] = A64_NOP;
            ctxp->reset_current_ins(current_idx, ++(*outpp));
          }
          (*outpp)[0] = 0x58000071u;
          (*outpp)[1] = 0x1000009eu;
          (*outpp)[2] = 0xd61f0220u;
          memcpy(*outpp + 3, &absolute_addr, sizeof(absolute_addr));
          *outpp += 5;
        }
      } else {
        if (special_fix_type) {
          intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr);
          if (ref_idx <= current_idx) {
            new_pc_offset = static_cast<int64_t>(ctxp->dat[ref_idx].ins - reinterpret_cast<int64_t>(*outpp)) >> 2;
          } else {
            ctxp->insert_fix_map(ref_idx, *outpp, 0u, rmask);
            new_pc_offset = 0;
          }
        }
        (*outpp)[0] = opc | (new_pc_offset & ~mask);
        ++(*outpp);
      }
      ++(*inpp);
      return ctxp->process_fix_map(current_idx), true;
    }
  }
  return false;
}
static bool __fix_cond_comp_test_branch(instruction inpp, instruction outpp, context *ctxp) {
  static constexpr uint32_t lsb = 5u;
  static constexpr uint32_t lmask01 = 0xff00001fu;
  static constexpr uint32_t mask0 = 0xff000010u;
  static constexpr uint32_t op_bc = 0x54000000u;
  static constexpr uint32_t mask1 = 0x7f000000u;
  static constexpr uint32_t op_cbz = 0x34000000u;
  static constexpr uint32_t op_cbnz = 0x35000000u;
  static constexpr uint32_t lmask2 = 0xfff8001fu;
  static constexpr uint32_t mask2 = 0x7f000000u;
  static constexpr uint32_t op_tbz = 0x36000000u;
  static constexpr uint32_t op_tbnz = 0x37000000u;
  const uint32_t ins = *(*inpp);
  uint32_t lmask = lmask01;
  if ((ins & mask0) != op_bc) {
    uint32_t opc = ins & mask1;
    if (opc != op_cbz && opc != op_cbnz) {
      opc = ins & mask2;
      if (opc != op_tbz && opc != op_tbnz) {
        return false;
      }
      lmask = lmask2;
    }
  }
  const uint32_t msb = __builtin_clz(~lmask);
  intptr_t current_idx = ctxp->get_and_set_current_index(*inpp, *outpp);
  int64_t absolute_addr = reinterpret_cast<int64_t>(*inpp) + (static_cast<int32_t>((ins & ~lmask) << msb) >> (lsb - 2u + msb));
  int64_t new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp)) >> 2;
  bool special_fix_type = ctxp->is_in_fixing_range(absolute_addr);
  if (!special_fix_type && llabs(new_pc_offset) >= (~lmask >> (lsb + 1))) {
    if ((reinterpret_cast<uint64_t>(*outpp + 4) & 7u) != 0u) {
      (*outpp)[0] = A64_NOP;
      ctxp->reset_current_ins(current_idx, ++(*outpp));
    }
    (*outpp)[0] = (((8u >> 2u) << lsb) & ~lmask) | (ins & lmask);
    (*outpp)[1] = 0x14000005u;
    (*outpp)[2] = 0x58000051u;
    (*outpp)[3] = 0xd61f0220u;
    memcpy(*outpp + 4, &absolute_addr, sizeof(absolute_addr));
    *outpp += 6;
  } else {
    if (special_fix_type) {
      intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr);
      if (ref_idx <= current_idx) {
        new_pc_offset = static_cast<int64_t>(ctxp->dat[ref_idx].ins - reinterpret_cast<int64_t>(*outpp)) >> 2;
      } else {
        ctxp->insert_fix_map(ref_idx, *outpp, lsb, ~lmask);
        new_pc_offset = 0;
      }
    }
    (*outpp)[0] = (static_cast<uint32_t>(new_pc_offset << lsb) & ~lmask) | (ins & lmask);
    ++(*outpp);
  }
  ++(*inpp);
  return ctxp->process_fix_map(current_idx), true;
}
static bool __fix_loadlit(instruction inpp, instruction outpp, context *ctxp) {
  const uint32_t ins = *(*inpp);
  if ((ins & 0xff000000u) == 0xd8000000u) {
    ctxp->process_fix_map(ctxp->get_and_set_current_index(*inpp, *outpp));
    ++(*inpp);
    return true;
  }
  static constexpr uint32_t msb = 8u;
  static constexpr uint32_t lsb = 5u;
  static constexpr uint32_t mask_30 = 0x40000000u;
  static constexpr uint32_t mask_31 = 0x80000000u;
  static constexpr uint32_t lmask = 0xff00001fu;
  static constexpr uint32_t mask_ldr = 0xbf000000u;
  static constexpr uint32_t op_ldr = 0x18000000u;
  static constexpr uint32_t mask_ldrv = 0x3f000000u;
  static constexpr uint32_t op_ldrv = 0x1c000000u;
  static constexpr uint32_t mask_ldrsw = 0xff000000u;
  static constexpr uint32_t op_ldrsw = 0x98000000u;
  uint32_t mask = mask_ldr;
  uintptr_t faligned = (ins & mask_30) ? 7u : 3u;
  if ((ins & mask_ldr) != op_ldr) {
    mask = mask_ldrv;
    if (faligned != 7u)
      faligned = (ins & mask_31) ? 15u : 3u;
    if ((ins & mask_ldrv) != op_ldrv) {
      if ((ins & mask_ldrsw) != op_ldrsw) {
        return false;
      }
      mask = mask_ldrsw;
      faligned = 7u;
    }
  }
  intptr_t current_idx = ctxp->get_and_set_current_index(*inpp, *outpp);
  int64_t absolute_addr = reinterpret_cast<int64_t>(*inpp) + ((static_cast<int32_t>(ins << msb) >> (msb + lsb - 2u)) & ~3u);
  int64_t new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp)) >> 2;
  bool special_fix_type = ctxp->is_in_fixing_range(absolute_addr);
  if (special_fix_type || (llabs(new_pc_offset) + (faligned + 1u - 4u) / 4u) >= (~lmask >> (lsb + 1))) {
    while ((reinterpret_cast<uint64_t>(*outpp + 2) & faligned) != 0u) {
      *(*outpp)++ = A64_NOP;
    }
    ctxp->reset_current_ins(current_idx, *outpp);
    uint32_t ns = static_cast<uint32_t>((faligned + 1) / sizeof(uint32_t));
    (*outpp)[0] = (((8u >> 2u) << lsb) & ~mask) | (ins & lmask);
    (*outpp)[1] = 0x14000001u + ns;
    memcpy(*outpp + 2, reinterpret_cast<void *>(absolute_addr), faligned + 1);
    *outpp += 2 + ns;
  } else {
    faligned >>= 2;
    while ((new_pc_offset & faligned) != 0) {
      *(*outpp)++ = A64_NOP;
      new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp)) >> 2;
    }
    ctxp->reset_current_ins(current_idx, *outpp);
    (*outpp)[0] = (static_cast<uint32_t>(new_pc_offset << lsb) & ~mask) | (ins & lmask);
    ++(*outpp);
  }
  ++(*inpp);
  return ctxp->process_fix_map(current_idx), true;
}
static bool __fix_pcreladdr(instruction inpp, instruction outpp, context *ctxp) {
  static constexpr uint32_t msb = 8u;
  static constexpr uint32_t lsb = 5u;
  static constexpr uint32_t mask = 0x9f000000u;
  static constexpr uint32_t rmask = 0x0000001fu;
  static constexpr uint32_t lmask = 0xff00001fu;
  static constexpr uint32_t fmask = 0x00ffffffu;
  static constexpr uint32_t max_val = 0x001fffffu;
  static constexpr uint32_t op_adr = 0x10000000u;
  static constexpr uint32_t op_adrp = 0x90000000u;
  const uint32_t ins = *(*inpp);
  intptr_t current_idx;
  switch (ins & mask) {
    case op_adr: {
      current_idx = ctxp->get_and_set_current_index(*inpp, *outpp);
      int64_t lsb_bytes = static_cast<uint32_t>(ins << 1u) >> 30u;
      int64_t absolute_addr = reinterpret_cast<int64_t>(*inpp) + (((static_cast<int32_t>(ins << msb) >> (msb + lsb - 2u)) & ~3u) | lsb_bytes);
      int64_t new_pc_offset = static_cast<int64_t>(absolute_addr - reinterpret_cast<int64_t>(*outpp));
      bool special_fix_type = ctxp->is_in_fixing_range(absolute_addr);
      if (!special_fix_type && llabs(new_pc_offset) >= (max_val >> 1)) {
        if ((reinterpret_cast<uint64_t>(*outpp + 2) & 7u) != 0u) {
          (*outpp)[0] = A64_NOP;
          ctxp->reset_current_ins(current_idx, ++(*outpp));
        }
        (*outpp)[0] = 0x58000000u | (((8u >> 2u) << lsb) & ~mask) | (ins & rmask);
        (*outpp)[1] = 0x14000003u;
        memcpy(*outpp + 2, &absolute_addr, sizeof(absolute_addr));
        *outpp += 4;
      } else {
        if (special_fix_type) {
          intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr & ~3ull);
          if (ref_idx <= current_idx) {
            new_pc_offset = static_cast<int64_t>(ctxp->dat[ref_idx].ins - reinterpret_cast<int64_t>(*outpp));
          } else {
            ctxp->insert_fix_map(ref_idx, *outpp, lsb, fmask);
            new_pc_offset = 0;
          }
        }
        (*outpp)[0] = (static_cast<uint32_t>(new_pc_offset << (lsb - 2u)) & fmask) | (ins & lmask);
        ++(*outpp);
      }
    }
    break;
    case op_adrp: {
      current_idx = ctxp->get_and_set_current_index(*inpp, *outpp);
      int32_t lsb_bytes = static_cast<uint32_t>(ins << 1u) >> 30u;
      int64_t absolute_addr = (reinterpret_cast<int64_t>(*inpp) & ~0xfffll) + ((((static_cast<int32_t>(ins << msb) >> (msb + lsb - 2u)) & ~3u) | lsb_bytes) << 12);
      if (ctxp->is_in_fixing_range(absolute_addr)) {
        intptr_t ref_idx = ctxp->get_ref_ins_index(absolute_addr);
        *(*outpp)++ = ins;
      } else {
        if ((reinterpret_cast<uint64_t>(*outpp + 2) & 7u) != 0u) {
          (*outpp)[0] = A64_NOP;
          ctxp->reset_current_ins(current_idx, ++(*outpp));
        }
        (*outpp)[0] = 0x58000000u | (((8u >> 2u) << lsb) & ~mask) | (ins & rmask);
        (*outpp)[1] = 0x14000003u;
        memcpy(*outpp + 2, &absolute_addr, sizeof(absolute_addr));
        *outpp += 4;
      }
    }
    break;
    default:
      return false;
  }
  ctxp->process_fix_map(current_idx);
  ++(*inpp);
  return true;
}
static void __fix_instructions(uint32_t *__restrict inp, int32_t count, uint32_t *__restrict outp) {
  context ctx;
  ctx.basep = reinterpret_cast<int64_t>(inp);
  ctx.endp = reinterpret_cast<int64_t>(inp + count);
  memset(ctx.dat, 0, sizeof(ctx.dat));
  uint32_t *const outp_base = outp;
  while (--count >= 0) {
    if (__fix_branch_imm(&inp, &outp, &ctx)) continue;
    if (__fix_cond_comp_test_branch(&inp, &outp, &ctx)) continue;
    if (__fix_loadlit(&inp, &outp, &ctx)) continue;
    if (__fix_pcreladdr(&inp, &outp, &ctx)) continue;
    ctx.process_fix_map(ctx.get_and_set_current_index(inp, outp));
    *(outp++) = *(inp++);
  }
  static constexpr uint_fast64_t mask = 0x03ffffffu;
  auto callback = reinterpret_cast<int64_t>(inp);
  auto pc_offset = static_cast<int64_t>(callback - reinterpret_cast<int64_t>(outp)) >> 2;
  if (llabs(pc_offset) >= (mask >> 1)) {
    if ((reinterpret_cast<uint64_t>(outp + 2) & 7u) != 0u) {
      outp[0] = A64_NOP;
      ++outp;
    }
    outp[0] = 0x58000051u;
    outp[1] = 0xd61f0220u;
    *reinterpret_cast<int64_t *>(outp + 2) = callback;
    outp += 4;
  } else {
    outp[0] = 0x14000000u | (pc_offset & mask);
    ++outp;
  }
  const uintptr_t total = (outp - outp_base) * sizeof(uint32_t);
  __flush_cache(outp_base, total);
}
extern "C" {
  static __attribute__((__aligned__(__page_size))) uint32_t __insns_pool[A64_MAX_BACKUPS][A64_MAX_INSTRUCTIONS * 10];
  class A64HookInit {
  public:
    A64HookInit() {
      __make_rwx(__insns_pool, sizeof(__insns_pool));
    }
  };
  static A64HookInit __init;
  static uint32_t *FastAllocateTrampoline() {
    static volatile int32_t __index = -1;
    int32_t i = __atomic_increase(&__index);
    if (__predict_true(i >= 0 && i < __countof(__insns_pool))) {
      return __insns_pool[i];
    }
    return NULL;
  }
  A64_JNIEXPORT void *A64HookFunctionV(void *const symbol, void *const replace, void *const rwx, const uintptr_t rwx_size) {
    static constexpr uint_fast64_t mask = 0x03ffffffu;
    uint32_t *trampoline = static_cast<uint32_t *>(rwx), *original = static_cast<uint32_t *>(symbol);
    auto pc_offset = static_cast<int64_t>(__intval(replace) - __intval(symbol)) >> 2;
    if (llabs(pc_offset) >= (mask >> 1)) {
      int32_t count = (reinterpret_cast<uint64_t>(original + 2) & 7u) != 0u ? 5 : 4;
      if (trampoline) {
        if (rwx_size < count * 10u) {
          return NULL;
        }
        __fix_instructions(original, count, trampoline);
      }
      if (__make_rwx(original, 5 * sizeof(uint32_t)) == 0) {
        if (count == 5) {
          original[0] = A64_NOP;
          ++original;
        }
        original[0] = 0x58000051u;
        original[1] = 0xd61f0220u;
        *reinterpret_cast<int64_t *>(original + 2) = __intval(replace);
        __flush_cache(symbol, 5 * sizeof(uint32_t));
      } else {
        trampoline = NULL;
      }
    } else {
      if (trampoline) {
        if (rwx_size < 1u * 10u) {
          return NULL;
        }
        __fix_instructions(original, 1, trampoline);
      }
      if (__make_rwx(original, 1 * sizeof(uint32_t)) == 0) {
        __sync_cmpswap(original, *original, 0x14000000u | (pc_offset & mask));
        __flush_cache(symbol, 1 * sizeof(uint32_t));
      } else {
        trampoline = NULL;
      }
    }
    return trampoline;
  }
  A64_JNIEXPORT void A64HookFunction(void *const symbol, void *const replace, void **result) {
    void *trampoline = NULL;
    if (result != NULL) {
      trampoline = FastAllocateTrampoline();
      *result = trampoline;
      if (trampoline == NULL) return;
    }
    __make_rwx(symbol, 5 * sizeof(size_t));
    trampoline = A64HookFunctionV(symbol, replace, trampoline, A64_MAX_INSTRUCTIONS * 10u);
    if (trampoline == NULL && result != NULL) {
      *result = NULL;
    }
  }
}
#endif
