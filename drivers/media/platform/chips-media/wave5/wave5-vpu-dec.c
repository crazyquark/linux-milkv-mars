// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave5 series multi-standard codec IP - decoder interface
 *
 * Copyright (C) 2021 CHIPS&MEDIA INC
 */

#include "wave5-helper.h"

#define VPU_DEC_DEV_NAME "C&M Wave5 VPU decoder"
#define VPU_DEC_DRV_NAME "wave5-dec"
#define V4L2_CID_VPU_THUMBNAIL_MODE (V4L2_CID_USER_BASE + 0x1001)

static const struct vpu_format dec_fmt_list[FMT_TYPES][MAX_FMTS] = {
	[VPU_FMT_TYPE_CODEC] = {
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_HEVC,
			.max_width = 8192,
			.min_width = 8,
			.max_height = 4320,
			.min_height = 8,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_H264,
			.max_width = 8192,
			.min_width = 32,
			.max_height = 4320,
			.min_height = 32,
		},
	},
	[VPU_FMT_TYPE_RAW] = {
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV420,
			.max_width = 8192,
			.min_width = 8,
			.max_height = 4320,
			.min_height = 8,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV12,
			.max_width = 8192,
			.min_width = 8,
			.max_height = 4320,
			.min_height = 8,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV21,
			.max_width = 8192,
			.min_width = 8,
			.max_height = 4320,
			.min_height = 8,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_YUV420M,
			.max_width = 8192,
			.min_width = 8,
			.max_height = 4320,
			.min_height = 8,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV12M,
			.max_width = 8192,
			.min_width = 8,
			.max_height = 4320,
			.min_height = 8,
		},
		{
			.v4l2_pix_fmt = V4L2_PIX_FMT_NV21M,
			.max_width = 8192,
			.min_width = 8,
			.max_height = 4320,
			.min_height = 8,
		},
	}
};

static enum wave_std wave5_to_vpu_codstd(unsigned int v4l2_pix_fmt)
{
	switch (v4l2_pix_fmt) {
	case V4L2_PIX_FMT_H264:
		return W_AVC_DEC;
	case V4L2_PIX_FMT_HEVC:
		return W_HEVC_DEC;
	default:
		return STD_UNKNOWN;
	}
}

static void wave5_handle_bitstream_buffer(struct vpu_instance *inst)
{
	struct v4l2_m2m_buffer *buf, *n;
	int ret;

	v4l2_m2m_for_each_src_buf_safe(inst->v4l2_fh.m2m_ctx, buf, n) {
		struct vb2_v4l2_buffer *vbuf = &buf->vb;
		struct vpu_buffer *vpu_buf = wave5_to_vpu_buf(vbuf);
		size_t src_size = vb2_get_plane_payload(&vbuf->vb2_buf, 0);
		void *src_buf = vb2_plane_vaddr(&vbuf->vb2_buf, 0);
		dma_addr_t rd_ptr = 0;
		dma_addr_t wr_ptr = 0;
		size_t remain_size = 0;
		size_t offset;

		if (src_size == vb2_plane_size(&vbuf->vb2_buf, 0))
			src_size = 0;

		if (vpu_buf->consumed) {
			dev_dbg(inst->dev->dev, "already consumed src buf (%u)\n",
				vbuf->vb2_buf.index);
			continue;
		}

		if (!src_buf) {
			dev_dbg(inst->dev->dev,
				"%s: Acquiring kernel pointer to src buf (%u), fail\n",
				__func__, vbuf->vb2_buf.index);
			break;
		}

		ret = wave5_vpu_dec_get_bitstream_buffer(inst, &rd_ptr, &wr_ptr, &remain_size);
		if (ret) {
			dev_err(inst->dev->dev, "Getting the bitstream buffer, fail: %d\n",
				ret);
			return;
		}

		if (remain_size < src_size) {
			dev_dbg(inst->dev->dev,
				"%s: remaining size: %zu < source size: %zu for src buf (%u)\n",
				__func__, remain_size, src_size, vbuf->vb2_buf.index);
			break;
		}

		offset = wr_ptr - inst->bitstream_vbuf.daddr;
		if (wr_ptr + src_size > inst->bitstream_vbuf.daddr + inst->bitstream_vbuf.size) {
			size_t size;

			size = inst->bitstream_vbuf.daddr + inst->bitstream_vbuf.size - wr_ptr;
			ret = wave5_vdi_write_memory(inst->dev, &inst->bitstream_vbuf, offset,
						     (u8 *)src_buf, size, VDI_128BIT_LITTLE_ENDIAN);
			if (ret < 0) {
				dev_dbg(inst->dev->dev,
					"%s: 1/2 write src buf (%u) into bitstream buf, fail: %d\n",
					__func__, vbuf->vb2_buf.index, ret);
				break;
			}
			ret = wave5_vdi_write_memory(inst->dev, &inst->bitstream_vbuf, 0,
						     (u8 *)src_buf + size, src_size - size,
						     VDI_128BIT_LITTLE_ENDIAN);
			if (ret < 0) {
				dev_dbg(inst->dev->dev,
					"%s: 2/2 write src buf (%u) into bitstream buf, fail: %d\n",
					__func__, vbuf->vb2_buf.index, ret);
				break;
			}
		} else {
			ret = wave5_vdi_write_memory(inst->dev, &inst->bitstream_vbuf, offset,
						     (u8 *)src_buf, src_size,
						     VDI_128BIT_LITTLE_ENDIAN);
			if (ret < 0) {
				dev_dbg(inst->dev->dev,
					"%s: write src buf (%u) into bitstream buf, fail: %d",
					__func__, vbuf->vb2_buf.index, ret);
				break;
			}
		}

		ret = wave5_vpu_dec_update_bitstream_buffer(inst, src_size);
		if (ret) {
			dev_dbg(inst->dev->dev,
				"vpu_dec_update_bitstream_buffer fail: %d for src buf (%u)\n",
				ret, vbuf->vb2_buf.index);
			break;
		}

		vpu_buf->consumed = true;
	}
}

static void wave5_handle_src_buffer(struct vpu_instance *inst)
{
	struct vb2_v4l2_buffer *src_buf;

	src_buf = v4l2_m2m_next_src_buf(inst->v4l2_fh.m2m_ctx);
	if (src_buf) {
		struct vpu_buffer *vpu_buf = wave5_to_vpu_buf(src_buf);

		if (vpu_buf->consumed) {
			dev_dbg(inst->dev->dev, "%s: already consumed buffer\n", __func__);
			src_buf = v4l2_m2m_src_buf_remove(inst->v4l2_fh.m2m_ctx);
			inst->timestamp = src_buf->vb2_buf.timestamp;
			v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
		}
	}
}

