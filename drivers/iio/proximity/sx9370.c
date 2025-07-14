// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Leica Geosystems AG <https://leica-geosystems.com>
/*
 * Driver for the SX937x
 */

#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/types.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "sx9370.h"

#define SX9370_CONVERSION_TIMEOUT_MS 200

struct sx9370_fw_header {
	__le32 magic;
	__le32 entry_count;
} __packed;
struct sx9370_fw_entry {
	__le32 reg_addr;
	__le32 reg_value;
} __packed;

#define SX9370_FW_MAGIC 0x53589370
#define SX9370_FW_MAX_ENTRIES 512

#define SX9370_FW_CALC_SIZE(c) \
	(sizeof(struct sx9370_fw_header) + (c) * sizeof(struct sx9370_fw_entry))

struct sx9370_data {
	struct mutex mutex;
	struct i2c_client *client;
	struct iio_trigger *trig;
	struct regmap *regmap;
	int prox_stat[SX9370_NUM_CHANNELS];
	bool trigger_enabled;
	u32 *buffer;
	struct completion completion;
	int channel_users[SX9370_NUM_CHANNELS];
	struct gpio_desc *dpr_gpio;
	struct regulator *vdd_supply;
	struct delayed_work firmware_load_work;
};

static const struct iio_event_spec sx9370_events[] = { {
	.mask_separate = BIT(IIO_EV_INFO_VALUE),
	.type = IIO_EV_TYPE_THRESH,
	.dir = IIO_EV_DIR_EITHER,
} };

#define SX9370_CHANNEL(idx)                                                \
	{                                                                  \
		.type = IIO_PROXIMITY,                                     \
		.info_mask_separate = BIT(IIO_CHAN_INFO_DEBOUNCE_COUNT) |  \
				      BIT(IIO_CHAN_INFO_SAMP_FREQ) |       \
				      BIT(IIO_CHAN_INFO_PROCESSED) |       \
				      BIT(IIO_CHAN_INFO_SCALE) |           \
				      BIT(IIO_CHAN_INFO_RAW),              \
		.info_mask_shared_by_all_available =                       \
			BIT(IIO_CHAN_INFO_DEBOUNCE_COUNT) |                \
			BIT(IIO_CHAN_INFO_SAMP_FREQ) |                     \
			BIT(IIO_CHAN_INFO_SCALE),                          \
		.indexed = 1, .channel = idx, .event_spec = sx9370_events, \
		.channel2 = idx, .datasheet_name = "proximity",            \
		.num_event_specs = ARRAY_SIZE(sx9370_events),              \
		.scan_index = idx,                                         \
		.scan_type = {                                             \
			.sign = 'u',                                       \
			.realbits = 32,                                    \
			.storagebits = 32,                                 \
			.shift = 0,                                        \
		},                                                         \
	}

static const struct iio_chan_spec sx9370_channels[] = {
	SX9370_CHANNEL(0), SX9370_CHANNEL(1), SX9370_CHANNEL(2),
	SX9370_CHANNEL(3), SX9370_CHANNEL(4), SX9370_CHANNEL(5),
	SX9370_CHANNEL(6), SX9370_CHANNEL(7), IIO_CHAN_SOFT_TIMESTAMP(8),
};

static const int sx9370_samp_freq_table[] = {
	250000, 219000, 194000, 175000, 159000, 146000, 135000, 125000,
	117000, 109000, 97200,	87500,	79600,	72900,	67300,	62500,
	58300,	54700,	48600,	43800,	39800,	36500,	33700,	31300,
	29200,	27300,	21900,	18200,	15600,	7800,	4600,	3400
};

static const int sx9370_scale_factor[] = { 1, 2, 4, 8, 16, 32, 64 };
static const u32 sx9370_debounce_table[] = { 0, 2, 4, 8 };

