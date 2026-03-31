/*
 *  Sparc MMU helpers
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"
#include "system/memory.h"
#include "qemu/qemu-print.h"
#include "trace.h"

/* Sparc MMU emulation */

#ifndef TARGET_SPARC64
/*
 * Sparc V8 Reference MMU (SRMMU)
 */
static const int access_table[8][8] = {
    { 0, 0, 0, 0, 8, 0, 12, 12 },
    { 0, 0, 0, 0, 8, 0, 0, 0 },
    { 8, 8, 0, 0, 0, 8, 12, 12 },
    { 8, 8, 0, 0, 0, 8, 0, 0 },
    { 8, 0, 8, 0, 8, 8, 12, 12 },
    { 8, 0, 8, 0, 8, 0, 8, 0 },
    { 8, 8, 8, 0, 8, 8, 12, 12 },
    { 8, 8, 8, 0, 8, 8, 8, 0 }
};

static const int perm_table[2][8] = {
    {
        PAGE_READ,
        PAGE_READ | PAGE_WRITE,
        PAGE_READ | PAGE_EXEC,
        PAGE_READ | PAGE_WRITE | PAGE_EXEC,
        PAGE_EXEC,
        PAGE_READ | PAGE_WRITE,
        PAGE_READ | PAGE_EXEC,
        PAGE_READ | PAGE_WRITE | PAGE_EXEC
    },
    {
        PAGE_READ,
        PAGE_READ | PAGE_WRITE,
        PAGE_READ | PAGE_EXEC,
        PAGE_READ | PAGE_WRITE | PAGE_EXEC,
        PAGE_EXEC,
        PAGE_READ,
        0,
        0,
    }
};

