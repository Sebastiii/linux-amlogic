// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/ratelimit.h>
#include <stdarg.h>
#include <linux/amlogic/cpu_version.h>
#include <linux/amlogic/media/amdolbyvision/dolby_vision.h>
#include "amdolby_vision.h"
#include "dv5_compat_abi.h"

static unsigned int dv_shim_debug;
module_param(dv_shim_debug, uint, 0664);
MODULE_PARM_DESC(dv_shim_debug, "bit0 reg, bit1 control_path, bit2 output, bit3 cfi/stub");

#define DVSHIM_REG BIT(0)
#define DVSHIM_CP  BIT(1)
#define DVSHIM_OUT BIT(2)
#define DVSHIM_CFI BIT(3)

#define dvshim_dbg(bit, fmt, args...) \
	do { \
		static DEFINE_RATELIMIT_STATE(_rs, HZ, 2); \
		if (unlikely((dv_shim_debug & (bit)) && __ratelimit(&_rs))) \
			pr_info("DVSHIM: " fmt, ## args); \
	} while (0)

static unsigned long dvshim_cfi_hits;
module_param(dvshim_cfi_hits, ulong, 0444);
static unsigned long dvshim_ubsan_hits;
module_param(dvshim_ubsan_hits, ulong, 0444);
static unsigned long dvshim_stkchk_hits;
module_param(dvshim_stkchk_hits, ulong, 0444);
static unsigned long dvshim_cp_calls;
module_param(dvshim_cp_calls, ulong, 0444);
static unsigned long dvshim_mp_calls;
module_param(dvshim_mp_calls, ulong, 0444);

static const struct dv5_funcs *blob_funcs;
static struct dolby_vision_func_s adapted_funcs;
static struct m_dovi_setting_s shim_m_setting;
static struct m_dovi_setting_s shim_invalid;
static void *mp_ctx;
static int mp_dv_type = DV5_TYPE_DOVI;

#define DV5_OUTPUT_CTRL_DATA_SIZE 0x1000
static u8 shim_output_ctrl_data[DV5_OUTPUT_CTRL_DATA_SIZE];

int get_cpu_type_from_media(void)
{
	static bool once;

	if (!once) {
		once = true;
		dvshim_dbg(DVSHIM_REG, "get_cpu_type_from_media -> 0 (force get_meson_cpu_version fallback)\n");
	}
	return 0;
}
EXPORT_SYMBOL(get_cpu_type_from_media);

void __cfi_slowpath_diag(u64 id, void *ptr, void *diag)
{
	if (!dvshim_cfi_hits)
		dvshim_dbg(DVSHIM_CFI, "cfi_slowpath id=0x%llx ptr=%px\n",
			   (unsigned long long)id, ptr);
	dvshim_cfi_hits++;
}
EXPORT_SYMBOL(__cfi_slowpath_diag);

void __ubsan_handle_cfi_check_fail_abort(void *data, void *value, void *vtype)
{
	if (!dvshim_ubsan_hits)
		dvshim_dbg(DVSHIM_CFI, "ubsan_cfi_check_fail data=%px\n", data);
	dvshim_ubsan_hits++;
}
EXPORT_SYMBOL(__ubsan_handle_cfi_check_fail_abort);

void __stack_chk_fail(void)
{
	if (!dvshim_stkchk_hits) {
		dvshim_dbg(DVSHIM_CFI, "stack_chk_fail (possible ABI/canary mismatch)\n");
		dump_stack();
	}
	dvshim_stkchk_hits++;
}
EXPORT_SYMBOL(__stack_chk_fail);

int _printk(const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);
	r = vprintk(fmt, args);
	va_end(args);
	return r;
}
EXPORT_SYMBOL(_printk);

static void cp_setup_invalid(void)
{
	memset(&shim_invalid, 0, sizeof(shim_invalid));
	shim_invalid.num_input = 0;
	shim_invalid.input[0].src_format = FORMAT_INVALID;
	shim_invalid.input[1].src_format = FORMAT_INVALID;
	shim_invalid.input[DV5_IPCORE2_ID].src_format = FORMAT_INVALID;
}