static const struct regmap_config sx9370_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.max_register = 0x8298,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static int sx9370_send_cfg(const struct firmware *fw, struct sx9370_data *data)
{
	const struct sx9370_fw_header *header;
	const struct sx9370_fw_entry *entries;
	u32 magic, entry_count;
	size_t expected_size;
	int ret, i;

	if (fw->size < sizeof(struct sx9370_fw_header)) {
		dev_err(&data->client->dev, "Firmware too small: %zu bytes\n",
			fw->size);
		return -EINVAL;
	}

	header = (const struct sx9370_fw_header *)fw->data;
	magic = le32_to_cpu(header->magic);
	entry_count = le32_to_cpu(header->entry_count);

	if (magic != SX9370_FW_MAGIC) {
		dev_err(&data->client->dev,
			"Invalid firmware magic: 0x%08x, expected 0x%08x\n",
			magic, SX9370_FW_MAGIC);
		return -EINVAL;
	}

	if (entry_count > SX9370_FW_MAX_ENTRIES) {
		dev_err(&data->client->dev, "Too many entries: %d, max %d\n",
			entry_count, SX9370_FW_MAX_ENTRIES);
		return -EINVAL;
	}

	expected_size = SX9370_FW_CALC_SIZE(entry_count);
	if (fw->size != expected_size) {
		dev_err(&data->client->dev,
			"Firmware size mismatch: got %zu, expected %zu\n",
			fw->size, expected_size);
		return -EINVAL;
	}

	entries = (const struct sx9370_fw_entry *)(fw->data + sizeof(*header));

	for (i = 0; i < entry_count; i++) {
		u32 reg_addr = le32_to_cpu(entries[i].reg_addr);
		u32 reg_value = le32_to_cpu(entries[i].reg_value);

		dev_info(&data->client->dev, "Config[%d]: 0x%04x = 0x%08x\n", i,
			 reg_addr, reg_value);

		if (reg_addr > sx9370_regmap_config.max_register) {
			dev_err(&data->client->dev,
				"Invalid register address: 0x%04x\n", reg_addr);
			return -EINVAL;
		}

		ret = regmap_write(data->regmap, reg_addr, reg_value);
		if (ret < 0) {
			dev_err(&data->client->dev,
				"Failed to write register 0x%04x = 0x%08x: %d\n",
				reg_addr, reg_value, ret);
			return ret;
		}
	}

	dev_info(&data->client->dev, "Apply registers config success\n");

	return 0;
}

static void sx9370_cfg_update(const struct firmware *fw, void *context)
{
	struct sx9370_data *data = context;
	struct device *dev = &data->client->dev;
	int ret;

	if (!fw || !fw->data) {
		dev_warn(dev,
			 "No firmware found, using default configuration\n");
		return;
	}

	ret = sx9370_send_cfg(fw, data);
	if (ret)
		dev_warn(dev, "Firmware update failed: %d\n", ret);

	release_firmware(fw);
}

static void sx9370_firmware_load_work(struct work_struct *work)
{
	struct sx9370_data *data =
		container_of(work, struct sx9370_data, firmware_load_work.work);
	struct iio_dev *indio_dev = dev_get_drvdata(&data->client->dev);
	char firmware_name[32];
	int ret;

	/* Construct firmware name based on detected chip type
	 * Currently only sx9370.bin is supported and loaded.
	 * Future chip variants (sx9373, sx9374, sx9376) will have
	 * their respective firmware files when support is added.
	 */
	snprintf(firmware_name, sizeof(firmware_name), "%s.bin",
		 indio_dev->name);

	ret = request_firmware_nowait(THIS_MODULE, true, firmware_name,
				      &data->client->dev, GFP_KERNEL, data,
				      sx9370_cfg_update);
	if (ret) {
		if (ret == -ENOENT)
			dev_info(&data->client->dev,
				 "Firmware %s not found, using defaults\n",
				 firmware_name);
		else
			dev_warn(
				&data->client->dev,
				"Failed to request firmware %s: %d, using defaults\n",
				firmware_name, ret);

	} else {
		dev_info(&data->client->dev, "Firmware loading initiated: %s\n",
			 firmware_name);
	}
}

static void sx9370_cleanup_firmware_work(void *data)
{
	struct sx9370_data *priv = data;

	cancel_delayed_work_sync(&priv->firmware_load_work);
}

static int sx9370_inc_chan_users(struct sx9370_data *data, int chan)
{
	data->channel_users[chan]++;
	return data->channel_users[chan];
}

static int sx9370_dec_chan_users(struct sx9370_data *data, int chan)
{
	data->channel_users[chan]--;
	return data->channel_users[chan];
}

static int sx9370_inc_users(struct sx9370_data *data, int chan)
{
	if (sx9370_inc_chan_users(data, chan) == 1)
		return regmap_update_bits(data->regmap, SX9370_COMMAND,
					  BIT(chan), BIT(chan));
	return 0;
}

