/*
 * Apple Silicon Mac SMC sensor config lists
 *
 * Copyright (C) The Asahi Linux Contributors
 */

#ifndef _APPLESMC_ARM_H
#define _APPLESMC_ARM_H

#include <linux/hwmon.h>
#include <linux/mfd/core.h>
#include <linux/mfd/macsmc.h>

struct macsmc_hwmon_sensor_info {
	smc_key key;
	char label[32];
};

static const struct macsmc_hwmon_sensor_info macsmc_t8103_temps[] = {
	{ SMC_KEY(TSCD), "SoC Backside Temp" },
	{ SMC_KEY(TB0T), "Battery Hotspot Temp" },
	{ SMC_KEY(TH0x), "NAND Hotspot Temp" },
	{ SMC_KEY(Th1a), "GPU Temp" },
	{ SMC_KEY(TW0P), "WiFi/BT Module Temp" },
};

static const struct macsmc_hwmon_sensor_info macsmc_t8103_powers[] = {
	{ SMC_KEY(PHPC), "Total CPU Core Power" },
	{ SMC_KEY(PSTR), "Total System Power" },

};

static const struct macsmc_hwmon_sensor_info macsmc_t600x_temps[] = {
	{ SMC_KEY(TSCD), "SoC Backside Temp" },
	{ SMC_KEY(TB0T), "Battery Hotspot Temp" },
	{ SMC_KEY(TH0x), "NAND Hotspot Temp" },
	{ SMC_KEY(Th1a), "GPU Temp" },
	{ SMC_KEY(TW0P), "WiFi/BT Module Temp" },
};

static const struct macsmc_hwmon_sensor_info macsmc_t600x_powers[] = {
	{ SMC_KEY(PHPC), "Total CPU Core Power" },
	{ SMC_KEY(PSTR), "Total System Power" },

};

static const struct hwmon_channel_info *macsmc_hwmon_t8103_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ),

	HWMON_CHANNEL_INFO(temp,
			HWMON_T_INPUT | HWMON_T_LABEL,
			HWMON_T_INPUT | HWMON_T_LABEL,
			HWMON_T_INPUT | HWMON_T_LABEL,
			HWMON_T_INPUT | HWMON_T_LABEL,
			HWMON_T_INPUT | HWMON_T_LABEL),

	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),

	NULL
};

static const struct hwmon_channel_info *macsmc_hwmon_t600x_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ),

	HWMON_CHANNEL_INFO(temp,
			HWMON_T_INPUT | HWMON_T_LABEL,
			HWMON_T_INPUT | HWMON_T_LABEL,
			HWMON_T_INPUT | HWMON_T_LABEL,
			HWMON_T_INPUT | HWMON_T_LABEL,
			HWMON_T_INPUT | HWMON_T_LABEL),

	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),

	NULL
};

#endif
