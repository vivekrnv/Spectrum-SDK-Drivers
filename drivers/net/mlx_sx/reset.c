/*
 * Copyright (C) 2010-2022 NVIDIA CORPORATION & AFFILIATES, Ltd. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION & AFFILIATES, Ltd.
 * (the "Company") and all right, title, and interest in and to the software product,
 * including all associated intellectual property rights, are and shall
 * remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */

#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/mlx_sx/cmd.h>
#include <linux/mlx_sx/device.h>
#include "sx.h"
#include "dq.h"
#include "alloc.h"

static int reset_trigger = 1;
module_param_named(reset_trigger, reset_trigger, int, 0644);
MODULE_PARM_DESC(reset_trigger, "a trigger to perform chip reset");

static int debug_fw_trace_boot_flow = 0;
module_param_named(debug_fw_trace_boot_flow, debug_fw_trace_boot_flow, int, 0644);
MODULE_PARM_DESC(debug_fw_trace_boot_flow, "only for debug environment - enable when debugging FW boot flow");

#define RESET_TRIGGER_TIMEOUT       (10 * HZ)
#define SX_RESET_TIMEOUT_JIFFIES    (2 * HZ)
#define SX_SYSTEM_STATUS_REG_OFFSET 0xA1844
#define SX_SYSTEM_STATUS_REG_SIZE   4         /* byte */
#define SX_SYSTEM_STATUS_REG_MASK   0xFF
#define SX_SYSTEM_STATUS_ENABLED    0x5E
#ifdef INCREASED_TIMEOUT
    #define SX_SW_RESET_TIMEOUT_MSECS (25 * 60 * 1000)           /* 15 minutes */
#else
    #define SX_SW_RESET_TIMEOUT_MSECS (5 * 1000)           /* 5 seconds */
#endif
#define SX_HCA_HEADERS_SIZE 256

static int legacy_reset_SwitchX(struct sx_dev *dev);
static int legacy_reset(struct sx_dev *dev);
static int reset_dev_by_mrsr_reg(struct sx_dev *dev);
static int sdk_sx_reset(struct sx_dev *dev);

/* wait for device to come up after reset, depending on device type.
 * SwitchX                                                      - 3 seconds timeout.
 * Spectrum, SwitchIB, SwitchIB2	- wait for FW ready control register.
 */
static int perform_dev_sw_reset(struct sx_dev *dev)
{
    int err = 0;

    switch (dev->pdev->device) {
    /* SwitchX */
    case SWITCHX_PCI_DEV_ID:
        err = legacy_reset_SwitchX(dev);
        if (err) {
            sx_err(dev, "legacy reset for SwitchX failed, err [%d]\n", err);
            goto out;
        }

        break;

    /* Spectrum, SwitchIB */
    case SPECTRUM_PCI_DEV_ID:     /* no break */
    case SPECTRUM2_PCI_DEV_ID:     /* no break */
    case SPECTRUM3_PCI_DEV_ID: /* no break */
    case SPECTRUM4_PCI_DEV_ID: /* no break */
    case SWITCH_IB_PCI_DEV_ID:     /* no break */
    case SWITCH_IB2_PCI_DEV_ID:
    case QUANTUM_PCI_DEV_ID:
    case QUANTUM2_PCI_DEV_ID:
    case QUANTUM3_PCI_DEV_ID:
        err = sdk_sx_reset(dev);
        if (err) {
            sx_err(dev, "chip reset failed, err [%d]. Running legacy reset.\n", err);
            err = legacy_reset(dev);
            if (err) {
                sx_err(dev, "chip legacy reset failed, err [%d]\n", err);
                goto out;
            }
        }

        break;

    default:
        err = -ENODEV;
        sx_err(dev, "%s: unsupported device type [%d], err [%d]\n", __func__,
               dev->pdev->device, err);
        goto out;
        break;
    }

out:
    return err;
}

