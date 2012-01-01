/*
 * linux/drivers/media/video/s5p-mfc/s5p_mfc_ctrl.c
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/jiffies.h>

#include <linux/firmware.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/cma.h>

#include "regs-mfc.h"

#include "s5p_mfc_common.h"
#include "s5p_mfc_mem.h"
#include "s5p_mfc_intr.h"
#include "s5p_mfc_debug.h"
#include "s5p_mfc_reg.h"
#include "s5p_mfc_cmd.h"
#include "s5p_mfc_pm.h"

static void *s5p_mfc_bitproc_buf;
static size_t s5p_mfc_bitproc_phys;
static unsigned char *s5p_mfc_bitproc_virt;
//static dma_addr_t s5p_mfc_bitproc_dma;

/* Allocate firmware */
int s5p_mfc_alloc_firmware(struct s5p_mfc_dev *dev)
{
#if defined(CONFIG_S5P_MFC_VB2_CMA)
	int err;
	struct cma_info mem_info_f, mem_info_a, mem_info_b;
#elif defined(CONFIG_S5P_MFC_VB2_DMA_POOL)
	void *b_base;
	size_t b_base_phys;
#endif

	mfc_debug_enter();

	if (s5p_mfc_bitproc_buf) {
		mfc_err("Attempting to allocate firmware when it seems that it is already loaded.\n");
		return -ENOMEM;
	}

	/* Get memory region information and check if it is correct */
#if defined(CONFIG_S5P_MFC_VB2_CMA)
	err = cma_info(&mem_info_f, dev->v4l2_dev.dev, MFC_CMA_FW);
	mfc_debug(3, "Area \"%s\" is from %08x to %08x and has size %08x", "f",
				mem_info_f.lower_bound, mem_info_f.upper_bound,
							mem_info_f.total_size);
	if (err) {
		mfc_err("Couldn't get memory information from CMA.\n");
		return -EINVAL;
	}
	err = cma_info(&mem_info_a, dev->v4l2_dev.dev, MFC_CMA_BANK1);
	mfc_debug(3, "Area \"%s\" is from %08x to %08x and has size %08x", "a",
			mem_info_a.lower_bound, mem_info_a.upper_bound,
						mem_info_a.total_size);
	if (err) {
		mfc_err("Couldn't get memory information from CMA.\n");
		return -EINVAL;
	}
	err = cma_info(&mem_info_b, dev->v4l2_dev.dev, MFC_CMA_BANK2);
	mfc_debug(3, "Area \"%s\" is from %08x to %08x and has size %08x", "b",
		mem_info_b.lower_bound, mem_info_b.upper_bound,
					mem_info_b.total_size);
	if (err) {
		mfc_err("Couldn't get memory information from CMA.\n");
		return -EINVAL;
	}
	if (mem_info_f.upper_bound > mem_info_a.lower_bound) {
			mfc_err("Firmware has to be "
			"allocated before  memory for buffers (bank A).\n");
		return -EINVAL;
	}
	mfc_debug(2, "Allocating memory for firmware.\n");
	s5p_mfc_bitproc_buf = s5p_mfc_mem_alloc(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], FIRMWARE_CODE_SIZE);
	if (IS_ERR(s5p_mfc_bitproc_buf)) {
		s5p_mfc_bitproc_buf = 0;
		printk(KERN_ERR "Allocating bitprocessor buffer failed\n");
		return -ENOMEM;
	}
	s5p_mfc_bitproc_phys = s5p_mfc_mem_cookie(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], s5p_mfc_bitproc_buf);
	if (s5p_mfc_bitproc_phys & (128 << 10)) {
		mfc_err("The base memory is not aligned to 128KB.\n");
		s5p_mfc_mem_put(dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX],
							s5p_mfc_bitproc_buf);
		s5p_mfc_bitproc_phys = 0;
		s5p_mfc_bitproc_buf = 0;
		return -EIO;
	}
	dev->port_a = s5p_mfc_bitproc_phys;
	dev->port_b = mem_info_b.lower_bound;
	mfc_debug(2, "Port A: %08x Port B: %08x (FW: %08x size: %08x)\n",
				dev->port_a, dev->port_b, s5p_mfc_bitproc_phys,
							FIRMWARE_CODE_SIZE);

	s5p_mfc_bitproc_virt = s5p_mfc_mem_vaddr(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], s5p_mfc_bitproc_buf);
	mfc_debug(2, "Virtual address for FW: %08lx\n",
				(long unsigned int)s5p_mfc_bitproc_virt);
	if (!s5p_mfc_bitproc_virt) {
		mfc_err("Bitprocessor memory remap failed\n");
		s5p_mfc_mem_put(dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX],
							s5p_mfc_bitproc_buf);
		s5p_mfc_bitproc_phys = 0;
		s5p_mfc_bitproc_buf = 0;
		return -EIO;
	}
