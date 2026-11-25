/* SPDX-License-Identifier:	GPL-2.0 */
/*
 * Copyright (C) 2019 MediaTek Inc. All Rights Reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 *
 */

#include <common.h>
#include <malloc.h>
#include <net/tcp.h>
#include <net/httpd.h>
#include <u-boot/md5.h>

#include "fs.h"

static u32 upload_data_id;
static const void *upload_data;
static size_t upload_size;
static int upgrade_success;

extern int write_firmware_failsafe(size_t data_addr, uint32_t data_size);

/* MAX3：通用分区写入与切槽，实现在 board/ralink/common/cmd_max3.c */
extern int m3_flash_target(const char *name, const void *data, ulong size);
extern int m3_set_slot(const char *slot);

static int output_plain_file(struct httpd_response *response,
	const char *filename)
{
	const struct fs_desc *file;
	int ret = 0;

	file = fs_find_file(filename);

	response->status = HTTP_RESP_STD;

	if (file) {
		response->data = file->data;
		response->size = file->size;
	} else {
		response->data = "Error: file not found";
		response->size = strlen(response->data);
		ret = 1;
	}

	response->info.code = 200;
	response->info.connection_close = 1;
	response->info.content_type = "text/html";

	return ret;
}

static void index_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status == HTTP_CB_NEW)
		output_plain_file(response, "index.html");
}

static void upload_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	char *buff, *md5_ptr, *size_ptr, size_str[16];
	u8 md5_sum[16];
	struct httpd_form_value *fw;
	const struct fs_desc *file;
	int i;

	static char hexchars[] = "0123456789abcdef";

	if (status == HTTP_CB_NEW) {
		fw = httpd_request_find_value(request, "firmware");
		if (!fw) {
			response->info.code = 302;
			response->info.connection_close = 1;
			response->info.location = "/";
			return;
		}

		/* TODO: add firmware validation here if necessary */

		if (output_plain_file(response, "upload.html")) {
			response->info.code = 500;
			return;
		}

		buff = malloc(response->size + 1);
		if (buff) {
			memcpy(buff, response->data, response->size);
			buff[response->size] = 0;

			md5_ptr = strstr(buff, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
			size_ptr = strstr(buff, "YYYYYYYYYY");

			if (md5_ptr) {
				md5((u8 *) fw->data, fw->size, md5_sum);
				for (i = 0; i < 16; i++) {
					u8 hex;
					
					hex = (md5_sum[i] >> 4) & 0xf;
					md5_ptr[i * 2] = hexchars[hex];
					hex = md5_sum[i] & 0xf;
					md5_ptr[i * 2 + 1] = hexchars[hex];
				}
			}

			if (size_ptr) {
				u32 n;

				n = snprintf(size_str, sizeof(size_str), "%d",
					fw->size);
				memset(size_str + n, ' ', sizeof(size_str) - n);
				memcpy(size_ptr, size_str, 10);
			}

			response->data = buff;
		}

		upload_data_id = upload_id;
		upload_data = fw->data;
		upload_size = fw->size;

		return;
	}

	if (status == HTTP_CB_CLOSED) {
		file = fs_find_file("upload.html");

		if (file) {
			if (file->data != response->data)
				free((void *) response->data);
		}
	}
}

static void flashing_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status == HTTP_CB_NEW)
		output_plain_file(response, "flashing.html");
}

struct flashing_status {
	char buf[4096];
	int ret;
	int body_sent;
};

static void result_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	const struct fs_desc *file;
	struct flashing_status *st;
	u32 size;

	if (status == HTTP_CB_NEW) {
		st = calloc(1, sizeof(*st));
		if (!st) {
			response->info.code = 500;
			return;
		}

		st->ret = -1;

		response->session_data = st;

		response->status = HTTP_RESP_CUSTOM;

		response->info.http_1_0 = 1;
		response->info.content_length = -1;
		response->info.connection_close = 1;
		response->info.content_type = "text/html";
		response->info.code = 200;

		size = http_make_response_header(&response->info,
			st->buf, sizeof(st->buf));

		response->data = st->buf;
		response->size = size;

		return;
	}

	if (status == HTTP_CB_RESPONDING) {
		st = response->session_data;

		if (st->body_sent) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		if (upload_data_id == upload_id)
			st->ret = write_firmware_failsafe((size_t) upload_data,
				upload_size);

		/* invalidate upload identifier */
		upload_data_id = rand();

		if (!st->ret)
			file = fs_find_file("success.html");
		else
			file = fs_find_file("fail.html");

		if (!file) {
			if (!st->ret)
				response->data = "Upgrade completed!";
			else
				response->data = "Upgrade failed!";
			response->size = strlen(response->data);
			return;
		}

		response->data = file->data;
		response->size = file->size;

		st->body_sent = 1;

		return;
	}

	if (status == HTTP_CB_CLOSED) {
		st = response->session_data;

		upgrade_success = !st->ret;

		free(response->session_data);
		
		if (upgrade_success)
			tcp_close_all_conn();
	}
}

static void style_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status == HTTP_CB_NEW) {
		output_plain_file(response, "style.css");
		response->info.content_type = "text/css";
	}
}

static void not_found_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request,
	struct httpd_response *response)
{
	if (status == HTTP_CB_NEW) {
		output_plain_file(response, "404.html");
		response->info.code = 404;
	}
}

/* ---------------- MAX3：按分区刷写 + 切槽 ---------------- */

struct max3_status {
	char body[384];
	int done;
	struct httpd_form_value *tgt;
	struct httpd_form_value *slot;
	struct httpd_form_value *file;
};

static void max3_index_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request, struct httpd_response *response)
{
	if (status == HTTP_CB_NEW)
		output_plain_file(response, "max3.html");
}

