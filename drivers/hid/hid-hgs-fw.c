// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Leica Geosystems AG <https://leica-geosystems.com>
/*
 * HID driver for Leica KDU/KU devices with firmware update support
 */

#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/delay.h>
#include "./i2c-hid/i2c-hid.h"
#include "hid-hgs.h"

#define KDU_FW_STATE_OK 0x00
#define KDU_FW_STATE_FINISH 0x01
#define KDU_FW_STATE_PROTOCOL_ERROR 0x02

#define MAX_FW_OUTPUT_REPORT_SIZE 64
#define VERSION_LENGTH 5
#define MATERIAL_INFO_LENGTH 10
#define BOARD_INFO_LENGTH 30
#define TRACE_ID_LENGTH 30
#define TRACE_ID_HEX_LENGTH 80

static int hgs_send_fw_output_report(struct hgs_ctx *ctx, u32 stream_cmd_val, const u8 *data)
{
	u8 send_block_buf[MAX_FW_OUTPUT_REPORT_SIZE];
	struct device *dev = &ctx->hid->dev;
	struct hgs_fw_data *fw = &ctx->fw;
	u32 report_id = fw->output_report->id;
	size_t current_offset = 0;
	u32 stream_size_field_len;
	u32 data_block_field_len;
	u32 stream_cmd_verified;
	int ret;

	data_block_field_len =
		fw->data_block_field->report_count * (fw->data_block_field->report_size >> 3);
	stream_size_field_len =
		fw->stream_size_field->report_count * (fw->stream_size_field->report_size >> 3);

	if (fw->is_stream_cmd_little_endian)
		stream_cmd_verified = cpu_to_le32(stream_cmd_val);
	else
		stream_cmd_verified = cpu_to_be32(stream_cmd_val);

	send_block_buf[current_offset++] = (u8)report_id;

	memcpy(send_block_buf + current_offset, &stream_cmd_verified, stream_size_field_len);
	current_offset += stream_size_field_len;

	if (data)
		memcpy(send_block_buf + current_offset, data, data_block_field_len);

	current_offset += data_block_field_len;

	ret = hid_hw_output_report(ctx->hid, send_block_buf, current_offset);
	if (ret < 0) {
		dev_err(dev, "hid_hw_output_report failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int hgs_read_fw_state(struct hgs_ctx *ctx, u8 *state)
{
	unsigned long timeout = msecs_to_jiffies(6000);
	struct hgs_fw_data *fw = &ctx->fw;
	struct device *dev = &ctx->hid->dev;
	unsigned long flags;
	int ret = 0;

	if (!wait_for_completion_timeout(&fw->fw_state_received, timeout)) {
		dev_err(dev, "Timeout waiting for FW state update (6s)\n");
		return -ETIMEDOUT;
	}

	spin_lock_irqsave(&fw->state_lock, flags);
	if (fw->fw_state_updated) {
		*state = fw->last_fw_state;
		fw->fw_state_updated = false;
	} else {
		dev_err(dev, "FW state completion signaled, but state wrong\n");
		ret = -EIO;
	}
	spin_unlock_irqrestore(&fw->state_lock, flags);

	if (ret == 0)
		reinit_completion(&fw->fw_state_received);

	return ret;
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_fw_data *fw = &ctx->fw;
	u8 ver_major = 0, ver_minor = 0;
	unsigned int report_id;
	u16 ver_build = 0;
	int ret;
	/*
	 * Temporary buffer for the raw feature report.
	 * Max size: Report ID (1) + Major (1) + Minor (1) + Build (2) = 5 bytes
	 * hid_hw_raw_request for GET_REPORT on a numbered report expects
	 * the first byte of 'raw_buf' to be the report ID.
	 * The returned data in 'raw_buf' will then start with the report ID.
	 */
	u8 raw_buf[VERSION_LENGTH];

	if (!fw->version_feature_report) {
		dev_warn(dev, "Version report (ID %d) not available\n", report_id);
		return scnprintf(buf, PAGE_SIZE, "N/A (Version Report not found)\n");
	}

	report_id = fw->version_feature_report->id;

	mutex_lock(&ctx->lock);

	raw_buf[0] = report_id;
	ret = hid_hw_raw_request(ctx->hid, report_id, raw_buf, sizeof(raw_buf), HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		dev_err(dev, "Failed to get firmware version report: %d\n", ret);
		mutex_unlock(&ctx->lock);
		return ret;
	}

	if (ret != sizeof(raw_buf) || raw_buf[0] != report_id) {
		dev_err(dev, "Invalid version report received. Len: %d, ID: 0x%02x\n", ret,
			raw_buf[0]);
		mutex_unlock(&ctx->lock);
		return -EIO;
	}

	ver_major = raw_buf[1];
	ver_minor = raw_buf[2];
	ver_build = ((u16)raw_buf[4] << 8) | raw_buf[3];

	mutex_unlock(&ctx->lock);

	return scnprintf(buf, PAGE_SIZE, "%u.%u.%u\n", ver_major, ver_minor, ver_build);
}
static DEVICE_ATTR_RO(version);

static ssize_t material_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_fw_data *fw = &ctx->fw;
	// Report ID (1) + Material Number (7) + Material Revision (2) = 10 bytes
	u8 raw_buf[MATERIAL_INFO_LENGTH];
	unsigned int report_id;
	int len = 0;
	int ret;
	/*
	 * Material Number: 7 bytes at raw_buf[1]
	 * Material Revision: 2 bytes at raw_buf[8]
	 * For example: "1008531_A"
	 * Material Number (7) + Revision (2) = 9 add EOF = 10
	 */
	char combined_str[11];
	int c_off = 0;

	if (!fw->material_info_feature_report) {
		dev_warn(dev, "Material Info feature report not available\n");
		return scnprintf(buf, PAGE_SIZE, "(Material Info Report not found)\n");
	}

	report_id = fw->material_info_feature_report->id;
	dev_info(dev, "Reading Material Info from feature report ID %u\n", report_id);

	mutex_lock(&ctx->lock);

	raw_buf[0] = report_id;

	ret = hid_hw_raw_request(ctx->hid, report_id, raw_buf, sizeof(raw_buf), HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		dev_err(dev, "Failed to get material info report (ID %u) %d\n", report_id, ret);
		mutex_unlock(&ctx->lock);
		return ret;
	}

	if (ret != sizeof(raw_buf) || raw_buf[0] != report_id) {
		dev_err(dev, "Invalid material info: id=%u got=0x%02x len=%d/%zu\n", report_id,
			raw_buf[0], ret, sizeof(raw_buf));
		mutex_unlock(&ctx->lock);
		return -EIO;
	}

	mutex_unlock(&ctx->lock);

	memcpy(combined_str + c_off, &raw_buf[1], 7); // Material Number
	c_off += 7;
	memcpy(combined_str + c_off, &raw_buf[8], 2); // Material Revision
	c_off += 2;
	combined_str[c_off] = '\0'; // Null terminate

	len = scnprintf(buf, PAGE_SIZE, "%s\n", combined_str);

	return len;
}
static DEVICE_ATTR_RO(material_info);

static ssize_t board_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_fw_data *fw = &ctx->fw;
	u8 raw_buf[BOARD_INFO_LENGTH];
	unsigned int report_id;
	int len = 0;
	int ret;

	char combined_str[30];
	int c_off = 0;

	if (!fw->board_info_feature_report) {
		dev_warn(dev, "Board Info feature report not available\n");
		return scnprintf(buf, PAGE_SIZE, "(Board Info Report not found)\n");
	}

	report_id = fw->board_info_feature_report->id;
	dev_info(dev, "Reading Board Info from feature report ID %u\n", report_id);

	mutex_lock(&ctx->lock);

	raw_buf[0] = report_id;

	ret = hid_hw_raw_request(ctx->hid, report_id, raw_buf, sizeof(raw_buf), HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		dev_err(dev, "Failed to get board info feature report (ID %u): %d\n", report_id,
			ret);
		mutex_unlock(&ctx->lock);
		return ret;
	}

	if (ret != sizeof(raw_buf) || raw_buf[0] != report_id) {
		dev_err(dev,
			"Invalid board info. Expected %u,Got ID 0x%02x, Len: %d (expected %zu)\n",
			report_id, raw_buf[0], ret, sizeof(raw_buf));
		mutex_unlock(&ctx->lock);
		return -EIO;
	}

	mutex_unlock(&ctx->lock);

	memcpy(combined_str + c_off, &raw_buf[1], 7);
	c_off += 7;
	memcpy(combined_str + c_off, &raw_buf[8], 2);
	c_off += 2;
	memcpy(combined_str + c_off, &raw_buf[10], 7);
	c_off += 7;
	memcpy(combined_str + c_off, &raw_buf[17], 8);
	c_off += 8;
	memcpy(combined_str + c_off, &raw_buf[25], 5);
	c_off += 5;
	combined_str[c_off] = '\0';
	len = scnprintf(buf, PAGE_SIZE, "%s\n", combined_str);

	return len;
}
static DEVICE_ATTR_RO(board_id);

static ssize_t trace_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_fw_data *fw = &ctx->fw;
	char hex_str_buf[TRACE_ID_HEX_LENGTH];
	u8 raw_buf[TRACE_ID_LENGTH];
	unsigned int report_id;
	int data_len;
	int hex_idx = 0;
	int len = 0;
	int ret;

	if (!fw->trace_id_feature_report) {
		dev_warn(dev, "Board Info feature report (ID 7) not available\n");
		return scnprintf(buf, PAGE_SIZE, "(Board Info Report not found)\n");
	}

	report_id = fw->trace_id_feature_report->id;
	dev_info(dev, "Reading Trace Info from feature report ID %u\n", report_id);

	mutex_lock(&ctx->lock);

	raw_buf[0] = report_id;

	ret = hid_hw_raw_request(ctx->hid, report_id, raw_buf, sizeof(raw_buf), HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		dev_err(dev, "Failed to get board info feature report (ID %u): %d\n", report_id,
			ret);
		mutex_unlock(&ctx->lock);
		return ret;
	}

	if (ret != sizeof(raw_buf) || raw_buf[0] != report_id) {
		dev_err(dev,
			"Invalid board info. Expected %u, Got 0x%02x, Len: %d (expected %zu)\n",
			report_id, raw_buf[0], ret, sizeof(raw_buf));
		mutex_unlock(&ctx->lock);
		return -EIO;
	}

	mutex_unlock(&ctx->lock);

	data_len = ret - 1;
	if (data_len <= 0)
		return scnprintf(buf, PAGE_SIZE, "No data in Trace ID report\n");

	for (int i = 0; i < data_len; i++) {
		if (hex_idx >= (sizeof(hex_str_buf) - 3))
			break;
		hex_idx += scnprintf(hex_str_buf + hex_idx, sizeof(hex_str_buf) - hex_idx, "%02x",
				     raw_buf[i + 1]);
	}
	hex_str_buf[hex_idx] = '\0';

	len = scnprintf(buf, PAGE_SIZE, "%s\n", hex_str_buf);

	return len;
}
static DEVICE_ATTR_RO(trace_id);