static int reset_dev_by_mrsr_reg(struct sx_dev *dev)
{
    int                       err = 0;
    struct ku_access_mrsr_reg reg_data;

    memset(&reg_data, 0, sizeof(reg_data));

    reg_data.dev_id = dev->device_id;
    reg_data.op_tlv.type = TLV_TYPE_OPERATION_E;
    reg_data.op_tlv.length = TLV_LEN;
    reg_data.op_tlv.dr = 0;
    reg_data.op_tlv.status = 0;
    reg_data.op_tlv.register_id = MRSR_REG_ID;
    reg_data.op_tlv.r = TLV_REQUEST;
    reg_data.op_tlv.method = EMAD_METHOD_WRITE;
    reg_data.op_tlv.op_class = EMAD_CLASS_REG_ACCESS;
    reg_data.op_tlv.tid = 0;

    reg_data.mrsr_reg.command = SXD_MRSR_CMD_SW_RESET;

    err = sx_ACCESS_REG_MRSR(dev, &reg_data);
    if (err) {
        printk(KERN_ERR "Failed accessing MRSR for SW reset command, err [%d]\n", err);
        goto out;
    }

out:
    return err;
}

static int __wait_for_system_ready(struct sx_dev *dev, u32 wait_for_reset_msec, u32 *time_waited_msec)
{
    void __iomem *sys_status = NULL;
    unsigned long start;
    unsigned long end;
    int           ret = -ETIME;
    u32           val;

    sys_status = ioremap(pci_resource_start(dev->pdev, 0) + SX_SYSTEM_STATUS_REG_OFFSET, SX_SYSTEM_STATUS_REG_SIZE);
    if (!sys_status) {
        printk(KERN_ERR "could not map system status register in BAR0\n");
        return -ENOMEM;
    }

    start = jiffies;
    end = jiffies + msecs_to_jiffies(wait_for_reset_msec);

    printk(KERN_INFO "device=%u, wait_for_reset=%u, start=%lu, end=%lu, HZ=%u (diff=%lu sec)\n",
           dev->pdev->device,
           wait_for_reset_msec,
           start,
           end,
           HZ,
           (end - start) / HZ);

    do {
        val = ioread32be(sys_status);
        if (SX_SYSTEM_STATUS_ENABLED == (val & SX_SYSTEM_STATUS_REG_MASK)) {
            if (time_waited_msec != NULL) {
                *time_waited_msec = jiffies_to_msecs(jiffies - start);
            }
            ret = 0;
            break;
        }

        msleep(1);
    } while (time_before(jiffies, end));

    iounmap(sys_status);
    return ret;
}


static u32 __get_chip_reset_duration(struct sx_dev *dev)
{
    u32 duration;

    switch (dev->pdev->device) {
    case QUANTUM_PCI_DEV_ID:
    case QUANTUM2_PCI_DEV_ID:
    case QUANTUM3_PCI_DEV_ID:
        duration = 15 * 1000; /* 15 seconds */
        break;

    case SPECTRUM2_PCI_DEV_ID:
    case SPECTRUM3_PCI_DEV_ID:
    case SPECTRUM4_PCI_DEV_ID:
        /* for now, until we do it in a proper way, always wait up to 15 minutes (!) for switch reset.
         * we have a special case with Tigris or Spectrum 3 setup, in which there is an upgrade for the
         * gearbox FWs and it might take up to 10 minutes.
         * here in the SDK, will give a grace of 5 more minutes for the switch to reset.
         */
        duration = 15 * 60 * 1000; /* 15 minutes */
        break;

    default:
        duration = SX_SW_RESET_TIMEOUT_MSECS;
        break;
    }

#if defined(PD_BU) && defined(QUANTUM3_BU)
    duration = 20 * 60 * 1000; /* wait 20 minutes for reset on palladium */
#endif

    return duration;
}


static int sdk_sx_reset(struct sx_dev *dev)
{
    struct sx_priv *priv = sx_priv(dev);
    u32             wait_for_reset, time_waited;
    int             err = 0;

    wait_for_reset = __get_chip_reset_duration(dev);

    printk(KERN_INFO "wait for system to be ready before reset\n");

    err = __wait_for_system_ready(dev, wait_for_reset, &time_waited);
    if (err) {
        printk(KERN_ERR "system is not ready and cannot be reset (err=%d)!\n", err);
        goto out;
    }

    printk(KERN_INFO "system is ready for reset [waited %u msec], performing reset now\n", time_waited);

    /* actually hit reset */
    priv->dev_sw_rst_flow = true;
    err = reset_dev_by_mrsr_reg(dev);
    if (err) {
        printk(KERN_ERR "Failed filling MRSR data, err [%d]\n", err);
        goto out;
    }

    /* verify that system status is not enabled due to MRSR */
    err = __wait_for_system_ready(dev, 0, &time_waited);
    if (err != -ETIME) {
        /* we've got a problem. system is enabled immediately after reset.
         * it means that the reset did not actually work. */

        printk(KERN_ERR "system is ready immediately after a reset command has been sent (err=%d)\n", err);
        err = -EFAULT;
        goto out;
    }

    /* now wait for reset to be completed */
    err = __wait_for_system_ready(dev, wait_for_reset, &time_waited);
    if (err) {
        printk(KERN_ERR "system status timeout after reset! (err=%d)\n", err);
        goto out;
    }

    printk(KERN_INFO "system is ready after reset [waited %u msec]\n", time_waited);

out:
    priv->dev_sw_rst_flow = false;
    return err;
}

