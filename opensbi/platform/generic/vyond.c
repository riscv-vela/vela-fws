#include <vyond.h>
#include <mprv.h>

#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_double_trap.h>
#include <sbi/sbi_ecall.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_illegal_insn.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_irqchip.h>
#include <sbi/sbi_trap_ldst.h>
#include <sbi/sbi_pmu.h>
#include <sbi/sbi_sse.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_sse.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_trap.h>
#include <sbi_utils/mailbox/mailbox.h>
#include <sbi_utils/mailbox/rpmi_mailbox.h>
#include <sbi/sbi_ras.h>

unsigned long sbi_sm_create_enclave(unsigned long* edi, uintptr_t create_args);
unsigned long sbi_sm_destroy_enclave(unsigned long eid);
unsigned long sbi_sm_enter_enclave(struct sbi_trap_regs *regs, unsigned long eid);
unsigned long sbi_sm_resume_enclave(struct sbi_trap_regs *regs, unsigned long eid);
unsigned long sbi_sm_stop_enclave(struct sbi_trap_regs *regs, unsigned long request);
unsigned long sbi_sm_exit_enclave(struct sbi_trap_regs *regs);

unsigned long copy_enclave_create_args(uintptr_t src, struct keystone_sbi_create_t* dest);
unsigned long sbi_sm_create_shm_region(unsigned long *rid, uintptr_t pa, unsigned long size);
unsigned long sbi_sm_map_shm_region(struct sbi_trap_regs *regs, unsigned long rid);
unsigned long sbi_sm_unmap_shm_region(unsigned long rid);
unsigned long sbi_sm_change_shm_region(unsigned long rid, unsigned long dyn_perm);
unsigned long sbi_sm_share_shm_region(unsigned long rid, unsigned long eid2share, unsigned long st_perm);

void sbi_rass_process(void)
{
        int rc;
        u32 pending_vectors[MAX_PEND_VECS] = { 0xfffffffful };
        u32 nr_pending, nr_remaining;

#if __riscv_xlen == 32
        csr_clear(CSR_MIPH, MIPH_RASHP_INTP);
#else
        csr_clear(CSR_MIP, MIP_RASHP_INTP);
#endif

        rc = sbi_ras_sync_hart_errs(pending_vectors, &nr_pending, &nr_remaining);
        if (rc)
                return;

        for (rc = 0; rc < nr_pending; rc++)
                if (pending_vectors[rc] == SBI_SSE_EVENT_LOCAL_HIGH_PRIO_RAS ||
                    pending_vectors[rc] == SBI_SSE_EVENT_GLOBAL_HIGH_PRIO_RAS)
                        sbi_sse_inject_event(pending_vectors[rc]);

        return;
}

static int sbi_trap_nonaia_irq(unsigned long irq)
{
        switch (irq) {
        case IRQ_M_TIMER:
                sbi_timer_process();
                break;
        case IRQ_M_SOFT:
                sbi_ipi_process();
                break;
        case IRQ_RASHP_INT:
                sbi_rass_process();
                break;
        case IRQ_M_EXT:
                return sbi_irqchip_process();
        default:
                if (irq == sbi_pmu_irq_bit()) {
                        sbi_pmu_ovf_irq();
                        return 0;
                }
                return SBI_ENOENT;
        }

        return 0;
}


static int sbi_trap_aia_irq(void)
{
        int rc;
        unsigned long mtopi;

        while ((mtopi = csr_read(CSR_MTOPI))) {
                mtopi = mtopi >> TOPI_IID_SHIFT;
                switch (mtopi) {
                case IRQ_M_TIMER:
                        sbi_timer_process();
                        break;
                case IRQ_M_SOFT:
                        sbi_ipi_process();
                        break;
                case IRQ_M_EXT:
                        rc = sbi_irqchip_process();
                        if (rc)
                                return rc;
                        break;
                case IRQ_RASHP_INT:
                        sbi_rass_process();
                        break;
                default:
                        if (mtopi == sbi_pmu_irq_bit()) {
                                sbi_pmu_ovf_irq();
                                break;
                        }

                        return SBI_ENOENT;
                }
        }

        return 0;
}



static int sbi_ecall_vyond_monitor_handler(
    unsigned long extid, unsigned long funcid,
    const struct sbi_trap_regs *regs,
    unsigned long *out_val,
    struct sbi_trap_info *out_trap)
{
    sbi_printf("SBI ECALL funcid: %ld\n", funcid);
    uintptr_t retval;

    //sbi_printf("SBI ECALL funcid: %ld\n", funcid);
  
    if (funcid <= FID_RANGE_DEPRECATED) {
        return SBI_ERR_SM_DEPRECATED;
    }

