#include "VideoDecoder.h"
#include <iostream>

VideoDecoder::VideoDecoder()
{
}

VideoDecoder::~VideoDecoder()
{
    if (ctx_)
        avcodec_free_context(&ctx_);
    if (frame_)
        av_frame_free(&frame_);
}

bool VideoDecoder::initialize(CodecType type)
{
    if (type == CodecType::H264)
        codec_ = avcodec_find_decoder(AV_CODEC_ID_H264);
    else if (type == CodecType::H265)
        codec_ = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    else
        return false;

    if (!codec_)
    {
        std::cerr << "Decoder not found." << std::endl;
        return false;
    }

    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_)
    {
        std::cerr << "Could not allocate codec context." << std::endl;
        return false;
    }

    if (avcodec_open2(ctx_, codec_, nullptr) < 0)
    {
        std::cerr << "Could not open codec." << std::endl;
        return false;
    }

    frame_ = av_frame_alloc();
    if (!frame_)
    {
        std::cerr << "Could not allocate frame." << std::endl;
        return false;
    }

    return true;
}

AVFrame* VideoDecoder::processPacket(uint8_t* data, int size)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        printf("Failed to allocate AVPacket.\n");
        return nullptr;
    }
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = size;

    int ret = avcodec_send_packet(ctx_, pkt);
    if (ret < 0) {
        printf("Error sending packet: %d\n", ret);
        return nullptr;
    }

    ret = avcodec_receive_frame(ctx_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return nullptr;
    if (ret < 0) {
        printf("Error receiving frame: %d\n", ret);
        return nullptr;
    }

    return frame_;
}
