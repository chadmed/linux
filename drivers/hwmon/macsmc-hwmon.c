// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SMC hwmon driver for Apple Silicon platforms
 *
 * The System Management Controller on Apple Silicon devices is responsible for
 * measuring data from sensors across the SoC and machine. These include power,
 * temperature, voltage and current sensors. Some "sensors" actually expose
 * derived values. An example of this is the key PHPC, which is an estimate
 * of the heat energy being dissipated by the SoC.
 *
 * While each SoC only has one SMC variant, each platform exposes a different
 * set of sensors. For example, M1 MacBooks expose battery telemetry sensors
 * which are not present on the M1 Mac mini. For this reason, the available
 * sensors for a given platform are described in the device tree in a child
 * node of the SMC device. We must walk this list of available sensors and
 * populate the required hwmon data structures at runtime.
 *
 * Originally based on a prototype by Jean-Francois Bortolotti <jeff@borto.fr>
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define MAX_LABEL_LENGTH 32

enum macsmc_hwmon_key_type {
	macsmc_hwmon_type_sensor,
	macsmc_hwmon_type_fan
};

struct macsmc_hwmon_sensor {
	struct apple_smc_key_info info;
	smc_key macsmc_key;
	char label[MAX_LABEL_LENGTH];
};

struct macsmc_hwmon_fan {
	struct macsmc_hwmon_sensor now;
	struct macsmc_hwmon_sensor min;
	struct macsmc_hwmon_sensor max;
	struct macsmc_hwmon_sensor set;
	char label[MAX_LABEL_LENGTH];
	u32 attrs;
};

struct macsmc_hwmon_sensors {
	struct hwmon_channel_info info;
	struct macsmc_hwmon_sensor *sensors;
	u32 n_sensors;
};

struct macsmc_hwmon_fans {
	struct hwmon_channel_info info;
	struct macsmc_hwmon_fan *fans;
	u32 n_fans;
};

struct macsmc_hwmon {
	struct device *dev;
	struct apple_smc *smc;
	struct device *hwmon_dev;
	struct macsmc_hwmon_sensors *temp;
	struct macsmc_hwmon_sensors *volt;
	struct macsmc_hwmon_sensors *curr;
	struct macsmc_hwmon_sensors *power;
	struct macsmc_hwmon_fans *fan;
};