static ssize_t stream_cmd_endianness_show(struct device *dev, struct device_attribute *attr,
					  char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_fw_data *fw = &ctx->fw;
	ssize_t len;

	mutex_lock(&ctx->lock);

	len = scnprintf(buf, PAGE_SIZE, "%s\n", fw->is_stream_cmd_little_endian ? "little" : "big");

	mutex_unlock(&ctx->lock);
	return len;
}

static ssize_t stream_cmd_endianness_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct hgs_ctx *ctx = hid_get_drvdata(hdev);
	struct hgs_fw_data *fw = &ctx->fw;
	bool new_val;

	if (sysfs_streq(buf, "little")) {
		new_val = true;
	} else if (sysfs_streq(buf, "big")) {
		new_val = false;
	} else {
		dev_warn(dev, "Invalid endianness value. Use 'big' or 'little'\n");
		return -EINVAL;
	}

	mutex_lock(&ctx->lock);

	if (fw->is_stream_cmd_little_endian != new_val) {
		fw->is_stream_cmd_little_endian = new_val;
		dev_info(dev, "Stream command endianness set to: %s\n",
			 fw->is_stream_cmd_little_endian ? "little" : "big");
	}

	mutex_unlock(&ctx->lock);
	return count;
}
static DEVICE_ATTR_RW(stream_cmd_endianness);

