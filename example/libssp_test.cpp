#define _GLIBCXX_USE_CXX11_ABI 1
#include <functional>
#include <memory>
#include <thread>

#include <stdlib.h>
#include <iostream>
#include "imf/net/loop.h"
#include "imf/net/threadloop.h"
#include "imf/ssp/sspclient.h"
#include <mutex>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <queue>
#include "nv12_to_bgra.cuh"
#include <spdlog/spdlog.h>
#include "Logger.h"
#include "mdns_exports.h"

using namespace std::placeholders;
using json = nlohmann::json;

#ifdef _DEBUG
#pragma comment (lib, "libsspd.lib")
#else
#pragma comment (lib, "libssp.lib")
#endif
#define MAX_QUEUE_SIZE 10
#define MAX_IP_ADDRESSES 16

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavcodec/bsf.h>
#include <libavutil/error.h> // For av_err2str
}

// fix temporary array error in c++1x
#ifdef av_err2str
#undef av_err2str
av_always_inline char* av_err2str(int errnum)
{
	// static char str[AV_ERROR_MAX_STRING_SIZE];
	// thread_local may be better than static in multi-thread circumstance
	thread_local char str[AV_ERROR_MAX_STRING_SIZE];
	memset(str, 0, sizeof(str));
	return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}
#endif


#include <Processing.NDI.Lib.h>
std::atomic<bool> running(true);
std::mutex out_mutex;

enum class DecoderType {
	SOFTWARE,
	HW_CUDA
};

enum class HWCodecType {
	H264_CUVID,
	HEVC_CUVID
};


class ZCamStreamBuilder {
public:
	ZCamStreamBuilder(const std::string& ip) : ip_(ip) {
		params_["send_stream"] = "Stream0";
	}
	ZCamStreamBuilder& index(const std::string& index) {
		params_["send_stream"] = index; 
		return *this; 
	}
	ZCamStreamBuilder& resolution(int w, int h) { 
		params_["width"] = std::to_string(w); 
		params_["height"] = std::to_string(h); 
		return *this; 
	}
	ZCamStreamBuilder& bitrate(int bps) { 
		params_["bitrate"] = std::to_string(bps); 
		return *this; 
	}
	ZCamStreamBuilder& encoder(const std::string& enc) { 
		params_["venc"] = enc; 
		return *this; 
	}
	ZCamStreamBuilder& fps(int value) { 
		params_["fps"] = std::to_string(value); 
		return *this; 
	}
	ZCamStreamBuilder& movfmt(const std::string& movfmt) { 
		params_["k=movfmt"] = movfmt; 
		return *this; 
	}
	ZCamStreamBuilder& movvfr(const std::string& movvfr) { 
		params_["k=movvfr"] = movvfr; 
		return *this; 
	}
	bool apply() {
		CURL* curl = curl_easy_init();
		if (!curl) return false;
		std::string url = "http://" + ip_ + "/ctrl/set?";
		bool first = true;
		for (const auto& pair : params_) {
			if (!first) url += "&";
			char* key = curl_easy_escape(curl, pair.first.c_str(), 0);
			char* value = curl_easy_escape(curl, pair.second.c_str(), 0);
			url += std::string(key) + "=" + value;
			curl_free(key);
			curl_free(value);
			first = false;
		}
		std::string response_data;
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 500L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
		CURLcode res = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		if (res != CURLE_OK) {
			fprintf(stderr, "CURL error: %s\n", curl_easy_strerror(res));
			return false;
		}
		try {
			auto j = json::parse(response_data);
			return j.contains("code") && j["code"] == 0;
		}
		catch (const std::exception& e) {
			fprintf(stderr, "JSON parse error: %s\n", e.what());
			return false;
		}
	}
private:
	std::string ip_;
	std::map<std::string, std::string> params_;
	static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
		((std::string*)userp)->append((char*)contents, size * nmemb);
		return size * nmemb;
	}
};


bool is_zcam(const std::string& ip) {
	std::string url = "http://" + ip + "/info";
	CURL* curl = curl_easy_init();
	if (!curl) return false;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) {
		std::string* response = static_cast<std::string*>(userdata);
		response->append(ptr, size * nmemb);
		return size * nmemb;
		});
	std::string response;
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if (res == CURLE_OK && response.find("\"model\"") != std::string::npos) {
		std::lock_guard<std::mutex> lock(out_mutex);
		spdlog::info("Found Z CAM : {}", ip);
		return true;
	}
	return false;
}