static int sx9370_dec_users(struct sx9370_data *data, int chan)
{
	if (sx9370_dec_chan_users(data, chan) == 0)
		return regmap_update_bits(data->regmap, SX9370_COMMAND,
					  BIT(chan), 0);

	return 0;
}

static int sx9370_read_reg_field(struct sx9370_data *data, int *val, int reg,
				 u32 mask)
{
	unsigned int regval;
	int ret;

	ret = regmap_read(data->regmap, reg, &regval);
	if (ret < 0)
		return ret;

	*val = (regval & mask) >> __ffs(mask);

	return 0;
}

static int sx9370_read_samp_freq(struct sx9370_data *data,
				 const struct iio_chan_spec *chan, int *val,
				 int *val2)
{
	int regfield;
	int ret;

	ret = sx9370_read_reg_field(data, &regfield,
				    SX9370_AFE_PARAMETERS_PH(chan->channel),
				    SX9370_SAMPLE_FREQ_MASK);
	if (ret < 0)
		return ret;

	*val = sx9370_samp_freq_table[regfield];
	*val2 = 0;

	return IIO_VAL_INT;
}

static int sx9370_read_debounce_count(struct sx9370_data *data,
				      const struct iio_chan_spec *chan,
				      int *val, int *val2)
{
	int regfield;
	int ret;

	ret = sx9370_read_reg_field(data, &regfield,
				    SX9370_FILTER_SETUP_A_PH(chan->channel),
				    DEBOUNCER_RELEASE_MASK);
	if (ret < 0)
		return ret;

	*val = sx9370_debounce_table[regfield];
	*val2 = 0;
	return IIO_VAL_INT;
}

static int sx9370_read_scale(struct sx9370_data *data,
			     const struct iio_chan_spec *chan, int *val,
			     int *val2)
{
	int regfield;
	int ret;

	ret = sx9370_read_reg_field(data, &regfield,
				    SX9370_FILTER_SETUP_A_PH(chan->channel),
				    SX9370_SCALE_MASK);
	if (ret < 0)
		return ret;

	*val = sx9370_scale_factor[regfield];
	*val2 = 0;

	return IIO_VAL_INT;
}

static int sx9370_set_samp_freq(struct sx9370_data *data,
				const struct iio_chan_spec *chan, int val,
				int val2)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sx9370_samp_freq_table); i++)
		if (val == sx9370_samp_freq_table[i] && val2 == 0)
			return regmap_update_bits(
				data->regmap,
				SX9370_AFE_PARAMETERS_PH(chan->channel),
				SX9370_SAMPLE_FREQ_MASK,
				i << SX9370_SAMPLE_FREQ_SHIFT);
	return -EINVAL;
}

static int sx9370_set_debounce_count(struct sx9370_data *data,
				     const struct iio_chan_spec *chan, int val,
				     int val2)
{
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(sx9370_debounce_table); i++) {
		if (val == sx9370_debounce_table[i] && val2 == 0) {
			ret = regmap_update_bits(
				data->regmap,
				SX9370_FILTER_SETUP_A_PH(chan->channel),
				DEBOUNCER_RELEASE_MASK,
				i << DEBOUNCER_RELEASE_SHIFT);
			if (ret < 0)
				return ret;

			ret = regmap_update_bits(
				data->regmap,
				SX9370_FILTER_SETUP_A_PH(chan->channel),
				DEBOUNCER_DETECTED_MASK,
				i << DEBOUNCER_DETECTED_SHIFT);

			return ret;
		}
	}
	return -EINVAL;
}

static int sx9370_set_diff_scale(struct sx9370_data *data,
				 const struct iio_chan_spec *chan, int val,
				 int val2)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sx9370_scale_factor); i++) {
		if (val == sx9370_scale_factor[i] && val2 == 0) {
			return regmap_update_bits(
				data->regmap,
				SX9370_FILTER_SETUP_A_PH(chan->channel),
				SX9370_SCALE_MASK, i << SX9370_SCALE_SHIFT);
		}
	}
	return -EINVAL;
}

static int sx9370_write_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan, int val, int val2,
			    long mask)
{
	struct sx9370_data *data = iio_priv(indio_dev);

	switch (chan->type) {
	case IIO_PROXIMITY:
		switch (mask) {
		case IIO_CHAN_INFO_SAMP_FREQ:
			return sx9370_set_samp_freq(data, chan, val, val2);
		case IIO_CHAN_INFO_DEBOUNCE_COUNT:
			return sx9370_set_debounce_count(data, chan, val, val2);
		case IIO_CHAN_INFO_SCALE:
			return sx9370_set_diff_scale(data, chan, val, val2);
		default:
			dev_err(&data->client->dev,
				"unknown channel type %d for write_raw\n",
				chan->type);
			return -EINVAL;
		}
	default:
		dev_err(&data->client->dev,
			"unknown channel type %d for write_raw\n", chan->type);
		return -EINVAL;
	}
}