static int get_physical_address(CPUSPARCState *env, CPUTLBEntryFull *full,
                                int *access_index, target_ulong address,
                                int rw, int mmu_idx)
{
    int access_perms = 0;
    hwaddr pde_ptr;
    uint32_t pde;
    int error_code = 0, is_dirty, is_user;
    unsigned long page_offset;
    CPUState *cs = env_cpu(env);
    MemTxResult result;

    is_user = mmu_idx == MMU_USER_IDX;

    if (mmu_idx == MMU_PHYS_IDX) {
        full->lg_page_size = TARGET_PAGE_BITS;
        /* Boot mode: instruction fetches are taken from PROM */
        if (rw == 2 && (env->mmuregs[0] & env->def.mmu_bm)) {
            full->phys_addr = env->prom_addr | (address & 0x7ffffULL);
            full->prot = PAGE_READ | PAGE_EXEC;
            return 0;
        }
        if ((address & 0xfff00000) == 0 && rw != 2) {
            fprintf(stderr, "[PHYS] va=0x%08x rw=%d (page fill for bypass access)\n",
                    (uint32_t)address, rw);
        }
        full->phys_addr = address;
        full->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

        /* VIVT D-cache overlay: bypass data accesses go through the
         * D-cache on SuperSPARC.  Record the VA→PA=VA mapping so that
         * subsequent MMU-enabled accesses to the same VA page hit the
         * "cache" and return data from the bypass PA, not the page table
         * translation. */
        if (rw != 2) {
            int dcache_index = (address >> TARGET_PAGE_BITS)
                               & (SPARC_DCACHE_WAYS - 1);
            env->dcache_overlay[dcache_index].va_page =
                address & TARGET_PAGE_MASK;
            env->dcache_overlay[dcache_index].pa_page =
                address & TARGET_PAGE_MASK;
            if ((address & 0xfffff000) == 0x00080000) {
                fprintf(stderr, "[DCACHE-POP] va=0x%08x idx=%d "
                        "va_page=0x%08x\n",
                        (uint32_t)address, dcache_index,
                        env->dcache_overlay[dcache_index].va_page);
            }
        }

        return 0;
    }

    /* VIVT D-cache overlay check: if a bypass access previously loaded
     * this VA page into the D-cache, return the cached PA instead of
     * walking the page table.  Only for data accesses (rw != 2) —
     * the I-cache has its own path. */
    if (rw != 2) {
        int dcache_index = (address >> TARGET_PAGE_BITS)
                           & (SPARC_DCACHE_WAYS - 1);
        if ((address & 0xfffff000) == 0x00080000) {
            fprintf(stderr, "[DCACHE-CHK] va=0x%08x idx=%d "
                    "stored_va=0x%08x want_va=0x%08x match=%d\n",
                    (uint32_t)address, dcache_index,
                    env->dcache_overlay[dcache_index].va_page,
                    (uint32_t)(address & TARGET_PAGE_MASK),
                    env->dcache_overlay[dcache_index].va_page ==
                    (uint32_t)(address & TARGET_PAGE_MASK));
        }
        if (env->dcache_overlay[dcache_index].va_page ==
            (address & TARGET_PAGE_MASK)) {
            full->lg_page_size = TARGET_PAGE_BITS;
            full->phys_addr = env->dcache_overlay[dcache_index].pa_page
                              | (address & ~TARGET_PAGE_MASK);
            full->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            fprintf(stderr, "[DCACHE-HIT] va=0x%08x → PA=0x%llx "
                    "(bypass overlay)\n",
                    (uint32_t)address,
                    (unsigned long long)full->phys_addr);
            return 0;
        }
    }

    *access_index = ((rw & 1) << 2) | (rw & 2) | (is_user ? 0 : 1);
    full->phys_addr = 0xffffffffffff0000ULL;

    /* SPARC reference MMU table walk: Context table->L1->L2->PTE */
    /* Context base + context number.
     * Cast to hwaddr before shifting to preserve 36-bit physical addresses
     * (mmuregs is uint32_t, so without the cast the shift truncates). */
    pde_ptr = ((hwaddr)env->mmuregs[1] << 4) + (env->mmuregs[2] << 2);
    pde = address_space_ldl_be(cs->as, pde_ptr,
                               MEMTXATTRS_UNSPECIFIED, &result);
    if ((address & 0xfff00000) == 0) {
        fprintf(stderr, "[WALK] va=0x%08x rw=%d CTP=0x%08x ctx=%d "
                "pde_ptr=0x%llx ctx_pde=0x%08x (type=%d)\n",
                (uint32_t)address, rw,
                env->mmuregs[1], env->mmuregs[2],
                (unsigned long long)pde_ptr, pde, pde & 3);
        /* If ctx entry is invalid (type=0), dump all 16 entries */
        if ((pde & 3) == 0) {
            hwaddr ctx_base = (hwaddr)env->mmuregs[1] << 4;
            int context_idx;
            fprintf(stderr, "[WALK] ctx table dump at 0x%llx:\n",
                    (unsigned long long)ctx_base);
            for (context_idx = 0; context_idx < 16; context_idx++) {
                uint32_t entry = address_space_ldl_be(cs->as,
                    ctx_base + context_idx * 4,
                    MEMTXATTRS_UNSPECIFIED, NULL);
                fprintf(stderr, "  ctx[%d]=0x%08x (type=%d)\n",
                        context_idx, entry, entry & 3);
            }
        }
    }
    if (result != MEMTX_OK) {
        return 4 << 2; /* Translation fault, L = 0 */
    }

    /* Ctx pde */
    switch (pde & PTE_ENTRYTYPE_MASK) {
    default:
    case 0: /* Invalid */
        return 1 << 2;
    case 2: /* L0 PTE, maybe should not happen? */
    case 3: /* Reserved */
        return 4 << 2;
    case 1: /* L0 PDE */
        pde_ptr = ((address >> 22) & ~3) + ((hwaddr)(pde & ~3) << 4);
        pde = address_space_ldl_be(cs->as, pde_ptr,
                                   MEMTXATTRS_UNSPECIFIED, &result);
        if ((address & 0xfff00000) == 0) {
            /* pde_ptr here points to the specific L1 entry that was read.
             * Compute the L1 table base from pde_ptr by masking off the
             * entry index (which is in the low bits from the VA shift). */
            hwaddr l1_table_base = pde_ptr & ~0x3ff; /* 256 entries × 4 = 1024 */
            fprintf(stderr, "[WALK]   L1 pde_ptr=0x%llx pde=0x%08x (type=%d)\n",
                    (unsigned long long)pde_ptr, pde, pde & 3);
            /* Dump first 4 L1 entries from the actual table base */
            {
                int dump_index;
                fprintf(stderr, "[WALK]   L1@0x%llx[0..3]:",
                        (unsigned long long)l1_table_base);
                for (dump_index = 0; dump_index < 4; dump_index++) {
                    uint32_t entry = address_space_ldl_be(cs->as,
                        l1_table_base + dump_index * 4,
                        MEMTXATTRS_UNSPECIFIED, NULL);
                    fprintf(stderr, " [%d]=0x%08x", dump_index, entry);
                }
                fprintf(stderr, "\n");
                /* Dump first word at PA 0x3000-0x6000 to see L2/L3 tables */
                fprintf(stderr, "[WALK]   RAM dumps:");
                for (dump_index = 3; dump_index <= 6; dump_index++) {
                    uint32_t v = address_space_ldl_be(cs->as,
                        dump_index * 0x1000,
                        MEMTXATTRS_UNSPECIFIED, NULL);
                    fprintf(stderr, " @0x%x=0x%08x", dump_index * 0x1000, v);
                }
                fprintf(stderr, "\n");
            }
        }
        if (result != MEMTX_OK) {
            return (1 << 8) | (4 << 2); /* Translation fault, L = 1 */
        }

        switch (pde & PTE_ENTRYTYPE_MASK) {
        default:
        case 0: /* Invalid */
            return (1 << 8) | (1 << 2);
        case 3: /* Reserved */
            return (1 << 8) | (4 << 2);
        case 1: /* L1 PDE */
            pde_ptr = ((address & 0xfc0000) >> 16) + ((hwaddr)(pde & ~3) << 4);
            {
                uint32_t l1_pde_saved = pde; /* the L1 PDP we just traversed */
                pde = address_space_ldl_be(cs->as, pde_ptr,
                                           MEMTXATTRS_UNSPECIFIED, &result);
                fprintf(stderr, "[WALK-L2] va=0x%08x L1-PDP=0x%08x "
                        "L2@0x%llx L2-pde=0x%08x (type=%d)\n",
                        (uint32_t)address, l1_pde_saved,
                        (unsigned long long)pde_ptr, pde, pde & 3);
            }
            if ((address & 0xfff00000) == 0) {
                fprintf(stderr, "[WALK]   L2 pde_ptr=0x%llx pde=0x%08x (type=%d)\n",
                        (unsigned long long)pde_ptr, pde, pde & 3);
            }
            if (result != MEMTX_OK) {
                return (2 << 8) | (4 << 2); /* Translation fault, L = 2 */
            }

            switch (pde & PTE_ENTRYTYPE_MASK) {
            default:
            case 0: /* Invalid */
                return (2 << 8) | (1 << 2);
            case 3: /* Reserved */
                return (2 << 8) | (4 << 2);
            case 1: /* L2 PDE */
                pde_ptr = ((address & 0x3f000) >> 10) + ((hwaddr)(pde & ~3) << 4);
                pde = address_space_ldl_be(cs->as, pde_ptr,
                                           MEMTXATTRS_UNSPECIFIED, &result);
                if (result != MEMTX_OK) {
                    return (3 << 8) | (4 << 2); /* Translation fault, L = 3 */
                }

                switch (pde & PTE_ENTRYTYPE_MASK) {
                default:
                case 0: /* Invalid */
                    return (3 << 8) | (1 << 2);
                case 1: /* PDE, should not happen */
                case 3: /* Reserved */
                    return (3 << 8) | (4 << 2);
                case 2: /* L3 PTE */
                    page_offset = 0;
                }
                full->lg_page_size = TARGET_PAGE_BITS;
                break;
            case 2: /* L2 PTE */
                page_offset = address & 0x3f000;
                full->lg_page_size = 18;
            }
            break;
        case 2: /* L1 PTE */
            page_offset = address & 0xfff000;
            full->lg_page_size = 24;
            break;
        }
    }

    /* check access */
    access_perms = (pde & PTE_ACCESS_MASK) >> PTE_ACCESS_SHIFT;
    error_code = access_table[*access_index][access_perms];
    if (error_code && !((env->mmuregs[0] & MMU_NF) && is_user)) {
        return error_code;
    }

    /* update page modified and dirty bits */
    is_dirty = (rw & 1) && !(pde & PG_MODIFIED_MASK);
    if (!(pde & PG_ACCESSED_MASK) || is_dirty) {
        pde |= PG_ACCESSED_MASK;
        if (is_dirty) {
            pde |= PG_MODIFIED_MASK;
        }
        address_space_stl_be(cs->as, pde_ptr, pde,
                             MEMTXATTRS_UNSPECIFIED, &result);
        assert(result == MEMTX_OK);
    }

    /* the page can be put in the TLB */
    full->prot = perm_table[is_user][access_perms];
    if (!(pde & PG_MODIFIED_MASK)) {
        /* only set write access if already dirty... otherwise wait
           for dirty access */
        full->prot &= ~PAGE_WRITE;
    }

    /* Even if large ptes, we map only one 4KB page in the cache to
       avoid filling it too fast */
    full->phys_addr = ((hwaddr)(pde & PTE_ADDR_MASK) << 4) + page_offset;

    if ((address & 0xfffff000) == 0x00080000) {
        uint32_t probe_val = address_space_ldl_be(cs->as, full->phys_addr,
            MEMTXATTRS_UNSPECIFIED, NULL);
        fprintf(stderr, "[WALK-PA] va=0x%08x rw=%d → PA=0x%llx "
                "data@PA=0x%08x pde=0x%08x prot=%x\n",
                (uint32_t)address, rw,
                (unsigned long long)full->phys_addr,
                probe_val, pde, full->prot);
    }

    if (error_code) {
        fprintf(stderr, "[WALK-ERR] va=0x%08x rw=%d err=0x%x\n",
                (uint32_t)address, rw, error_code);
    }

    /* Populate TLB diagnostic arrays (SuperSPARC unified TLB fill).
     *
     * The SuperSPARC-II has a unified 64-entry fully-associative TLB
     * shared by instruction and data accesses.  On a TLB miss the
     * hardware walker fills an entry with the PTE from the page table.
     *
     * Diagnostic address layout per entry E:
     *   Each entry occupies one 64-word row in the ASI 0x05 data array.
     *   Field F → idx = E*64 + (F*16) % 64
     *
     * Field 5 (sub=16) → adjacent PTP cached during walk.
     * Field 6 (sub=32) → PTE from the page table walk.
     */
    if (!error_code) {
        uint32_t entry_number = env->dtlb_fill_next;
        /* Field 5 (PTP) → sub_part = (5*16) % 64 = 16
         * Field 6 (PTE) → sub_part = (6*16) % 64 = 32 */
        int field5_idx = entry_number * 64 + 16;
        int field6_idx = entry_number * 64 + 32;

        /* Field 6: PTE from the page table walk */
        env->tlb_diag_data[field6_idx] = pde;

        /* Field 5: adjacent PTP from the same 16-byte L1 block.
         * The walker already read one L1 entry at pde_ptr; read the other
         * 3 entries in the same 16-byte aligned group and deposit the
         * Nth PDP (type 1) we find, where N = dtlb_ptp_skip.  This models
         * the hardware depositing successive adjacent PTPs across walks. */
        {
            hwaddr block_base = pde_ptr & ~0xfULL;
            int adjacent_index;
            uint32_t cached_ptp = 0;
            uint32_t pdp_skip_remaining = env->dtlb_ptp_skip;
            for (adjacent_index = 0; adjacent_index < 4; adjacent_index++) {
                hwaddr entry_addr = block_base + adjacent_index * 4;
                if (entry_addr != pde_ptr) {
                    uint32_t adjacent_entry = address_space_ldl_be(cs->as,
                        entry_addr, MEMTXATTRS_UNSPECIFIED, NULL);
                    if ((adjacent_entry & PTE_ENTRYTYPE_MASK) == 1) {
                        if (pdp_skip_remaining == 0) {
                            cached_ptp = adjacent_entry;
                            break;
                        }
                        pdp_skip_remaining--;
                    }
                }
            }
            env->tlb_diag_data[field5_idx] = cached_ptp;
            env->dtlb_ptp_skip++;
        }

        if (entry_number < 15) {
            env->dtlb_fill_next = entry_number + 1;
        }
        fprintf(stderr, "[TLB-FILL] va=0x%08x rw=%d entry=%u data[%d]=0x%08x "
                "data[%d]=0x%08x\n",
                (uint32_t)address, rw, entry_number,
                field5_idx, env->tlb_diag_data[field5_idx],
                field6_idx, env->tlb_diag_data[field6_idx]);
    }

    return error_code;
}

