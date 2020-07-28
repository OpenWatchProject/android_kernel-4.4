/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#define LOG_TAG "WDMA"
#include "ddp_log.h"
#include "ddp_clkmgr.h"
#include <linux/delay.h>
#include "ddp_reg.h"
#include "ddp_matrix_para.h"
#include "ddp_info.h"
#include "ddp_wdma.h"
#include "ddp_wdma_ex.h"
#include "primary_display.h"
#include "ddp_m4u.h"
#include "ddp_mmp.h"
#include "ddp_dump.h"


#define ALIGN_TO(x, n)  \
	(((x) + ((n) - 1)) & ~((n) - 1))

/*****************************************************************************/
unsigned int wdma_index(enum DISP_MODULE_ENUM module)
{
	int idx = 0;

	switch (module) {
	case DISP_MODULE_WDMA0:
		idx = 0;
		break;
	default:
		DDPERR("[DDP] error: invalid wdma module=%d\n", module);	/* invalid module */
		ASSERT(0);
	}
	return idx;
}

int wdma_stop(enum DISP_MODULE_ENUM module, void *handle)
{
	unsigned int idx = wdma_index(module);

	DISP_REG_SET(handle, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_INTEN, 0x00);
	DISP_REG_SET(handle, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_EN, 0x00);
	DISP_REG_SET(handle, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_INTSTA, 0x00);

	return 0;
}

int wdma_reset(enum DISP_MODULE_ENUM module, void *handle)
{
	unsigned int delay_cnt = 0;
	unsigned int idx = wdma_index(module);

	DISP_REG_SET(handle, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_RST, 0x01);	/* trigger soft reset */
	if (!handle) {
		while ((DISP_REG_GET(idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_FLOW_CTRL_DBG) &
			0x1) == 0) {
			delay_cnt++;
			udelay(10);
			if (delay_cnt > 2000) {
				DDPERR("wdma%d reset timeout!\n", idx);
				break;
			}
		}
	} else {
		/* add comdq polling */
	}
	DISP_REG_SET(handle, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_RST, 0x0);	/* trigger soft reset */

	return 0;
}

unsigned int ddp_wdma_get_cur_addr(void)
{
	return INREG32(DISP_REG_WDMA_DST_ADDR0);
}

/*****************************************************************************/
static char *wdma_get_status(unsigned int status)
{
	switch (status) {
	case 0x1:
		return "idle";
	case 0x2:
		return "clear";
	case 0x4:
		return "prepare";
	case 0x8:
		return "prepare";
	case 0x10:
		return "data_running";
	case 0x20:
		return "eof_wait";
	case 0x40:
		return "soft_reset_wait";
	case 0x80:
		return "eof_done";
	case 0x100:
		return "soft_reset_done";
	case 0x200:
		return "frame_complete";
	}
	return "unknown";

}

int wdma_start(enum DISP_MODULE_ENUM module, void *handle)
{
	unsigned int idx = wdma_index(module);

	DISP_REG_SET(handle, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_INTEN, 0x07);

	DISP_REG_SET_FIELD(handle, WDMA_EN_FLD_ENABLE,
		idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_EN, 0x1);

	return 0;
}

static int wdma_config_yuv420(enum DISP_MODULE_ENUM module,
			      enum UNIFIED_COLOR_FMT fmt,
			      unsigned int dstPitch,
			      unsigned int Height,
			      unsigned long dstAddress, enum DISP_BUFFER_TYPE sec, void *handle)
{
	unsigned int idx = wdma_index(module);
	unsigned int idx_offst = idx * DISP_WDMA_INDEX_OFFSET;
	/* size_t size; */
	unsigned int u_off = 0;
	unsigned int v_off = 0;
	unsigned int u_stride = 0;
	unsigned int y_size = 0;
	unsigned int u_size = 0;
	/* unsigned int v_size = 0; */
	unsigned int stride = dstPitch;
	int has_v = 1;

	if (fmt != UFMT_YV12 && fmt != UFMT_I420 && fmt != UFMT_NV12 && fmt != UFMT_NV21)
		return 0;

	if (fmt == UFMT_YV12) {
		y_size = stride * Height;
		u_stride = ALIGN_TO(stride / 2, 16);
		u_size = u_stride * Height / 2;
		u_off = y_size;
		v_off = y_size + u_size;
	} else if (fmt == UFMT_I420) {
		y_size = stride * Height;
		u_stride = ALIGN_TO(stride / 2, 16);
		u_size = u_stride * Height / 2;
		v_off = y_size;
		u_off = y_size + u_size;
	} else if (fmt == UFMT_NV12 || fmt == UFMT_NV21) {
		y_size = stride * Height;
		u_stride = stride / 2;
		u_size = u_stride * Height / 2;
		u_off = y_size;
		has_v = 0;
	}

	if (sec != DISP_SECURE_BUFFER) {
		DISP_REG_SET(handle, idx_offst + DISP_REG_WDMA_DST_ADDR1, dstAddress + u_off);
		if (has_v)
			DISP_REG_SET(handle, idx_offst + DISP_REG_WDMA_DST_ADDR2,
				     dstAddress + v_off);
	} else {
		int m4u_port;

		m4u_port = idx == 0 ? DISP_M4U_PORT_DISP_WDMA0 : DISP_M4U_PORT_DISP_WDMA0;

		cmdqRecWriteSecure(handle, disp_addr_convert(idx_offst + DISP_REG_WDMA_DST_ADDR1),
				   CMDQ_SAM_H_2_MVA, dstAddress, u_off, u_size, m4u_port);
		if (has_v)
			cmdqRecWriteSecure(handle,
					   disp_addr_convert(idx_offst + DISP_REG_WDMA_DST_ADDR2),
					   CMDQ_SAM_H_2_MVA, dstAddress, v_off, u_size, m4u_port);
	}
	DISP_REG_SET_FIELD(handle, DST_W_IN_BYTE_FLD_DST_W_IN_BYTE,
			   idx_offst + DISP_REG_WDMA_DST_UV_PITCH, u_stride);
	return 0;
}

