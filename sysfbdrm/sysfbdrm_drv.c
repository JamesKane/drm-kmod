/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 James Kane
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * KMS driver for the framebuffer the boot firmware left running, such as the
 * UEFI GOP framebuffer.  The display mode is the one the firmware set and
 * cannot be changed.  Clients draw into dumb buffers in system memory, and
 * the damaged parts are copied to the framebuffer on each commit.  There is
 * no hardware vblank; a timer running at the refresh rate stands in for it.
 *
 * Dumb buffers are write-combining: a GPU may render into them through
 * PRIME, and on systems where it does not snoop the CPU's caches, the copy
 * would otherwise read stale cache lines.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/fbio.h>
#include <sys/kernel.h>
#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/rwlock.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <machine/metadata.h>

#include <dev/vt/vt.h>

#include <linux/device.h>
#include <linux/fb.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_prime.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#define	SYSFBDRM_CPP	4

extern struct vt_device *main_vd;

/* KMS state, allocated with drmm_kzalloc() to live as long as the device. */
struct sysfbdrm_kms {
	struct drm_device	*drm;
	uint8_t			*fb_vaddr;
	u_int			fb_pitch;
	struct drm_display_mode	mode;
	struct drm_plane	plane;
	struct drm_crtc		crtc;
	struct drm_encoder	encoder;
	struct drm_connector	connector;
	struct callout		vblank;
	sbintime_t		vblank_period;
	sbintime_t		vblank_next;	/* deadline of the next tick */
	u_int			vblank_enabled;
};

struct sysfbdrm_softc {
	device_t		dev;
	struct device		*ldev;
	struct drm_device	*drm;
	struct sysfbdrm_kms	*kms;
	vm_paddr_t		fb_paddr;
	vm_size_t		fb_size;
	uint8_t			*fb_vaddr;
	bool			vt_frozen;	/* by master_set */
};

/* A dumb buffer: pages of system memory with a kernel mapping. */
struct sysfbdrm_bo {
	struct drm_gem_object	base;
	struct page		**pages;
	u_int			npages;
	void			*vaddr;
};

#define	to_sysfbdrm_bo(obj)	container_of(obj, struct sysfbdrm_bo, base)

static const uint32_t sysfbdrm_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const uint64_t sysfbdrm_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static struct efi_fb *
sysfbdrm_efifb(void)
{
	return ((struct efi_fb *)preload_search_info(preload_kmdp,
	    MODINFO_METADATA | MODINFOMD_EFI_FB));
}

/* GEM objects */

static void
sysfbdrm_bo_free(struct drm_gem_object *obj)
{
	struct sysfbdrm_bo *bo = to_sysfbdrm_bo(obj);
	u_int i;

	if (bo->vaddr != NULL)
		vunmap(bo->vaddr);
	if (bo->pages != NULL) {
		for (i = 0; i < bo->npages && bo->pages[i] != NULL; i++)
			__free_page(bo->pages[i]);
		kvfree(bo->pages);
	}
	drm_gem_object_release(obj);
	kfree(bo);
}

static struct sg_table *
sysfbdrm_bo_get_sg_table(struct drm_gem_object *obj)
{
	struct sysfbdrm_bo *bo = to_sysfbdrm_bo(obj);

	return (drm_prime_pages_to_sg(obj->dev, bo->pages, bo->npages));
}

static int
sysfbdrm_bo_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	iosys_map_set_vaddr(map, to_sysfbdrm_bo(obj)->vaddr);
	return (0);
}