static void wave5_update_pix_fmt(struct v4l2_pix_format_mplane *pix_mp, unsigned int width,
				 unsigned int height)
{
	switch (pix_mp->pixelformat) {
	case V4L2_PIX_FMT_YUV420:
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
		pix_mp->width = round_up(width, 32);
		pix_mp->height = round_up(height, 8);
		pix_mp->plane_fmt[0].bytesperline = round_up(width, 32);
		pix_mp->plane_fmt[0].sizeimage = width * height * 3 / 2;
		break;
	case V4L2_PIX_FMT_YUV420M:
		pix_mp->width = round_up(width, 32);
		pix_mp->height = round_up(height, 8);
		pix_mp->plane_fmt[0].bytesperline = round_up(width, 32);
		pix_mp->plane_fmt[0].sizeimage = width * height;
		pix_mp->plane_fmt[1].bytesperline = round_up(width, 32) / 2;
		pix_mp->plane_fmt[1].sizeimage = width * height / 4;
		pix_mp->plane_fmt[2].bytesperline = round_up(width, 32) / 2;
		pix_mp->plane_fmt[2].sizeimage = width * height / 4;
		break;
	case V4L2_PIX_FMT_NV12M:
	case V4L2_PIX_FMT_NV21M:
		pix_mp->width = round_up(width, 32);
		pix_mp->height = round_up(height, 8);
		pix_mp->plane_fmt[0].bytesperline = round_up(width, 32);
		pix_mp->plane_fmt[0].sizeimage = width * height;
		pix_mp->plane_fmt[1].bytesperline = round_up(width, 32);
		pix_mp->plane_fmt[1].sizeimage = width * height / 2;
		break;
	default:
		pix_mp->width = width;
		pix_mp->height = height;
		pix_mp->plane_fmt[0].bytesperline = 0;
		pix_mp->plane_fmt[0].sizeimage = width * height;
		break;
	}
}

static void wave5_vpu_dec_start_decode(struct vpu_instance *inst)
{
	struct dec_param pic_param;
	int ret;
	u32 fail_res = 0;

	memset(&pic_param, 0, sizeof(struct dec_param));

	if (inst->state == VPU_INST_STATE_INIT_SEQ) {
		u32 non_linear_num = inst->dst_buf_count;
		u32 linear_num = inst->dst_buf_count;
		u32 stride = inst->dst_fmt.width;

		ret = wave5_vpu_dec_register_frame_buffer_ex(inst, non_linear_num, linear_num,
							     stride, inst->dst_fmt.height,
							     COMPRESSED_FRAME_MAP);
		if (ret)
			dev_dbg(inst->dev->dev, "%s: vpu_dec_register_frame_buffer_ex fail: %d",
				__func__, ret);
	}

	ret = wave5_vpu_dec_start_one_frame(inst, &pic_param, &fail_res);
	if (ret && fail_res != WAVE5_SYSERR_QUEUEING_FAIL) {
		struct vb2_v4l2_buffer *src_buf;

		src_buf = v4l2_m2m_src_buf_remove(inst->v4l2_fh.m2m_ctx);
		inst->state = VPU_INST_STATE_STOP;
		v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
	}
}

static void wave5_vpu_dec_stop_decode(struct vpu_instance *inst)
{
	unsigned int i;
	int ret;

	inst->state = VPU_INST_STATE_STOP;

	ret = wave5_vpu_dec_update_bitstream_buffer(inst, 0);
	if (ret) {
		dev_warn(inst->dev->dev,
			 "Setting EOS for the bitstream, fail: %d\n", ret);
	}

	for (i = 0; i < inst->dst_buf_count; i++) {
		ret = wave5_vpu_dec_clr_disp_flag(inst, i);
		if (ret) {
			dev_dbg(inst->dev->dev,
				"%s: Clearing the display flag of buffer index: %u, fail: %d\n",
				__func__, i, ret);
		}
	}

	v4l2_m2m_job_finish(inst->v4l2_m2m_dev, inst->v4l2_fh.m2m_ctx);
}

static void wave5_vpu_dec_finish_decode(struct vpu_instance *inst)
{
	struct dec_output_info dec_output_info;
	int ret;
	u32 irq_status;

	if (kfifo_out(&inst->irq_status, &irq_status, sizeof(int)))
		wave5_vpu_clear_interrupt_ex(inst, irq_status);

	ret = wave5_vpu_dec_get_output_info(inst, &dec_output_info);
	if (ret) {
		v4l2_m2m_job_finish(inst->v4l2_m2m_dev, inst->v4l2_fh.m2m_ctx);
		return;
	}
	if (dec_output_info.index_frame_decoded == DECODED_IDX_FLAG_NO_FB &&
	    dec_output_info.index_frame_display == DISPLAY_IDX_FLAG_NO_FB) {
		dev_dbg(inst->dev->dev, "%s: no more frame buffer\n", __func__);
	} else {
		wave5_handle_src_buffer(inst);

		if (dec_output_info.index_frame_display >= 0) {
			struct vb2_v4l2_buffer *dst_buf =
				v4l2_m2m_dst_buf_remove_by_idx(inst->v4l2_fh.m2m_ctx,
							       dec_output_info.index_frame_display);
			int stride = dec_output_info.disp_frame.stride;
			int height = dec_output_info.disp_pic_height -
				dec_output_info.rc_display.bottom;
			if (dec_output_info.disp_pic_height != inst->display_fmt.height)
				height = inst->display_fmt.height;
			dev_dbg(inst->dev->dev, "%s %d disp_pic_height %u rc_display.bottom %u\n",
				__func__, __LINE__, dec_output_info.disp_pic_height, dec_output_info.rc_display.bottom);
			dev_dbg(inst->dev->dev, "%s %d stride %u height %u\n", __func__, __LINE__, stride, height);

			if (inst->dst_fmt.num_planes == 1) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0,
						      (stride * height * 3 / 2));
			} else if (inst->dst_fmt.num_planes == 2) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0,
						      (stride * height));
				vb2_set_plane_payload(&dst_buf->vb2_buf, 1,
						      ((stride / 2) * height));
			} else if (inst->dst_fmt.num_planes == 3) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0,
						      (stride * height));
				vb2_set_plane_payload(&dst_buf->vb2_buf, 1,
						      ((stride / 2) * (height / 2)));
				vb2_set_plane_payload(&dst_buf->vb2_buf, 2,
						      ((stride / 2) * (height / 2)));
			}

			dst_buf->vb2_buf.timestamp = inst->timestamp;
			dst_buf->field = V4L2_FIELD_NONE;
			v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);

			dev_dbg(inst->dev->dev, "%s: frame_cycle %8u\n",
				__func__, dec_output_info.frame_cycle);
		} else if (dec_output_info.index_frame_display == DISPLAY_IDX_FLAG_SEQ_END &&
			   !inst->eos) {
			static const struct v4l2_event vpu_event_eos = {
				.type = V4L2_EVENT_EOS
			};
			struct vb2_v4l2_buffer *dst_buf =
				v4l2_m2m_dst_buf_remove(inst->v4l2_fh.m2m_ctx);

			if (!dst_buf)
				return;

			if (inst->dst_fmt.num_planes == 1) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0,
						      vb2_plane_size(&dst_buf->vb2_buf, 0));
			} else if (inst->dst_fmt.num_planes == 2) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0,
						      vb2_plane_size(&dst_buf->vb2_buf, 0));
				vb2_set_plane_payload(&dst_buf->vb2_buf, 1,
						      vb2_plane_size(&dst_buf->vb2_buf, 1));
			} else if (inst->dst_fmt.num_planes == 3) {
				vb2_set_plane_payload(&dst_buf->vb2_buf, 0,
						      vb2_plane_size(&dst_buf->vb2_buf, 0));
				vb2_set_plane_payload(&dst_buf->vb2_buf, 1,
						      vb2_plane_size(&dst_buf->vb2_buf, 1));
				vb2_set_plane_payload(&dst_buf->vb2_buf, 2,
						      vb2_plane_size(&dst_buf->vb2_buf, 2));
			}

			dst_buf->vb2_buf.timestamp = inst->timestamp;
			dst_buf->flags |= V4L2_BUF_FLAG_LAST;
			dst_buf->field = V4L2_FIELD_NONE;
			v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);

			inst->eos = TRUE;
			pr_err("wave5 queue event type: %d id: %d\n",vpu_event_eos.type, vpu_event_eos.id);
			v4l2_event_queue_fh(&inst->v4l2_fh, &vpu_event_eos);

			v4l2_m2m_job_finish(inst->v4l2_m2m_dev, inst->v4l2_fh.m2m_ctx);
		}
	}
}