static int wdma_config(enum DISP_MODULE_ENUM module,
		       unsigned srcWidth,
		       unsigned srcHeight,
		       unsigned clipX,
		       unsigned clipY,
		       unsigned clipWidth,
		       unsigned clipHeight,
		       enum UNIFIED_COLOR_FMT out_format,
		       unsigned long dstAddress,
		       unsigned dstPitch,
		       unsigned int useSpecifiedAlpha,
		       unsigned char alpha, enum DISP_BUFFER_TYPE sec, void *handle)
{
	unsigned int idx = wdma_index(module);
	unsigned int output_swap = ufmt_get_byteswap(out_format);
	unsigned int is_rgb = ufmt_get_rgb(out_format);
	unsigned int out_fmt_reg = ufmt_get_format(out_format);
	int color_matrix = 0x2;	/* 0010 RGB_TO_BT601 */
	unsigned int idx_offst = idx * DISP_WDMA_INDEX_OFFSET;
	size_t size = dstPitch * clipHeight;

	DDPDBG("%s,src(w%d,h%d),clip(x%d,y%d,w%d,h%d),fmt=%s,addr=0x%lx,pitch=%d,s_alfa=%d,alpa=%d,hnd=0x%p,sec%d\n",
	     ddp_get_module_name(module), srcWidth, srcHeight, clipX, clipY, clipWidth, clipHeight,
	     unified_color_fmt_name(out_format), dstAddress, dstPitch, useSpecifiedAlpha, alpha,
	     handle, sec);

	/* should use OVL alpha instead of sw config */
	DISP_REG_SET(handle, idx_offst + DISP_REG_WDMA_SRC_SIZE, srcHeight << 16 | srcWidth);
	DISP_REG_SET(handle, idx_offst + DISP_REG_WDMA_CLIP_COORD, clipY << 16 | clipX);
	DISP_REG_SET(handle, idx_offst + DISP_REG_WDMA_CLIP_SIZE, clipHeight << 16 | clipWidth);
	DISP_REG_SET_FIELD(handle, CFG_FLD_OUT_FORMAT, idx_offst + DISP_REG_WDMA_CFG, out_fmt_reg);

	if (!is_rgb) {
		/* set DNSP for UYVY and YUV_3P format for better quality */
		wdma_config_yuv420(module, out_format, dstPitch, clipHeight, dstAddress, sec,
				   handle);
		/*user internal matrix */
		DISP_REG_SET_FIELD(handle, CFG_FLD_EXT_MTX_EN, idx_offst + DISP_REG_WDMA_CFG, 0);
		DISP_REG_SET_FIELD(handle, CFG_FLD_CT_EN, idx_offst + DISP_REG_WDMA_CFG, 1);
		DISP_REG_SET_FIELD(handle, CFG_FLD_INT_MTX_SEL, idx_offst + DISP_REG_WDMA_CFG,
				   color_matrix);
		DISP_REG_SET_FIELD(handle, CFG_FLD_DNSP_SEL, idx_offst + DISP_REG_WDMA_CFG, 1);
	} else {
		DISP_REG_SET_FIELD(handle, CFG_FLD_EXT_MTX_EN, idx_offst + DISP_REG_WDMA_CFG, 0);
		DISP_REG_SET_FIELD(handle, CFG_FLD_CT_EN, idx_offst + DISP_REG_WDMA_CFG, 0);
		DISP_REG_SET_FIELD(handle, CFG_FLD_DNSP_SEL, idx_offst + DISP_REG_WDMA_CFG, 0);
	}
	DISP_REG_SET_FIELD(handle, CFG_FLD_SWAP, idx_offst + DISP_REG_WDMA_CFG, output_swap);
	if (sec != DISP_SECURE_BUFFER) {
		DISP_REG_SET(handle, idx_offst + DISP_REG_WDMA_DST_ADDR0, dstAddress);
	} else {
		int m4u_port;

		m4u_port = idx == 0 ? DISP_M4U_PORT_DISP_WDMA0 : DISP_M4U_PORT_DISP_WDMA0;

		/* for sec layer, addr variable stores sec handle */
		/* we need to pass this handle and offset to cmdq driver */
		/* cmdq sec driver will help to convert handle to correct address */
		cmdqRecWriteSecure(handle, disp_addr_convert(idx_offst + DISP_REG_WDMA_DST_ADDR0),
				   CMDQ_SAM_H_2_MVA, dstAddress, 0, size, m4u_port);
	}
	DISP_REG_SET(handle, idx_offst + DISP_REG_WDMA_DST_W_IN_BYTE, dstPitch);
	DISP_REG_SET_FIELD(handle, ALPHA_FLD_A_SEL, idx_offst + DISP_REG_WDMA_ALPHA,
			   useSpecifiedAlpha);
	DISP_REG_SET_FIELD(handle, ALPHA_FLD_A_VALUE, idx_offst + DISP_REG_WDMA_ALPHA, alpha);

	return 0;
}

