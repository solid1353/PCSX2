// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSCapture.h"
#include "GS/GSPng.h"
#include "GS/GSUtil.h"
#include "GS/GSExtra.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "SPU2/spu2.h"
#include "Host.h"
#include "Host/AudioStream.h"
#include "IconsFontAwesome.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/BitUtils.h"
#include "common/DynamicLibrary.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SmallString.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

// We're using deprecated fields because we're targeting multiple ffmpeg versions.
#if defined(_MSC_VER)
#pragma warning(disable : 4996) // warning C4996: 'AVCodecContext::channels': was declared deprecated
#elif defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#ifdef _WIN32
// Before FFmpeg's D3D11 header, which includes d3d11.h inside its extern "C" block.
#include "common/RedtapeWindows.h"
#include <d3d11_4.h>
#include <dxgi1_4.h>
#endif

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/version.h"
#include "libavformat/avformat.h"
#include "libavformat/version.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"
#include "libavutil/version.h"
#include "libavutil/channel_layout.h"
#include "libavutil/pixdesc.h"
#include "libswscale/swscale.h"
#include "libswscale/version.h"
#include "libswresample/swresample.h"
#include "libswresample/version.h"
#ifdef _WIN32
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"
#endif
}

#ifdef _WIN32
#include "GS/Renderers/DX11/D3D.h"
#include "GS/Renderers/DX12/GSDevice12.h"
#endif

// Compatibility with both ffmpeg 4.x and 5.x.
#if (LIBAVFORMAT_VERSION_MAJOR < 59)
#define ff_const59
#else
#define ff_const59 const
#endif

#define VISIT_AVCODEC_IMPORTS(X) \
	X(avcodec_find_encoder_by_name) \
	X(avcodec_find_encoder) \
	X(avcodec_alloc_context3) \
	X(avcodec_open2) \
	X(avcodec_free_context) \
	X(avcodec_send_frame) \
	X(avcodec_receive_packet) \
	X(avcodec_parameters_from_context) \
	X(avcodec_get_hw_config) \
	X(av_codec_iterate) \
	X(av_packet_alloc) \
	X(av_packet_free) \
	X(av_packet_rescale_ts) \
	X(av_packet_unref)

#define VISIT_AVFORMAT_IMPORTS(X) \
	X(avformat_alloc_output_context2) \
	X(avformat_new_stream) \
	X(avformat_write_header) \
	X(av_guess_format) \
	X(av_interleaved_write_frame) \
	X(av_write_trailer) \
	X(avformat_free_context) \
	X(avformat_query_codec) \
	X(avio_open) \
	X(avio_closep)

#if LIBAVUTIL_VERSION_MAJOR < 57
#define AVUTIL_57_IMPORTS(X)
#else
#define AVUTIL_57_IMPORTS(X) \
	X(av_channel_layout_default) \
	X(av_channel_layout_copy) \
	X(av_opt_set_chlayout)
#endif

#define VISIT_AVUTIL_IMPORTS(X) \
	AVUTIL_57_IMPORTS(X) \
	X(av_frame_alloc) \
	X(av_frame_get_buffer) \
	X(av_frame_free) \
	X(av_frame_make_writable) \
	X(av_strerror) \
	X(av_reduce) \
	X(av_dict_parse_string) \
	X(av_dict_get) \
	X(av_dict_free) \
	X(av_opt_set_int) \
	X(av_opt_set_sample_fmt) \
	X(av_compare_ts) \
	X(av_get_bytes_per_sample) \
	X(av_sample_fmt_is_planar) \
	X(av_d2q) \
	X(av_hwdevice_get_type_name) \
	X(av_hwdevice_ctx_create) \
	X(av_hwframe_ctx_alloc) \
	X(av_hwframe_ctx_init) \
	X(av_hwframe_transfer_data) \
	X(av_hwframe_get_buffer) \
	X(av_hwdevice_ctx_alloc) \
	X(av_hwdevice_ctx_init) \
	X(av_frame_unref) \
	X(av_buffer_ref) \
	X(av_buffer_unref) \
	X(av_get_pix_fmt_name)

#define VISIT_SWSCALE_IMPORTS(X) \
	X(sws_getCachedContext) \
	X(sws_scale) \
	X(sws_freeContext)

#define VISIT_SWRESAMPLE_IMPORTS(X) \
	X(swr_alloc) \
	X(swr_init) \
	X(swr_free) \
	X(swr_convert) \
	X(swr_next_pts)

namespace GSCapture
{
	static constexpr u32 NUM_FRAMES_IN_FLIGHT = 3;
	static constexpr u32 MAX_PENDING_FRAMES = NUM_FRAMES_IN_FLIGHT * 2;
	static constexpr u32 AUDIO_BUFFER_SIZE = Common::AlignUpPow2((MAX_PENDING_FRAMES * 48000) / 60, AudioStream::CHUNK_SIZE);
	static constexpr u32 AUDIO_CHANNELS = 2;

	struct PendingFrame
	{
		enum class State
		{
			Unused,
			NeedsMap,
			NeedsEncoding
		};

		std::unique_ptr<GSDownloadTexture> tex;
		s64 pts;
		State state;
		// GPU frames: the ready fence value after which the frame's shared texture holds it.
		u64 ready_value;
	};

#ifdef _WIN32
	// Frames for an NVENC encoder under the D3D12 renderer stay on the GPU: the renderer copies each one into a
	// texture it shares with this D3D11 device, which draws it into the encoder's own D3D11 frames. Shared fences
	// order the two devices. Encoder frames are BGRA, the only 8-bit RGB layout FFmpeg's D3D11 frames take.
	struct GpuFrames
	{
		template <typename T>
		using ComPtr = wil::com_ptr_nothrow<T>;

		ComPtr<ID3D11Device5> device;
		ComPtr<ID3D11DeviceContext4> context;
		ComPtr<ID3D11VertexShader> vertex_shader;
		ComPtr<ID3D11PixelShader> pixel_shader;

		// Shared with the renderer's current D3D12 device; rebuilt when that device changes.
		GSDevice12* renderer = nullptr;
		std::vector<ComPtr<ID3D11ShaderResourceView>> views;
		ComPtr<ID3D11Fence> ready_fence;
		ComPtr<ID3D11Fence> free_fence;
		u64 free_value = 0;
		std::array<u64, MAX_PENDING_FRAMES> slot_free_values = {};

		// Render target views of the encoder frame textures, by texture and array slice.
		std::vector<std::pair<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11RenderTargetView>>> targets;
	};
	static std::unique_ptr<GpuFrames> s_gpu_frames;
	static bool BeginGpuFrames(const AVCodec* vcodec);
	static bool ShareGpuFrameTextures();
	static void ReleaseGpuFrameTextures();
	static bool SendGpuFrame(const PendingFrame& pf, u32 slot);
#endif

	static void LogAVError(int errnum, const char* format, ...);
	static bool LoadFFmpeg(bool report_errors);
	static void UnloadFFmpeg();
	static std::string GetCaptureTypeForMessage(bool capture_video, bool capture_audio);
	static bool IsUsingHardwareVideoEncoding();
	static void ProcessFramePendingMap(std::unique_lock<std::mutex>& lock);
	static void ProcessAllInFlightFrames(std::unique_lock<std::mutex>& lock);
	static void EncoderThreadEntryPoint();
	static void StartEncoderThread();
	static void StopEncoderThread(std::unique_lock<std::mutex>& lock);
	static bool SendFrame(const PendingFrame& pf);
	static bool ReceivePackets(AVCodecContext* codec_context, AVStream* stream, AVPacket* packet);
	static bool ProcessAudioPackets(std::optional<s64> video_pts);
	static bool SendPartialReplayAudioFrame();
	static bool InternalEndCapture(std::unique_lock<std::mutex>& lock);
	static CodecList GetCodecListForContainer(const char* container, AVMediaType type);

	static std::mutex s_lock;
	static GSVector2i s_size{};
	static std::string s_filename;
	static std::atomic_bool s_capturing{false};
	static std::atomic_bool s_encoding_error{false};
	static bool s_force_audio_video = false;
	static bool s_recording_replay = false;
	static bool s_wrote_media = false;

	static AVFormatContext* s_format_context = nullptr;

	static AVCodecContext* s_video_codec_context = nullptr;
	static AVStream* s_video_stream = nullptr;
	static AVFrame* s_converted_video_frame = nullptr; // YUV
	static AVFrame* s_hw_video_frame = nullptr;
	static AVPacket* s_video_packet = nullptr;
	static SwsContext* s_sws_context = nullptr;
	static AVDictionary* s_video_codec_arguments = nullptr;
	static AVBufferRef* s_video_hw_context = nullptr;
	static AVBufferRef* s_video_hw_frames = nullptr;
	static s64 s_next_video_pts = 0;

	static AVCodecContext* s_audio_codec_context = nullptr;
	static AVStream* s_audio_stream = nullptr;
	static AVFrame* s_converted_audio_frame = nullptr;
	static AVPacket* s_audio_packet = nullptr;
	static SwrContext* s_swr_context = nullptr;
	static AVDictionary* s_audio_codec_arguments = nullptr;
	static s64 s_next_audio_pts = 0;
	static u32 s_audio_frame_bps = 0;
	static u32 s_audio_frame_size = 0;
	static u32 s_audio_frame_pos = 0;
	static bool s_audio_frame_planar = false;

	static Threading::Thread s_encoder_thread;
	static std::condition_variable s_frame_ready_cv;
	static std::condition_variable s_frame_encoded_cv;
	static std::array<PendingFrame, MAX_PENDING_FRAMES> s_pending_frames = {};
	static u32 s_pending_frames_pos = 0;
	static u32 s_frames_pending_map = 0;
	static u32 s_frames_map_consume_pos = 0;
	static u32 s_frames_pending_encode = 0;
	static u32 s_frames_encode_consume_pos = 0;