static vm_fault_t
sysfbdrm_bo_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct sysfbdrm_bo *bo = to_sysfbdrm_bo(vma->vm_private_data);
	unsigned long addr;
	vm_fault_t ret;
	pgoff_t idx;

	idx = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	if (idx >= bo->npages)
		return (VM_FAULT_SIGBUS);

	/* Map the rest of the buffer as well; only the first page must succeed. */
	addr = vmf->address;
	VM_OBJECT_WLOCK(vma->vm_obj);
	ret = lkpi_vmf_insert_pfn_prot_locked(vma, addr,
	    page_to_pfn(bo->pages[idx]), vma->vm_page_prot);
	while ((ret & VM_FAULT_ERROR) == 0 && ++idx < bo->npages) {
		addr += PAGE_SIZE;
		if ((lkpi_vmf_insert_pfn_prot_locked(vma, addr,
		    page_to_pfn(bo->pages[idx]), vma->vm_page_prot) &
		    VM_FAULT_ERROR) != 0)
			break;
	}
	VM_OBJECT_WUNLOCK(vma->vm_obj);
	return (ret);
}

static const struct vm_operations_struct sysfbdrm_vm_ops = {
	.fault = sysfbdrm_bo_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int
sysfbdrm_bo_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	/* The caching mode must match the kernel's mapping; see the top. */
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	return (0);
}

static const struct drm_gem_object_funcs sysfbdrm_bo_funcs = {
	.free = sysfbdrm_bo_free,
	.get_sg_table = sysfbdrm_bo_get_sg_table,
	.vmap = sysfbdrm_bo_vmap,
	.mmap = sysfbdrm_bo_mmap,
	.vm_ops = &sysfbdrm_vm_ops,
};

static struct sysfbdrm_bo *
sysfbdrm_bo_create(struct drm_device *drm, size_t size)
{
	struct sysfbdrm_bo *bo;
	u_int i;

	size = round_up(size, PAGE_SIZE);
	if (size == 0)
		return (ERR_PTR(-EINVAL));
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (bo == NULL)
		return (ERR_PTR(-ENOMEM));
	bo->base.funcs = &sysfbdrm_bo_funcs;
	drm_gem_private_object_init(drm, &bo->base, size);

	bo->npages = size >> PAGE_SHIFT;
	bo->pages = kvcalloc(bo->npages, sizeof(*bo->pages), GFP_KERNEL);
	if (bo->pages == NULL)
		goto fail;
	for (i = 0; i < bo->npages; i++) {
		bo->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (bo->pages[i] == NULL)
			goto fail;
		/*
		 * The device pager that maps the pages into user space
		 * tracks their mappings, which it can only do for managed
		 * pages.
		 */
#ifdef PAGE_IS_LKPI_PAGE
		bo->pages[i]->vm_page->oflags &= ~VPO_UNMANAGED;
#else
		bo->pages[i]->oflags &= ~VPO_UNMANAGED;
#endif
	}
	bo->vaddr = vmap(bo->pages, bo->npages, VM_MAP,
	    pgprot_writecombine(PAGE_KERNEL));
	if (bo->vaddr == NULL)
		goto fail;
	return (bo);

fail:
	drm_gem_object_put(&bo->base);
	return (ERR_PTR(-ENOMEM));
}

static int
sysfbdrm_dumb_create(struct drm_file *file, struct drm_device *drm,
    struct drm_mode_create_dumb *args)
{
	struct sysfbdrm_bo *bo;
	int error;

	args->pitch = roundup(args->width * DIV_ROUND_UP(args->bpp, 8), 64);
	args->size = (uint64_t)args->pitch * args->height;
	bo = sysfbdrm_bo_create(drm, args->size);
	if (IS_ERR(bo))
		return (PTR_ERR(bo));
	error = drm_gem_handle_create(file, &bo->base, &args->handle);
	drm_gem_object_put(&bo->base);
	return (error);
}

/* Plane */

static int
sysfbdrm_plane_atomic_check(struct drm_plane *plane,
    struct drm_atomic_state *state)
{
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
	    plane);
	struct drm_crtc_state *crtc_state = NULL;
	int error;

	if (new->crtc != NULL)
		crtc_state = drm_atomic_get_new_crtc_state(state, new->crtc);
	error = drm_atomic_helper_check_plane_state(new, crtc_state,
	    DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, false, false);
	if (error != 0)
		return (error);
	/* The copy is of whole pixels. */
	if (new->visible && ((new->src.x1 | new->src.y1) & 0xffff) != 0)
		return (-EINVAL);
	return (0);
}

