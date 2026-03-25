#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/export-internal.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_MITIGATION_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xc1514a3b, "free_irq" },
	{ 0x4126521a, "pci_request_region" },
	{ 0xe4adbee8, "misc_deregister" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0x709c09b0, "devm_kmalloc" },
	{ 0xd086e8e6, "pci_enable_device" },
	{ 0x4a453f53, "iowrite32" },
	{ 0x4d9208e2, "pci_iomap" },
	{ 0xc165389, "pci_alloc_irq_vectors" },
	{ 0xc8c85086, "sg_free_table" },
	{ 0x92540fbf, "finish_wait" },
	{ 0xbc771b1f, "__pci_register_driver" },
	{ 0x3bc9fa63, "unpin_user_pages" },
	{ 0x8c26d495, "prepare_to_wait_event" },
	{ 0xe2964344, "__wake_up" },
	{ 0x1e85d180, "pci_irq_vector" },
	{ 0x34db050b, "_raw_spin_lock_irqsave" },
	{ 0xa33adda, "__dynamic_dev_dbg" },
	{ 0x7c1102b1, "pci_unregister_driver" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0x3e5f1cef, "pin_user_pages" },
	{ 0x92997ed8, "_printk" },
	{ 0x1000e51, "schedule" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x4412517b, "_dev_info" },
	{ 0x7cd8d75e, "page_offset_base" },
	{ 0x599fb41c, "kvmalloc_node" },
	{ 0xfe487975, "init_wait_entry" },
	{ 0x369d3df3, "_dev_err" },
	{ 0x92d5838e, "request_threaded_irq" },
	{ 0xd3b80b2f, "dma_alloc_attrs" },
	{ 0xd35cce70, "_raw_spin_unlock_irqrestore" },
	{ 0x4082f2a9, "pci_iounmap" },
	{ 0xe1359c44, "sg_alloc_table_from_pages_segment" },
	{ 0xca66f6d3, "_dev_warn" },
	{ 0xe43fe535, "misc_register" },
	{ 0x13e648fe, "pci_set_master" },
	{ 0x5b8239ca, "__x86_return_thunk" },
	{ 0x6b10bee1, "_copy_to_user" },
	{ 0xd9a5ea54, "__init_waitqueue_head" },
	{ 0x4ed56abd, "dma_set_coherent_mask" },
	{ 0x97651e6c, "vmemmap_base" },
	{ 0x6983fbb5, "dma_free_attrs" },
	{ 0xfbe215e4, "sg_next" },
	{ 0xf6b4b72c, "pci_disable_device" },
	{ 0xee790a06, "dma_set_mask" },
	{ 0x8ae85807, "dma_unmap_sg_attrs" },
	{ 0x7aa1756e, "kvfree" },
	{ 0x3e9d7c51, "pci_release_region" },
	{ 0xec0ba69d, "pci_free_irq_vectors" },
	{ 0xe2c17b5d, "__SCT__might_resched" },
	{ 0xe412a233, "dma_map_sg_attrs" },
	{ 0x33bd423, "module_layout" },
};

MODULE_INFO(depends, "");

MODULE_ALIAS("pci:v000010EEd00009038sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v000010EEd00009028sv*sd*bc*sc*i*");

MODULE_INFO(srcversion, "FC4B8217AC04118668106B5");
MODULE_INFO(rhelversion, "9.7");