	// NOTE: So this doesn't need locking, we allocate it once, and leave it.
	static std::unique_ptr<float[]> s_audio_buffer;
	static std::atomic<u32> s_audio_buffer_size{0};
	static u32 s_audio_buffer_write_pos = 0;
	alignas(__cachelinesize) static u32 s_audio_buffer_read_pos = 0;
} // namespace GSCapture

#ifndef USE_LINKED_FFMPEG
#define DECLARE_IMPORT(X) static decltype(X)* wrap_##X;
#else
#define DECLARE_IMPORT(X) static constexpr decltype(X)* wrap_##X = X;
#endif
VISIT_AVCODEC_IMPORTS(DECLARE_IMPORT);
VISIT_AVFORMAT_IMPORTS(DECLARE_IMPORT);
VISIT_AVUTIL_IMPORTS(DECLARE_IMPORT);
VISIT_SWSCALE_IMPORTS(DECLARE_IMPORT);
VISIT_SWRESAMPLE_IMPORTS(DECLARE_IMPORT);
#undef DECLARE_IMPORT

// We could refcount this, but really, may as well just load it and pay the cost once.
// Not like we need to save a few megabytes of memory...
#ifndef USE_LINKED_FFMPEG
static void UnloadFFmpegFunctions(std::unique_lock<std::mutex>& lock);

static DynamicLibrary s_avcodec_library;
static DynamicLibrary s_avformat_library;
static DynamicLibrary s_avutil_library;
static DynamicLibrary s_swscale_library;
static DynamicLibrary s_swresample_library;
static bool s_library_loaded = false;
static std::mutex s_load_mutex;