static struct attribute *hgs_fw_attrs[] = {
	&dev_attr_version.attr,	 &dev_attr_material_info.attr,	       &dev_attr_board_id.attr,
	&dev_attr_trace_id.attr, &dev_attr_stream_cmd_endianness.attr, NULL,
};

static const struct attribute_group hgs_fw_attr_group = {
	.name = NULL,
	.attrs = hgs_fw_attrs,
};

static enum fw_upload_err hgs_fw_upload_prepare(struct fw_upload *fw_upload,
						const u8 *fw_image_data, u32 total_fw_size)
{
	struct hgs_ctx *ctx = fw_upload->dd_handle;
	struct hgs_fw_data *fw = &ctx->fw;
	enum fw_upload_err ret = FW_UPLOAD_ERR_NONE;
	struct device *dev = &ctx->hid->dev;
	unsigned long flags;

	dev_info(dev, "fwl_prepare: Preparing firmware upload\n");
	mutex_lock(&fw->cancel_lock);
	fw->cancel = false;
	mutex_unlock(&fw->cancel_lock);

	mutex_lock(&ctx->lock);

	if (!fw->output_report || !fw->data_block_field || !fw->stream_size_field ||
	    !fw->input_report) {
		dev_err(dev, "Output/Input report or fields not configured\n");
		ret = FW_UPLOAD_ERR_HW_ERROR;
		goto out_unlock;
	}

	if (total_fw_size == 0) {
		dev_err(dev, "Firmware size is 0\n");
		ret = FW_UPLOAD_ERR_INVALID_SIZE;
		goto out_unlock;
	}

	spin_lock_irqsave(&fw->state_lock, flags);
	fw->fw_state_updated = false;
	spin_unlock_irqrestore(&fw->state_lock, flags);
	reinit_completion(&fw->fw_state_received);

	mutex_lock(&fw->cancel_lock);
	ret = fw->cancel ? FW_UPLOAD_ERR_CANCELED : FW_UPLOAD_ERR_NONE;
	mutex_unlock(&fw->cancel_lock);

out_unlock:
	if (ret != FW_UPLOAD_ERR_NONE)
		mutex_unlock(&ctx->lock);

	return ret;
}