    switch (funcid) {
    case SBI_SM_CREATE_ENCLAVE:
        struct keystone_sbi_create_t create_args_local;
        retval = copy_enclave_create_args((uintptr_t)regs->a0, &create_args_local);
        if (!retval) {
            retval = sbi_sm_create_enclave(out_val, (uintptr_t)&create_args_local);
        }
        break;
    case SBI_SM_DESTROY_ENCLAVE:
        retval = sbi_sm_destroy_enclave(regs->a0);
        break;
    case SBI_SM_ENTER_ENCLAVE:
        retval = sbi_sm_enter_enclave((struct sbi_trap_regs*) regs, regs->a0);
        ((struct sbi_trap_regs *)regs)->mepc += 4;
        sbi_trap_exit(regs);
        break;
    case SBI_SM_RESUME_ENCLAVE:
        retval = sbi_sm_resume_enclave((struct sbi_trap_regs*) regs, regs->a0);
        if (!regs->zero) {
          ((struct sbi_trap_regs *)regs)->a0 = retval;
        }
        ((struct sbi_trap_regs *)regs)->mepc += 4;
        sbi_trap_exit(regs);
        break;
    case SBI_SM_RANDOM:
        static uint64_t w = 0, s = 0xb5ad4eceda1ce2a9;
        unsigned long cycles;
        asm volatile ("rdcycle %0" : "=r" (cycles));

        // from Middle Square Weyl Sequence algorithm
        uint64_t x = cycles;
        x *= x;
        x += (w += s);
        *out_val = (x>>32) | (x<<32);
        retval = 0;
        break;
    case SBI_SM_STOP_ENCLAVE:
        retval = sbi_sm_stop_enclave((struct sbi_trap_regs*) regs, regs->a0);
        ((struct sbi_trap_regs *)regs)->a0 = retval;
        ((struct sbi_trap_regs *)regs)->mepc += 4;
        sbi_trap_exit(regs);
        break;
    case SBI_SM_EXIT_ENCLAVE:
        unsigned long prev_reg_a0 = regs->a0;
        retval = sbi_sm_exit_enclave((struct sbi_trap_regs*) regs);
        ((struct sbi_trap_regs *)regs)->a0 = retval;
        ((struct sbi_trap_regs *)regs)->a1 = prev_reg_a0;
        ((struct sbi_trap_regs *)regs)->mepc += 4;
        sbi_trap_exit(regs);
        break;
	case SBI_SM_CREATE_SHM_REGION:
		retval = sbi_sm_create_shm_region(out_val, (uintptr_t)regs->a0, (unsigned long)regs->a1);
		break;
	case SBI_SM_MAP_SHM_REGION:
		retval = sbi_sm_map_shm_region((struct sbi_trap_regs *)regs, (uint32_t)regs->a0);
		break;
	case SBI_SM_UNMAP_SHM_REGION:
		retval = sbi_sm_unmap_shm_region((uint32_t)regs->a0);
		break;
	case SBI_SM_CHANGE_SHM_REGION:
		retval = sbi_sm_change_shm_region(regs->a0, regs->a1);
		break;
	case SBI_SM_SHARE_SHM_REGION:
		retval = sbi_sm_share_shm_region(regs->a0, regs->a1, regs->a2);
		break;
	default:
		retval = SBI_ERR_SM_NOT_IMPLEMENTED;
		break;
	}

	// sbi_printf("Retval = %lx\n", retval);

    return retval;
}

static int sbi_ecall_vyond_monitor_handler_vela(ulong extid, ulong funcid,
                                               struct sbi_trap_regs *regs,
                                               struct sbi_ecall_return *out)
{

    ulong out_val = 0;
    struct sbi_trap_info trap;
    int rc;

    rc = sbi_ecall_vyond_monitor_handler(extid, funcid,
                                             (const struct sbi_trap_regs *)regs,
                                             &out_val, &trap);
    out->value = out_val;
    return rc;
}

  
//extern struct sbi_ecall_extension ecall_vyond_monitor;
#define SBI_EXT_EXPERIMENTAL_VYOND_MONITOR 0x08424b45 // BKE (Berkeley Keystone Enclave)
  
struct sbi_ecall_extension ecall_vyond_monitor = {
    .extid_start = SBI_EXT_EXPERIMENTAL_VYOND_MONITOR,
    .extid_end = SBI_EXT_EXPERIMENTAL_VYOND_MONITOR,
    .handle = sbi_ecall_vyond_monitor_handler_vela,
};
  
int sm_init(int cold_boot);

int vyond_monitor_init(bool cold_boot)
{
    sbi_printf("Initializing vyond monitor\n");

    int ret = sm_init(cold_boot);
    if (ret != 0) {
        return ret;
    }
    sbi_printf("Finished Initializing vyond monitor\n");
    sbi_ecall_register_extension(&ecall_vyond_monitor);

    return ret;
}

// TODO: This function is externally used by sm-sbi.c.
// Change it to be internal (remove from the enclave.h and make static)
/* Internal function enforcing a copy source is from the untrusted world.
 * Does NOT do verification of dest, assumes caller knows what that is.
 * Dest should be inside the SM memory.
 */
unsigned long copy_enclave_create_args(uintptr_t src, struct keystone_sbi_create_t* dest){



  int region_overlap = copy_to_sm(dest, src, sizeof(struct keystone_sbi_create_t));

  if (region_overlap)
    return SBI_ERR_SM_ENCLAVE_REGION_OVERLAPS;
  else
    return SBI_ERR_SM_ENCLAVE_SUCCESS;
}