#elif defined(CONFIG_S5P_MFC_VB2_DMA_POOL)
	mfc_debug(2, "Allocating memory for firmware.\n");
	s5p_mfc_bitproc_buf = s5p_mfc_mem_alloc(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], FIRMWARE_CODE_SIZE);
	if (IS_ERR(s5p_mfc_bitproc_buf)) {
		s5p_mfc_bitproc_buf = 0;
		printk(KERN_ERR "Allocating bitprocessor buffer failed\n");
		return -ENOMEM;
	}

	s5p_mfc_bitproc_phys = s5p_mfc_mem_cookie(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], s5p_mfc_bitproc_buf);
	if (s5p_mfc_bitproc_phys & (128 << 10)) {
		mfc_err("The base memory is not aligned to 128KB.\n");
		s5p_mfc_mem_put(dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX],
							s5p_mfc_bitproc_buf);
		s5p_mfc_bitproc_phys = 0;
		s5p_mfc_bitproc_buf = 0;
		return -EIO;
	}

	s5p_mfc_bitproc_virt = s5p_mfc_mem_vaddr(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], s5p_mfc_bitproc_buf);
	mfc_debug(2, "Virtual address for FW: %08lx\n",
				(long unsigned int)s5p_mfc_bitproc_virt);
	if (!s5p_mfc_bitproc_virt) {
		mfc_err("Bitprocessor memory remap failed\n");
		s5p_mfc_mem_put(dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX],
							s5p_mfc_bitproc_buf);
		s5p_mfc_bitproc_phys = 0;
		s5p_mfc_bitproc_buf = 0;
		return -EIO;
	}

	dev->port_a = s5p_mfc_bitproc_phys;

	b_base = s5p_mfc_mem_alloc(
		dev->alloc_ctx[MFC_CMA_BANK2_ALLOC_CTX], 4096);
	if (IS_ERR(b_base)) {
		printk(KERN_ERR "Allocating Port B base failed\n");
		return -ENOMEM;
	}

	b_base_phys = s5p_mfc_mem_cookie(
		dev->alloc_ctx[MFC_CMA_BANK2_ALLOC_CTX], b_base);

	s5p_mfc_mem_put(dev->alloc_ctx[MFC_CMA_BANK2_ALLOC_CTX],
				b_base);

	dev->port_b = b_base_phys;

	mfc_debug(2, "Port A: %08x Port B: %08x (FW: %08x size: %08x)\n",
			dev->port_a, dev->port_b,
			s5p_mfc_bitproc_phys,
			FIRMWARE_CODE_SIZE);