static int macsmc_hwmon_read_label(struct device *dev,
				enum hwmon_sensor_types type, u32 attr,
				int channel, const char **str)
{
	struct macsmc_hwmon *hwmon = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		if (channel >= hwmon->temp->n_sensors)
			return -EINVAL;
		*str = hwmon->temp->sensors[channel].label;
		break;
	case hwmon_in:
		if (channel >= hwmon->volt->n_sensors)
			return -EINVAL;
		*str = hwmon->volt->sensors[channel].label;
		break;
	case hwmon_curr:
		if (channel >= hwmon->curr->n_sensors)
			return -EINVAL;
		*str = hwmon->curr->sensors[channel].label;
		break;
	case hwmon_power:
		if (channel >= hwmon->power->n_sensors)
			return -EINVAL;
		*str = hwmon->power->sensors[channel].label;
		break;
	case hwmon_fan:
		if (channel >= hwmon->fan->n_fans)
			return -EINVAL;
		*str = hwmon->fan->fans[channel].label;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

/*
 * The SMC has keys of multiple types, denoted by a FourCC of the same format
 * as the key ID. We don't know what data type a key encodes until we poke at it.
 *
 * TODO: support other key types
 */
static int macsmc_hwmon_read_key(struct apple_smc *smc,
				struct macsmc_hwmon_sensor *sensor, int scale,
				long *val)
{
	int ret = 0;

	switch (sensor->info.type_code) {
	/* 32-bit IEEE 754 float */
	case _SMC_KEY("flt "): {
		u32 flt_ = 0;

		ret = apple_smc_read_f32_scaled(smc, sensor->macsmc_key, &flt_,
						scale);
		*val = flt_;
		break;
	}
	/* 48.16 fixed point decimal */
	case _SMC_KEY("ioft"): {
		u64 ioft = 0;

		ret = apple_smc_read_ioft_scaled(smc, sensor->macsmc_key, &ioft,
						scale);
		*val = ioft;
		break;
	}
	default:
		return -EOPNOTSUPP;
	}

	if (ret)
		return -EINVAL;


	return 0;
}

static int macsmc_hwmon_read_fan(struct macsmc_hwmon *hwmon, u32 attr, int chan, long *val)
{
	if (!(hwmon->fan->fans[chan].attrs & BIT(attr)))
		return -EINVAL;

	switch (attr) {
	case hwmon_fan_input:
		return macsmc_hwmon_read_key(hwmon->smc, &hwmon->fan->fans[chan].now,
					     1, val);
	case hwmon_fan_min:
		return macsmc_hwmon_read_key(hwmon->smc, &hwmon->fan->fans[chan].min,
					     1, val);
	case hwmon_fan_max:
		return macsmc_hwmon_read_key(hwmon->smc, &hwmon->fan->fans[chan].max,
					     1, val);
	case hwmon_fan_target:
		return macsmc_hwmon_read_key(hwmon->smc, &hwmon->fan->fans[chan].set,
					     1, val);
	default:
		return -EINVAL;
	}
}

static int macsmc_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct macsmc_hwmon *hwmon = dev_get_drvdata(dev);
	int ret = 0;

	switch (type) {
	case hwmon_temp:
		ret = macsmc_hwmon_read_key(hwmon->smc, &hwmon->temp->sensors[channel],
					    1000, val);
		break;
	case hwmon_in:
		ret = macsmc_hwmon_read_key(hwmon->smc, &hwmon->volt->sensors[channel],
					    1000, val);
		break;
	case hwmon_curr:
		ret = macsmc_hwmon_read_key(hwmon->smc, &hwmon->curr->sensors[channel],
					    1000, val);
		break;
	case hwmon_power:
		/* SMC returns power in Watts with acceptable precision to scale to uW */
		ret = macsmc_hwmon_read_key(hwmon->smc, &hwmon->power->sensors[channel],
					    1000000, val);
		break;
	case hwmon_fan:
		ret = macsmc_hwmon_read_fan(hwmon, attr, channel, val);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return ret;
}

static int macsmc_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long val)
{
	return -EOPNOTSUPP;
}

static umode_t macsmc_hwmon_is_visible(const void *data,
				enum hwmon_sensor_types type, u32 attr,
				int channel)
{
	return 0444;
}

static const struct hwmon_ops macsmc_hwmon_ops = {
	.is_visible = macsmc_hwmon_is_visible,
	.read = macsmc_hwmon_read,
	.read_string = macsmc_hwmon_read_label,
	.write = macsmc_hwmon_write,
};

static struct hwmon_chip_info macsmc_hwmon_info = {
	.ops = &macsmc_hwmon_ops,
	.info = NULL, /* see macsmc_hwmon_create_infos */
};

/*
 * Get the key metadata, including key data type, from the SMC.
 */
static int macsmc_hwmon_parse_key(struct device *dev, struct apple_smc *smc,
			struct macsmc_hwmon_sensor *sensor, const char *key)
{
	int ret = 0;

	ret = apple_smc_get_key_info(smc, _SMC_KEY(key), &sensor->info);
	if (ret) {
		dev_err(dev, "Failed to retrieve key info for %s\n", key);
		return ret;
	}
	sensor->macsmc_key = _SMC_KEY(key);

	return 0;
}

/*
 * A sensor is a single key-value pair as made available by the SMC.
 * The devicetree gives us the SMC key ID and a friendly name where the
 * purpose of the sensor is known.
 */
static int macsmc_hwmon_create_sensor(struct device *dev, struct apple_smc *smc,
				struct device_node *sensor_node,
				struct macsmc_hwmon_sensor *sensor)
{
	const char *key, *label;
	int ret = 0;

	ret = of_property_read_string(sensor_node, "apple,key-id", &key);
	if (ret) {
		dev_err(dev, "Could not find apple,key-id in sensor node");
		return ret;
	}

	ret = macsmc_hwmon_parse_key(dev, smc, sensor, key);
	if (ret)
		return ret;