static int sx9370_enable_conversion_irq(struct sx9370_data *data, bool enable)
{
	int ret;

	if (enable) {
		ret = regmap_update_bits(data->regmap, SX9370_IRQ_MASK_A,
					 SX9370_IRQ_CONVERSION_DONE,
					 SX9370_IRQ_CONVERSION_DONE);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(data->regmap, SX9370_IRQ_SETUP,
					 SX9370_IRQ_PAUSE_ON_INTERRUPT,
					 SX9370_IRQ_PAUSE_ON_INTERRUPT);
	} else {
		ret = regmap_update_bits(data->regmap, SX9370_IRQ_MASK_A,
					 SX9370_IRQ_CONVERSION_DONE, 0);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(data->regmap, SX9370_IRQ_SETUP,
					 SX9370_IRQ_PAUSE_ON_INTERRUPT, 0);
	}

	return ret;
}

static int sx9370_read_conversion_sync(struct sx9370_data *data,
				       struct iio_chan_spec const *chan,
				       int max_len, int *vals, int *val_len,
				       long mask)
{
	int ret;

	ret = wait_for_completion_timeout(
		&data->completion,
		msecs_to_jiffies(SX9370_CONVERSION_TIMEOUT_MS));

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		/*
		 * 4 values: offset, useful, usefilter, average
		 */
		if (max_len < 4)
			return -EINVAL;

		ret = sx9370_read_reg_field(data, &vals[0],
					    SX9370_OFFSET_PH(chan->channel),
					    SX9370_OFFSET_MASK);
		if (ret < 0)
			return ret;

		ret = sx9370_read_reg_field(data, &vals[1],
					    SX9370_USEFUL_PH(chan->channel),
					    SX9370_USEFUL_MASK);
		if (ret < 0)
			return ret;
		ret = sx9370_read_reg_field(data, &vals[2],
					    SX9370_USEFILTER_PH(chan->channel),
					    SX9370_USEFILTER_MASK);
		if (ret < 0)
			return ret;
		ret = sx9370_read_reg_field(data, &vals[3],
					    SX9370_AVERAGE_PH(chan->channel),
					    SX9370_AVERAGE_MASK);
		if (ret < 0)
			return ret;

		*val_len = 4;
		return IIO_VAL_INT_MULTIPLE;

	case IIO_CHAN_INFO_RAW:
		ret = sx9370_read_reg_field(data, &vals[0],
					    SX9370_DIFF_PH(chan->channel),
					    SX9370_DIFF_MASK);
		if (ret < 0)
			return ret;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}

	return -EINVAL;
}

static int sx9370_update_scan_mode(struct iio_dev *indio_dev,
				   const unsigned long *scan_mask)
{
	struct sx9370_data *data = iio_priv(indio_dev);
	u32 *old_buffer, *new_buffer;

	new_buffer = kzalloc(indio_dev->scan_bytes, GFP_KERNEL);
	if (!new_buffer)
		return -ENOMEM;

	mutex_lock(&data->mutex);
	old_buffer = data->buffer;
	data->buffer = new_buffer;
	mutex_unlock(&data->mutex);

	kfree(old_buffer);
	return 0;
}

static int sx9370_read_raw_multi(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan, int max_len,
				 int *vals, int *val_len, long mask)
{
	struct sx9370_data *data = iio_priv(indio_dev);
	int ret;

	if (chan->type != IIO_PROXIMITY) {
		dev_err(&data->client->dev,
			"unknown channel type %d for read_raw\n", chan->type);
		return -EINVAL;
	}

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		return sx9370_read_samp_freq(data, chan, &vals[0], &vals[1]);
	case IIO_CHAN_INFO_DEBOUNCE_COUNT:
		return sx9370_read_debounce_count(data, chan, &vals[0],
						  &vals[1]);
	case IIO_CHAN_INFO_SCALE:
		return sx9370_read_scale(data, chan, &vals[0], &vals[1]);

	case IIO_CHAN_INFO_RAW:
	case IIO_CHAN_INFO_PROCESSED:
		ret = sx9370_enable_conversion_irq(data, true);
		if (ret < 0)
			return ret;
		ret = sx9370_read_conversion_sync(data, chan, max_len, vals,
						  val_len, mask);
		sx9370_enable_conversion_irq(data, false);
		return ret;
	default:
		dev_err(&data->client->dev,
			"unknown mask %ld for read_raw_multi\n", mask);
		return -EINVAL;
	}
}

