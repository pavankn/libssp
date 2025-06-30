#include "VideoPipeline.h"
#include <iostream>

VideoPipeline::VideoPipeline(const std::string& ip,
    const std::string& ndiName,
    VideoDecoder::CodecType codecType)
    : ip_(ip), ndiName_(ndiName), codecType_(codecType)
{
}

VideoPipeline::~VideoPipeline()
{
    if (swsCtx_)
        sws_freeContext(swsCtx_);
    if (rgbFrame_)
        av_frame_free(&rgbFrame_);
    if (ndiSender_)
        NDIlib_send_destroy(ndiSender_);
}

bool VideoPipeline::initialize()
{
    // Initialize NDI
    if (!NDIlib_initialize())
    {
        std::cerr << "NDI initialization failed." << std::endl;
        return false;
    }

    NDIlib_send_create_t sendDesc;
    sendDesc.p_ndi_name = ndiName_.c_str();
    ndiSender_ = NDIlib_send_create(&sendDesc);
    if (!ndiSender_)
    {
        std::cerr << "NDI sender creation failed." << std::endl;
        return false;
    }

    // Init decoder
    decoder_ = std::make_unique<VideoDecoder>();
    if (!decoder_->initialize(codecType_))
    {
        std::cerr << "Decoder initialization failed." << std::endl;
        return false;
    }

    std::cout << "Pipeline initialized for " << ndiName_ << std::endl;
    return true;
}

void VideoPipeline::processPacket(uint8_t* data, int size, int frameNo)
{
    AVFrame* frame = decoder_->processPacket(data, size);
    if (!frame)
        return;

    if (!rgbFrame_ || frame->width != lastWidth_ || frame->height != lastHeight_)
    {
        if (rgbFrame_)
            av_frame_free(&rgbFrame_);
        if (swsCtx_)
            sws_freeContext(swsCtx_);

        rgbFrame_ = av_frame_alloc();
        rgbFrame_->format = AV_PIX_FMT_BGRA;
        rgbFrame_->width = frame->width;
        rgbFrame_->height = frame->height;
        if (av_frame_get_buffer(rgbFrame_, 32) < 0)
        {
            std::cerr << "Failed to allocate RGB buffer." << std::endl;
            return;
        }

        swsCtx_ = sws_getContext(
            frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height,
            AV_PIX_FMT_BGRA,
            SWS_BILINEAR,
            nullptr, nullptr, nullptr);

        if (!swsCtx_)
        {
            std::cerr << "Failed to create SwsContext." << std::endl;
            return;
        }

        lastWidth_ = frame->width;
        lastHeight_ = frame->height;

        std::cout << "Initialized swscale context for resolution " << frame->width << "x" << frame->height << std::endl;
    }

    sws_scale(
        swsCtx_,
        frame->data, frame->linesize,
        0, frame->height,
        rgbFrame_->data, rgbFrame_->linesize);

    NDIlib_video_frame_v2_t ndiFrame;
    ndiFrame.xres = rgbFrame_->width;
    ndiFrame.yres = rgbFrame_->height;
    ndiFrame.FourCC = NDIlib_FourCC_type_BGRA;
    ndiFrame.frame_rate_N = 30000;
    ndiFrame.frame_rate_D = 1001;
    ndiFrame.picture_aspect_ratio = (float)rgbFrame_->width / rgbFrame_->height;
    ndiFrame.timecode = NDIlib_send_timecode_synthesize;
    ndiFrame.p_data = rgbFrame_->data[0];
    ndiFrame.line_stride_in_bytes = rgbFrame_->linesize[0];

    NDIlib_send_send_video_v2(ndiSender_, &ndiFrame);

    std::cout << "Sent frame " << frameNo << " to NDI [" << ndiName_ << "]" << std::endl;
}