static void sbi_trap_error(const char *msg, int rc,
				      ulong mcause, ulong mtval, ulong mtval2,
				      ulong mtinst, struct sbi_trap_regs *regs)
{
	u32 hartid = current_hartid();

	if (misa_extension('H')) {
		sbi_printf("%s: hart%d: mtval2=0x%" PRILX
			   " mtinst=0x%" PRILX "\n",
			   __func__, hartid, mtval2, mtinst);
	}
	sbi_printf("%s: hart%d: mepc=0x%" PRILX " mstatus=0x%" PRILX "\n",
		   __func__, hartid, regs->mepc, regs->mstatus);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "ra", regs->ra, "sp", regs->sp);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "gp", regs->gp, "tp", regs->tp);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "s0", regs->s0, "s1", regs->s1);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "a0", regs->a0, "a1", regs->a1);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "a2", regs->a2, "a3", regs->a3);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "a4", regs->a4, "a5", regs->a5);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "a6", regs->a6, "a7", regs->a7);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "s2", regs->s2, "s3", regs->s3);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "s4", regs->s4, "s5", regs->s5);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "s6", regs->s6, "s7", regs->s7);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "s8", regs->s8, "s9", regs->s9);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "s10", regs->s10, "s11", regs->s11);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "t0", regs->t0, "t1", regs->t1);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "t2", regs->t2, "t3", regs->t3);
	sbi_printf("%s: hart%d: %s=0x%" PRILX " %s=0x%" PRILX "\n", __func__,
		   hartid, "t4", regs->t4, "t5", regs->t5);
	sbi_printf("%s: hart%d: %s=0x%" PRILX "\n", __func__, hartid, "t6",
		   regs->t6);

    sbi_sm_exit_enclave((struct sbi_trap_regs*) regs);
unsigned long copy_enclave_create_args(uintptr_t src, struct keystone_sbi_create_t *dest);
}


/**
 * Handle trap/interrupt
 *
 * This function is called by firmware linked to OpenSBI
 * library for handling trap/interrupt. It expects the
 * following:
 * 1. The 'mscratch' CSR is pointing to sbi_scratch of current HART
 * 2. The 'mcause' CSR is having exception/interrupt cause
 * 3. The 'mtval' CSR is having additional trap information
 * 4. The 'mtval2' CSR is having additional trap information
 * 5. The 'mtinst' CSR is having decoded trap instruction
 * 6. Stack pointer (SP) is setup for current HART
 * 7. Interrupts are disabled in MSTATUS CSR
 *
 * @param regs pointer to register state
 */
struct sbi_trap_context *sbi_trap_handler_keystone_enclave(struct sbi_trap_context *tcntx)
{
        int rc = SBI_ENOTSUPP;
        const char *msg = "trap handler failed";
        struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
        const struct sbi_trap_info *trap = &tcntx->trap;
        struct sbi_trap_regs *regs = &tcntx->regs;
        ulong mcause = tcntx->trap.cause;

        /* Update trap context pointer */
        tcntx->prev_context = sbi_trap_get_context(scratch);
        sbi_trap_set_context(scratch, tcntx);

        if (mcause & MCAUSE_IRQ_MASK) {
                if (sbi_hart_has_extension(sbi_scratch_thishart_ptr(),
                                           SBI_HART_EXT_SMAIA))
                        rc = sbi_trap_aia_irq();
                else
                        rc = sbi_trap_nonaia_irq(mcause & ~MCAUSE_IRQ_MASK);
                msg = "unhandled local interrupt";
                goto trap_done;
        }

        switch (mcause) {
        case CAUSE_ILLEGAL_INSTRUCTION:
                rc  = sbi_illegal_insn_handler(tcntx);
                msg = "illegal instruction handler failed";
                break;
        case CAUSE_MISALIGNED_LOAD:
                sbi_pmu_ctr_incr_fw(SBI_PMU_FW_MISALIGNED_LOAD);
                rc  = sbi_misaligned_load_handler(tcntx);
                msg = "misaligned load handler failed";
                break;
        case CAUSE_MISALIGNED_STORE:
                sbi_pmu_ctr_incr_fw(SBI_PMU_FW_MISALIGNED_STORE);
                rc  = sbi_misaligned_store_handler(tcntx);
                msg = "misaligned store handler failed";
                break;
        case CAUSE_SUPERVISOR_ECALL:
        case CAUSE_MACHINE_ECALL:
                rc  = sbi_ecall_handler(tcntx);
                msg = "ecall handler failed";
                break;
        default:
                /* If the trap came from S or U mode, redirect it there */
                msg = "trap redirect failed";
                rc  = sbi_trap_redirect(regs, trap);
                break;
        }

trap_done:
        if (rc){
		sbi_trap_error(msg, rc,
              tcntx->trap.cause,
              tcntx->trap.tval,
              tcntx->trap.tval2,
              tcntx->trap.tinst,
              &tcntx->regs);}

        if (sbi_mstatus_prev_mode(regs->mstatus) != PRV_M){
                sbi_sse_process_pending_events(regs);}

        sbi_trap_set_context(scratch, tcntx->prev_context);
        return tcntx;
}