static int wave5_vpu_dec_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, VPU_DEC_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, VPU_DEC_DRV_NAME, sizeof(cap->card));
	strscpy(cap->bus_info, "platform:" VPU_DEC_DRV_NAME, sizeof(cap->bus_info));

	return 0;
}

static int wave5_vpu_dec_enum_framesizes(struct file *f, void *fh, struct v4l2_frmsizeenum *fsize)
{
	const struct vpu_format *vpu_fmt;

	if (fsize->index)
		return -EINVAL;

	vpu_fmt = wave5_find_vpu_fmt(fsize->pixel_format, dec_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt) {
		vpu_fmt = wave5_find_vpu_fmt(fsize->pixel_format, dec_fmt_list[VPU_FMT_TYPE_RAW]);
		if (!vpu_fmt)
			return -EINVAL;
	}

	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise.min_width = vpu_fmt->min_width;
	fsize->stepwise.max_width = vpu_fmt->max_width;
	fsize->stepwise.step_width = 1;
	fsize->stepwise.min_height = vpu_fmt->min_height;
	fsize->stepwise.max_height = vpu_fmt->max_height;
	fsize->stepwise.step_height = 1;

	return 0;
}

static int wave5_vpu_dec_enum_fmt_cap(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	const struct vpu_format *vpu_fmt;

	vpu_fmt = wave5_find_vpu_fmt_by_idx(f->index, dec_fmt_list[VPU_FMT_TYPE_RAW]);
	if (!vpu_fmt)
		return -EINVAL;

	f->pixelformat = vpu_fmt->v4l2_pix_fmt;
	f->flags = 0;

	return 0;
}

static int wave5_vpu_dec_try_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);
	const struct vpu_format *vpu_fmt;

	dev_dbg(inst->dev->dev,
		"%s: fourcc: %u width: %u height: %u nm planes: %u colorspace: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.colorspace, f->fmt.pix_mp.field);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	vpu_fmt = wave5_find_vpu_fmt(f->fmt.pix_mp.pixelformat, dec_fmt_list[VPU_FMT_TYPE_RAW]);
	if (!vpu_fmt) {
		f->fmt.pix_mp.pixelformat = inst->dst_fmt.pixelformat;
		f->fmt.pix_mp.num_planes = inst->dst_fmt.num_planes;
		wave5_update_pix_fmt(&f->fmt.pix_mp, inst->dst_fmt.width, inst->dst_fmt.height);
	} else {
		int width = clamp(f->fmt.pix_mp.width, vpu_fmt->min_width, vpu_fmt->max_width);
		int height = clamp(f->fmt.pix_mp.height, vpu_fmt->min_height, vpu_fmt->max_height);
		const struct v4l2_format_info *info = v4l2_format_info(vpu_fmt->v4l2_pix_fmt);

		f->fmt.pix_mp.pixelformat = vpu_fmt->v4l2_pix_fmt;
		f->fmt.pix_mp.num_planes = info->mem_planes;
		wave5_update_pix_fmt(&f->fmt.pix_mp, width, height);
	}

	f->fmt.pix_mp.flags = 0;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.colorspace = inst->colorspace;
	f->fmt.pix_mp.ycbcr_enc = inst->ycbcr_enc;
	f->fmt.pix_mp.hsv_enc = inst->hsv_enc;
	f->fmt.pix_mp.quantization = inst->quantization;
	f->fmt.pix_mp.xfer_func = inst->xfer_func;
	memset(&f->fmt.pix_mp.reserved, 0, sizeof(f->fmt.pix_mp.reserved));

	return 0;
}

static int wave5_vpu_dec_s_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);
	int i, ret;
	unsigned int scalew, scaleh;

	printk(
		"%s: fourcc: %u width: %u height: %u num_planes: %u colorspace: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.colorspace, f->fmt.pix_mp.field);

	ret = wave5_vpu_dec_try_fmt_cap(file, fh, f);

	if (ret)
		return ret;

	scalew = inst->src_fmt.width / f->fmt.pix_mp.width;
	scaleh = inst->src_fmt.height / f->fmt.pix_mp.height;

	if (scalew > 8 || scaleh > 8 || scalew < 1 || scaleh < 1) {
		dev_err(inst->dev->dev,"Scaling should be 1 to 1/8 (down-scaling only)! Use input parameter. \n");
		return -EINVAL;
	}

	inst->dst_fmt.width = f->fmt.pix_mp.width;
	inst->dst_fmt.height = f->fmt.pix_mp.height;
	inst->dst_fmt.pixelformat = f->fmt.pix_mp.pixelformat;
	inst->dst_fmt.field = f->fmt.pix_mp.field;
	inst->dst_fmt.flags = f->fmt.pix_mp.flags;
	inst->dst_fmt.num_planes = f->fmt.pix_mp.num_planes;
	for (i = 0; i < inst->dst_fmt.num_planes; i++) {
		inst->dst_fmt.plane_fmt[i].bytesperline = f->fmt.pix_mp.plane_fmt[i].bytesperline;
		inst->dst_fmt.plane_fmt[i].sizeimage = f->fmt.pix_mp.plane_fmt[i].sizeimage;
	}

	if (inst->dst_fmt.pixelformat == V4L2_PIX_FMT_NV12 ||
	    inst->dst_fmt.pixelformat == V4L2_PIX_FMT_NV12M) {
		inst->cbcr_interleave = true;
		inst->nv21 = false;
	} else if (inst->dst_fmt.pixelformat == V4L2_PIX_FMT_NV21 ||
		   inst->dst_fmt.pixelformat == V4L2_PIX_FMT_NV21M) {
		inst->cbcr_interleave = true;
		inst->nv21 = true;
	} else {
		inst->cbcr_interleave = false;
		inst->nv21 = false;
	}

	memcpy((void *)&inst->display_fmt, (void *)&inst->dst_fmt, sizeof(struct v4l2_pix_format_mplane));

	return 0;
}

static int wave5_vpu_dec_g_fmt_cap(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);
	int i;

	f->fmt.pix_mp.width = inst->display_fmt.width;
	f->fmt.pix_mp.height = inst->display_fmt.height;
	f->fmt.pix_mp.pixelformat = inst->display_fmt.pixelformat;
	f->fmt.pix_mp.field = inst->display_fmt.field;
	f->fmt.pix_mp.flags = inst->display_fmt.flags;
	f->fmt.pix_mp.num_planes = inst->display_fmt.num_planes;
	for (i = 0; i < f->fmt.pix_mp.num_planes; i++) {
		f->fmt.pix_mp.plane_fmt[i].bytesperline = inst->display_fmt.plane_fmt[i].bytesperline;
		f->fmt.pix_mp.plane_fmt[i].sizeimage = inst->display_fmt.plane_fmt[i].sizeimage;
	}

	f->fmt.pix_mp.colorspace = inst->colorspace;
	f->fmt.pix_mp.ycbcr_enc = inst->ycbcr_enc;
	f->fmt.pix_mp.hsv_enc = inst->hsv_enc;
	f->fmt.pix_mp.quantization = inst->quantization;
	f->fmt.pix_mp.xfer_func = inst->xfer_func;

	return 0;
}

