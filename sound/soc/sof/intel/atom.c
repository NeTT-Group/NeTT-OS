// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018-2021 Intel Corporation
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//

/*
 * Hardware interface for audio DSP on Atom devices
 */

#include <linux/module.h>
#include <sound/sof.h>
#include <sound/sof/xtensa.h>
#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>
#include <sound/intel-dsp-config.h>
#include "../ops.h"
#include "shim.h"
#include "atom.h"
#include "../sof-acpi-dev.h"
#include "../sof-audio.h"
#include "../../intel/common/soc-intel-quirks.h"

static void atom_host_done(struct snd_sof_dev *sdev);
static void atom_dsp_done(struct snd_sof_dev *sdev);

/*
 * Debug
 */

static void atom_get_registers(struct snd_sof_dev *sdev,
			       struct sof_ipc_dsp_oops_xtensa *xoops,
			       struct sof_ipc_panic_info *panic_info,
			       u32 *stack, size_t stack_words)
{
	u32 offset = sdev->dsp_oops_offset;

	/* first read regsisters */
	sof_mailbox_read(sdev, offset, xoops, sizeof(*xoops));

	/* note: variable AR register array is not read */

	/* then get panic info */
	if (xoops->arch_hdr.totalsize > EXCEPT_MAX_HDR_SIZE) {
		dev_err(sdev->dev, "invalid header size 0x%x. FW oops is bogus\n",
			xoops->arch_hdr.totalsize);
		return;
	}
	offset += xoops->arch_hdr.totalsize;
	sof_mailbox_read(sdev, offset, panic_info, sizeof(*panic_info));

	/* then get the stack */
	offset += sizeof(*panic_info);
	sof_mailbox_read(sdev, offset, stack, stack_words * sizeof(u32));
}

void atom_dump(struct snd_sof_dev *sdev, u32 flags)
{
	struct sof_ipc_dsp_oops_xtensa xoops;
	struct sof_ipc_panic_info panic_info;
	u32 stack[STACK_DUMP_SIZE];
	u64 status, panic, imrd, imrx;

	/* now try generic SOF status messages */
	status = snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_IPCD);
	panic = snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_IPCX);
	atom_get_registers(sdev, &xoops, &panic_info, stack,
			   STACK_DUMP_SIZE);
	sof_print_oops_and_stack(sdev, KERN_ERR, status, panic, &xoops,
				 &panic_info, stack, STACK_DUMP_SIZE);

	/* provide some context for firmware debug */
	imrx = snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_IMRX);
	imrd = snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_IMRD);
	dev_err(sdev->dev,
		"error: ipc host -> DSP: pending %s complete %s raw 0x%llx\n",
		str_yes_no(panic & SHIM_IPCX_BUSY),
		str_yes_no(panic & SHIM_IPCX_DONE), panic);
	dev_err(sdev->dev,
		"error: mask host: pending %s complete %s raw 0x%llx\n",
		str_yes_no(imrx & SHIM_IMRX_BUSY),
		str_yes_no(imrx & SHIM_IMRX_DONE), imrx);
	dev_err(sdev->dev,
		"error: ipc DSP -> host: pending %s complete %s raw 0x%llx\n",
		str_yes_no(status & SHIM_IPCD_BUSY),
		str_yes_no(status & SHIM_IPCD_DONE), status);
	dev_err(sdev->dev,
		"error: mask DSP: pending %s complete %s raw 0x%llx\n",
		str_yes_no(imrd & SHIM_IMRD_BUSY),
		str_yes_no(imrd & SHIM_IMRD_DONE), imrd);

}
EXPORT_SYMBOL_NS(atom_dump, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

/*
 * IPC Doorbell IRQ handler and thread.
 */

irqreturn_t atom_irq_handler(int irq, void *context)
{
	struct snd_sof_dev *sdev = context;
	u64 ipcx, ipcd;
	int ret = IRQ_NONE;

	ipcx = snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_IPCX);
	ipcd = snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_IPCD);

	if (ipcx & SHIM_BYT_IPCX_DONE) {

		/* reply message from DSP, Mask Done interrupt first */
		snd_sof_dsp_update_bits64_unlocked(sdev, DSP_BAR,
						   SHIM_IMRX,
						   SHIM_IMRX_DONE,
						   SHIM_IMRX_DONE);
		ret = IRQ_WAKE_THREAD;
	}

	if (ipcd & SHIM_BYT_IPCD_BUSY) {

		/* new message from DSP, Mask Busy interrupt first */
		snd_sof_dsp_update_bits64_unlocked(sdev, DSP_BAR,
						   SHIM_IMRX,
						   SHIM_IMRX_BUSY,
						   SHIM_IMRX_BUSY);
		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}
