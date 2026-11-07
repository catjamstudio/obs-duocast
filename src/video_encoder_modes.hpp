#pragma once

#include <obs-module.h>

#include <cstdint>
#include <cstring>

namespace duocast {

enum class VideoEncoderMode : int {
	Mjpeg = 0,
	NvencH264 = 1,
	NvencHevc = 2,
	QuickSyncH264 = 3,
	QuickSyncHevc = 4,
	AmfH264 = 5,
	AmfHevc = 6,
};

struct VideoEncoderModeInfo {
	VideoEncoderMode mode;
	const char *encoder_id;
	const char *label;
	const char *codec;
	bool hevc;
};

inline constexpr VideoEncoderModeInfo kHardwareVideoEncoders[] = {
	{VideoEncoderMode::NvencH264, "obs_nvenc_h264_tex", "NVIDIA NVENC H.264 (GPU texture)", "h264", false},
	{VideoEncoderMode::NvencHevc, "obs_nvenc_hevc_tex", "NVIDIA NVENC HEVC / H.265 (GPU texture)", "hevc", true},
	{VideoEncoderMode::QuickSyncH264, "obs_qsv11_v2", "Intel QuickSync H.264 (GPU texture)", "h264", false},
	{VideoEncoderMode::QuickSyncHevc, "obs_qsv11_hevc", "Intel QuickSync HEVC / H.265 (GPU texture)", "hevc", true},
	{VideoEncoderMode::AmfH264, "h264_texture_amf", "AMD AMF H.264 (GPU texture)", "h264", false},
	{VideoEncoderMode::AmfHevc, "h265_texture_amf", "AMD AMF HEVC / H.265 (GPU texture)", "hevc", true},
};

inline const VideoEncoderModeInfo *video_encoder_mode_info(int mode)
{
	for (const auto &info : kHardwareVideoEncoders) {
		if (static_cast<int>(info.mode) == mode)
			return &info;
	}
	return nullptr;
}

inline bool video_encoder_available(const VideoEncoderModeInfo &info)
{
	const char *codec = obs_get_encoder_codec(info.encoder_id);
	return obs_get_encoder_type(info.encoder_id) == OBS_ENCODER_VIDEO && codec &&
	       std::strcmp(codec, info.codec) == 0 &&
	       (obs_get_encoder_caps(info.encoder_id) & OBS_ENCODER_CAP_PASS_TEXTURE) != 0;
}

inline bool video_encoder_available(int mode)
{
	const auto *info = video_encoder_mode_info(mode);
	return info && video_encoder_available(*info);
}

inline const char *video_encoder_display_name(int mode)
{
	if (mode == static_cast<int>(VideoEncoderMode::Mjpeg))
		return "TurboJPEG MJPEG";
	const auto *info = video_encoder_mode_info(mode);
	return info ? info->label : "TurboJPEG MJPEG";
}

} // namespace duocast