bool GSCapture::LoadFFmpeg(bool report_errors)
{
	std::unique_lock lock(s_load_mutex);
	if (s_library_loaded)
		return true;

	const auto open_dynlib = [](DynamicLibrary& lib, const char* name, int major_version) {
		std::string full_name(DynamicLibrary::GetVersionedFilename(name, major_version));
		return lib.Open(full_name.c_str(), nullptr);
	};

	bool result = true;

	result = result && open_dynlib(s_avutil_library, "avutil", LIBAVUTIL_VERSION_MAJOR);
	result = result && open_dynlib(s_avcodec_library, "avcodec", LIBAVCODEC_VERSION_MAJOR);
	result = result && open_dynlib(s_avformat_library, "avformat", LIBAVFORMAT_VERSION_MAJOR);
	result = result && open_dynlib(s_swscale_library, "swscale", LIBSWSCALE_VERSION_MAJOR);
	result = result && open_dynlib(s_swresample_library, "swresample", LIBSWRESAMPLE_VERSION_MAJOR);

#define RESOLVE_IMPORT(X) result = result && s_avcodec_library.GetSymbol(#X, &wrap_##X);
	VISIT_AVCODEC_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_avformat_library.GetSymbol(#X, &wrap_##X);
	VISIT_AVFORMAT_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_avutil_library.GetSymbol(#X, &wrap_##X);
	VISIT_AVUTIL_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_swscale_library.GetSymbol(#X, &wrap_##X);
	VISIT_SWSCALE_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

#define RESOLVE_IMPORT(X) result = result && s_swresample_library.GetSymbol(#X, &wrap_##X);
	VISIT_SWRESAMPLE_IMPORTS(RESOLVE_IMPORT);
#undef RESOLVE_IMPORT

	if (result)
	{
		s_library_loaded = true;
		return true;
	}

	UnloadFFmpegFunctions(lock);
	lock.unlock();

	if (report_errors)
	{
		Host::ReportErrorAsync(TRANSLATE_SV("GSCapture", "Failed to load FFmpeg"),
			fmt::format(TRANSLATE_FS("GSCapture", "You may be missing one or more files, or are using the incorrect version. This build of PCSX2 requires:\n"
												  "  libavcodec: {}\n"
												  "  libavformat: {}\n"
												  "  libavutil: {}\n"
												  "  libswscale: {}\n"
												  "  libswresample: {}\n\n"
												  "Please see our official documentation for more information."),
				LIBAVCODEC_VERSION_MAJOR, LIBAVFORMAT_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR,
				LIBSWSCALE_VERSION_MAJOR, LIBSWRESAMPLE_VERSION_MAJOR));
	}

	return false;
}

void UnloadFFmpegFunctions(std::unique_lock<std::mutex>& lock)
{
#define CLEAR_IMPORT(X) wrap_##X = nullptr;
	VISIT_AVCODEC_IMPORTS(CLEAR_IMPORT);
	VISIT_AVFORMAT_IMPORTS(CLEAR_IMPORT);
	VISIT_AVUTIL_IMPORTS(CLEAR_IMPORT);
	VISIT_SWSCALE_IMPORTS(CLEAR_IMPORT);
	VISIT_SWRESAMPLE_IMPORTS(CLEAR_IMPORT);
#undef CLEAR_IMPORT

	s_swresample_library.Close();
	s_swscale_library.Close();
	s_avutil_library.Close();
	s_avformat_library.Close();
	s_avcodec_library.Close();
}

void GSCapture::UnloadFFmpeg()
{
	std::unique_lock lock(s_load_mutex);
	if (!s_library_loaded)
		return;

	s_library_loaded = false;
	UnloadFFmpegFunctions(lock);
}

#else

bool GSCapture::LoadFFmpeg(bool report_errors)
{
	return true;
}

void GSCapture::UnloadFFmpeg()
{
}

#endif

void GSCapture::LogAVError(int errnum, const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	std::string msg(StringUtil::StdStringFromFormatV(format, ap));
	va_end(ap);

	char errbuf[128];
	wrap_av_strerror(errnum, errbuf, sizeof(errbuf));

	Host::AddIconOSDMessage("GSCaptureError", ICON_FA_CAMERA, fmt::format("{}{} ({})", msg, errbuf, errnum), Host::OSD_ERROR_DURATION);
}

std::string GSCapture::GetCaptureTypeForMessage(bool capture_video, bool capture_audio)
{
	return capture_video ? capture_audio ? TRANSLATE("GSCapture", "capturing audio and video") : TRANSLATE("GSCapture", "capturing video") : TRANSLATE("GSCapture", "capturing audio");
}

bool GSCapture::IsUsingHardwareVideoEncoding()
{
	return (s_video_hw_context != nullptr);
}

bool GSCapture::BeginCapture(float fps, GSVector2i recommendedResolution, float aspect, std::string filename,
	bool force_audio_video, bool recording_replay)
{
	const bool capture_video = force_audio_video || GSConfig.EnableVideoCapture;
	const bool capture_audio = force_audio_video || GSConfig.EnableAudioCapture;

	if (filename.empty() || fps <= 0.0f ||
		(capture_video && (recommendedResolution.x <= 0 || recommendedResolution.y <= 0)))
	{
		return false;
	}

	Console.WriteLn("Recommended resolution: %d x %d, DAR for muxing: %.4f", recommendedResolution.x, recommendedResolution.y, aspect);

	std::unique_lock<std::mutex> lock(s_lock);

	InternalEndCapture(lock);
	s_recording_replay = recording_replay;
	if (!LoadFFmpeg(!force_audio_video))
		return false;

	s_size = GSVector2i(Common::AlignUpPow2(recommendedResolution.x, 8), Common::AlignUpPow2(recommendedResolution.y, 8));
	s_filename = std::move(filename);

	ff_const59 AVOutputFormat* output_format = wrap_av_guess_format(nullptr, s_filename.c_str(), nullptr);
	if (!output_format)
	{
		Console.Error(fmt::format("Failed to get output format for '{}'", s_filename));
		InternalEndCapture(lock);
		return false;
	}

	int res = wrap_avformat_alloc_output_context2(&s_format_context, output_format, nullptr, s_filename.c_str());
	if (res < 0)
	{
		LogAVError(res, "avformat_alloc_output_context2() failed: ");
		InternalEndCapture(lock);
		return false;
	}
	// find the codec id
	if (capture_video)
	{
		const float sample_aspect_ratio = aspect / (static_cast<float>(s_size.x) / static_cast<float>(s_size.y));

		const AVCodec* vcodec = nullptr;
		if (!GSConfig.VideoCaptureCodec.empty())
		{
			vcodec = wrap_avcodec_find_encoder_by_name(GSConfig.VideoCaptureCodec.c_str());
			if (!vcodec)
			{
				Host::AddIconOSDMessage("GSCaptureCodecNotFound", ICON_FA_CAMERA,
					fmt::format("Video codec {} not found, using default.", GSConfig.VideoCaptureCodec), Host::OSD_ERROR_DURATION);
			}
		}

		// FFmpeg decides whether mp4, mkv, etc should use h264 or mpeg4 as their default codec by whether x264 was enabled
		// But there's a lot of other h264 encoders (e.g. hardware encoders) we may want to use instead
		if (!vcodec && wrap_avformat_query_codec(output_format, AV_CODEC_ID_H264, FF_COMPLIANCE_NORMAL))
			vcodec = wrap_avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!vcodec)
			vcodec = wrap_avcodec_find_encoder(output_format->video_codec);

		if (!vcodec)
		{
			Host::AddIconOSDMessage("GSCaptureError", ICON_FA_CAMERA, "Failed to find video encoder.", Host::OSD_ERROR_DURATION);
			InternalEndCapture(lock);
			return false;
		}

		s_video_codec_context = wrap_avcodec_alloc_context3(vcodec);
		if (!s_video_codec_context)
		{
			Host::AddIconOSDMessage("GSCaptureError", ICON_FA_CAMERA, "Failed to allocate video codec context.", Host::OSD_ERROR_DURATION);
			InternalEndCapture(lock);
			return false;
		}

		s_video_codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
		s_video_codec_context->bit_rate = GSConfig.VideoCaptureBitrate * 1000;
		s_video_codec_context->width = s_size.x;
		s_video_codec_context->height = s_size.y;
		s_video_codec_context->sample_aspect_ratio = wrap_av_d2q(sample_aspect_ratio, 100000);
		wrap_av_reduce(&s_video_codec_context->time_base.num, &s_video_codec_context->time_base.den, 10000,
			static_cast<s64>(static_cast<double>(fps) * 10000.0), std::numeric_limits<s32>::max());

		// Default to NV12 if not overridden by the user
		const AVPixelFormat preferred_sw_pix_fmt = GSConfig.VideoCaptureFormat.empty() ? AV_PIX_FMT_NV12 : static_cast<AVPixelFormat>(std::stoi(GSConfig.VideoCaptureFormat));
		AVPixelFormat sw_pix_fmt = preferred_sw_pix_fmt;
		if (vcodec->pix_fmts)
		{
			sw_pix_fmt = vcodec->pix_fmts[0];
			for (u32 i = 0; vcodec->pix_fmts[i] != AV_PIX_FMT_NONE; i++)
			{
				if (vcodec->pix_fmts[i] == preferred_sw_pix_fmt)
				{
					sw_pix_fmt = vcodec->pix_fmts[i];
					break;
				}
			}
		}

		if (sw_pix_fmt == AV_PIX_FMT_VAAPI)
			sw_pix_fmt = AV_PIX_FMT_NV12;

		s_video_codec_context->pix_fmt = sw_pix_fmt;

		if (preferred_sw_pix_fmt != sw_pix_fmt)
			Console.Warning("GSCapture: preferred pixel format (%d) was unsupported by the codec. Using (%d) instead.", preferred_sw_pix_fmt, sw_pix_fmt);

		// Can we use hardware encoding?
		const AVCodecHWConfig* hwconfig = wrap_avcodec_get_hw_config(vcodec, 0);
		if (hwconfig && hwconfig->pix_fmt != AV_PIX_FMT_NONE && hwconfig->pix_fmt != sw_pix_fmt)
		{
			// First index isn't our preferred pixel format, try the others, but fall back if one doesn't exist.
			int index = 1;
			while (const AVCodecHWConfig* next_hwconfig = wrap_avcodec_get_hw_config(vcodec, index++))
			{
				if (next_hwconfig->pix_fmt == sw_pix_fmt)
				{
					hwconfig = next_hwconfig;
					break;
				}
			}
		}

		if (GSConfig.EnableVideoCaptureParameters)
		{
			res = wrap_av_dict_parse_string(&s_video_codec_arguments, GSConfig.VideoCaptureParameters.c_str(), "=", ":", 0);
			if (res < 0)
			{
				LogAVError(res, "av_dict_parse_string() for video failed: ");
				InternalEndCapture(lock);
				return false;
			}
		}

#ifdef _WIN32
		// NVENC under the D3D12 renderer takes its frames on the GPU instead of through a readback.
		if (BeginGpuFrames(vcodec))
			hwconfig = nullptr;
#endif

		if (hwconfig)
		{
			const char* device = nullptr;
			if (AVDictionaryEntry* dev = wrap_av_dict_get(s_video_codec_arguments, "vaapi_device", nullptr, 0))
				device = dev->value;
			if (AVDictionaryEntry* dev = wrap_av_dict_get(s_video_codec_arguments, "hwaccel_device", nullptr, 0))
				device = dev->value;
			Console.WriteLnFmt(Color_StrongGreen, "Trying to use {}:{} for video encoding.",
			                   wrap_av_hwdevice_get_type_name(hwconfig->device_type), device ? device : "default");
			res = wrap_av_hwdevice_ctx_create(&s_video_hw_context, hwconfig->device_type, device, nullptr, 0);
			if (res < 0)
			{
				LogAVError(res, "av_hwdevice_ctx_create() failed: ");
			}
			else
			{
				s_video_hw_frames = wrap_av_hwframe_ctx_alloc(s_video_hw_context);
				if (!s_video_hw_frames)
				{
					Console.Error("s_video_hw_frames() failed");
					wrap_av_buffer_unref(&s_video_hw_context);
				}
				else
				{
					AVHWFramesContext* frames_ctx = reinterpret_cast<AVHWFramesContext*>(s_video_hw_frames->data);
					frames_ctx->format = (hwconfig->pix_fmt != AV_PIX_FMT_NONE) ? hwconfig->pix_fmt : sw_pix_fmt;
					frames_ctx->sw_format = sw_pix_fmt;
					frames_ctx->width = s_video_codec_context->width;
					frames_ctx->height = s_video_codec_context->height;
					res = wrap_av_hwframe_ctx_init(s_video_hw_frames);
					if (res < 0)
					{
						LogAVError(res, "av_hwframe_ctx_init() failed: ");
						wrap_av_buffer_unref(&s_video_hw_frames);
						wrap_av_buffer_unref(&s_video_hw_context);
					}
					else
					{
						s_video_codec_context->hw_frames_ctx = wrap_av_buffer_ref(s_video_hw_frames);
						if (hwconfig->pix_fmt != AV_PIX_FMT_NONE)
							s_video_codec_context->pix_fmt = hwconfig->pix_fmt;
					}
				}
			}

			if (!s_video_hw_context)
			{
				Host::AddIconOSDMessage("GSCaptureHWError", ICON_FA_CAMERA,
					"Failed to create hardware encoder, using software encoding.", Host::OSD_ERROR_DURATION);
				hwconfig = nullptr;
			}
		}

		if (output_format->flags & AVFMT_GLOBALHEADER)
			s_video_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		bool has_pixel_format_override = wrap_av_dict_get(s_video_codec_arguments, "pixel_format", nullptr, 0);

		res = wrap_avcodec_open2(s_video_codec_context, vcodec, &s_video_codec_arguments);
		if (res < 0)
		{
			LogAVError(res, "avcodec_open2() for video failed: ");
			InternalEndCapture(lock);
			return false;
		}

		// If the user overrode the pixel format, get that now
		if (has_pixel_format_override)
			sw_pix_fmt = s_video_codec_context->pix_fmt;

		s_converted_video_frame = wrap_av_frame_alloc();
		s_hw_video_frame = IsUsingHardwareVideoEncoding() ? wrap_av_frame_alloc() : nullptr;
		if (!s_converted_video_frame || (IsUsingHardwareVideoEncoding() && !s_hw_video_frame))
		{
			LogAVError(AVERROR(ENOMEM), "Failed to allocate frame: ");
			InternalEndCapture(lock);
			return false;
		}

		s_converted_video_frame->format = sw_pix_fmt;
		s_converted_video_frame->width = s_video_codec_context->width;
		s_converted_video_frame->height = s_video_codec_context->height;
		res = wrap_av_frame_get_buffer(s_converted_video_frame, 0);
		if (res < 0)
		{
			LogAVError(res, "av_frame_get_buffer() for converted frame failed: ");
			InternalEndCapture(lock);
			return false;
		}

		if (IsUsingHardwareVideoEncoding())
		{
			s_hw_video_frame->format = s_video_codec_context->pix_fmt;
			s_hw_video_frame->width = s_video_codec_context->width;
			s_hw_video_frame->height = s_video_codec_context->height;
			res = wrap_av_hwframe_get_buffer(s_video_hw_frames, s_hw_video_frame, 0);
			if (res < 0)
			{
				LogAVError(res, "av_frame_get_buffer() for HW frame failed: ");
				InternalEndCapture(lock);
				return false;
			}
		}

		s_video_stream = wrap_avformat_new_stream(s_format_context, vcodec);
		if (!s_video_stream)
		{
			LogAVError(AVERROR(ENOMEM), "avformat_new_stream() for video failed: ");
			InternalEndCapture(lock);
			return false;
		}

		res = wrap_avcodec_parameters_from_context(s_video_stream->codecpar, s_video_codec_context);
		if (res < 0)
		{
			LogAVError(AVERROR(ENOMEM), "avcodec_parameters_from_context() for video failed: ");
			InternalEndCapture(lock);
			return false;
		}

		s_video_stream->time_base = s_video_codec_context->time_base;
		s_video_stream->sample_aspect_ratio = s_video_codec_context->sample_aspect_ratio;

		s_video_packet = wrap_av_packet_alloc();
		if (!s_video_packet)
		{
			LogAVError(AVERROR(ENOMEM), "av_packet_alloc() for video failed: ");
			InternalEndCapture(lock);
			return false;
		}

		s_next_video_pts = 0;
	}

	if (capture_audio)
	{
		// The CPU thread might have dumped some frames in here from the last capture, so clear it out.
		s_audio_buffer_read_pos = 0;
		s_audio_buffer_write_pos = 0;
		s_audio_buffer_size.store(0, std::memory_order_release);
		if (!s_audio_buffer)
			s_audio_buffer = std::make_unique<float[]>(AUDIO_BUFFER_SIZE * AUDIO_CHANNELS);

		const AVCodec* acodec = nullptr;
		if (!GSConfig.AudioCaptureCodec.empty())
		{
			acodec = wrap_avcodec_find_encoder_by_name(GSConfig.AudioCaptureCodec.c_str());
			if (!acodec)
			{
				Host::AddIconOSDMessage("GSCaptureCodecNotFound", ICON_FA_CAMERA,
					fmt::format("Audio codec {} not found, using default.", GSConfig.VideoCaptureCodec), Host::OSD_ERROR_DURATION);
			}
		}
		if (!acodec)
			acodec = wrap_avcodec_find_encoder(output_format->audio_codec);
		if (!acodec)
		{
			Host::AddIconOSDMessage("GSCaptureError", ICON_FA_CAMERA, "Failed to find audio encoder.", Host::OSD_ERROR_DURATION);
			InternalEndCapture(lock);
			return false;
		}

		s_audio_codec_context = wrap_avcodec_alloc_context3(acodec);
		if (!s_audio_codec_context)
		{
			Host::AddIconOSDMessage("GSCaptureError", ICON_FA_CAMERA, "Failed to allocate audio codec context.", Host::OSD_ERROR_DURATION);
			InternalEndCapture(lock);
			return false;
		}

		const s32 sample_rate = SPU2::GetConsoleSampleRate();
		s_audio_codec_context->codec_type = AVMEDIA_TYPE_AUDIO;
		s_audio_codec_context->bit_rate = GSConfig.AudioCaptureBitrate * 1000;
		s_audio_codec_context->sample_fmt = AV_SAMPLE_FMT_FLT;
		s_audio_codec_context->sample_rate = sample_rate;
		s_audio_codec_context->time_base = {1, sample_rate};
#if LIBAVUTIL_VERSION_MAJOR < 57
		s_audio_codec_context->channels = AUDIO_CHANNELS;
		s_audio_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
#else
		wrap_av_channel_layout_default(&s_audio_codec_context->ch_layout, AUDIO_CHANNELS);
#endif

		bool supports_format = false;
		for (const AVSampleFormat* p = acodec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++)
		{
			if (*p == s_audio_codec_context->sample_fmt)
			{
				supports_format = true;
				break;
			}
		}
		if (!supports_format)
		{
			Console.WriteLn(fmt::format("Audio codec '{}' does not support float samples, using default.", acodec->name));
			s_audio_codec_context->sample_fmt = acodec->sample_fmts[0];
			s_swr_context = wrap_swr_alloc();
			if (!s_swr_context)
			{
				LogAVError(AVERROR(ENOMEM), "swr_alloc() failed: ");
				InternalEndCapture(lock);
				return false;
			}

#if LIBAVUTIL_VERSION_MAJOR >= 57
			const AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
			wrap_av_opt_set_chlayout(s_swr_context, "in_chlayout", &layout, 0);
			wrap_av_opt_set_chlayout(s_swr_context, "out_chlayout", &layout, 0);
#endif

			wrap_av_opt_set_int(s_swr_context, "in_channel_count", AUDIO_CHANNELS, 0);
			wrap_av_opt_set_int(s_swr_context, "in_sample_rate", sample_rate, 0);
			wrap_av_opt_set_sample_fmt(s_swr_context, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

			wrap_av_opt_set_int(s_swr_context, "out_channel_count", AUDIO_CHANNELS, 0);
			wrap_av_opt_set_int(s_swr_context, "out_sample_rate", sample_rate, 0);
			wrap_av_opt_set_sample_fmt(s_swr_context, "out_sample_fmt", s_audio_codec_context->sample_fmt, 0);
			res = wrap_swr_init(s_swr_context);
			if (res < 0)
			{
				LogAVError(res, "swr_init() failed: ");
				InternalEndCapture(lock);
				return false;
			}
		}

		// TODO: Check channel layout support

		if (GSConfig.EnableAudioCaptureParameters)
		{
			res = wrap_av_dict_parse_string(&s_audio_codec_arguments, GSConfig.AudioCaptureParameters.c_str(), "=", ":", 0);
			if (res < 0)
			{
				LogAVError(res, "av_dict_parse_string() for audio failed: ");
				InternalEndCapture(lock);
				return false;
			}
		}

		if (output_format->flags & AVFMT_GLOBALHEADER)
			s_audio_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		res = wrap_avcodec_open2(s_audio_codec_context, acodec, &s_audio_codec_arguments);
		if (res < 0)
		{
			LogAVError(res, "avcodec_open2() for audio failed: ");
			InternalEndCapture(lock);
			return false;
		}

		// Use packet size for frame if it supports it... but most don't.
		if (acodec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
			s_audio_frame_size = AudioStream::CHUNK_SIZE;
		else
			s_audio_frame_size = s_audio_codec_context->frame_size;
		if (s_audio_frame_size == 0 || s_audio_frame_size >= AUDIO_BUFFER_SIZE)
		{
			LogAVError(AVERROR(EINVAL), "Audio frame size %u exceeds buffer size %u", s_audio_frame_size, AUDIO_BUFFER_SIZE);
			InternalEndCapture(lock);
			return false;
		}

		s_audio_frame_bps = wrap_av_get_bytes_per_sample(s_audio_codec_context->sample_fmt);
		s_audio_frame_planar = (wrap_av_sample_fmt_is_planar(s_audio_codec_context->sample_fmt) != 0);

		s_converted_audio_frame = wrap_av_frame_alloc();
		if (!s_converted_audio_frame)
		{
			LogAVError(AVERROR(ENOMEM), "Failed to allocate audio frame: ");
			InternalEndCapture(lock);
			return false;
		}

		s_converted_audio_frame->format = s_audio_codec_context->sample_fmt;
		s_converted_audio_frame->nb_samples = s_audio_frame_size;
#if LIBAVUTIL_VERSION_MAJOR < 57
		s_converted_audio_frame->channels = AUDIO_CHANNELS;
		s_converted_audio_frame->channel_layout = s_audio_codec_context->channel_layout;
#else
		wrap_av_channel_layout_copy(&s_converted_audio_frame->ch_layout, &s_audio_codec_context->ch_layout);
#endif
		res = wrap_av_frame_get_buffer(s_converted_audio_frame, 0);
		if (res < 0)
		{
			LogAVError(res, "av_frame_get_buffer() for audio frame failed: ");
			InternalEndCapture(lock);
			return false;
		}

		s_audio_stream = wrap_avformat_new_stream(s_format_context, acodec);
		if (!s_audio_stream)
		{
			LogAVError(AVERROR(ENOMEM), "avformat_new_stream() for audio failed: ");
			InternalEndCapture(lock);
			return false;
		}

		res = wrap_avcodec_parameters_from_context(s_audio_stream->codecpar, s_audio_codec_context);
		if (res < 0)
		{
			LogAVError(res, "avcodec_parameters_from_context() for audio failed: ");
			InternalEndCapture(lock);
			return false;
		}

		s_audio_stream->time_base = s_audio_codec_context->time_base;

		s_audio_packet = wrap_av_packet_alloc();
		if (!s_audio_packet)
		{
			LogAVError(AVERROR(ENOMEM), "av_packet_alloc() for audio failed: ");
			InternalEndCapture(lock);
			return false;
		}

		s_next_audio_pts = 0;
	}

	res = wrap_avio_open(&s_format_context->pb, s_filename.c_str(), AVIO_FLAG_WRITE);
	if (res < 0)
	{
		LogAVError(res, "avio_open() failed: ");
		InternalEndCapture(lock);
		return false;
	}

	res = wrap_avformat_write_header(s_format_context, nullptr);
	if (res < 0)
	{
		LogAVError(res, "avformat_write_header() failed: ");
		InternalEndCapture(lock);
		return false;
	}

	Host::AddIconOSDMessage("GSCapture", ICON_FA_VIDEO,
		fmt::format(TRANSLATE_FS("GSCapture", "Starting {} to '{}'."), GetCaptureTypeForMessage(capture_video, capture_audio), Path::GetFileName(s_filename)),
		Host::OSD_INFO_DURATION);

	if (capture_audio && !recording_replay)
		SPU2::SetAudioCaptureActive(true);

	s_force_audio_video = force_audio_video;
	s_capturing.store(true, std::memory_order_release);
	StartEncoderThread();

	lock.unlock();
	Host::OnCaptureStarted(s_filename);

	return true;
}

bool GSCapture::DeliverVideoFrame(GSTexture* stex)
{
	std::unique_lock<std::mutex> lock(s_lock);

	// If the encoder thread reported an error, stop the capture.
	if (s_encoding_error)
	{
		InternalEndCapture(lock);
		return false;
	}

	if (s_frames_pending_map >= NUM_FRAMES_IN_FLIGHT)
		ProcessFramePendingMap(lock);

	PendingFrame& pf = s_pending_frames[s_pending_frames_pos];

	// It shouldn't be pending map, but the encode thread might be lagging.
	pxAssert(pf.state != PendingFrame::State::NeedsMap);
	if (pf.state == PendingFrame::State::NeedsEncoding)
	{
		s_frame_encoded_cv.wait(lock, [&pf]() { return pf.state == PendingFrame::State::Unused; });
	}

#ifdef _WIN32
	if (s_gpu_frames)
	{
		// The renderer copies the frame into this slot's shared texture on the GPU; the encoder thread draws it
		// into an encoder frame once the copy's fence is reached.
		const u32 slot = s_pending_frames_pos;
		if (!ShareGpuFrameTextures())
		{
			s_encoding_error = true;
			return false;
		}
		pf.ready_value = s_gpu_frames->renderer->CopyToCaptureSharedTexture(stex, slot, s_gpu_frames->slot_free_values[slot]);
		pf.pts = s_next_video_pts++;
		pf.state = PendingFrame::State::NeedsEncoding;
		s_pending_frames_pos = (s_pending_frames_pos + 1) % MAX_PENDING_FRAMES;
		s_frames_pending_encode++;
		s_frame_ready_cv.notify_one();
		return true;
	}
#endif

	if (!pf.tex || pf.tex->GetWidth() != static_cast<u32>(stex->GetWidth()) || pf.tex->GetHeight() != static_cast<u32>(stex->GetHeight()))
	{
		pf.tex.reset();
		pf.tex = g_gs_device->CreateDownloadTexture(stex->GetWidth(), stex->GetHeight(), stex->GetFormat());
		if (!pf.tex)
		{
			Console.Error("GSCapture: Failed to create %x%d download texture", stex->GetWidth(), stex->GetHeight());
			return false;
		}

#ifdef PCSX2_DEVBUILD
		pf.tex->SetDebugName(TinyString::from_format("GSCapture {}x{} Download Texture", stex->GetWidth(), stex->GetHeight()));
#endif
	}

	const GSVector4i rc(0, 0, stex->GetWidth(), stex->GetHeight());
	pf.tex->CopyFromTexture(rc, stex, rc, 0);
	pf.pts = s_next_video_pts++;
	pf.state = PendingFrame::State::NeedsMap;

	s_pending_frames_pos = (s_pending_frames_pos + 1) % MAX_PENDING_FRAMES;
	s_frames_pending_map++;
	return true;
}

void GSCapture::ProcessFramePendingMap(std::unique_lock<std::mutex>& lock)
{
	pxAssert(s_frames_pending_map > 0);

	PendingFrame& pf = s_pending_frames[s_frames_map_consume_pos];
	pxAssert(pf.state == PendingFrame::State::NeedsMap);

	// Flushing is potentially expensive, so we leave it unlocked in case the encode thread
	// needs to pick up another thread while we're waiting.
	lock.unlock();

	if (pf.tex->NeedsFlush())
		pf.tex->Flush();

	// Even if the map failed, we need to kick it to the encode thread anyway, because
	// otherwise our queue indices will get desynchronized.
	if (!pf.tex->Map(GSVector4i(0, 0, s_size.x, s_size.y)))
		Console.Warning("GSCapture: Failed to map previously flushed frame.");

	lock.lock();

	// Kick to encoder thread!
	pf.state = PendingFrame::State::NeedsEncoding;
	s_frames_map_consume_pos = (s_frames_map_consume_pos + 1) % MAX_PENDING_FRAMES;
	s_frames_pending_map--;
	s_frames_pending_encode++;
	s_frame_ready_cv.notify_one();
}

void GSCapture::EncoderThreadEntryPoint()
{
	Threading::SetNameOfCurrentThread("GS Capture Encoding");

	std::unique_lock<std::mutex> lock(s_lock);

	for (;;)
	{
		s_frame_ready_cv.wait(lock, []() { return (s_frames_pending_encode > 0 || !s_capturing.load(std::memory_order_acquire)); });
		if (s_frames_pending_encode == 0 && !s_capturing.load(std::memory_order_acquire))
			break;

		PendingFrame& pf = s_pending_frames[s_frames_encode_consume_pos];
		pxAssert(pf.state == PendingFrame::State::NeedsEncoding);

		lock.unlock();

		bool okay = !s_encoding_error;

		// If the frame failed to map, this will be false, and we'll just skip it.
#ifdef _WIN32
		if (okay && s_video_stream && s_gpu_frames)
			okay = SendGpuFrame(pf, s_frames_encode_consume_pos);
		else
#endif
		if (okay && s_video_stream)
			okay = pf.tex->IsMapped() ? SendFrame(pf) : !s_recording_replay;

		// Encode as many audio frames while the video is ahead.
		if (okay && s_audio_stream && !s_recording_replay)
			okay = ProcessAudioPackets(pf.pts);

		lock.lock();

		// If we had an encoding error, tell the GS thread to shut down the capture (later).
		if (!okay)
			s_encoding_error = true;

		// Done with this frame! Wait for the next.
		pf.state = PendingFrame::State::Unused;
		s_frames_encode_consume_pos = (s_frames_encode_consume_pos + 1) % MAX_PENDING_FRAMES;
		s_frames_pending_encode--;
		s_frame_encoded_cv.notify_all();
	}
}

void GSCapture::StartEncoderThread()
{
	Console.WriteLn("GSCapture: Starting encoder thread.");
	pxAssert(s_capturing.load(std::memory_order_acquire) && !s_encoder_thread.Joinable());
	s_encoder_thread.Start(EncoderThreadEntryPoint);
}

void GSCapture::StopEncoderThread(std::unique_lock<std::mutex>& lock)
{
	// Thread will exit when s_capturing is false.
	pxAssert(!s_capturing.load(std::memory_order_acquire));

	if (s_encoder_thread.Joinable())
	{
		Console.WriteLn("GSCapture: Stopping encoder thread.");

		// Might be sleeping, so wake it before joining.
		s_frame_ready_cv.notify_one();
		lock.unlock();
		s_encoder_thread.Join();
		lock.lock();
	}
}

#ifdef _WIN32

static GSDevice12* GetD3D12Renderer()
{
	return (g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::D3D12) ? static_cast<GSDevice12*>(g_gs_device.get()) :
	                                                                          nullptr;
}

bool GSCapture::BeginGpuFrames(const AVCodec* vcodec)
{
	// Only NVENC is known to take BGRA frames in D3D11 textures; other encoders keep the readback path.
	GSDevice12* const renderer = GetD3D12Renderer();
	if (!renderer || !std::string_view(vcodec->name).ends_with("_nvenc"))
		return false;
	bool takes_d3d11_frames = false;
	for (int i = 0; const AVCodecHWConfig* config = wrap_avcodec_get_hw_config(vcodec, i); i++)
	{
		takes_d3d11_frames |= (config->pix_fmt == AV_PIX_FMT_D3D11 &&
							   (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX));
	}
	if (!takes_d3d11_frames)
		return false;

	const auto fail = [](const char* what, HRESULT hr) {
		Console.Warning("GSCapture: GPU frames unavailable, %s failed (%08X); using readback.", what, static_cast<unsigned>(hr));
		return false;
	};

	// A D3D11 device on the renderer's adapter, for the encoder's frames.
	auto gpu = std::make_unique<GpuFrames>();
	GpuFrames::ComPtr<IDXGIFactory4> factory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
	if (FAILED(hr))
		return fail("CreateDXGIFactory1", hr);
	GpuFrames::ComPtr<IDXGIAdapter> adapter;
	hr = factory->EnumAdapterByLuid(renderer->GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(adapter.put()));
	if (FAILED(hr))
		return fail("EnumAdapterByLuid", hr);
	static constexpr D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	GpuFrames::ComPtr<ID3D11Device> device;
	GpuFrames::ComPtr<ID3D11DeviceContext> context;
	hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
		static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, device.put(), nullptr, context.put());
	if (FAILED(hr))
		return fail("D3D11CreateDevice", hr);
	gpu->device = device.try_query<ID3D11Device5>();
	gpu->context = context.try_query<ID3D11DeviceContext4>();
	if (!gpu->device || !gpu->context)
		return fail("ID3D11Device5", E_NOINTERFACE);

	// Draws a shared texture into an encoder frame; the BGRA render target swaps the channels.
	static constexpr std::string_view copy_shader = R"(
Texture2D<float4> source : register(t0);
void vs_main(uint id : SV_VertexID, out float4 position : SV_Position)
{
	const float2 uv = float2((id << 1) & 2, id & 2);
	position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 ps_main(float4 position : SV_Position) : SV_Target
{
	return source.Load(int3(position.xy, 0));
}
)";
	const auto vs = D3D::CompileShader(D3D::ShaderType::Vertex, D3D::ShaderModel::SM50, false, copy_shader, nullptr, "vs_main");
	const auto ps = D3D::CompileShader(D3D::ShaderType::Pixel, D3D::ShaderModel::SM50, false, copy_shader, nullptr, "ps_main");
	if (!vs || !ps)
		return fail("CompileShader", E_FAIL);
	hr = gpu->device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, gpu->vertex_shader.put());
	if (SUCCEEDED(hr))
		hr = gpu->device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, gpu->pixel_shader.put());
	if (FAILED(hr))
		return fail("CreateShader", hr);

	// The encoder's frames: BGRA D3D11 textures on that device, allocated as the encoder needs them.
	AVBufferRef* device_ref = wrap_av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	if (!device_ref)
		return fail("av_hwdevice_ctx_alloc", E_OUTOFMEMORY);
	AVD3D11VADeviceContext* const device_hw =
		static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device_ref->data)->hwctx);
	device_hw->device = gpu->device.get();
	device_hw->device->AddRef();
	int res = wrap_av_hwdevice_ctx_init(device_ref);
	if (res < 0)
	{
		LogAVError(res, "av_hwdevice_ctx_init() for GPU frames failed: ");
		wrap_av_buffer_unref(&device_ref);
		return false;
	}
	AVBufferRef* frames_ref = wrap_av_hwframe_ctx_alloc(device_ref);
	if (!frames_ref)
	{
		wrap_av_buffer_unref(&device_ref);
		return fail("av_hwframe_ctx_alloc", E_OUTOFMEMORY);
	}
	AVHWFramesContext* const frames = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
	frames->format = AV_PIX_FMT_D3D11;
	frames->sw_format = AV_PIX_FMT_BGRA;
	frames->width = s_size.x;
	frames->height = s_size.y;
	frames->initial_pool_size = 0;
	static_cast<AVD3D11VAFramesContext*>(frames->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
	res = wrap_av_hwframe_ctx_init(frames_ref);
	if (res < 0)
	{
		LogAVError(res, "av_hwframe_ctx_init() for GPU frames failed: ");
		wrap_av_buffer_unref(&frames_ref);
		wrap_av_buffer_unref(&device_ref);
		return false;
	}

	s_video_hw_context = device_ref;
	s_video_hw_frames = frames_ref;
	s_video_codec_context->hw_frames_ctx = wrap_av_buffer_ref(s_video_hw_frames);
	s_video_codec_context->pix_fmt = AV_PIX_FMT_D3D11;
	s_gpu_frames = std::move(gpu);
	Console.WriteLnFmt(Color_StrongGreen, "GSCapture: {} takes its frames on the GPU.", vcodec->name);
	return true;
}