static int sx9370_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, const int **vals,
			     int *type, int *length, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = (const int *)sx9370_samp_freq_table;
		*length = ARRAY_SIZE(sx9370_samp_freq_table);
		*type = IIO_VAL_INT;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_DEBOUNCE_COUNT:
		*vals = (const int *)sx9370_debounce_table;
		*length = ARRAY_SIZE(sx9370_debounce_table);
		*type = IIO_VAL_INT;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SCALE:
		*vals = (const int *)sx9370_scale_factor;
		*length = ARRAY_SIZE(sx9370_scale_factor);
		*type = IIO_VAL_INT;
		return IIO_AVAIL_LIST;

	default:
		dev_err(&indio_dev->dev, "unknown mask %ld for read_avail\n",
			mask);
		return -EINVAL;
	}
}

static irqreturn_t sx9370_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct sx9370_data *data = iio_priv(indio_dev);

	if (data->trigger_enabled)
		iio_trigger_poll(data->trig);
	return IRQ_WAKE_THREAD;
}

static void sx9370_handle_proximity_events(struct iio_dev *indio_dev)
{
	struct sx9370_data *data = iio_priv(indio_dev);
	unsigned int status_a, status_c;
	bool any_sar_proximity = false;
	enum iio_event_direction dir;
	unsigned int engine1_status;
	unsigned int engine2_status;
	int ph, prox;
	int ret;

	ret = regmap_read(data->regmap, SX9370_DEVICE_STATUS_A, &status_a);
	if (ret < 0) {
		dev_err(&data->client->dev, "Failed to read status A: %d\n",
			ret);
		return;
	}

	ret = regmap_read(data->regmap, SX9370_DEVICE_STATUS_C, &status_c);
	if (ret < 0) {
		dev_err(&data->client->dev, "Failed to read status C: %d\n",
			ret);
		return;
	}

	for (ph = 0; ph < SX9370_NUM_CHANNELS; ph++) {
		for (prox = 0; prox < 4; prox++) {
			int bit_pos = ph + ((3 - prox) * SX9370_NUM_CHANNELS);
			bool new_state = !!(status_a & BIT(bit_pos));
			bool old_state = !!(data->prox_stat[ph] & BIT(prox));

			if (new_state != old_state) {
				dev_info(&data->client->dev,
					 "PH%d THRESH%d %s event detected\n",
					 ph, prox + 1,
					 new_state ? "CLOSE" : "FAR");

				if (new_state) {
					dir = IIO_EV_DIR_RISING;
					data->prox_stat[ph] |= BIT(prox);
				} else {
					dir = IIO_EV_DIR_FALLING;
					data->prox_stat[ph] &= ~BIT(prox);
				}

				iio_push_event(indio_dev,
					       IIO_UNMOD_EVENT_CODE(
						       IIO_PROXIMITY, ph,
						       IIO_EV_TYPE_THRESH, dir),
					       iio_get_time_ns(indio_dev));
			}
		}
		if (data->prox_stat[ph] & (BIT(0) | BIT(1)))
			any_sar_proximity = true;
	}
	engine1_status = (status_c & SX9370_SMART_ENGINE_1_MASK) >>
			 __ffs(SX9370_SMART_ENGINE_1_MASK);
	engine2_status = (status_c & SX9370_SMART_ENGINE_2_MASK) >>
			 __ffs(SX9370_SMART_ENGINE_2_MASK);
	if (engine1_status == SX9370_SMART_SENSING_BODY) {
		any_sar_proximity = true;
		dev_info(&data->client->dev,
			 "Smart Sensing Engine 1: Body detected\n");
	}
	if (engine2_status == SX9370_SMART_SENSING_BODY) {
		any_sar_proximity = true;
		dev_info(&data->client->dev,
			 "Smart Sensing Engine 2: Body detected\n");
	}
	gpiod_set_value_cansleep(data->dpr_gpio, any_sar_proximity);
}