static int wdma_clock_on(enum DISP_MODULE_ENUM module, void *handle)
{
	ddp_clk_prepare_enable(ddp_get_module_clk_id(module));
	return 0;
}

static int wdma_clock_off(enum DISP_MODULE_ENUM module, void *handle)
{
	ddp_clk_disable_unprepare(ddp_get_module_clk_id(module));

	return 0;
}

void wdma_dump_analysis(enum DISP_MODULE_ENUM module)
{
	unsigned int index = wdma_index(module);
	unsigned int idx_offst = index * DISP_WDMA_INDEX_OFFSET;

	DDPDUMP("== DISP WDMA%d ANALYSIS ==\n", index);
	DDPDUMP("wdma%d:en=%d,w=%d,h=%d,clip=(%d,%d,%d,%d),pitch=(W=%d,UV=%d),addr=(0x%x,0x%x,0x%x),fmt=%s\n",
	     index, DISP_REG_GET(DISP_REG_WDMA_EN + idx_offst) & 0x01,
	     DISP_REG_GET(DISP_REG_WDMA_SRC_SIZE + idx_offst) & 0x3fff,
	     (DISP_REG_GET(DISP_REG_WDMA_SRC_SIZE + idx_offst) >> 16) & 0x3fff,
	     DISP_REG_GET(DISP_REG_WDMA_CLIP_COORD + idx_offst) & 0x3fff,
	     (DISP_REG_GET(DISP_REG_WDMA_CLIP_COORD + idx_offst) >> 16) & 0x3fff,
	     DISP_REG_GET(DISP_REG_WDMA_CLIP_SIZE + idx_offst) & 0x3fff,
	     (DISP_REG_GET(DISP_REG_WDMA_CLIP_SIZE + idx_offst) >> 16) & 0x3fff,
	     DISP_REG_GET(DISP_REG_WDMA_DST_W_IN_BYTE + idx_offst),
	     DISP_REG_GET(DISP_REG_WDMA_DST_UV_PITCH + idx_offst),
	     DISP_REG_GET(DISP_REG_WDMA_DST_ADDR0 + idx_offst),
	     DISP_REG_GET(DISP_REG_WDMA_DST_ADDR1 + idx_offst),
	     DISP_REG_GET(DISP_REG_WDMA_DST_ADDR2 + idx_offst),
	     unified_color_fmt_name(display_fmt_reg_to_unified_fmt
				    ((DISP_REG_GET(DISP_REG_WDMA_CFG + idx_offst) >> 4) & 0xf,
				     (DISP_REG_GET(DISP_REG_WDMA_CFG + idx_offst) >> 10) & 0x1, 0))
	    );
	DDPDUMP("wdma%d:status=%s,in_req=%d(prev sent data),in_ack=%d(ask data to prev),exec=%d,in_pix=(L:%d,P:%d)\n",
		index,
		wdma_get_status(DISP_REG_GET_FIELD
				(FLOW_CTRL_DBG_FLD_WDMA_STA_FLOW_CTRL,
				 DISP_REG_WDMA_FLOW_CTRL_DBG + idx_offst)),
		DISP_REG_GET_FIELD(EXEC_DBG_FLD_WDMA_IN_REQ,
				   DISP_REG_WDMA_FLOW_CTRL_DBG + idx_offst),
		DISP_REG_GET_FIELD(EXEC_DBG_FLD_WDMA_IN_ACK,
				   DISP_REG_WDMA_FLOW_CTRL_DBG + idx_offst),
		DISP_REG_GET(DISP_REG_WDMA_EXEC_DBG + idx_offst) & 0x1f,
		(DISP_REG_GET(DISP_REG_WDMA_CT_DBG + idx_offst) >> 16) & 0xffff,
		DISP_REG_GET(DISP_REG_WDMA_CT_DBG + idx_offst) & 0xffff);
}


