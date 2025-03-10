/*
 * GTT virtualization
 *
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Zhi Wang <zhi.a.wang@intel.com>
 *    Zhenyu Wang <zhenyuw@linux.intel.com>
 *    Xiao Zheng <xiao.zheng@intel.com>
 *
 * Contributors:
 *    Min He <min.he@intel.com>
 *    Bing Niu <bing.niu@intel.com>
 *
 */

#include <linux/stop_machine.h>
#include "i915_drv.h"
#include "gvt.h"
#include "i915_pvinfo.h"
#include "trace.h"

static int preallocated_oos_pages = 8192;

/*
 * validate a gm address and related range size,
 * translate it to host gm address
 */
bool intel_gvt_ggtt_validate_range(struct intel_vgpu *vgpu, u64 addr, u32 size)
{
	if ((!vgpu_gmadr_is_valid(vgpu, addr)) || (size
			&& !vgpu_gmadr_is_valid(vgpu, addr + size - 1))) {
		gvt_vgpu_err("invalid range gmadr 0x%llx size 0x%x\n",
				addr, size);
		return false;
	}
	return true;
}

/* translate a guest gmadr to host gmadr */
int intel_gvt_ggtt_gmadr_g2h(struct intel_vgpu *vgpu, u64 g_addr, u64 *h_addr)
{
	if (WARN(!vgpu_gmadr_is_valid(vgpu, g_addr),
		 "invalid guest gmadr %llx\n", g_addr))
		return -EACCES;

	if (vgpu_gmadr_is_aperture(vgpu, g_addr))
		*h_addr = vgpu_aperture_gmadr_base(vgpu)
			  + (g_addr - vgpu_aperture_offset(vgpu));
	else
		*h_addr = vgpu_hidden_gmadr_base(vgpu)
			  + (g_addr - vgpu_hidden_offset(vgpu));
	return 0;
}

/* translate a host gmadr to guest gmadr */
int intel_gvt_ggtt_gmadr_h2g(struct intel_vgpu *vgpu, u64 h_addr, u64 *g_addr)
{
	if (WARN(!gvt_gmadr_is_valid(vgpu->gvt, h_addr),
		 "invalid host gmadr %llx\n", h_addr))
		return -EACCES;

	if (gvt_gmadr_is_aperture(vgpu->gvt, h_addr))
		*g_addr = vgpu_aperture_gmadr_base(vgpu)
			+ (h_addr - gvt_aperture_gmadr_base(vgpu->gvt));
	else
		*g_addr = vgpu_hidden_gmadr_base(vgpu)
			+ (h_addr - gvt_hidden_gmadr_base(vgpu->gvt));
	return 0;
}

int intel_gvt_ggtt_index_g2h(struct intel_vgpu *vgpu, unsigned long g_index,
			     unsigned long *h_index)
{
	u64 h_addr;
	int ret;

	ret = intel_gvt_ggtt_gmadr_g2h(vgpu, g_index << GTT_PAGE_SHIFT,
				       &h_addr);
	if (ret)
		return ret;

	*h_index = h_addr >> GTT_PAGE_SHIFT;
	return 0;
}

int intel_gvt_ggtt_h2g_index(struct intel_vgpu *vgpu, unsigned long h_index,
			     unsigned long *g_index)
{
	u64 g_addr;
	int ret;

	ret = intel_gvt_ggtt_gmadr_h2g(vgpu, h_index << GTT_PAGE_SHIFT,
				       &g_addr);
	if (ret)
		return ret;

	*g_index = g_addr >> GTT_PAGE_SHIFT;
	return 0;
}

#define gtt_type_is_entry(type) \
	(type > GTT_TYPE_INVALID && type < GTT_TYPE_PPGTT_ENTRY \
	 && type != GTT_TYPE_PPGTT_PTE_ENTRY \
	 && type != GTT_TYPE_PPGTT_ROOT_ENTRY)

#define gtt_type_is_pt(type) \
	(type >= GTT_TYPE_PPGTT_PTE_PT && type < GTT_TYPE_MAX)

#define gtt_type_is_pte_pt(type) \
	(type == GTT_TYPE_PPGTT_PTE_PT)

#define gtt_type_is_root_pointer(type) \
	(gtt_type_is_entry(type) && type > GTT_TYPE_PPGTT_ROOT_ENTRY)

#define gtt_init_entry(e, t, p, v) do { \
	(e)->type = t; \
	(e)->pdev = p; \
	memcpy(&(e)->val64, &v, sizeof(v)); \
} while (0)

/*
 * Mappings between GTT_TYPE* enumerations.
 * Following information can be found according to the given type:
 * - type of next level page table
 * - type of entry inside this level page table
 * - type of entry with PSE set
 *
 * If the given type doesn't have such a kind of information,
 * e.g. give a l4 root entry type, then request to get its PSE type,
 * give a PTE page table type, then request to get its next level page
 * table type, as we know l4 root entry doesn't have a PSE bit,
 * and a PTE page table doesn't have a next level page table type,
 * GTT_TYPE_INVALID will be returned. This is useful when traversing a
 * page table.
 */

struct gtt_type_table_entry {
	int entry_type;
	int next_pt_type;
	int pse_entry_type;
};

#define GTT_TYPE_TABLE_ENTRY(type, e_type, npt_type, pse_type) \
	[type] = { \
		.entry_type = e_type, \
		.next_pt_type = npt_type, \
		.pse_entry_type = pse_type, \
	}

static struct gtt_type_table_entry gtt_type_table[] = {
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_ROOT_L4_ENTRY,
			GTT_TYPE_PPGTT_ROOT_L4_ENTRY,
			GTT_TYPE_PPGTT_PML4_PT,
			GTT_TYPE_INVALID),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PML4_PT,
			GTT_TYPE_PPGTT_PML4_ENTRY,
			GTT_TYPE_PPGTT_PDP_PT,
			GTT_TYPE_INVALID),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PML4_ENTRY,
			GTT_TYPE_PPGTT_PML4_ENTRY,
			GTT_TYPE_PPGTT_PDP_PT,
			GTT_TYPE_INVALID),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PDP_PT,
			GTT_TYPE_PPGTT_PDP_ENTRY,
			GTT_TYPE_PPGTT_PDE_PT,
			GTT_TYPE_PPGTT_PTE_1G_ENTRY),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_ROOT_L3_ENTRY,
			GTT_TYPE_PPGTT_ROOT_L3_ENTRY,
			GTT_TYPE_PPGTT_PDE_PT,
			GTT_TYPE_PPGTT_PTE_1G_ENTRY),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PDP_ENTRY,
			GTT_TYPE_PPGTT_PDP_ENTRY,
			GTT_TYPE_PPGTT_PDE_PT,
			GTT_TYPE_PPGTT_PTE_1G_ENTRY),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PDE_PT,
			GTT_TYPE_PPGTT_PDE_ENTRY,
			GTT_TYPE_PPGTT_PTE_PT,
			GTT_TYPE_PPGTT_PTE_2M_ENTRY),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PDE_ENTRY,
			GTT_TYPE_PPGTT_PDE_ENTRY,
			GTT_TYPE_PPGTT_PTE_PT,
			GTT_TYPE_PPGTT_PTE_2M_ENTRY),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PTE_PT,
			GTT_TYPE_PPGTT_PTE_4K_ENTRY,
			GTT_TYPE_INVALID,
			GTT_TYPE_INVALID),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PTE_4K_ENTRY,
			GTT_TYPE_PPGTT_PTE_4K_ENTRY,
			GTT_TYPE_INVALID,
			GTT_TYPE_INVALID),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PTE_2M_ENTRY,
			GTT_TYPE_PPGTT_PDE_ENTRY,
			GTT_TYPE_INVALID,
			GTT_TYPE_PPGTT_PTE_2M_ENTRY),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_PPGTT_PTE_1G_ENTRY,
			GTT_TYPE_PPGTT_PDP_ENTRY,
			GTT_TYPE_INVALID,
			GTT_TYPE_PPGTT_PTE_1G_ENTRY),
	GTT_TYPE_TABLE_ENTRY(GTT_TYPE_GGTT_PTE,
			GTT_TYPE_GGTT_PTE,
			GTT_TYPE_INVALID,
			GTT_TYPE_INVALID),
};

static inline int get_next_pt_type(int type)
{
	return gtt_type_table[type].next_pt_type;
}

static inline int get_entry_type(int type)
{
	return gtt_type_table[type].entry_type;
}

static inline int get_pse_type(int type)
{
	return gtt_type_table[type].pse_entry_type;
}

static u64 read_pte64(struct drm_i915_private *dev_priv, unsigned long index)
{
	void __iomem *addr = (gen8_pte_t __iomem *)dev_priv->ggtt.gsm + index;

	return readq(addr);
}

static void gtt_invalidate(struct drm_i915_private *dev_priv)
{
	mmio_hw_access_pre(dev_priv);
	I915_WRITE(GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN);
	mmio_hw_access_post(dev_priv);
}

static void write_pte64(struct drm_i915_private *dev_priv,
		unsigned long index, u64 pte)
{
	void __iomem *addr = (gen8_pte_t __iomem *)dev_priv->ggtt.gsm + index;

	writeq(pte, addr);
}

static inline int gtt_get_entry64(void *pt,
		struct intel_gvt_gtt_entry *e,
		unsigned long index, bool hypervisor_access, unsigned long gpa,
		struct intel_vgpu *vgpu)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	int ret;

	if (WARN_ON(info->gtt_entry_size != 8))
		return -EINVAL;

	if (hypervisor_access) {
		if (vgpu->ge_cache_enable && vgpu->cached_guest_entry) {
			if (index == 0) {
				ret = intel_gvt_hypervisor_read_gpa(vgpu, gpa,
				      vgpu->cached_guest_entry, GTT_PAGE_SIZE);
				if (WARN_ON(ret))
					return ret;
			}
			e->val64 = *(vgpu->cached_guest_entry + index);
			return 0;

		}
		ret = intel_gvt_hypervisor_read_gpa(vgpu, gpa +
				(index << info->gtt_entry_size_shift),
				&e->val64, 8);
		if (WARN_ON(ret))
			return ret;
	} else if (!pt) {
		e->val64 = read_pte64(vgpu->gvt->dev_priv, index);
	} else {
		e->val64 = *((u64 *)pt + index);
	}
	return 0;
}

struct ggtt_entry64 {
	void *pt;
	struct intel_gvt_gtt_entry *e;
	unsigned long index;
	bool hypervisor_access;
	unsigned long gpa;
	struct intel_vgpu *vgpu;
};

#ifdef CONFIG_INTEL_IOMMU
static int gtt_get_entry64__cb(void *_arg)
{
	struct ggtt_entry64 *arg = _arg;
	int ret = 0;

	gvt_pause_user_domains(arg->vgpu->gvt->dev_priv);
	ret = gtt_get_entry64(arg->pt, arg->e, arg->index,
			arg->hypervisor_access, arg->gpa, arg->vgpu);
	gvt_unpause_user_domains(arg->vgpu->gvt->dev_priv);

	return ret;
}
#endif

static inline int gtt_get_entry64__BKL(void *pt,
		struct intel_gvt_gtt_entry *e,
		unsigned long index, bool hypervisor_access, unsigned long gpa,
		struct intel_vgpu *vgpu)
{
#ifdef CONFIG_INTEL_IOMMU
	struct ggtt_entry64 arg = { pt, e, index, hypervisor_access, gpa, vgpu };

	if (!intel_iommu_gfx_mapped || !IS_BROXTON(vgpu->gvt->dev_priv) ||
				hypervisor_access || pt) {
		return gtt_get_entry64(pt, e, index, hypervisor_access, gpa, vgpu);
	} else {
		stop_machine(gtt_get_entry64__cb, &arg, NULL);
		return 0;
	}
#else
	return gtt_get_entry64(pt, e, index, hypervisor_access, gpa, vgpu);
#endif
}

static inline int gtt_set_entry64(void *pt,
		struct intel_gvt_gtt_entry *e,
		unsigned long index, bool hypervisor_access, unsigned long gpa,
		struct intel_vgpu *vgpu)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	int ret;

	if (WARN_ON(info->gtt_entry_size != 8))
		return -EINVAL;

	if (hypervisor_access) {
		ret = intel_gvt_hypervisor_write_gpa(vgpu, gpa +
				(index << info->gtt_entry_size_shift),
				&e->val64, 8);
		if (WARN_ON(ret))
			return ret;
	} else if (!pt) {
		write_pte64(vgpu->gvt->dev_priv, index, e->val64);
	} else {
		*((u64 *)pt + index) = e->val64;
	}
	return 0;
}

#ifdef CONFIG_INTEL_IOMMU
static int gtt_set_entry64__cb(void *_arg)
{
	struct ggtt_entry64 *arg = _arg;
	int ret;

	gvt_pause_user_domains(arg->vgpu->gvt->dev_priv);
	ret = gtt_set_entry64(arg->pt, arg->e, arg->index, arg->hypervisor_access,
				arg->gpa, arg->vgpu);
	gvt_unpause_user_domains(arg->vgpu->gvt->dev_priv);

	return ret;
}
#endif

static inline int gtt_set_entry64__BKL(void *pt,
		struct intel_gvt_gtt_entry *e,
		unsigned long index, bool hypervisor_access, unsigned long gpa,
		struct intel_vgpu *vgpu)
{
#ifdef CONFIG_INTEL_IOMMU
	struct ggtt_entry64 arg = { pt, e, index, hypervisor_access, gpa, vgpu };

