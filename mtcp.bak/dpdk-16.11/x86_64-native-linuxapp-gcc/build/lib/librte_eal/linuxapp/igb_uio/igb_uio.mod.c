#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

__visible struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0x1fc32c62, __VMLINUX_SYMBOL_STR(module_layout) },
	{ 0x5d41c87c, __VMLINUX_SYMBOL_STR(param_ops_charp) },
	{ 0x9ecce67a, __VMLINUX_SYMBOL_STR(pci_unregister_driver) },
	{ 0x6bc3fbc0, __VMLINUX_SYMBOL_STR(__unregister_chrdev) },
	{ 0xa348685e, __VMLINUX_SYMBOL_STR(__pci_register_driver) },
	{ 0x4e317bdf, __VMLINUX_SYMBOL_STR(__register_chrdev) },
	{ 0xe2d5255a, __VMLINUX_SYMBOL_STR(strcmp) },
	{ 0x27e1a049, __VMLINUX_SYMBOL_STR(printk) },
	{ 0xdef909b2, __VMLINUX_SYMBOL_STR(__dynamic_dev_dbg) },
	{ 0x33fe096c, __VMLINUX_SYMBOL_STR(__uio_register_device) },
	{ 0x20c55ae0, __VMLINUX_SYMBOL_STR(sscanf) },
	{ 0x8e0cbc7a, __VMLINUX_SYMBOL_STR(pci_intx_mask_supported) },
	{ 0x5bc62333, __VMLINUX_SYMBOL_STR(dev_notice) },
	{ 0x29bb6f, __VMLINUX_SYMBOL_STR(pci_release_selected_regions) },
	{ 0xb9dfd3c4, __VMLINUX_SYMBOL_STR(pci_select_bars) },
	{ 0xdb694183, __VMLINUX_SYMBOL_STR(register_netdev) },
	{ 0x9166fada, __VMLINUX_SYMBOL_STR(strncpy) },
	{ 0x72399536, __VMLINUX_SYMBOL_STR(alloc_etherdev_mqs) },
	{ 0x27b987db, __VMLINUX_SYMBOL_STR(sysfs_create_group) },
	{ 0x5e9a9361, __VMLINUX_SYMBOL_STR(pci_enable_msix) },
	{ 0xcfc62f27, __VMLINUX_SYMBOL_STR(xen_start_info) },
	{ 0x731dba7a, __VMLINUX_SYMBOL_STR(xen_domain_type) },
	{ 0x203b1c2, __VMLINUX_SYMBOL_STR(dma_supported) },
	{ 0x1d7c2a5c, __VMLINUX_SYMBOL_STR(dma_set_mask) },
	{ 0x42c8de35, __VMLINUX_SYMBOL_STR(ioremap_nocache) },
	{ 0x905e1518, __VMLINUX_SYMBOL_STR(pci_set_master) },
	{ 0x6274557a, __VMLINUX_SYMBOL_STR(dev_err) },
	{ 0x8cd3f8d2, __VMLINUX_SYMBOL_STR(pci_enable_device) },
	{ 0x1a236d64, __VMLINUX_SYMBOL_STR(kmem_cache_alloc_trace) },
	{ 0x6a4bf290, __VMLINUX_SYMBOL_STR(kmalloc_caches) },
	{ 0x44729265, __VMLINUX_SYMBOL_STR(pci_check_and_mask_intx) },
	{ 0xc30d0aa3, __VMLINUX_SYMBOL_STR(pci_intx) },
	{ 0xb4a392e3, __VMLINUX_SYMBOL_STR(pci_cfg_access_unlock) },
	{ 0xc05b4895, __VMLINUX_SYMBOL_STR(pci_cfg_access_lock) },
	{ 0xbf2a314d, __VMLINUX_SYMBOL_STR(remap_pfn_range) },
	{ 0xc715d9e0, __VMLINUX_SYMBOL_STR(boot_cpu_data) },
	{ 0xbbe9d805, __VMLINUX_SYMBOL_STR(pci_disable_msix) },
	{ 0xbbf25a70, __VMLINUX_SYMBOL_STR(unregister_netdev) },
	{ 0x37a0cba, __VMLINUX_SYMBOL_STR(kfree) },
	{ 0x5fe56825, __VMLINUX_SYMBOL_STR(dev_set_drvdata) },
	{ 0x77019e6d, __VMLINUX_SYMBOL_STR(pci_disable_device) },
	{ 0x1b669713, __VMLINUX_SYMBOL_STR(uio_unregister_device) },
	{ 0xec6d3c84, __VMLINUX_SYMBOL_STR(sysfs_remove_group) },
	{ 0x2930b797, __VMLINUX_SYMBOL_STR(free_netdev) },
	{ 0xb749ae89, __VMLINUX_SYMBOL_STR(dev_get_drvdata) },
	{ 0x28318305, __VMLINUX_SYMBOL_STR(snprintf) },
	{ 0xe814631, __VMLINUX_SYMBOL_STR(pci_bus_type) },
	{ 0xf46ef563, __VMLINUX_SYMBOL_STR(pci_enable_sriov) },
	{ 0x84623aa1, __VMLINUX_SYMBOL_STR(pci_num_vf) },
	{ 0x1364cdd9, __VMLINUX_SYMBOL_STR(pci_disable_sriov) },
	{ 0x3c80c06c, __VMLINUX_SYMBOL_STR(kstrtoull) },
	{ 0xedc03953, __VMLINUX_SYMBOL_STR(iounmap) },
	{ 0x288a77b9, __VMLINUX_SYMBOL_STR(_dev_info) },
	{ 0xbdfb6dbb, __VMLINUX_SYMBOL_STR(__fentry__) },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=uio";


MODULE_INFO(srcversion, "7AF52BA54B850AD84EBE0D8");