static int wdma_dump(enum DISP_MODULE_ENUM module, int level)
{
	wdma_dump_analysis(module);
	disp_wdma_dump_reg(module);

	return 0;
}

static int wdma_golden_setting(enum DISP_MODULE_ENUM module,
	struct golden_setting_context *p_golden_setting, void *cmdq)
{
	unsigned int regval;
	unsigned int idx = wdma_index(module);
	unsigned long res;
	unsigned int ultra_low_us = 6;
	unsigned int ultra_high_us = 4;
	unsigned int preultra_low_us = 7;
	unsigned int preultra_high_us = ultra_low_us;
	int fifo_pseudo_size = 116;
	unsigned int frame_rate = 60;
	unsigned int bytes_per_sec = 3;
	unsigned long long temp = 0;

	int fifo_off_drs_enter = 4;
	int fifo_off_drs_leave = 2;
	int fifo_off_dvfs = 4;

	unsigned long long consume_rate = 0;
	int ultra_low;
	int preultra_low;
	int preultra_high;
	int ultra_high;

	int ultra_low_UV;
	int preultra_low_UV;
	int preultra_high_UV;
	int ultra_high_UV;

	if (!p_golden_setting) {
		DDPERR("golden setting is null, %s,%d\n", __FILE__, __LINE__);
		ASSERT(0);
		return 0;
	}

	frame_rate = p_golden_setting->fps;

	fifo_off_drs_enter = 4;
	fifo_off_drs_leave = 1;
	fifo_off_dvfs = 2;
	res = (unsigned long)p_golden_setting->dst_width * p_golden_setting->dst_height;