/* Perform address translation */
bool sparc_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr)
{
    CPUSPARCState *env = cpu_env(cs);
    CPUTLBEntryFull full = {};
    target_ulong vaddr;
    int error_code = 0, access_index;

    /*
     * TODO: If we ever need tlb_vaddr_to_host for this target,
     * then we must figure out how to manipulate FSR and FAR
     * when both MMU_NF and probe are set.  In the meantime,
     * do not support this use case.
     */
    assert(!probe);

    address &= TARGET_PAGE_MASK;
    error_code = get_physical_address(env, &full, &access_index,
                                      address, access_type, mmu_idx);
    vaddr = address;
    if (likely(error_code == 0)) {
        qemu_log_mask(CPU_LOG_MMU,
                      "Translate at %" VADDR_PRIx " -> "
                      HWADDR_FMT_plx ", vaddr " TARGET_FMT_lx "\n",
                      address, full.phys_addr, vaddr);
        tlb_set_page_full(cs, mmu_idx, vaddr, &full);
        return true;
    }

    if (env->mmuregs[3]) { /* Fault status register */
        env->mmuregs[3] = 1; /* overflow (not read before another fault) */
    }
    env->mmuregs[3] |= (access_index << 5) | error_code | 2;
    env->mmuregs[4] = address; /* Fault address register */

    if ((env->mmuregs[0] & MMU_NF) || env->psret == 0)  {
        /* No fault mode: if a mapping is available, just override
           permissions. If no mapping is available, redirect accesses to
           neverland. Fake/overridden mappings will be flushed when
           switching to normal mode. */
        full.prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        tlb_set_page_full(cs, mmu_idx, vaddr, &full);
        return true;
    } else {
        if (access_type == MMU_INST_FETCH) {
            cs->exception_index = TT_TFAULT;
        } else {
            cs->exception_index = TT_DFAULT;
        }
        cpu_loop_exit_restore(cs, retaddr);
    }
}

