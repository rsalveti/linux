/*
 *  linux/drivers/clocksource/arm_arch_timer.c
 *
 *  Copyright (C) 2011 ARM Ltd.
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt)	"arm_arch_timer: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/sched_clock.h>
#include <linux/acpi.h>

#include <asm/arch_timer.h>
#include <asm/virt.h>

#include <clocksource/arm_arch_timer.h>

#undef pr_fmt
#define pr_fmt(fmt) "arch_timer: " fmt

#define CNTTIDR		0x08
#define CNTTIDR_VIRT(n)	(BIT(1) << ((n) * 4))

#define CNTACR(n)	(0x40 + ((n) * 4))
#define CNTACR_RPCT	BIT(0)
#define CNTACR_RVCT	BIT(1)
#define CNTACR_RFRQ	BIT(2)
#define CNTACR_RVOFF	BIT(3)
#define CNTACR_RWVT	BIT(4)
#define CNTACR_RWPT	BIT(5)

#define CNTVCT_LO	0x08
#define CNTVCT_HI	0x0c
#define CNTFRQ		0x10
#define CNTP_TVAL	0x28
#define CNTP_CTL	0x2c
#define CNTV_TVAL	0x38
#define CNTV_CTL	0x3c

static unsigned arch_timers_present __initdata;

static void __iomem *arch_counter_base;
static void __iomem *cntctlbase __initdata;

struct arch_timer {
	void __iomem *base;
	struct clock_event_device evt;
};

#define to_arch_timer(e) container_of(e, struct arch_timer, evt)

static u32 arch_timer_rate;
static int arch_timer_ppi[MAX_TIMER_PPI];

static struct clock_event_device __percpu *arch_timer_evt;

static enum ppi_nr arch_timer_uses_ppi = VIRT_PPI;
static bool arch_timer_c3stop;
static bool arch_timer_mem_use_virtual;

static bool evtstrm_enable = IS_ENABLED(CONFIG_ARM_ARCH_TIMER_EVTSTREAM);

static int __init early_evtstrm_cfg(char *buf)
{
	return strtobool(buf, &evtstrm_enable);
}
early_param("clocksource.arm_arch_timer.evtstrm", early_evtstrm_cfg);

/*
 * Architected system timer support.
 */

#if CONFIG_FSL_ERRATUM_A008585 || IS_ENABLED(CONFIG_HISILICON_ERRATUM_161601)
struct arch_timer_erratum_workaround *timer_unstable_counter_workaround = NULL;
EXPORT_SYMBOL_GPL(timer_unstable_counter_workaround);

#define        FSL_A008585	0x0001
#define        HISILICON_161601	0x0002

DEFINE_STATIC_KEY_FALSE(arch_timer_read_ool_enabled);
EXPORT_SYMBOL_GPL(arch_timer_read_ool_enabled);
#endif

#ifdef CONFIG_FSL_ERRATUM_A008585
/*
 * The number of retries is an arbitrary value well beyond the highest number
 * of iterations the loop has been observed to take.
 */
#define __fsl_a008585_read_reg(reg) ({			\
	u64 _old, _new;					\
	int _retries = 200;				\
							\
	do {						\
		_old = read_sysreg(reg);		\
		_new = read_sysreg(reg);		\
		_retries--;				\
	} while (unlikely(_old != _new) && _retries);	\
							\
	WARN_ON_ONCE(!_retries);			\
	_new;						\
})

static u32 fsl_a008585_read_cntp_tval_el0(void)
{
	return __fsl_a008585_read_reg(cntp_tval_el0);
}

static  u32 fsl_a008585_read_cntv_tval_el0(void)
{
	return __fsl_a008585_read_reg(cntv_tval_el0);
}

static u64 fsl_a008585_read_cntvct_el0(void)
{
	return __fsl_a008585_read_reg(cntvct_el0);
}

static struct arch_timer_erratum_workaround arch_timer_fsl_a008585 = {
	.erratum = FSL_A008585,
	.read_cntp_tval_el0 = fsl_a008585_read_cntp_tval_el0,
	.read_cntv_tval_el0 = fsl_a008585_read_cntv_tval_el0,
	.read_cntvct_el0 = fsl_a008585_read_cntvct_el0,
};
#endif /* CONFIG_FSL_ERRATUM_A008585 */

#ifdef CONFIG_HISILICON_ERRATUM_161601
/*
 * Theoretically the erratum should not occur more than twice in succession,
 * so set the retry count to 2 is sufficient here.
 * But in the stress test with lots of interrupts happened, it will interrupt
 * the read for _old and _new, then we will get WARNING even with timer counter
 * is right, so use 10 instead to retry more.
 * Verify whether the value of the second read is larger than the first by
 * less than 32 is the only way to confirm the value is correct, so clear the
 * lower 5 bits to check whether the difference is greater than 32 or not.
 */
#define __hisi_161601_read_reg(reg) ({				\
	u64 _old, _new;						\
	int _retries = 10;					\
								\
	do {							\
		_old = read_sysreg(reg);			\
		_new = read_sysreg(reg);			\
		_retries--;					\
	} while (unlikely((_new - _old) >> 5) && _retries);	\
								\
	WARN_ON_ONCE(!_retries);				\
	_new;							\
})

static u32 notrace hisi_161601_read_cntp_tval_el0(void)
{
	return __hisi_161601_read_reg(cntp_tval_el0);
}