	if (!intel_iommu_gfx_mapped || !IS_BROXTON(vgpu->gvt->dev_priv) ||
				hypervisor_access || pt) {
		return gtt_set_entry64(pt, e, index, hypervisor_access, gpa, vgpu);
	} else {
		stop_machine(gtt_set_entry64__cb, &arg, NULL);
		return 0;
	}
#else
	return gtt_set_entry64(pt, e, index, hypervisor_access, gpa, vgpu);
#endif
}

#define GTT_HAW 46

#define ADDR_1G_MASK (((1UL << (GTT_HAW - 30)) - 1) << 30)
#define ADDR_2M_MASK (((1UL << (GTT_HAW - 21)) - 1) << 21)
#define ADDR_4K_MASK (((1UL << (GTT_HAW - 12)) - 1) << 12)

static unsigned long gen8_gtt_get_pfn(struct intel_gvt_gtt_entry *e)
{
	unsigned long pfn;

	if (e->type == GTT_TYPE_PPGTT_PTE_1G_ENTRY)
		pfn = (e->val64 & ADDR_1G_MASK) >> 12;
	else if (e->type == GTT_TYPE_PPGTT_PTE_2M_ENTRY)
		pfn = (e->val64 & ADDR_2M_MASK) >> 12;
	else
		pfn = (e->val64 & ADDR_4K_MASK) >> 12;
	return pfn;
}

static void gen8_gtt_set_pfn(struct intel_gvt_gtt_entry *e, unsigned long pfn)
{
	if (e->type == GTT_TYPE_PPGTT_PTE_1G_ENTRY) {
		e->val64 &= ~ADDR_1G_MASK;
		pfn &= (ADDR_1G_MASK >> 12);
	} else if (e->type == GTT_TYPE_PPGTT_PTE_2M_ENTRY) {
		e->val64 &= ~ADDR_2M_MASK;
		pfn &= (ADDR_2M_MASK >> 12);
	} else {
		e->val64 &= ~ADDR_4K_MASK;
		pfn &= (ADDR_4K_MASK >> 12);
	}

	e->val64 |= (pfn << 12);
}

static bool gen8_gtt_test_pse(struct intel_gvt_gtt_entry *e)
{
	/* Entry doesn't have PSE bit. */
	if (get_pse_type(e->type) == GTT_TYPE_INVALID)
		return false;

	e->type = get_entry_type(e->type);
	if (!(e->val64 & (1 << 7)))
		return false;

	e->type = get_pse_type(e->type);
	return true;
}

static bool gen8_gtt_test_present(struct intel_gvt_gtt_entry *e)
{
	/*
	 * i915 writes PDP root pointer registers without present bit,
	 * it also works, so we need to treat root pointer entry
	 * specifically.
	 */
	if (e->type == GTT_TYPE_PPGTT_ROOT_L3_ENTRY
			|| e->type == GTT_TYPE_PPGTT_ROOT_L4_ENTRY)
		return (e->val64 != 0);
	else
		return (e->val64 & (1 << 0));
}

static void gtt_entry_clear_present(struct intel_gvt_gtt_entry *e)
{
	e->val64 &= ~(1 << 0);
}

/*
 * Per-platform GMA routines.
 */
static unsigned long gma_to_ggtt_pte_index(unsigned long gma)
{
	unsigned long x = (gma >> GTT_PAGE_SHIFT);

	trace_gma_index(__func__, gma, x);
	return x;
}

#define DEFINE_PPGTT_GMA_TO_INDEX(prefix, ename, exp) \
static unsigned long prefix##_gma_to_##ename##_index(unsigned long gma) \
{ \
	unsigned long x = (exp); \
	trace_gma_index(__func__, gma, x); \
	return x; \
}

DEFINE_PPGTT_GMA_TO_INDEX(gen8, pte, (gma >> 12 & 0x1ff));
DEFINE_PPGTT_GMA_TO_INDEX(gen8, pde, (gma >> 21 & 0x1ff));
DEFINE_PPGTT_GMA_TO_INDEX(gen8, l3_pdp, (gma >> 30 & 0x3));
DEFINE_PPGTT_GMA_TO_INDEX(gen8, l4_pdp, (gma >> 30 & 0x1ff));
DEFINE_PPGTT_GMA_TO_INDEX(gen8, pml4, (gma >> 39 & 0x1ff));

static struct intel_gvt_gtt_pte_ops gen8_gtt_pte_ops = {
	.get_entry = gtt_get_entry64__BKL,
	.set_entry = gtt_set_entry64__BKL,
	.clear_present = gtt_entry_clear_present,
	.test_present = gen8_gtt_test_present,
	.test_pse = gen8_gtt_test_pse,
	.get_pfn = gen8_gtt_get_pfn,
	.set_pfn = gen8_gtt_set_pfn,
};

static struct intel_gvt_gtt_gma_ops gen8_gtt_gma_ops = {
	.gma_to_ggtt_pte_index = gma_to_ggtt_pte_index,
	.gma_to_pte_index = gen8_gma_to_pte_index,
	.gma_to_pde_index = gen8_gma_to_pde_index,
	.gma_to_l3_pdp_index = gen8_gma_to_l3_pdp_index,
	.gma_to_l4_pdp_index = gen8_gma_to_l4_pdp_index,
	.gma_to_pml4_index = gen8_gma_to_pml4_index,
};

static int gtt_entry_p2m(struct intel_vgpu *vgpu, struct intel_gvt_gtt_entry *p,
		struct intel_gvt_gtt_entry *m)
{
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	unsigned long gfn, mfn;
	static unsigned long saved_gfn, saved_mfn;

	*m = *p;

	if (!ops->test_present(p))
		return 0;

	gfn = ops->get_pfn(p);

	if (gfn != saved_gfn)
		mfn = intel_gvt_hypervisor_gfn_to_mfn(vgpu, gfn);
	else
		mfn = saved_mfn;

	if (mfn == INTEL_GVT_INVALID_ADDR) {
		gvt_vgpu_err("fail to translate gfn: 0x%lx\n", gfn);
		return -ENXIO;
	}

	ops->set_pfn(m, mfn);
	saved_gfn = gfn;
	saved_mfn = mfn;
	return 0;
}

/*
 * MM helpers.
 */
int intel_vgpu_mm_get_entry(struct intel_vgpu_mm *mm,
		void *page_table, struct intel_gvt_gtt_entry *e,
		unsigned long index)
{
	struct intel_gvt *gvt = mm->vgpu->gvt;
	struct intel_gvt_gtt_pte_ops *ops = gvt->gtt.pte_ops;
	int ret;

	e->type = mm->page_table_entry_type;

	ret = ops->get_entry(page_table, e, index, false, 0, mm->vgpu);
	if (ret)
		return ret;

	ops->test_pse(e);
	return 0;
}

int intel_vgpu_mm_set_entry(struct intel_vgpu_mm *mm,
		void *page_table, struct intel_gvt_gtt_entry *e,
		unsigned long index)
{
	struct intel_gvt *gvt = mm->vgpu->gvt;
	struct intel_gvt_gtt_pte_ops *ops = gvt->gtt.pte_ops;

	return ops->set_entry(page_table, e, index, false, 0, mm->vgpu);
}

/*
 * PPGTT shadow page table helpers.
 */
static inline int ppgtt_spt_get_entry(
		struct intel_vgpu_ppgtt_spt *spt,
		void *page_table, int type,
		struct intel_gvt_gtt_entry *e, unsigned long index,
		bool guest)
{
	struct intel_gvt *gvt = spt->vgpu->gvt;
	struct intel_gvt_gtt_pte_ops *ops = gvt->gtt.pte_ops;
	int ret;

	e->type = get_entry_type(type);

	if (WARN(!gtt_type_is_entry(e->type), "invalid entry type\n"))
		return -EINVAL;

	ret = ops->get_entry(page_table, e, index, guest,
			spt->guest_page.gfn << GTT_PAGE_SHIFT,
			spt->vgpu);
	if (ret)
		return ret;

	ops->test_pse(e);
	return 0;
}

static inline int ppgtt_spt_set_entry(
		struct intel_vgpu_ppgtt_spt *spt,
		void *page_table, int type,
		struct intel_gvt_gtt_entry *e, unsigned long index,
		bool guest)
{
	struct intel_gvt *gvt = spt->vgpu->gvt;
	struct intel_gvt_gtt_pte_ops *ops = gvt->gtt.pte_ops;

	if (WARN(!gtt_type_is_entry(e->type), "invalid entry type\n"))
		return -EINVAL;

	return ops->set_entry(page_table, e, index, guest,
			spt->guest_page.gfn << GTT_PAGE_SHIFT,
			spt->vgpu);
}

#define ppgtt_get_guest_entry(spt, e, index) \
	ppgtt_spt_get_entry(spt, NULL, \
		spt->guest_page_type, e, index, true)

#define ppgtt_set_guest_entry(spt, e, index) \
	ppgtt_spt_set_entry(spt, NULL, \
		spt->guest_page_type, e, index, true)

#define ppgtt_get_shadow_entry(spt, e, index) \
	ppgtt_spt_get_entry(spt, spt->shadow_page.vaddr, \
		spt->shadow_page.type, e, index, false)

#define ppgtt_set_shadow_entry(spt, e, index) \
	ppgtt_spt_set_entry(spt, spt->shadow_page.vaddr, \
		spt->shadow_page.type, e, index, false)

/**
 * intel_vgpu_init_guest_page - init a guest page data structure
 * @vgpu: a vGPU
 * @p: a guest page data structure
 * @gfn: guest memory page frame number
 * @handler: function will be called when target guest memory page has
 * been modified.
 *
 * This function is called when user wants to track a guest memory page.
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_init_guest_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_guest_page *p,
		unsigned long gfn,
		int (*handler)(void *, u64, void *, int),
		void *data)
{
	INIT_HLIST_NODE(&p->node);

	p->writeprotection = false;
	p->gfn = gfn;
	p->handler = handler;
	p->data = data;
	p->oos_page = NULL;
	p->write_cnt = 0;

	hash_add(vgpu->gtt.guest_page_hash_table, &p->node, p->gfn);
	return 0;
}

static int detach_oos_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_oos_page *oos_page);

/**
 * intel_vgpu_clean_guest_page - release the resource owned by guest page data
 * structure
 * @vgpu: a vGPU
 * @p: a tracked guest page
 *
 * This function is called when user tries to stop tracking a guest memory
 * page.
 */
void intel_vgpu_clean_guest_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_guest_page *p)
{
	if (!hlist_unhashed(&p->node))
		hash_del(&p->node);

	if (p->oos_page)
		detach_oos_page(vgpu, p->oos_page);

	if (p->writeprotection)
		intel_gvt_hypervisor_unset_wp_page(vgpu, p);
}

/**
 * intel_vgpu_find_guest_page - find a guest page data structure by GFN.
 * @vgpu: a vGPU
 * @gfn: guest memory page frame number
 *
 * This function is called when emulation logic wants to know if a trapped GFN
 * is a tracked guest page.
 *
 * Returns:
 * Pointer to guest page data structure, NULL if failed.
 */
struct intel_vgpu_guest_page *intel_vgpu_find_guest_page(
		struct intel_vgpu *vgpu, unsigned long gfn)
{
	struct intel_vgpu_guest_page *p;

	hash_for_each_possible(vgpu->gtt.guest_page_hash_table,
		p, node, gfn) {
		if (p->gfn == gfn)
			return p;
	}
	return NULL;
}

static inline int init_shadow_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_shadow_page *p, int type)
{
	struct device *kdev = &vgpu->gvt->dev_priv->drm.pdev->dev;
	dma_addr_t daddr;

	daddr = dma_map_page(kdev, p->page, 0, 4096, PCI_DMA_BIDIRECTIONAL);
	if (dma_mapping_error(kdev, daddr)) {
		gvt_vgpu_err("fail to map dma addr\n");
		return -EINVAL;
	}

	p->vaddr = page_address(p->page);
	p->type = type;

	INIT_HLIST_NODE(&p->node);

	p->mfn = daddr >> GTT_PAGE_SHIFT;
	hash_add(vgpu->gtt.shadow_page_hash_table, &p->node, p->mfn);
	return 0;
}

static inline void clean_shadow_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_shadow_page *p)
{
	struct device *kdev = &vgpu->gvt->dev_priv->drm.pdev->dev;

	dma_unmap_page(kdev, p->mfn << GTT_PAGE_SHIFT, 4096,
			PCI_DMA_BIDIRECTIONAL);

	if (!hlist_unhashed(&p->node))
		hash_del(&p->node);
}

static inline struct intel_vgpu_shadow_page *find_shadow_page(
		struct intel_vgpu *vgpu, unsigned long mfn)
{
	struct intel_vgpu_shadow_page *p;

	hash_for_each_possible(vgpu->gtt.shadow_page_hash_table,
		p, node, mfn) {
		if (p->mfn == mfn)
			return p;
	}
	return NULL;
}

#define guest_page_to_ppgtt_spt(ptr) \
	container_of(ptr, struct intel_vgpu_ppgtt_spt, guest_page)

#define shadow_page_to_ppgtt_spt(ptr) \
	container_of(ptr, struct intel_vgpu_ppgtt_spt, shadow_page)

static void *alloc_spt(gfp_t gfp_mask)
{
	struct intel_vgpu_ppgtt_spt *spt;

	spt = kzalloc(sizeof(*spt), gfp_mask);
	if (!spt)
		return NULL;

	spt->shadow_page.page = alloc_page(gfp_mask);
	if (!spt->shadow_page.page) {
		kfree(spt);
		return NULL;
	}
	return spt;
}