static int wave5_vpu_dec_enum_fmt_out(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);
	const struct vpu_format *vpu_fmt;

	dev_dbg(inst->dev->dev, "%s: index: %u\n", __func__, f->index);

	vpu_fmt = wave5_find_vpu_fmt_by_idx(f->index, dec_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt)
		return -EINVAL;

	f->pixelformat = vpu_fmt->v4l2_pix_fmt;
	f->flags = 0;

	return 0;
}

static int wave5_vpu_dec_try_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);
	const struct vpu_format *vpu_fmt;

	dev_dbg(inst->dev->dev,
		"%s: fourcc: %u width: %u height: %u num_planes: %u colorspace: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.colorspace, f->fmt.pix_mp.field);

	if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	vpu_fmt = wave5_find_vpu_fmt(f->fmt.pix_mp.pixelformat, dec_fmt_list[VPU_FMT_TYPE_CODEC]);
	if (!vpu_fmt) {
		f->fmt.pix_mp.pixelformat = inst->src_fmt.pixelformat;
		f->fmt.pix_mp.num_planes = inst->src_fmt.num_planes;
		wave5_update_pix_fmt(&f->fmt.pix_mp, inst->src_fmt.width, inst->src_fmt.height);
	} else {
		int width = clamp(f->fmt.pix_mp.width, vpu_fmt->min_width, vpu_fmt->max_width);
		int height = clamp(f->fmt.pix_mp.height, vpu_fmt->min_height, vpu_fmt->max_height);

		f->fmt.pix_mp.pixelformat = vpu_fmt->v4l2_pix_fmt;
		f->fmt.pix_mp.num_planes = 1;
		wave5_update_pix_fmt(&f->fmt.pix_mp, width, height);
	}

	f->fmt.pix_mp.flags = 0;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	memset(&f->fmt.pix_mp.reserved, 0, sizeof(f->fmt.pix_mp.reserved));

	return 0;
}

static int wave5_vpu_dec_s_fmt_out(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);
	int i, ret;

	printk(
		"%s: fourcc: %u width: %u height: %u num_planes: %u field: %u\n",
		__func__, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height,
		f->fmt.pix_mp.num_planes, f->fmt.pix_mp.field);

	ret = wave5_vpu_dec_try_fmt_out(file, fh, f);
	if (ret)
		return ret;

	inst->src_fmt.width = f->fmt.pix_mp.width;
	inst->src_fmt.height = f->fmt.pix_mp.height;
	inst->src_fmt.pixelformat = f->fmt.pix_mp.pixelformat;
	inst->src_fmt.field = f->fmt.pix_mp.field;
	inst->src_fmt.flags = f->fmt.pix_mp.flags;
	inst->src_fmt.num_planes = f->fmt.pix_mp.num_planes;
	for (i = 0; i < inst->src_fmt.num_planes; i++) {
		inst->src_fmt.plane_fmt[i].bytesperline = f->fmt.pix_mp.plane_fmt[i].bytesperline;
		inst->src_fmt.plane_fmt[i].sizeimage = f->fmt.pix_mp.plane_fmt[i].sizeimage;
	}

	inst->colorspace = f->fmt.pix_mp.colorspace;
	inst->ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
	inst->hsv_enc = f->fmt.pix_mp.hsv_enc;
	inst->quantization = f->fmt.pix_mp.quantization;
	inst->xfer_func = f->fmt.pix_mp.xfer_func;

	wave5_update_pix_fmt(&inst->dst_fmt, f->fmt.pix_mp.width, f->fmt.pix_mp.height);

	return 0;
}

static int wave5_vpu_dec_g_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);

	dev_dbg(inst->dev->dev, "%s: type: %u | target: %u\n", __func__, s->type, s->target);

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = inst->dst_fmt.width;
		s->r.height = inst->dst_fmt.height;
		break;
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		s->r.left = 0;
		s->r.top = 0;
		if (inst->state > VPU_INST_STATE_OPEN) {
			s->r.width = inst->conf_win_width;
			s->r.height = inst->conf_win_height;
		} else {
			s->r.width = inst->src_fmt.width;
			s->r.height = inst->src_fmt.height;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int wave5_vpu_dec_s_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (s->target != V4L2_SEL_TGT_COMPOSE)
		return -EINVAL;

	dev_dbg(inst->dev->dev, "V4L2_SEL_TGT_COMPOSE w: %u h: %u\n",
		s->r.width, s->r.height);

	s->r.left = 0;
	s->r.top = 0;
	s->r.width = inst->dst_fmt.width;
	s->r.height = inst->dst_fmt.height;

	return 0;
}

