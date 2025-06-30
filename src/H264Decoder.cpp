#include "H264Decoder.h"
#include <cstdio>

H264Decoder::H264Decoder()
{
}

H264Decoder::~H264Decoder()
{
    if (frame_)
        av_frame_free(&frame_);
    if (codec_ctx_)
        avcodec_free_context(&codec_ctx_);
}

bool H264Decoder::initialize()
{
    codec_ = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec_) {
        printf("Codec not found\n");
        return false;
    }
    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        printf("Could not allocate codec context\n");
        return false;
    }
    if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
        printf("Could not open codec\n");
        return false;
    }
    frame_ = av_frame_alloc();
    return true;
}

AVFrame* H264Decoder::processPacket(const uint8_t* data, int size)
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        printf("Failed to allocate AVPacket.\n");
        return nullptr;
    }
    pkt->data = const_cast<uint8_t*>(data);
    pkt->size = size;

    int ret = avcodec_send_packet(codec_ctx_, pkt);
    if (ret < 0) {
        printf("Error sending packet: %d\n", ret);
        return nullptr;
    }

    ret = avcodec_receive_frame(codec_ctx_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return nullptr;
    if (ret < 0) {
        printf("Error receiving frame: %d\n", ret);
        return nullptr;
    }

    return frame_;
}