	/* DISP_REG_WDMA_SMI_CON */
	regval = 0;
	regval |= REG_FLD_VAL(SMI_CON_FLD_THRESHOLD, 7);
	regval |= REG_FLD_VAL(SMI_CON_FLD_SLOW_ENABLE, 0);
	regval |= REG_FLD_VAL(SMI_CON_FLD_SLOW_LEVEL, 0);
	regval |= REG_FLD_VAL(SMI_CON_FLD_SLOW_COUNT, 0);
	regval |= REG_FLD_VAL(SMI_CON_FLD_SMI_Y_REPEAT_NUM, 4);
	regval |= REG_FLD_VAL(SMI_CON_FLD_SMI_U_REPEAT_NUM, 2);
	regval |= REG_FLD_VAL(SMI_CON_FLD_SMI_V_REPEAT_NUM, 2);
	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_SMI_CON, regval);

	/* DISP_REG_WDMA_BUF_CON1 */
	regval = 0;
	if (p_golden_setting->is_dc)
		regval |= REG_FLD_VAL(BUF_CON1_FLD_ULTRA_ENABLE, 0);
	else
		regval |= REG_FLD_VAL(BUF_CON1_FLD_ULTRA_ENABLE, 1);

	regval |= REG_FLD_VAL(BUF_CON1_FLD_PRE_ULTRA_ENABLE, 1);

	if (p_golden_setting->is_dc)
		regval |= REG_FLD_VAL(BUF_CON1_FLD_FRAME_END_ULTRA, 0);
	else
		regval |= REG_FLD_VAL(BUF_CON1_FLD_FRAME_END_ULTRA, 1);

	regval |= REG_FLD_VAL(BUF_CON1_FLD_FIFO_PSEUDO_SIZE, fifo_pseudo_size);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON1, regval);

	/* DISP_REG_WDMA_BUF_CON3 */
	regval = 0;
	regval |= REG_FLD_VAL(BUF_CON3_FLD_ISSUE_REQ_TH_Y, 16);
	regval |= REG_FLD_VAL(BUF_CON3_FLD_ISSUE_REQ_TH_U, 16);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON3, regval);

	/* DISP_REG_WDMA_BUF_CON4 */
	regval = 0;
	regval |= REG_FLD_VAL(BUF_CON4_FLD_ISSUE_REQ_TH_V, 16);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON4, regval);

	consume_rate = (unsigned long long)res * frame_rate;
	do_div(consume_rate, 1000);
	consume_rate *= 1250;
	do_div(consume_rate, 16 * 1000);

	preultra_low = (preultra_low_us) * consume_rate * bytes_per_sec;
	preultra_low_UV = (preultra_low_us) * consume_rate;
	do_div(preultra_low, 1000);
	do_div(preultra_low_UV, 1000);

	preultra_high = (preultra_high_us) * consume_rate * bytes_per_sec;
	preultra_high_UV = (preultra_high_us) * consume_rate;
	do_div(preultra_high, 1000);
	do_div(preultra_high_UV, 1000);

	ultra_high = (ultra_high_us) * consume_rate * bytes_per_sec;
	ultra_high_UV = (ultra_high_us) * consume_rate;
	do_div(ultra_high, 1000);
	do_div(ultra_high_UV, 1000);

	ultra_low = preultra_high;
	ultra_low_UV = preultra_high_UV;

	/* DISP_REG_WDMA_BUF_CON5  Y*/
	regval = 0;
	temp = fifo_pseudo_size - preultra_low;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_LOW, temp);
	temp = fifo_pseudo_size - ultra_low;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_LOW, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON5, regval);

	/* DISP_REG_WDMA_BUF_CON6 Y*/
	regval = 0;
	temp = fifo_pseudo_size - preultra_high;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_HIGH, temp);
	temp = fifo_pseudo_size - ultra_high;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_HIGH, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON6, regval);

	/* DISP_REG_WDMA_BUF_CON7 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_LOW, temp);
	temp = fifo_pseudo_size - ultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_LOW, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON7, regval);

	/* DISP_REG_WDMA_BUF_CON8 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_HIGH, temp);
	temp = fifo_pseudo_size - ultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_HIGH, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON8, regval);

	/* DISP_REG_WDMA_BUF_CON9 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_LOW, temp);
	temp = fifo_pseudo_size - ultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_LOW, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON9, regval);

	/* DISP_REG_WDMA_BUF_CON10 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_HIGH, temp);
	temp = fifo_pseudo_size - ultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_HIGH, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON10, regval);

	/* DISP_REG_WDMA_DRS_CON0 */

	/* TODO: SET DRS_EN */
	ultra_low = (ultra_low_us + fifo_off_drs_enter) * consume_rate * bytes_per_sec;
	ultra_low_UV = (ultra_low_us + fifo_off_drs_enter) * consume_rate;
	do_div(ultra_low, 1000);
	do_div(ultra_low_UV, 1000);

	regval = 0;
	temp = fifo_pseudo_size - ultra_low;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_DRS_FLD_ENTER_DRS_TH_Y, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_DRS_CON0, regval);

	/* DISP_REG_WDMA_DRS_CON1 */
	regval = 0;
	temp = fifo_pseudo_size - ultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_DRS_FLD_ENTER_DRS_TH_U, temp);
	regval |= REG_FLD_VAL(BUF_DRS_FLD_ENTER_DRS_TH_V, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_DRS_CON1, regval);

	ultra_low = (ultra_low_us + fifo_off_drs_leave) * consume_rate * bytes_per_sec;
	ultra_low_UV = (ultra_low_us + fifo_off_drs_leave) * consume_rate;
	do_div(ultra_low, 1000);
	do_div(ultra_low_UV, 1000);

	regval = 0;
	temp = fifo_pseudo_size - ultra_low;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_DRS_FLD_LEAVE_DRS_TH_Y, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_DRS_CON2, regval);

	regval = 0;
	temp = fifo_pseudo_size - ultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_DRS_FLD_LEAVE_DRS_TH_U, temp);
	regval |= REG_FLD_VAL(BUF_DRS_FLD_LEAVE_DRS_TH_V, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_DRS_CON3, regval);

	/*DVFS*/
	preultra_low = (preultra_low_us + fifo_off_dvfs) * consume_rate * bytes_per_sec;
	preultra_low_UV = (preultra_low_us + fifo_off_dvfs) * consume_rate;
	do_div(preultra_low, 1000);
	do_div(preultra_low_UV, 1000);

	preultra_high = (preultra_high_us + fifo_off_dvfs) * consume_rate * bytes_per_sec;
	preultra_high_UV = (preultra_high_us + fifo_off_dvfs) * consume_rate;
	do_div(preultra_high, 1000);
	do_div(preultra_high_UV, 1000);

	ultra_high = (ultra_high_us + fifo_off_dvfs) * consume_rate * bytes_per_sec;
	ultra_high_UV = (ultra_high_us + fifo_off_dvfs) * consume_rate;
	do_div(ultra_high, 1000);
	do_div(ultra_high_UV, 1000);

	ultra_low = preultra_high;
	ultra_low_UV = preultra_high_UV;

	/* DISP_REG_WDMA_BUF_CON11 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_low;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_LOW, temp);
	temp = fifo_pseudo_size - ultra_low;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_LOW, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON11, regval);

	/* DISP_REG_WDMA_BUF_CON12 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_high;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_HIGH, temp);
	temp = fifo_pseudo_size - ultra_high;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_HIGH, temp);


	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON12, regval);

	/* DISP_REG_WDMA_BUF_CON13 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_LOW, temp);
	temp = fifo_pseudo_size - ultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_LOW, temp);


	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON13, regval);

	/* DISP_REG_WDMA_BUF_CON14 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_HIGH, temp);
	temp = fifo_pseudo_size - ultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_HIGH, temp);


	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON14, regval);

	/* DISP_REG_WDMA_BUF_CON15 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_LOW, temp);
	temp = fifo_pseudo_size - ultra_low_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_LOW, temp);


	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON15, regval);

	/* DISP_REG_WDMA_BUF_CON16 */
	regval = 0;
	temp = fifo_pseudo_size - preultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_PRE_ULTRA_HIGH, temp);
	temp = fifo_pseudo_size - ultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON_FLD_ULTRA_HIGH, temp);


	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON16, regval);

	/* DISP_REG_WDMA_BUF_CON17 */
	regval = 0;
	/* TODO: SET DVFS_EN */
	temp = fifo_pseudo_size - ultra_high;
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON17_FLD_DVFS_TH_Y, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON17, regval);

	/* DISP_REG_WDMA_BUF_CON18 */
	regval = 0;
	temp = fifo_pseudo_size - ultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON18_FLD_DVFS_TH_U, temp);
	temp = fifo_pseudo_size - ultra_high_UV;
	temp = DIV_ROUND_UP(temp, 4);
	temp = (temp > 0) ? temp : 1;
	regval |= REG_FLD_VAL(BUF_CON18_FLD_DVFS_TH_V, temp);

	DISP_REG_SET(cmdq, idx * DISP_WDMA_INDEX_OFFSET + DISP_REG_WDMA_BUF_CON18, regval);

	return 0;
}