static int wave5_vpu_dec_decoder_cmd(struct file *file, void *fh, struct v4l2_decoder_cmd *dc)
{
	struct vpu_instance *inst = wave5_to_vpu_inst(fh);
	int ret;

	dev_dbg(inst->dev->dev, "decoder command: %u\n", dc->cmd);

	ret = v4l2_m2m_ioctl_try_decoder_cmd(file, fh, dc);
	if (ret)
		return ret;

	if (!wave5_vpu_both_queues_are_streaming(inst))
		return 0;

	switch (dc->cmd) {
	case V4L2_DEC_CMD_STOP:
		inst->state = VPU_INST_STATE_STOP;

		ret = wave5_vpu_dec_update_bitstream_buffer(inst, 0);
		if (ret) {
			dev_err(inst->dev->dev,
				"Setting EOS for the bitstream, fail: %d\n", ret);
			return ret;
		}
		break;
	case V4L2_DEC_CMD_START:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ioctl_ops wave5_vpu_dec_ioctl_ops = {
	.vidioc_querycap = wave5_vpu_dec_querycap,
	.vidioc_enum_framesizes = wave5_vpu_dec_enum_framesizes,

	.vidioc_enum_fmt_vid_cap	= wave5_vpu_dec_enum_fmt_cap,
	.vidioc_s_fmt_vid_cap_mplane = wave5_vpu_dec_s_fmt_cap,
	.vidioc_g_fmt_vid_cap_mplane = wave5_vpu_dec_g_fmt_cap,
	.vidioc_try_fmt_vid_cap_mplane = wave5_vpu_dec_try_fmt_cap,

	.vidioc_enum_fmt_vid_out	= wave5_vpu_dec_enum_fmt_out,
	.vidioc_s_fmt_vid_out_mplane = wave5_vpu_dec_s_fmt_out,
	.vidioc_g_fmt_vid_out_mplane = wave5_vpu_g_fmt_out,
	.vidioc_try_fmt_vid_out_mplane = wave5_vpu_dec_try_fmt_out,

	.vidioc_g_selection = wave5_vpu_dec_g_selection,
	.vidioc_s_selection = wave5_vpu_dec_s_selection,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_try_decoder_cmd,
	.vidioc_decoder_cmd = wave5_vpu_dec_decoder_cmd,

	.vidioc_subscribe_event = wave5_vpu_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int wave5_vpu_dec_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vpu_instance *inst = wave5_ctrl_to_vpu_inst(ctrl);

	dev_dbg(inst->dev->dev, "%s: name: %s | value: %d\n",
		__func__, ctrl->name, ctrl->val);

	switch (ctrl->id) {
	case V4L2_CID_VPU_THUMBNAIL_MODE:
		inst->thumbnail_mode = ctrl->val;
		break;
	case V4L2_CID_MIN_BUFFERS_FOR_CAPTURE:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops wave5_vpu_dec_ctrl_ops = {
	.s_ctrl = wave5_vpu_dec_s_ctrl,
};

static const struct v4l2_ctrl_config wave5_vpu_thumbnail_mode = {
	.ops = &wave5_vpu_dec_ctrl_ops,
	.id = V4L2_CID_VPU_THUMBNAIL_MODE,
	.name = "thumbnail mode",
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.def = 0,
	.min = 0,
	.max = 1,
	.step = 1,
	.flags = V4L2_CTRL_FLAG_WRITE_ONLY,
};

static void wave5_set_default_dec_openparam(struct dec_open_param *open_param)
{
	open_param->bitstream_mode = BS_MODE_INTERRUPT;
	open_param->stream_endian = VPU_STREAM_ENDIAN;
	open_param->frame_endian = VPU_FRAME_ENDIAN;
}

static int wave5_vpu_dec_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
				     unsigned int *num_planes, unsigned int sizes[],
				     struct device *alloc_devs[])
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	struct v4l2_pix_format_mplane inst_format =
		(q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ? inst->src_fmt : inst->display_fmt;
	unsigned int i;
	int ret;

	dev_dbg(inst->dev->dev, "%s: num_buffers: %u | num_planes: %u | type: %u\n", __func__,
		*num_buffers, *num_planes, q->type);

	if (*num_planes) {
		if (inst_format.num_planes != *num_planes)
			return -EINVAL;

		for (i = 0; i < *num_planes; i++) {
			if (sizes[i] < inst_format.plane_fmt[i].sizeimage)
				return -EINVAL;
		}
	} else {
		*num_planes = inst_format.num_planes;

		if (*num_planes == 1) {
			sizes[0] = inst_format.width * inst_format.height * 3 / 2;
			if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
				sizes[0] = inst_format.plane_fmt[0].sizeimage;
			dev_dbg(inst->dev->dev, "%s: size[0]: %u\n", __func__, sizes[0]);
		} else if (*num_planes == 2) {
			sizes[0] = inst_format.width * inst_format.height;
			sizes[1] = inst_format.width * inst_format.height / 2;
			dev_dbg(inst->dev->dev, "%s: size[0]: %u | size[1]: %u\n",
				__func__, sizes[0], sizes[1]);
		} else if (*num_planes == 3) {
			sizes[0] = inst_format.width * inst_format.height;
			sizes[1] = inst_format.width * inst_format.height / 4;
			sizes[2] = inst_format.width * inst_format.height / 4;
			dev_dbg(inst->dev->dev, "%s: size[0]: %u | size[1]: %u | size[2]: %u\n",
				__func__, sizes[0], sizes[1], sizes[2]);
		}
	}

	if (inst->state == VPU_INST_STATE_NONE && q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		struct dec_open_param open_param;

		memset(&open_param, 0, sizeof(struct dec_open_param));
		wave5_set_default_dec_openparam(&open_param);

		inst->bitstream_vbuf.size = ALIGN(inst->src_fmt.plane_fmt[0].sizeimage, 1024) * 4;
		ret = wave5_vdi_allocate_dma_memory(inst->dev, &inst->bitstream_vbuf);
		if (ret) {
			dev_dbg(inst->dev->dev, "%s: alloc bitstream of size %zu fail: %d\n",
				__func__, inst->bitstream_vbuf.size, ret);
			return ret;
		}

		inst->std = wave5_to_vpu_codstd(inst->src_fmt.pixelformat);
		if (inst->std == STD_UNKNOWN) {
			dev_warn(inst->dev->dev, "unsupported pixelformat: %.4s\n",
				 (char *)&inst->src_fmt.pixelformat);
			ret = -EINVAL;
			goto free_bitstream_vbuf;
		}
		open_param.bitstream_buffer = inst->bitstream_vbuf.daddr;
		open_param.bitstream_buffer_size = inst->bitstream_vbuf.size;

		ret = wave5_vpu_dec_open(inst, &open_param);
		if (ret) {
			dev_dbg(inst->dev->dev, "%s: wave5_vpu_dec_open, fail: %d\n",
				__func__, ret);
			goto free_bitstream_vbuf;
		}

		inst->state = VPU_INST_STATE_OPEN;

		if (inst->thumbnail_mode)
			wave5_vpu_dec_give_command(inst, ENABLE_DEC_THUMBNAIL_MODE, NULL);

	} else if (inst->state == VPU_INST_STATE_INIT_SEQ &&
		   q->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		u32 non_linear_num;
		u32 fb_stride, fb_height;
		u32 luma_size, chroma_size;

		if (*num_buffers > inst->min_dst_buf_count &&
		    *num_buffers < WAVE5_MAX_FBS)
			inst->dst_buf_count = *num_buffers;

		*num_buffers = inst->dst_buf_count;
		non_linear_num = inst->dst_buf_count;

		for (i = 0; i < non_linear_num; i++) {
			struct frame_buffer *frame = &inst->frame_buf[i];
			struct vpu_buf *vframe = &inst->frame_vbuf[i];

			fb_stride = inst->dst_fmt.width;
			fb_height = ALIGN(inst->dst_fmt.height, 32);
			luma_size = fb_stride * fb_height;
			chroma_size = ALIGN(fb_stride / 2, 16) * fb_height;

			vframe->size = luma_size + chroma_size;
			ret = wave5_vdi_allocate_dma_memory(inst->dev, vframe);
			if (ret) {
				dev_dbg(inst->dev->dev,
					"%s: Allocating FBC buf of size %zu, fail: %d\n",
					__func__, vframe->size, ret);
				return ret;
			}

			frame->buf_y = vframe->daddr;
			frame->buf_cb = vframe->daddr + luma_size;
			frame->buf_cr = (dma_addr_t)-1;
			frame->size = vframe->size;
			frame->width = inst->src_fmt.width;
			frame->stride = fb_stride;
			frame->map_type = COMPRESSED_FRAME_MAP;
			frame->update_fb_info = true;
			dev_dbg(inst->dev->dev, "no linear framebuf y 0x%llx cb 0x%llx cr 0x%llx\n",
												frame->buf_y, frame->buf_cb, frame->buf_cr);
		}
	} else if (inst->state == VPU_INST_STATE_STOP &&
		   q->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		*num_buffers = 0;
	}

	return 0;

free_bitstream_vbuf:
	wave5_vdi_free_dma_memory(inst->dev, &inst->bitstream_vbuf);
	return ret;
}

static int wave5_vpu_dec_start_streaming_open(struct vpu_instance *inst)
{
	struct dec_initial_info initial_info;
	unsigned int scalew, scaleh;
	int ret = 0;

	memset(&initial_info, 0, sizeof(struct dec_initial_info));

	ret = wave5_vpu_dec_issue_seq_init(inst);
	if (ret) {
		dev_dbg(inst->dev->dev, "%s: wave5_vpu_dec_issue_seq_init, fail: %d\n",
			__func__, ret);
		return ret;
	}

	if (wave5_vpu_wait_interrupt(inst, VPU_DEC_TIMEOUT) < 0)
		dev_dbg(inst->dev->dev, "%s: failed to call vpu_wait_interrupt()\n", __func__);

	ret = wave5_vpu_dec_complete_seq_init(inst, &initial_info);
	if (ret) {
		dev_dbg(inst->dev->dev, "%s: vpu_dec_complete_seq_init, fail: %d, reason: %u\n",
			__func__, ret, initial_info.seq_init_err_reason);
	} else {
		static const struct v4l2_event vpu_event_src_ch = {
			.type = V4L2_EVENT_SOURCE_CHANGE,
			.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
		};
		struct v4l2_ctrl *ctrl;

		dev_dbg(inst->dev->dev, "%s: width: %u height: %u profile: %u | minbuffer: %u\n",
			__func__, initial_info.pic_width, initial_info.pic_height,
			initial_info.profile, initial_info.min_frame_buffer_count);

		inst->state = VPU_INST_STATE_INIT_SEQ;
		inst->min_dst_buf_count = initial_info.min_frame_buffer_count + 1;
		inst->dst_buf_count = inst->min_dst_buf_count;

		inst->conf_win_width = initial_info.pic_width - initial_info.pic_crop_rect.right;
		inst->conf_win_height = initial_info.pic_height - initial_info.pic_crop_rect.bottom;

		ctrl = v4l2_ctrl_find(&inst->v4l2_ctrl_hdl,
				      V4L2_CID_MIN_BUFFERS_FOR_CAPTURE);
		if (ctrl)
			v4l2_ctrl_s_ctrl(ctrl, inst->min_dst_buf_count);

		if (initial_info.pic_width != inst->src_fmt.width ||
		    initial_info.pic_height != inst->src_fmt.height) {
			wave5_update_pix_fmt(&inst->src_fmt, initial_info.pic_width,
					     initial_info.pic_height);
			wave5_update_pix_fmt(&inst->dst_fmt, initial_info.pic_width,
					     initial_info.pic_height);
		}

		scalew = inst->dst_fmt.width / inst->display_fmt.width;
		scaleh = inst->dst_fmt.height / inst->display_fmt.height;

		if (scalew > 8 || scaleh > 8 || scalew < 1 || scaleh < 1) {
			wave5_update_pix_fmt(&inst->display_fmt, inst->dst_fmt.width,
						inst->dst_fmt.height);
		}

		printk("wave5 queue event type: %d id: %d\n",vpu_event_src_ch.type, vpu_event_src_ch.id);
		v4l2_event_queue_fh(&inst->v4l2_fh, &vpu_event_src_ch);

		wave5_handle_src_buffer(inst);
	}

	return ret;
}

static int wave5_vpu_dec_start_streaming_seek(struct vpu_instance *inst)
{
	struct dec_initial_info initial_info;
	struct dec_param pic_param;
	struct dec_output_info dec_output_info;
	unsigned int scalew, scaleh;
	int ret = 0;
	u32 fail_res = 0;

	memset(&pic_param, 0, sizeof(struct dec_param));

	ret = wave5_vpu_dec_start_one_frame(inst, &pic_param, &fail_res);
	if (ret && fail_res != WAVE5_SYSERR_QUEUEING_FAIL) {
		struct vb2_v4l2_buffer *src_buf;

		src_buf = v4l2_m2m_src_buf_remove(inst->v4l2_fh.m2m_ctx);
		inst->state = VPU_INST_STATE_STOP;
		v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
		dev_dbg(inst->dev->dev, "%s: wave5_vpu_dec_start_one_frame\n", __func__);
		return ret;
	}

	if (wave5_vpu_wait_interrupt(inst, VPU_DEC_TIMEOUT) < 0)
		dev_dbg(inst->dev->dev, "%s: failed to call vpu_wait_interrupt()\n", __func__);

	ret = wave5_vpu_dec_get_output_info(inst, &dec_output_info);
	if (ret) {
		dev_dbg(inst->dev->dev, "%s: wave5_vpu_dec_get_output_info, fail: %d\n",
			__func__, ret);
		return ret;
	}

	if (dec_output_info.sequence_changed) {
		static const struct v4l2_event vpu_event_src_ch = {
			.type = V4L2_EVENT_SOURCE_CHANGE,
			.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
		};
		struct v4l2_ctrl *ctrl;

		wave5_vpu_dec_give_command(inst, DEC_RESET_FRAMEBUF_INFO, NULL);
		wave5_vpu_dec_give_command(inst, DEC_GET_SEQ_INFO, &initial_info);

		dev_dbg(inst->dev->dev, "%s: width: %u height: %u profile: %u | minbuffer: %u\n",
			__func__, initial_info.pic_width, initial_info.pic_height,
			initial_info.profile, initial_info.min_frame_buffer_count);

		inst->min_dst_buf_count = initial_info.min_frame_buffer_count + 1;
		inst->dst_buf_count = inst->min_dst_buf_count;

		inst->conf_win_width = initial_info.pic_width - initial_info.pic_crop_rect.right;
		inst->conf_win_height = initial_info.pic_height - initial_info.pic_crop_rect.bottom;

		ctrl = v4l2_ctrl_find(&inst->v4l2_ctrl_hdl,
				      V4L2_CID_MIN_BUFFERS_FOR_CAPTURE);
		if (ctrl)
			v4l2_ctrl_s_ctrl(ctrl, inst->min_dst_buf_count);

		if (initial_info.pic_width != inst->src_fmt.width ||
		    initial_info.pic_height != inst->src_fmt.height) {
			wave5_update_pix_fmt(&inst->src_fmt, initial_info.pic_width,
					     initial_info.pic_height);
			wave5_update_pix_fmt(&inst->dst_fmt, initial_info.pic_width,
					     initial_info.pic_height);
		}

		scalew = inst->dst_fmt.width / inst->display_fmt.width;
		scaleh = inst->dst_fmt.height / inst->display_fmt.height;

		if (scalew > 8 || scaleh > 8 || scalew < 1 || scaleh < 1) {
			wave5_update_pix_fmt(&inst->display_fmt, inst->dst_fmt.width,
						inst->dst_fmt.height);
		}

		v4l2_event_queue_fh(&inst->v4l2_fh, &vpu_event_src_ch);

		wave5_handle_src_buffer(inst);
	}

	return ret;
}

static void wave5_vpu_dec_buf_queue_src(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);
	struct vpu_buffer *vpu_buf = wave5_to_vpu_buf(vbuf);

	vpu_buf->consumed = false;
	vbuf->sequence = inst->queued_src_buf_num++;

	if (inst->state == VPU_INST_STATE_PIC_RUN) {
		wave5_handle_bitstream_buffer(inst);
		inst->ops->start_process(inst);
	}
}

static void wave5_vpu_dec_buf_queue_dst(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);
	int ret;

	vbuf->sequence = inst->queued_dst_buf_num++;
	ret = wave5_vpu_dec_clr_disp_flag(inst, vb->index);
	if (ret) {
		dev_dbg(inst->dev->dev,
			"%s: Clearing the display flag of buffer index: %u, fail: %d\n",
			__func__, vb->index, ret);
	}

	if (inst->state == VPU_INST_STATE_INIT_SEQ) {
		dma_addr_t buf_addr_y = 0, buf_addr_cb = 0, buf_addr_cr = 0;
		u32 buf_size = 0;
		u32 non_linear_num = inst->dst_buf_count;
		u32 fb_stride = inst->display_fmt.width;
		u32 luma_size = fb_stride * inst->display_fmt.height;
		u32 chroma_size = (fb_stride / 2) * (inst->display_fmt.height / 2);

		if (inst->display_fmt.num_planes == 1) {
			buf_size = vb2_plane_size(&vbuf->vb2_buf, 0);
			buf_addr_y = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 0);
			buf_addr_cb = buf_addr_y + luma_size;
			buf_addr_cr = buf_addr_cb + chroma_size;
		} else if (inst->display_fmt.num_planes == 2) {
			buf_size = vb2_plane_size(&vbuf->vb2_buf, 0) +
				vb2_plane_size(&vbuf->vb2_buf, 1);
			buf_addr_y = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 0);
			buf_addr_cb = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 1);
			buf_addr_cr = buf_addr_cb + chroma_size;
		} else if (inst->display_fmt.num_planes == 3) {
			buf_size = vb2_plane_size(&vbuf->vb2_buf, 0) +
				vb2_plane_size(&vbuf->vb2_buf, 1) +
				vb2_plane_size(&vbuf->vb2_buf, 2);
			buf_addr_y = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 0);
			buf_addr_cb = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 1);
			buf_addr_cr = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 2);
		}
		inst->frame_buf[vb->index + non_linear_num].buf_y = buf_addr_y;
		inst->frame_buf[vb->index + non_linear_num].buf_cb = buf_addr_cb;
		inst->frame_buf[vb->index + non_linear_num].buf_cr = buf_addr_cr;
		inst->frame_buf[vb->index + non_linear_num].size = buf_size;
		inst->frame_buf[vb->index + non_linear_num].width = inst->display_fmt.width;
		inst->frame_buf[vb->index + non_linear_num].stride = fb_stride;
		inst->frame_buf[vb->index + non_linear_num].map_type = LINEAR_FRAME_MAP;
		inst->frame_buf[vb->index + non_linear_num].update_fb_info = true;
		dev_dbg(inst->dev->dev, "linear framebuf y 0x%llx cb 0x%llx cr 0x%llx\n",buf_addr_y, buf_addr_cb, buf_addr_cr);
	}

	if (!vb2_is_streaming(vb->vb2_queue))
		return;

	if (inst->state == VPU_INST_STATE_STOP && inst->eos == FALSE)
		inst->ops->start_process(inst);
}