target_ulong mmu_probe(CPUSPARCState *env, target_ulong address, int mmulev)
{
    CPUState *cs = env_cpu(env);
    hwaddr pde_ptr;
    uint32_t pde;
    MemTxResult result;

    /*
     * TODO: MMU probe operations are supposed to set the fault
     * status registers, but we don't do this.
     */

    /* Context base + context number */
    pde_ptr = ((hwaddr)env->mmuregs[1] << 4) +
        (env->mmuregs[2] << 2);
    pde = address_space_ldl_be(cs->as, pde_ptr,
                               MEMTXATTRS_UNSPECIFIED, &result);
    fprintf(stderr, "[PROBE] va=0x%08x lev=%d ctxbase=0x%llx ctx=%d "
            "pde_ptr=0x%llx pde=0x%08x res=%d\n",
            (uint32_t)address, mmulev,
            (unsigned long long)((hwaddr)env->mmuregs[1] << 4),
            env->mmuregs[2],
            (unsigned long long)pde_ptr, pde, result);
    if (result != MEMTX_OK) {
        return 0;
    }

    switch (pde & PTE_ENTRYTYPE_MASK) {
    default:
    case 0: /* Invalid */
    case 2: /* PTE, maybe should not happen? */
    case 3: /* Reserved */
        return 0;
    case 1: /* L1 PDE */
        if (mmulev == 3) {
            return pde;
        }
        pde_ptr = ((address >> 22) & ~3) + ((hwaddr)(pde & ~3) << 4);
        pde = address_space_ldl_be(cs->as, pde_ptr,
                                   MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            return 0;
        }

        switch (pde & PTE_ENTRYTYPE_MASK) {
        default:
        case 0: /* Invalid */
        case 3: /* Reserved */
            return 0;
        case 2: /* L1 PTE */
            return pde;
        case 1: /* L2 PDE */
            if (mmulev == 2) {
                return pde;
            }
            pde_ptr = ((address & 0xfc0000) >> 16) + ((hwaddr)(pde & ~3) << 4);
            pde = address_space_ldl_be(cs->as, pde_ptr,
                                       MEMTXATTRS_UNSPECIFIED, &result);
            if (result != MEMTX_OK) {
                return 0;
            }

            switch (pde & PTE_ENTRYTYPE_MASK) {
            default:
            case 0: /* Invalid */
            case 3: /* Reserved */
                return 0;
            case 2: /* L2 PTE */
                return pde;
            case 1: /* L3 PDE */
                if (mmulev == 1) {
                    return pde;
                }
                pde_ptr = ((address & 0x3f000) >> 10) + ((hwaddr)(pde & ~3) << 4);
                pde = address_space_ldl_be(cs->as, pde_ptr,
                                           MEMTXATTRS_UNSPECIFIED, &result);
                if (result != MEMTX_OK) {
                    return 0;
                }

                switch (pde & PTE_ENTRYTYPE_MASK) {
                default:
                case 0: /* Invalid */
                case 1: /* PDE, should not happen */
                case 3: /* Reserved */
                    return 0;
                case 2: /* L3 PTE */
                    return pde;
                }
            }
        }
    }
    return 0;
}