static void cp_send_reset(void)
{
	if (blob_funcs && blob_funcs->multi_control_path) {
		dvshim_dbg(DVSHIM_CP, "reset (num_input=0)\n");
		blob_funcs->multi_control_path(&shim_invalid);
	}
}

static int cp_adapter(enum signal_format_enum in_format,
		      enum signal_format_enum out_format,
		      char *in_comp, int in_comp_size,
		      char *in_md, int in_md_size,
		      enum priority_mode_enum set_priority,
		      int set_bit_depth, int set_chroma_format, int set_yuv_range,
		      int set_graphic_min_lum, int set_graphic_max_lum,
		      int set_target_min_lum, int set_target_max_lum,
		      int set_no_el,
		      struct hdr10_parameter *hdr10_param,
		      struct dovi_setting_s *output)
{
	struct m_dovi_setting_s *m = &shim_m_setting;
	struct private_info_s *vid = &m->input[0];
	struct private_info_s *gfx = &m->input[DV5_IPCORE2_ID];
	static enum signal_format_enum last_in = FORMAT_INVALID;
	static enum signal_format_enum last_out = FORMAT_INVALID;
	static u32 last_use_ll = 0xffffffff;
	static u32 last_ll_rgb = 0xffffffff;
	static int last_pri = -1;
	static u32 last_w;
	static u32 last_h;
	bool need_reset;
	int flag;
	int ret;

	dvshim_cp_calls++;

	if (!blob_funcs || !blob_funcs->multi_control_path || !output)
		return -1;

	if (in_format == FORMAT_INVALID) {
		last_in = FORMAT_INVALID;
		last_out = FORMAT_INVALID;
		last_use_ll = 0xffffffff;
		last_ll_rgb = 0xffffffff;
		last_pri = -1;
		last_w = 0;
		last_h = 0;
		cp_send_reset();
		amdv_set_l11(NULL, NULL);
		return -1;
	}

	memset(m, 0, sizeof(*m));
	m->num_input = DV5_NUM_INPUTS;
	m->num_video = DV5_NUM_IPCORE1;
	m->pri_input = 0;
	m->enable_multi_core1 = 0;
	m->set_priority = set_priority;
	m->dst_format = out_format;
	m->vout_width = output->vout_width;
	m->vout_height = output->vout_height;
	m->use_ll_flag = output->use_ll_flag;
	m->ll_rgb_desired = output->ll_rgb_desired;
	m->dovi2hdr10_nomapping = output->dovi2hdr10_nomapping;
	memcpy(m->vsvdb_tbl, output->vsvdb_tbl, sizeof(m->vsvdb_tbl));
	m->vsvdb_len = output->vsvdb_len;
	m->vsvdb_changed = output->vsvdb_changed;
	m->mode_changed = output->mode_changed;
	m->set_graphic_min_lum = set_graphic_min_lum;
	m->set_graphic_max_lum = set_graphic_max_lum;
	m->set_target_min_lum = set_target_min_lum;
	m->set_target_max_lum = set_target_max_lum;
	m->output_ctrl_data = shim_output_ctrl_data;
	m->output_ctrl_data_len = DV5_OUTPUT_CTRL_DATA_SIZE;

	vid->valid = 1;
	vid->src_format = in_format;
	vid->el_flag = !set_no_el;
	vid->el_halfsize_flag = output->el_halfsize_flag;
	vid->video_width = output->video_width >> 16;
	vid->video_height = output->video_height >> 16;
	vid->set_bit_depth = DV5_VIDEO_BIT_DEPTH;
	vid->set_chroma_format = set_chroma_format;
	vid->set_yuv_range = set_yuv_range;
	vid->color_format = DV5_CP_YUV;
	vid->in_comp = in_comp;
	vid->in_comp_size = in_comp_size;
	vid->in_md = in_md;
	vid->in_md_size = in_md_size;
	vid->input_mode = DV5_IN_MODE_OTT;
	vid->p_hdr10_param = hdr10_param;