static int wdma_check_input_param(struct WDMA_CONFIG_STRUCT *config)
{
	if (!is_unified_color_fmt_supported(config->outputFormat)) {
		DDPERR("wdma parameter invalidate outfmt %s:0x%x\n",
		       unified_color_fmt_name(config->outputFormat), config->outputFormat);
		return -1;
	}

	if (config->dstAddress == 0 || config->srcWidth == 0 || config->srcHeight == 0) {
		DDPERR("wdma parameter invalidate, addr=0x%lx, w=%d, h=%d\n",
		       config->dstAddress, config->srcWidth, config->srcHeight);
		return -1;
	}
	return 0;
}

static int wdma_is_sec[2];
static inline int wdma_switch_to_sec(enum DISP_MODULE_ENUM module, void *handle)
{
	unsigned int wdma_idx = wdma_index(module);
	/*int *wdma_is_sec = svp_pgc->module_sec.wdma_sec;*/
	enum CMDQ_ENG_ENUM cmdq_engine;
	enum CMDQ_EVENT_ENUM cmdq_event;

	/*cmdq_engine = module_to_cmdq_engine(module);*/
	cmdq_engine = wdma_idx == 0 ? CMDQ_ENG_DISP_WDMA0 : CMDQ_ENG_DISP_WDMA1;
	cmdq_event  = wdma_idx == 0 ? CMDQ_EVENT_DISP_WDMA0_EOF : CMDQ_EVENT_DISP_WDMA1_EOF;

	cmdqRecSetSecure(handle, 1);
	/* set engine as sec */
	cmdqRecSecureEnablePortSecurity(handle, (1LL << cmdq_engine));
	cmdqRecSecureEnableDAPC(handle, (1LL << cmdq_engine));
	if (wdma_is_sec[wdma_idx] == 0) {
		DDPSVPMSG("[SVP] switch wdma%d to sec\n", wdma_idx);
		mmprofile_log_ex(ddp_mmp_get_events()->svp_module[module],
			MMPROFILE_FLAG_START, 0, 0);
		/*mmprofile_log_ex(ddp_mmp_get_events()->svp_module[module],
		 *	MMPROFILE_FLAG_PULSE, wdma_idx, 1);
		 */
	}
	wdma_is_sec[wdma_idx] = 1;

	return 0;
}