void scan_range(const std::string& base, int start, int end, std::vector<std::string>& cameras) {
	for (int i = start; i <= end; ++i) {
		std::string ip = base + std::to_string(i);
		if (is_zcam(ip)) {
			std::lock_guard<std::mutex> lock(out_mutex);
			cameras.push_back(ip);
		}
	}
}

int DetectZCam() {
	std::vector<std::string> found;
	std::vector<std::thread> threads;
	std::string base = "192.168.11.";
	for (int i = 1; i <= 254; i += 32)
		threads.emplace_back(scan_range, base, i, ((i + 31) < 254 ? (i + 31) : 254), std::ref(found));
	for (auto& t : threads) t.join();
	spdlog::info("Z CAMs Found {}: ", found.size());

	for (int i = 0; i < found.size(); ++i) {
		ZCamStreamBuilder builder(found[i]);
		bool success = builder.index("Stream0")
			.fps(30)
			.bitrate(10000000)
			.apply();

		if (success) {
			spdlog::info("Stream settings applied successfully");
		}else {
			spdlog::error("Failed to apply stream settings for {}: ", found[i]);
			return -1;
		}
	}
	return 0;
}


struct ClientContext {
	int client_id;                        // Unique ID per client
	std::string name;                    // NDI sender name

	DecoderType type = DecoderType::SOFTWARE;
	HWCodecType hwCodecType = HWCodecType::H264_CUVID;

	// FFmpeg decoding
	const AVCodec* video_codec = nullptr;
	AVCodecContext* video_codec_ctx = nullptr;
	AVBufferRef* hw_device_ctx = nullptr;  // Only used in HW mode

	// Conversion (YUV → BGRA)
	SwsContext* sws_ctx = nullptr;

	// RGB frame for rendering or NDI
	AVFrame* rgb_frame = nullptr;
	AVFrame* audio_frame = nullptr;
	int last_width = 0;
	int last_height = 0;

	// NDI sender
	NDIlib_send_instance_t ndi_sender = nullptr;

	// Thread safety
	std::mutex decoder_mutex;

	std::thread ndi_thread;

	// Async decoding additions
	std::queue<AVPacket*> video_packet_queue;
	std::mutex video_packet_mutex;
	std::condition_variable video_packet_queue_cv;
	std::thread video_thread;


	std::queue<AVPacket*> audio_packet_queue;
	std::mutex audio_packet_mutex;
	std::condition_variable audio_packet_queue_cv;
	std::thread audio_thread;
	AVCodecContext* audio_codec_ctx = nullptr;


	//std::queue<AVFrame*> frame_queue;
	//std::mutex queue_mutex;
	//std::condition_variable queue_cv;

	bool seen_keyframe = false;  // Track if we've seen a keyframe

	std::string ip;
	DecoderType decoder_type;
	std::string ndi_name;

	std::mutex audio_decoder_mutex;

	std::atomic<bool> audio_running = true;
	std::atomic<bool> video_running = true;
};

static std::vector<std::unique_ptr<ClientContext>> g_client_contexts;
static std::vector<std::unique_ptr<imf::SspClient>> g_ssp_clients;


static void handle_meta_data(ClientContext& ctx, struct imf::SspVideoMeta* v, struct imf::SspAudioMeta* a, struct imf::SspMeta* m)
{
	spdlog::info("on meta {} wall clock {} video {} {} {} {} audio {}", ctx.client_id, m->pts_is_wall_clock, v->width, v->height,
		v->unit, v->timescale, a->sample_rate);
}


static void handle_h264_data(ClientContext& ctx, struct imf::SspH264Data* h264) {
	if (!h264 || !h264->data || h264->len == 0) {
		printf("[Decoder %d] Invalid H264 data received\n", ctx.client_id);
		return;
	}

	AVPacket* pkt = av_packet_alloc();
	if (!pkt) {
		printf("[Decoder %d] Failed to allocate AVPacket\n", ctx.client_id);
		return;
	}

	pkt->dts = h264->pts;
	pkt->pts = h264->pts;
	pkt->flags = (h264->type == 0) ? AV_PKT_FLAG_KEY : 0; // 0 for I-frame, non-zero for P-frame

	pkt->data = (uint8_t*)av_memdup(h264->data, h264->len);
	pkt->size = (int)h264->len;

	std::lock_guard<std::mutex> lock(ctx.video_packet_mutex);
	ctx.video_packet_queue.push(pkt);

	ctx.video_packet_queue_cv.notify_one();
}