static irqreturn_t sx9370_irq_thread_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct sx9370_data *data = iio_priv(indio_dev);
	int ret;
	unsigned int val;

	ret = regmap_read(data->regmap, SX9370_IRQ_SOURCE, &val);
	if (ret < 0) {
		dev_err(&data->client->dev, "i2c transfer error in irq\n");
		return IRQ_HANDLED;
	}

	if (val & SX9370_IRQ_SMART_SENSOR)
		sx9370_handle_proximity_events(indio_dev);
	if (val & SX9370_IRQ_RELEASE)
		sx9370_handle_proximity_events(indio_dev);
	if (val & SX9370_IRQ_DETECTED)
		sx9370_handle_proximity_events(indio_dev);
	if (val & SX9370_IRQ_CONVERSION_DONE)
		complete(&data->completion);
	return IRQ_HANDLED;
}

static int sx9370_write_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info, int val, int val2)
{
	struct sx9370_data *data = iio_priv(indio_dev);
	int scale_factor;
	int mask;
	int ret;

	if (chan->channel < 0 || chan->channel >= SX9370_NUM_CHANNELS)
		return -EINVAL;

	switch (type) {
	case IIO_EV_TYPE_THRESH:
		mask = GENMASK(7, 0);
		break;
	default:
		return -EINVAL;
	}

	switch (info) {
	case IIO_EV_INFO_VALUE:
		ret = sx9370_read_reg_field(
			data, &scale_factor,
			SX9370_FILTER_SETUP_A_PH(chan->channel),
			SX9370_SCALE_MASK);
		if (ret < 0)
			return ret;
		val = int_sqrt((val << 1) / (1 << scale_factor));

		return regmap_update_bits(data->regmap,
					  SX9370_PROX_THRESH_PH(chan->channel),
					  mask, val << __ffs(mask));
	default:
		dev_err(&data->client->dev, "unknown info %d\n", info);
		return -EINVAL;
	}
}

static int sx9370_read_event_value(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan,
				   enum iio_event_type type,
				   enum iio_event_direction dir,
				   enum iio_event_info info, int *val,
				   int *val2)
{
	struct sx9370_data *data = iio_priv(indio_dev);
	unsigned int mask;
	int scale_factor;
	int thresh_value;
	int ret;

	if (chan->channel < 0 || chan->channel >= SX9370_NUM_CHANNELS)
		return -EINVAL;

	switch (type) {
	case IIO_EV_TYPE_THRESH:
		mask = GENMASK(7, 0);
		break;
	default:
		dev_err(&data->client->dev, "unknown type %d\n", type);
		return -EINVAL;
	}

	switch (info) {
	case IIO_EV_INFO_VALUE:
		ret = sx9370_read_reg_field(
			data, &thresh_value,
			SX9370_PROX_THRESH_PH(chan->channel), mask);
		if (ret < 0)
			return ret;

		ret = sx9370_read_reg_field(
			data, &scale_factor,
			SX9370_FILTER_SETUP_A_PH(chan->channel),
			SX9370_SCALE_MASK);
		if (ret < 0)
			return ret;

		if (scale_factor == 0x07)
			return -EINVAL;

		scale_factor = 1 << scale_factor;

		*val = ((thresh_value * thresh_value) >> 1) * scale_factor;
		*val2 = 0;
		return IIO_VAL_INT;

	default:
		dev_err(&data->client->dev, "unknown info %d\n", info);
		return -EINVAL;
	}
}

static int sx9370_validate_trigger(struct iio_dev *indio_dev,
				   struct iio_trigger *trig)
{
	struct sx9370_data *data = iio_priv(indio_dev);

	if (data->trig != trig)
		return -EINVAL;

	return 0;
}

static const struct iio_info sx9370_info = {
	.read_avail = &sx9370_read_avail,
	.read_event_value = &sx9370_read_event_value,
	.read_raw_multi = &sx9370_read_raw_multi,
	.write_event_value = &sx9370_write_event_value,
	.write_raw = &sx9370_write_raw,
	.update_scan_mode = &sx9370_update_scan_mode,
	.validate_trigger = &sx9370_validate_trigger,
};

static int sx9370_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct sx9370_data *data = iio_priv(indio_dev);
	int ret;

	ret = sx9370_enable_conversion_irq(data, state);
	if (ret < 0)
		return ret;

	data->trigger_enabled = state;
	return 0;
}

static const struct iio_trigger_ops sx9370_trigger_ops = {
	.validate_device = &iio_trigger_validate_own_device,
	.set_trigger_state = sx9370_set_trigger_state,
};