void dump_mmu(CPUSPARCState *env)
{
    CPUState *cs = env_cpu(env);
    target_ulong va, va1, va2;
    unsigned int n, m, o;
    hwaddr pa;
    uint32_t pde;

    qemu_printf("Root ptr: " HWADDR_FMT_plx ", ctx: %d\n",
                (hwaddr)env->mmuregs[1] << 4, env->mmuregs[2]);
    for (n = 0, va = 0; n < 256; n++, va += 16 * 1024 * 1024) {
        pde = mmu_probe(env, va, 2);
        if (pde) {
            pa = cpu_get_phys_page_debug(cs, va);
            qemu_printf("VA: " TARGET_FMT_lx ", PA: " HWADDR_FMT_plx
                        " PDE: " TARGET_FMT_lx "\n", va, pa, pde);
            for (m = 0, va1 = va; m < 64; m++, va1 += 256 * 1024) {
                pde = mmu_probe(env, va1, 1);
                if (pde) {
                    pa = cpu_get_phys_page_debug(cs, va1);
                    qemu_printf(" VA: " TARGET_FMT_lx ", PA: "
                                HWADDR_FMT_plx " PDE: " TARGET_FMT_lx "\n",
                                va1, pa, pde);
                    for (o = 0, va2 = va1; o < 64; o++, va2 += 4 * 1024) {
                        pde = mmu_probe(env, va2, 0);
                        if (pde) {
                            pa = cpu_get_phys_page_debug(cs, va2);
                            qemu_printf("  VA: " TARGET_FMT_lx ", PA: "
                                        HWADDR_FMT_plx " PTE: "
                                        TARGET_FMT_lx "\n",
                                        va2, pa, pde);
                        }
                    }
                }
            }
        }
    }
}

/* Gdb expects all registers windows to be flushed in ram. This function handles
 * reads (and only reads) in stack frames as if windows were flushed. We assume
 * that the sparc ABI is followed.
 */
int sparc_cpu_memory_rw_debug(CPUState *cs, vaddr address,
                              uint8_t *buf, size_t len, bool is_write)
{
    CPUSPARCState *env = cpu_env(cs);
    target_ulong addr = address;
    int i;
    int len1;
    int cwp = env->cwp;

    if (!is_write) {
        for (i = 0; i < env->nwindows; i++) {
            int off;
            target_ulong fp = env->regbase[cwp * 16 + 22];

            /* Assume fp == 0 means end of frame.  */
            if (fp == 0) {
                break;
            }

            cwp = cpu_cwp_inc(env, cwp + 1);

            /* Invalid window ? */
            if (env->wim & (1 << cwp)) {
                break;
            }

            /* According to the ABI, the stack is growing downward.  */
            if (addr + len < fp) {
                break;
            }

            /* Not in this frame.  */
            if (addr > fp + 64) {
                continue;
            }

            /* Handle access before this window.  */
            if (addr < fp) {
                len1 = fp - addr;
                if (cpu_memory_rw_debug(cs, addr, buf, len1, is_write) != 0) {
                    return -1;
                }
                addr += len1;
                len -= len1;
                buf += len1;
            }

            /* Access byte per byte to registers. Not very efficient but speed
             * is not critical.
             */
            off = addr - fp;
            len1 = 64 - off;

            if (len1 > len) {
                len1 = len;
            }

            for (; len1; len1--) {
                int reg = cwp * 16 + 8 + (off >> 2);
                union {
                    uint32_t v;
                    uint8_t c[4];
                } u;
                u.v = cpu_to_be32(env->regbase[reg]);
                *buf++ = u.c[off & 3];
                addr++;
                len--;
                off++;
            }

            if (len == 0) {
                return 0;
            }
        }
    }
    return cpu_memory_rw_debug(cs, addr, buf, len, is_write);
}

#else /* !TARGET_SPARC64 */

