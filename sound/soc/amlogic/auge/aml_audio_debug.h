/* SPDX-License-Identifier: GPL-2.0 */

#ifndef AML_AUDIO_DEBUG_H
#define AML_AUDIO_DEBUG_H

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>

extern unsigned int aml_audio_debug;

#define AML_AUDIO_DBG_HWPARAMS  0x1
#define AML_AUDIO_DBG_CODEC     0x2
#define AML_AUDIO_DBG_TRIGGER   0x4
#define AML_AUDIO_DBG_PREPARE   0x8
#define AML_AUDIO_DBG_RATE      0x10

#define aml_audio_dbg(flag, fmt, ...) \
	do { \
		static DEFINE_RATELIMIT_STATE(_aml_audio_rs, HZ, 1); \
		if (unlikely(aml_audio_debug & (flag)) && \
		    __ratelimit(&_aml_audio_rs)) \
			pr_info("AMLAUDIO: " fmt "\n", ##__VA_ARGS__); \
	} while (0)

#endif