/* This function saves PCI headers for restoration after SW reset,
 * using __restore_headers_data, according to device type.
 * SwitchX                                                      - Saves and restores PCI headers.
 * Spectrum, SwitchIB, SwitchIB2	- Doesn't save PCI headers.
 *
 *
 * @param dev[in]				- sx device.
 * @param hca_header_p[in/out]	- hca headers, must be size of SX_HCA_HEADERS_SIZE.
 */
static int __save_headers_data(struct sx_dev *dev, u32* hca_header_p)
{
    int err = 0;
    int i = 0;
    int pcie_cap = 0;

    memset(hca_header_p, 0, SX_HCA_HEADERS_SIZE);

    pcie_cap = pci_find_capability(dev->pdev, PCI_CAP_ID_EXP);

    /* We skip config space offsets 22
     * and 23 since those have a special meaning.
     */
    for (i = 0; i < 64; ++i) {
        if ((i == 22) || (i == 23)) {
            continue;
        }
        if (pci_read_config_dword(dev->pdev, i * 4, hca_header_p + i)) {
            err = -ENODEV;
            sx_err(dev, "%s: Couldn't save HCA PCI header, aborting, err[%d]\n", __func__, err);
            goto out;
        }
    }

out:
    return err;
}


/* This function restores PCI headers after SW reset, according to
 * headers as saved by __save_headers_data, according to device type.
 * SwitchX                                                      - Restores and restores PCI headers.
 * Spectrum, SwitchIB, SwitchIB2	- Doesn't restore PCI headers.
 *
 *
 * @param dev[in]				- sx device.
 * @param hca_header_p[in]	    - hca headers, must be size of SX_HCA_HEADERS_SIZE.
 */
static int __restore_headers_data(struct sx_dev *dev, u32* hca_header_p)
{
    int err = 0;
    int pcie_cap;
    int i = 0;
    u16 devctl = 0;
    u16 linkctl = 0;

    /* restore PCIE headers to restore after reset from hca_header_p */
    /* Now restore the PCI headers */
    pcie_cap = pci_find_capability(dev->pdev, PCI_CAP_ID_EXP);
    if (pcie_cap) {
        devctl = hca_header_p[(pcie_cap + PCI_EXP_DEVCTL) / 4];
        if (pci_write_config_word(dev->pdev, pcie_cap + PCI_EXP_DEVCTL, devctl)) {
            err = -ENODEV;
            sx_err(dev, "%s: Couldn't restore HCA PCI Express "
                   "Device Control register, aborting, err[%d]\n", __func__, err);
            goto out;
        }

        linkctl = hca_header_p[(pcie_cap + PCI_EXP_LNKCTL) / 4];
        if (pci_write_config_word(dev->pdev, pcie_cap + PCI_EXP_LNKCTL,
                                  linkctl)) {
            err = -ENODEV;
            sx_err(dev, "%s: Couldn't restore HCA PCI Express "
                   "Link control register, aborting, err[%d]\n", __func__, err);
            goto out;
        }
    }

    for (i = 0; i < 16; ++i) {
        if (i * 4 == PCI_COMMAND) {
            continue;
        }

        if (pci_write_config_dword(dev->pdev, i * 4, hca_header_p[i])) {
            err = -ENODEV;
            sx_err(dev, "%s: Couldn't restore HCA reg %x, aborting, err[%d]\n", __func__, i, err);
            goto out;
        }
    }

    if (pci_write_config_dword(dev->pdev, PCI_COMMAND,
                               hca_header_p[PCI_COMMAND / 4])) {
        err = -ENODEV;
        sx_err(dev, "%s: Couldn't restore HCA COMMAND, aborting, err[%d]\n", __func__, err);
        goto out;
    }

out:
    return err;
}


/* This function is used to read system status from FW
 *
 * @param dev[in]				- sx device.
 * @param system_status[out]	- system status.
 */