static void free_spt(struct intel_vgpu_ppgtt_spt *spt)
{
	__free_page(spt->shadow_page.page);
	kfree(spt);
}

static void ppgtt_free_shadow_page(struct intel_vgpu_ppgtt_spt *spt)
{
	trace_spt_free(spt->vgpu->id, spt, spt->shadow_page.type);

	clean_shadow_page(spt->vgpu, &spt->shadow_page);
	intel_vgpu_clean_guest_page(spt->vgpu, &spt->guest_page);
	list_del_init(&spt->post_shadow_list);

	free_spt(spt);
}

static void ppgtt_free_all_shadow_page(struct intel_vgpu *vgpu)
{
	struct hlist_node *n;
	struct intel_vgpu_shadow_page *sp;
	int i;

	hash_for_each_safe(vgpu->gtt.shadow_page_hash_table, i, n, sp, node)
		ppgtt_free_shadow_page(shadow_page_to_ppgtt_spt(sp));
}

static int ppgtt_handle_guest_write_page_table_bytes(void *gp,
		u64 pa, void *p_data, int bytes);

static int ppgtt_write_protection_handler(void *gp, u64 pa,
		void *p_data, int bytes)
{
	struct intel_vgpu_guest_page *gpt = (struct intel_vgpu_guest_page *)gp;
	int ret;

	if (bytes != 4 && bytes != 8)
		return -EINVAL;

	if (!gpt->writeprotection)
		return -EINVAL;

	ret = ppgtt_handle_guest_write_page_table_bytes(gp,
		pa, p_data, bytes);
	if (ret)
		return ret;
	return ret;
}

static int reclaim_one_mm(struct intel_gvt *gvt);

static struct intel_vgpu_ppgtt_spt *ppgtt_alloc_shadow_page(
		struct intel_vgpu *vgpu, int type, unsigned long gfn)
{
	struct intel_vgpu_ppgtt_spt *spt = NULL;
	int ret;

retry:
	spt = alloc_spt(GFP_KERNEL | __GFP_ZERO);
	if (!spt) {
		if (reclaim_one_mm(vgpu->gvt))
			goto retry;

		gvt_vgpu_err("fail to allocate ppgtt shadow page\n");
		return ERR_PTR(-ENOMEM);
	}

	spt->vgpu = vgpu;
	spt->guest_page_type = type;
	atomic_set(&spt->refcount, 1);
	INIT_LIST_HEAD(&spt->post_shadow_list);

	/*
	 * TODO: guest page type may be different with shadow page type,
	 *	 when we support PSE page in future.
	 */
	ret = init_shadow_page(vgpu, &spt->shadow_page, type);
	if (ret) {
		gvt_vgpu_err("fail to initialize shadow page for spt\n");
		goto err;
	}

	ret = intel_vgpu_init_guest_page(vgpu, &spt->guest_page,
			gfn, ppgtt_write_protection_handler, NULL);
	if (ret) {
		gvt_vgpu_err("fail to initialize guest page for spt\n");
		goto err;
	}

	trace_spt_alloc(vgpu->id, spt, type, spt->shadow_page.mfn, gfn);
	return spt;
err:
	ppgtt_free_shadow_page(spt);
	return ERR_PTR(ret);
}

static struct intel_vgpu_ppgtt_spt *ppgtt_find_shadow_page(
		struct intel_vgpu *vgpu, unsigned long mfn)
{
	struct intel_vgpu_shadow_page *p = find_shadow_page(vgpu, mfn);

	if (p)
		return shadow_page_to_ppgtt_spt(p);

	gvt_vgpu_err("fail to find ppgtt shadow page: 0x%lx\n", mfn);
	return NULL;
}

#define pt_entry_size_shift(spt) \
	((spt)->vgpu->gvt->device_info.gtt_entry_size_shift)

#define pt_entries(spt) \
	(GTT_PAGE_SIZE >> pt_entry_size_shift(spt))

#define for_each_present_guest_entry(spt, e, i) \
	for (i = 0; i < pt_entries(spt); i++) \
		if (!ppgtt_get_guest_entry(spt, e, i) && \
		    spt->vgpu->gvt->gtt.pte_ops->test_present(e))

#define for_each_present_shadow_entry(spt, e, i) \
	for (i = 0; i < pt_entries(spt); i++) \
		if (!ppgtt_get_shadow_entry(spt, e, i) && \
		    spt->vgpu->gvt->gtt.pte_ops->test_present(e))

static void ppgtt_get_shadow_page(struct intel_vgpu_ppgtt_spt *spt)
{
	int v = atomic_read(&spt->refcount);

	trace_spt_refcount(spt->vgpu->id, "inc", spt, v, (v + 1));

	atomic_inc(&spt->refcount);
}

static int ppgtt_invalidate_shadow_page(struct intel_vgpu_ppgtt_spt *spt);

static int ppgtt_invalidate_shadow_page_by_shadow_entry(struct intel_vgpu *vgpu,
		struct intel_gvt_gtt_entry *e)
{
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	struct intel_vgpu_ppgtt_spt *s;
	intel_gvt_gtt_type_t cur_pt_type;

	if (WARN_ON(!gtt_type_is_pt(get_next_pt_type(e->type))))
		return -EINVAL;

	if (e->type != GTT_TYPE_PPGTT_ROOT_L3_ENTRY
		&& e->type != GTT_TYPE_PPGTT_ROOT_L4_ENTRY) {
		cur_pt_type = get_next_pt_type(e->type) + 1;
		if (unlikely(cur_pt_type <= GTT_TYPE_INVALID ||
				cur_pt_type >= GTT_TYPE_MAX))
			return -EINVAL;
		if (ops->get_pfn(e) ==
			vgpu->gtt.scratch_pt[cur_pt_type].page_mfn)
			return 0;
	}
	s = ppgtt_find_shadow_page(vgpu, ops->get_pfn(e));
	if (!s) {
		gvt_vgpu_err("fail to find shadow page: mfn: 0x%lx\n",
				ops->get_pfn(e));
		return -ENXIO;
	}
	return ppgtt_invalidate_shadow_page(s);
}

static int ppgtt_invalidate_shadow_page(struct intel_vgpu_ppgtt_spt *spt)
{
	struct intel_vgpu *vgpu = spt->vgpu;
	struct intel_gvt_gtt_entry e;
	unsigned long index;
	int ret;
	int v = atomic_read(&spt->refcount);

	trace_spt_change(spt->vgpu->id, "die", spt,
			spt->guest_page.gfn, spt->shadow_page.type);

	trace_spt_refcount(spt->vgpu->id, "dec", spt, v, (v - 1));

	if (atomic_dec_return(&spt->refcount) > 0)
		return 0;

	if (gtt_type_is_pte_pt(spt->shadow_page.type))
		goto release;

	for_each_present_shadow_entry(spt, &e, index) {
		if (!gtt_type_is_pt(get_next_pt_type(e.type))) {
			gvt_vgpu_err("GVT doesn't support pse bit for now\n");
			return -EINVAL;
		}
		ret = ppgtt_invalidate_shadow_page_by_shadow_entry(
				spt->vgpu, &e);
		if (ret)
			goto fail;
	}
release:
	trace_spt_change(spt->vgpu->id, "release", spt,
			spt->guest_page.gfn, spt->shadow_page.type);
	ppgtt_free_shadow_page(spt);
	return 0;
fail:
	gvt_vgpu_err("fail: shadow page %p shadow entry 0x%llx type %d\n",
			spt, e.val64, e.type);
	return ret;
}

static int ppgtt_populate_shadow_page(struct intel_vgpu_ppgtt_spt *spt);

static struct intel_vgpu_ppgtt_spt *ppgtt_populate_shadow_page_by_guest_entry(
		struct intel_vgpu *vgpu, struct intel_gvt_gtt_entry *we)
{
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	struct intel_vgpu_ppgtt_spt *s = NULL;
	struct intel_vgpu_guest_page *g;
	int ret;

	if (WARN_ON(!gtt_type_is_pt(get_next_pt_type(we->type)))) {
		ret = -EINVAL;
		goto fail;
	}

	g = intel_vgpu_find_guest_page(vgpu, ops->get_pfn(we));
	if (g) {
		s = guest_page_to_ppgtt_spt(g);
		ppgtt_get_shadow_page(s);
	} else {
		int type = get_next_pt_type(we->type);

		s = ppgtt_alloc_shadow_page(vgpu, type, ops->get_pfn(we));
		if (IS_ERR(s)) {
			ret = PTR_ERR(s);
			goto fail;
		}

		ret = intel_gvt_hypervisor_set_wp_page(vgpu, &s->guest_page);
		if (ret)
			goto fail;

		ret = ppgtt_populate_shadow_page(s);
		if (ret)
			goto fail;

		trace_spt_change(vgpu->id, "new", s, s->guest_page.gfn,
			s->shadow_page.type);
	}
	return s;
fail:
	gvt_vgpu_err("fail: shadow page %p guest entry 0x%llx type %d\n",
			s, we->val64, we->type);
	return ERR_PTR(ret);
}

static inline void ppgtt_generate_shadow_entry(struct intel_gvt_gtt_entry *se,
		struct intel_vgpu_ppgtt_spt *s, struct intel_gvt_gtt_entry *ge)
{
	struct intel_gvt_gtt_pte_ops *ops = s->vgpu->gvt->gtt.pte_ops;

	se->type = ge->type;
	se->val64 = ge->val64;

	ops->set_pfn(se, s->shadow_page.mfn);
}

static int ppgtt_populate_shadow_page(struct intel_vgpu_ppgtt_spt *spt)
{
	struct intel_vgpu *vgpu = spt->vgpu;
	struct intel_vgpu_ppgtt_spt *s;
	struct intel_gvt_gtt_entry se, ge;
	unsigned long i;
	int ret;

	trace_spt_change(spt->vgpu->id, "born", spt,
			spt->guest_page.gfn, spt->shadow_page.type);

	if (gtt_type_is_pte_pt(spt->shadow_page.type)) {
		vgpu->ge_cache_enable = true;
		for_each_present_guest_entry(spt, &ge, i) {
			ret = gtt_entry_p2m(vgpu, &ge, &se);
			if (ret)
				goto fail;
			ppgtt_set_shadow_entry(spt, &se, i);
		}
		vgpu->ge_cache_enable = false;
		return 0;
	}

	for_each_present_guest_entry(spt, &ge, i) {
		if (!gtt_type_is_pt(get_next_pt_type(ge.type))) {
			gvt_vgpu_err("GVT doesn't support pse bit now\n");
			ret = -EINVAL;
			goto fail;
		}

		s = ppgtt_populate_shadow_page_by_guest_entry(vgpu, &ge);
		if (IS_ERR(s)) {
			ret = PTR_ERR(s);
			goto fail;
		}
		ppgtt_get_shadow_entry(spt, &se, i);
		ppgtt_generate_shadow_entry(&se, s, &ge);
		ppgtt_set_shadow_entry(spt, &se, i);
	}
	return 0;
fail:
	gvt_vgpu_err("fail: shadow page %p guest entry 0x%llx type %d\n",
			spt, ge.val64, ge.type);
	return ret;
}

static int ppgtt_handle_guest_entry_removal(struct intel_vgpu_guest_page *gpt,
		struct intel_gvt_gtt_entry *se, unsigned long index)
{
	struct intel_vgpu_ppgtt_spt *spt = guest_page_to_ppgtt_spt(gpt);
	struct intel_vgpu_shadow_page *sp = &spt->shadow_page;
	struct intel_vgpu *vgpu = spt->vgpu;
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	int ret;

	trace_gpt_change(spt->vgpu->id, "remove", spt, sp->type, se->val64,
			 index);

	if (!ops->test_present(se))
		return 0;

	if (ops->get_pfn(se) == vgpu->gtt.scratch_pt[sp->type].page_mfn)
		return 0;

	if (gtt_type_is_pt(get_next_pt_type(se->type))) {
		struct intel_vgpu_ppgtt_spt *s =
			ppgtt_find_shadow_page(vgpu, ops->get_pfn(se));
		if (!s) {
			gvt_vgpu_err("fail to find guest page\n");
			ret = -ENXIO;
			goto fail;
		}
		ret = ppgtt_invalidate_shadow_page(s);
		if (ret)
			goto fail;
	}
	return 0;
fail:
	gvt_vgpu_err("fail: shadow page %p guest entry 0x%llx type %d\n",
			spt, se->val64, se->type);
	return ret;
}

static int ppgtt_handle_guest_entry_add(struct intel_vgpu_guest_page *gpt,
		struct intel_gvt_gtt_entry *we, unsigned long index)
{
	struct intel_vgpu_ppgtt_spt *spt = guest_page_to_ppgtt_spt(gpt);
	struct intel_vgpu_shadow_page *sp = &spt->shadow_page;
	struct intel_vgpu *vgpu = spt->vgpu;
	struct intel_gvt_gtt_entry m;
	struct intel_vgpu_ppgtt_spt *s;
	int ret;

	trace_gpt_change(spt->vgpu->id, "add", spt, sp->type,
		we->val64, index);

	if (gtt_type_is_pt(get_next_pt_type(we->type))) {
		s = ppgtt_populate_shadow_page_by_guest_entry(vgpu, we);
		if (IS_ERR(s)) {
			ret = PTR_ERR(s);
			goto fail;
		}
		ppgtt_get_shadow_entry(spt, &m, index);
		ppgtt_generate_shadow_entry(&m, s, we);
		ppgtt_set_shadow_entry(spt, &m, index);
	} else {
		ret = gtt_entry_p2m(vgpu, we, &m);
		if (ret)
			goto fail;
		ppgtt_set_shadow_entry(spt, &m, index);
	}
	return 0;
fail:
	gvt_vgpu_err("fail: spt %p guest entry 0x%llx type %d\n",
		spt, we->val64, we->type);
	return ret;
}