	if (!of_property_read_string(sensor_node, "apple,key-desc", &label))
		strscpy_pad(sensor->label, label, sizeof(sensor->label));
	else
		strscpy_pad(sensor->label, key, sizeof(sensor->label));

	return 0;
}

/*
 * Fan data is exposed by the SMC as multiple sensors.
 *
 * The devicetree schema reuses apple,key-id for the actual fan speed sensor.
 * Mix, max and target keys do not need labels, so we can reuse apple,key-desc
 * for naming the entire fan.
 */
static int macsmc_hwmon_create_fan(struct device *dev, struct apple_smc *smc,
				struct device_node *fan_node, struct macsmc_hwmon_fan *fan)
{
	const char *label;
	const char *now;
	const char *min;
	const char *max;
	const char *set;
	int ret = 0;

	ret = of_property_read_string(fan_node, "apple,key-id", &now);
	if (ret) {
		dev_err(dev, "apple,key-id not found in fan node!");
		return -EINVAL;
	}

	ret = macsmc_hwmon_parse_key(dev, smc, &fan->now, now);
	if (ret)
		return ret;

	if (!of_property_read_string(fan_node, "apple,key-desc", &label))
		strscpy_pad(fan->label, label, sizeof(fan->label));
	else
		strscpy_pad(fan->label, now, sizeof(fan->label));

	fan->attrs = HWMON_F_LABEL | HWMON_F_INPUT;

	ret = of_property_read_string(fan_node, "apple,fan-minimum", &min);
	if (ret)
		dev_warn(dev, "No minimum fan speed key for %s", fan->label);
	else {
		if (!macsmc_hwmon_parse_key(dev, smc, &fan->min, min))
			fan->attrs |= HWMON_F_MIN;
	}

	ret = of_property_read_string(fan_node, "apple,fan-maximum", &max);
	if (ret)
		dev_warn(dev, "No maximum fan speed key for %s", fan->label);
	else {
		if (!macsmc_hwmon_parse_key(dev, smc, &fan->max, max))
			fan->attrs |= HWMON_F_MAX;
	}

	ret = of_property_read_string(fan_node, "apple,fan-target", &set);
	if (ret)
		dev_warn(dev, "No target fan speed key for %s", fan->label);
	else {
		if (!macsmc_hwmon_parse_key(dev, smc, &fan->set, set))
			fan->attrs |= HWMON_F_TARGET;
	}

	return 0;
}

static int macsmc_hwmon_populate_sensors(struct device *dev, struct apple_smc *smc,
					struct device_node *hwmon_node,
					enum macsmc_hwmon_key_type type,
					struct macsmc_hwmon_sensors *sensors,
					struct macsmc_hwmon_fans *fans,
					const char *group_name)
{
	struct device_node *group_node = NULL;
	struct device_node *key_node = NULL;
	u32 n_keys = 0;
	int i = 0;

	group_node = of_get_child_by_name(hwmon_node, group_name);
	if (!group_node) {
		dev_info(dev, "Key group %s not found\n", group_name);
		return -EOPNOTSUPP;
	}

	n_keys = of_get_child_count(group_node);
	if (!n_keys) {
		of_node_put(group_node);
		dev_err(dev, "No keys found in %s!\n", group_name);
		return -EOPNOTSUPP;
	}

	switch (type) {
	case (macsmc_hwmon_type_sensor):
		if (!sensors)
			return -EINVAL;

		sensors->sensors = devm_kzalloc(dev, sizeof(struct macsmc_hwmon_sensor) * n_keys,
					GFP_KERNEL);
		if (!(sensors->sensors)) {
			of_node_put(group_node);
			return -ENOMEM;
		}

		for_each_child_of_node(group_node, key_node) {
			if (!macsmc_hwmon_create_sensor(dev, smc, key_node,
						&sensors->sensors[i]))
				i += 1;
		}

		sensors->n_sensors = i;
		if (!(sensors->n_sensors)) {
			dev_err(dev, "No valid sensor keys found in %s\n", group_name);
			of_node_put(group_node);
			return -EINVAL;
		}
		break;
	case (macsmc_hwmon_type_fan):
		if (!fans)
			return -EINVAL;

		fans->fans = devm_kzalloc(dev, sizeof(struct macsmc_hwmon_fan) * n_keys,
					GFP_KERNEL);
		if (!(fans->fans)) {
			of_node_put(group_node);
			return -ENOMEM;
		}

		for_each_child_of_node(group_node, key_node) {
			if (!macsmc_hwmon_create_fan(dev, smc, key_node, &fans->fans[i]))
				i += 1;
		}

		fans->n_fans = i;
		if (!(fans->n_fans)) {
			dev_err(dev, "No valid fan keys found in %s\n", group_name);
			of_node_put(group_node);
			return -EINVAL;
		}

		break;
	default:
		return -EOPNOTSUPP;
	}

	of_node_put(group_node);

	return 0;
}