#elif defined(CONFIG_S5P_MFC_VB2_SDVMM)
	mfc_debug(2, "Allocating memory for firmware.\n");
	s5p_mfc_bitproc_buf = s5p_mfc_mem_alloc(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], FIRMWARE_CODE_SIZE);
	if (IS_ERR(s5p_mfc_bitproc_buf)) {
		s5p_mfc_bitproc_buf = 0;
		printk(KERN_ERR "Allocating bitprocessor buffer failed\n");
		return -ENOMEM;
	}

	s5p_mfc_bitproc_phys = s5p_mfc_mem_cookie(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], s5p_mfc_bitproc_buf);
	if (s5p_mfc_bitproc_phys & (128 << 10)) {
		mfc_err("The base memory is not aligned to 128KB.\n");
		s5p_mfc_mem_put(dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX],
							s5p_mfc_bitproc_buf);
		s5p_mfc_bitproc_phys = 0;
		s5p_mfc_bitproc_buf = 0;
		return -EIO;
	}

	s5p_mfc_bitproc_virt = s5p_mfc_mem_vaddr(
		dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX], s5p_mfc_bitproc_buf);
	mfc_debug(2, "Virtual address for FW: %08lx\n",
				(long unsigned int)s5p_mfc_bitproc_virt);
	if (!s5p_mfc_bitproc_virt) {
		mfc_err("Bitprocessor memory remap failed\n");
		s5p_mfc_mem_put(dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX],
							s5p_mfc_bitproc_buf);
		s5p_mfc_bitproc_phys = 0;
		s5p_mfc_bitproc_buf = 0;
		return -EIO;
	}

	dev->port_a = s5p_mfc_bitproc_phys;
	dev->port_b = s5p_mfc_bitproc_phys;

	mfc_debug(2, "Port A: %08x Port B: %08x (FW: %08x size: %08x)\n",
			dev->port_a, dev->port_b,
			s5p_mfc_bitproc_phys,
			FIRMWARE_CODE_SIZE);
#endif
	mfc_debug_leave();

	return 0;
}

/* Load firmware to MFC */
int s5p_mfc_load_firmware(struct s5p_mfc_dev *dev)
{
	struct firmware *fw_blob;
	int err;

	/* Firmare has to be present as a separate file or compiled
	 * into kernel. */
	mfc_debug_enter();
	mfc_debug(2, "Requesting fw\n");
	err = request_firmware((const struct firmware **)&fw_blob,
				     "s5pc110-mfc.fw", dev->v4l2_dev.dev);

	if (err != 0) {
		mfc_err("Firmware is not present in the /lib/firmware directory nor compiled in kernel.\n");
		return -EINVAL;
	}

	mfc_debug(2, "Ret of request_firmware: %d Size: %d\n", err, fw_blob->size);

	if (fw_blob->size > FIRMWARE_CODE_SIZE) {
		mfc_err("MFC firmware is too big to be loaded.\n");
		release_firmware(fw_blob);
		return -ENOMEM;
	}
	if (s5p_mfc_bitproc_buf == 0 || s5p_mfc_bitproc_phys == 0) {
		mfc_err("MFC firmware is not allocated or was not mapped correctly.\n");
		release_firmware(fw_blob);
		return -EINVAL;
	}
	memcpy(s5p_mfc_bitproc_virt, fw_blob->data, fw_blob->size);
	/*
	s5p_mfc_bitproc_dma = dma_map_single(dev->v4l2_dev.dev,
					     s5p_mfc_bitproc_virt,
					     FIRMWARE_CODE_SIZE,
					     DMA_TO_DEVICE);
	*/
	s5p_mfc_cache_clean(s5p_mfc_bitproc_virt, FIRMWARE_CODE_SIZE);
	release_firmware(fw_blob);
	mfc_debug_leave();
	return 0;
}

/* Release firmware memory */
int s5p_mfc_release_firmware(struct s5p_mfc_dev *dev)
{
	/* Before calling this function one has to make sure
	 * that MFC is no longer processing */
	if (!s5p_mfc_bitproc_buf)
		return -EINVAL;
	/*
	if (s5p_mfc_bitproc_dma)
		dma_unmap_single(dev->v4l2_dev.dev, s5p_mfc_bitproc_dma,
				 FIRMWARE_CODE_SIZE, DMA_TO_DEVICE);
	*/
	s5p_mfc_mem_put(dev->alloc_ctx[MFC_CMA_FW_ALLOC_CTX],
			s5p_mfc_bitproc_buf);

	s5p_mfc_bitproc_virt =  0;
	s5p_mfc_bitproc_phys = 0;
	s5p_mfc_bitproc_buf = 0;
	/*
	s5p_mfc_bitproc_dma = 0;
	*/
	return 0;
}

#if 0
int mfc_load_firmware(const unsigned char *data, size_t size)
{
	volatile unsigned char *fw;

	/* MFC F/W area already 128KB aligned */
	fw = mfc_mem_addr(0);

	memcpy((void *)fw, data, size);

	mfc_mem_cache_clean((void *)fw, size);

	return 1;
}