static  u32 notrace hisi_161601_read_cntv_tval_el0(void)
{
	return __hisi_161601_read_reg(cntv_tval_el0);
}

static u64 notrace hisi_161601_read_cntvct_el0(void)
{
	return __hisi_161601_read_reg(cntvct_el0);
}

static struct arch_timer_erratum_workaround arch_timer_hisi_161601 = {
	.erratum = HISILICON_161601,
	.read_cntp_tval_el0 = hisi_161601_read_cntp_tval_el0,
	.read_cntv_tval_el0 = hisi_161601_read_cntv_tval_el0,
	.read_cntvct_el0 = hisi_161601_read_cntvct_el0,
};
#endif /* CONFIG_HISILICON_ERRATUM_161601 */

static __always_inline
void arch_timer_reg_write(int access, enum arch_timer_reg reg, u32 val,
			  struct clock_event_device *clk)
{
	if (access == ARCH_TIMER_MEM_PHYS_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			writel_relaxed(val, timer->base + CNTP_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			writel_relaxed(val, timer->base + CNTP_TVAL);
			break;
		}
	} else if (access == ARCH_TIMER_MEM_VIRT_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			writel_relaxed(val, timer->base + CNTV_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			writel_relaxed(val, timer->base + CNTV_TVAL);
			break;
		}
	} else {
		arch_timer_reg_write_cp15(access, reg, val);
	}
}

static __always_inline
u32 arch_timer_reg_read(int access, enum arch_timer_reg reg,
			struct clock_event_device *clk)
{
	u32 val;

	if (access == ARCH_TIMER_MEM_PHYS_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			val = readl_relaxed(timer->base + CNTP_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			val = readl_relaxed(timer->base + CNTP_TVAL);
			break;
		}
	} else if (access == ARCH_TIMER_MEM_VIRT_ACCESS) {
		struct arch_timer *timer = to_arch_timer(clk);
		switch (reg) {
		case ARCH_TIMER_REG_CTRL:
			val = readl_relaxed(timer->base + CNTV_CTL);
			break;
		case ARCH_TIMER_REG_TVAL:
			val = readl_relaxed(timer->base + CNTV_TVAL);
			break;
		}
	} else {
		val = arch_timer_reg_read_cp15(access, reg);
	}

	return val;
}

static __always_inline irqreturn_t timer_handler(const int access,
					struct clock_event_device *evt)
{
	unsigned long ctrl;

	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, evt);
	if (ctrl & ARCH_TIMER_CTRL_IT_STAT) {
		ctrl |= ARCH_TIMER_CTRL_IT_MASK;
		arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, evt);
		evt->event_handler(evt);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static irqreturn_t arch_timer_handler_virt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	return timer_handler(ARCH_TIMER_VIRT_ACCESS, evt);
}

static irqreturn_t arch_timer_handler_phys(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	return timer_handler(ARCH_TIMER_PHYS_ACCESS, evt);
}

static irqreturn_t arch_timer_handler_phys_mem(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	return timer_handler(ARCH_TIMER_MEM_PHYS_ACCESS, evt);
}

static irqreturn_t arch_timer_handler_virt_mem(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	return timer_handler(ARCH_TIMER_MEM_VIRT_ACCESS, evt);
}

static __always_inline int timer_shutdown(const int access,
					  struct clock_event_device *clk)
{
	unsigned long ctrl;

	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, clk);
	ctrl &= ~ARCH_TIMER_CTRL_ENABLE;
	arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, clk);

	return 0;
}

static int arch_timer_shutdown_virt(struct clock_event_device *clk)
{
	return timer_shutdown(ARCH_TIMER_VIRT_ACCESS, clk);
}

static int arch_timer_shutdown_phys(struct clock_event_device *clk)
{
	return timer_shutdown(ARCH_TIMER_PHYS_ACCESS, clk);
}

static int arch_timer_shutdown_virt_mem(struct clock_event_device *clk)
{
	return timer_shutdown(ARCH_TIMER_MEM_VIRT_ACCESS, clk);
}

static int arch_timer_shutdown_phys_mem(struct clock_event_device *clk)
{
	return timer_shutdown(ARCH_TIMER_MEM_PHYS_ACCESS, clk);
}

static __always_inline void set_next_event(const int access, unsigned long evt,
					   struct clock_event_device *clk)
{
	unsigned long ctrl;
	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, clk);
	ctrl |= ARCH_TIMER_CTRL_ENABLE;
	ctrl &= ~ARCH_TIMER_CTRL_IT_MASK;
	arch_timer_reg_write(access, ARCH_TIMER_REG_TVAL, evt, clk);
	arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, clk);
}

#if IS_ENABLED(CONFIG_FSL_ERRATUM_A008585) || IS_ENABLED(CONFIG_HISILICON_ERRATUM_161601)
static __always_inline void erratum_set_next_event_generic(const int access,
		unsigned long evt, struct clock_event_device *clk)
{
	unsigned long ctrl;
	u64 cval = evt + arch_counter_get_cntvct();

	ctrl = arch_timer_reg_read(access, ARCH_TIMER_REG_CTRL, clk);
	ctrl |= ARCH_TIMER_CTRL_ENABLE;
	ctrl &= ~ARCH_TIMER_CTRL_IT_MASK;

	if (access == ARCH_TIMER_PHYS_ACCESS)
		write_sysreg(cval, cntp_cval_el0);
	else if (access == ARCH_TIMER_VIRT_ACCESS)
		write_sysreg(cval, cntv_cval_el0);

	arch_timer_reg_write(access, ARCH_TIMER_REG_CTRL, ctrl, clk);
}

