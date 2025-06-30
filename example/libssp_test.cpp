#define _GLIBCXX_USE_CXX11_ABI 1
#include <functional>
#include <memory>
#include <thread>

#include <stdlib.h>
#include <iostream>
#include "imf/net/loop.h"
#include "imf/net/threadloop.h"
#include "imf/ssp/sspclient.h"

using namespace std::placeholders;

#ifdef _DEBUG
#pragma comment (lib, "libsspd.lib")
#else
#pragma comment (lib, "libssp.lib")
#endif


extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavcodec/bsf.h>
}

#include <Processing.NDI.Lib.h>
#include "VideoPipeline.h"
#include "VideoDecoder.h"

static AVCodecContext* codec_ctx_265 = nullptr;
static const AVCodec* codec_265 = nullptr;
static AVCodecContext* codec_ctx_264 = nullptr;
static const AVCodec* codec_264 = nullptr;
NDIlib_send_instance_t ndi_sender_1;
NDIlib_send_instance_t ndi_sender_2;
static SwsContext* sws_ctx_1 = nullptr;
static SwsContext* sws_ctx_2 = nullptr;


void init_264_decoder() {
	codec_264 = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec_264) {
		printf("Codec not found\n");
		return;
	}
	codec_ctx_264 = avcodec_alloc_context3(codec_264);
	if (!codec_ctx_264) {
		printf("Could not allocate codec context\n");
		return;
	}
	if (avcodec_open2(codec_ctx_264, codec_264, nullptr) < 0) {
		printf("Could not open codec\n");
		return;
	}
	printf("Successfully Opened H264 Codec\n");
}

void initialize_decoder_and_ndi(int srcWidth, int srcHeight, int dstWidth, int dstHeight)
{
	// Initialize NDI
	if (!NDIlib_initialize()) {
		printf("NDI initialization failed.\n");
		exit(1);
	}

	// Create NDI sender
	NDIlib_send_create_t NDI_send_create_desc_1;
	NDI_send_create_desc_1.p_ndi_name = "Khel_NDI_1";
	ndi_sender_1 = NDIlib_send_create(&NDI_send_create_desc_1);
	if (!ndi_sender_1) {
		printf("NDI sender creation failed.\n");
		exit(1);
	}

	NDIlib_send_create_t NDI_send_create_desc_2;
	NDI_send_create_desc_2.p_ndi_name = "Khel_NDI_2";
	ndi_sender_2 = NDIlib_send_create(&NDI_send_create_desc_2);
	if (!ndi_sender_2) {
		printf("NDI sender creation failed.\n");
		exit(1);
	}

	printf("NDI Sender Creation Successful \n");

	// Create swscale context
	sws_ctx_1 = sws_getContext(
		srcWidth, srcHeight,
		AV_PIX_FMT_YUV420P,
		dstWidth, dstHeight,
		AV_PIX_FMT_BGRA,
		SWS_BILINEAR,
		nullptr, nullptr, nullptr);

	if (!sws_ctx_1) {
		printf("Failed to create SwsContext.\n");
		exit(1);
	}

	sws_ctx_2 = sws_getContext(
		srcWidth, srcHeight,
		AV_PIX_FMT_YUV420P,
		dstWidth, dstHeight,
		AV_PIX_FMT_BGRA,
		SWS_BILINEAR,
		nullptr, nullptr, nullptr);

	if (!sws_ctx_2) {
		printf("Failed to create SwsContext.\n");
		exit(1);
	}
	printf("Created wsContext.\n");

}


