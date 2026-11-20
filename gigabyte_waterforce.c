// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Gigabyte AORUS Waterforce AIO CPU coolers: X240, X280 and X360.
 *
 * Copyright 2023 Aleksa Savic <savicaleksa83@gmail.com>
 *
 * Out-of-tree fork that adds fan and pump control on top of the read-only
 * mainline driver. The device has no working fixed-speed command (0xE6 is
 * ignored by this firmware), so control works by selecting the firmware's
 * built-in profiles with 0xE5 and applying them with 0xB6. The profile codes
 * are from D05Col/omarchy-aorus-fan and were confirmed on an X360 (1044:7a4d).
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>

#define DRIVER_NAME	"gigabyte_waterforce"

#define USB_VENDOR_ID_GIGABYTE		0x1044
#define USB_PRODUCT_ID_WATERFORCE	0x7a4d	/* Gigabyte AORUS WATERFORCE X240, X280 and X360 */

#define STATUS_VALIDITY		(2 * 1000)	/* ms */
#define MAX_REPORT_LENGTH	6144

#define WATERFORCE_TEMP_SENSOR	0xD
#define WATERFORCE_FAN_SPEED	0x02
#define WATERFORCE_PUMP_SPEED	0x05
#define WATERFORCE_FAN_DUTY	0x08
#define WATERFORCE_PUMP_DUTY	0x09

/* Control commands, inner offsets and lengths */
static const u8 get_status_cmd[] = { 0x99, 0xDA };

#define FIRMWARE_VER_START_OFFSET_1	2
#define FIRMWARE_VER_START_OFFSET_2	3
static const u8 get_firmware_ver_cmd[] = { 0x99, 0xD6 };

/* Command lengths */
#define GET_STATUS_CMD_LENGTH		2
#define GET_FIRMWARE_VER_CMD_LENGTH	2

/* Firmware profile command (0xE5), applied by the commit command (0xB6) */
#define SET_PROFILE_CMD		0xE5
#define SET_PROFILE_FAN		0x01
#define SET_PROFILE_PUMP	0x02
static const u8 commit_cmd[] = { 0x99, 0xB6 };
#define CMD_GAP_MS		20

/*
 * Firmware profiles. pwm is the duty the device reports while the profile is
 * active at idle coolant temperature (0-255), or 0 if a pwm write must never
 * pick it. A pwm write selects the lowest profile whose pwm is at least the
 * requested value, so the cooler never runs below what was asked for.
 * Profiles are listed in increasing pwm order for that search.
 */
struct waterforce_profile {
	const char *name;
	u8 code;
	u8 pwm;
};

static const struct waterforce_profile fan_profiles[] = {
	{ "zero", 0x07, 0 },		/* fan stop, only by explicit choice */
	{ "quiet", 0x06, 143 },		/* ~56 % */
	{ "balance", 0x00, 184 },	/* ~72 % */
	{ "performance", 0x05, 224 },	/* ~88 % */
	{ "max", 0x04, 255 },		/* 100 % */
	{ "default", 0x02, 0 },		/* 100 % on tested unit, duplicate of max */
};

static const struct waterforce_profile pump_profiles[] = {
	{ "balance", 0x00, 207 },	/* ~81 %, ~2670 RPM */
	{ "max", 0x04, 255 },		/* 100 %, ~3280 RPM */
};

#define FAN_PROFILE_BALANCE	2
#define PUMP_PROFILE_BALANCE	0

static const struct {
	const struct waterforce_profile *list;
	int count;
	u8 selector;
} channel_profiles[2] = {
	{ fan_profiles, ARRAY_SIZE(fan_profiles), SET_PROFILE_FAN },
	{ pump_profiles, ARRAY_SIZE(pump_profiles), SET_PROFILE_PUMP },
};

/* pwmN_enable values */
#define PWM_MODE_MANUAL	1	/* pwmN writes pick the profile */
#define PWM_MODE_AUTO	2	/* the profile chosen in fan_profile / pump_profile */

static const char *const waterforce_temp_label[] = {
	"Coolant temp"
};

static const char *const waterforce_speed_label[] = {
	"Fan speed",
	"Pump speed"
};

struct waterforce_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	/* For locking access to buffer */
	struct mutex buffer_lock;
	/* For queueing multiple readers */
	struct mutex status_report_request_mutex;
	/* For reinitializing the completion below */
	spinlock_t status_report_request_lock;
	struct completion status_report_received;
	struct completion fw_version_processed;

	/* Sensor data */
	s32 temp_input[1];
	u16 speed_input[2];	/* Fan and pump speed in RPM */
	u8 duty_input[2];	/* Fan and pump duty in 0-100% */

	/* Control state, protected by control_lock */
	struct mutex control_lock;
	u8 pwm_mode[2];		/* PWM_MODE_MANUAL or PWM_MODE_AUTO */
	u8 auto_profile[2];	/* Profile for PWM_MODE_AUTO, index into the list */
	u8 active_profile[2];	/* Profile last sent to the device */
	bool profile_sent[2];	/* active_profile is known to be on the device */

	u8 *buffer;
	int firmware_version;
	unsigned long updated;	/* jiffies */
};