	gfx->valid = 1;
	gfx->input_mode = DV5_IN_MODE_GRAPHICS;
	gfx->src_format = (output->g_format == G_HDR_YUV ||
			   output->g_format == G_HDR_RGB) ? FORMAT_HDR10 : FORMAT_SDR;
	gfx->video_width = DV5_GRAPHIC_W;
	gfx->video_height = DV5_GRAPHIC_H;
	gfx->set_bit_depth = output->g_bitdepth;
	gfx->set_chroma_format = DV5_CP_I444;
	gfx->set_yuv_range = DV5_SIGNAL_RANGE_FULL;
	gfx->color_format = (output->g_format == G_SDR_RGB ||
			     output->g_format == G_HDR_RGB) ? DV5_CP_RGB : DV5_CP_YUV;

	dvshim_dbg(DVSHIM_CP,
		   "cp in=%d out=%d el=%d bd=%d %ux%u md=%d cmp=%d pri=%d vsvdb=%d ll=%d gfmt=%d gbd=%d\n",
		   in_format, out_format, vid->el_flag, set_bit_depth,
		   vid->video_width, vid->video_height,
		   in_md_size, in_comp_size, set_priority,
		   m->vsvdb_len, m->use_ll_flag, output->g_format, output->g_bitdepth);

	need_reset = in_format != last_in ||
		out_format != last_out ||
		m->use_ll_flag != last_use_ll ||
		m->ll_rgb_desired != last_ll_rgb ||
		set_priority != last_pri ||
		vid->video_width != last_w ||
		vid->video_height != last_h;
	if (need_reset)
		cp_send_reset();
	last_in = in_format;
	last_out = out_format;
	last_use_ll = m->use_ll_flag;
	last_ll_rgb = m->ll_rgb_desired;
	last_pri = set_priority;
	last_w = vid->video_width;
	last_h = vid->video_height;

	flag = blob_funcs->multi_control_path(m);
	if (flag)
		dvshim_dbg(DVSHIM_CP, "multi_control_path ret=%d\n", flag);

	memcpy(&output->comp_reg, &m->core1[0].comp_reg, sizeof(output->comp_reg));
	memcpy(&output->dm_reg1, &m->core1[0].dm_reg, sizeof(output->dm_reg1));
	memcpy(&output->dm_lut1, &m->core1[0].dm_lut, sizeof(output->dm_lut1));
	memcpy(&output->dm_reg2, &m->dm_reg2, sizeof(output->dm_reg2));
	memcpy(&output->dm_reg3, &m->dm_reg3, sizeof(output->dm_reg3));
	memcpy(&output->dm_lut2, &m->dm_lut2, sizeof(output->dm_lut2));
	memcpy(&output->md_reg3, &m->md_reg3, sizeof(output->md_reg3));
	memcpy(&output->hdr_info, &m->hdr_info, sizeof(output->hdr_info));
	memcpy(&output->ext_md, &m->ext_md, sizeof(output->ext_md));
	amdv_set_l11(&m->output_vsif, &m->content_info);
	output->src_format = in_format;
	output->dst_format = out_format;
	output->el_flag = vid->el_flag;
	output->diagnostic_enable = (out_format == FORMAT_DOVI) ? m->diagnostic_enable : 0;
	output->diagnostic_mux_select = (out_format == FORMAT_DOVI) ? m->diagnostic_mux_select : 0;
	output->dovi_ll_enable = (out_format == FORMAT_DOVI) ? m->dovi_ll_enable : 0;

	if (unlikely(dv_shim_debug & DVSHIM_OUT)) {
		u32 *d1 = (u32 *)&output->dm_reg1;
		u32 *d3 = (u32 *)&output->dm_reg3;
		char b[260];
		int i, n = 0;

		dvshim_dbg(DVSHIM_OUT,
			   "y2rgb c=%08x %08x %08x %08x %08x o=%08x %08x %08x\n",
			   d1[5], d1[6], d1[7], d1[8], d1[9], d1[10], d1[11], d1[12]);
		for (i = 0; i < 26 && n < 250; i++)
			n += scnprintf(b + n, sizeof(b) - n, "%08x ", d3[i]);
		dvshim_dbg(DVSHIM_OUT, "dm3= %s\n", b);
	}