static void wave5_vpu_dec_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vpu_instance *inst = vb2_get_drv_priv(vb->vb2_queue);

	dev_dbg(inst->dev->dev, "%s: type: %4u index: %4u size: ([0]=%4lu, [1]=%4lu, [2]=%4lu)\n",
		__func__, vb->type, vb->index, vb2_plane_size(&vbuf->vb2_buf, 0),
		vb2_plane_size(&vbuf->vb2_buf, 1), vb2_plane_size(&vbuf->vb2_buf, 2));

	v4l2_m2m_buf_queue(inst->v4l2_fh.m2m_ctx, vbuf);

	if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		wave5_vpu_dec_buf_queue_src(vb);
	else if (vb->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		wave5_vpu_dec_buf_queue_dst(vb);
}

static int wave5_vpu_dec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	int ret = 0;

	dev_dbg(inst->dev->dev, "%s: type: %u\n", __func__, q->type);

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		wave5_handle_bitstream_buffer(inst);
		if (inst->state == VPU_INST_STATE_OPEN)
			ret = wave5_vpu_dec_start_streaming_open(inst);
		else if (inst->state == VPU_INST_STATE_INIT_SEQ)
			ret = wave5_vpu_dec_start_streaming_seek(inst);

		if (ret) {
			struct vb2_v4l2_buffer *buf;

			while ((buf = v4l2_m2m_src_buf_remove(inst->v4l2_fh.m2m_ctx))) {
				dev_dbg(inst->dev->dev, "%s: (Multiplanar) buf type %4d | index %4d\n",
					    __func__, buf->vb2_buf.type, buf->vb2_buf.index);
				v4l2_m2m_buf_done(buf, VB2_BUF_STATE_QUEUED);
			}
		}
	}

	return ret;
}