static irqreturn_t sx9370_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct sx9370_data *data = iio_priv(indio_dev);
	int val;
	int bit;
	int ret;
	int i = 0;

	ret = wait_for_completion_timeout(&data->completion,
					  msecs_to_jiffies(200));
	if (!ret) {
		dev_err(&data->client->dev,
			"Timeout waiting for conversion done\n");
		iio_trigger_notify_done(indio_dev->trig);
		return IRQ_HANDLED;
	}

	mutex_lock(&data->mutex);

	iio_for_each_active_channel(indio_dev, bit) {
		ret = sx9370_read_reg_field(
			data, &val,
			SX9370_DIFF_PH(indio_dev->channels[bit].channel),
			SX9370_DIFF_MASK);
		if (ret < 0) {
			dev_err(&data->client->dev,
				"Failed to read channel %d: %d\n", bit, ret);
			break;
		}

		data->buffer[i++] = val;
	}

	if (i > 0)
		iio_push_to_buffers_with_timestamp(indio_dev, data->buffer,
						   iio_get_time_ns(indio_dev));

	mutex_unlock(&data->mutex);
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int sx9370_buffer_postenable(struct iio_dev *indio_dev)
{
	struct sx9370_data *data = iio_priv(indio_dev);
	int ret = 0, i;

	mutex_lock(&data->mutex);
	for (i = 0; i < SX9370_NUM_CHANNELS; i++) {
		if (test_bit(i, indio_dev->active_scan_mask)) {
			ret = sx9370_inc_users(data, i);
			if (ret < 0)
				break;
		}
	}

	if (ret < 0) {
		for (i = i - 1; i >= 0; i--) {
			if (test_bit(i, indio_dev->active_scan_mask))
				sx9370_dec_users(data, i);
		}
		mutex_unlock(&data->mutex);
		return ret;
	}

	mutex_unlock(&data->mutex);

	return sx9370_enable_conversion_irq(data, true);
}

static int sx9370_buffer_predisable(struct iio_dev *indio_dev)
{
	struct sx9370_data *data = iio_priv(indio_dev);
	int ret, i;

	ret = sx9370_enable_conversion_irq(data, false);
	if (ret < 0)
		return ret;

	mutex_lock(&data->mutex);

	for (i = 0; i < SX9370_NUM_CHANNELS; i++) {
		if (test_bit(i, indio_dev->active_scan_mask))
			sx9370_dec_users(data, i);
	}

	mutex_unlock(&data->mutex);

	return 0;
}

static const struct iio_buffer_setup_ops sx9370_buffer_setup_ops = {
	.postenable = sx9370_buffer_postenable,
	.predisable = sx9370_buffer_predisable,
};

static int sx9370_init_device(struct iio_dev *indio_dev)
{
	struct sx9370_data *data = iio_priv(indio_dev);
	unsigned int val;
	int ret;

	ret = regmap_write(data->regmap, SX9370_DEVICE_RESET,
			   SX9370_SOFT_RESET);
	if (ret < 0)
		return ret;

	usleep_range(1000, 2000);
	ret = regmap_read_poll_timeout(data->regmap, SX9370_IRQ_SOURCE, val,
				       (val == 0), 1000, 1000000);
	if (ret < 0)
		dev_warn(&data->client->dev, "Reset timeout: %d\n", ret);

	ret = regmap_read(data->regmap, SX9370_DEVICE_INFO, &val);
	if (ret < 0)
		return ret;

	switch (FIELD_GET(SX9370_WHOAMI_MASK, val)) {
	case CHIP_SX9370:
		indio_dev->name = "sx9370";
		break;
	default:
		dev_err(&data->client->dev, "Unknown device ID: 0x%04x\n",
			FIELD_GET(SX9370_WHOAMI_MASK, val));
		return -ENODEV;
	}
	dev_info(&data->client->dev, "Device ID: %s\n", indio_dev->name);

	INIT_DELAYED_WORK(&data->firmware_load_work, sx9370_firmware_load_work);
	schedule_delayed_work(&data->firmware_load_work,
			      msecs_to_jiffies(5000));

	return 0;
}