static int erratum_set_next_event_virt(unsigned long evt,
					   struct clock_event_device *clk)
{
	erratum_set_next_event_generic(ARCH_TIMER_VIRT_ACCESS, evt, clk);
	return 0;
}

static int erratum_set_next_event_phys(unsigned long evt,
					   struct clock_event_device *clk)
{
	erratum_set_next_event_generic(ARCH_TIMER_PHYS_ACCESS, evt, clk);
	return 0;
}
#endif /* CONFIG_FSL_ERRATUM_A008585 */

static int arch_timer_set_next_event_virt(unsigned long evt,
					  struct clock_event_device *clk)
{
	set_next_event(ARCH_TIMER_VIRT_ACCESS, evt, clk);
	return 0;
}

static int arch_timer_set_next_event_phys(unsigned long evt,
					  struct clock_event_device *clk)
{
	set_next_event(ARCH_TIMER_PHYS_ACCESS, evt, clk);
	return 0;
}

static int arch_timer_set_next_event_virt_mem(unsigned long evt,
					      struct clock_event_device *clk)
{
	set_next_event(ARCH_TIMER_MEM_VIRT_ACCESS, evt, clk);
	return 0;
}

static int arch_timer_set_next_event_phys_mem(unsigned long evt,
					      struct clock_event_device *clk)
{
	set_next_event(ARCH_TIMER_MEM_PHYS_ACCESS, evt, clk);
	return 0;
}

static void erratum_workaround_set_sne(struct clock_event_device *clk)
{
#if IS_ENABLED(CONFIG_FSL_ERRATUM_A008585) || IS_ENABLED(CONFIG_HISILICON_ERRATUM_161601)
	if (!static_branch_unlikely(&arch_timer_read_ool_enabled))
		return;

	if (arch_timer_uses_ppi == VIRT_PPI)
		clk->set_next_event = erratum_set_next_event_virt;
	else
		clk->set_next_event = erratum_set_next_event_phys;
#endif
}

static void __arch_timer_setup(unsigned type,
			       struct clock_event_device *clk)
{
	clk->features = CLOCK_EVT_FEAT_ONESHOT;

	if (type == ARCH_CP15_TIMER) {
		if (arch_timer_c3stop)
			clk->features |= CLOCK_EVT_FEAT_C3STOP;
		clk->name = "arch_sys_timer";
		clk->rating = 450;
		clk->cpumask = cpumask_of(smp_processor_id());
		clk->irq = arch_timer_ppi[arch_timer_uses_ppi];
		switch (arch_timer_uses_ppi) {
		case VIRT_PPI:
			clk->set_state_shutdown = arch_timer_shutdown_virt;
			clk->set_state_oneshot_stopped = arch_timer_shutdown_virt;
			clk->set_next_event = arch_timer_set_next_event_virt;
			break;
		case PHYS_SECURE_PPI:
		case PHYS_NONSECURE_PPI:
		case HYP_PPI:
			clk->set_state_shutdown = arch_timer_shutdown_phys;
			clk->set_state_oneshot_stopped = arch_timer_shutdown_phys;
			clk->set_next_event = arch_timer_set_next_event_phys;
			break;
		default:
			BUG();
		}

		erratum_workaround_set_sne(clk);
	} else {
		clk->features |= CLOCK_EVT_FEAT_DYNIRQ;
		clk->name = "arch_mem_timer";
		clk->rating = 400;
		clk->cpumask = cpu_all_mask;
		if (arch_timer_mem_use_virtual) {
			clk->set_state_shutdown = arch_timer_shutdown_virt_mem;
			clk->set_state_oneshot_stopped = arch_timer_shutdown_virt_mem;
			clk->set_next_event =
				arch_timer_set_next_event_virt_mem;
		} else {
			clk->set_state_shutdown = arch_timer_shutdown_phys_mem;
			clk->set_state_oneshot_stopped = arch_timer_shutdown_phys_mem;
			clk->set_next_event =
				arch_timer_set_next_event_phys_mem;
		}
	}

	clk->set_state_shutdown(clk);

	clockevents_config_and_register(clk, arch_timer_rate, 0xf, 0x7fffffff);
}

static void arch_timer_evtstrm_enable(int divider)
{
	u32 cntkctl = arch_timer_get_cntkctl();

	cntkctl &= ~ARCH_TIMER_EVT_TRIGGER_MASK;
	/* Set the divider and enable virtual event stream */
	cntkctl |= (divider << ARCH_TIMER_EVT_TRIGGER_SHIFT)
			| ARCH_TIMER_VIRT_EVT_EN;
	arch_timer_set_cntkctl(cntkctl);
	elf_hwcap |= HWCAP_EVTSTRM;
#ifdef CONFIG_COMPAT
	compat_elf_hwcap |= COMPAT_HWCAP_EVTSTRM;
#endif
}

static void arch_timer_configure_evtstream(void)
{
	int evt_stream_div, pos;

	/* Find the closest power of two to the divisor */
	evt_stream_div = arch_timer_rate / ARCH_TIMER_EVT_STREAM_FREQ;
	pos = fls(evt_stream_div);
	if (pos > 1 && !(evt_stream_div & (1 << (pos - 2))))
		pos--;
	/* enable event stream */
	arch_timer_evtstrm_enable(min(pos, 15));
}