static void handle_audio_data(ClientContext& ctx, struct imf::SspAudioData* audio) {
	if (!audio || !audio->data || audio->len == 0) {
		printf("[Decoder %d] Invalid audio data received\n", ctx.client_id);
		return;
	}

	AVPacket* pkt = av_packet_alloc();
	if (!pkt) {
		printf("[Decoder %d] Failed to allocate AVPacket\n", ctx.client_id);
		return;
	}

	pkt->dts = audio->pts;
	pkt->pts = audio->pts;

	pkt->data = (uint8_t*)av_memdup(audio->data, audio->len);
	pkt->size = (int)audio->len;

	std::lock_guard<std::mutex> lock(ctx.audio_packet_mutex);
	ctx.audio_packet_queue.push(pkt);

	ctx.audio_packet_queue_cv.notify_one();
}

static void handle_disconnect(ClientContext& ctx) {
	spdlog::error("on Disconnect");
}



bool init_video_decoder(ClientContext& ctx) {
	std::string video_decoder;
	if (ctx.type == DecoderType::HW_CUDA) {
		if (ctx.hwCodecType == HWCodecType::H264_CUVID) {
			video_decoder = "h264_cuvid";
		}else if (ctx.hwCodecType == HWCodecType::HEVC_CUVID) {
			video_decoder = "hevc_cuvid";
		}else {
			return false;
		}
		ctx.video_codec = avcodec_find_decoder_by_name(video_decoder.c_str());  // or "h264_nvdec"
		if (!ctx.video_codec) {
			printf("Client[%d] CUDA decoder not found\n", ctx.client_id);
			return false;
		}

		ctx.video_codec_ctx = avcodec_alloc_context3(ctx.video_codec);
		if (!ctx.video_codec_ctx) return false;

		if (av_hwdevice_ctx_create(&ctx.hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
			printf("Failed to create CUDA HW device for client %d\n", ctx.client_id);
			return false;
		}
		ctx.video_codec_ctx->hw_device_ctx = av_buffer_ref(ctx.hw_device_ctx);

		ctx.video_codec_ctx->get_format = [](AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
			while (*pix_fmts != AV_PIX_FMT_NONE) {
				if (*pix_fmts == AV_PIX_FMT_CUDA)
					return *pix_fmts;
				pix_fmts++;
			}
			return pix_fmts[0];
			};
	}
	else {  // SOFTWARE
		if (ctx.hwCodecType == HWCodecType::H264_CUVID) {
			ctx.video_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		}else if (ctx.hwCodecType == HWCodecType::HEVC_CUVID) {
			ctx.video_codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
		}else {
			return false;
		}
		if (!ctx.video_codec) {
			printf("Client[%d] software decoder not found\n", ctx.client_id);
			return false;
		}

		ctx.video_codec_ctx = avcodec_alloc_context3(ctx.video_codec);
		if (!ctx.video_codec_ctx) return false;
	}

	printf("before avcodec_open2");

	int ret = avcodec_open2(ctx.video_codec_ctx, ctx.video_codec, nullptr);
	if (ret < 0) {
		printf("Client[%d] Could not open codec: %s\n", ctx.client_id, av_err2str(ret));
		return false;
	}

	printf("Client[%d] decoder (%s) initialized.\n", ctx.client_id,
		(ctx.type == DecoderType::HW_CUDA ? "HW_CUDA" : "SOFTWARE"));
	return true;
}

bool init_audio_decoder(ClientContext& ctx, AVCodecID codec_id = AV_CODEC_ID_AAC) {
	const AVCodec* codec = avcodec_find_decoder(codec_id);
	if (!codec) {
		printf("Client[%d] Audio decoder not found for codec id %d\n", ctx.client_id, codec_id);
		return false;
	}

	ctx.audio_codec_ctx = avcodec_alloc_context3(codec);
	if (!ctx.audio_codec_ctx) {
		printf("Client[%d] Failed to alloc audio_codec_ctx\n", ctx.client_id);
		return false;
	}

	// Optional: set sample_rate, channels, channel_layout if known.
	// ctx.audio_codec_ctx->sample_rate = ...;
	// ctx.audio_codec_ctx->channels = ...;

	int ret = avcodec_open2(ctx.audio_codec_ctx, codec, nullptr);
	if (ret < 0) {
		char errbuf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(ret, errbuf, sizeof(errbuf));
		printf("Client[%d] Could not open audio codec: %s\n", ctx.client_id, errbuf);
		return false;
	}

	printf("Client[%d] Audio decoder initialized.\n", ctx.client_id);
	return true;
}


void send_video_to_ndi(ClientContext* ctx, AVFrame* frame) {
	if (!ctx || !frame) return;
	NDIlib_video_frame_v2_t ndi_frame = {};
	ndi_frame.xres = frame->width;
	ndi_frame.yres = frame->height;
	ndi_frame.FourCC = NDIlib_FourCC_type_BGRA;
	ndi_frame.frame_rate_N = 240000;
	ndi_frame.frame_rate_D = 1001;
	ndi_frame.picture_aspect_ratio = (float)frame->width / frame->height;
	ndi_frame.timecode = NDIlib_send_timecode_synthesize;
	ndi_frame.p_data = frame->data[0];
	ndi_frame.line_stride_in_bytes = frame->linesize[0];
	NDIlib_send_send_video_async_v2(ctx->ndi_sender, &ndi_frame);
}

void send_audio_to_ndi(ClientContext* ctx, AVFrame* frame) {
	if (!ctx || !frame || frame->format != AV_SAMPLE_FMT_FLTP) return;

	AVChannelLayout layout = frame->ch_layout;
	int num_channels = layout.nb_channels;
	int num_samples = frame->nb_samples;

	int channel_stride = num_samples * sizeof(float);
	int total_size = channel_stride * num_channels;

	static std::vector<uint8_t> audio_buffer;
	if (audio_buffer.size() < total_size)
		audio_buffer.resize(total_size);

	for (int ch = 0; ch < num_channels; ++ch) {
		memcpy(audio_buffer.data() + ch * channel_stride,
		       frame->extended_data[ch],
		       channel_stride);
	}

	NDIlib_audio_frame_v3_t audio_frame = {};
	audio_frame.sample_rate = frame->sample_rate;
	audio_frame.no_channels = num_channels;
	audio_frame.no_samples = num_samples;
	audio_frame.channel_stride_in_bytes = channel_stride;
	audio_frame.FourCC = NDIlib_FourCC_audio_type_FLTP;
	audio_frame.timecode = NDIlib_send_timecode_synthesize;
	audio_frame.p_data = audio_buffer.data();

	NDIlib_send_send_audio_v3(ctx->ndi_sender, &audio_frame);
}

void decode_audio_thread_func(ClientContext* ctx) {
	while (ctx->audio_running) {
		AVPacket* pkt = nullptr;

		{
			std::unique_lock<std::mutex> lock(ctx->audio_packet_mutex);
			ctx->audio_packet_queue_cv.wait(lock, [&] {
				return !ctx->audio_packet_queue.empty() || !ctx->audio_running;
				});

			if (!ctx->audio_running && ctx->audio_packet_queue.empty())
				break;

			pkt = ctx->audio_packet_queue.front();
			ctx->audio_packet_queue.pop();
		}

		{
			std::lock_guard<std::mutex> decoder_lock(ctx->audio_decoder_mutex);
			if (!ctx->audio_codec_ctx) {
				av_packet_free(&pkt);
				continue;
			}

			int ret = avcodec_send_packet(ctx->audio_codec_ctx, pkt);
			if (ret < 0) {
				fprintf(stderr, "[AudioDecoder %d] send_packet failed: %s\n", ctx->client_id, av_err2str(ret));
				av_packet_free(&pkt);
				continue;
			}

			AVFrame* frame = av_frame_alloc();

			while (true) {
				int ret = avcodec_receive_frame(ctx->audio_codec_ctx, frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					break;
				if (ret < 0) break;

				send_audio_to_ndi(ctx, frame);
				av_frame_unref(frame);

			}
			av_frame_free(&frame);
		}

		av_packet_free(&pkt);
	}
}

void decode_video_thread_func(ClientContext* ctx) {
	while (ctx->video_running) {
		AVPacket* pkt = nullptr;

		// Wait for incoming video packet
		{
			std::unique_lock<std::mutex> lock(ctx->video_packet_mutex);
			ctx->video_packet_queue_cv.wait(lock, [&] {
				return !ctx->video_packet_queue.empty() || !ctx->video_running;
				});

			if (!ctx->video_running && ctx->video_packet_queue.empty())
				break;

			pkt = ctx->video_packet_queue.front();
			ctx->video_packet_queue.pop();
		}

		// Decode the packet
		{
			std::lock_guard<std::mutex> decoder_lock(ctx->decoder_mutex);

			if (!ctx->video_codec_ctx || !pkt) {
				av_packet_free(&pkt);
				continue;
			}

			int ret = avcodec_send_packet(ctx->video_codec_ctx, pkt);
			if (ret < 0) {
				fprintf(stderr, "[Decoder %d] avcodec_send_packet failed: %s\n",
					ctx->client_id, av_err2str(ret));
				av_packet_free(&pkt);
				continue;
			}

			AVFrame* decoded_frame = av_frame_alloc();
			if (!decoded_frame) {
				av_packet_free(&pkt);
				continue;
			}

			AVFrame* hw_frame = nullptr;

			while (true) {
				ret = avcodec_receive_frame(ctx->video_codec_ctx, decoded_frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					break;
				if (ret < 0) {
					fprintf(stderr, "[Decoder %d] avcodec_receive_frame failed: %s\n",
						ctx->client_id, av_err2str(ret));
					break;
				}

				AVFrame* input_frame = decoded_frame;

				// Handle HW → SW frame transfer (if CUDA decoding)
				if (ctx->type == DecoderType::HW_CUDA) {
					if (!hw_frame) hw_frame = av_frame_alloc();

					ret = av_hwframe_transfer_data(hw_frame, decoded_frame, 0);
					if (ret < 0) {
						fprintf(stderr, "[Decoder %d] av_hwframe_transfer_data failed: %s\n",
							ctx->client_id, av_err2str(ret));
						break;
					}

					input_frame = hw_frame;
				}

				// Allocate or reallocate BGRA frame if resolution changed
				if (!ctx->rgb_frame ||
					ctx->last_width != input_frame->width ||
					ctx->last_height != input_frame->height) {

					if (ctx->rgb_frame)
						av_frame_free(&ctx->rgb_frame);

					ctx->rgb_frame = av_frame_alloc();
					ctx->rgb_frame->format = AV_PIX_FMT_BGRA;
					ctx->rgb_frame->width = input_frame->width;
					ctx->rgb_frame->height = input_frame->height;

					if (av_frame_get_buffer(ctx->rgb_frame, 32) < 0) {
						fprintf(stderr, "[Decoder %d] Failed to allocate RGB frame\n", ctx->client_id);
						break;
					}

					ctx->last_width = input_frame->width;
					ctx->last_height = input_frame->height;
				}

				// Perform color conversion using GPU
				bool ok = ConvertAVFrameToBGRA(
					input_frame->data[0], input_frame->linesize[0],
					input_frame->data[1], input_frame->linesize[1],
					input_frame->width, input_frame->height,
					ctx->rgb_frame->data[0], ctx->rgb_frame->linesize[0]);

				if (!ok) {
					fprintf(stderr, "[Decoder %d] GPU color conversion failed\n", ctx->client_id);
					break;
				}

				// Send the converted frame to NDI
				send_video_to_ndi(ctx, ctx->rgb_frame);

				// (Optional) Logging
				// printf("[Decoder %d] Frame sent: %dx%d\n", ctx->client_id, input_frame->width, input_frame->height);

				av_frame_unref(decoded_frame);
				if (hw_frame) av_frame_unref(hw_frame);
			}

			av_frame_free(&decoded_frame);
			if (hw_frame) av_frame_free(&hw_frame);
		}

		av_packet_free(&pkt);
	}
}

// Define client inputs (IP + decoder type)
typedef struct {
	std::string ip;
	DecoderType decoder_type;
	HWCodecType hwCodecType;
	std::string ndi_name;
} client_inputs;


static void setup(imf::Loop* loop)
{
	char* results[MAX_IP_ADDRESSES];
	int client_count = mdns_discover_ips(results, MAX_IP_ADDRESSES);
	for (int i = 0; i < client_count; ++i) {
		printf("Pavankn Discovered IP: %s\n", results[i]);
	}

	client_inputs* clientInputs = new client_inputs[client_count];	

	for (int i = 0; i < client_count; ++i) {

		clientInputs[i].ip = results[i];
		clientInputs[i].decoder_type = DecoderType::HW_CUDA;
		clientInputs[i].hwCodecType = HWCodecType::HEVC_CUVID;
		clientInputs[i].ndi_name = "Khel_NDI_" + std::to_string(i + 1);

		const auto& input = clientInputs[i];

		// Create ClientContext
		auto ctx = std::make_unique<ClientContext>();
		ctx->client_id = i;
		ctx->type = input.decoder_type;
		ctx->name = input.ndi_name;
		ctx->hwCodecType = input.hwCodecType;
		ctx->ip = input.ip;

		if(results[i] != nullptr) {
			free(results[i]);
		}

		// Initialize decoder
		if (!init_video_decoder(*ctx)) {
			spdlog::info("Video Decoder init failed for client {}", i);
			continue;
		}

		if (!init_audio_decoder(*ctx)) {
			spdlog::info("Audio Decoder init failed for client {}", i);
			continue;
		}

		// Initialize NDI sender
		NDIlib_send_create_t NDI_send_create_desc;
		NDI_send_create_desc.p_ndi_name = ctx->name.c_str();
		ctx->ndi_sender = NDIlib_send_create(&NDI_send_create_desc);
		if (!ctx->ndi_sender) {
			spdlog::error("NDI Sender init failed for client {}", i);
			continue;
		}

		//ctx->ndi_thread = std::thread(ndi_sender_thread, ctx.get());
		ctx->video_thread = std::thread(decode_video_thread_func, ctx.get());
		ctx->audio_thread = std::thread(decode_audio_thread_func, ctx.get());

		// Save context pointer for lambda
		ClientContext* ctx_ptr = ctx.get();

		// Create SspClient
		auto client = std::make_unique<imf::SspClient>(input.ip, loop, 0x400000);
		client->init();
		client->setOnH264DataCallback([ctx_ptr](imf::SspH264Data* h264) {
			handle_h264_data(*ctx_ptr, h264);
			});

		client->setOnMetaCallback([ctx_ptr](imf::SspVideoMeta* v, imf::SspAudioMeta* a, imf::SspMeta* m) {
			handle_meta_data(*ctx_ptr, v, a, m);
			});

		client->setOnDisconnectedCallback([ctx_ptr]() {
			handle_disconnect(*ctx_ptr);
			});		

		client->setOnAudioDataCallback([ctx_ptr](imf::SspAudioData* audio) {
			handle_audio_data(*ctx_ptr, audio);
			});

		client->start();

		// Save for lifetime management
		g_client_contexts.push_back(std::move(ctx));
		g_ssp_clients.push_back(std::move(client));

		spdlog::info("Started client {} {} {}", i, input.ip.c_str(),
			input.decoder_type == DecoderType::HW_CUDA ? "HW_CUDA" : "SW");
	}
}

void cleanup_clients() {
	for (auto& ctx : g_client_contexts) {
		ctx->video_running = false;
		ctx->audio_running = false;

		ctx->audio_packet_queue_cv.notify_all();
		if (ctx->audio_thread.joinable())
			ctx->audio_thread.join();

		ctx->video_packet_queue_cv.notify_all();
		if (ctx->video_thread.joinable())
			ctx->video_thread.join();

		if (ctx->ndi_sender)
			NDIlib_send_destroy(ctx->ndi_sender);
		if (ctx->video_codec_ctx)
			avcodec_free_context(&ctx->video_codec_ctx);
		if (ctx->audio_codec_ctx)
			avcodec_free_context(&ctx->audio_codec_ctx);
		if (ctx->hw_device_ctx)
			av_buffer_unref(&ctx->hw_device_ctx);
		if (ctx->sws_ctx)
			sws_freeContext(ctx->sws_ctx);
		if (ctx->rgb_frame)
			av_frame_free(&ctx->rgb_frame);
	}
	g_client_contexts.clear();
	g_ssp_clients.clear();
}

void handle_sigint(int) {
	running = false;
}

int main(int argc, char ** argv)
{
	signal(SIGINT, handle_sigint);

	init_logger();
	spdlog::info("App started");

	std::unique_ptr<imf::ThreadLoop> threadLooper(new imf::ThreadLoop(std::bind(setup, _1)));
	threadLooper->start();

	while (running) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	cleanup_clients();	
	threadLooper->stop();
	return 0;
}

