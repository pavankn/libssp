#include "NDISender.h"
#include <cstdio>

NDISender::NDISender(const std::string& ndiName)
    : ndi_sender_(nullptr), sws_ctx_(nullptr), rgb_frame_(nullptr), last_width_(0), last_height_(0)
{
    NDIlib_initialize();
    NDIlib_send_create_t desc;
    desc.p_ndi_name = ndiName.c_str();
    ndi_sender_ = NDIlib_send_create(&desc);
}

NDISender::~NDISender()
{
    if (rgb_frame_)
        av_frame_free(&rgb_frame_);
    if (sws_ctx_)
        sws_freeContext(sws_ctx_);
    if (ndi_sender_)
        NDIlib_send_destroy(ndi_sender_);
}

bool NDISender::initialize()
{
    return ndi_sender_ != nullptr;
}

void NDISender::sendFrame(AVFrame* yuvFrame)
{
    if (!yuvFrame)
        return;

    // Reallocate if needed
    if (!rgb_frame_ || yuvFrame->width != last_width_ || yuvFrame->height != last_height_)
    {
        if (rgb_frame_)
            av_frame_free(&rgb_frame_);
        if (sws_ctx_)
            sws_freeContext(sws_ctx_);

        rgb_frame_ = av_frame_alloc();
        rgb_frame_->format = AV_PIX_FMT_BGRA;
        rgb_frame_->width = yuvFrame->width;
        rgb_frame_->height = yuvFrame->height;
        av_frame_get_buffer(rgb_frame_, 32);

        sws_ctx_ = sws_getContext(
            yuvFrame->width, yuvFrame->height, (AVPixelFormat)yuvFrame->format,
            rgb_frame_->width, rgb_frame_->height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        last_width_ = yuvFrame->width;
        last_height_ = yuvFrame->height;
    }

    // Convert
    sws_scale(
        sws_ctx_,
        yuvFrame->data, yuvFrame->linesize,
        0, yuvFrame->height,
        rgb_frame_->data, rgb_frame_->linesize);

    // Push NDI
    NDIlib_video_frame_v2_t ndi_frame;
    ndi_frame.xres = rgb_frame_->width;
    ndi_frame.yres = rgb_frame_->height;
    ndi_frame.FourCC = NDIlib_FourCC_type_BGRA;
    ndi_frame.frame_rate_N = 240000;
    ndi_frame.frame_rate_D = 1000;
    ndi_frame.picture_aspect_ratio = (float)rgb_frame_->width / rgb_frame_->height;
    ndi_frame.timecode = NDIlib_send_timecode_synthesize;
    ndi_frame.p_data = rgb_frame_->data[0];
    ndi_frame.line_stride_in_bytes = rgb_frame_->linesize[0];

    NDIlib_send_send_video_v2(ndi_sender_, &ndi_frame);
}