static int sync_oos_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_oos_page *oos_page)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_gvt_gtt_pte_ops *ops = gvt->gtt.pte_ops;
	struct intel_vgpu_ppgtt_spt *spt =
		guest_page_to_ppgtt_spt(oos_page->guest_page);
	struct intel_gvt_gtt_entry old, new, m;
	int index;
	int ret;

	trace_oos_change(vgpu->id, "sync", oos_page->id,
			oos_page->guest_page, spt->guest_page_type);

	old.type = new.type = get_entry_type(spt->guest_page_type);
	old.val64 = new.val64 = 0;

	for (index = 0; index < (GTT_PAGE_SIZE >> info->gtt_entry_size_shift);
		index++) {
		ops->get_entry(oos_page->mem, &old, index, false, 0, vgpu);
		ops->get_entry(NULL, &new, index, true,
			oos_page->guest_page->gfn << PAGE_SHIFT, vgpu);

		if (old.val64 == new.val64
			&& !test_and_clear_bit(index, spt->post_shadow_bitmap))
			continue;

		trace_oos_sync(vgpu->id, oos_page->id,
				oos_page->guest_page, spt->guest_page_type,
				new.val64, index);

		ret = gtt_entry_p2m(vgpu, &new, &m);
		if (ret)
			return ret;

		ops->set_entry(oos_page->mem, &new, index, false, 0, vgpu);
		ppgtt_set_shadow_entry(spt, &m, index);
	}

	oos_page->guest_page->write_cnt = 0;
	list_del_init(&spt->post_shadow_list);
	return 0;
}

static int detach_oos_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_oos_page *oos_page)
{
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_vgpu_ppgtt_spt *spt =
		guest_page_to_ppgtt_spt(oos_page->guest_page);

	trace_oos_change(vgpu->id, "detach", oos_page->id,
			oos_page->guest_page, spt->guest_page_type);

	oos_page->guest_page->write_cnt = 0;
	oos_page->guest_page->oos_page = NULL;
	oos_page->guest_page = NULL;

	list_del_init(&oos_page->vm_list);
	list_move_tail(&oos_page->list, &gvt->gtt.oos_page_free_list_head);

	return 0;
}

static int attach_oos_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_oos_page *oos_page,
		struct intel_vgpu_guest_page *gpt)
{
	struct intel_gvt *gvt = vgpu->gvt;
	int ret;

	ret = intel_gvt_hypervisor_read_gpa(vgpu, gpt->gfn << GTT_PAGE_SHIFT,
		oos_page->mem, GTT_PAGE_SIZE);
	if (ret)
		return ret;

	oos_page->guest_page = gpt;
	gpt->oos_page = oos_page;

	list_move_tail(&oos_page->list, &gvt->gtt.oos_page_use_list_head);

	trace_oos_change(vgpu->id, "attach", gpt->oos_page->id,
			gpt, guest_page_to_ppgtt_spt(gpt)->guest_page_type);
	return 0;
}

static int ppgtt_set_guest_page_sync(struct intel_vgpu *vgpu,
		struct intel_vgpu_guest_page *gpt)
{
	int ret;

	ret = intel_gvt_hypervisor_set_wp_page(vgpu, gpt);
	if (ret)
		return ret;

	trace_oos_change(vgpu->id, "set page sync", gpt->oos_page->id,
			gpt, guest_page_to_ppgtt_spt(gpt)->guest_page_type);

	list_del_init(&gpt->oos_page->vm_list);
	return sync_oos_page(vgpu, gpt->oos_page);
}

static int ppgtt_allocate_oos_page(struct intel_vgpu *vgpu,
		struct intel_vgpu_guest_page *gpt)
{
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_gvt_gtt *gtt = &gvt->gtt;
	struct intel_vgpu_oos_page *oos_page = gpt->oos_page;
	int ret;

	WARN(oos_page, "shadow PPGTT page has already has a oos page\n");

	if (list_empty(&gtt->oos_page_free_list_head)) {
		oos_page = container_of(gtt->oos_page_use_list_head.next,
			struct intel_vgpu_oos_page, list);
		ret = ppgtt_set_guest_page_sync(vgpu, oos_page->guest_page);
		if (ret)
			return ret;
		ret = detach_oos_page(vgpu, oos_page);
		if (ret)
			return ret;
	} else
		oos_page = container_of(gtt->oos_page_free_list_head.next,
			struct intel_vgpu_oos_page, list);
	return attach_oos_page(vgpu, oos_page, gpt);
}

static int ppgtt_set_guest_page_oos(struct intel_vgpu *vgpu,
		struct intel_vgpu_guest_page *gpt)
{
	struct intel_vgpu_oos_page *oos_page = gpt->oos_page;

	if (WARN(!oos_page, "shadow PPGTT page should have a oos page\n"))
		return -EINVAL;

	trace_oos_change(vgpu->id, "set page out of sync", gpt->oos_page->id,
			gpt, guest_page_to_ppgtt_spt(gpt)->guest_page_type);

	list_add_tail(&oos_page->vm_list, &vgpu->gtt.oos_page_list_head);
	return intel_gvt_hypervisor_unset_wp_page(vgpu, gpt);
}

/**
 * intel_vgpu_sync_oos_pages - sync all the out-of-synced shadow for vGPU
 * @vgpu: a vGPU
 *
 * This function is called before submitting a guest workload to host,
 * to sync all the out-of-synced shadow for vGPU
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_sync_oos_pages(struct intel_vgpu *vgpu)
{
	struct list_head *pos, *n;
	struct intel_vgpu_oos_page *oos_page;
	int ret;

	if (!i915_modparams.enable_gvt_oos)
		return 0;

	list_for_each_safe(pos, n, &vgpu->gtt.oos_page_list_head) {
		oos_page = container_of(pos,
				struct intel_vgpu_oos_page, vm_list);
		ret = ppgtt_set_guest_page_sync(vgpu, oos_page->guest_page);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * The heart of PPGTT shadow page table.
 */
static int ppgtt_handle_guest_write_page_table(
		struct intel_vgpu_guest_page *gpt,
		struct intel_gvt_gtt_entry *we, unsigned long index)
{
	struct intel_vgpu_ppgtt_spt *spt = guest_page_to_ppgtt_spt(gpt);
	struct intel_vgpu *vgpu = spt->vgpu;
	int type = spt->shadow_page.type;
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	struct intel_gvt_gtt_entry se;

	int ret;
	int new_present;

	new_present = ops->test_present(we);

	/*
	 * Adding the new entry first and then removing the old one, that can
	 * guarantee the ppgtt table is validated during the window between
	 * adding and removal.
	 */
	ppgtt_get_shadow_entry(spt, &se, index);

	if (new_present) {
		ret = ppgtt_handle_guest_entry_add(gpt, we, index);
		if (ret)
			goto fail;
	}

	ret = ppgtt_handle_guest_entry_removal(gpt, &se, index);
	if (ret)
		goto fail;

	if (!new_present) {
		ops->set_pfn(&se, vgpu->gtt.scratch_pt[type].page_mfn);
		ppgtt_set_shadow_entry(spt, &se, index);
	}

	return 0;
fail:
	gvt_vgpu_err("fail: shadow page %p guest entry 0x%llx type %d.\n",
			spt, we->val64, we->type);
	return ret;
}

static inline bool can_do_out_of_sync(struct intel_vgpu_guest_page *gpt)
{
	return i915_modparams.enable_gvt_oos
		&& gtt_type_is_pte_pt(
			guest_page_to_ppgtt_spt(gpt)->guest_page_type)
		&& gpt->write_cnt >= 2;
}

static void ppgtt_set_post_shadow(struct intel_vgpu_ppgtt_spt *spt,
		unsigned long index)
{
	set_bit(index, spt->post_shadow_bitmap);
	if (!list_empty(&spt->post_shadow_list))
		return;

	list_add_tail(&spt->post_shadow_list,
			&spt->vgpu->gtt.post_shadow_list_head);
}