static void arch_counter_set_user_access(void)
{
	u32 cntkctl = arch_timer_get_cntkctl();

	/* Disable user access to the timers and the physical counter */
	/* Also disable virtual event stream */
	cntkctl &= ~(ARCH_TIMER_USR_PT_ACCESS_EN
			| ARCH_TIMER_USR_VT_ACCESS_EN
			| ARCH_TIMER_VIRT_EVT_EN
			| ARCH_TIMER_USR_PCT_ACCESS_EN);

	/* Enable user access to the virtual counter */
	cntkctl |= ARCH_TIMER_USR_VCT_ACCESS_EN;

	arch_timer_set_cntkctl(cntkctl);
}

static bool arch_timer_has_nonsecure_ppi(void)
{
	return (arch_timer_uses_ppi == PHYS_SECURE_PPI &&
		arch_timer_ppi[PHYS_NONSECURE_PPI]);
}

static u32 check_ppi_trigger(int irq)
{
	u32 flags = irq_get_trigger_type(irq);

	if (flags != IRQF_TRIGGER_HIGH && flags != IRQF_TRIGGER_LOW) {
		pr_warn("WARNING: Invalid trigger for IRQ%d, assuming level low\n", irq);
		pr_warn("WARNING: Please fix your firmware\n");
		flags = IRQF_TRIGGER_LOW;
	}

	return flags;
}

static int arch_timer_starting_cpu(unsigned int cpu)
{
	struct clock_event_device *clk = this_cpu_ptr(arch_timer_evt);
	u32 flags;

	__arch_timer_setup(ARCH_CP15_TIMER, clk);

	flags = check_ppi_trigger(arch_timer_ppi[arch_timer_uses_ppi]);
	enable_percpu_irq(arch_timer_ppi[arch_timer_uses_ppi], flags);

	if (arch_timer_has_nonsecure_ppi()) {
		flags = check_ppi_trigger(arch_timer_ppi[PHYS_NONSECURE_PPI]);
		enable_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI], flags);
	}

	arch_counter_set_user_access();
	if (evtstrm_enable)
		arch_timer_configure_evtstream();

	return 0;
}

static void
arch_timer_detect_rate(void __iomem *cntbase, struct device_node *np)
{
	/* Who has more than one independent system counter? */
	if (arch_timer_rate)
		return;

	/*
	 * Try to determine the frequency from the device tree or CNTFRQ,
	 * if ACPI is enabled, get the frequency from CNTFRQ ONLY.
	 */
	if (!acpi_disabled ||
	    of_property_read_u32(np, "clock-frequency", &arch_timer_rate)) {
		if (cntbase)
			arch_timer_rate = readl_relaxed(cntbase + CNTFRQ);
		else
			arch_timer_rate = arch_timer_get_cntfrq();
	}

	/* Check the timer frequency. */
	if (arch_timer_rate == 0)
		pr_warn("frequency not available\n");
}

static void arch_timer_banner(unsigned type)
{
	pr_info("%s%s%s timer(s) running at %lu.%02luMHz (%s%s%s).\n",
		type & ARCH_CP15_TIMER ? "cp15" : "",
		type == (ARCH_CP15_TIMER | ARCH_MEM_TIMER) ?  " and " : "",
		type & ARCH_MEM_TIMER ? "mmio" : "",
		(unsigned long)arch_timer_rate / 1000000,
		(unsigned long)(arch_timer_rate / 10000) % 100,
		type & ARCH_CP15_TIMER ?
		(arch_timer_uses_ppi == VIRT_PPI) ? "virt" : "phys" :
		"",
		type == (ARCH_CP15_TIMER | ARCH_MEM_TIMER) ?  "/" : "",
		type & ARCH_MEM_TIMER ?
		arch_timer_mem_use_virtual ? "virt" : "phys" :
		"");
}

u32 arch_timer_get_rate(void)
{
	return arch_timer_rate;
}

static u64 arch_counter_get_cntvct_mem(void)
{
	u32 vct_lo, vct_hi, tmp_hi;

	do {
		vct_hi = readl_relaxed(arch_counter_base + CNTVCT_HI);
		vct_lo = readl_relaxed(arch_counter_base + CNTVCT_LO);
		tmp_hi = readl_relaxed(arch_counter_base + CNTVCT_HI);
	} while (vct_hi != tmp_hi);

	return ((u64) vct_hi << 32) | vct_lo;
}

/*
 * Default to cp15 based access because arm64 uses this function for
 * sched_clock() before DT is probed and the cp15 method is guaranteed
 * to exist on arm64. arm doesn't use this before DT is probed so even
 * if we don't have the cp15 accessors we won't have a problem.
 */
u64 (*arch_timer_read_counter)(void) = arch_counter_get_cntvct;

static cycle_t arch_counter_read(struct clocksource *cs)
{
	return arch_timer_read_counter();
}

static cycle_t arch_counter_read_cc(const struct cyclecounter *cc)
{
	return arch_timer_read_counter();
}

static struct clocksource clocksource_counter = {
	.name	= "arch_sys_counter",
	.rating	= 400,
	.read	= arch_counter_read,
	.mask	= CLOCKSOURCE_MASK(56),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS | CLOCK_SOURCE_SUSPEND_NONSTOP,
};