static enum fw_upload_err hgs_fw_upload_write(struct fw_upload *fw_upload, const u8 *fw_image_data,
					      u32 offset, u32 remaining_size, u32 *written)
{
	struct hgs_ctx *ctx = fw_upload->dd_handle;
	struct hgs_fw_data *fw = &ctx->fw;
	enum fw_upload_err fwl_ret = FW_UPLOAD_ERR_NONE;
	u8 data_chunk[MAX_FW_OUTPUT_REPORT_SIZE];
	struct device *dev = &ctx->hid->dev;
	u32 data_block_field_len;
	u8 current_state;
	bool cancel;
	int ret;

	mutex_lock(&fw->cancel_lock);
	cancel = fw->cancel;
	mutex_unlock(&fw->cancel_lock);

	if (cancel)
		return FW_UPLOAD_ERR_CANCELED;

	data_block_field_len =
		fw->data_block_field->report_count * (fw->data_block_field->report_size >> 3);

	size_t len = min_t(size_t, data_block_field_len, remaining_size);

	memcpy(data_chunk, fw_image_data + offset, len);

	if (len < data_block_field_len) // Zero-pad if last chunk is smaller
		memset(data_chunk + len, 0, data_block_field_len - len);

	/*
	 * For data packets, the 'stream_size_field' is remaining size,
	 * and a generic "data" command.
	 */
	ret = hgs_send_fw_output_report(ctx, (u32)remaining_size, data_chunk);
	if (ret) {
		dev_err(dev, "Failed to send data chunk at offset %u: %d\n", offset, ret);
		fwl_ret = FW_UPLOAD_ERR_HW_ERROR;
		goto out;
	}

	ret = hgs_read_fw_state(ctx, &current_state);
	if (ret) {
		dev_err(dev, "Failed to read FW state after sending chunk at offset %u: %d\n",
			offset, ret);
		fwl_ret = FW_UPLOAD_ERR_TIMEOUT;
		goto out;
	}

	if (remaining_size > data_block_field_len && current_state != KDU_FW_STATE_OK) {
		dev_err(dev, "Bad Status: 0x%02x\n", current_state);
		fwl_ret = FW_UPLOAD_ERR_HW_ERROR;
		goto out;
	}

	if (remaining_size > data_block_field_len && current_state == KDU_FW_STATE_FINISH) {
		dev_err(dev, "Unexpected update done, but data remaining\n");
		fwl_ret = FW_UPLOAD_ERR_HW_ERROR;
		goto out;
	}

	if (remaining_size <= data_block_field_len && current_state == KDU_FW_STATE_FINISH)
		dev_info(dev, "Firmware update completed successfully\n");

out:
	*written = len;

	return fwl_ret;
}

