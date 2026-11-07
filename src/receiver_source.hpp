#pragma once

#include <obs.h>
#include "protocol.hpp"
#include <string>
#include <utility>
#include <vector>

void register_receiver_source();
void receiver_set_enabled(bool enabled);
void receiver_set_sync_buffer_ms(int delay_ms);
void receiver_set_auto_recovery_enabled(bool enabled);
void receiver_set_decoder_mode(int mode);
std::vector<std::pair<int, std::string>> receiver_hardware_decoder_options();
struct ReceiverDecodeStatus {
	std::string text;
	bool software_fallback = false;
	bool waiting = true;
};
ReceiverDecodeStatus receiver_decode_status();
bool receiver_vertical_feed_healthy();
bool receiver_vertical_feed_configured();
bool receiver_landscape_feed_healthy();
bool receiver_video_connection_seen();
bool receiver_video_feed_healthy();
std::string receiver_active_peer_address();
obs_source_t *receiver_preview_source(int port, bool vertical);
void receiver_update_remote_stats(const obs_duocast::protocol::ReceiverStats &stats);
