#pragma once
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoDecoder
{
public:
    enum class CodecType { H264, H265 };

    VideoDecoder();
    ~VideoDecoder();

    // Initialize the decoder for H264 or H265
    bool initialize(CodecType type);

    // Decode a raw packet
    AVFrame* processPacket(uint8_t* data, int size);

private:
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
};