static umode_t waterforce_is_visible(const void *data,
				     enum hwmon_sensor_types type, u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
		case hwmon_temp_input:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_label:
		case hwmon_fan_input:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

/* Writes the command to the device with the rest of the report filled with zeroes */
static int waterforce_write_expanded(struct waterforce_data *priv, const u8 *cmd, int cmd_length)
{
	int ret;

	mutex_lock(&priv->buffer_lock);

	memcpy_and_pad(priv->buffer, MAX_REPORT_LENGTH, cmd, cmd_length, 0x00);
	ret = hid_hw_output_report(priv->hdev, priv->buffer, MAX_REPORT_LENGTH);

	mutex_unlock(&priv->buffer_lock);
	return ret;
}

static int waterforce_get_status(struct waterforce_data *priv)
{
	int ret = mutex_lock_interruptible(&priv->status_report_request_mutex);

	if (ret < 0)
		return ret;

	if (!time_after(jiffies, priv->updated + msecs_to_jiffies(STATUS_VALIDITY))) {
		/* Data is up to date */
		goto unlock_and_return;
	}

	/*
	 * Disable raw event parsing for a moment to safely reinitialize the
	 * completion. Reinit is done because hidraw could have triggered
	 * the raw event parsing and marked the priv->status_report_received
	 * completion as done.
	 */
	spin_lock_bh(&priv->status_report_request_lock);
	reinit_completion(&priv->status_report_received);
	spin_unlock_bh(&priv->status_report_request_lock);

	/* Send command for getting status */
	ret = waterforce_write_expanded(priv, get_status_cmd, GET_STATUS_CMD_LENGTH);
	if (ret < 0)
		goto unlock_and_return;

	ret = wait_for_completion_interruptible_timeout(&priv->status_report_received,
							msecs_to_jiffies(STATUS_VALIDITY));
	if (ret == 0)
		ret = -ETIMEDOUT;

unlock_and_return:
	mutex_unlock(&priv->status_report_request_mutex);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * Select a firmware profile on one channel. Skipped if it is already active,
 * since tools such as CoolerControl rewrite pwm often. Caller holds control_lock.
 */
static int waterforce_apply_profile(struct waterforce_data *priv, int channel, int idx)
{
	u8 cmd[4] = { 0x99, SET_PROFILE_CMD, channel_profiles[channel].selector,
		      channel_profiles[channel].list[idx].code };
	int ret;

	if (priv->profile_sent[channel] && priv->active_profile[channel] == idx)
		return 0;

	ret = waterforce_write_expanded(priv, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	msleep(CMD_GAP_MS);

	ret = waterforce_write_expanded(priv, commit_cmd, sizeof(commit_cmd));
	if (ret < 0)
		return ret;

	priv->active_profile[channel] = idx;
	priv->profile_sent[channel] = true;
	/* Invalidate cached status so the next read shows the new profile */
	priv->updated = jiffies - msecs_to_jiffies(STATUS_VALIDITY);
	return 0;
}

/* Lowest pwm-selectable profile that runs at least at the requested duty */
static int waterforce_profile_for_pwm(int channel, long pwm)
{
	const struct waterforce_profile *list = channel_profiles[channel].list;
	int i, best = -1;

	for (i = 0; i < channel_profiles[channel].count; i++) {
		if (!list[i].pwm)
			continue;
		if (best < 0 || list[i].pwm > list[best].pwm)
			best = i;	/* fallback: the highest one */
		if (list[i].pwm >= pwm)
			return i;
	}

	return best;
}

static int waterforce_set_pwm(struct waterforce_data *priv, int channel, long val)
{
	int ret;

	if (val < 0 || val > 255)
		return -EINVAL;

	mutex_lock(&priv->control_lock);
	ret = waterforce_apply_profile(priv, channel, waterforce_profile_for_pwm(channel, val));
	if (!ret)
		priv->pwm_mode[channel] = PWM_MODE_MANUAL;
	mutex_unlock(&priv->control_lock);

	return ret;
}

static int waterforce_set_pwm_mode(struct waterforce_data *priv, int channel, long mode)
{
	int ret = 0;

	if (mode != PWM_MODE_MANUAL && mode != PWM_MODE_AUTO)
		return -EINVAL;

	mutex_lock(&priv->control_lock);
	/* Manual keeps the current profile until the next pwm write */
	if (mode == PWM_MODE_AUTO)
		ret = waterforce_apply_profile(priv, channel, priv->auto_profile[channel]);
	if (!ret)
		priv->pwm_mode[channel] = mode;
	mutex_unlock(&priv->control_lock);

	return ret;
}

static int waterforce_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct waterforce_data *priv = dev_get_drvdata(dev);
	int ret;

	/* Kept by the driver, so the device need not be queried */
	if (type == hwmon_pwm && attr == hwmon_pwm_enable) {
		*val = priv->pwm_mode[channel];
		return 0;
	}

	ret = waterforce_get_status(priv);
	if (ret < 0)
		return ret;

	switch (type) {
	case hwmon_temp:
		*val = priv->temp_input[channel];
		break;
	case hwmon_fan:
		*val = priv->speed_input[channel];
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			*val = DIV_ROUND_CLOSEST(priv->duty_input[channel] * 255, 100);
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;	/* unreachable */
	}

	return 0;
}

static int waterforce_write(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long val)
{
	struct waterforce_data *priv = dev_get_drvdata(dev);

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_input:
		return waterforce_set_pwm(priv, channel, val);
	case hwmon_pwm_enable:
		return waterforce_set_pwm_mode(priv, channel, val);
	default:
		return -EOPNOTSUPP;
	}
}

static int waterforce_read_string(struct device *dev, enum hwmon_sensor_types type,
				  u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = waterforce_temp_label[channel];
		break;
	case hwmon_fan:
		*str = waterforce_speed_label[channel];
		break;
	default:
		return -EOPNOTSUPP;	/* unreachable */
	}

	return 0;
}