EXPORT_SYMBOL_NS(atom_irq_handler, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

irqreturn_t atom_irq_thread(int irq, void *context)
{
	struct snd_sof_dev *sdev = context;
	u64 ipcx, ipcd;

	ipcx = snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_IPCX);
	ipcd = snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_IPCD);

	/* reply message from DSP */
	if (ipcx & SHIM_BYT_IPCX_DONE) {

		spin_lock_irq(&sdev->ipc_lock);

		/*
		 * handle immediate reply from DSP core. If the msg is
		 * found, set done bit in cmd_done which is called at the
		 * end of message processing function, else set it here
		 * because the done bit can't be set in cmd_done function
		 * which is triggered by msg
		 */
		snd_sof_ipc_process_reply(sdev, ipcx);

		atom_dsp_done(sdev);

		spin_unlock_irq(&sdev->ipc_lock);
	}

	/* new message from DSP */
	if (ipcd & SHIM_BYT_IPCD_BUSY) {

		/* Handle messages from DSP Core */
		if ((ipcd & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
			snd_sof_dsp_panic(sdev, PANIC_OFFSET(ipcd) + MBOX_OFFSET,
					  true);
		} else {
			snd_sof_ipc_msgs_rx(sdev);
		}

		atom_host_done(sdev);
	}

	return IRQ_HANDLED;
}
EXPORT_SYMBOL_NS(atom_irq_thread, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

int atom_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	/* unmask and prepare to receive Done interrupt */
	snd_sof_dsp_update_bits64_unlocked(sdev, DSP_BAR, SHIM_IMRX,
					   SHIM_IMRX_DONE, 0);

	/* send the message */
	sof_mailbox_write(sdev, sdev->host_box.offset, msg->msg_data,
			  msg->msg_size);
	snd_sof_dsp_write64(sdev, DSP_BAR, SHIM_IPCX, SHIM_BYT_IPCX_BUSY);

	return 0;
}
EXPORT_SYMBOL_NS(atom_send_msg, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

int atom_get_mailbox_offset(struct snd_sof_dev *sdev)
{
	return MBOX_OFFSET;
}
EXPORT_SYMBOL_NS(atom_get_mailbox_offset, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

int atom_get_window_offset(struct snd_sof_dev *sdev, u32 id)
{
	return MBOX_OFFSET;
}
EXPORT_SYMBOL_NS(atom_get_window_offset, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

static void atom_host_done(struct snd_sof_dev *sdev)
{
	/* clear BUSY bit and set DONE bit - accept new messages */
	snd_sof_dsp_update_bits64_unlocked(sdev, DSP_BAR, SHIM_IPCD,
					   SHIM_BYT_IPCD_BUSY |
					   SHIM_BYT_IPCD_DONE,
					   SHIM_BYT_IPCD_DONE);

	/* unmask and prepare to receive next new message */
	snd_sof_dsp_update_bits64_unlocked(sdev, DSP_BAR, SHIM_IMRX,
					   SHIM_IMRX_BUSY, 0);
}

static void atom_dsp_done(struct snd_sof_dev *sdev)
{
	/* clear DONE bit - tell DSP we have completed */
	snd_sof_dsp_update_bits64_unlocked(sdev, DSP_BAR, SHIM_IPCX,
					   SHIM_BYT_IPCX_DONE, 0);
}

/*
 * DSP control.
 */

int atom_run(struct snd_sof_dev *sdev)
{
	int tries = 10;

	/* release stall and wait to unstall */
	snd_sof_dsp_update_bits64(sdev, DSP_BAR, SHIM_CSR,
				  SHIM_BYT_CSR_STALL, 0x0);
	while (tries--) {
		if (!(snd_sof_dsp_read64(sdev, DSP_BAR, SHIM_CSR) &
		      SHIM_BYT_CSR_PWAITMODE))
			break;
		msleep(100);
	}
	if (tries < 0)
		return -ENODEV;

	/* return init core mask */
	return 1;
}
EXPORT_SYMBOL_NS(atom_run, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

int atom_reset(struct snd_sof_dev *sdev)
{
	/* put DSP into reset, set reset vector and stall */
	snd_sof_dsp_update_bits64(sdev, DSP_BAR, SHIM_CSR,
				  SHIM_BYT_CSR_RST | SHIM_BYT_CSR_VECTOR_SEL |
				  SHIM_BYT_CSR_STALL,
				  SHIM_BYT_CSR_RST | SHIM_BYT_CSR_VECTOR_SEL |
				  SHIM_BYT_CSR_STALL);

	usleep_range(10, 15);

	/* take DSP out of reset and keep stalled for FW loading */
	snd_sof_dsp_update_bits64(sdev, DSP_BAR, SHIM_CSR,
				  SHIM_BYT_CSR_RST, 0);

	return 0;
}
EXPORT_SYMBOL_NS(atom_reset, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

static const char *fixup_tplg_name(struct snd_sof_dev *sdev,
				   const char *sof_tplg_filename,
				   const char *ssp_str)
{
	const char *tplg_filename = NULL;
	const char *split_ext;
	char *filename, *tmp;

	filename = kstrdup(sof_tplg_filename, GFP_KERNEL);
	if (!filename)
		return NULL;

	/* this assumes a .tplg extension */
	tmp = filename;
	split_ext = strsep(&tmp, ".");
	if (split_ext)
		tplg_filename = devm_kasprintf(sdev->dev, GFP_KERNEL,
					       "%s-%s.tplg",
					       split_ext, ssp_str);
	kfree(filename);

	return tplg_filename;
}

struct snd_soc_acpi_mach *atom_machine_select(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *sof_pdata = sdev->pdata;
	const struct sof_dev_desc *desc = sof_pdata->desc;
	struct snd_soc_acpi_mach *mach;
	struct platform_device *pdev;
	const char *tplg_filename;

	mach = snd_soc_acpi_find_machine(desc->machines);
	if (!mach) {
		dev_warn(sdev->dev, "warning: No matching ASoC machine driver found\n");
		return NULL;
	}

	pdev = to_platform_device(sdev->dev);
	if (soc_intel_is_byt_cr(pdev)) {
		dev_dbg(sdev->dev,
			"BYT-CR detected, SSP0 used instead of SSP2\n");

		tplg_filename = fixup_tplg_name(sdev,
						mach->sof_tplg_filename,
						"ssp0");
	} else {
		tplg_filename = mach->sof_tplg_filename;
	}

	if (!tplg_filename) {
		dev_dbg(sdev->dev,
			"error: no topology filename\n");
		return NULL;
	}

	sof_pdata->tplg_filename = tplg_filename;
	mach->mach_params.acpi_ipc_irq_index = desc->irqindex_host_ipc;

	return mach;
}
EXPORT_SYMBOL_NS(atom_machine_select, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

/* Atom DAIs */
struct snd_soc_dai_driver atom_dai[] = {
{
	.name = "ssp0-port",
	.playback = {
		.channels_min = 1,
		.channels_max = 8,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 8,
	},
},
{
	.name = "ssp1-port",
	.playback = {
		.channels_min = 1,
		.channels_max = 8,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 8,
	},
},
{
	.name = "ssp2-port",
	.playback = {
		.channels_min = 1,
		.channels_max = 8,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 8,
	}
},
{
	.name = "ssp3-port",
	.playback = {
		.channels_min = 1,
		.channels_max = 8,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 8,
	},
},
{
	.name = "ssp4-port",
	.playback = {
		.channels_min = 1,
		.channels_max = 8,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 8,
	},
},
{
	.name = "ssp5-port",
	.playback = {
		.channels_min = 1,
		.channels_max = 8,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 8,
	},
},
};
EXPORT_SYMBOL_NS(atom_dai, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

void atom_set_mach_params(struct snd_soc_acpi_mach *mach,
			  struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *pdata = sdev->pdata;
	const struct sof_dev_desc *desc = pdata->desc;
	struct snd_soc_acpi_mach_params *mach_params;

	mach_params = &mach->mach_params;
	mach_params->platform = dev_name(sdev->dev);
	mach_params->num_dai_drivers = desc->ops->num_drv;
	mach_params->dai_drivers = desc->ops->drv;
}
EXPORT_SYMBOL_NS(atom_set_mach_params, "SND_SOC_SOF_INTEL_ATOM_HIFI_EP");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SOF support for Atom platforms");