/**
 * intel_vgpu_flush_post_shadow - flush the post shadow transactions
 * @vgpu: a vGPU
 *
 * This function is called before submitting a guest workload to host,
 * to flush all the post shadows for a vGPU.
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_flush_post_shadow(struct intel_vgpu *vgpu)
{
	struct list_head *pos, *n;
	struct intel_vgpu_ppgtt_spt *spt;
	struct intel_gvt_gtt_entry ge;
	unsigned long index;
	int ret;

	list_for_each_safe(pos, n, &vgpu->gtt.post_shadow_list_head) {
		spt = container_of(pos, struct intel_vgpu_ppgtt_spt,
				post_shadow_list);

		for_each_set_bit(index, spt->post_shadow_bitmap,
				GTT_ENTRY_NUM_IN_ONE_PAGE) {
			ppgtt_get_guest_entry(spt, &ge, index);

			ret = ppgtt_handle_guest_write_page_table(
					&spt->guest_page, &ge, index);
			if (ret)
				return ret;
			clear_bit(index, spt->post_shadow_bitmap);
		}
		list_del_init(&spt->post_shadow_list);
	}
	return 0;
}

static int ppgtt_handle_guest_write_page_table_bytes(void *gp,
		u64 pa, void *p_data, int bytes)
{
	struct intel_vgpu_guest_page *gpt = (struct intel_vgpu_guest_page *)gp;
	struct intel_vgpu_ppgtt_spt *spt = guest_page_to_ppgtt_spt(gpt);
	struct intel_vgpu *vgpu = spt->vgpu;
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	struct intel_gvt_gtt_entry we, se;
	unsigned long index;
	int ret;

	index = (pa & (PAGE_SIZE - 1)) >> info->gtt_entry_size_shift;

	/* Set guest ppgtt entry. Optional for KVMGT, but MUST for XENGT. */
	intel_gvt_hypervisor_write_gpa(vgpu, pa, p_data, bytes);
	ppgtt_get_guest_entry(spt, &we, index);

	ops->test_pse(&we);

	if (bytes == info->gtt_entry_size) {
		ret = ppgtt_handle_guest_write_page_table(gpt, &we, index);
		if (ret)
			return ret;
	} else {
		if (!test_bit(index, spt->post_shadow_bitmap)) {
			int type = spt->shadow_page.type;

			ppgtt_get_shadow_entry(spt, &se, index);
			ret = ppgtt_handle_guest_entry_removal(gpt, &se, index);
			if (ret)
				return ret;
			ops->set_pfn(&se, vgpu->gtt.scratch_pt[type].page_mfn);
			ppgtt_set_shadow_entry(spt, &se, index);
		}
		ppgtt_set_post_shadow(spt, index);
	}

	if (!i915_modparams.enable_gvt_oos)
		return 0;

	gpt->write_cnt++;

	if (gpt->oos_page)
		ops->set_entry(gpt->oos_page->mem, &we, index,
				false, 0, vgpu);

	if (can_do_out_of_sync(gpt)) {
		if (!gpt->oos_page)
			ppgtt_allocate_oos_page(vgpu, gpt);

		ret = ppgtt_set_guest_page_oos(vgpu, gpt);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static void free_ggtt_virtual_page_table(struct intel_vgpu_mm *mm)
{
	struct intel_vgpu_gm *gm = &mm->vgpu->gm;
	struct sg_table *st = gm->st;
	struct scatterlist *sg;

	for (sg = st->sgl; sg; sg = __sg_next(sg)) {
		if (sg_page(sg))
			__free_pages(sg_page(sg), get_order(sg->length));
	}

	sg_free_table(st);
	kfree(st);
	vunmap(mm->virtual_page_table);
}

/*
 * Alloc virtual page table for guest ggtt. If ggtt pv enabled, the
 * physical pages behind virtual page table is also mapped to guest,
 * guest can update its pte entries directly to avoid trap.
 */
static void *alloc_ggtt_virtual_page_table(struct intel_vgpu_mm *mm)
{
	struct intel_vgpu *vgpu = mm->vgpu;
	unsigned int page_count = mm->page_table_entry_size >> PAGE_SHIFT;
	struct intel_vgpu_gm *gm = &vgpu->gm;
	struct page **pages = NULL;
	struct page *p;
	unsigned int i;
	void *vaddr = NULL;
	int order;
	struct sg_table *st;
	struct scatterlist *sg;
	struct sgt_iter sgt_iter;
	unsigned int npages = page_count;

	/*
	 * page_table_entry_size is bigger than the size alloc_pages can
	 * allocate, We have to split it according to the PMD size (2M).
	 * Head page is kept in scatter list so that we can free them later.
	 */
	order = get_order(1 << PMD_SHIFT);

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return ERR_PTR(-ENOMEM);

	if (sg_alloc_table(st, page_count, GFP_KERNEL)) {
		kfree(st);
		return ERR_PTR(-ENOMEM);
	}

	sg = st->sgl;
	st->nents = 0;
	gm->st = st;
	do {
		p = alloc_pages(GFP_KERNEL, order);
		if (!p)
			goto fail;
		gvt_dbg_mm("page=%p size=%ld\n", p, PAGE_SIZE << order);
		sg_set_page(sg, p, PAGE_SIZE << order, 0);
		st->nents++;
		npages -= 1 << order;
		if (!npages) {
			sg_mark_end(sg);
			break;
		}
		sg = __sg_next(sg);
	} while (1);


	/* keep all the pages for vmap */
	pages = kmalloc_array(page_count, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		goto fail;

	i = 0;
	for_each_sgt_page(p, sgt_iter, st)
		pages[i++] = p;

	WARN_ON(i != page_count);

	vaddr = vmap(pages, page_count, VM_MAP, PAGE_KERNEL);
	if (!vaddr) {
		gvt_vgpu_err("fail to vmap pages");
		goto fail;
	}
	kfree(pages);
	return vaddr;

fail:
	sg_set_page(sg, NULL, 0, 0);
	sg_mark_end(sg);
	free_ggtt_virtual_page_table(mm);
	kfree(pages);
	gm->st = NULL;
	return NULL;
}

/*
 * mm page table allocation policy for bdw+
 *  - for ggtt, only virtual page table will be allocated.
 *  - for ppgtt, dedicated virtual/shadow page table will be allocated.
 */
static int gen8_mm_alloc_page_table(struct intel_vgpu_mm *mm)
{
	struct intel_vgpu *vgpu = mm->vgpu;
	struct intel_gvt *gvt = vgpu->gvt;
	const struct intel_gvt_device_info *info = &gvt->device_info;
	void *mem;

	if (mm->type == INTEL_GVT_MM_PPGTT) {
		mm->page_table_entry_cnt = 4;
		mm->page_table_entry_size = mm->page_table_entry_cnt *
			info->gtt_entry_size;
		mem = kzalloc(mm->has_shadow_page_table ?
			mm->page_table_entry_size * 2
				: mm->page_table_entry_size, GFP_KERNEL);
		if (!mem)
			return -ENOMEM;
		mm->virtual_page_table = mem;
		if (!mm->has_shadow_page_table)
			return 0;
		mm->shadow_page_table = mem + mm->page_table_entry_size;
	} else if (mm->type == INTEL_GVT_MM_GGTT) {
		mm->page_table_entry_cnt =
			(gvt_ggtt_gm_sz(gvt) >> GTT_PAGE_SHIFT);
		mm->page_table_entry_size = mm->page_table_entry_cnt *
			info->gtt_entry_size;
		mem = alloc_ggtt_virtual_page_table(mm);
		if (!mem) {
			DRM_INFO("fail to alloc contiguous pages, fallback\n");
			mem = vzalloc(mm->page_table_entry_size);
			if (!mem)
				return -ENOMEM;
		}
		mm->virtual_page_table = mem;
	}
	return 0;
}

static void gen8_mm_free_page_table(struct intel_vgpu_mm *mm)
{
	if (mm->type == INTEL_GVT_MM_PPGTT) {
		kfree(mm->virtual_page_table);
	} else if (mm->type == INTEL_GVT_MM_GGTT) {
		if (mm->virtual_page_table) {
			struct intel_vgpu *vgpu = mm->vgpu;
			struct intel_vgpu_gm *gm = &vgpu->gm;

			if (gm->st) {
				map_gttmmio(mm->vgpu, false);
				free_ggtt_virtual_page_table(mm);
			} else
				vfree(mm->virtual_page_table);
		}
	}
	mm->virtual_page_table = mm->shadow_page_table = NULL;
}

static void invalidate_mm_pv(struct intel_vgpu_mm *mm)
{
	struct intel_vgpu *vgpu = mm->vgpu;
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_gvt_gtt *gtt = &gvt->gtt;
	struct intel_gvt_gtt_pte_ops *ops = gtt->pte_ops;
	struct intel_gvt_gtt_entry se;

	if (WARN_ON(mm->page_table_level != 4))
		return;

	i915_ppgtt_close(&mm->ppgtt->base);
	i915_ppgtt_put(mm->ppgtt);

	ppgtt_get_shadow_root_entry(mm, &se, 0);
	if (!ops->test_present(&se))
		return;
	trace_gpt_change(vgpu->id, "destroy root pointer",
			NULL, se.type, se.val64, 0);
	se.val64 = 0;
	ppgtt_set_shadow_root_entry(mm, &se, 0);

	mm->shadowed = false;
}

static void invalidate_mm(struct intel_vgpu_mm *mm)
{
	struct intel_vgpu *vgpu = mm->vgpu;
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_gvt_gtt *gtt = &gvt->gtt;
	struct intel_gvt_gtt_pte_ops *ops = gtt->pte_ops;
	struct intel_gvt_gtt_entry se;
	int i;

	if (WARN_ON(!mm->has_shadow_page_table || !mm->shadowed))
		return;

	if (VGPU_PVMMIO(mm->vgpu) & PVMMIO_PPGTT_UPDATE) {
		invalidate_mm_pv(mm);
		return;
	}

	for (i = 0; i < mm->page_table_entry_cnt; i++) {
		ppgtt_get_shadow_root_entry(mm, &se, i);
		if (!ops->test_present(&se))
			continue;
		ppgtt_invalidate_shadow_page_by_shadow_entry(
				vgpu, &se);
		se.val64 = 0;
		ppgtt_set_shadow_root_entry(mm, &se, i);

		trace_gpt_change(vgpu->id, "destroy root pointer",
				NULL, se.type, se.val64, i);
	}
	mm->shadowed = false;
}

/**
 * intel_vgpu_destroy_mm - destroy a mm object
 * @mm: a kref object
 *
 * This function is used to destroy a mm object for vGPU
 *
 */
void intel_vgpu_destroy_mm(struct kref *mm_ref)
{
	struct intel_vgpu_mm *mm = container_of(mm_ref, typeof(*mm), ref);
	struct intel_vgpu *vgpu = mm->vgpu;
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_gvt_gtt *gtt = &gvt->gtt;

	if (!mm->initialized)
		goto out;

	list_del(&mm->list);
	list_del(&mm->lru_list);

	if (mm->has_shadow_page_table)
		invalidate_mm(mm);

	gtt->mm_free_page_table(mm);
out:
	kfree(mm);
}

static int shadow_mm_pv(struct intel_vgpu_mm *mm)
{
	struct intel_vgpu *vgpu = mm->vgpu;
	struct intel_gvt *gvt = vgpu->gvt;
	char name[16];
	struct intel_gvt_gtt_entry se;

	if (WARN_ON(mm->page_table_level != 4))
		return -EINVAL;

	snprintf(name, sizeof(name), "%p", mm);

	mm->ppgtt = i915_ppgtt_create(gvt->dev_priv, NULL, name);
	if (IS_ERR(mm->ppgtt)) {
		gvt_vgpu_err("fail to create ppgtt for pdp 0x%llx\n",
				px_dma(&mm->ppgtt->pml4));
		return PTR_ERR(mm->ppgtt);
	}

	se.type = GTT_TYPE_PPGTT_ROOT_L4_ENTRY;
	se.val64 = px_dma(&mm->ppgtt->pml4);
	ppgtt_set_shadow_root_entry(mm, &se, 0);

	trace_gpt_change(vgpu->id, "populate root pointer",
			NULL, se.type, se.val64, 0);
	mm->shadowed = true;

	return 0;
}

static int shadow_mm(struct intel_vgpu_mm *mm)
{
	struct intel_vgpu *vgpu = mm->vgpu;
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_gvt_gtt *gtt = &gvt->gtt;
	struct intel_gvt_gtt_pte_ops *ops = gtt->pte_ops;
	struct intel_vgpu_ppgtt_spt *spt;
	struct intel_gvt_gtt_entry ge, se;
	int i;
	int ret;

	if (WARN_ON(!mm->has_shadow_page_table || mm->shadowed))
		return 0;

	if (VGPU_PVMMIO(mm->vgpu) & PVMMIO_PPGTT_UPDATE)
		return shadow_mm_pv(mm);

	mm->shadowed = true;

	for (i = 0; i < mm->page_table_entry_cnt; i++) {
		ppgtt_get_guest_root_entry(mm, &ge, i);
		if (!ops->test_present(&ge))
			continue;

		trace_gpt_change(vgpu->id, __func__, NULL,
				ge.type, ge.val64, i);

		spt = ppgtt_populate_shadow_page_by_guest_entry(vgpu, &ge);
		if (IS_ERR(spt)) {
			gvt_vgpu_err("fail to populate guest root pointer\n");
			ret = PTR_ERR(spt);
			goto fail;
		}
		ppgtt_generate_shadow_entry(&se, spt, &ge);
		ppgtt_set_shadow_root_entry(mm, &se, i);

		trace_gpt_change(vgpu->id, "populate root pointer",
				NULL, se.type, se.val64, i);
	}
	return 0;
fail:
	invalidate_mm(mm);
	return ret;
}

/**
 * intel_vgpu_create_mm - create a mm object for a vGPU
 * @vgpu: a vGPU
 * @mm_type: mm object type, should be PPGTT or GGTT
 * @virtual_page_table: page table root pointers. Could be NULL if user wants
 *	to populate shadow later.
 * @page_table_level: describe the page table level of the mm object
 * @pde_base_index: pde root pointer base in GGTT MMIO.
 *
 * This function is used to create a mm object for a vGPU.
 *
 * Returns:
 * Zero on success, negative error code in pointer if failed.
 */
struct intel_vgpu_mm *intel_vgpu_create_mm(struct intel_vgpu *vgpu,
		int mm_type, void *virtual_page_table, int page_table_level,
		u32 pde_base_index)
{
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_gvt_gtt *gtt = &gvt->gtt;
	struct intel_vgpu_mm *mm;
	int ret;

	mm = kzalloc(sizeof(*mm), GFP_KERNEL);
	if (!mm) {
		ret = -ENOMEM;
		goto fail;
	}

	mm->type = mm_type;

	if (page_table_level == 1)
		mm->page_table_entry_type = GTT_TYPE_GGTT_PTE;
	else if (page_table_level == 3)
		mm->page_table_entry_type = GTT_TYPE_PPGTT_ROOT_L3_ENTRY;
	else if (page_table_level == 4)
		mm->page_table_entry_type = GTT_TYPE_PPGTT_ROOT_L4_ENTRY;
	else {
		WARN_ON(1);
		ret = -EINVAL;
		goto fail;
	}

	mm->page_table_level = page_table_level;
	mm->pde_base_index = pde_base_index;

	mm->vgpu = vgpu;
	mm->has_shadow_page_table = !!(mm_type == INTEL_GVT_MM_PPGTT);

	kref_init(&mm->ref);
	atomic_set(&mm->pincount, 0);
	INIT_LIST_HEAD(&mm->list);
	INIT_LIST_HEAD(&mm->lru_list);
	list_add_tail(&mm->list, &vgpu->gtt.mm_list_head);

	ret = gtt->mm_alloc_page_table(mm);
	if (ret) {
		gvt_vgpu_err("fail to allocate page table for mm\n");
		goto fail;
	}

	mm->initialized = true;

	if (virtual_page_table)
		memcpy(mm->virtual_page_table, virtual_page_table,
				mm->page_table_entry_size);

	if (mm->has_shadow_page_table) {
		ret = shadow_mm(mm);
		if (ret)
			goto fail;
		list_add_tail(&mm->lru_list, &gvt->gtt.mm_lru_list_head);
	}
	return mm;
fail:
	gvt_vgpu_err("fail to create mm\n");
	if (mm)
		intel_gvt_mm_unreference(mm);
	return ERR_PTR(ret);
}

/**
 * intel_vgpu_unpin_mm - decrease the pin count of a vGPU mm object
 * @mm: a vGPU mm object
 *
 * This function is called when user doesn't want to use a vGPU mm object
 */
void intel_vgpu_unpin_mm(struct intel_vgpu_mm *mm)
{
	if (WARN_ON(mm->type != INTEL_GVT_MM_PPGTT))
		return;

	atomic_dec(&mm->pincount);
}

/**
 * intel_vgpu_pin_mm - increase the pin count of a vGPU mm object
 * @vgpu: a vGPU
 *
 * This function is called when user wants to use a vGPU mm object. If this
 * mm object hasn't been shadowed yet, the shadow will be populated at this
 * time.
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_pin_mm(struct intel_vgpu_mm *mm)
{
	int ret;

	if (WARN_ON(mm->type != INTEL_GVT_MM_PPGTT))
		return 0;

	atomic_inc(&mm->pincount);

	if (!mm->shadowed) {
		ret = shadow_mm(mm);
		if (ret)
			return ret;
	}

	list_del_init(&mm->lru_list);
	list_add_tail(&mm->lru_list, &mm->vgpu->gvt->gtt.mm_lru_list_head);
	return 0;
}

static int reclaim_one_mm(struct intel_gvt *gvt)
{
	struct intel_vgpu_mm *mm;
	struct list_head *pos, *n;

	list_for_each_safe(pos, n, &gvt->gtt.mm_lru_list_head) {
		mm = container_of(pos, struct intel_vgpu_mm, lru_list);

		if (mm->type != INTEL_GVT_MM_PPGTT)
			continue;
		if (atomic_read(&mm->pincount))
			continue;

		list_del_init(&mm->lru_list);
		invalidate_mm(mm);
		return 1;
	}
	return 0;
}

/*
 * GMA translation APIs.
 */
static inline int ppgtt_get_next_level_entry(struct intel_vgpu_mm *mm,
		struct intel_gvt_gtt_entry *e, unsigned long index, bool guest)
{
	struct intel_vgpu *vgpu = mm->vgpu;
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	struct intel_vgpu_ppgtt_spt *s;

	if (WARN_ON(!mm->has_shadow_page_table))
		return -EINVAL;

	s = ppgtt_find_shadow_page(vgpu, ops->get_pfn(e));
	if (!s)
		return -ENXIO;

	if (!guest)
		ppgtt_get_shadow_entry(s, e, index);
	else
		ppgtt_get_guest_entry(s, e, index);
	return 0;
}

/**
 * intel_vgpu_gma_to_gpa - translate a gma to GPA
 * @mm: mm object. could be a PPGTT or GGTT mm object
 * @gma: graphics memory address in this mm object
 *
 * This function is used to translate a graphics memory address in specific
 * graphics memory space to guest physical address.
 *
 * Returns:
 * Guest physical address on success, INTEL_GVT_INVALID_ADDR if failed.
 */
unsigned long intel_vgpu_gma_to_gpa(struct intel_vgpu_mm *mm, unsigned long gma)
{
	struct intel_vgpu *vgpu = mm->vgpu;
	struct intel_gvt *gvt = vgpu->gvt;
	struct intel_gvt_gtt_pte_ops *pte_ops = gvt->gtt.pte_ops;
	struct intel_gvt_gtt_gma_ops *gma_ops = gvt->gtt.gma_ops;
	unsigned long gpa = INTEL_GVT_INVALID_ADDR;
	unsigned long gma_index[4];
	struct intel_gvt_gtt_entry e;
	int i, index;
	int ret;

	if (mm->type != INTEL_GVT_MM_GGTT && mm->type != INTEL_GVT_MM_PPGTT)
		return INTEL_GVT_INVALID_ADDR;

	if (mm->type == INTEL_GVT_MM_GGTT) {
		if (!vgpu_gmadr_is_valid(vgpu, gma))
			goto err;

		ret = ggtt_get_guest_entry(mm, &e,
				gma_ops->gma_to_ggtt_pte_index(gma));
		if (ret)
			goto err;
		gpa = (pte_ops->get_pfn(&e) << GTT_PAGE_SHIFT)
			+ (gma & ~GTT_PAGE_MASK);

		trace_gma_translate(vgpu->id, "ggtt", 0, 0, gma, gpa);
		return gpa;
	}

	switch (mm->page_table_level) {
	case 4:
		ret = ppgtt_get_shadow_root_entry(mm, &e, 0);
		if (ret)
			goto err;
		gma_index[0] = gma_ops->gma_to_pml4_index(gma);
		gma_index[1] = gma_ops->gma_to_l4_pdp_index(gma);
		gma_index[2] = gma_ops->gma_to_pde_index(gma);
		gma_index[3] = gma_ops->gma_to_pte_index(gma);
		index = 4;
		break;
	case 3:
		ret = ppgtt_get_shadow_root_entry(mm, &e,
				gma_ops->gma_to_l3_pdp_index(gma));
		if (ret)
			goto err;
		gma_index[0] = gma_ops->gma_to_pde_index(gma);
		gma_index[1] = gma_ops->gma_to_pte_index(gma);
		index = 2;
		break;
	case 2:
		ret = ppgtt_get_shadow_root_entry(mm, &e,
				gma_ops->gma_to_pde_index(gma));
		if (ret)
			goto err;
		gma_index[0] = gma_ops->gma_to_pte_index(gma);
		index = 1;
		break;
	default:
		WARN_ON(1);
		goto err;
	}

	/* walk into the shadow page table and get gpa from guest entry */
	for (i = 0; i < index; i++) {
		ret = ppgtt_get_next_level_entry(mm, &e, gma_index[i],
			(i == index - 1));
		if (ret)
			goto err;

		if (!pte_ops->test_present(&e)) {
			gvt_dbg_core("GMA 0x%lx is not present\n", gma);
			goto err;
		}
	}

	gpa = (pte_ops->get_pfn(&e) << GTT_PAGE_SHIFT)
		+ (gma & ~GTT_PAGE_MASK);

	trace_gma_translate(vgpu->id, "ppgtt", 0,
			mm->page_table_level, gma, gpa);
	return gpa;
err:
	gvt_vgpu_err("invalid mm type: %d gma %lx\n", mm->type, gma);
	return INTEL_GVT_INVALID_ADDR;
}

static int emulate_gtt_mmio_read(struct intel_vgpu *vgpu,
	unsigned int off, void *p_data, unsigned int bytes)
{
	struct intel_vgpu_mm *ggtt_mm = vgpu->gtt.ggtt_mm;
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	unsigned long index = off >> info->gtt_entry_size_shift;
	struct intel_gvt_gtt_entry e;

	if (bytes != 4 && bytes != 8)
		return -EINVAL;

	ggtt_get_guest_entry(ggtt_mm, &e, index);
	memcpy(p_data, (void *)&e.val64 + (off & (info->gtt_entry_size - 1)),
			bytes);
	return 0;
}

/**
 * intel_vgpu_emulate_gtt_mmio_read - emulate GTT MMIO register read
 * @vgpu: a vGPU
 * @off: register offset
 * @p_data: data will be returned to guest
 * @bytes: data length
 *
 * This function is used to emulate the GTT MMIO register read
 *
 * Returns:
 * Zero on success, error code if failed.
 */
int intel_vgpu_emulate_gtt_mmio_read(struct intel_vgpu *vgpu, unsigned int off,
	void *p_data, unsigned int bytes)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	int ret;

	if (bytes != 4 && bytes != 8)
		return -EINVAL;

	off -= info->gtt_start_offset;
	ret = emulate_gtt_mmio_read(vgpu, off, p_data, bytes);
	return ret;
}

static int emulate_gtt_mmio_write(struct intel_vgpu *vgpu, unsigned int off,
	void *p_data, unsigned int bytes)
{
	struct intel_gvt *gvt = vgpu->gvt;
	const struct intel_gvt_device_info *info = &gvt->device_info;
	struct intel_vgpu_mm *ggtt_mm = vgpu->gtt.ggtt_mm;
	struct intel_gvt_gtt_pte_ops *ops = gvt->gtt.pte_ops;
	unsigned long g_gtt_index = off >> info->gtt_entry_size_shift;
	unsigned long gma;
	struct intel_gvt_gtt_entry e, m;
	int ret;

	if (bytes != 4 && bytes != 8)
		return -EINVAL;

	gma = g_gtt_index << GTT_PAGE_SHIFT;

	/* the VM may configure the whole GM space when ballooning is used */
	if (!vgpu_gmadr_is_valid(vgpu, gma))
		return 0;

	ggtt_get_guest_entry(ggtt_mm, &e, g_gtt_index);

	memcpy((void *)&e.val64 + (off & (info->gtt_entry_size - 1)), p_data,
			bytes);

	if (ops->test_present(&e)) {
		ret = gtt_entry_p2m(vgpu, &e, &m);
		if (ret) {
			gvt_vgpu_err("fail to translate guest gtt entry\n");
			/* guest driver may read/write the entry when partial
			 * update the entry in this situation p2m will fail
			 * settting the shadow entry to point to a scratch page
			 */
			ops->set_pfn(&m, gvt->gtt.scratch_ggtt_mfn);
		}
	} else {
		m = e;
		ops->set_pfn(&m, gvt->gtt.scratch_ggtt_mfn);
	}

	ggtt_set_shadow_entry(ggtt_mm, &m, g_gtt_index);
	gtt_invalidate(gvt->dev_priv);
	ggtt_set_guest_entry(ggtt_mm, &e, g_gtt_index);
	return 0;
}

/*
 * intel_vgpu_emulate_gtt_mmio_write - emulate GTT MMIO register write
 * @vgpu: a vGPU
 * @off: register offset
 * @p_data: data from guest write
 * @bytes: data length
 *
 * This function is used to emulate the GTT MMIO register write
 *
 * Returns:
 * Zero on success, error code if failed.
 */
int intel_vgpu_emulate_gtt_mmio_write(struct intel_vgpu *vgpu, unsigned int off,
	void *p_data, unsigned int bytes)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	int ret;

	if (bytes != 4 && bytes != 8)
		return -EINVAL;

	off -= info->gtt_start_offset;
	ret = emulate_gtt_mmio_write(vgpu, off, p_data, bytes);
	return ret;
}