/*
 * fan_profile / pump_profile: reading lists all profiles with the active one in
 * brackets. Writing a name selects it and sets that channel to pwmN_enable = 2.
 */
static ssize_t profile_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct waterforce_data *priv = dev_get_drvdata(dev);
	int ch = to_sensor_dev_attr(attr)->index;
	int i, len = 0;

	for (i = 0; i < channel_profiles[ch].count; i++)
		len += sysfs_emit_at(buf, len, i == priv->active_profile[ch] ? "[%s] " : "%s ",
				     channel_profiles[ch].list[i].name);
	if (len)
		buf[len - 1] = '\n';

	return len;
}

static ssize_t profile_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct waterforce_data *priv = dev_get_drvdata(dev);
	int ch = to_sensor_dev_attr(attr)->index;
	int i, ret;

	for (i = 0; i < channel_profiles[ch].count; i++) {
		if (sysfs_streq(buf, channel_profiles[ch].list[i].name))
			break;
	}
	if (i == channel_profiles[ch].count)
		return -EINVAL;

	mutex_lock(&priv->control_lock);
	priv->profile_sent[ch] = false;	/* an explicit choice is always sent */
	ret = waterforce_apply_profile(priv, ch, i);
	if (!ret) {
		priv->auto_profile[ch] = i;
		priv->pwm_mode[ch] = PWM_MODE_AUTO;
	}
	mutex_unlock(&priv->control_lock);

	return ret < 0 ? ret : count;
}

static SENSOR_DEVICE_ATTR_RW(fan_profile, profile, 0);
static SENSOR_DEVICE_ATTR_RW(pump_profile, profile, 1);