/* 41 bit physical address space */
static inline hwaddr ultrasparc_truncate_physical(uint64_t x)
{
    return x & 0x1ffffffffffULL;
}

/*
 * UltraSparc IIi I/DMMUs
 */

/* Returns true if TTE tag is valid and matches virtual address value
   in context requires virtual address mask value calculated from TTE
   entry size */
static inline int ultrasparc_tag_match(SparcTLBEntry *tlb,
                                       uint64_t address, uint64_t context,
                                       hwaddr *physical)
{
    uint64_t mask = -(8192ULL << 3 * TTE_PGSIZE(tlb->tte));

    /* valid, context match, virtual address match? */
    if (TTE_IS_VALID(tlb->tte) &&
        (TTE_IS_GLOBAL(tlb->tte) || tlb_compare_context(tlb, context))
        && compare_masked(address, tlb->tag, mask)) {
        /* decode physical address */
        *physical = ((tlb->tte & mask) | (address & ~mask)) & 0x1ffffffe000ULL;
        return 1;
    }

    return 0;
}

static uint64_t build_sfsr(CPUSPARCState *env, int mmu_idx, int rw)
{
    uint64_t sfsr = SFSR_VALID_BIT;

    switch (mmu_idx) {
    case MMU_PHYS_IDX:
        sfsr |= SFSR_CT_NOTRANS;
        break;
    case MMU_USER_IDX:
    case MMU_KERNEL_IDX:
        sfsr |= SFSR_CT_PRIMARY;
        break;
    case MMU_USER_SECONDARY_IDX:
    case MMU_KERNEL_SECONDARY_IDX:
        sfsr |= SFSR_CT_SECONDARY;
        break;
    case MMU_NUCLEUS_IDX:
        sfsr |= SFSR_CT_NUCLEUS;
        break;
    default:
        g_assert_not_reached();
    }

    if (rw == 1) {
        sfsr |= SFSR_WRITE_BIT;
    } else if (rw == 4) {
        sfsr |= SFSR_NF_BIT;
    }

    if (env->pstate & PS_PRIV) {
        sfsr |= SFSR_PR_BIT;
    }

    if (env->dmmu.sfsr & SFSR_VALID_BIT) { /* Fault status register */
        sfsr |= SFSR_OW_BIT; /* overflow (not read before another fault) */
    }

    /* FIXME: ASI field in SFSR must be set */

    return sfsr;
}

static int get_physical_address_data(CPUSPARCState *env, CPUTLBEntryFull *full,
                                     target_ulong address, int rw, int mmu_idx)
{
    CPUState *cs = env_cpu(env);
    unsigned int i;
    uint64_t sfsr;
    uint64_t context;
    bool is_user = false;

    sfsr = build_sfsr(env, mmu_idx, rw);

    switch (mmu_idx) {
    case MMU_PHYS_IDX:
        g_assert_not_reached();
    case MMU_USER_IDX:
        is_user = true;
        /* fallthru */
    case MMU_KERNEL_IDX:
        context = env->dmmu.mmu_primary_context & 0x1fff;
        break;
    case MMU_USER_SECONDARY_IDX:
        is_user = true;
        /* fallthru */
    case MMU_KERNEL_SECONDARY_IDX:
        context = env->dmmu.mmu_secondary_context & 0x1fff;
        break;
    default:
        context = 0;
        break;
    }

    for (i = 0; i < 64; i++) {
        /* ctx match, vaddr match, valid? */
        if (ultrasparc_tag_match(&env->dtlb[i], address, context,
                                 &full->phys_addr)) {
            int do_fault = 0;

            if (TTE_IS_IE(env->dtlb[i].tte)) {
                full->tlb_fill_flags |= TLB_BSWAP;
            }

            /* access ok? */
            /* multiple bits in SFSR.FT may be set on TT_DFAULT */
            if (TTE_IS_PRIV(env->dtlb[i].tte) && is_user) {
                do_fault = 1;
                sfsr |= SFSR_FT_PRIV_BIT; /* privilege violation */
                trace_mmu_helper_dfault(address, context, mmu_idx, env->tl);
            }
            if (rw == 4) {
                if (TTE_IS_SIDEEFFECT(env->dtlb[i].tte)) {
                    do_fault = 1;
                    sfsr |= SFSR_FT_NF_E_BIT;
                }
            } else {
                if (TTE_IS_NFO(env->dtlb[i].tte)) {
                    do_fault = 1;
                    sfsr |= SFSR_FT_NFO_BIT;
                }
            }

            if (do_fault) {
                /* faults above are reported with TT_DFAULT. */
                cs->exception_index = TT_DFAULT;
            } else if (!TTE_IS_W_OK(env->dtlb[i].tte) && (rw == 1)) {
                do_fault = 1;
                cs->exception_index = TT_DPROT;

                trace_mmu_helper_dprot(address, context, mmu_idx, env->tl);
            }

            if (!do_fault) {
                full->prot = PAGE_READ;
                if (TTE_IS_W_OK(env->dtlb[i].tte)) {
                    full->prot |= PAGE_WRITE;
                }

                TTE_SET_USED(env->dtlb[i].tte);

                return 0;
            }

            env->dmmu.sfsr = sfsr;
            env->dmmu.sfar = address; /* Fault address register */
            env->dmmu.tag_access = (address & ~0x1fffULL) | context;
            return 1;
        }
    }

    trace_mmu_helper_dmiss(address, context);

    /*
     * On MMU misses:
     * - UltraSPARC IIi: SFSR and SFAR unmodified
     * - JPS1: SFAR updated and some fields of SFSR updated
     */
    env->dmmu.tag_access = (address & ~0x1fffULL) | context;
    cs->exception_index = TT_DMISS;
    return 1;
}