int mfc_start(struct mfc_dev *dev)
{
	int ret;

	/* FIXME: when MFC start, load firmware again */
	/*
	dev->fw.state = mfc_load_firmware(dev->fw.info->data, dev->fw.info->size);
	*/

	mfc_clock_on();

	if (mfc_reset() == false) {
		mfc_clock_off();
		return MFC_FAIL;
	}

	mfc_init_memctrl();
	mfc_clear_cmds();

	ret = mfc_cmd_fw_start(dev);
	if (ret < 0) {
		mfc_clock_off();
		return ret;
	}

	ret = mfc_cmd_sys_init(dev);

	mfc_clock_off();

	return ret;
}
#endif

/* Reset the device */
static int s5p_mfc_reset(void)
{
	unsigned int mc_status;
	unsigned long timeout;

	mfc_debug_enter();

	/* Stop procedure */
	/* FIXME: F/W can be access invalid address */
	/* Reset VI */
	/*
	s5p_mfc_write_reg(0x3f7, S5P_FIMV_SW_RESET);
	*/
	s5p_mfc_write_reg(0x3f6, S5P_FIMV_SW_RESET);	/*  reset RISC */
	s5p_mfc_write_reg(0x3e2, S5P_FIMV_SW_RESET);	/*  All reset except for MC */
	mdelay(10);

	timeout = jiffies + msecs_to_jiffies(MFC_BW_TIMEOUT);

	/* Check MC status */
	do {
		if (time_after(jiffies, timeout)) {
			mfc_err("Timeout while resetting MFC.\n");
			return -EIO;
		}

		mc_status = s5p_mfc_read_reg(S5P_FIMV_MC_STATUS);

	} while (mc_status & 0x3);

	s5p_mfc_write_reg(0x0, S5P_FIMV_SW_RESET);
	s5p_mfc_write_reg(0x3fe, S5P_FIMV_SW_RESET);

	mfc_debug_leave();

	return 0;
}

static inline void s5p_mfc_init_memctrl(struct s5p_mfc_dev *dev)
{
	/* channelA, port0 */
	s5p_mfc_write_reg(dev->port_a, S5P_FIMV_MC_DRAMBASE_ADR_A);
	/* channelB, port1 */
	s5p_mfc_write_reg(dev->port_b, S5P_FIMV_MC_DRAMBASE_ADR_B);

	mfc_debug(2, "Port A: %08x, Port B: %08x\n", dev->port_a, dev->port_b);
}

static inline void s5p_mfc_clear_cmds(void)
{
	s5p_mfc_write_reg(0xffffffff, S5P_FIMV_SI_CH0_INST_ID);
	s5p_mfc_write_reg(0xffffffff, S5P_FIMV_SI_CH1_INST_ID);

	s5p_mfc_write_reg(0, S5P_FIMV_RISC2HOST_CMD);
	s5p_mfc_write_reg(0, S5P_FIMV_HOST2RISC_CMD);
}

/* Initialize hardware */
int s5p_mfc_init_hw(struct s5p_mfc_dev *dev)
{
	int ret;

	mfc_debug_enter();

	/* RMVME: */
	if (!s5p_mfc_bitproc_buf)
		return -EINVAL;

	/* 0. MFC reset */
	mfc_debug(2, "MFC reset...\n");

	s5p_mfc_clock_on();

	ret = s5p_mfc_reset();
	if (ret) {
		mfc_err("Failed to reset MFC - timeout.\n");
		return ret;
	}
	mfc_debug(2, "Done MFC reset...\n");

	/* 1. Set DRAM base Addr */
	s5p_mfc_init_memctrl(dev);

	/* 2. Initialize registers of channel I/F */
	s5p_mfc_clear_cmds();

	/* 3. Release reset signal to the RISC */
	s5p_mfc_write_reg(0x3ff, S5P_FIMV_SW_RESET);
	mfc_debug(2, "Will now wait for completion of firmware transfer.\n");
	if (s5p_mfc_wait_for_done_dev(dev, S5P_FIMV_R2H_CMD_FW_STATUS_RET)) {
		mfc_err("Failed to load firmware.\n");
		s5p_mfc_clean_dev_int_flags(dev);
		return -EIO;
	}

	s5p_mfc_clean_dev_int_flags(dev);
	/* 4. Initialize firmware */
	ret = s5p_mfc_sys_init_cmd(dev);
	if (ret) {
		mfc_err("Failed to send command to MFC - timeout.\n");
		return ret;
	}
	mfc_debug(2, "Ok, now will write a command to init the system\n");
	if (s5p_mfc_wait_for_done_dev(dev, S5P_FIMV_R2H_CMD_SYS_INIT_RET)) {
		mfc_err("Failed to load firmware\n");
		return -EIO;
	}

	dev->int_cond = 0;
	if (dev->int_err != 0 || dev->int_type !=
						S5P_FIMV_R2H_CMD_SYS_INIT_RET) {
		/* Failure. */
		mfc_err("Failed to init firmware - error: %d"
				" int: %d.\n",dev->int_err, dev->int_type);
		return -EIO;
	}

	mfc_info("MFC F/W version : %02xyy, %02xmm, %02xdd\n",
		 MFC_GET_REG(SYS_FW_VER_YEAR),
		 MFC_GET_REG(SYS_FW_VER_MONTH),
		 MFC_GET_REG(SYS_FW_VER_DATE));

	s5p_mfc_clock_off();

	mfc_debug_leave();

	return 0;
}