static void
sysfbdrm_plane_atomic_update(struct drm_plane *plane,
    struct drm_atomic_state *state)
{
	struct sysfbdrm_kms *kms = container_of(plane, struct sysfbdrm_kms,
	    plane);
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state,
	    plane);
	struct drm_plane_state *new = drm_atomic_get_new_plane_state(state,
	    plane);
	struct drm_framebuffer *fb = new->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip, scr;
	struct sysfbdrm_bo *bo;
	const uint8_t *src;
	uint8_t *dst;
	int dx, dy, idx, len, y;

	if (fb == NULL || !new->visible || !drm_dev_enter(kms->drm, &idx))
		return;
	bo = to_sysfbdrm_bo(drm_gem_fb_get_obj(fb, 0));
	/* Framebuffer coordinates to screen coordinates. */
	dx = new->dst.x1 - (new->src.x1 >> 16);
	dy = new->dst.y1 - (new->src.y1 >> 16);

	drm_atomic_helper_damage_iter_init(&iter, old, new);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		/* Never copy outside the plane's part of the screen. */
		scr = clip;
		drm_rect_translate(&scr, dx, dy);
		if (!drm_rect_intersect(&scr, &new->dst))
			continue;
		len = drm_rect_width(&scr) * SYSFBDRM_CPP;
		src = (const uint8_t *)bo->vaddr + fb->offsets[0] +
		    (scr.y1 - dy) * fb->pitches[0] +
		    (scr.x1 - dx) * SYSFBDRM_CPP;
		dst = kms->fb_vaddr + scr.y1 * kms->fb_pitch +
		    scr.x1 * SYSFBDRM_CPP;
		for (y = scr.y1; y < scr.y2; y++) {
			memcpy(dst, src, len);
			src += fb->pitches[0];
			dst += kms->fb_pitch;
		}
	}
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs sysfbdrm_plane_helper_funcs = {
	.atomic_check = sysfbdrm_plane_atomic_check,
	.atomic_update = sysfbdrm_plane_atomic_update,
};

static const struct drm_plane_funcs sysfbdrm_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/* CRTC */

static void
sysfbdrm_vblank_tick(void *arg)
{
	struct sysfbdrm_kms *kms = arg;

	sbintime_t now;

	drm_crtc_handle_vblank(&kms->crtc);
	/* A concurrent disable may have stopped the callout; don't restart it. */
	if (!atomic_load_acq_int(&kms->vblank_enabled))
		return;
	/* Absolute deadlines keep callout latency from lowering the rate. */
	now = sbinuptime();
	kms->vblank_next += kms->vblank_period;
	if (kms->vblank_next <= now)
		kms->vblank_next = now + kms->vblank_period;
	callout_schedule_sbt(&kms->vblank, kms->vblank_next, 0, C_ABSOLUTE);
}

static int
sysfbdrm_enable_vblank(struct drm_crtc *crtc)
{
	struct sysfbdrm_kms *kms = container_of(crtc, struct sysfbdrm_kms,
	    crtc);

	atomic_store_rel_int(&kms->vblank_enabled, 1);
	kms->vblank_next = sbinuptime() + kms->vblank_period;
	callout_reset_sbt(&kms->vblank, kms->vblank_next, 0,
	    sysfbdrm_vblank_tick, kms, C_ABSOLUTE);
	return (0);
}

static void
sysfbdrm_disable_vblank(struct drm_crtc *crtc)
{
	struct sysfbdrm_kms *kms = container_of(crtc, struct sysfbdrm_kms,
	    crtc);

	atomic_store_rel_int(&kms->vblank_enabled, 0);
	callout_stop(&kms->vblank);
}