bool GSCapture::ShareGpuFrameTextures()
{
	GpuFrames& gpu = *s_gpu_frames;
	GSDevice12* const renderer = GetD3D12Renderer();
	if (gpu.renderer && gpu.renderer == renderer)
		return true;

	ReleaseGpuFrameTextures();
	if (!renderer)
	{
		Console.Error("GSCapture: GPU frames need the Direct3D 12 renderer.");
		return false;
	}

	std::vector<HANDLE> textures;
	HANDLE ready_fence = nullptr, free_fence = nullptr;
	if (!renderer->CreateCaptureSharedTextures(MAX_PENDING_FRAMES, s_size.x, s_size.y, &textures, &ready_fence, &free_fence))
		return false;

	bool okay = true;
	for (HANDLE handle : textures)
	{
		GpuFrames::ComPtr<ID3D11Texture2D> texture;
		GpuFrames::ComPtr<ID3D11ShaderResourceView> view;
		okay = okay && SUCCEEDED(gpu.device->OpenSharedResource1(handle, IID_PPV_ARGS(texture.put()))) &&
			   SUCCEEDED(gpu.device->CreateShaderResourceView(texture.get(), nullptr, view.put()));
		gpu.views.push_back(std::move(view));
		CloseHandle(handle);
	}
	okay = okay && SUCCEEDED(gpu.device->OpenSharedFence(ready_fence, IID_PPV_ARGS(gpu.ready_fence.put()))) &&
		   SUCCEEDED(gpu.device->OpenSharedFence(free_fence, IID_PPV_ARGS(gpu.free_fence.put())));
	CloseHandle(ready_fence);
	CloseHandle(free_fence);
	gpu.renderer = renderer;
	if (!okay)
	{
		Console.Error("GSCapture: Failed to open the renderer's shared capture textures.");
		ReleaseGpuFrameTextures();
		return false;
	}
	return true;
}

