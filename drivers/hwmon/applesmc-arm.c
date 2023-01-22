// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * hwmon driver for the Apple SMC as found on Apple Silicon devices
 *
 * TODO: fan keys + writing
 *
 * Based heavily on a prototype by Jean-Francois Bortolotti.
 *
 * Copyright (C) The Asahi Linux Contributors
 */

#include <linux/hwmon-sysfs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>

#include "applesmc-arm.h"

struct macsmc_hwmon_data {
	const struct macsmc_hwmon_sensor_info *temps;
	const struct macsmc_hwmon_sensor_info *powers;
	const struct hwmon_channel_info **info;
};

struct macsmc_hwmon {
	struct device *dev;
	struct apple_smc *smc;
	struct device *hwmon_dev;
	const struct macsmc_hwmon_data *sensors;
};

static const struct macsmc_hwmon_data macsmc_hwmon_t8103_data = {
	macsmc_t8103_temps,
	macsmc_t8103_powers,
	macsmc_hwmon_t8103_info,
};

static const struct macsmc_hwmon_data macsmc_hwmon_t600x_data = {
	macsmc_t600x_temps,
	macsmc_t600x_powers,
	macsmc_hwmon_t600x_info,
};


static u32 macsmc_f32_to_u32(u32 flt)
{
	unsigned int sign, exp, mant;
	unsigned long val;
	int i, b;
	s32 result;

	sign = flt>>31;
	exp = flt>>23;
	mant = flt<<9>>9;

	result = 0;
	val = 0;
	if (exp == 0 && mant != 0) {
		for (i = 22; i >= 0; i -= 1) {
			b = (mant&(1<<i))>>i;
			val += b*(1000000000>>(23-i));
		}
		if (exp > 127)
			result = (val<<(exp-127))/1000000;
		else
			result = (val>>(127-exp))/1000000;
	} else if (!(exp == 0 && mant == 0)) {
		for (i = 22; i >= 0; i -= 1) {
			b = (mant&(1<<i))>>i;
			val += b*(1000000000>>(23-i));
		}
		if (exp > 127)
			result = ((val+1000000000)<<(exp-127))/1000000;
		else
			result = ((val+1000000000)>>(127-exp))/1000000;
	}

	if (sign == 1)
		result *= -1;

	return result;
}


static umode_t macsmc_hwmon_is_visible(const void *data,
									   enum hwmon_sensor_types type,
									   u32 attr, int chan)
{
	umode_t mode = 0444;
	return mode;
}

static int macsmc_hwmon_get_label(struct device *dev,
				  enum hwmon_sensor_types type,
				  u32 attr, int chan, const char **label)
{
	static struct macsmc_hwmon *hwmon;

	hwmon = dev_get_drvdata(dev);

	switch (type) {
		case hwmon_temp:
			*label = hwmon->sensors->temps[chan].label;
			break;
		case hwmon_power:
			*label = hwmon->sensors->powers[chan].label;
			break;
		default:
			return -EOPNOTSUPP;
	}

	return 0;
}

static int macsmc_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int chan, long *val)
{
	smc_key key;
	int ret = 0;
	u32 readback = 0;
	struct macsmc_hwmon *hwmon;
	struct apple_smc *smc;

	hwmon = dev_get_drvdata(dev);
	smc = hwmon->smc;

	switch (type) {
		case hwmon_temp:
			key = hwmon->sensors->temps[chan].key;
			ret = apple_smc_read_u32(smc, key, &readback);
			break;
		case hwmon_power:
			key = hwmon->sensors->powers[chan].key;
			ret = apple_smc_read_u32(smc, key, &readback);
			break;
		default:
			return -EOPNOTSUPP;
	}

	*val = macsmc_f32_to_u32(readback);
	return 0;
}

static int macsmc_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int chan, long val)
{
	return -EOPNOTSUPP;
}


static const struct hwmon_ops macsmc_hwmon_ops = {
	.is_visible = macsmc_hwmon_is_visible,
	.read = macsmc_hwmon_read,
	.write = macsmc_hwmon_write,
	.read_string = macsmc_hwmon_get_label,
};

static struct hwmon_chip_info macsmc_hwmon_chip_info = {
	.ops = &macsmc_hwmon_ops,
	.info = NULL, /* filled at runtime */
};

static struct of_device_id macsmc_hwmon_of_match[] = {
	{ .compatible = "apple,t8103-smc", .data = &macsmc_hwmon_t8103_data },
	{ .compatible = "apple,t6000-smc", .data = &macsmc_hwmon_t600x_data },
	{ .compatible = "apple,smc" },
	{ },
};
MODULE_DEVICE_TABLE(of, macsmc_hwmon_of_match);

static int macsmc_hwmon_probe(struct platform_device *pdev)
{
	struct apple_smc *smc = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct macsmc_hwmon *macsmc_hwmon;
	static const struct of_device_id *of_id;

	dev_info(&pdev->dev, "Probing SMC hwmon");

	of_id = of_match_device(macsmc_hwmon_of_match, dev->parent);
	if (!of_id) {
		dev_err(&pdev->dev, "No suitable SMC found!");
		return -EINVAL;
	}

	macsmc_hwmon = devm_kzalloc(&pdev->dev, sizeof(*macsmc_hwmon), GFP_KERNEL);
	if (!macsmc_hwmon) {
		dev_err(&pdev->dev, "Unable to allocate memory!");
		return -ENOMEM;
	}

	macsmc_hwmon->smc = smc;
	macsmc_hwmon->sensors = of_id->data;
	macsmc_hwmon_chip_info.info = macsmc_hwmon->sensors->info;

	macsmc_hwmon->hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
							"macsmc_hwmon", macsmc_hwmon,
							&macsmc_hwmon_chip_info, NULL);
	if (IS_ERR(macsmc_hwmon->hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(macsmc_hwmon->hwmon_dev),
				     "Failed to probe SMC hwmon device!\n");

	return 0;
}



static struct platform_driver macsmc_hwmon_driver = {
	.probe = macsmc_hwmon_probe,
	.driver = {
		.name = "macsmc_hwmon",
		.of_match_table = macsmc_hwmon_of_match,
		.owner = THIS_MODULE,
	}
};
module_platform_driver(macsmc_hwmon_driver);

MODULE_DESCRIPTION("Apple SMC (Apple Silicon)");
MODULE_AUTHOR("James Calligeros <jcalligeros99@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
