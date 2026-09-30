#include <sys/types.h>
#include <sys/bus.h>
#include <sys/reboot.h>

#include <linux/cdev.h>
#undef cdev

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_print.h>
#include <drm/drm_os_freebsd.h>

#include <vm/vm_phys.h>

#include "vt_drmfb.h"	/* skip_ddb */

MALLOC_DEFINE(DRM_MEM_DRIVER, "drm_driver", "DRM DRIVER Data Structures");

SYSCTL_NODE(_dev, OID_AUTO, drm, CTLFLAG_RW, 0, "DRM args (compat)");
SYSCTL_LONG(_dev_drm, OID_AUTO, __drm_debug, CTLFLAG_RWTUN, &__drm_debug, 0, "drm debug flags (compat)");
SYSCTL_NODE(_hw, OID_AUTO, dri, CTLFLAG_RW, 0, "DRI args");
SYSCTL_LONG(_hw_dri, OID_AUTO, __drm_debug, CTLFLAG_RWTUN, &__drm_debug, 0, "drm debug flags");
SYSCTL_INT(_dev_drm, OID_AUTO, skip_ddb, CTLFLAG_RWTUN, &skip_ddb, 0, "go straight to dumping core (compat)");
SYSCTL_INT(_hw_dri, OID_AUTO, skip_ddb, CTLFLAG_RWTUN, &skip_ddb, 0, "go straight to dumping core");
#if defined(DRM_DEBUG_LOG_ALL)
int drm_debug_persist = 1;
#else
int drm_debug_persist = 0;
#endif
SYSCTL_INT(_dev_drm, OID_AUTO, drm_debug_persist, CTLFLAG_RWTUN, &drm_debug_persist, 0, "keep drm debug flags post-load (compat)");
SYSCTL_INT(_hw_dri, OID_AUTO, drm_debug_persist, CTLFLAG_RWTUN, &drm_debug_persist, 0, "keep drm debug flags post-load");

static struct callout reset_debug_log_handle;

static void
clear_debug_func(void *arg __unused)
{
	__drm_debug = 0;
}

void
cancel_reset_debug_log(void)
{
	callout_stop(&reset_debug_log_handle);
}

static void
reset_debug_log(void)
{
	if (drm_debug_persist)
		return;

	if (!callout_pending(&reset_debug_log_handle))
		callout_reset(&reset_debug_log_handle, 10*hz, clear_debug_func, NULL);
}