static struct attribute *waterforce_attrs[] = {
	&sensor_dev_attr_fan_profile.dev_attr.attr,
	&sensor_dev_attr_pump_profile.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(waterforce);

static int waterforce_get_fw_ver(struct hid_device *hdev)
{
	struct waterforce_data *priv = hid_get_drvdata(hdev);
	int ret;

	ret = waterforce_write_expanded(priv, get_firmware_ver_cmd, GET_FIRMWARE_VER_CMD_LENGTH);
	if (ret < 0)
		return ret;

	ret = wait_for_completion_interruptible_timeout(&priv->fw_version_processed,
							msecs_to_jiffies(STATUS_VALIDITY));
	if (ret == 0)
		return -ETIMEDOUT;
	else if (ret < 0)
		return ret;

	return 0;
}

static const struct hwmon_ops waterforce_hwmon_ops = {
	.is_visible = waterforce_is_visible,
	.read = waterforce_read,
	.read_string = waterforce_read_string,
	.write = waterforce_write,
};

static const struct hwmon_channel_info *waterforce_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info waterforce_chip_info = {
	.ops = &waterforce_hwmon_ops,
	.info = waterforce_info,
};

static int waterforce_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data,
				int size)
{
	struct waterforce_data *priv = hid_get_drvdata(hdev);

	if (size < 2)
		return 0;

	if (data[0] == get_firmware_ver_cmd[0] && data[1] == get_firmware_ver_cmd[1]) {
		if (size <= FIRMWARE_VER_START_OFFSET_2)
			return 0;

		/* Received a firmware version report */
		priv->firmware_version =
		    data[FIRMWARE_VER_START_OFFSET_1] * 10 + data[FIRMWARE_VER_START_OFFSET_2];

		if (!completion_done(&priv->fw_version_processed))
			complete_all(&priv->fw_version_processed);
		return 0;
	}

	if (data[0] != get_status_cmd[0] || data[1] != get_status_cmd[1])
		return 0;

	if (size <= WATERFORCE_TEMP_SENSOR)
		return 0;

	priv->temp_input[0] = data[WATERFORCE_TEMP_SENSOR] * 1000;
	priv->speed_input[0] = get_unaligned_le16(data + WATERFORCE_FAN_SPEED);
	priv->speed_input[1] = get_unaligned_le16(data + WATERFORCE_PUMP_SPEED);
	priv->duty_input[0] = data[WATERFORCE_FAN_DUTY];
	priv->duty_input[1] = data[WATERFORCE_PUMP_DUTY];

	spin_lock(&priv->status_report_request_lock);
	if (!completion_done(&priv->status_report_received))
		complete_all(&priv->status_report_received);
	spin_unlock(&priv->status_report_request_lock);

	priv->updated = jiffies;

	return 0;
}

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	struct waterforce_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->firmware_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static void waterforce_debugfs_init(struct waterforce_data *priv)
{
	char name[64];

	if (!priv->firmware_version)
		return;	/* There's nothing to show in debugfs */

	scnprintf(name, sizeof(name), "%s-%s", DRIVER_NAME, dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);
}

static int waterforce_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct waterforce_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	/*
	 * Initialize priv->updated to STATUS_VALIDITY seconds in the past, making
	 * the initial empty data invalid for waterforce_read() without the need for
	 * a special case there.
	 */
	priv->updated = jiffies - msecs_to_jiffies(STATUS_VALIDITY);

	/*
	 * The device can't report its active profile. Assume balance, but leave
	 * profile_sent false so the first request is always sent.
	 */
	priv->pwm_mode[0] = PWM_MODE_AUTO;
	priv->pwm_mode[1] = PWM_MODE_AUTO;
	priv->auto_profile[0] = FAN_PROFILE_BALANCE;
	priv->auto_profile[1] = PUMP_PROFILE_BALANCE;
	priv->active_profile[0] = FAN_PROFILE_BALANCE;
	priv->active_profile[1] = PUMP_PROFILE_BALANCE;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed with %d\n", ret);
		return ret;
	}

	/*
	 * Enable hidraw so existing user-space tools can continue to work.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid hw start failed with %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid hw open failed with %d\n", ret);
		goto fail_and_stop;
	}

	priv->buffer = devm_kzalloc(&hdev->dev, MAX_REPORT_LENGTH, GFP_KERNEL);
	if (!priv->buffer) {
		ret = -ENOMEM;
		goto fail_and_close;
	}

	mutex_init(&priv->status_report_request_mutex);
	mutex_init(&priv->buffer_lock);
	mutex_init(&priv->control_lock);
	spin_lock_init(&priv->status_report_request_lock);
	init_completion(&priv->status_report_received);
	init_completion(&priv->fw_version_processed);

	hid_device_io_start(hdev);
	ret = waterforce_get_fw_ver(hdev);
	if (ret < 0)
		hid_warn(hdev, "fw version request failed with %d\n", ret);

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, "waterforce",
							  priv, &waterforce_chip_info,
							  waterforce_groups);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon registration failed with %d\n", ret);
		goto fail_and_close;
	}

	waterforce_debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void waterforce_remove(struct hid_device *hdev)
{
	struct waterforce_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id waterforce_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_GIGABYTE, USB_PRODUCT_ID_WATERFORCE) },
	{ }
};

MODULE_DEVICE_TABLE(hid, waterforce_table);

static struct hid_driver waterforce_driver = {
	.name = "waterforce",
	.id_table = waterforce_table,
	.probe = waterforce_probe,
	.remove = waterforce_remove,
	.raw_event = waterforce_raw_event,
};

static int __init waterforce_init(void)
{
	return hid_register_driver(&waterforce_driver);
}

static void __exit waterforce_exit(void)
{
	hid_unregister_driver(&waterforce_driver);
}

/* When compiled into the kernel, initialize after the HID bus */
late_initcall(waterforce_init);
module_exit(waterforce_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>");
MODULE_DESCRIPTION("Hwmon driver for Gigabyte AORUS Waterforce AIO coolers, with fan and pump control");
MODULE_VERSION("1.1.0-control");