/*
 * 两阶段：先回头，再在 RESPONDING 里擦写。
 * 10~96MiB 的擦写在头发出之后执行，浏览器不必等满整段。
 */
static void max3flash_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request, struct httpd_response *response)
{
	struct max3_status *st;
	struct httpd_form_value *tgt, *slot, *file;
	u32 hdr, n;
	int ret, sret;

	if (status == HTTP_CB_NEW) {
		st = calloc(1, sizeof(*st));
		if (!st) {
			response->info.code = 500;
			return;
		}

		tgt = httpd_request_find_value(request, "target");
		slot = httpd_request_find_value(request, "slot");
		file = httpd_request_find_value(request, "firmware");

		if (!tgt || !file || !file->size) {
			free(st);
			response->status = HTTP_RESP_STD;
			response->data = "missing target or file";
			response->size = strlen(response->data);
			response->info.code = 400;
			response->info.content_type = "text/html";
			response->info.connection_close = 1;
			return;
		}

		st->tgt = tgt;
		st->slot = slot;
		st->file = file;

		response->session_data = st;
		response->status = HTTP_RESP_CUSTOM;
		response->info.http_1_0 = 1;
		response->info.content_length = -1;
		response->info.connection_close = 1;
		response->info.content_type = "text/html";
		response->info.code = 200;

		hdr = http_make_response_header(&response->info, st->body,
						sizeof(st->body) - 1);
		n = snprintf(st->body + hdr, sizeof(st->body) - hdr,
			     "<p>正在写入 %s（%u 字节），请勿断电…</p>",
			     tgt->data, (u32)file->size);
		response->data = st->body;
		response->size = hdr + n;
		return;
	}

	if (status == HTTP_CB_RESPONDING) {
		st = response->session_data;
		if (!st || st->done) {
			response->status = HTTP_RESP_NONE;
			return;
		}

		ret = m3_flash_target(st->tgt->data, st->file->data,
				      st->file->size);

		sret = 0;
		if (st->slot && st->slot->data[0] &&
		    strcmp(st->slot->data, "keep"))
			sret = m3_set_slot(st->slot->data);

		snprintf(st->body, sizeof(st->body),
			 "<p>目标 <b>%s</b>：%u 字节，<b>%s</b></p>"
			 "%s%s"
			 "<p><a href=\"/max3\">返回</a></p>",
			 st->tgt->data, (u32)st->file->size,
			 ret ? "失败" : "擦写并回读校验通过",
			 (st->slot && st->slot->data[0] &&
			  strcmp(st->slot->data, "keep")) ?
			 (sret ? "<p>boot_slot 写入失败</p>"
			       : "<p>boot_slot 已 saveenv</p>") : "",
			 ret ? "<p>该分区内容不可信，请重刷</p>" : "");

		response->data = st->body;
		response->size = strlen(st->body);
		st->done = 1;
		return;
	}

	if (status == HTTP_CB_CLOSED) {
		free(response->session_data);
		response->session_data = NULL;
	}
}

static void max3slot_handler(enum httpd_uri_handler_status status,
	struct httpd_request *request, struct httpd_response *response)
{
	static char msg[192];
	struct httpd_form_value *v;
	int ret;

	if (status != HTTP_CB_NEW)
		return;

	v = httpd_request_find_value(request, "slot");
	if (!v) {
		response->info.code = 400;
		snprintf(msg, sizeof(msg),
			 "<p>缺 slot 字段</p><p><a href=\"/max3\">返回</a></p>");
	} else {
		ret = m3_set_slot(v->data);
		response->info.code = 200;
		snprintf(msg, sizeof(msg),
			 "<p>boot_slot = <b>%s</b>：%s</p>"
			 "<p><a href=\"/max3\">返回</a></p>",
			 v->data,
			 ret ? "失败（saveenv 不可用？）" : "已 saveenv，重启生效");
	}

	response->status = HTTP_RESP_STD;
	response->data = msg;
	response->size = strlen(msg);
	response->info.content_type = "text/html";
	response->info.connection_close = 1;
}

int start_web_failsafe(void)
{
	struct httpd_instance *inst;

	inst = httpd_find_instance(80);
	if (inst)
		httpd_free_instance(inst);

	inst = httpd_create_instance(80);
	if (!inst) {
		printf("Error: failed to create HTTP instance on port 80\n");
		return -1;
	}

	httpd_register_uri_handler(inst, "/", &index_handler, NULL);
	httpd_register_uri_handler(inst, "/cgi-bin/luci", &index_handler, NULL);
	httpd_register_uri_handler(inst, "/upload", &upload_handler, NULL);
	httpd_register_uri_handler(inst, "/flashing", &flashing_handler, NULL);
	httpd_register_uri_handler(inst, "/result", &result_handler, NULL);
	httpd_register_uri_handler(inst, "/style.css", &style_handler, NULL);
	httpd_register_uri_handler(inst, "/max3", &max3_index_handler, NULL);
	httpd_register_uri_handler(inst, "/max3flash", &max3flash_handler, NULL);
	httpd_register_uri_handler(inst, "/max3slot", &max3slot_handler, NULL);
	httpd_register_uri_handler(inst, "", &not_found_handler, NULL);

	net_loop(TCP);

	return 0;
}

static int do_httpd(cmd_tbl_t *cmdtp, int flag, int argc,
	char *const argv[])
{
	int ret;

	printf("\nWeb failsafe UI started\n");
	
	ret = start_web_failsafe();

	if (upgrade_success)
		do_reset(NULL, 0, 0, NULL);

	return ret;
}

U_BOOT_CMD(httpd, 1, 0, do_httpd,
	"Start failsafe HTTP server", ""
);