void GSCapture::ReleaseGpuFrameTextures()
{
	GpuFrames& gpu = *s_gpu_frames;

	// Let the encoder's device finish reading the shared textures first.
	if (gpu.free_fence && gpu.free_value != 0 && gpu.free_fence->GetCompletedValue() < gpu.free_value)
	{
		const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (event && SUCCEEDED(gpu.free_fence->SetEventOnCompletion(gpu.free_value, event)))
			WaitForSingleObject(event, 5000);
		if (event)
			CloseHandle(event);
	}

	gpu.views.clear();
	gpu.ready_fence.reset();
	gpu.free_fence.reset();
	gpu.free_value = 0;
	gpu.slot_free_values = {};
	if (gpu.renderer && gpu.renderer == GetD3D12Renderer())
		gpu.renderer->DestroyCaptureSharedTextures();
	gpu.renderer = nullptr;
}

bool GSCapture::SendGpuFrame(const PendingFrame& pf, u32 slot)
{
	GpuFrames& gpu = *s_gpu_frames;
	AVFrame* const frame = s_hw_video_frame;
	wrap_av_frame_unref(frame);
	int res = wrap_av_hwframe_get_buffer(s_video_hw_frames, frame, 0);
	if (res < 0)
	{
		LogAVError(res, "av_hwframe_get_buffer() for a GPU frame failed: ");
		return false;
	}

	ID3D11Texture2D* const texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
	const UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
	ID3D11RenderTargetView* target = nullptr;
	for (const auto& [key, view] : gpu.targets)
	{
		if (key.first == texture && key.second == index)
			target = view.get();
	}
	if (!target)
	{
		D3D11_TEXTURE2D_DESC desc;
		texture->GetDesc(&desc);
		D3D11_RENDER_TARGET_VIEW_DESC view_desc = {};
		view_desc.Format = desc.Format;
		if (desc.ArraySize > 1)
		{
			view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
			view_desc.Texture2DArray.FirstArraySlice = index;
			view_desc.Texture2DArray.ArraySize = 1;
		}
		else
		{
			view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		}
		GpuFrames::ComPtr<ID3D11RenderTargetView> view;
		const HRESULT hr = gpu.device->CreateRenderTargetView(texture, &view_desc, view.put());
		if (FAILED(hr))
		{
			Console.Error("GSCapture: CreateRenderTargetView for a GPU frame failed: %08X", static_cast<unsigned>(hr));
			return false;
		}
		target = view.get();
		gpu.targets.emplace_back(std::make_pair(texture, index), std::move(view));
	}

	// Wait for the renderer's copy, draw the frame into the encoder's texture, and release the shared texture.
	AVD3D11VADeviceContext* const device_hw = static_cast<AVD3D11VADeviceContext*>(
		reinterpret_cast<AVHWDeviceContext*>(s_video_hw_context->data)->hwctx);
	device_hw->lock(device_hw->lock_ctx);
	ID3D11DeviceContext4* const context = gpu.context.get();
	context->Wait(gpu.ready_fence.get(), pf.ready_value);
	const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(s_size.x), static_cast<float>(s_size.y), 0.0f, 1.0f};
	context->OMSetRenderTargets(1, &target, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(nullptr);
	context->RSSetViewports(1, &viewport);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(gpu.vertex_shader.get(), nullptr, 0);
	context->PSSetShader(gpu.pixel_shader.get(), nullptr, 0);
	ID3D11ShaderResourceView* const source = gpu.views[slot].get();
	context->PSSetShaderResources(0, 1, &source);
	context->Draw(3, 0);
	ID3D11ShaderResourceView* const no_source = nullptr;
	context->PSSetShaderResources(0, 1, &no_source);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->Signal(gpu.free_fence.get(), ++gpu.free_value);
	gpu.slot_free_values[slot] = gpu.free_value;
	context->Flush();
	device_hw->unlock(device_hw->lock_ctx);

	frame->pts = pf.pts;
	res = wrap_avcodec_send_frame(s_video_codec_context, frame);
	wrap_av_frame_unref(frame);
	if (res < 0)
	{
		LogAVError(res, "avcodec_send_frame() failed: ");
		return false;
	}

	return ReceivePackets(s_video_codec_context, s_video_stream, s_video_packet);
}