int get_system_status(struct sx_dev *dev, u16 *system_status)
{
    int           err = 0;
    u32           val = 0;
    void __iomem *sys_status_addr = NULL;

    if (!dev) {
        err = -EINVAL;
        printk(KERN_ERR "%s: Given null device parameter, err [%d]\n", __func__, err);
        goto out;
    }
    if (!system_status) {
        err = -EINVAL;
        sx_err(dev, "%s: Given null system status device parameter, err [%d]\n", __func__, err);
        goto out;
    }

    sys_status_addr =
        ioremap(pci_resource_start(dev->pdev, 0) + SX_SYSTEM_STATUS_REG_OFFSET, SX_SYSTEM_STATUS_REG_SIZE);
    if (!sys_status_addr) {
        err = -ENOMEM;
        sx_err(dev, "%s: Couldn't map HCA reset register, err [%d]\n", __func__, err);
        goto out;
    }

    val = ioread32be(sys_status_addr);
    *system_status = val & SX_SYSTEM_STATUS_REG_MASK;

    iounmap(sys_status_addr);

out:
    return err;
}


static int __do_legacy_reset(struct sx_dev *dev)
{
    void __iomem *reset;
    int           err = 0;

#define SX_RESET_BASE  0xf0010
#define SX_RESET_SIZE  (4)
#define SX_RESET_VALUE swab32(1)

    /*
     * Reset the chip.  This is somewhat ugly because we have to
     * save off the PCI header before reset and then restore it
     * after the chip reboots.  We skip config space offsets 22
     * and 23 since those have a special meaning.
     */
    reset = ioremap(pci_resource_start(dev->pdev, 0) + SX_RESET_BASE, SX_RESET_SIZE);
    if (!reset) {
        err = -ENOMEM;
        sx_err(dev, "Couldn't map reset register, aborting.\n");
        goto out;
    }

    /* actually hit reset */
    writel(SX_RESET_VALUE, reset);
    iounmap(reset);

    /* Wait three seconds before accessing device */
#ifndef INCREASED_TIMEOUT
    msleep(3000);
#else
    msleep(180000);
#endif

out:
    return err;
}


static int legacy_reset_SwitchX(struct sx_dev *dev)
{
    u16           vendor = 0xffff;
    unsigned long end;
    int           err = 0;

    printk(KERN_INFO PFX "performing SwitchX legacy reset\n");

    if (!dev->pdev) {
        sx_err(dev, "SW reset will not be executed since PCI device is not present");
        err = -ENODEV;
        goto out;
    }

    err = __do_legacy_reset(dev);
    if (err) {
        sx_err(dev, "failed SwitchX legacy reset [err=%d]\n", err);
        goto out;
    }

    /* SwitchX does not support System_Status register, so we will poll the vendor-id */
    end = jiffies + SX_RESET_TIMEOUT_JIFFIES;
    do {
        if (!pci_read_config_word(dev->pdev, PCI_VENDOR_ID, &vendor) && (vendor != 0xffff)) {
            break;
        }

        msleep(1);
    } while (time_before(jiffies, end));

    if (vendor == 0xffff) {
        err = -ENODEV;
        sx_err(dev, "PCI device did not come back after reset, aborting.\n");
        goto out;
    }

out:
    return err;
}


static int legacy_reset(struct sx_dev *dev)
{
    struct sx_priv *priv = sx_priv(dev);
    int             err = 0;
    u32             wait_for_reset, time_waited;

    printk(KERN_INFO PFX "performing legacy SW reset\n");

    if (!dev->pdev) {
        sx_err(dev, "SW reset will not be executed since PCI device is not present");
        err = -ENODEV;
        goto out;
    }

    wait_for_reset = __get_chip_reset_duration(dev);

    /* we fall-back from MRSR to legacy reset. it is not a common flow.
     * in legacy reset we will wait double the time that we did on MRSR! */
    wait_for_reset *= 2;

    printk(KERN_INFO "wait for system to be ready before legacy reset\n");

    err = __wait_for_system_ready(dev, wait_for_reset, &time_waited);
    if (err) {
        printk(KERN_ERR "system is not ready and cannot be reset (err=%d)!\n", err);
        goto out;
    }

    printk(KERN_INFO "system is ready for reset [waited %u msec], performing legacy reset now\n", time_waited);

    priv->dev_sw_rst_flow = true;

    err = __do_legacy_reset(dev);
    if (err) {
        sx_err(dev, "failed chip legacy reset [err=%d]\n", err);
        goto out;
    }

    /* now wait for reset to be completed */
    err = __wait_for_system_ready(dev, wait_for_reset, &time_waited);
    if (err) {
        printk(KERN_ERR "system status timeout after legacy reset! (err=%d)\n", err);
        goto out;
    }

    printk(KERN_INFO "system is ready after legacy reset [waited %u msec]\n", time_waited);

out:
    priv->dev_sw_rst_flow = false;
    return err;
}