static int alloc_scratch_pages(struct intel_vgpu *vgpu,
		intel_gvt_gtt_type_t type)
{
	struct intel_vgpu_gtt *gtt = &vgpu->gtt;
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	int page_entry_num = GTT_PAGE_SIZE >>
				vgpu->gvt->device_info.gtt_entry_size_shift;
	void *scratch_pt;
	int i;
	struct device *dev = &vgpu->gvt->dev_priv->drm.pdev->dev;
	dma_addr_t daddr;

	if (WARN_ON(type < GTT_TYPE_PPGTT_PTE_PT || type >= GTT_TYPE_MAX))
		return -EINVAL;

	scratch_pt = (void *)get_zeroed_page(GFP_KERNEL);
	if (!scratch_pt) {
		gvt_vgpu_err("fail to allocate scratch page\n");
		return -ENOMEM;
	}

	daddr = dma_map_page(dev, virt_to_page(scratch_pt), 0,
			4096, PCI_DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, daddr)) {
		gvt_vgpu_err("fail to dmamap scratch_pt\n");
		__free_page(virt_to_page(scratch_pt));
		return -ENOMEM;
	}
	gtt->scratch_pt[type].page_mfn =
		(unsigned long)(daddr >> GTT_PAGE_SHIFT);
	gtt->scratch_pt[type].page = virt_to_page(scratch_pt);
	gvt_dbg_mm("vgpu%d create scratch_pt: type %d mfn=0x%lx\n",
			vgpu->id, type, gtt->scratch_pt[type].page_mfn);

	/* Build the tree by full filled the scratch pt with the entries which
	 * point to the next level scratch pt or scratch page. The
	 * scratch_pt[type] indicate the scratch pt/scratch page used by the
	 * 'type' pt.
	 * e.g. scratch_pt[GTT_TYPE_PPGTT_PDE_PT] is used by
	 * GTT_TYPE_PPGTT_PDE_PT level pt, that means this scratch_pt it self
	 * is GTT_TYPE_PPGTT_PTE_PT, and full filled by scratch page mfn.
	 */
	if (type > GTT_TYPE_PPGTT_PTE_PT) {
		struct intel_gvt_gtt_entry se;

		memset(&se, 0, sizeof(struct intel_gvt_gtt_entry));
		se.type = get_entry_type(type - 1);
		ops->set_pfn(&se, gtt->scratch_pt[type - 1].page_mfn);

		/* The entry parameters like present/writeable/cache type
		 * set to the same as i915's scratch page tree.
		 */
		se.val64 |= _PAGE_PRESENT | _PAGE_RW;
		if (type == GTT_TYPE_PPGTT_PDE_PT)
			se.val64 |= PPAT_CACHED_INDEX;

		for (i = 0; i < page_entry_num; i++)
			ops->set_entry(scratch_pt, &se, i, false, 0, vgpu);
	}

	return 0;
}

static int release_scratch_page_tree(struct intel_vgpu *vgpu)
{
	int i;
	struct device *dev = &vgpu->gvt->dev_priv->drm.pdev->dev;
	dma_addr_t daddr;

	for (i = GTT_TYPE_PPGTT_PTE_PT; i < GTT_TYPE_MAX; i++) {
		if (vgpu->gtt.scratch_pt[i].page != NULL) {
			daddr = (dma_addr_t)(vgpu->gtt.scratch_pt[i].page_mfn <<
					GTT_PAGE_SHIFT);
			dma_unmap_page(dev, daddr, 4096, PCI_DMA_BIDIRECTIONAL);
			__free_page(vgpu->gtt.scratch_pt[i].page);
			vgpu->gtt.scratch_pt[i].page = NULL;
			vgpu->gtt.scratch_pt[i].page_mfn = 0;
		}
	}

	return 0;
}

static int create_scratch_page_tree(struct intel_vgpu *vgpu)
{
	int i, ret;

	for (i = GTT_TYPE_PPGTT_PTE_PT; i < GTT_TYPE_MAX; i++) {
		ret = alloc_scratch_pages(vgpu, i);
		if (ret)
			goto err;
	}

	return 0;

err:
	release_scratch_page_tree(vgpu);
	return ret;
}

/**
 * intel_vgpu_init_gtt - initialize per-vGPU graphics memory virulization
 * @vgpu: a vGPU
 *
 * This function is used to initialize per-vGPU graphics memory virtualization
 * components.
 *
 * Returns:
 * Zero on success, error code if failed.
 */
int intel_vgpu_init_gtt(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_gtt *gtt = &vgpu->gtt;
	struct intel_vgpu_mm *ggtt_mm;

	hash_init(gtt->guest_page_hash_table);
	hash_init(gtt->shadow_page_hash_table);

	INIT_LIST_HEAD(&gtt->mm_list_head);
	INIT_LIST_HEAD(&gtt->oos_page_list_head);
	INIT_LIST_HEAD(&gtt->post_shadow_list_head);

	intel_vgpu_reset_ggtt(vgpu);

	ggtt_mm = intel_vgpu_create_mm(vgpu, INTEL_GVT_MM_GGTT,
			NULL, 1, 0);
	if (IS_ERR(ggtt_mm)) {
		gvt_vgpu_err("fail to create mm for ggtt.\n");
		return PTR_ERR(ggtt_mm);
	}

	gtt->ggtt_mm = ggtt_mm;
	vgpu->cached_guest_entry = kzalloc(GTT_PAGE_SIZE, GFP_KERNEL);
	if (!vgpu->cached_guest_entry) {
		gvt_vgpu_err("fail to allocate cached_guest_entry page\n");
		return -ENOMEM;
	}
	vgpu->ge_cache_enable = false;

	return create_scratch_page_tree(vgpu);
}