static void wave5_vpu_dec_stop_streaming(struct vb2_queue *q)
{
	struct vpu_instance *inst = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *buf;
	bool check_cmd = TRUE;

	dev_dbg(inst->dev->dev, "%s: type: %u\n", __func__, q->type);

	while (check_cmd) {
		struct queue_status_info q_status;
		struct dec_output_info dec_output_info;
		int try_cnt = 0;

		wave5_vpu_dec_give_command(inst, DEC_GET_QUEUE_STATUS, &q_status);

		if (q_status.instance_queue_count + q_status.report_queue_count == 0)
			break;

		if (wave5_vpu_wait_interrupt(inst, 600) < 0){
			try_cnt++;
			if (try_cnt >= 100)
				break;
			continue;
		}

		if (wave5_vpu_dec_get_output_info(inst, &dec_output_info))
			dev_dbg(inst->dev->dev, "Getting decoding results from fw, fail\n");
	}

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		while ((buf = v4l2_m2m_src_buf_remove(inst->v4l2_fh.m2m_ctx))) {
			dev_dbg(inst->dev->dev, "%s: (Multiplanar) buf type %4u | index %4u\n",
				__func__, buf->vb2_buf.type, buf->vb2_buf.index);
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
		}
		inst->queued_src_buf_num = 0;
	} else {
		unsigned int i;
		int ret;
		dma_addr_t rd_ptr, wr_ptr;

		while ((buf = v4l2_m2m_dst_buf_remove(inst->v4l2_fh.m2m_ctx))) {
			u32 plane;

			dev_dbg(inst->dev->dev, "%s: buf type %4u | index %4u\n",
				__func__, buf->vb2_buf.type, buf->vb2_buf.index);

			for (plane = 0; plane < inst->dst_fmt.num_planes; plane++)
				vb2_set_plane_payload(&buf->vb2_buf, plane, 0);

			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
		}

		for (i = 0; i < inst->dst_buf_count; i++) {
			ret = wave5_vpu_dec_set_disp_flag(inst, i);
			if (ret) {
				dev_dbg(inst->dev->dev,
					"%s: Setting display flag of buf index: %u, fail: %d\n",
					__func__, i, ret);
			}
		}

		ret = wave5_vpu_dec_get_bitstream_buffer(inst, &rd_ptr, &wr_ptr, NULL);
		if (ret) {
			dev_err(inst->dev->dev,
				"Getting bitstream buf, fail: %d\n", ret);
			return;
		}
		ret = wave5_vpu_dec_set_rd_ptr(inst, wr_ptr, TRUE);
		if (ret) {
			dev_err(inst->dev->dev,
				"Setting read pointer for the decoder, fail: %d\n", ret);
			return;
		}
		if (inst->eos) {
			inst->eos = FALSE;
			inst->state = VPU_INST_STATE_INIT_SEQ;
		}
		inst->queued_dst_buf_num = 0;
	}
}