#endif

bool GSCapture::SendFrame(const PendingFrame& pf)
{
	const AVPixelFormat source_format = AV_PIX_FMT_RGBA;
	const u8* source_ptr = pf.tex->GetMapPointer();
	const int source_width = static_cast<int>(pf.tex->GetWidth());
	const int source_height = static_cast<int>(pf.tex->GetHeight());
	const int source_pitch = static_cast<int>(pf.tex->GetMapPitch());

	// In case a previous frame is still using the frame.
	const int writable_result = wrap_av_frame_make_writable(s_converted_video_frame);
	if (writable_result < 0)
	{
		LogAVError(writable_result, "av_frame_make_writable() for video failed: ");
		return false;
	}

	s_sws_context = wrap_sws_getCachedContext(s_sws_context, source_width, source_height, source_format, s_converted_video_frame->width,
		s_converted_video_frame->height, static_cast<AVPixelFormat>(s_converted_video_frame->format), SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (!s_sws_context)
	{
		Console.Error("sws_getCachedContext() failed");
		return false;
	}

	if (wrap_sws_scale(s_sws_context, reinterpret_cast<const u8**>(&source_ptr), &source_pitch, 0, source_height,
			s_converted_video_frame->data, s_converted_video_frame->linesize) != s_converted_video_frame->height)
	{
		Console.Error("GSCapture: Failed to convert the complete video frame.");
		return false;
	}

	AVFrame* frame_to_send = s_converted_video_frame;
	if (IsUsingHardwareVideoEncoding())
	{
		// Need to transfer the frame to hardware.
		const int res = wrap_av_hwframe_transfer_data(s_hw_video_frame, s_converted_video_frame, 0);
		if (res < 0)
		{
			LogAVError(res, "av_hwframe_transfer_data() failed: ");
			return false;
		}

		frame_to_send = s_hw_video_frame;
	}

	// Set the correct PTS before handing it off.
	frame_to_send->pts = pf.pts;

	const int res = wrap_avcodec_send_frame(s_video_codec_context, frame_to_send);
	if (res < 0)
	{
		LogAVError(res, "avcodec_send_frame() failed: ");
		return false;
	}

	return ReceivePackets(s_video_codec_context, s_video_stream, s_video_packet);
}

void GSCapture::ProcessAllInFlightFrames(std::unique_lock<std::mutex>& lock)
{
	while (s_frames_pending_map > 0)
		ProcessFramePendingMap(lock);

	s_frame_encoded_cv.wait(lock, []() { return s_frames_pending_encode == 0; });
}

bool GSCapture::ReceivePackets(AVCodecContext* codec_context, AVStream* stream, AVPacket* packet)
{
	for (;;)
	{
		int res = wrap_avcodec_receive_packet(codec_context, packet);
		if (res == AVERROR(EAGAIN) || res == AVERROR_EOF)
		{
			// no more data available
			break;
		}
		else if (res < 0)
		{
			LogAVError(res, "avcodec_receive_packet() failed: ");
			return false;
		}

		packet->stream_index = stream->index;

		// in case the frame rate changed...
		wrap_av_packet_rescale_ts(packet, codec_context->time_base, stream->time_base);

		res = wrap_av_interleaved_write_frame(s_format_context, packet);
		if (res < 0)
		{
			LogAVError(res, "av_interleaved_write_frame() failed: ");
			return false;
		}

		if (!s_video_stream || stream == s_video_stream)
			s_wrote_media = true;

		wrap_av_packet_unref(packet);
	}

	return true;
}

void GSCapture::DeliverAudioPacket(const float* frames)
{
	// Since this gets called from the EE thread, we might race here after capture stops, and send a few too many samples
	// late. We don't really want to be grabbing the lock on the EE thread, so instead, we'll just YOLO push the frames
	// through and clear them out for the next capture. If we happen to fill the buffer, *then* we'll lock, and check if
	// the capture has stopped.

	static constexpr u32 num_frames = AudioStream::CHUNK_SIZE;

	if ((AUDIO_BUFFER_SIZE - s_audio_buffer_size.load(std::memory_order_acquire)) < num_frames)
	{
		// Need to wait for it to drain a bit.
		std::unique_lock<std::mutex> lock(s_lock);
		s_frame_encoded_cv.wait(lock, []() {
			return (!s_capturing.load(std::memory_order_acquire) ||
					((AUDIO_BUFFER_SIZE - s_audio_buffer_size.load(std::memory_order_acquire)) >= num_frames));
		});
		if (!s_capturing.load(std::memory_order_acquire))
			return;
	}

	// Since the buffer size is aligned to the SndOut packet size, we should always have space for at least one full packet.
	pxAssert((AUDIO_BUFFER_SIZE - s_audio_buffer_write_pos) >= num_frames);
	std::memcpy(s_audio_buffer.get() + (s_audio_buffer_write_pos * AUDIO_CHANNELS), frames, sizeof(float) * AUDIO_CHANNELS * num_frames);
	s_audio_buffer_write_pos = (s_audio_buffer_write_pos + num_frames) % AUDIO_BUFFER_SIZE;

	const u32 buffer_size = s_audio_buffer_size.fetch_add(num_frames, std::memory_order_release) + num_frames;

	if (!s_video_stream && buffer_size >= s_audio_frame_size)
	{
		// If we're not capturing video, push "frames" when we hit the audio packet size.
		std::unique_lock<std::mutex> lock(s_lock);
		if (!s_capturing.load(std::memory_order_acquire))
			return;

		PendingFrame& pf = s_pending_frames[s_pending_frames_pos];
		pf.state = PendingFrame::State::NeedsEncoding;
		s_pending_frames_pos = (s_pending_frames_pos + 1) % MAX_PENDING_FRAMES;

		s_frames_pending_encode++;
		s_frame_ready_cv.notify_one();
	}
}

bool GSCapture::DeliverReplayAudio(const std::vector<float>& samples)
{
	std::unique_lock lock(s_lock);
	if (!s_capturing.load(std::memory_order_acquire) || !s_recording_replay || s_encoding_error)
		return false;

	// Replay supplies the exact SPU2 samples of one emulated frame on the GS queue.
	// Drain the encoder before accessing its frame and muxer.
	ProcessAllInFlightFrames(lock);
	if (s_encoding_error || !s_audio_stream)
		return !s_encoding_error;

	// Stage the samples like SPU2 capture does, then encode every complete codec frame now; a partial frame
	// waits for the next emulated frame, and the last one is sent when the capture ends.
	const u32 count = static_cast<u32>(samples.size() / AUDIO_CHANNELS);
	if (count > AUDIO_BUFFER_SIZE - s_audio_buffer_size.load(std::memory_order_acquire))
	{
		Console.Error("GSCapture: replay audio of %u samples exceeds the capture buffer.", count);
		s_encoding_error = true;
		return false;
	}
	for (u32 pos = 0; pos < count;)
	{
		const u32 batch = std::min(count - pos, AUDIO_BUFFER_SIZE - s_audio_buffer_write_pos);
		std::memcpy(s_audio_buffer.get() + (s_audio_buffer_write_pos * AUDIO_CHANNELS), samples.data() + (pos * AUDIO_CHANNELS),
			sizeof(float) * AUDIO_CHANNELS * batch);
		s_audio_buffer_write_pos = (s_audio_buffer_write_pos + batch) % AUDIO_BUFFER_SIZE;
		s_audio_buffer_size.fetch_add(batch, std::memory_order_release);
		pos += batch;
	}
	if (!ProcessAudioPackets(std::nullopt))
	{
		s_encoding_error = true;
		return false;
	}
	return true;
}

s64 GSCapture::GetVideoFrameCount()
{
	return s_next_video_pts;
}

bool GSCapture::SendPartialReplayAudioFrame()
{
	// Replay audio ends with its last emulated frame; send the incomplete codec frame when the codec accepts one.
	if (!s_audio_stream || s_audio_frame_pos == 0 ||
		!(s_audio_codec_context->codec->capabilities & (AV_CODEC_CAP_VARIABLE_FRAME_SIZE | AV_CODEC_CAP_SMALL_LAST_FRAME)))
	{
		return true;
	}

	s_converted_audio_frame->nb_samples = s_audio_frame_pos;
	s_converted_audio_frame->pts = s_swr_context ? wrap_swr_next_pts(s_swr_context, s_next_audio_pts) : s_next_audio_pts;
	s_next_audio_pts += s_audio_frame_pos;
	s_audio_frame_pos = 0;
	const int res = wrap_avcodec_send_frame(s_audio_codec_context, s_converted_audio_frame);
	s_converted_audio_frame->nb_samples = s_audio_frame_size;
	if (res < 0)
	{
		LogAVError(res, "avcodec_send_frame() for final replay audio failed: ");
		return false;
	}
	return ReceivePackets(s_audio_codec_context, s_audio_stream, s_audio_packet);
}

bool GSCapture::ProcessAudioPackets(std::optional<s64> video_pts)
{
	// Without a video timestamp, every staged sample is encoded, as replay audio needs.
	u32 pending_frames = s_audio_buffer_size.load(std::memory_order_acquire);
	while (pending_frames > 0 && (!video_pts.has_value() || !s_video_codec_context ||
									 wrap_av_compare_ts(video_pts.value(), s_video_codec_context->time_base,
										 s_next_audio_pts, s_audio_codec_context->time_base) > 0))
	{
		pxAssert(!video_pts.has_value() || pending_frames >= AudioStream::CHUNK_SIZE);

		// In case the encoder is still using it.
		if (s_audio_frame_pos == 0)
			wrap_av_frame_make_writable(s_converted_audio_frame);

		// Grab as many source frames as we can.
		const u32 contig_frames = std::min(pending_frames, AUDIO_BUFFER_SIZE - s_audio_buffer_read_pos);
		const u32 this_batch = std::min(s_audio_frame_size - s_audio_frame_pos, contig_frames);

		// Do we need to convert the sample format?
		if (!s_swr_context)
		{
			// No, just copy frames out of staging buffer.
			if (s_audio_frame_planar)
			{
				// This is slow. Hopefully doesn't happen in too many configurations.
				for (u32 i = 0; i < AUDIO_CHANNELS; i++)
				{
					u8* output = s_converted_audio_frame->data[i] + s_audio_frame_pos * s_audio_frame_bps;
					const u8* input = reinterpret_cast<u8*>(&s_audio_buffer[s_audio_buffer_read_pos * AUDIO_CHANNELS + i]);
					for (u32 j = 0; j < this_batch; j++)
					{
						std::memcpy(output, input, sizeof(float));
						input += sizeof(float) * AUDIO_CHANNELS;
						output += s_audio_frame_bps;
					}
				}
			}
			else
			{
				// Direct copy - optimal.
				std::memcpy(s_converted_audio_frame->data[0] + s_audio_frame_pos * s_audio_frame_bps * AUDIO_CHANNELS,
					&s_audio_buffer[s_audio_buffer_read_pos * AUDIO_CHANNELS], this_batch * sizeof(float) * AUDIO_CHANNELS);
			}
		}
		else
		{
			// Use swresample to convert.
			const u8* input = reinterpret_cast<u8*>(&s_audio_buffer[s_audio_buffer_read_pos * AUDIO_CHANNELS]);

			// Might be planar, so offset both buffers.
			u8* output[AUDIO_CHANNELS];
			if (s_audio_frame_planar)
			{
				for (u32 i = 0; i < AUDIO_CHANNELS; i++)
					output[i] = s_converted_audio_frame->data[i] + (s_audio_frame_pos * s_audio_frame_bps);
			}
			else
			{
				output[0] = s_converted_audio_frame->data[0] + (s_audio_frame_pos * s_audio_frame_bps * AUDIO_CHANNELS);
			}

			const int res = wrap_swr_convert(s_swr_context, output, this_batch, &input, this_batch);
			if (res < 0)
			{
				LogAVError(res, "swr_convert() failed: ");
				return false;
			}
		}

		s_audio_buffer_read_pos = (s_audio_buffer_read_pos + this_batch) % AUDIO_BUFFER_SIZE;
		s_audio_buffer_size.fetch_sub(this_batch);
		s_audio_frame_pos += this_batch;
		pending_frames -= this_batch;

		// Do we have a complete frame?
		if (s_audio_frame_pos == s_audio_frame_size)
		{
			s_audio_frame_pos = 0;

			if (!s_swr_context)
			{
				// PTS is simply frames.
				s_converted_audio_frame->pts = s_next_audio_pts;
			}
			else
			{
				s_converted_audio_frame->pts = wrap_swr_next_pts(s_swr_context, s_next_audio_pts);
			}

			// Increment PTS.
			s_next_audio_pts += s_audio_frame_size;

			// Send off for encoding.
			int res = wrap_avcodec_send_frame(s_audio_codec_context, s_converted_audio_frame);
			if (res < 0)
			{
				LogAVError(res, "avcodec_send_frame() for audio failed: ");
				return false;
			}

			// Write any packets back to the output file.
			if (!ReceivePackets(s_audio_codec_context, s_audio_stream, s_audio_packet))
				return false;
		}
	}

	return true;
}

bool GSCapture::InternalEndCapture(std::unique_lock<std::mutex>& lock)
{
	int res;

	const bool was_capturing = s_capturing.load(std::memory_order_acquire);
	bool finalized = was_capturing;

	if (was_capturing)
	{
		SPU2::SetAudioCaptureActive(false);

		if (!s_encoding_error)
		{
			ProcessAllInFlightFrames(lock);
			if (s_recording_replay && !s_encoding_error && !SendPartialReplayAudioFrame())
				s_encoding_error = true;
			Host::AddIconOSDMessage("GSCapture", ICON_FA_VIDEO_SLASH,
				fmt::format(
					TRANSLATE_FS("GSCapture", "Stopped {} to '{}'."), GetCaptureTypeForMessage(IsCapturingVideo(), IsCapturingAudio()), Path::GetFileName(s_filename)),
				Host::OSD_INFO_DURATION);
		}
		else
		{
			Host::AddIconOSDMessage("GSCapture", ICON_FA_VIDEO_SLASH,
				fmt::format(TRANSLATE_FS("GSCapture", "Aborted {} due to encoding error in '{}'."), GetCaptureTypeForMessage(IsCapturingVideo(), IsCapturingAudio()),
					Path::GetFileName(s_filename)),
				Host::OSD_INFO_DURATION);
		}

		s_capturing.store(false, std::memory_order_release);
		StopEncoderThread(lock);

		s_pending_frames = {};
		s_pending_frames_pos = 0;
		s_frames_pending_map = 0;
		s_frames_map_consume_pos = 0;
		s_frames_pending_encode = 0;
		s_frames_encode_consume_pos = 0;

		s_audio_buffer_read_pos = 0;
		s_audio_buffer_write_pos = 0;
		s_audio_buffer_size.store(0, std::memory_order_release);
		s_audio_frame_pos = 0;

		finalized = !s_encoding_error;
		s_encoding_error = false;

		// end of stream
		const auto flush_encoder = [](AVCodecContext* codec, AVStream* stream, AVPacket* packet) {
			if (!stream)
				return true;

			const int result = wrap_avcodec_send_frame(codec, nullptr);
			if (result < 0)
			{
				LogAVError(result, "avcodec_send_frame() for EOS failed: ");
				return false;
			}

			return ReceivePackets(codec, stream, packet);
		};
		finalized = flush_encoder(s_video_codec_context, s_video_stream, s_video_packet) && finalized;
		finalized = flush_encoder(s_audio_codec_context, s_audio_stream, s_audio_packet) && finalized;

		// end of file!
		res = wrap_av_write_trailer(s_format_context);
		if (res < 0)
		{
			LogAVError(res, "av_write_trailer() failed: ");
			finalized = false;
		}
	}

	if (s_format_context)
	{
		res = wrap_avio_closep(&s_format_context->pb);
		if (res < 0)
		{
			LogAVError(res, "avio_closep() failed: ");
			finalized = false;
		}
	}

	if (s_sws_context)
	{
		wrap_sws_freeContext(s_sws_context);
		s_sws_context = nullptr;
	}
	if (s_video_packet)
		wrap_av_packet_free(&s_video_packet);
	if (s_converted_video_frame)
		wrap_av_frame_free(&s_converted_video_frame);
	if (s_hw_video_frame)
		wrap_av_frame_free(&s_hw_video_frame);
	if (s_video_hw_frames)
		wrap_av_buffer_unref(&s_video_hw_frames);
	if (s_video_hw_context)
		wrap_av_buffer_unref(&s_video_hw_context);
	if (s_video_codec_context)
		wrap_avcodec_free_context(&s_video_codec_context);
	s_video_stream = nullptr;
#ifdef _WIN32
	if (s_gpu_frames)
	{
		ReleaseGpuFrameTextures();
		s_gpu_frames.reset();
	}
#endif

	if (s_swr_context)
		wrap_swr_free(&s_swr_context);
	if (s_audio_packet)
		wrap_av_packet_free(&s_audio_packet);
	if (s_converted_audio_frame)
		wrap_av_frame_free(&s_converted_audio_frame);
	if (s_audio_codec_context)
		wrap_avcodec_free_context(&s_audio_codec_context);
	s_audio_stream = nullptr;

	if (s_format_context)
	{
		wrap_avformat_free_context(s_format_context);
		s_format_context = nullptr;
	}
	if (s_video_codec_arguments)
		wrap_av_dict_free(&s_video_codec_arguments);
	if (s_audio_codec_arguments)
		wrap_av_dict_free(&s_audio_codec_arguments);

	if (was_capturing)
		UnloadFFmpeg();

	finalized = finalized && s_wrote_media && FileSystem::OpenManagedCFile(s_filename.c_str(), "rb");
	s_filename.clear();
	s_force_audio_video = false;
	s_recording_replay = false;
	s_wrote_media = false;
	return finalized;
}

bool GSCapture::EndCapture()
{
	bool finalized;
	{
		std::unique_lock<std::mutex> lock(s_lock);
		finalized = InternalEndCapture(lock);
	}

	Host::OnCaptureStopped();
	return finalized;
}

bool GSCapture::IsCapturing()
{
	return s_capturing.load(std::memory_order_acquire);
}

bool GSCapture::IsRecordingReplay()
{
	return IsCapturing() && s_recording_replay;
}

bool GSCapture::IsForcingAudioVideo()
{
	return s_force_audio_video;
}

bool GSCapture::IsCapturingVideo()
{
	return (s_video_stream != nullptr);
}

bool GSCapture::IsCapturingAudio()
{
	return (s_audio_stream != nullptr);
}

TinyString GSCapture::GetElapsedTime()
{
	std::unique_lock<std::mutex> lock(s_lock);
	s64 seconds;
	if (s_video_stream)
	{
		seconds = (s_next_video_pts * static_cast<s64>(s_video_codec_context->time_base.num)) /
		          static_cast<s64>(s_video_codec_context->time_base.den);
	}
	else if (s_audio_stream)
	{
		seconds = (s_next_audio_pts * static_cast<s64>(s_audio_codec_context->time_base.num)) /
		          static_cast<s64>(s_audio_codec_context->time_base.den);
	}
	else
	{
		seconds = -1;
	}

	TinyString ret;
	if (seconds >= 0)
		ret.format("{:02d}:{:02d}:{:02d}", seconds / 3600, (seconds % 3600) / 60, seconds % 60);
	return ret;
}

const Threading::ThreadHandle& GSCapture::GetEncoderThreadHandle()
{
	return s_encoder_thread;
}

GSVector2i GSCapture::GetSize()
{
	return s_size;
}

std::string GSCapture::GetNextCaptureFileName()
{
	std::string ret;
	if (!IsCapturing())
		return ret;

	const std::string_view ext = Path::GetExtension(s_filename);
	std::string_view name = Path::GetFileTitle(s_filename);

	// Should end with a number.
	int partnum = 2;
	std::string_view::size_type pos = name.rfind("_part");
	if (pos != std::string_view::npos)
	{
		std::string_view::size_type cpos = pos + 5;
		for (; cpos < name.length(); cpos++)
		{
			if (name[cpos] < '0' || name[cpos] > '9')
				break;
		}
		if (cpos == name.length())
		{
			// Has existing part number, so add to it.
			partnum = StringUtil::FromChars<int>(name.substr(pos + 5)).value_or(1) + 1;
			name = name.substr(0, pos);
		}
	}

	// If we haven't started a new file previously, add "_part2".
	ret = Path::BuildRelativePath(s_filename, fmt::format("{}_part{:03d}.{}", name, partnum, ext));
	return ret;
}

void GSCapture::Flush()
{
	std::unique_lock<std::mutex> lock(s_lock);

	if (s_encoding_error)
		return;

	ProcessAllInFlightFrames(lock);

	if (IsCapturingAudio())
	{
		// Clear any buffered audio frames out, we don't want to delay the CPU thread.
		const u32 audio_frames = s_audio_buffer_size.load(std::memory_order_acquire);
		if (audio_frames > 0)
			Console.Warning("Dropping %u audio frames on for buffer clear.", audio_frames);

		s_audio_buffer_read_pos = 0;
		s_audio_buffer_write_pos = 0;
		s_audio_buffer_size.store(0, std::memory_order_release);
	}
}

void GSCapture::PrepareForGSReopen()
{
	std::unique_lock lock(s_lock);
	ProcessAllInFlightFrames(lock);
	// Download textures belong to the old device. Keep the codecs, timestamps, and file open.
	// Replay across automatic renderer switches still needs proper runtime testing.
	for (PendingFrame& frame : s_pending_frames)
		frame.tex.reset();
#ifdef _WIN32
	// So are the GPU frames' shared textures; the next frame shares new ones with the reopened renderer.
	if (s_gpu_frames)
		ReleaseGpuFrameTextures();
#endif
}

GSCapture::CodecList GSCapture::GetCodecListForContainer(const char* container, AVMediaType type)
{
	CodecList ret;

	if (!LoadFFmpeg(false))
		return ret;

	const AVOutputFormat* output_format =
		wrap_av_guess_format(nullptr, fmt::format("video.{}", container ? container : "mp4").c_str(), nullptr);
	if (!output_format)
	{
		Console.Error("(GetCodecListForContainer) av_guess_format() failed");
		return ret;
	}

	void* iter = nullptr;
	const AVCodec* codec;
	while ((codec = wrap_av_codec_iterate(&iter)) != nullptr)
	{
		// only get audio codecs
		if (codec->type != type || !wrap_avcodec_find_encoder(codec->id) || !wrap_avcodec_find_encoder_by_name(codec->name))
			continue;

		if (!wrap_avformat_query_codec(output_format, codec->id, FF_COMPLIANCE_NORMAL))
			continue;

		if (std::find_if(ret.begin(), ret.end(), [codec](const auto& it) { return it.first == codec->name; }) != ret.end())
			continue;

		ret.emplace_back(codec->name, codec->long_name ? codec->long_name : codec->name);
	}

	return ret;
}

GSCapture::CodecList GSCapture::GetVideoCodecList(const char* container)
{
	return GetCodecListForContainer(container, AVMEDIA_TYPE_VIDEO);
}

GSCapture::CodecList GSCapture::GetAudioCodecList(const char* container)
{
	return GetCodecListForContainer(container, AVMEDIA_TYPE_AUDIO);
}

GSCapture::FormatList GSCapture::GetVideoFormatList(const char* codec)
{
	FormatList ret;

	if (!LoadFFmpeg(false))
		return ret;

	const AVCodec* v_codec = wrap_avcodec_find_encoder_by_name(codec);

	if (!v_codec)
	{
		Console.Error("(GetVideoFormatList) avcodec_find_encoder_by_name() failed");
		return ret;
	}

	// rawvideo doesn't have a list of formats.
	if (v_codec->pix_fmts == nullptr)
	{
		Console.Error("(GetVideoFormatList) v_codec->pix_fmts is null.");
		return ret;
	}

	for (int i = 0; v_codec->pix_fmts[i] != AVPixelFormat::AV_PIX_FMT_NONE; i++)
	{
		ret.emplace_back(v_codec->pix_fmts[i], wrap_av_get_pix_fmt_name(v_codec->pix_fmts[i]));
	}

	return ret;
}