static struct cyclecounter cyclecounter = {
	.read	= arch_counter_read_cc,
	.mask	= CLOCKSOURCE_MASK(56),
};

static struct arch_timer_kvm_info arch_timer_kvm_info;

struct arch_timer_kvm_info *arch_timer_get_kvm_info(void)
{
	return &arch_timer_kvm_info;
}

static void __init arch_counter_register(unsigned type)
{
	u64 start_count;

	/* Register the CP15 based counter if we have one */
	if (type & ARCH_CP15_TIMER) {
		if (IS_ENABLED(CONFIG_ARM64) || arch_timer_uses_ppi == VIRT_PPI)
			arch_timer_read_counter = arch_counter_get_cntvct;
		else
			arch_timer_read_counter = arch_counter_get_cntpct;

		clocksource_counter.archdata.vdso_direct = true;

#if IS_ENABLED(CONFIG_FSL_ERRATUM_A008585) || IS_ENABLED(CONFIG_HISILICON_ERRATUM_161601)
		/*
		 * Don't use the vdso fastpath if errata require using
		 * the out-of-line counter accessor.
		 */
		if (static_branch_unlikely(&arch_timer_read_ool_enabled))
			clocksource_counter.archdata.vdso_direct = false;
#endif
	} else {
		arch_timer_read_counter = arch_counter_get_cntvct_mem;
	}

	start_count = arch_timer_read_counter();
	clocksource_register_hz(&clocksource_counter, arch_timer_rate);
	cyclecounter.mult = clocksource_counter.mult;
	cyclecounter.shift = clocksource_counter.shift;
	timecounter_init(&arch_timer_kvm_info.timecounter,
			 &cyclecounter, start_count);

	/* 56 bits minimum, so we assume worst case rollover */
	sched_clock_register(arch_timer_read_counter, 56, arch_timer_rate);
}

static void arch_timer_stop(struct clock_event_device *clk)
{
	pr_debug("disable IRQ%d cpu #%d\n", clk->irq, smp_processor_id());

	disable_percpu_irq(arch_timer_ppi[arch_timer_uses_ppi]);
	if (arch_timer_has_nonsecure_ppi())
		disable_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI]);

	clk->set_state_shutdown(clk);
}

static int arch_timer_dying_cpu(unsigned int cpu)
{
	struct clock_event_device *clk = this_cpu_ptr(arch_timer_evt);

	arch_timer_stop(clk);
	return 0;
}

#ifdef CONFIG_CPU_PM
static unsigned int saved_cntkctl;
static int arch_timer_cpu_pm_notify(struct notifier_block *self,
				    unsigned long action, void *hcpu)
{
	if (action == CPU_PM_ENTER)
		saved_cntkctl = arch_timer_get_cntkctl();
	else if (action == CPU_PM_ENTER_FAILED || action == CPU_PM_EXIT)
		arch_timer_set_cntkctl(saved_cntkctl);
	return NOTIFY_OK;
}

static struct notifier_block arch_timer_cpu_pm_notifier = {
	.notifier_call = arch_timer_cpu_pm_notify,
};

static int __init arch_timer_cpu_pm_init(void)
{
	return cpu_pm_register_notifier(&arch_timer_cpu_pm_notifier);
}

static void __init arch_timer_cpu_pm_deinit(void)
{
	WARN_ON(cpu_pm_unregister_notifier(&arch_timer_cpu_pm_notifier));
}

#else
static int __init arch_timer_cpu_pm_init(void)
{
	return 0;
}

static void __init arch_timer_cpu_pm_deinit(void)
{
}
#endif

static int __init arch_timer_register(void)
{
	int err;
	int ppi;

	arch_timer_evt = alloc_percpu(struct clock_event_device);
	if (!arch_timer_evt) {
		err = -ENOMEM;
		goto out;
	}

	ppi = arch_timer_ppi[arch_timer_uses_ppi];
	switch (arch_timer_uses_ppi) {
	case VIRT_PPI:
		err = request_percpu_irq(ppi, arch_timer_handler_virt,
					 "arch_timer", arch_timer_evt);
		break;
	case PHYS_SECURE_PPI:
	case PHYS_NONSECURE_PPI:
		err = request_percpu_irq(ppi, arch_timer_handler_phys,
					 "arch_timer", arch_timer_evt);
		if (!err && arch_timer_ppi[PHYS_NONSECURE_PPI]) {
			ppi = arch_timer_ppi[PHYS_NONSECURE_PPI];
			err = request_percpu_irq(ppi, arch_timer_handler_phys,
						 "arch_timer", arch_timer_evt);
			if (err)
				free_percpu_irq(arch_timer_ppi[PHYS_SECURE_PPI],
						arch_timer_evt);
		}
		break;
	case HYP_PPI:
		err = request_percpu_irq(ppi, arch_timer_handler_phys,
					 "arch_timer", arch_timer_evt);
		break;
	default:
		BUG();
	}

	if (err) {
		pr_err("can't register interrupt %d (%d)\n", ppi, err);
		goto out_free;
	}

	err = arch_timer_cpu_pm_init();
	if (err)
		goto out_unreg_notify;


	/* Register and immediately configure the timer on the boot CPU */
	err = cpuhp_setup_state(CPUHP_AP_ARM_ARCH_TIMER_STARTING,
				"AP_ARM_ARCH_TIMER_STARTING",
				arch_timer_starting_cpu, arch_timer_dying_cpu);
	if (err)
		goto out_unreg_cpupm;
	return 0;

out_unreg_cpupm:
	arch_timer_cpu_pm_deinit();

out_unreg_notify:
	free_percpu_irq(arch_timer_ppi[arch_timer_uses_ppi], arch_timer_evt);
	if (arch_timer_has_nonsecure_ppi())
		free_percpu_irq(arch_timer_ppi[PHYS_NONSECURE_PPI],
				arch_timer_evt);

out_free:
	free_percpu(arch_timer_evt);
out:
	return err;
}