static enum fw_upload_err hgs_fw_upload_poll_complete(struct fw_upload *fw_upload)
{
	return FW_UPLOAD_ERR_NONE;
}

static void hgs_fw_upload_cancel(struct fw_upload *fw_upload)
{
	struct hgs_ctx *ctx = fw_upload->dd_handle;
	struct hgs_fw_data *fw = &ctx->fw;
	struct device *dev = &ctx->hid->dev;

	dev_info(dev, "fwl_cancel: Firmware upload cancelled by user\n");

	mutex_lock(&fw->cancel_lock);
	fw->cancel = true;
	mutex_unlock(&fw->cancel_lock);
}

static void hgs_fw_upload_cleanup(struct fw_upload *fw_upload)
{
	struct hgs_ctx *ctx = fw_upload->dd_handle;
	struct device *dev = &ctx->hid->dev;
	dev_info(dev, "waiting for HID client reset\n");
	i2c_hid_wait_reset_complete(ctx->hid->dev.parent, 10000);
	mutex_unlock(&ctx->lock);
	hid_driver_reset_resume(ctx->hid);
	dev_info(dev, "fwl_cleanup: Cleaning up firmware upload state\n");
}

static const struct fw_upload_ops hgs_fw_upload_ops = {
	.prepare = hgs_fw_upload_prepare,
	.write = hgs_fw_upload_write,
	.poll_complete = hgs_fw_upload_poll_complete,
	.cancel = hgs_fw_upload_cancel,
	.cleanup = hgs_fw_upload_cleanup,
};

int hgs_fw_init(struct hgs_ctx *ctx)
{
	struct hid_device *hdev = ctx->hid;
	struct hgs_fw_data *fw = &ctx->fw;
	struct device *dev = &hdev->dev;
	char *fw_upload_name = NULL;
	int ret;

	/* Default to little-endian for stream commands*/
	fw->is_stream_cmd_little_endian = true;

	mutex_init(&ctx->lock);
	mutex_init(&fw->cancel_lock);
	spin_lock_init(&fw->state_lock);
	init_completion(&fw->fw_state_received);
	fw->fw_state_updated = false;

	ret = sysfs_create_group(&dev->kobj, &hgs_fw_attr_group);
	if (ret) {
		dev_err(dev, "Failed to create firmware sysfs group: %d\n", ret);
		mutex_destroy(&ctx->lock);
		return ret;
	}

	if (!IS_ENABLED(CONFIG_FW_UPLOAD)) {
		dev_dbg(dev, "kdu update disabled\n");
		return 0;
	}

	fw_upload_name = kasprintf(GFP_KERNEL, "hidi2c:%s", dev_name(dev));
	if (!fw_upload_name) {
		ret = -ENOMEM;
		goto err;
	}

	fw->fwl =
		firmware_upload_register(THIS_MODULE, dev, fw_upload_name, &hgs_fw_upload_ops, ctx);
	kfree(fw_upload_name);
	if (IS_ERR(fw->fwl)) {
		ret = PTR_ERR(fw->fwl);
		fw->fwl = NULL; // Ensure fwl is NULL on error
		dev_err(dev, "Failed to register firmware upload: %d\n", ret);
		goto err;
	}

	dev_info(dev, "HGS KDU Firmware/Info interface initialized\n");
	return 0;

err:
	sysfs_remove_group(&dev->kobj, &hgs_fw_attr_group);
	mutex_destroy(&ctx->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(hgs_fw_init);

void hgs_fw_exit(struct hgs_ctx *ctx)
{
	struct hgs_fw_data *fw = &ctx->fw;
	struct device *dev = &ctx->hid->dev;

	if (IS_ENABLED(CONFIG_FW_UPLOAD) && fw->fwl) {
		firmware_upload_unregister(fw->fwl);
		fw->fwl = NULL;
	}

	sysfs_remove_group(&ctx->hid->dev.kobj, &hgs_fw_attr_group);
	mutex_destroy(&ctx->lock);
	dev_info(dev, "HGS KDU Firmware interface removed\n");
}
EXPORT_SYMBOL_GPL(hgs_fw_exit);