static int sx9370_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct sx9370_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	data->trigger_enabled = false;
	mutex_init(&data->mutex);
	init_completion(&data->completion);

	data->regmap = devm_regmap_init_i2c(client, &sx9370_regmap_config);
	if (IS_ERR(data->regmap))
		return PTR_ERR(data->regmap);

	data->vdd_supply = devm_regulator_get_optional(&client->dev, "vdd");
	if (IS_ERR(data->vdd_supply)) {
		ret = PTR_ERR(data->vdd_supply);
		if (ret == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		data->vdd_supply = NULL;
	}
	if (data->vdd_supply) {
		ret = regulator_enable(data->vdd_supply);
		if (ret) {
			dev_err(&client->dev,
				"Failed to enable vdd regulator: %d\n", ret);
			return ret;
		}
	}

	indio_dev->channels = sx9370_channels;
	indio_dev->num_channels = ARRAY_SIZE(sx9370_channels);
	indio_dev->info = &sx9370_info;
	indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_TRIGGERED;
	dev_set_drvdata(&client->dev, indio_dev);

	data->dpr_gpio =
		devm_gpiod_get_optional(&client->dev, "dpr", GPIOD_ASIS);
	gpiod_direction_output(data->dpr_gpio, false);

	if (client->irq <= 0) {
		dev_err(&client->dev, "no valid irq found\n");
		return -EINVAL;
	}

	ret = sx9370_init_device(indio_dev);
	if (ret < 0)
		return ret;

	ret = devm_add_action_or_reset(&client->dev,
				       sx9370_cleanup_firmware_work, data);
	if (ret)
		return ret;

	ret = devm_iio_triggered_buffer_setup(&client->dev, indio_dev,
					      iio_pollfunc_store_time,
					      sx9370_trigger_handler,
					      &sx9370_buffer_setup_ops);
	if (ret < 0)
		return ret;

	data->trig = devm_iio_trigger_alloc(&client->dev, "%s-dev%d",
					    indio_dev->name,
					    iio_device_id(indio_dev));
	if (!data->trig)
		return -ENOMEM;

	data->trig->ops = &sx9370_trigger_ops;
	iio_trigger_set_drvdata(data->trig, indio_dev);

	ret = devm_iio_trigger_register(&client->dev, data->trig);
	if (ret < 0)
		return ret;

	ret = devm_request_threaded_irq(&client->dev, client->irq,
					sx9370_irq_handler,
					sx9370_irq_thread_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					indio_dev->name, indio_dev);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to request IRQ: %d\n", ret);
		return ret;
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static int sx9370_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct sx9370_data *data = iio_priv(indio_dev);
	int ret = 0;

	ret = regmap_write(data->regmap, SX9370_COMMAND, SX9370_CMD_PAUSE);
	if (ret < 0)
		dev_warn(dev, "Failed to pause device: %d\n", ret);

	disable_irq(data->client->irq);

	if (data->vdd_supply) {
		ret = regulator_disable(data->vdd_supply);
		if (ret)
			dev_warn(dev, "Failed to disable vdd in suspend: %d\n",
				 ret);
	}
	return 0;
}

static int sx9370_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct sx9370_data *data = iio_priv(indio_dev);
	int ret = 0;

	if (data->vdd_supply) {
		ret = regulator_enable(data->vdd_supply);
		if (ret) {
			dev_err(dev, "Failed to enable vdd in resume: %d\n",
				ret);
			return ret;
		}
		usleep_range(1000, 2000);
	}

	ret = regmap_write(data->regmap, SX9370_COMMAND, SX9370_CMD_UNPAUSE);
	if (ret < 0)
		dev_warn(dev, "Failed to unpause device: %d\n", ret);

	enable_irq(data->client->irq);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sx9370_pm_ops, sx9370_suspend, sx9370_resume);
static const struct of_device_id sx9370_of_match[] = {
	{ .compatible = "semtech,sx9370" },
	{}
};

MODULE_DEVICE_TABLE(of, sx9370_of_match);

static const struct i2c_device_id sx9370_id[] = {
	{ "sx9370" },
	{}
};

MODULE_DEVICE_TABLE(i2c, sx9370_id);

static struct i2c_driver sx9370_driver = {
	.driver = {
		.name = "sx9370",
		.of_match_table = sx9370_of_match,
		.pm = pm_sleep_ptr(&sx9370_pm_ops),
	},
	.probe = sx9370_probe,
	.id_table = sx9370_id,
};

module_i2c_driver(sx9370_driver);
MODULE_DESCRIPTION("SX9370 SAR Sensor Driver");
MODULE_LICENSE("GPL v2");
MODULE_FIRMWARE("sx9370.bin");