static int __init arch_timer_mem_register(struct device_node *np, void *frame)
{
	struct device_node *frame_node = NULL;
	struct gt_timer_data *frame_data = NULL;
	struct arch_timer *t;
	void __iomem *base;
	irq_handler_t func;
	unsigned int irq;
	int ret;

	if (!frame)
		return -EINVAL;

	if (np) {
		frame_node = (struct device_node *)frame;
		base = of_iomap(frame_node, 0);
		arch_timer_detect_rate(base, np);
		if (arch_timer_mem_use_virtual)
			irq = irq_of_parse_and_map(frame_node, VIRT_SPI);
		else
			irq = irq_of_parse_and_map(frame_node, PHYS_SPI);
	} else {
		frame_data = (struct gt_timer_data *)frame;
		/*
		 * According to ARMv8 Architecture Reference Manual(ARM),
		 * the size of CNTBaseN frames of memory-mapped timer
		 * is SZ_4K(Offset 0x000 – 0xFFF).
		 */
		base = ioremap(frame_data->cntbase_phy, SZ_4K);
		if (arch_timer_mem_use_virtual)
			irq = frame_data->virtual_irq;
		else
			irq = frame_data->irq;
	}

	if (!base) {
		pr_err("Can't map frame's registers\n");
		return -ENXIO;
	}
	if (!irq) {
		pr_err("Frame missing %s irq",
		       arch_timer_mem_use_virtual ? "virt" : "phys");
		ret = -EINVAL;
		goto out;
	}

	arch_counter_base = base;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		ret = -ENOMEM;
		goto out;
	}

	t->base = base;
	t->evt.irq = irq;
	__arch_timer_setup(ARCH_MEM_TIMER, &t->evt);

	if (arch_timer_mem_use_virtual)
		func = arch_timer_handler_virt_mem;
	else
		func = arch_timer_handler_phys_mem;

	ret = request_irq(irq, func, IRQF_TIMER, "arch_mem_timer", &t->evt);
	if (!ret)
		return 0;

	pr_err("Failed to request mem timer irq\n");
	kfree(t);
out:
	iounmap(base);
	return ret;
}

static const struct of_device_id arch_timer_of_match[] __initconst = {
	{ .compatible   = "arm,armv7-timer",    },
	{ .compatible   = "arm,armv8-timer",    },
	{},
};

static const struct of_device_id arch_timer_mem_of_match[] __initconst = {
	{ .compatible   = "arm,armv7-timer-mem", },
	{},
};

static bool __init
arch_timer_needs_probing(int type, const struct of_device_id *matches)
{
	struct device_node *dn;
	bool needs_probing = false;

	dn = of_find_matching_node(NULL, matches);
	if (dn && of_device_is_available(dn) && !(arch_timers_present & type))
		needs_probing = true;
	of_node_put(dn);

	return needs_probing;
}

static int __init arch_timer_common_init(void)
{
	unsigned mask = ARCH_CP15_TIMER | ARCH_MEM_TIMER;

	/* Wait until both nodes are probed if we have two timers */
	if ((arch_timers_present & mask) != mask) {
		if (arch_timer_needs_probing(ARCH_MEM_TIMER, arch_timer_mem_of_match))
			return 0;
		if (arch_timer_needs_probing(ARCH_CP15_TIMER, arch_timer_of_match))
			return 0;
	}

	arch_timer_banner(arch_timers_present);
	arch_counter_register(arch_timers_present);
	return arch_timer_arch_init();
}

static int __init arch_timer_init(void)
{
	int ret;
	/*
	 * If HYP mode is available, we know that the physical timer
	 * has been configured to be accessible from PL1. Use it, so
	 * that a guest can use the virtual timer instead.
	 *
	 * If no interrupt provided for virtual timer, we'll have to
	 * stick to the physical timer. It'd better be accessible...
	 *
	 * On ARMv8.1 with VH extensions, the kernel runs in HYP. VHE
	 * accesses to CNTP_*_EL1 registers are silently redirected to
	 * their CNTHP_*_EL2 counterparts, and use a different PPI
	 * number.
	 */
	if (is_hyp_mode_available() || !arch_timer_ppi[VIRT_PPI]) {
		bool has_ppi;

		if (is_kernel_in_hyp_mode()) {
			arch_timer_uses_ppi = HYP_PPI;
			has_ppi = !!arch_timer_ppi[HYP_PPI];
		} else {
			arch_timer_uses_ppi = PHYS_SECURE_PPI;
			has_ppi = (!!arch_timer_ppi[PHYS_SECURE_PPI] ||
				   !!arch_timer_ppi[PHYS_NONSECURE_PPI]);
		}

		if (!has_ppi) {
			pr_warn("No interrupt available, giving up\n");
			return -EINVAL;
		}
	}

	ret = arch_timer_register();
	if (ret)
		return ret;

	ret = arch_timer_common_init();
	if (ret)
		return ret;

	arch_timer_kvm_info.virtual_irq = arch_timer_ppi[VIRT_PPI];

	return 0;
}

