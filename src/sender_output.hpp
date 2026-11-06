#pragma once

#include <cstdint>
#include <obs.h>
#include "protocol.hpp"

struct SenderMetrics {
	std::uint64_t video_packets = 0;
	std::uint64_t live_audio_packets = 0;
	std::uint64_t vod_audio_packets = 0;
	std::uint64_t video_bytes = 0;
	std::uint64_t live_audio_bytes = 0;
	std::uint64_t vod_audio_bytes = 0;
	std::uint64_t vertical_video_packets = 0;
	std::uint64_t vertical_video_bytes = 0;
};

struct SenderRemoteStats {
	obs_duocast::protocol::ReceiverStats stats{};
	std::uint64_t received_at_ns = 0;
};

void register_sender_output();
bool sender_output_connected();
bool sender_vertical_canvas_available();
obs_canvas_t *sender_vertical_canvas_ref();
SenderMetrics sender_output_metrics();
SenderRemoteStats sender_remote_stats();
void sender_remote_stats_clear();