/*
 * Create NULL-terminated config arrays
 */
static void macsmc_hwmon_populate_configs(u32 *configs,
					u32 num_keys, u32 flags)
{
	int idx = 0;

	for (idx = 0; idx < num_keys; idx += 1)
		configs[idx] = flags;

	configs[idx + 1] = 0;
}

static void macsmc_hwmon_populate_fan_configs(u32 *configs,
					u32 num_keys, struct macsmc_hwmon_fans *fans)
{
	int idx = 0;

	for (idx = 0; idx < num_keys; idx += 1)
		configs[idx] = fans->fans[idx].attrs;

	configs[idx + 1] = 0;
}

static int macsmc_hwmon_create_infos(struct macsmc_hwmon *hwmon,
				struct hwmon_channel_info *chip, u32 *info_sz)
{
	*info_sz += (sizeof(struct hwmon_channel_info *));

	chip->type = hwmon_chip;
	chip->config = devm_kzalloc(hwmon->dev, sizeof(u32) * 2, GFP_KERNEL);
	if (!chip->config)
		return -ENOMEM;
	macsmc_hwmon_populate_configs((u32 *)chip->config, 1, HWMON_C_REGISTER_TZ);

	if (hwmon->temp->n_sensors) {
		hwmon->temp->info.type = hwmon_temp;
		hwmon->temp->info.config = devm_kzalloc(hwmon->dev,
						sizeof(u32) * hwmon->temp->n_sensors + 1,
						GFP_KERNEL);
		if (!hwmon->temp->info.config)
			return -ENOMEM;

		macsmc_hwmon_populate_configs((u32 *)hwmon->temp->info.config,
						hwmon->temp->n_sensors,
						(HWMON_T_INPUT | HWMON_T_LABEL));

		*info_sz += (sizeof(struct hwmon_channel_info *));
	}

	if (hwmon->volt->n_sensors) {
		hwmon->volt->info.type = hwmon_in;
		hwmon->volt->info.config = devm_kzalloc(hwmon->dev,
						sizeof(u32) * hwmon->volt->n_sensors + 1,
						GFP_KERNEL);
		if (!hwmon->volt->info.config)
			return -ENOMEM;

		macsmc_hwmon_populate_configs((u32 *)hwmon->volt->info.config,
						hwmon->volt->n_sensors,
						(HWMON_I_INPUT | HWMON_I_LABEL));

		*info_sz += (sizeof(struct hwmon_channel_info *));
	}

	if (hwmon->curr->n_sensors) {
		hwmon->curr->info.type = hwmon_curr;
		hwmon->curr->info.config = devm_kzalloc(hwmon->dev,
						sizeof(u32) * hwmon->curr->n_sensors + 1,
						GFP_KERNEL);
		if (!hwmon->curr->info.config)
			return -ENOMEM;

		macsmc_hwmon_populate_configs((u32 *)hwmon->curr->info.config,
						hwmon->curr->n_sensors,
						(HWMON_C_INPUT | HWMON_C_LABEL));

		*info_sz += (sizeof(struct hwmon_channel_info *));
	}