static int __init arch_timer_of_init(struct device_node *np)
{
	int i;

	if (arch_timers_present & ARCH_CP15_TIMER) {
		pr_warn("multiple nodes in dt, skipping\n");
		return 0;
	}

	arch_timers_present |= ARCH_CP15_TIMER;
	for (i = PHYS_SECURE_PPI; i < MAX_TIMER_PPI; i++)
		arch_timer_ppi[i] = irq_of_parse_and_map(np, i);

	arch_timer_detect_rate(NULL, np);

	arch_timer_c3stop = !of_property_read_bool(np, "always-on");

#ifdef CONFIG_FSL_ERRATUM_A008585
	if (!timer_unstable_counter_workaround && of_property_read_bool(np, "fsl,erratum-a008585"))
		timer_unstable_counter_workaround = &arch_timer_fsl_a008585;
#endif

#ifdef CONFIG_HISILICON_ERRATUM_161601
	if (!timer_unstable_counter_workaround && of_property_read_bool(np, "hisilicon,erratum-161601"))
		timer_unstable_counter_workaround = &arch_timer_hisi_161601;
#endif

#if IS_ENABLED(CONFIG_FSL_ERRATUM_A008585) || IS_ENABLED(CONFIG_HISILICON_ERRATUM_161601)
	if (timer_unstable_counter_workaround) {
		static_branch_enable(&arch_timer_read_ool_enabled);
		pr_info("Enabling workaround for %s\n",
			timer_unstable_counter_workaround->erratum == FSL_A008585 ?
			"FSL ERRATUM A-008585" : "HISILICON ERRATUM 161601");
	}
#endif

	/*
	 * If we cannot rely on firmware initializing the timer registers then
	 * we should use the physical timers instead.
	 */
	if (IS_ENABLED(CONFIG_ARM) &&
	    of_property_read_bool(np, "arm,cpu-registers-not-fw-configured"))
		arch_timer_uses_ppi = PHYS_SECURE_PPI;

	return arch_timer_init();
}
CLOCKSOURCE_OF_DECLARE(armv7_arch_timer, "arm,armv7-timer", arch_timer_of_init);
CLOCKSOURCE_OF_DECLARE(armv8_arch_timer, "arm,armv8-timer", arch_timer_of_init);

static int __init get_cnttidr(struct device_node *np,
			      struct gt_block_data *gt_block, u32 *cnttidr)
{
	if (!cnttidr)
		return -EINVAL;

	if (np)
		cntctlbase = of_iomap(np, 0);
	else if (gt_block)
		cntctlbase = ioremap(gt_block->cntctlbase_phy, SZ_4K);
	else
		return -EINVAL;

	if (!cntctlbase) {
		pr_err("Can't find CNTCTLBase\n");
		return -ENXIO;
	}

	*cnttidr = readl_relaxed(cntctlbase + CNTTIDR);
	return 0;
}

static bool __init is_best_frame(u32 cnttidr, int n)
{
	u32 cntacr = CNTACR_RFRQ | CNTACR_RWPT | CNTACR_RPCT | CNTACR_RWVT |
		     CNTACR_RVOFF | CNTACR_RVCT;

	/* Try enabling everything, and see what sticks */
	writel_relaxed(cntacr, cntctlbase + CNTACR(n));
	cntacr = readl_relaxed(cntctlbase + CNTACR(n));

	if ((cnttidr & CNTTIDR_VIRT(n)) &&
	    !(~cntacr & (CNTACR_RWVT | CNTACR_RVCT)))
		arch_timer_mem_use_virtual = true;
	else if (~cntacr & (CNTACR_RWPT | CNTACR_RPCT))
		return false;

	return true;
}

static int __init arch_timer_mem_init(struct device_node *np)
{
	struct device_node *frame, *best_frame = NULL;
	unsigned int ret;
	u32 cnttidr;

	arch_timers_present |= ARCH_MEM_TIMER;

	ret = get_cnttidr(np, NULL, &cnttidr);
	if (ret)
		return ret;

	/*
	 * Try to find a virtual capable frame. Otherwise fall back to a
	 * physical capable frame.
	 */
	for_each_available_child_of_node(np, frame) {
		int n;
		if (of_property_read_u32(frame, "frame-number", &n)) {
			pr_err("Missing frame-number.\n");
			of_node_put(frame);
			ret = -EINVAL;
			goto out;
		}
		if (is_best_frame(cnttidr, n)) {
			of_node_put(best_frame);
			best_frame = of_node_get(frame);
			if (arch_timer_mem_use_virtual)
				break;
		}
	}

	ret = arch_timer_mem_register(np, best_frame);
	if (!ret)
		ret = arch_timer_common_init();
out:
	iounmap(cntctlbase);
	of_node_put(best_frame);
	return ret;
}
CLOCKSOURCE_OF_DECLARE(armv7_arch_timer_mem, "arm,armv7-timer-mem",
		       arch_timer_mem_init);

#ifdef CONFIG_ACPI_GTDT
struct gtdt_arch_timer_fixup {
	char oem_id[ACPI_OEM_ID_SIZE + 1];
	char oem_table_id[ACPI_OEM_TABLE_ID_SIZE + 1];
	u32 oem_revision;

	/* quirk handler for arch timer erratum */
	void (*handler)(void *context);
	void *context;
};