/* Deinitialize hardware */
void s5p_mfc_deinit_hw(struct s5p_mfc_dev *dev)
{
	s5p_mfc_clock_on();
	s5p_mfc_reset();
	s5p_mfc_clock_off();
}

int s5p_mfc_sleep(struct s5p_mfc_dev *dev)
{
	int ret;

	mfc_debug_enter();

	s5p_mfc_clock_on();

	s5p_mfc_clean_dev_int_flags(dev);
	ret = s5p_mfc_sleep_cmd(dev);
	if (ret) {
		mfc_err("Failed to send command to MFC - timeout.\n");
		return ret;
	}
	if (s5p_mfc_wait_for_done_dev(dev, S5P_FIMV_R2H_CMD_SLEEP_RET)) {
		mfc_err("Failed to sleep\n");
		return -EIO;
	}

	s5p_mfc_clock_off();

	dev->int_cond = 0;
	if (dev->int_err != 0 || dev->int_type !=
						S5P_FIMV_R2H_CMD_SLEEP_RET) {
		/* Failure. */
		mfc_err("Failed to sleep - error: %d"
				" int: %d.\n",dev->int_err, dev->int_type);
		return -EIO;
	}

	mfc_debug_leave();

	return ret;
}

int s5p_mfc_wakeup(struct s5p_mfc_dev *dev)
{
	int ret;

	mfc_debug_enter();

	/* 0. MFC reset */
	mfc_debug(2, "MFC reset...\n");

	s5p_mfc_clock_on();

	ret = s5p_mfc_reset();
	if (ret) {
		mfc_err("Failed to reset MFC - timeout.\n");
		return ret;
	}
	mfc_debug(2, "Done MFC reset...\n");

	/* 1. Set DRAM base Addr */
	s5p_mfc_init_memctrl(dev);

	/* 2. Initialize registers of channel I/F */
	s5p_mfc_clear_cmds();

	s5p_mfc_clean_dev_int_flags(dev);
	/* 3. Initialize firmware */
	ret = s5p_mfc_wakeup_cmd(dev);
	if (ret) {
		mfc_err("Failed to send command to MFC - timeout.\n");
		return ret;
	}

	/* 4. Release reset signal to the RISC */
	s5p_mfc_write_reg(0x3ff, S5P_FIMV_SW_RESET);

	mfc_debug(2, "Ok, now will write a command to wakeup the system\n");
	if (s5p_mfc_wait_for_done_dev(dev, S5P_FIMV_R2H_CMD_WAKEUP_RET)) {
		mfc_err("Failed to load firmware\n");
		return -EIO;
	}

	s5p_mfc_clock_off();

	dev->int_cond = 0;
	if (dev->int_err != 0 || dev->int_type !=
						S5P_FIMV_R2H_CMD_WAKEUP_RET) {
		/* Failure. */
		mfc_err("Failed to wakeup - error: %d"
				" int: %d.\n",dev->int_err, dev->int_type);
		return -EIO;
	}

	mfc_debug_leave();

	return 0;
}