	if (flag < 0)
		return flag;
	ret = 0;
	if (flag & DV5_CP_FLAG_BLOB_CHANGE_TC)
		ret |= DV5_CP_FLAG_CHANGE_TC;
	if (flag & DV5_CP_FLAG_BLOB_CHANGE_TC2)
		ret |= DV5_CP_FLAG_CHANGE_TC2;
	ret |= flag & DV5_CP_FLAG_CONST_TC2;
	return ret;
}

static void *mp_init_adapter(int flag)
{
	dvshim_dbg(DVSHIM_REG, "mp_init flag=%d -> cached ctx=%px\n", flag, mp_ctx);
	return mp_ctx;
}

static int mp_reset_adapter(int flag)
{
	mp_dv_type = (flag & 0x2) ? DV5_TYPE_ATSC : DV5_TYPE_DOVI;
	if (!blob_funcs || !blob_funcs->multi_mp_reset)
		return -1;
	return blob_funcs->multi_mp_reset(mp_ctx, flag);
}

static int mp_process_adapter(char *src_rpu, int rpu_len,
			      char *dst_comp, int *comp_len,
			      char *dst_md, int *md_len, bool src_eos)
{
	int ret;

	if (!blob_funcs || !blob_funcs->multi_mp_process)
		return -1;
	ret = blob_funcs->multi_mp_process(mp_ctx, src_rpu, rpu_len,
					   dst_comp, comp_len, dst_md, md_len,
					   src_eos, mp_dv_type);
	dvshim_mp_calls++;
	dvshim_dbg(DVSHIM_CP, "mp_process rpu=%d -> comp=%d md=%d ret=%d\n",
		   rpu_len, comp_len ? *comp_len : -1,
		   md_len ? *md_len : -1, ret);
	return ret;
}

static void mp_release_adapter(void)
{
}

int register_dv5shim_func(const struct dv5_funcs *f)
{
	if (!f)
		return -1;

	blob_funcs = f;
	if (!mp_ctx && f->multi_mp_init)
		mp_ctx = f->multi_mp_init(0);
	if (dv_shim_debug & DVSHIM_REG)
		pr_info("DVSHIM: blob reg ver='%s' multi_mp[i=%d r=%d p=%d rel=%d] multi_cp=%d ctx=%px\n",
			f->version_info ? f->version_info : "(null)",
			!!f->multi_mp_init, !!f->multi_mp_reset,
			!!f->multi_mp_process, !!f->multi_mp_release,
			!!f->multi_control_path, mp_ctx);

	memset(&adapted_funcs, 0, sizeof(adapted_funcs));
	adapted_funcs.version_info = f->version_info;
	adapted_funcs.metadata_parser_init = mp_init_adapter;
	adapted_funcs.metadata_parser_reset = mp_reset_adapter;
	adapted_funcs.metadata_parser_process = mp_process_adapter;
	adapted_funcs.metadata_parser_release = mp_release_adapter;
	adapted_funcs.control_path = cp_adapter;

	cp_setup_invalid();
	return register_dv_functions_multi(&adapted_funcs);
}
EXPORT_SYMBOL(register_dv5shim_func);

int unregister_dv5shim_func(void)
{
	int ret = unregister_dv_functions_multi();

	if (mp_ctx && blob_funcs && blob_funcs->multi_mp_release)
		blob_funcs->multi_mp_release(&mp_ctx);
	mp_ctx = NULL;
	blob_funcs = NULL;
	return ret;
}
EXPORT_SYMBOL(unregister_dv5shim_func);

static int __init dv_compat_shim_init(void)
{
	pr_info("DVSHIM: dv5->4.9 compat shim loaded, m_setting=%zu B, debug=0x%x\n",
		sizeof(struct m_dovi_setting_s), dv_shim_debug);
	return 0;
}

static void __exit dv_compat_shim_exit(void)
{
	pr_info("DVSHIM: unloaded cp=%lu cfi=%lu ubsan=%lu stkchk=%lu\n",
		dvshim_cp_calls, dvshim_cfi_hits, dvshim_ubsan_hits,
		dvshim_stkchk_hits);
}

module_init(dv_compat_shim_init);
module_exit(dv_compat_shim_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Dolby Vision 5.15 dovi.ko compatibility shim for the 4.9 kernel");