	if (hwmon->power->n_sensors) {
		hwmon->power->info.type = hwmon_power;
		hwmon->power->info.config = devm_kzalloc(hwmon->dev,
						sizeof(u32) * hwmon->power->n_sensors + 1,
						GFP_KERNEL);
		if (!hwmon->power->info.config)
			return -ENOMEM;

		macsmc_hwmon_populate_configs((u32 *)hwmon->power->info.config,
						hwmon->power->n_sensors,
						(HWMON_P_INPUT | HWMON_P_LABEL));

		*info_sz += (sizeof(struct hwmon_channel_info *));
	}

	if (hwmon->fan->n_fans) {
		hwmon->fan->info.type = hwmon_fan;
		hwmon->fan->info.config = devm_kzalloc(hwmon->dev,
						sizeof(u32) * hwmon->fan->n_fans + 1,
						GFP_KERNEL);
		if (!hwmon->fan->info.config)
			return -ENOMEM;

		macsmc_hwmon_populate_fan_configs((u32 *)hwmon->fan->info.config,
							hwmon->fan->n_fans, hwmon->fan);

		*info_sz += (sizeof(struct hwmon_channel_info *));
	}

	/* NULL termination */
	*info_sz += (sizeof(struct hwmon_channel_info *));

	return 0;
}

static int macsmc_hwmon_populate_info_list(struct macsmc_hwmon *hwmon,
					struct hwmon_channel_info *chip,
					struct hwmon_channel_info **info)
{
	int i = 0;

	info[i] = chip;

	if (hwmon->temp->n_sensors) {
		i += 1;
		info[i] = &hwmon->temp->info;
	}

	if (hwmon->volt->n_sensors) {
		i += 1;
		info[i] = &hwmon->volt->info;
	}

	if (hwmon->curr->n_sensors) {
		i += 1;
		info[i] = &hwmon->curr->info;
	}

	if (hwmon->power->n_sensors) {
		i += 1;
		info[i] = &hwmon->power->info;
	}

	if (hwmon->fan->n_fans) {
		i += 1;
		info[i] = &hwmon->fan->info;
	}

	i += 1;
	info[i] = (struct hwmon_channel_info *)NULL;

	return 0;
}