int wdma_switch_to_nonsec(enum DISP_MODULE_ENUM module, void *handle)
{
	unsigned int wdma_idx = wdma_index(module);

	enum CMDQ_ENG_ENUM cmdq_engine;
	enum CMDQ_EVENT_ENUM cmdq_event;

	cmdq_engine = wdma_idx == 0 ? CMDQ_ENG_DISP_WDMA0 : CMDQ_ENG_DISP_WDMA1;
	cmdq_event  = wdma_idx == 0 ? CMDQ_EVENT_DISP_WDMA0_EOF : CMDQ_EVENT_DISP_WDMA1_EOF;

	if (wdma_is_sec[wdma_idx] == 1) {
		/* wdma is in sec stat, we need to switch it to nonsec */
		struct cmdqRecStruct *nonsec_switch_handle;
		int ret;

		ret = cmdqRecCreate(CMDQ_SCENARIO_DISP_PRIMARY_DISABLE_SECURE_PATH,
			&(nonsec_switch_handle));
		if (ret)
			DDPAEE("[SVP]fail to create disable handle %s ret=%d\n",
				__func__, ret);

		cmdqRecReset(nonsec_switch_handle);

		if (wdma_idx == 0) {
			/*Primary Mode*/
			if (primary_display_is_decouple_mode())
				cmdqRecWaitNoClear(nonsec_switch_handle, cmdq_event);
			else
				_cmdq_insert_wait_frame_done_token_mira(nonsec_switch_handle);
		} else {
			/*External Mode*/
			/*ovl1->wdma1*/
			/*_cmdq_insert_wait_frame_done_token_mira(nonsec_switch_handle);*/
			cmdqRecWaitNoClear(nonsec_switch_handle, CMDQ_SYNC_DISP_EXT_STREAM_EOF);
		}

		/*_cmdq_insert_wait_frame_done_token_mira(nonsec_switch_handle);*/
		cmdqRecSetSecure(nonsec_switch_handle, 1);

		/*in fact, dapc/port_sec will be disabled by cmdq */
		cmdqRecSecureEnablePortSecurity(nonsec_switch_handle, (1LL << cmdq_engine));
		cmdqRecSecureEnableDAPC(nonsec_switch_handle, (1LL << cmdq_engine));
		if (handle != NULL) {
			/*Async Flush method*/
			enum CMDQ_EVENT_ENUM cmdq_event_nonsec_end;
			/*cmdq_event_nonsec_end  = module_to_cmdq_event_nonsec_end(module);*/
			cmdq_event_nonsec_end  = wdma_idx == 0 ? CMDQ_SYNC_DISP_WDMA0_2NONSEC_END
						: CMDQ_SYNC_DISP_WDMA1_2NONSEC_END;
			cmdqRecSetEventToken(nonsec_switch_handle, cmdq_event_nonsec_end);
			cmdqRecFlushAsync(nonsec_switch_handle);
			cmdqRecWait(handle, cmdq_event_nonsec_end);
		} else {
			/*Sync Flush method*/
			cmdqRecFlush(nonsec_switch_handle);
		}
		cmdqRecDestroy(nonsec_switch_handle);
		DDPSVPMSG("[SVP] switch wdma%d to nonsec\n", wdma_idx);
		mmprofile_log_ex(ddp_mmp_get_events()->svp_module[module],
			MMPROFILE_FLAG_END, 0, 0);
		/*mmprofile_log_ex(ddp_mmp_get_events()->svp_module[module],
		 *MMPROFILE_FLAG_PULSE, wdma_idx, 0);
		 */
	}
	wdma_is_sec[wdma_idx] = 0;

	return 0;
}

int setup_wdma_sec(enum DISP_MODULE_ENUM module, struct disp_ddp_path_config *pConfig, void *handle)
{
	int ret;
	int is_engine_sec = 0;

	if (pConfig->wdma_config.security == DISP_SECURE_BUFFER)
		is_engine_sec = 1;

	if (!handle) {
		DDPDBG("[SVP] bypass wdma sec setting sec=%d,handle=NULL\n", is_engine_sec);
		return 0;
	}

	if (is_engine_sec == 1)
		ret = wdma_switch_to_sec(module, handle);
	else
		ret = wdma_switch_to_nonsec(module, NULL);/*hadle = NULL, use the sync flush method*/
	if (ret)
		DDPAEE("[SVP]fail to setup_ovl_sec: %s ret=%d\n",
			__func__, ret);

	return is_engine_sec;
}