static const struct vb2_ops wave5_vpu_dec_vb2_ops = {
	.queue_setup = wave5_vpu_dec_queue_setup,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.buf_queue = wave5_vpu_dec_buf_queue,
	.start_streaming = wave5_vpu_dec_start_streaming,
	.stop_streaming = wave5_vpu_dec_stop_streaming,
};

static void wave5_set_default_format(struct v4l2_pix_format_mplane *src_fmt,
				     struct v4l2_pix_format_mplane *dst_fmt)
{
	unsigned int dst_pix_fmt = dec_fmt_list[VPU_FMT_TYPE_RAW][0].v4l2_pix_fmt;
	const struct v4l2_format_info *dst_fmt_info = v4l2_format_info(dst_pix_fmt);

	src_fmt->pixelformat = dec_fmt_list[VPU_FMT_TYPE_CODEC][0].v4l2_pix_fmt;
	src_fmt->field = V4L2_FIELD_NONE;
	src_fmt->flags = 0;
	src_fmt->num_planes = 1;
	wave5_update_pix_fmt(src_fmt, 720, 480);

	dst_fmt->pixelformat = dst_pix_fmt;
	dst_fmt->field = V4L2_FIELD_NONE;
	dst_fmt->flags = 0;
	dst_fmt->num_planes = dst_fmt_info->mem_planes;
	wave5_update_pix_fmt(dst_fmt, 736, 480);
}

static int wave5_vpu_dec_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	return wave5_vpu_queue_init(priv, src_vq, dst_vq, &wave5_vpu_dec_vb2_ops);
}

static const struct vpu_instance_ops wave5_vpu_dec_inst_ops = {
	.start_process = wave5_vpu_dec_start_decode,
	.stop_process = wave5_vpu_dec_stop_decode,
	.finish_process = wave5_vpu_dec_finish_decode,
};

static void wave5_vpu_dec_device_run(void *priv)
{
	struct vpu_instance *inst = priv;

	inst->ops->start_process(inst);

	inst->state = VPU_INST_STATE_PIC_RUN;
}

static void wave5_vpu_dec_job_abort(void *priv)
{
	struct vpu_instance *inst = priv;

	inst->ops->stop_process(inst);
}

static const struct v4l2_m2m_ops wave5_vpu_dec_m2m_ops = {
	.device_run = wave5_vpu_dec_device_run,
	.job_abort = wave5_vpu_dec_job_abort,
};

static int wave5_vpu_open_dec(struct file *filp)
{
	struct video_device *vdev = video_devdata(filp);
	struct vpu_device *dev = video_drvdata(filp);
	struct vpu_instance *inst = NULL;
	int ret = 0;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	inst->dev = dev;
	inst->type = VPU_INST_TYPE_DEC;
	inst->ops = &wave5_vpu_dec_inst_ops;

	v4l2_fh_init(&inst->v4l2_fh, vdev);
	filp->private_data = &inst->v4l2_fh;
	v4l2_fh_add(&inst->v4l2_fh);

	INIT_LIST_HEAD(&inst->list);
	list_add_tail(&inst->list, &dev->instances);

	inst->v4l2_m2m_dev = v4l2_m2m_init(&wave5_vpu_dec_m2m_ops);
	if (IS_ERR(inst->v4l2_m2m_dev)) {
		ret = PTR_ERR(inst->v4l2_m2m_dev);
		dev_err(inst->dev->dev, "v4l2_m2m_init, fail: %d\n", ret);
		goto cleanup_inst;
	}

	inst->v4l2_fh.m2m_ctx =
		v4l2_m2m_ctx_init(inst->v4l2_m2m_dev, inst, wave5_vpu_dec_queue_init);
	if (IS_ERR(inst->v4l2_fh.m2m_ctx)) {
		ret = PTR_ERR(inst->v4l2_fh.m2m_ctx);
		goto cleanup_inst;
	}

	v4l2_ctrl_handler_init(&inst->v4l2_ctrl_hdl, 10);
	v4l2_ctrl_new_custom(&inst->v4l2_ctrl_hdl, &wave5_vpu_thumbnail_mode, NULL);
	v4l2_ctrl_new_std(&inst->v4l2_ctrl_hdl, &wave5_vpu_dec_ctrl_ops,
			  V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 1, 32, 1, 1);

	if (inst->v4l2_ctrl_hdl.error) {
		ret = -ENODEV;
		goto cleanup_inst;
	}

	inst->v4l2_fh.ctrl_handler = &inst->v4l2_ctrl_hdl;
	v4l2_ctrl_handler_setup(&inst->v4l2_ctrl_hdl);

	wave5_set_default_format(&inst->src_fmt, &inst->dst_fmt);
	memcpy((void *)&inst->display_fmt, (void *)&inst->dst_fmt, sizeof(struct v4l2_pix_format_mplane));
	inst->colorspace = V4L2_COLORSPACE_REC709;
	inst->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	inst->hsv_enc = 0;
	inst->quantization = V4L2_QUANTIZATION_DEFAULT;
	inst->xfer_func = V4L2_XFER_FUNC_DEFAULT;

	init_completion(&inst->irq_done);
	ret = kfifo_alloc(&inst->irq_status, 16 * sizeof(int), GFP_KERNEL);
	if (ret) {
		dev_err(inst->dev->dev, "failed to allocate fifo\n");
		goto cleanup_inst;
	}

	inst->id = ida_alloc(&inst->dev->inst_ida, GFP_KERNEL);
	if (inst->id < 0) {
		dev_warn(inst->dev->dev, "Allocating instance ID, fail: %d\n", inst->id);
		ret = inst->id;
		goto cleanup_inst;
	}

	return 0;

cleanup_inst:
	wave5_cleanup_instance(inst);
	return ret;
}

static int wave5_vpu_dec_release(struct file *filp)
{
	return wave5_vpu_release_device(filp, wave5_vpu_dec_close, "decoder");
}

static const struct v4l2_file_operations wave5_vpu_dec_fops = {
	.owner = THIS_MODULE,
	.open = wave5_vpu_open_dec,
	.release = wave5_vpu_dec_release,
	.unlocked_ioctl = video_ioctl2,
	.poll = v4l2_m2m_fop_poll,
	.mmap = v4l2_m2m_fop_mmap,
};

int wave5_vpu_dec_register_device(struct vpu_device *dev)
{
	struct video_device *vdev_dec;
	int ret;

	vdev_dec = devm_kzalloc(dev->v4l2_dev.dev, sizeof(*vdev_dec), GFP_KERNEL);
	if (!vdev_dec)
		return -ENOMEM;

	dev->video_dev_dec = vdev_dec;

	strscpy(vdev_dec->name, VPU_DEC_DEV_NAME, sizeof(vdev_dec->name));
	vdev_dec->fops = &wave5_vpu_dec_fops;
	vdev_dec->ioctl_ops = &wave5_vpu_dec_ioctl_ops;
	vdev_dec->release = video_device_release_empty;
	vdev_dec->v4l2_dev = &dev->v4l2_dev;
	vdev_dec->vfl_dir = VFL_DIR_M2M;
	vdev_dec->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	vdev_dec->lock = &dev->dev_lock;

	ret = video_register_device(vdev_dec, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;

	video_set_drvdata(vdev_dec, dev);

	return 0;
}

void wave5_vpu_dec_unregister_device(struct vpu_device *dev)
{
	video_unregister_device(dev->video_dev_dec);
}