static enum drm_mode_status
sysfbdrm_crtc_mode_valid(struct drm_crtc *crtc,
    const struct drm_display_mode *mode)
{
	struct sysfbdrm_kms *kms = container_of(crtc, struct sysfbdrm_kms,
	    crtc);

	if (mode->hdisplay != kms->mode.hdisplay ||
	    mode->vdisplay != kms->mode.vdisplay)
		return (MODE_ONE_SIZE);
	return (MODE_OK);
}

static int
sysfbdrm_crtc_atomic_check(struct drm_crtc *crtc,
    struct drm_atomic_state *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);

	if (!new->enable)
		return (0);
	return (drm_atomic_helper_check_crtc_primary_plane(new));
}

static void
sysfbdrm_crtc_atomic_flush(struct drm_crtc *crtc,
    struct drm_atomic_state *state)
{
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (event == NULL)
		return;
	crtc->state->event = NULL;
	spin_lock_irq(&crtc->dev->event_lock);
	if (drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void
sysfbdrm_crtc_atomic_enable(struct drm_crtc *crtc,
    struct drm_atomic_state *state)
{
	drm_crtc_vblank_on(crtc);
}

static void
sysfbdrm_crtc_atomic_disable(struct drm_crtc *crtc,
    struct drm_atomic_state *state)
{
	drm_crtc_vblank_off(crtc);
}

static const struct drm_crtc_helper_funcs sysfbdrm_crtc_helper_funcs = {
	.mode_valid = sysfbdrm_crtc_mode_valid,
	.atomic_check = sysfbdrm_crtc_atomic_check,
	.atomic_flush = sysfbdrm_crtc_atomic_flush,
	.atomic_enable = sysfbdrm_crtc_atomic_enable,
	.atomic_disable = sysfbdrm_crtc_atomic_disable,
};

static const struct drm_crtc_funcs sysfbdrm_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = sysfbdrm_enable_vblank,
	.disable_vblank = sysfbdrm_disable_vblank,
};

/* Encoder and connector */

static const struct drm_encoder_funcs sysfbdrm_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int
sysfbdrm_connector_get_modes(struct drm_connector *connector)
{
	struct sysfbdrm_kms *kms = container_of(connector, struct sysfbdrm_kms,
	    connector);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &kms->mode);
	if (mode == NULL)
		return (0);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	return (1);
}

static const struct drm_connector_helper_funcs
    sysfbdrm_connector_helper_funcs = {
	.get_modes = sysfbdrm_connector_get_modes,
};

static const struct drm_connector_funcs sysfbdrm_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_framebuffer_funcs sysfbdrm_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
	.dirty = drm_atomic_helper_dirtyfb,
};

static struct drm_framebuffer *
sysfbdrm_fb_create(struct drm_device *drm, struct drm_file *file,
    const struct drm_mode_fb_cmd2 *cmd)
{
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	uint64_t need;
	int error;

	/* Only single-plane 32 bpp formats are offered by the plane. */
	if (cmd->pixel_format != DRM_FORMAT_XRGB8888 &&
	    cmd->pixel_format != DRM_FORMAT_ARGB8888)
		return (ERR_PTR(-EINVAL));
	if (cmd->modifier[0] != DRM_FORMAT_MOD_LINEAR &&
	    (cmd->flags & DRM_MODE_FB_MODIFIERS) != 0)
		return (ERR_PTR(-EINVAL));
	if (cmd->pitches[0] < (uint64_t)cmd->width * SYSFBDRM_CPP)
		return (ERR_PTR(-EINVAL));

	/* The plane update copies from the buffer; it must cover the fb. */
	obj = drm_gem_object_lookup(file, cmd->handles[0]);
	if (obj == NULL)
		return (ERR_PTR(-ENOENT));
	need = (uint64_t)cmd->offsets[0] +
	    (uint64_t)cmd->pitches[0] * (cmd->height - 1) +
	    (uint64_t)cmd->width * SYSFBDRM_CPP;
	if (obj->funcs != &sysfbdrm_bo_funcs || need > obj->size) {
		drm_gem_object_put(obj);
		return (ERR_PTR(-EINVAL));
	}
	drm_gem_object_put(obj);

	fb = kzalloc(sizeof(*fb), GFP_KERNEL);
	if (fb == NULL)
		return (ERR_PTR(-ENOMEM));
	error = drm_gem_fb_init_with_funcs(drm, fb, file, cmd,
	    &sysfbdrm_fb_funcs);
	if (error != 0) {
		kfree(fb);
		return (ERR_PTR(error));
	}
	return (fb);
}

