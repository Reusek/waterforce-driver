// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Gigabyte AORUS Waterforce AIO CPU coolers: X240, X280 and X360.
 *
 * Copyright 2023 Aleksa Savic <savicaleksa83@gmail.com>
 *
 * Out-of-tree fork that adds fan and pump control on top of the read-only
 * mainline driver. Everything below was confirmed on an X360 (1044:7a4d):
 *  - 0xE5 <channel> <code> selects a firmware profile, 0xB6 applies it
 *    (profile codes from D05Col/omarchy-aorus-fan, pump codes probed)
 *  - 0xE6 writes the custom curve (from Aleksa Savic's WIP driver). It only
 *    takes effect while the channel is in custom mode, profile code 0x01.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
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
#define PROFILE_CODE_CUSTOM	0x01	/* follow the 0xE6 custom curve */
static const u8 commit_cmd[] = { 0x99, 0xB6 };
#define CMD_GAP_MS		20

/*
 * Custom curve (0xE6): a big-endian RPM at each of curve_rpm_offsets[], with
 * temperature markers in between (0x1E = 30, 0x32 = 50, 0x41 = 65 degrees C).
 * The same RPM at every point gives a fixed speed.
 */
static const u8 set_curve_cmd_template[] = {
	0x99, 0xE6, 0, 0, 0, 0, 0, 0x1E, 0, 0, 0x32, 0, 0, 0x41, 0, 0
};

#define SET_CURVE_CHANNEL_OFFSET	2
static const u8 curve_channel[2][2] = {
	{ 0x01, 0x01 },	/* Fan */
	{ 0x04, 0x02 },	/* Pump */
};
static const u8 curve_rpm_offsets[] = { 5, 8, 11, 14 };

/* The firmware is reported to leave custom mode at 2800 RPM or more */
#define CURVE_RPM_LIMIT		2799

/*
 * Firmware profiles. Duty is what the device reported at 26 degrees C coolant;
 * the profiles are curves, so it rises with coolant temperature.
 */
struct waterforce_profile {
	const char *name;
	u8 code;
};

static const struct waterforce_profile fan_profiles[] = {
	{ "zero", 0x07 },		/* fan stop */
	{ "quiet", 0x06 },		/* ~56 % */
	{ "balance", 0x00 },		/* ~72 % */
	{ "performance", 0x05 },	/* ~88 % */
	{ "max", 0x04 },		/* 100 % */
	{ "default", 0x02 },		/* 100 % on tested unit */
};

static const struct waterforce_profile pump_profiles[] = {
	{ "silent", 0x05 },		/* ~63 %, ~2100 RPM */
	{ "quiet", 0x06 },		/* ~66 %, ~2140 RPM */
	{ "balance", 0x00 },		/* ~81 %, ~2650 RPM */
	{ "high", 0x02 },		/* ~93 %, ~2980 RPM */
	{ "max", 0x04 },		/* 100 %, ~3250 RPM */
};

#define FAN_PROFILE_BALANCE	2
#define FAN_PROFILE_MAX		4
#define PUMP_PROFILE_BALANCE	2
#define PUMP_PROFILE_MAX	4
#define PROFILE_CUSTOM		0xFF	/* active_profile value for custom mode */

static const struct {
	const struct waterforce_profile *list;
	int count;
	u8 selector;
	u8 max_idx;
} channel_profiles[2] = {
	{ fan_profiles, ARRAY_SIZE(fan_profiles), SET_PROFILE_FAN, FAN_PROFILE_MAX },
	{ pump_profiles, ARRAY_SIZE(pump_profiles), SET_PROFILE_PUMP, PUMP_PROFILE_MAX },
};

/* pwmN_enable values */
#define PWM_MODE_MANUAL	1	/* pwmN / fanN_target set a fixed speed */
#define PWM_MODE_AUTO	2	/* the profile chosen in fan_profile / pump_profile */

static unsigned int fan_min_rpm = 750;
module_param(fan_min_rpm, uint, 0644);
MODULE_PARM_DESC(fan_min_rpm, "Lowest fan speed a manual setting may ask for (default 750)");

static unsigned int fan_max_rpm = 2600;
module_param(fan_max_rpm, uint, 0644);
MODULE_PARM_DESC(fan_max_rpm, "Fan speed at pwm1 = 255 (default 2600)");

static unsigned int pump_min_rpm = 1500;
module_param(pump_min_rpm, uint, 0644);
MODULE_PARM_DESC(pump_min_rpm, "Lowest pump speed a manual setting may ask for (default 1500)");

static unsigned int pump_max_rpm = 3300;
module_param(pump_max_rpm, uint, 0644);
MODULE_PARM_DESC(pump_max_rpm, "Pump speed at pwm2 = 255 (default 3300)");

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
	u8 active_profile[2];	/* Index of the profile last sent, or PROFILE_CUSTOM */
	u16 target_rpm[2];	/* Fixed speed last sent while in custom mode */
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
		case hwmon_fan_target:
			return 0644;
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

/* Invalidate cached status so the next read shows the effect of a command */
static void waterforce_invalidate_status(struct waterforce_data *priv)
{
	priv->updated = jiffies - msecs_to_jiffies(STATUS_VALIDITY);
}

/* Send 0xE5 <channel> <code> followed by the 0xB6 apply command */
static int waterforce_send_profile_code(struct waterforce_data *priv, int channel, u8 code)
{
	u8 cmd[4] = { 0x99, SET_PROFILE_CMD, channel_profiles[channel].selector, code };
	int ret;

	ret = waterforce_write_expanded(priv, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	msleep(CMD_GAP_MS);

	return waterforce_write_expanded(priv, commit_cmd, sizeof(commit_cmd));
}

/*
 * Select a firmware profile on one channel. Skipped if it is already active,
 * since tools such as CoolerControl rewrite pwm often. Caller holds control_lock.
 */
static int waterforce_apply_profile(struct waterforce_data *priv, int channel, int idx)
{
	int ret;

	if (priv->profile_sent[channel] && priv->active_profile[channel] == idx)
		return 0;

	ret = waterforce_send_profile_code(priv, channel,
					   channel_profiles[channel].list[idx].code);
	if (ret < 0)
		return ret;

	priv->active_profile[channel] = idx;
	priv->profile_sent[channel] = true;
	waterforce_invalidate_status(priv);
	return 0;
}

/*
 * Run one channel at a fixed speed: write a flat custom curve, then switch the
 * channel to custom mode. Skipped if that speed is already set. Caller holds
 * control_lock.
 */
static int waterforce_apply_rpm(struct waterforce_data *priv, int channel, u16 rpm)
{
	u8 cmd[sizeof(set_curve_cmd_template)];
	int i, ret;

	if (priv->profile_sent[channel] && priv->active_profile[channel] == PROFILE_CUSTOM &&
	    priv->target_rpm[channel] == rpm)
		return 0;

	memcpy(cmd, set_curve_cmd_template, sizeof(cmd));
	cmd[SET_CURVE_CHANNEL_OFFSET] = curve_channel[channel][0];
	cmd[SET_CURVE_CHANNEL_OFFSET + 1] = curve_channel[channel][1];
	for (i = 0; i < ARRAY_SIZE(curve_rpm_offsets); i++)
		put_unaligned_be16(rpm, cmd + curve_rpm_offsets[i]);

	ret = waterforce_write_expanded(priv, cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	msleep(CMD_GAP_MS);

	ret = waterforce_send_profile_code(priv, channel, PROFILE_CODE_CUSTOM);
	if (ret < 0)
		return ret;

	priv->active_profile[channel] = PROFILE_CUSTOM;
	priv->target_rpm[channel] = rpm;
	priv->profile_sent[channel] = true;
	waterforce_invalidate_status(priv);
	return 0;
}

static unsigned int waterforce_min_rpm(int channel)
{
	return min_t(unsigned int, channel ? READ_ONCE(pump_min_rpm) : READ_ONCE(fan_min_rpm),
		     CURVE_RPM_LIMIT);
}

static unsigned int waterforce_max_rpm(int channel)
{
	return channel ? READ_ONCE(pump_max_rpm) : READ_ONCE(fan_max_rpm);
}

static int waterforce_set_rpm(struct waterforce_data *priv, int channel, long rpm)
{
	int ret;

	rpm = clamp_val(rpm, waterforce_min_rpm(channel), CURVE_RPM_LIMIT);

	mutex_lock(&priv->control_lock);
	ret = waterforce_apply_rpm(priv, channel, rpm);
	if (!ret)
		priv->pwm_mode[channel] = PWM_MODE_MANUAL;
	mutex_unlock(&priv->control_lock);

	return ret;
}

/*
 * pwm 0-254 maps linearly onto 0..max RPM for that channel, raised to the
 * minimum and capped below the custom-mode limit. pwm 255 selects the max
 * profile, which is the only way to get full speed.
 */
static int waterforce_set_pwm(struct waterforce_data *priv, int channel, long val)
{
	int ret;

	if (val < 0 || val > 255)
		return -EINVAL;

	if (val < 255)
		return waterforce_set_rpm(priv, channel,
					  DIV_ROUND_CLOSEST(val * waterforce_max_rpm(channel), 255));

	mutex_lock(&priv->control_lock);
	ret = waterforce_apply_profile(priv, channel, channel_profiles[channel].max_idx);
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
	/* Manual keeps the current setting until the next pwm write */
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
	if (type == hwmon_fan && attr == hwmon_fan_target) {
		*val = priv->active_profile[channel] == PROFILE_CUSTOM ?
		       priv->target_rpm[channel] : 0;
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

	if (type == hwmon_fan && attr == hwmon_fan_target)
		return waterforce_set_rpm(priv, channel, val);

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
	if (priv->active_profile[ch] == PROFILE_CUSTOM)
		len += sysfs_emit_at(buf, len, "[custom %u rpm] ", priv->target_rpm[ch]);
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
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_TARGET,
			   HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_TARGET),
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
MODULE_VERSION("1.3.0-control");