int sx_reset(struct sx_dev *dev, u8 perform_chip_reset)
{
    u32                 *hca_header = NULL;
    u16                  vendor = 0xffff;
    unsigned long        end;
    int                  err = 0;
    bool                 is_pre_reset_event = false;
    union sx_event_data *event_data = NULL;

    if ((dev == NULL) || !dev->pdev) {
        printk(KERN_ERR "SW reset will not be executed since PCI device is not present\n");
        err = -ENODEV;
        goto out;
    }

    event_data = kzalloc(sizeof(union sx_event_data), GFP_KERNEL);
    if (!event_data) {
        printk(KERN_ERR PFX "Failed to allocate memory for event data.\n");
        err = -ENOMEM;
        goto out;
    }

    if (SWITCHX_PCI_DEV_ID == dev->pdev->device) {
        hca_header = kmalloc(SX_HCA_HEADERS_SIZE, GFP_KERNEL);
        if (!hca_header) {
            err = -ENOMEM;
            sx_err(dev, "%s: Couldn't allocate memory to save HCA "
                   "PCI header, aborting, err[%d]\n", __func__, err);
            goto out;
        }

        err = __save_headers_data(dev, hca_header);
        if (err) {
            sx_err(dev, "PCI device reset failed saving PCI headers data, err [%d].\n", err);
            goto out;
        }
    }

    /* return device to use polling */
    sx_cmd_use_polling(dev);

    if (reset_trigger) {
        sx_info(dev, "reset trigger is already set\n");
    } else {
        sx_info(dev, "waiting for reset trigger\n");

        end = jiffies + RESET_TRIGGER_TIMEOUT;

        while (!reset_trigger && time_before(jiffies, end)) {
            msleep(100);
        }

        if (reset_trigger) {
            sx_info(dev, "reset trigger is set\n");
        } else {
            sx_err(dev, "reset trigger timeout. self triggering.\n");
            reset_trigger = 1;
        }
    }

    if (perform_chip_reset) {
        pr_debug("Performing chip reset in this phase\n");

        err = sx_core_dispatch_event(dev, SX_DEV_EVENT_PRE_RESET, NULL);
        is_pre_reset_event = true;
        if (err) {
            sx_err(dev, "PRE_RESET event failed, err [%d].\n", err);
            goto out;
        }

        err = perform_dev_sw_reset(dev);
        if (err) {
            sx_err(dev, "PCI device reset failed waiting for device, err [%d].\n", err);
            goto out;
        }

        end = jiffies + SX_RESET_TIMEOUT_JIFFIES;
        do {
            if (!pci_read_config_word(dev->pdev, PCI_VENDOR_ID, &vendor) &&
                (vendor != 0xffff)) {
                break;
            }

            msleep(1);
        } while (time_before(jiffies, end));

        if (vendor == 0xffff) {
            err = -ENODEV;
            sx_err(dev, "PCI device did not come back after reset, aborting.\n");
            goto out;
        }
    } else {
        pr_debug("Did not perform chip reset in this phase. Check system is ready.\n");

        if (!debug_fw_trace_boot_flow) {
            /* check that system is in ready state */
            err = __wait_for_system_ready(dev, 0, NULL);
            if (err) {
                err = -ENODEV;
                printk(KERN_ERR "system is not ready.\n");
                goto out;
            }
        }
    }

    if (SWITCHX_PCI_DEV_ID == dev->pdev->device) {
        /* Now restore the PCI headers */
        err = __restore_headers_data(dev, hca_header);
        if (err) {
            sx_err(dev, "PCI device reset failed restoring PCI headers data, err [%d].\n", err);
            goto out;
        }
    }

out:
    if (hca_header) {
        kfree(hca_header);
    }

    if (event_data) {
        if (is_pre_reset_event) {
            event_data->post_reset.err = err;
            err = sx_core_dispatch_event(dev, SX_DEV_EVENT_POST_RESET, event_data);
            if (err) {
                sx_err(dev, "POST_RESET event failed, err [%d].\n", err);
            }
            err = event_data->post_reset.err;
        }

        kfree(event_data);
    }

    return err;
}