static const struct drm_mode_config_funcs sysfbdrm_mode_config_funcs = {
	.fb_create = sysfbdrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * vt(4) keeps drawing text in KD_GRAPHICS mode, and the efifb console writes
 * to the framebuffer we scan out.  Keep it off the framebuffer while a client
 * is DRM master; vt redraws everything when it gets the display back, on
 * KD_TEXT or a VT switch, both of which follow the drop of master.
 */
static void
sysfbdrm_master_set(struct drm_device *drm, struct drm_file *file,
    bool from_open)
{
	struct sysfbdrm_softc *sc = device_get_softc(drm->dev->bsddev);
	struct fb_info *fb;

	/*
	 * Stop vt drawing on our framebuffer, and remember whether it was
	 * this that stopped it: master_drop must not start it again when a
	 * console on another framebuffer, or another driver, stopped it.
	 */
	if (sc->vt_frozen || main_vd == NULL || main_vd->vd_driver == NULL ||
	    strcmp(main_vd->vd_driver->vd_name, "efifb") != 0)
		return;
	fb = main_vd->vd_softc;
	if ((fb->fb_flags & FB_FLAG_NOWRITE) != 0)
		return;
	vt_freeze_main_vd(sc->fb_paddr, sc->fb_size);
	sc->vt_frozen = (fb->fb_flags & FB_FLAG_NOWRITE) != 0;
}

static void
sysfbdrm_master_drop(struct drm_device *drm, struct drm_file *file)
{
	struct sysfbdrm_softc *sc = device_get_softc(drm->dev->bsddev);

	if (sc->vt_frozen) {
		vt_unfreeze_main_vd();
		sc->vt_frozen = false;
	}
}

DEFINE_DRM_GEM_FOPS(sysfbdrm_fops);

static const struct drm_driver sysfbdrm_driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops = &sysfbdrm_fops,
	.dumb_create = sysfbdrm_dumb_create,
	.master_set = sysfbdrm_master_set,
	.master_drop = sysfbdrm_master_drop,
	.name = "sysfbdrm",
	.desc = "Firmware framebuffer",
	.date = "20260929",
	.major = 1,
	.minor = 0,
};

