#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

class H264Decoder
{
public:
    H264Decoder();
    ~H264Decoder();

    bool initialize();
    // Returns decoded AVFrame, or nullptr if none ready
    AVFrame* processPacket(const uint8_t* data, int size);

private:
    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
};