static int macsmc_hwmon_probe(struct platform_device *pdev)
{
	struct apple_smc *smc = dev_get_drvdata(pdev->dev.parent);
	struct macsmc_hwmon *hwmon;
	struct device_node *hwmon_node;
	struct hwmon_channel_info *chip = NULL;
	struct hwmon_channel_info **macsmc_chip_info = NULL;
	int ret = 0;
	u32 info_sz = 0;

	hwmon = devm_kzalloc(&pdev->dev, sizeof(struct macsmc_hwmon), GFP_KERNEL);
	if (!hwmon)
		return -ENOMEM;

	hwmon->dev = &pdev->dev;
	hwmon->smc = smc;

	hwmon->temp = devm_kzalloc(&pdev->dev, sizeof(struct macsmc_hwmon_sensors),
				GFP_KERNEL);
	if (!hwmon->temp)
		return -ENOMEM;

	hwmon->volt = devm_kzalloc(&pdev->dev, sizeof(struct macsmc_hwmon_sensors),
				GFP_KERNEL);
	if (!hwmon->volt)
		return -ENOMEM;

	hwmon->curr = devm_kzalloc(&pdev->dev, sizeof(struct macsmc_hwmon_sensors),
				GFP_KERNEL);
	if (!hwmon->curr)
		return -ENOMEM;

	hwmon->power = devm_kzalloc(&pdev->dev, sizeof(struct macsmc_hwmon_sensors),
				GFP_KERNEL);
	if (!hwmon->power)
		return -ENOMEM;

	hwmon->fan = devm_kzalloc(&pdev->dev, sizeof(struct macsmc_hwmon_fans),
				GFP_KERNEL);
	if (!hwmon->fan)
		return -ENOMEM;

	hwmon_node = of_find_node_by_name(NULL, "macsmc-hwmon");
	if (!hwmon_node) {
		dev_err(hwmon->dev, "macsmc-hwmon not found in devicetree!\n");
		return -ENODEV;
	}

	ret = macsmc_hwmon_populate_sensors(hwmon->dev, hwmon->smc, hwmon_node,
					macsmc_hwmon_type_sensor, hwmon->temp,
					NULL, "apple,temp-keys");
	if (ret)
		dev_info(hwmon->dev, "Could not populate temp keys!\n");

	ret = macsmc_hwmon_populate_sensors(hwmon->dev, hwmon->smc, hwmon_node,
					macsmc_hwmon_type_sensor, hwmon->volt,
					NULL, "apple,volt-keys");
	if (ret)
		dev_info(hwmon->dev, "Could not populate voltage keys!\n");


	ret = macsmc_hwmon_populate_sensors(hwmon->dev, hwmon->smc, hwmon_node,
					macsmc_hwmon_type_sensor, hwmon->curr,
					NULL, "apple,current-keys");
	if (ret)
		dev_info(hwmon->dev, "Could not populate current keys!\n");

	ret = macsmc_hwmon_populate_sensors(hwmon->dev, hwmon->smc, hwmon_node,
					macsmc_hwmon_type_sensor, hwmon->power,
					NULL, "apple,power-keys");
	if (ret)
		dev_info(hwmon->dev, "Could not populate power keys!\n");

	ret = macsmc_hwmon_populate_sensors(hwmon->dev, hwmon->smc, hwmon_node,
					macsmc_hwmon_type_fan, NULL,
					hwmon->fan, "apple,fan-keys");
	if (ret)
		dev_info(hwmon->dev, "Could not populate fan keys!\n");


	of_node_put(hwmon_node);

	if (!hwmon->temp->n_sensors && !hwmon->volt->n_sensors &&
		!hwmon->curr->n_sensors && !hwmon->power->n_sensors &&
		!hwmon->fan->n_fans) {
		dev_err(hwmon->dev, "No valid keys found of any supported type");
		return -ENODEV;
	}

	chip = devm_kzalloc(hwmon->dev, sizeof(struct hwmon_channel_info), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	ret = macsmc_hwmon_create_infos(hwmon, chip, &info_sz);
	if (ret)
		return ret;

	macsmc_chip_info = devm_kzalloc(hwmon->dev, info_sz, GFP_KERNEL);
	if (!macsmc_chip_info)
		return -ENOMEM;

	ret = macsmc_hwmon_populate_info_list(hwmon, chip, macsmc_chip_info);
	if (ret)
		return ret;

	macsmc_hwmon_info.info = (const struct hwmon_channel_info **)macsmc_chip_info;

	hwmon->hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
						"macsmc_hwmon", hwmon,
						&macsmc_hwmon_info, NULL);
	if (IS_ERR(hwmon->hwmon_dev))
		return dev_err_probe(hwmon->dev, PTR_ERR(hwmon->hwmon_dev),
				     "Probing SMC hwmon device failed!\n");

	dev_info(hwmon->dev, "Registered SMC hwmon device. Sensors:");
	dev_info(hwmon->dev, "Temperature: %d, Voltage: %d, Current: %d, Power: %d, Fans: %d",
		hwmon->temp->n_sensors, hwmon->volt->n_sensors,
		hwmon->curr->n_sensors, hwmon->power->n_sensors, hwmon->fan->n_fans);

	/* Free unused memory */
	if (!hwmon->temp->n_sensors)
		devm_kfree(hwmon->dev, hwmon->temp);

	if (!hwmon->power->n_sensors)
		devm_kfree(hwmon->dev, hwmon->power);

	if (!hwmon->curr->n_sensors)
		devm_kfree(hwmon->dev, hwmon->curr);

	if (!hwmon->volt->n_sensors)
		devm_kfree(hwmon->dev, hwmon->volt);

	if (!hwmon->fan->n_fans)
		devm_kfree(hwmon->dev, hwmon->fan);

	return 0;
}

static struct platform_driver macsmc_hwmon_driver = {
	.probe = macsmc_hwmon_probe,
	.driver = {
		.name = "macsmc_hwmon",
	},
};
module_platform_driver(macsmc_hwmon_driver);

MODULE_DESCRIPTION("Apple Silicon SMC hwmon driver");
MODULE_AUTHOR("James Calligeros <jcalligeros99@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_ALIAS("platform:macsmc_hwmon");