static int
sysfbdrm_kms_init(struct sysfbdrm_softc *sc, const struct efi_fb *efifb)
{
	struct drm_device *drm = sc->drm;
	struct sysfbdrm_kms *kms;
	int error;

	kms = drmm_kzalloc(drm, sizeof(*kms), GFP_KERNEL);
	if (kms == NULL)
		return (-ENOMEM);
	sc->kms = kms;
	kms->drm = drm;
	kms->fb_vaddr = sc->fb_vaddr;
	kms->fb_pitch = efifb->fb_stride * SYSFBDRM_CPP;
	callout_init(&kms->vblank, 1);

	/* The firmware doesn't report timings; assume 60 Hz and 96 dpi. */
	kms->mode = (struct drm_display_mode){ DRM_MODE_INIT(60,
	    efifb->fb_width, efifb->fb_height,
	    DRM_MODE_RES_MM(efifb->fb_width, 96ul),
	    DRM_MODE_RES_MM(efifb->fb_height, 96ul)) };
	kms->mode.type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(&kms->mode);
	kms->vblank_period = SBT_1S / drm_mode_vrefresh(&kms->mode);

	error = drmm_mode_config_init(drm);
	if (error != 0)
		return (error);
	drm->mode_config.min_width = efifb->fb_width;
	drm->mode_config.max_width = max(efifb->fb_width, 4096u);
	drm->mode_config.min_height = efifb->fb_height;
	drm->mode_config.max_height = max(efifb->fb_height, 4096u);
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &sysfbdrm_mode_config_funcs;

	error = drm_universal_plane_init(drm, &kms->plane, 0,
	    &sysfbdrm_plane_funcs, sysfbdrm_formats,
	    nitems(sysfbdrm_formats), sysfbdrm_modifiers,
	    DRM_PLANE_TYPE_PRIMARY, NULL);
	if (error != 0)
		return (error);
	drm_plane_helper_add(&kms->plane, &sysfbdrm_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&kms->plane);

	error = drm_crtc_init_with_planes(drm, &kms->crtc, &kms->plane, NULL,
	    &sysfbdrm_crtc_funcs, NULL);
	if (error != 0)
		return (error);
	drm_crtc_helper_add(&kms->crtc, &sysfbdrm_crtc_helper_funcs);

	error = drm_encoder_init(drm, &kms->encoder, &sysfbdrm_encoder_funcs,
	    DRM_MODE_ENCODER_NONE, NULL);
	if (error != 0)
		return (error);
	kms->encoder.possible_crtcs = drm_crtc_mask(&kms->crtc);

	error = drm_connector_init(drm, &kms->connector,
	    &sysfbdrm_connector_funcs, DRM_MODE_CONNECTOR_Unknown);
	if (error != 0)
		return (error);
	drm_connector_helper_add(&kms->connector,
	    &sysfbdrm_connector_helper_funcs);
	error = drm_connector_attach_encoder(&kms->connector, &kms->encoder);
	if (error != 0)
		return (error);

	error = drm_vblank_init(drm, 1);
	if (error != 0)
		return (error);
	drm_mode_config_reset(drm);
	return (0);
}

/* Newbus glue */

static void
sysfbdrm_ldev_release(struct device *ldev)
{
	kfree(ldev);
}

static void
sysfbdrm_identify(driver_t *driver, device_t parent)
{
	if (device_find_child(parent, driver->name, DEVICE_UNIT_ANY) != NULL)
		return;
	if (sysfbdrm_efifb() == NULL)
		return;
	if (BUS_ADD_CHILD(parent, 0, driver->name, DEVICE_UNIT_ANY) == NULL)
		device_printf(parent, "failed to add %s\n", driver->name);
}

static int
sysfbdrm_probe(device_t dev)
{
	device_set_desc(dev, "EFI framebuffer");
	return (BUS_PROBE_NOWILDCARD);
}

static int sysfbdrm_detach(device_t dev);