static void intel_vgpu_free_mm(struct intel_vgpu *vgpu, int type)
{
	struct list_head *pos, *n;
	struct intel_vgpu_mm *mm;

	list_for_each_safe(pos, n, &vgpu->gtt.mm_list_head) {
		mm = container_of(pos, struct intel_vgpu_mm, list);
		if (mm->type == type) {
			vgpu->gvt->gtt.mm_free_page_table(mm);
			list_del(&mm->list);
			list_del(&mm->lru_list);
			kfree(mm);
		}
	}
}

/**
 * intel_vgpu_clean_gtt - clean up per-vGPU graphics memory virulization
 * @vgpu: a vGPU
 *
 * This function is used to clean up per-vGPU graphics memory virtualization
 * components.
 *
 * Returns:
 * Zero on success, error code if failed.
 */
void intel_vgpu_clean_gtt(struct intel_vgpu *vgpu)
{
	ppgtt_free_all_shadow_page(vgpu);
	release_scratch_page_tree(vgpu);

	intel_vgpu_free_mm(vgpu, INTEL_GVT_MM_PPGTT);
	intel_vgpu_free_mm(vgpu, INTEL_GVT_MM_GGTT);
	intel_vgpu_reset_ggtt(vgpu);
	kfree(vgpu->cached_guest_entry);
}

static void clean_spt_oos(struct intel_gvt *gvt)
{
	struct intel_gvt_gtt *gtt = &gvt->gtt;
	struct list_head *pos, *n;
	struct intel_vgpu_oos_page *oos_page;

	WARN(!list_empty(&gtt->oos_page_use_list_head),
		"someone is still using oos page\n");

	list_for_each_safe(pos, n, &gtt->oos_page_free_list_head) {
		oos_page = container_of(pos, struct intel_vgpu_oos_page, list);
		list_del(&oos_page->list);
		kfree(oos_page);
	}
}

static int setup_spt_oos(struct intel_gvt *gvt)
{
	struct intel_gvt_gtt *gtt = &gvt->gtt;
	struct intel_vgpu_oos_page *oos_page;
	int i;
	int ret;

	INIT_LIST_HEAD(&gtt->oos_page_free_list_head);
	INIT_LIST_HEAD(&gtt->oos_page_use_list_head);

	for (i = 0; i < preallocated_oos_pages; i++) {
		oos_page = kzalloc(sizeof(*oos_page), GFP_KERNEL);
		if (!oos_page) {
			ret = -ENOMEM;
			goto fail;
		}

		INIT_LIST_HEAD(&oos_page->list);
		INIT_LIST_HEAD(&oos_page->vm_list);
		oos_page->id = i;
		list_add_tail(&oos_page->list, &gtt->oos_page_free_list_head);
	}

	gvt_dbg_mm("%d oos pages preallocated\n", i);

	return 0;
fail:
	clean_spt_oos(gvt);
	return ret;
}

/**
 * intel_vgpu_find_ppgtt_mm - find a PPGTT mm object
 * @vgpu: a vGPU
 * @page_table_level: PPGTT page table level
 * @root_entry: PPGTT page table root pointers
 *
 * This function is used to find a PPGTT mm object from mm object pool
 *
 * Returns:
 * pointer to mm object on success, NULL if failed.
 */
struct intel_vgpu_mm *intel_vgpu_find_ppgtt_mm(struct intel_vgpu *vgpu,
		int page_table_level, void *root_entry)
{
	struct list_head *pos;
	struct intel_vgpu_mm *mm;
	u64 *src, *dst;

	list_for_each(pos, &vgpu->gtt.mm_list_head) {
		mm = container_of(pos, struct intel_vgpu_mm, list);
		if (mm->type != INTEL_GVT_MM_PPGTT)
			continue;

		if (mm->page_table_level != page_table_level)
			continue;

		src = root_entry;
		dst = mm->virtual_page_table;

		if (page_table_level == 3) {
			if (src[0] == dst[0]
					&& src[1] == dst[1]
					&& src[2] == dst[2]
					&& src[3] == dst[3])
				return mm;
		} else {
			if (src[0] == dst[0])
				return mm;
		}
	}
	return NULL;
}

/**
 * intel_vgpu_g2v_create_ppgtt_mm - create a PPGTT mm object from
 * g2v notification
 * @vgpu: a vGPU
 * @page_table_level: PPGTT page table level
 *
 * This function is used to create a PPGTT mm object from a guest to GVT-g
 * notification.
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_g2v_create_ppgtt_mm(struct intel_vgpu *vgpu,
		int page_table_level)
{
	u64 *pdp = (u64 *)&vgpu_vreg64(vgpu, vgtif_reg(pdp[0]));
	struct intel_vgpu_mm *mm;

	if (WARN_ON((page_table_level != 4) && (page_table_level != 3)))
		return -EINVAL;

	mm = intel_vgpu_find_ppgtt_mm(vgpu, page_table_level, pdp);
	if (mm) {
		intel_gvt_mm_reference(mm);
	} else {
		mm = intel_vgpu_create_mm(vgpu, INTEL_GVT_MM_PPGTT,
				pdp, page_table_level, 0);
		if (IS_ERR(mm)) {
			gvt_vgpu_err("fail to create mm\n");
			return PTR_ERR(mm);
		}
	}
	return 0;
}

/**
 * intel_vgpu_g2v_destroy_ppgtt_mm - destroy a PPGTT mm object from
 * g2v notification
 * @vgpu: a vGPU
 * @page_table_level: PPGTT page table level
 *
 * This function is used to create a PPGTT mm object from a guest to GVT-g
 * notification.
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_g2v_destroy_ppgtt_mm(struct intel_vgpu *vgpu,
		int page_table_level)
{
	u64 *pdp = (u64 *)&vgpu_vreg64(vgpu, vgtif_reg(pdp[0]));
	struct intel_vgpu_mm *mm;

	if (WARN_ON((page_table_level != 4) && (page_table_level != 3)))
		return -EINVAL;

	mm = intel_vgpu_find_ppgtt_mm(vgpu, page_table_level, pdp);
	if (!mm) {
		gvt_vgpu_err("fail to find ppgtt instance.\n");
		return -EINVAL;
	}
	intel_gvt_mm_unreference(mm);
	return 0;
}

/**
 * intel_gvt_init_gtt - initialize mm components of a GVT device
 * @gvt: GVT device
 *
 * This function is called at the initialization stage, to initialize
 * the mm components of a GVT device.
 *
 * Returns:
 * zero on success, negative error code if failed.
 */
int intel_gvt_init_gtt(struct intel_gvt *gvt)
{
	int ret;
	void *page;
	struct device *dev = &gvt->dev_priv->drm.pdev->dev;
	dma_addr_t daddr;

	gvt_dbg_core("init gtt\n");

	if (IS_BROADWELL(gvt->dev_priv) || IS_SKYLAKE(gvt->dev_priv)
		|| IS_KABYLAKE(gvt->dev_priv) || IS_BROXTON(gvt->dev_priv)) {
		gvt->gtt.pte_ops = &gen8_gtt_pte_ops;
		gvt->gtt.gma_ops = &gen8_gtt_gma_ops;
		gvt->gtt.mm_alloc_page_table = gen8_mm_alloc_page_table;
		gvt->gtt.mm_free_page_table = gen8_mm_free_page_table;
	} else {
		return -ENODEV;
	}

	page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!page) {
		gvt_err("fail to allocate scratch ggtt page\n");
		return -ENOMEM;
	}

	daddr = dma_map_page(dev, virt_to_page(page), 0,
			4096, PCI_DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, daddr)) {
		gvt_err("fail to dmamap scratch ggtt page\n");
		__free_page(virt_to_page(page));
		return -ENOMEM;
	}
	gvt->gtt.scratch_ggtt_page = virt_to_page(page);
	gvt->gtt.scratch_ggtt_mfn = (unsigned long)(daddr >> GTT_PAGE_SHIFT);

	if (i915_modparams.enable_gvt_oos) {
		ret = setup_spt_oos(gvt);
		if (ret) {
			gvt_err("fail to initialize SPT oos\n");
			dma_unmap_page(dev, daddr, 4096, PCI_DMA_BIDIRECTIONAL);
			__free_page(gvt->gtt.scratch_ggtt_page);
			return ret;
		}
	}
	INIT_LIST_HEAD(&gvt->gtt.mm_lru_list_head);
	return 0;
}

/**
 * intel_gvt_clean_gtt - clean up mm components of a GVT device
 * @gvt: GVT device
 *
 * This function is called at the driver unloading stage, to clean up the
 * the mm components of a GVT device.
 *
 */
void intel_gvt_clean_gtt(struct intel_gvt *gvt)
{
	struct device *dev = &gvt->dev_priv->drm.pdev->dev;
	dma_addr_t daddr = (dma_addr_t)(gvt->gtt.scratch_ggtt_mfn <<
					GTT_PAGE_SHIFT);

	dma_unmap_page(dev, daddr, 4096, PCI_DMA_BIDIRECTIONAL);

	__free_page(gvt->gtt.scratch_ggtt_page);

	if (i915_modparams.enable_gvt_oos)
		clean_spt_oos(gvt);
}

/**
 * intel_vgpu_reset_ggtt - reset the GGTT entry
 * @vgpu: a vGPU
 *
 * This function is called at the vGPU create stage
 * to reset all the GGTT entries.
 *
 */
void intel_vgpu_reset_ggtt(struct intel_vgpu *vgpu)
{
	struct intel_gvt *gvt = vgpu->gvt;
	struct drm_i915_private *dev_priv = gvt->dev_priv;
	struct intel_gvt_gtt_pte_ops *ops = vgpu->gvt->gtt.pte_ops;
	u32 index;
	u32 offset;
	u32 num_entries;
	struct intel_gvt_gtt_entry e;

	memset(&e, 0, sizeof(struct intel_gvt_gtt_entry));
	e.type = GTT_TYPE_GGTT_PTE;
	ops->set_pfn(&e, gvt->gtt.scratch_ggtt_mfn);
	e.val64 |= _PAGE_PRESENT;

	index = vgpu_aperture_gmadr_base(vgpu) >> PAGE_SHIFT;
	num_entries = vgpu_aperture_sz(vgpu) >> PAGE_SHIFT;
	for (offset = 0; offset < num_entries; offset++)
		ops->set_entry(NULL, &e, index + offset, false, 0, vgpu);

	index = vgpu_hidden_gmadr_base(vgpu) >> PAGE_SHIFT;
	num_entries = vgpu_hidden_sz(vgpu) >> PAGE_SHIFT;
	for (offset = 0; offset < num_entries; offset++)
		ops->set_entry(NULL, &e, index + offset, false, 0, vgpu);

	gtt_invalidate(dev_priv);
}

/**
 * intel_vgpu_reset_gtt - reset the all GTT related status
 * @vgpu: a vGPU
 *
 * This function is called from vfio core to reset reset all
 * GTT related status, including GGTT, PPGTT, scratch page.
 *
 */
void intel_vgpu_reset_gtt(struct intel_vgpu *vgpu)
{
	int i;

	ppgtt_free_all_shadow_page(vgpu);

	/* Shadow pages are only created when there is no page
	 * table tracking data, so remove page tracking data after
	 * removing the shadow pages.
	 */
	intel_vgpu_free_mm(vgpu, INTEL_GVT_MM_PPGTT);

	intel_vgpu_reset_ggtt(vgpu);

	/* clear scratch page for security */
	for (i = GTT_TYPE_PPGTT_PTE_PT; i < GTT_TYPE_MAX; i++) {
		if (vgpu->gtt.scratch_pt[i].page != NULL)
			memset(page_address(vgpu->gtt.scratch_pt[i].page),
				0, PAGE_SIZE);
	}
}

int intel_vgpu_g2v_pv_ppgtt_alloc_4lvl(struct intel_vgpu *vgpu,
		int page_table_level)
{
	struct pv_ppgtt_update *pv_ppgtt = &vgpu->mmio.shared_page->pv_ppgtt;
	struct intel_vgpu_mm *mm;
	u64 pdp = pv_ppgtt->pdp;
	int ret = 0;

	if (WARN_ON(page_table_level != 4))
		return -EINVAL;

	gvt_dbg_mm("alloc_4lvl pdp=%llx start=%llx length=%llx\n",
			pv_ppgtt->pdp, pv_ppgtt->start,
			pv_ppgtt->length);

	mm = intel_vgpu_find_ppgtt_mm(vgpu, page_table_level, &pdp);
	if (!mm) {
		gvt_vgpu_err("failed to find mm for pdp 0x%llx\n", pdp);
		ret = -EINVAL;
	} else {
		ret = mm->ppgtt->base.allocate_va_range(&mm->ppgtt->base,
			pv_ppgtt->start, pv_ppgtt->length);
		if (ret)
			gvt_vgpu_err("failed to alloc for pdp %llx\n", pdp);
	}

	return ret;
}

int intel_vgpu_g2v_pv_ppgtt_clear_4lvl(struct intel_vgpu *vgpu,
		int page_table_level)
{
	struct pv_ppgtt_update *pv_ppgtt = &vgpu->mmio.shared_page->pv_ppgtt;
	struct intel_vgpu_mm *mm;
	u64 pdp = pv_ppgtt->pdp;
	int ret = 0;

	if (WARN_ON(page_table_level != 4))
		return -EINVAL;

	gvt_dbg_mm("clear_4lvl pdp=%llx start=%llx length=%llx\n",
			pv_ppgtt->pdp, pv_ppgtt->start,
			pv_ppgtt->length);

	mm = intel_vgpu_find_ppgtt_mm(vgpu, page_table_level, &pdp);
	if (!mm) {
		gvt_vgpu_err("failed to find mm for pdp 0x%llx\n", pdp);
		ret = -EINVAL;
	} else {
		mm->ppgtt->base.clear_range(&mm->ppgtt->base,
			pv_ppgtt->start, pv_ppgtt->length);
	}

	return ret;
}