static int wdma_config_l(enum DISP_MODULE_ENUM module, struct disp_ddp_path_config *pConfig, void *handle)
{

	struct WDMA_CONFIG_STRUCT *config = &pConfig->wdma_config;

	if (!pConfig->wdma_dirty)
		return 0;

#if 0
	int wdma_idx = wdma_index(module);
	enum CMDQ_ENG_ENUM cmdq_engine;
	enum CMDQ_EVENT_ENUM cmdq_event;
	enum CMDQ_EVENT_ENUM cmdq_event_nonsec_end;

	cmdq_engine = wdma_idx == 0 ? CMDQ_ENG_DISP_WDMA0 : CMDQ_ENG_DISP_WDMA1;
	cmdq_event  = wdma_idx == 0 ? CMDQ_EVENT_DISP_WDMA0_EOF : CMDQ_EVENT_DISP_WDMA1_EOF;
	cmdq_event_nonsec_end  = wdma_idx == 0 ? CMDQ_SYNC_DISP_WDMA0_2NONSEC_END : CMDQ_SYNC_DISP_WDMA1_2NONSEC_END;

	if (config->security == DISP_SECURE_BUFFER) {
		cmdqRecSetSecure(handle, 1);

		/* set engine as sec */
		cmdqRecSecureEnablePortSecurity(handle, (1LL << cmdq_engine));
		cmdqRecSecureEnableDAPC(handle, (1LL << cmdq_engine));
		if (wdma_is_sec[wdma_idx] == 0)
			DDPMSG("[SVP] switch wdma%d to sec\n", wdma_idx);
		wdma_is_sec[wdma_idx] = 1;
	} else {
		if (wdma_is_sec[wdma_idx]) {
			/* wdma is in sec stat, we need to switch it to nonsec */
			struct cmdqRecStruct *nonsec_switch_handle;
			int ret;

			ret = cmdqRecCreate(CMDQ_SCENARIO_DISP_PRIMARY_DISABLE_SECURE_PATH,
					    &(nonsec_switch_handle));
			if (ret)
				DDPAEE("[SVP]fail to create disable handle %s ret=%d\n",
				       __func__, ret);

			cmdqRecReset(nonsec_switch_handle);

			if (wdma_idx == 0) {
				/*Primary Mode*/
				if (primary_display_is_decouple_mode())
					cmdqRecWaitNoClear(nonsec_switch_handle, cmdq_event);
				else
					_cmdq_insert_wait_frame_done_token_mira(nonsec_switch_handle);
			} else {
				/*External Mode*/
				/*ovl1->wdma1*/
				cmdqRecWaitNoClear(nonsec_switch_handle, CMDQ_SYNC_DISP_EXT_STREAM_EOF);
			}

			/*_cmdq_insert_wait_frame_done_token_mira(nonsec_switch_handle);*/
			cmdqRecSetSecure(nonsec_switch_handle, 1);

			/*in fact, dapc/port_sec will be disabled by cmdq */
			cmdqRecSecureEnablePortSecurity(nonsec_switch_handle, (1LL << cmdq_engine));
			cmdqRecSecureEnableDAPC(nonsec_switch_handle, (1LL << cmdq_engine));
			cmdqRecSetEventToken(nonsec_switch_handle, cmdq_event_nonsec_end);
			cmdqRecFlushAsync(nonsec_switch_handle);
			cmdqRecDestroy(nonsec_switch_handle);
			cmdqRecWait(handle, cmdq_event_nonsec_end);
			DDPMSG("[SVP] switch wdma%d to nonsec\n", wdma_idx);
		}
		wdma_is_sec[wdma_idx] = 0;
	}
#else
	setup_wdma_sec(module, pConfig, handle);
#endif
	if (wdma_check_input_param(config) == 0) {
		struct golden_setting_context *p_golden_setting;

		wdma_config(module,
			    config->srcWidth,
			    config->srcHeight,
			    config->clipX,
			    config->clipY,
			    config->clipWidth,
			    config->clipHeight,
			    config->outputFormat,
			    config->dstAddress,
			    config->dstPitch,
			    config->useSpecifiedAlpha, config->alpha, config->security, handle);

		p_golden_setting = pConfig->p_golden_setting_context;
		wdma_golden_setting(module, p_golden_setting, handle);
	}
	return 0;
}

struct DDP_MODULE_DRIVER ddp_driver_wdma = {
	.module = DISP_MODULE_WDMA0,
	.init = NULL,
	.deinit = NULL,
	.config = wdma_config_l,
	.start = wdma_start,
	.trigger = NULL,
	.stop = wdma_stop,
	.reset = wdma_reset,
	.power_on = wdma_clock_on,
	.power_off = wdma_clock_off,
	.is_idle = NULL,
	.is_busy = NULL,
	.dump_info = wdma_dump,
	.bypass = NULL,
	.build_cmdq = NULL,
	.set_lcm_utils = NULL,
	.switch_to_nonsec = wdma_switch_to_nonsec,
};