static int
sysfbdrm_attach(device_t dev)
{
	struct sysfbdrm_softc *sc = device_get_softc(dev);
	struct efi_fb *efifb;
	struct device *ldev;
	int error;

	sc->dev = dev;
	linux_set_current(curthread);

	efifb = sysfbdrm_efifb();
	if (efifb == NULL)
		return (ENXIO);
	if (efifb->fb_mask_red != 0x00ff0000 ||
	    efifb->fb_mask_green != 0x0000ff00 ||
	    efifb->fb_mask_blue != 0x000000ff) {
		device_printf(dev, "unsupported pixel format "
		    "(r %08x g %08x b %08x)\n", efifb->fb_mask_red,
		    efifb->fb_mask_green, efifb->fb_mask_blue);
		return (ENXIO);
	}
	sc->fb_paddr = efifb->fb_addr;
	sc->fb_size = efifb->fb_size;
	sc->fb_vaddr = pmap_mapdev_attr(sc->fb_paddr, sc->fb_size,
	    VM_MEMATTR_WRITE_COMBINING);

	/*
	 * The drm device needs a LinuxKPI device.  It holds a reference, so
	 * the device is freed by its release function, not by us.
	 */
	ldev = kzalloc(sizeof(*ldev), GFP_KERNEL);
	ldev->parent = &linux_root_device;
	ldev->bsddev = dev;
	ldev->release = sysfbdrm_ldev_release;
	spin_lock_init(&ldev->devres_lock);
	INIT_LIST_HEAD(&ldev->devres_head);
	INIT_LIST_HEAD(&ldev->irqents);
	error = kobject_init_and_add(&ldev->kobj, &linux_dev_ktype,
	    &linux_root_device.kobj, device_get_nameunit(dev));
	if (error != 0) {
		kobject_put(&ldev->kobj);
		pmap_unmapdev(sc->fb_vaddr, sc->fb_size);
		return (-error);
	}
	sc->ldev = ldev;

	sc->drm = drm_dev_alloc(&sysfbdrm_driver, ldev);
	if (IS_ERR(sc->drm)) {
		error = -PTR_ERR(sc->drm);
		sc->drm = NULL;
		sysfbdrm_detach(dev);
		return (error);
	}

	error = sysfbdrm_kms_init(sc, efifb);
	if (error == 0)
		error = drm_dev_register(sc->drm, 0);
	if (error != 0) {
		device_printf(dev, "failed to initialize: %d\n", error);
		sysfbdrm_detach(dev);
		return (-error);
	}
	device_printf(dev, "%ux%u framebuffer at %#jx\n", efifb->fb_width,
	    efifb->fb_height, (uintmax_t)sc->fb_paddr);
	return (0);
}

static int
sysfbdrm_detach(device_t dev)
{
	struct sysfbdrm_softc *sc = device_get_softc(dev);

	/* Closing files later would call into the unloaded module. */
	if (sc->drm != NULL && atomic_read(&sc->drm->open_count) > 0)
		return (EBUSY);
	linux_set_current(curthread);
	if (sc->drm != NULL) {
		if (sc->drm->registered) {
			drm_dev_unplug(sc->drm);
			drm_atomic_helper_shutdown(sc->drm);
		}
		if (sc->kms != NULL) {
			atomic_store_rel_int(&sc->kms->vblank_enabled, 0);
			callout_drain(&sc->kms->vblank);
		}
		drm_dev_put(sc->drm);
		sc->drm = NULL;
	}
	if (sc->ldev != NULL) {
		put_device(sc->ldev);
		sc->ldev = NULL;
	}
	if (sc->fb_vaddr != NULL) {
		pmap_unmapdev(sc->fb_vaddr, sc->fb_size);
		sc->fb_vaddr = NULL;
	}
	return (0);
}

static device_method_t sysfbdrm_methods[] = {
	DEVMETHOD(device_identify,	sysfbdrm_identify),
	DEVMETHOD(device_probe,		sysfbdrm_probe),
	DEVMETHOD(device_attach,	sysfbdrm_attach),
	DEVMETHOD(device_detach,	sysfbdrm_detach),
	DEVMETHOD_END
};

static driver_t sysfbdrm_bsd_driver = {
	"sysfbdrm",
	sysfbdrm_methods,
	sizeof(struct sysfbdrm_softc),
};

DRIVER_MODULE(sysfbdrm, nexus, sysfbdrm_bsd_driver, NULL, NULL);
MODULE_VERSION(sysfbdrm, 1);
MODULE_DEPEND(sysfbdrm, drmn, 2, 2, 2);
MODULE_DEPEND(sysfbdrm, dmabuf, 1, 1, 1);
MODULE_DEPEND(sysfbdrm, linuxkpi, 1, 1, 1);
MODULE_DEPEND(sysfbdrm, linuxkpi_video, 1, 1, 1);