#ifdef CONFIG_HISILICON_ERRATUM_161601
static void __init erratum_workaround_enable(void *context)
{
	u64 erratum = (u64) context;

	if (erratum & HISILICON_161601) {
		timer_unstable_counter_workaround = &arch_timer_hisi_161601;
		static_branch_enable(&arch_timer_read_ool_enabled);
		pr_info("Enabling workaround for HISILICON ERRATUM 161601\n");
	}
}
#endif

/* note: this needs to be updated according to the doc of OEM ID
 * and TABLE ID for different board.
 */
struct gtdt_arch_timer_fixup arch_timer_quirks[] __initdata = {
#ifdef CONFIG_HISILICON_ERRATUM_161601
	{"HISI  ", "HIP05   ", 0, &erratum_workaround_enable, (void *) HISILICON_161601},
	{"HISI  ", "HIP06   ", 0, &erratum_workaround_enable, (void *) HISILICON_161601},
	{"HISI  ", "HIP07   ", 0, &erratum_workaround_enable, (void *) HISILICON_161601},
#endif
};

void __init arch_timer_acpi_quirks_handler(char *oem_id,
					   char *oem_table_id,
					   u32 oem_revision)
{
	struct gtdt_arch_timer_fixup *quirks = arch_timer_quirks;
	int i;

	for (i = 0; i < ARRAY_SIZE(arch_timer_quirks); i++, quirks++) {
		if (!memcmp(quirks->oem_id, oem_id, ACPI_OEM_ID_SIZE) &&
		    !memcmp(quirks->oem_table_id, oem_table_id, ACPI_OEM_TABLE_ID_SIZE) &&
		    quirks->oem_revision == oem_revision) {
			if (quirks->handler && quirks->context)
				quirks->handler(quirks->context);
		}
	}
}

static struct gt_timer_data __init *arch_timer_mem_get_timer(
						struct gt_block_data *gt_blocks)
{
	struct gt_block_data *gt_block = gt_blocks;
	struct gt_timer_data *best_frame = NULL;
	u32 cnttidr;
	int i;

	if (get_cnttidr(NULL, gt_block, &cnttidr))
		return NULL;
	/*
	 * Try to find a virtual capable frame. Otherwise fall back to a
	 * physical capable frame.
	 */
	for (i = 0; i < gt_block->timer_count; i++) {
		if (is_best_frame(cnttidr, gt_block->timer[i].frame_nr)) {
			best_frame = &gt_block->timer[i];
			if (arch_timer_mem_use_virtual)
				break;
		}
	}
	iounmap(cntctlbase);

	return best_frame;
}

static int __init arch_timer_mem_acpi_init(size_t timer_count)
{
	struct gt_block_data *gt_blocks;
	struct gt_timer_data *gt_timer;
	int ret = -EINVAL;

	/*
	 * If we don't have any Platform Timer Structures, just return.
	 */
	if (!timer_count)
		return 0;

	/*
	 * before really check all the Platform Timer Structures,
	 * we assume they are GT block, and allocate memory for them.
	 * We will free these memory once we finish the initialization.
	 */
	gt_blocks = kcalloc(timer_count, sizeof(*gt_blocks), GFP_KERNEL);
	if (!gt_blocks)
		return -ENOMEM;

	if (gtdt_arch_timer_mem_init(gt_blocks) > 0) {
		gt_timer = arch_timer_mem_get_timer(gt_blocks);
		if (!gt_timer) {
			pr_err("Failed to get mem timer info.\n");
			goto error;
		}
		ret = arch_timer_mem_register(NULL, gt_timer);
		if (ret) {
			pr_err("Failed to register mem timer.\n");
			goto error;
		}
	}
	arch_timers_present |= ARCH_MEM_TIMER;
error:
	kfree(gt_blocks);
	return ret;
}

/* Initialize per-processor generic timer and memory-mapped timer(if present) */
static int __init arch_timer_acpi_init(struct acpi_table_header *table)
{
	int timer_count;

	if (arch_timers_present & ARCH_CP15_TIMER) {
		pr_warn("already initialized, skipping\n");
		return -EINVAL;
	}

	arch_timer_acpi_quirks_handler(table->oem_id, table->oem_table_id,
				       table->oem_revision);

	arch_timers_present |= ARCH_CP15_TIMER;

	timer_count = acpi_gtdt_init(table);

	arch_timer_ppi[PHYS_SECURE_PPI] = acpi_gtdt_map_ppi(PHYS_SECURE_PPI);
	arch_timer_ppi[PHYS_NONSECURE_PPI] = acpi_gtdt_map_ppi(PHYS_NONSECURE_PPI);
	arch_timer_ppi[VIRT_PPI] = acpi_gtdt_map_ppi(VIRT_PPI);
	arch_timer_ppi[HYP_PPI] = acpi_gtdt_map_ppi(HYP_PPI);
	/* Always-on capability */
	arch_timer_c3stop = acpi_gtdt_c3stop();

	/* Get the frequency from CNTFRQ */
	arch_timer_detect_rate(NULL, NULL);

	if (timer_count < 0 || arch_timer_mem_acpi_init((size_t)timer_count))
		pr_err("Failed to initialize memory-mapped timer.\n");

	return arch_timer_init();
}
CLOCKSOURCE_ACPI_DECLARE(arch_timer, ACPI_SIG_GTDT, arch_timer_acpi_init);
#endif