static void on_264(struct imf::SspH264Data* h264)
{
	printf("on 264 [frame: %d] [pts: %lld] [type: %d] [len: %d]\n",
		h264->frm_no, h264->pts, h264->type, h264->len);

	if (!codec_264) {
		printf("Decoder not initialized!\n");
		return;
	}

	AVPacket* pkt = av_packet_alloc();
	if (!pkt) {
		printf("Failed to allocate AVPacket.\n");
		return;
	}
	pkt->data = h264->data;
	pkt->size = h264->len;

	int ret = avcodec_send_packet(codec_ctx_264, pkt);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, errbuf, sizeof(errbuf));
		printf("Error sending packet to decoder: %s\n", errbuf);
		av_packet_free(&pkt);
		return;
	}

	AVFrame* frame = av_frame_alloc();
	static AVFrame* rgb_frame = nullptr;
	static int last_width = 0;
	static int last_height = 0;

	if (!frame) {
		printf("Failed to allocate AVFrame.\n");
		av_packet_free(&pkt);
		return;
	}

	while (true) {
		ret = avcodec_receive_frame(codec_ctx_264, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		else if (ret < 0) {
			char errbuf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, errbuf, sizeof(errbuf));
			printf("Error during decoding: %s\n", errbuf);
			break;
		}

		printf("Decoded frame %d size %dx%d\n", h264->frm_no, frame->width, frame->height);

		// Allocate/reallocate RGB frame and SwsContext if needed
		if (!rgb_frame || frame->width != last_width || frame->height != last_height) {
			if (rgb_frame) {
				av_frame_free(&rgb_frame);
			}
			if (sws_ctx_2) {
				sws_freeContext(sws_ctx_2);
			}

			rgb_frame = av_frame_alloc();
			if (!rgb_frame) {
				printf("Failed to allocate RGB frame.\n");
				break;
			}

			rgb_frame->format = AV_PIX_FMT_BGRA;
			rgb_frame->width = frame->width;
			rgb_frame->height = frame->height;

			if (av_frame_get_buffer(rgb_frame, 32) < 0) {
				printf("Failed to allocate RGB frame buffer.\n");
				break;
			}

			sws_ctx_2 = sws_getContext(
				frame->width, frame->height,
				(AVPixelFormat)frame->format,
				rgb_frame->width, rgb_frame->height,
				AV_PIX_FMT_BGRA,
				SWS_BILINEAR,
				nullptr, nullptr, nullptr);

			if (!sws_ctx_2) {
				printf("Failed to create SwsContext.\n");
				break;
			}

			last_width = frame->width;
			last_height = frame->height;

			printf("Initialized RGB frame and SwsContext for %dx%d.\n", last_width, last_height);
		}

		printf("RGB 264 Frame Width(%d), height(%d)\n", rgb_frame->width, rgb_frame->height);

		// Convert YUV420P -> BGRA
		sws_scale(
			sws_ctx_2,
			frame->data, frame->linesize,
			0, frame->height,
			rgb_frame->data, rgb_frame->linesize);

		// Prepare NDI frame
		NDIlib_video_frame_v2_t ndi_frame;
		ndi_frame.xres = rgb_frame->width;
		ndi_frame.yres = rgb_frame->height;
		ndi_frame.FourCC = NDIlib_FourCC_type_BGRA;
		ndi_frame.frame_rate_N = 240000; // ~30 fps
		ndi_frame.frame_rate_D = 1000;
		ndi_frame.picture_aspect_ratio = (float)rgb_frame->width / (float)rgb_frame->height;
		ndi_frame.timecode = NDIlib_send_timecode_synthesize;
		ndi_frame.p_data = rgb_frame->data[0];
		ndi_frame.line_stride_in_bytes = rgb_frame->linesize[0];

		// Send over NDI
		NDIlib_send_send_video_v2(ndi_sender_2, &ndi_frame);

		av_frame_unref(frame);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
}

static void on_264_2(struct imf::SspH264Data * h264)
{
	printf("on 2 264 [%d] [%lld] [%d] [%d]\n", h264->frm_no, h264->pts, h264->type, h264->len);
}

static void on_audio_data_1(struct imf::SspAudioData * audio)
{

}

static void on_audio_data_2(struct imf::SspAudioData * audio)
{

}

static void on_meta_1(struct imf::SspVideoMeta *v, struct imf::SspAudioMeta *a, struct imf::SspMeta * m)
{
	printf("on meta 1 wall clock %d", m->pts_is_wall_clock);
	printf("              video %dx%d %d/%d\n", v->width, v->height, v->unit, v->timescale);
	printf("              audio %d\n", a->sample_rate);
}

static void on_meta_2(struct imf::SspVideoMeta *v, struct imf::SspAudioMeta *a, struct imf::SspMeta * m)
{

}

static void on_disconnect()
{
	printf("on disconnect\n");
}

static void setup(imf::Loop* loop)
{

	auto client = std::make_unique<imf::SspClient>("192.168.11.108", loop, 0x400000);
	client->init();

	auto pipeline = std::make_unique<VideoPipeline>(
		"192.168.11.108",
		"Camera_1_NDI",
		VideoDecoder::CodecType::H264);

	if (!pipeline->initialize())
	{
		printf("Pipeline failed to initialize\n");
		return;
	}


	// Wiring callback
	client->setOnH264DataCallback(
		[pipelinePtr = pipeline.get()](struct imf::SspH264Data* h264)
		{
			pipelinePtr->processPacket(h264->data, h264->len, h264->frm_no);
		});

	// Other callbacks
	client->setOnMetaCallback([](auto, auto, auto) {});
	client->setOnDisconnectedCallback([]() { printf("Disconnected\n"); });

	client->start();

	// If you have multiple pipelines/clients, store them somewhere (e.g., std::vector)
}

#if 0
static void setup(imf::Loop *loop)
{
	std::string ip = "192.168.11.108";
	imf::SspClient * client = new imf::SspClient(ip, loop, 0x400000);
	client->init();

	client->setOnH264DataCallback(std::bind(on_264, _1));
	client->setOnMetaCallback(std::bind(on_meta_1, _1, _2, _3));
	client->setOnDisconnectedCallback(std::bind(on_disconnect));
	client->setOnAudioDataCallback(std::bind(on_audio_data_1, _1));

	client->start();

	////////////////////
	//client = new imf::SspClient(std::string("10.98.32.2"), loop, 0x400000);
	//client->init();

	//client->setOnH264DataCallback(std::bind(on_264_2, _1));
	//client->setOnMetaCallback(std::bind(on_meta_2, _1, _2, _3));
	//client->setOnDisconnectedCallback(std::bind(on_disconnect));
	//client->setOnAudioDataCallback(std::bind(on_audio_data_2, _1));

	//client->start();

	// add more SspClient if you need
}
#endif

int main(int argc, char ** argv)
{
	std::unique_ptr<imf::ThreadLoop> threadLooper(new imf::ThreadLoop(std::bind(setup, _1)));
	threadLooper->start();


	while (1) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	threadLooper->stop();
	return 0;
}