static int
sysctl_pci_id(SYSCTL_HANDLER_ARGS)
{
	uint16_t vid = arg2 & 0xFFFF;
	uint16_t pid = (arg2 >> 16) & 0xFFFF;
	char buf[32];

	snprintf(buf, sizeof(buf), "%x:%x", vid, pid);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

int
register_fictitious_range(struct drm_device *ddev, vm_paddr_t base, size_t size)
{
	int ret;

	MPASS(base != 0);
	MPASS(size != 0);

	vt_freeze_main_vd(base, size);

	ret = vm_phys_fictitious_reg_range(base, base + size,
#ifdef VM_MEMATTR_WRITE_COMBINING
					   VM_MEMATTR_WRITE_COMBINING
#else
					   VM_MEMATTR_UNCACHEABLE
#endif
	    );
	MPASS(ret == 0);

	ddev->fictitious_range_registered = true;

	return (ret);
}

void
unregister_fictitious_range(struct drm_device *ddev, vm_paddr_t base, size_t size)
{
	if (ddev->fictitious_range_registered) {
		vm_phys_fictitious_unreg_range(base, base + size);
		vt_unfreeze_main_vd();
	}
}

/* Framebuffer related code */

/* The bus ID userland identifies the device by: "pci:D:B:S.F" or "platform:<name>". */
void
drm_busid_init(struct drm_device *dev)
{
	struct pci_dev *pdev;

	if (drm_dev_is_pci(dev->dev)) {
		pdev = to_pci_dev(dev->dev);
		snprintf(dev->busid_str, sizeof(dev->busid_str),
		    "pci:%04x:%02x:%02x.%d", pci_domain_nr(pdev->bus),
		    pdev->bus->number, PCI_SLOT(pdev->devfn),
		    PCI_FUNC(pdev->devfn));
	} else
		snprintf(dev->busid_str, sizeof(dev->busid_str),
		    "platform:%s", dev_name(dev->dev));
}

int
drm_dev_alias(struct device *ldev, struct drm_minor *minor, const char *minor_str)
{
	struct sysctl_oid_list *oid_list;
	struct sysctl_ctx_list *ctx_list;
	struct linux_cdev *cdevp;
	struct sysctl_oid *node;
	device_t dev = ldev->parent->bsddev;
	char buf[32];
	u32 tmp;
	int error;

	MPASS(dev != NULL);
	/*
	 * The nodes point into the minor, so they belong to it rather than to
	 * the device, which can outlive it when the driver is unloaded.
	 */
	ctx_list = malloc(sizeof(*ctx_list), DRM_MEM_DRIVER, M_WAITOK);
	sysctl_ctx_init(ctx_list);
	minor->bsd_sysctl_ctx = ctx_list;
	snprintf(buf, sizeof(buf), "%d", minor->index);
	node = SYSCTL_ADD_NODE(ctx_list, SYSCTL_STATIC_CHILDREN(_dev_drm), OID_AUTO, buf,
	    CTLFLAG_RD, NULL, "DRM properties");
	oid_list = SYSCTL_CHILDREN(node);
	if (drm_dev_is_pci(ldev->parent)) {
		tmp = pci_get_vendor(dev) + ((u32)pci_get_device(dev) << 16);
		SYSCTL_ADD_PROC(ctx_list, oid_list, OID_AUTO, "PCI_ID",
		    CTLTYPE_STRING | CTLFLAG_RD, NULL, tmp,
		    sysctl_pci_id, "A", "PCI vendor and device ID");
	}
	SYSCTL_ADD_INT(ctx_list, oid_list, OID_AUTO, "type",
	    CTLFLAG_RD, &minor->type, 0,
	    "DRM minor type (0=primary, 2=render)");
	/* Which device the node is, for any node type (libdrm). */
	drm_busid_init(minor->dev);
	SYSCTL_ADD_STRING(ctx_list, oid_list, OID_AUTO, "busid",
	    CTLFLAG_RD, minor->dev->busid_str, 0, "Bus ID of the device");

	/*
	 * FreeBSD won't automaticaly create the corresponding device
	 * node as linux must so we find the corresponding one created by
	 * register_chrdev in drm_drv.c and alias it.
	 */
	snprintf(buf, sizeof(buf), "dri/%s", minor_str);
	cdevp = linux_find_cdev("drm", DRM_MAJOR, minor->index);
	MPASS(cdevp != NULL);
	if (cdevp == NULL)
		return (-ENXIO);
	minor->bsd_device = cdevp->cdev;
	error = make_dev_alias_p(MAKEDEV_WAITOK | MAKEDEV_CHECKNAME,
	    &minor->bsd_alias, cdevp->cdev, buf, minor->index);
	if (error != 0)
		return (-error);
	reset_debug_log();
	return (0);
}

/* Undo drm_dev_alias() when the minor is released. */
void
drm_dev_unalias(struct drm_minor *minor)
{
	if (minor->bsd_alias != NULL) {
		destroy_dev(minor->bsd_alias);
		minor->bsd_alias = NULL;
	}
	if (minor->bsd_sysctl_ctx != NULL) {
		sysctl_ctx_free(minor->bsd_sysctl_ctx);
		free(minor->bsd_sysctl_ctx, DRM_MEM_DRIVER);
		minor->bsd_sysctl_ctx = NULL;
	}
}

static int
drm_modevent(module_t mod, int type, void *data)
{
	switch (type) {
	case MOD_LOAD:
		TUNABLE_LONG_FETCH("drm.debug", &__drm_debug);
		callout_init(&reset_debug_log_handle, 1);
		break;
	case MOD_UNLOAD:
		callout_drain(&reset_debug_log_handle);
		break;
	}
	return (0);
}

static moduledata_t drm_mod = {
	"drmn",
	drm_modevent,
	0
};

DECLARE_MODULE(drmn, drm_mod, SI_SUB_DRIVERS, SI_ORDER_FIRST);
MODULE_VERSION(drmn, 2);
#ifdef CONFIG_AGP
MODULE_DEPEND(drmn, agp, 1, 1, 1);
#endif
MODULE_DEPEND(drmn, linuxkpi, 1, 1, 1);
MODULE_DEPEND(drmn, linuxkpi_video, 1, 1, 1);
MODULE_DEPEND(drmn, dmabuf, 1, 1, 1);
#ifdef CONFIG_DEBUG_FS
MODULE_DEPEND(drmn, lindebugfs, 1, 1, 1);
#endif