static int get_physical_address_code(CPUSPARCState *env, CPUTLBEntryFull *full,
                                     target_ulong address, int mmu_idx)
{
    CPUState *cs = env_cpu(env);
    unsigned int i;
    uint64_t context;
    bool is_user = false;

    switch (mmu_idx) {
    case MMU_PHYS_IDX:
    case MMU_USER_SECONDARY_IDX:
    case MMU_KERNEL_SECONDARY_IDX:
        g_assert_not_reached();
    case MMU_USER_IDX:
        is_user = true;
        /* fallthru */
    case MMU_KERNEL_IDX:
        context = env->dmmu.mmu_primary_context & 0x1fff;
        break;
    default:
        context = 0;
        break;
    }

    if (env->tl == 0) {
        /* PRIMARY context */
        context = env->dmmu.mmu_primary_context & 0x1fff;
    } else {
        /* NUCLEUS context */
        context = 0;
    }

    for (i = 0; i < 64; i++) {
        /* ctx match, vaddr match, valid? */
        if (ultrasparc_tag_match(&env->itlb[i],
                                 address, context, &full->phys_addr)) {
            /* access ok? */
            if (TTE_IS_PRIV(env->itlb[i].tte) && is_user) {
                /* Fault status register */
                if (env->immu.sfsr & SFSR_VALID_BIT) {
                    env->immu.sfsr = SFSR_OW_BIT; /* overflow (not read before
                                                     another fault) */
                } else {
                    env->immu.sfsr = 0;
                }
                if (env->pstate & PS_PRIV) {
                    env->immu.sfsr |= SFSR_PR_BIT;
                }
                if (env->tl > 0) {
                    env->immu.sfsr |= SFSR_CT_NUCLEUS;
                }

                /* FIXME: ASI field in SFSR must be set */
                env->immu.sfsr |= SFSR_FT_PRIV_BIT | SFSR_VALID_BIT;
                cs->exception_index = TT_TFAULT;

                env->immu.tag_access = (address & ~0x1fffULL) | context;

                trace_mmu_helper_tfault(address, context);

                return 1;
            }
            full->prot = PAGE_EXEC;
            TTE_SET_USED(env->itlb[i].tte);
            return 0;
        }
    }

    trace_mmu_helper_tmiss(address, context);

    /* Context is stored in DMMU (dmmuregs[1]) also for IMMU */
    env->immu.tag_access = (address & ~0x1fffULL) | context;
    cs->exception_index = TT_TMISS;
    return 1;
}

static int get_physical_address(CPUSPARCState *env, CPUTLBEntryFull *full,
                                int *access_index, target_ulong address,
                                int rw, int mmu_idx)
{
    /* ??? We treat everything as a small page, then explicitly flush
       everything when an entry is evicted.  */
    full->lg_page_size = TARGET_PAGE_BITS;

    /* safety net to catch wrong softmmu index use from dynamic code */
    if (env->tl > 0 && mmu_idx != MMU_NUCLEUS_IDX) {
        if (rw == 2) {
            trace_mmu_helper_get_phys_addr_code(env->tl, mmu_idx,
                                                env->dmmu.mmu_primary_context,
                                                env->dmmu.mmu_secondary_context,
                                                address);
        } else {
            trace_mmu_helper_get_phys_addr_data(env->tl, mmu_idx,
                                                env->dmmu.mmu_primary_context,
                                                env->dmmu.mmu_secondary_context,
                                                address);
        }
    }

    if (mmu_idx == MMU_PHYS_IDX) {
        full->phys_addr = ultrasparc_truncate_physical(address);
        full->prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return 0;
    }

    if (rw == 2) {
        return get_physical_address_code(env, full, address, mmu_idx);
    } else {
        return get_physical_address_data(env, full, address, rw, mmu_idx);
    }
}

/* Perform address translation */
bool sparc_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                        MMUAccessType access_type, int mmu_idx,
                        bool probe, uintptr_t retaddr)
{
    CPUSPARCState *env = cpu_env(cs);
    CPUTLBEntryFull full = {};
    int error_code = 0, access_index;

    address &= TARGET_PAGE_MASK;
    error_code = get_physical_address(env, &full, &access_index,
                                      address, access_type, mmu_idx);
    if (likely(error_code == 0)) {
        trace_mmu_helper_mmu_fault(address, full.phys_addr, mmu_idx, env->tl,
                                   env->dmmu.mmu_primary_context,
                                   env->dmmu.mmu_secondary_context);
        tlb_set_page_full(cs, mmu_idx, address, &full);
        return true;
    }
    if (probe) {
        return false;
    }
    cpu_loop_exit_restore(cs, retaddr);
}