#define GEN8_PML4E_SIZE		(1UL << GEN8_PML4E_SHIFT)
#define GEN8_PML4E_SIZE_MASK	(~(GEN8_PML4E_SIZE - 1))
#define GEN8_PDPE_SIZE		(1UL << GEN8_PDPE_SHIFT)
#define GEN8_PDPE_SIZE_MASK	(~(GEN8_PDPE_SIZE - 1))
#define GEN8_PDE_SIZE		(1UL << GEN8_PDE_SHIFT)
#define GEN8_PDE_SIZE_MASK	(~(GEN8_PDE_SIZE - 1))

#define pml4_addr_end(addr, end)					\
({	unsigned long __boundary = \
			((addr) + GEN8_PML4E_SIZE) & GEN8_PML4E_SIZE_MASK; \
	(__boundary < (end)) ? __boundary : (end);		\
})

#define pdp_addr_end(addr, end)						\
({	unsigned long __boundary = \
			((addr) + GEN8_PDPE_SIZE) & GEN8_PDPE_SIZE_MASK; \
	(__boundary < (end)) ? __boundary : (end);		\
})

#define pd_addr_end(addr, end)						\
({	unsigned long __boundary = \
			((addr) + GEN8_PDE_SIZE) & GEN8_PDE_SIZE_MASK;	\
	(__boundary < (end)) ? __boundary : (end);		\
})

struct ppgtt_walk {
	unsigned long *mfns;
	int mfn_index;
	unsigned long *pt;
};

static int walk_pt_range(struct intel_vgpu *vgpu, u64 pt,
				u64 start, u64 end, struct ppgtt_walk *walk)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	struct intel_gvt_gtt_gma_ops *gma_ops = vgpu->gvt->gtt.gma_ops;
	unsigned long start_index, end_index;
	int ret;
	int i;
	unsigned long mfn, gfn;

	start_index = gma_ops->gma_to_pte_index(start);
	end_index = ((end - start) >> PAGE_SHIFT) + start_index;

	gvt_dbg_mm("%s: %llx start=%llx end=%llx start_index=%lx end_index=%lx mfn_index=%x\n",
			__func__, pt, start, end,
			start_index, end_index, walk->mfn_index);
	ret = intel_gvt_hypervisor_read_gpa(vgpu,
		(pt & PAGE_MASK) + (start_index << info->gtt_entry_size_shift),
		walk->pt + start_index,
		(end_index - start_index) << info->gtt_entry_size_shift);
	if (ret) {
		gvt_vgpu_err("fail to read gpa %llx\n", pt);
		return ret;
	}

	for (i = start_index; i < end_index; i++) {
		gfn = walk->pt[i] >> PAGE_SHIFT;
		mfn = intel_gvt_hypervisor_gfn_to_mfn(vgpu, gfn);
		if (mfn == INTEL_GVT_INVALID_ADDR) {
			gvt_vgpu_err("fail to translate gfn: 0x%lx\n", gfn);
			return -ENXIO;
		}
		walk->mfns[walk->mfn_index++] = mfn << PAGE_SHIFT;
	}

	return 0;
}


static int walk_pd_range(struct intel_vgpu *vgpu, u64 pd,
				u64 start, u64 end, struct ppgtt_walk *walk)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	struct intel_gvt_gtt_gma_ops *gma_ops = vgpu->gvt->gtt.gma_ops;
	unsigned long index;
	u64 pt, next;
	int ret  = 0;

	do {
		index = gma_ops->gma_to_pde_index(start);

		ret = intel_gvt_hypervisor_read_gpa(vgpu,
			(pd & PAGE_MASK) + (index <<
			info->gtt_entry_size_shift), &pt, 8);
		if (ret)
			return ret;
		next = pd_addr_end(start, end);
		gvt_dbg_mm("%s: %llx start=%llx end=%llx next=%llx\n",
			__func__, pd, start, end, next);
		walk_pt_range(vgpu, pt, start, next, walk);

		start = next;
	} while (start != end);

	return ret;
}


static int walk_pdp_range(struct intel_vgpu *vgpu, u64 pdp,
				  u64 start, u64 end, struct ppgtt_walk *walk)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	struct intel_gvt_gtt_gma_ops *gma_ops = vgpu->gvt->gtt.gma_ops;
	unsigned long index;
	u64 pd, next;
	int ret  = 0;

	do {
		index = gma_ops->gma_to_l4_pdp_index(start);

		ret = intel_gvt_hypervisor_read_gpa(vgpu,
			(pdp & PAGE_MASK) + (index <<
			info->gtt_entry_size_shift), &pd, 8);
		if (ret)
			return ret;
		next = pdp_addr_end(start, end);
		gvt_dbg_mm("%s: %llx start=%llx end=%llx next=%llx\n",
			__func__, pdp, start, end, next);

		walk_pd_range(vgpu, pd, start, next, walk);
		start = next;
	} while (start != end);

	return ret;
}


static int walk_pml4_range(struct intel_vgpu *vgpu, u64 pml4,
				u64 start, u64 end, struct ppgtt_walk *walk)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;
	struct intel_gvt_gtt_gma_ops *gma_ops = vgpu->gvt->gtt.gma_ops;
	unsigned long index;
	u64 pdp, next;
	int ret  = 0;

	do {
		index = gma_ops->gma_to_pml4_index(start);
		ret = intel_gvt_hypervisor_read_gpa(vgpu,
			(pml4 & PAGE_MASK) + (index <<
			info->gtt_entry_size_shift), &pdp, 8);
		if (ret)
			return ret;
		next = pml4_addr_end(start, end);
		gvt_dbg_mm("%s: %llx start=%llx end=%llx next=%llx\n",
			__func__, pml4, start, end, next);

		walk_pdp_range(vgpu, pdp, start, next, walk);
		start = next;
	} while (start != end);

	return ret;
}

int intel_vgpu_g2v_pv_ppgtt_insert_4lvl(struct intel_vgpu *vgpu,
		int page_table_level)
{
	struct pv_ppgtt_update *pv_ppgtt = &vgpu->mmio.shared_page->pv_ppgtt;
	struct intel_vgpu_mm *mm;
	u64 pml4 = pv_ppgtt->pdp;
	int ret = 0;
	u64 start = pv_ppgtt->start;
	u64 length = pv_ppgtt->length;
	struct sg_table st;
	struct scatterlist *sg = NULL;
	int num_pages = length >> PAGE_SHIFT;
	struct i915_vma vma;
	struct ppgtt_walk walk;
	int i;

	if (WARN_ON(page_table_level != 4))
		return -EINVAL;

	gvt_dbg_mm("insert_4lvl pml4=%llx start=%llx length=%llx cache=%x\n",
			pv_ppgtt->pdp, start, length, pv_ppgtt->cache_level);

	mm = intel_vgpu_find_ppgtt_mm(vgpu, page_table_level, &pml4);
	if (!mm) {
		gvt_vgpu_err("fail to find mm for pml4 0x%llx\n", pml4);
		return -EINVAL;
	}

	walk.mfn_index = 0;
	walk.mfns = NULL;
	walk.pt = NULL;

	walk.mfns = kmalloc_array(num_pages,
			sizeof(unsigned long), GFP_KERNEL);
	if (!walk.mfns) {
		ret = -ENOMEM;
		goto fail;
	}

	walk.pt = (unsigned long *)__get_free_pages(GFP_KERNEL, 0);
	if (!walk.pt) {
		ret = -ENOMEM;
		goto fail;
	}

	if (sg_alloc_table(&st, num_pages, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = walk_pml4_range(vgpu, pml4, start, start + length, &walk);
	if (ret)
		goto fail_free_sg;

	WARN_ON(num_pages != walk.mfn_index);

	for_each_sg(st.sgl, sg, num_pages, i) {
		sg->offset = 0;
		sg->length = PAGE_SIZE;
		sg_dma_address(sg) = walk.mfns[i];
		sg_dma_len(sg) = PAGE_SIZE;
	}

	/* fake vma for insert call*/
	memset(&vma, 0, sizeof(vma));
	vma.node.start = start;
	vma.pages = &st;
	mm->ppgtt->base.insert_entries(&mm->ppgtt->base, &vma,
					pv_ppgtt->cache_level, 0);

fail_free_sg:
	sg_free_table(&st);
fail:
	kfree(walk.mfns);
	free_page((unsigned long)walk.pt);

	return ret;
}

static void validate_ggtt_range(struct intel_vgpu *vgpu,
	u64 *start, u64 *length)
{
	u64 end;

	if (WARN_ON(*start > vgpu->gvt->dev_priv->ggtt.base.total ||
	     *length > vgpu->gvt->dev_priv->ggtt.base.total)) {
		*length = 0;
		return;
	}

	end = *start + *length - 1;

	if (*start >= vgpu_aperture_gmadr_base(vgpu) &&
	     end <= vgpu_aperture_gmadr_end(vgpu))
		return;

	if (*start >= vgpu_hidden_gmadr_base(vgpu) &&
	     end <= vgpu_hidden_gmadr_end(vgpu))
		return;

	/* handle the cases with invalid ranges */
	WARN_ON(1);

	/* start is in aperture range, end is after apeture range */
	if (*start >= vgpu_aperture_gmadr_base(vgpu) &&
	    *start <= vgpu_aperture_gmadr_end(vgpu)) {
		*length = vgpu_aperture_gmadr_end(vgpu) - *start + 1;
		return;
	}

	/* start is before aperture range, end is in apeture range */
	if (end >= vgpu_aperture_gmadr_base(vgpu) &&
	    end <= vgpu_aperture_gmadr_end(vgpu)) {
		*start = vgpu_aperture_gmadr_base(vgpu);
		return;
	}

	/* start is in hidden range, end is after hidden range */
	if (*start >= vgpu_hidden_gmadr_base(vgpu) &&
	    *start <= vgpu_hidden_gmadr_end(vgpu)) {
		*length = vgpu_hidden_gmadr_end(vgpu) - *start + 1;
		return;
	}

	/* start is before hidden range, end is in hidden range */
	if (end >= vgpu_hidden_gmadr_base(vgpu) &&
	    end <= vgpu_hidden_gmadr_end(vgpu)) {
		*start = vgpu_hidden_gmadr_base(vgpu);
		return;
	}

	/* both start and end are not in valid range*/
	*length = 0;

	return;
}

int intel_vgpu_g2v_pv_ggtt_insert(struct intel_vgpu *vgpu)
{
	struct intel_vgpu_gtt *gtt = &vgpu->gtt;
	struct gvt_shared_page *shared_page = vgpu->mmio.shared_page;
	struct drm_i915_private *dev_priv = vgpu->gvt->dev_priv;
	struct i915_ggtt *ggtt = &dev_priv->ggtt;
	u64 start = shared_page->pv_ggtt.start;
	u64 num_entries = shared_page->pv_ggtt.length;
	u32 cache_level = shared_page->pv_ggtt.cache_level;
	u64 length = num_entries << PAGE_SHIFT;
	u64 *vaddr = gtt->ggtt_mm->virtual_page_table;
	u64 gtt_entry_index;
	u64 gtt_entry;
	unsigned long mfn;
	struct i915_vma vma;
	struct sg_table st;
	struct scatterlist *sg = NULL;
	int ret = 0;
	int i;

	gvt_dbg_mm("ggtt_insert: start=%llx length=%llx cache=%x\n",
		start, length, cache_level);
	validate_ggtt_range(vgpu, &start, &length);
	if (length == 0)
		return 0;

	num_entries = length >> PAGE_SHIFT;

	if (sg_alloc_table(&st, num_entries, GFP_KERNEL))
		return -ENOMEM;

	for_each_sg(st.sgl, sg, num_entries, i) {
		gtt_entry_index = (start >> PAGE_SHIFT) + i;
		gtt_entry = vaddr[gtt_entry_index];
		mfn = intel_gvt_hypervisor_gfn_to_mfn(vgpu,
					gtt_entry >> PAGE_SHIFT);
		if (mfn == INTEL_GVT_INVALID_ADDR) {
			gvt_vgpu_err("fail to translate gfn: 0x%llx\n",
					gtt_entry >> PAGE_SHIFT);
			ret = -ENXIO;
			goto fail;
		}
		sg->offset = 0;
		sg->length = PAGE_SIZE;
		sg_dma_address(sg) = mfn << PAGE_SHIFT;
		sg_dma_len(sg) = PAGE_SIZE;
	}

	/* fake vma for insert call*/
	memset(&vma, 0, sizeof(vma));
	vma.node.start = start;
	vma.pages = &st;
	ggtt->base.insert_entries(&ggtt->base, &vma, cache_level, 0);

fail:
	sg_free_table(&st);
	return ret;
}

int intel_vgpu_g2v_pv_ggtt_clear(struct intel_vgpu *vgpu)
{
	struct gvt_shared_page *shared_page = vgpu->mmio.shared_page;
	u64 start = shared_page->pv_ggtt.start;
	u64 length = shared_page->pv_ggtt.length;
	struct i915_ggtt *ggtt = &vgpu->gvt->dev_priv->ggtt;

	gvt_dbg_mm("ggtt_clear: start=%llx length=%llx\n",
		start, length);
	validate_ggtt_range(vgpu, &start, &length);
	if (length == 0)
		return 0;

	ggtt->base.clear_range(&ggtt->base, start, length);

	return 0;
}