void dump_mmu(CPUSPARCState *env)
{
    unsigned int i;
    const char *mask;

    qemu_printf("MMU contexts: Primary: %" PRId64 ", Secondary: %"
                PRId64 "\n",
                env->dmmu.mmu_primary_context,
                env->dmmu.mmu_secondary_context);
    qemu_printf("DMMU Tag Access: %" PRIx64 ", TSB Tag Target: %" PRIx64
                "\n", env->dmmu.tag_access, env->dmmu.tsb_tag_target);
    if ((env->lsu & DMMU_E) == 0) {
        qemu_printf("DMMU disabled\n");
    } else {
        qemu_printf("DMMU dump\n");
        for (i = 0; i < 64; i++) {
            switch (TTE_PGSIZE(env->dtlb[i].tte)) {
            default:
            case 0x0:
                mask = "  8k";
                break;
            case 0x1:
                mask = " 64k";
                break;
            case 0x2:
                mask = "512k";
                break;
            case 0x3:
                mask = "  4M";
                break;
            }
            if (TTE_IS_VALID(env->dtlb[i].tte)) {
                qemu_printf("[%02u] VA: %" PRIx64 ", PA: %llx"
                            ", %s, %s, %s, %s, ie %s, ctx %" PRId64 " %s\n",
                            i,
                            env->dtlb[i].tag & (uint64_t)~0x1fffULL,
                            TTE_PA(env->dtlb[i].tte),
                            mask,
                            TTE_IS_PRIV(env->dtlb[i].tte) ? "priv" : "user",
                            TTE_IS_W_OK(env->dtlb[i].tte) ? "RW" : "RO",
                            TTE_IS_LOCKED(env->dtlb[i].tte) ?
                            "locked" : "unlocked",
                            TTE_IS_IE(env->dtlb[i].tte) ?
                            "yes" : "no",
                            env->dtlb[i].tag & (uint64_t)0x1fffULL,
                            TTE_IS_GLOBAL(env->dtlb[i].tte) ?
                            "global" : "local");
            }
        }
    }
    if ((env->lsu & IMMU_E) == 0) {
        qemu_printf("IMMU disabled\n");
    } else {
        qemu_printf("IMMU dump\n");
        for (i = 0; i < 64; i++) {
            switch (TTE_PGSIZE(env->itlb[i].tte)) {
            default:
            case 0x0:
                mask = "  8k";
                break;
            case 0x1:
                mask = " 64k";
                break;
            case 0x2:
                mask = "512k";
                break;
            case 0x3:
                mask = "  4M";
                break;
            }
            if (TTE_IS_VALID(env->itlb[i].tte)) {
                qemu_printf("[%02u] VA: %" PRIx64 ", PA: %llx"
                            ", %s, %s, %s, ctx %" PRId64 " %s\n",
                            i,
                            env->itlb[i].tag & (uint64_t)~0x1fffULL,
                            TTE_PA(env->itlb[i].tte),
                            mask,
                            TTE_IS_PRIV(env->itlb[i].tte) ? "priv" : "user",
                            TTE_IS_LOCKED(env->itlb[i].tte) ?
                            "locked" : "unlocked",
                            env->itlb[i].tag & (uint64_t)0x1fffULL,
                            TTE_IS_GLOBAL(env->itlb[i].tte) ?
                            "global" : "local");
            }
        }
    }
}

#endif /* TARGET_SPARC64 */

static int cpu_sparc_get_phys_page(CPUSPARCState *env, hwaddr *phys,
                                   target_ulong addr, int rw, int mmu_idx)
{
    CPUTLBEntryFull full = {};
    int access_index, ret;

    ret = get_physical_address(env, &full, &access_index, addr, rw, mmu_idx);
    if (ret == 0) {
        *phys = full.phys_addr;
    }
    return ret;
}

#if defined(TARGET_SPARC64)
hwaddr cpu_get_phys_page_nofault(CPUSPARCState *env, target_ulong addr,
                                           int mmu_idx)
{
    hwaddr phys_addr;

    if (cpu_sparc_get_phys_page(env, &phys_addr, addr, 4, mmu_idx) != 0) {
        return -1;
    }
    return phys_addr;
}
#endif

hwaddr sparc_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    CPUSPARCState *env = cpu_env(cs);
    hwaddr phys_addr;
    int mmu_idx = cpu_mmu_index(cs, false);

    if (cpu_sparc_get_phys_page(env, &phys_addr, addr, 2, mmu_idx) != 0) {
        if (cpu_sparc_get_phys_page(env, &phys_addr, addr, 0, mmu_idx) != 0) {
            return -1;
        }
    }
    return phys_addr;
}

G_NORETURN void sparc_cpu_do_unaligned_access(CPUState *cs, vaddr addr,
                                              MMUAccessType access_type,
                                              int mmu_idx,
                                              uintptr_t retaddr)
{
    CPUSPARCState *env = cpu_env(cs);

#ifdef TARGET_SPARC64
    env->dmmu.sfsr = build_sfsr(env, mmu_idx, access_type);
    env->dmmu.sfar = addr;
#else
    env->mmuregs[4] = addr;
#endif

    cpu_raise_exception_ra(env, TT_UNALIGNED, retaddr);
}
