#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <media-io/video-io.h>
#include <callback/proc.h>

#include <algorithm>
#include <string>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QPointer>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QGridLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QShowEvent>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QPaintEngine>
#include <QMenu>
#include <QContextMenuEvent>
#include <QSlider>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QWindow>
#include <QDockWidget>
#include <QMainWindow>
#include <QMetaObject>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "receiver_source.hpp"
#include "sender_output.hpp"

namespace {
constexpr char kConfigSection[] = "OBS-DuoCast";
constexpr char kDestinationKey[] = "Destination";
constexpr char kPortKey[] = "Port";
constexpr char kLiveTrackKey[] = "LiveTrack";
constexpr char kVodTrackKey[] = "VodTrack";
constexpr char kVideoQualityKey[] = "VideoQuality";
constexpr char kVideoEncoderModeKey[] = "VideoEncoderMode";
constexpr char kNvencBitrateKey[] = "NvencBitrateMbps";
constexpr char kMaxBitrateKey[] = "MaxBitrateMbps";
constexpr char kMaxFpsKey[] = "MaxVideoFps";
constexpr char kSendVerticalKey[] = "SendVerticalCanvas";
constexpr char kVideoResolutionKey[] = "VideoResolutionMode";
constexpr char kAutostartKey[] = "AutostartSending";
constexpr char kSenderConfiguredKey[] = "SenderConfigured";
constexpr char kEnableDuoCastKey[] = "EnableDuoCast";
constexpr char kEnableSenderKey[] = "EnableDuoCastSender";
constexpr char kEnableReceiverKey[] = "EnableDuoCastReceiver";
constexpr char kFallbackEnabledKey[] = "FallbackEnabled";
constexpr char kFallbackSceneKey[] = "FallbackScene";
constexpr char kVerticalFallbackSceneKey[] = "VerticalFallbackScene";
constexpr char kLandscapeLiveSceneKey[] = "LandscapeLiveScene";
constexpr char kVerticalLiveSceneKey[] = "VerticalLiveScene";
constexpr char kForceAutoSwitchLiveKey[] = "ForceAutoSwitchLiveScene";
constexpr char kReceiverSyncBufferKey[] = "ReceiverSyncBufferMs";
constexpr char kAutoRecoverLowFpsKey[] = "AutoRecoverLowFps";
std::string g_sceneBeforeFallback;
std::string g_activeFallbackScene;
std::string g_sceneBeforeVerticalFallback;
std::string g_activeVerticalFallbackScene;
std::uint64_t g_lastFallbackRequestNs = 0;
std::uint64_t g_lastVerticalFallbackRequestNs = 0;
std::string g_lastLandscapeLiveRequest;
std::string g_lastVerticalLiveRequest;
std::uint64_t g_lastLandscapeLiveRequestNs = 0;
std::uint64_t g_lastVerticalLiveRequestNs = 0;
obs_output_t *g_senderOutput = nullptr;
std::mutex g_senderMutex;
std::mutex g_draftMutex;
SenderMetrics g_lastMetrics{};
std::uint64_t g_lastMetricsAt = 0;
bool g_haveMetricsBaseline = false;
std::uint64_t g_settingsSavedAt = 0;
bool g_settingsSaveOk = false;
bool g_frontendCallbackRegistered = false;
std::atomic<std::uint64_t> g_videoRate{0}, g_liveAudioRate{0}, g_vodAudioRate{0};
std::atomic<std::uint64_t> g_videoKbps{0}, g_liveAudioKbps{0}, g_vodAudioKbps{0};
std::atomic<std::uint64_t> g_verticalVideoKbps{0};
std::atomic<std::uint64_t> g_verticalVideoRate{0};
std::atomic<std::uint64_t> g_lastRefreshAt{0};
std::atomic<std::uint64_t> g_lastControlEditAt{0};
enum class ToolView { Sender, Receiver };
// The main OBS window owns this dialog. QPointer prevents shutdown from
// deleting it a second time if Qt has already destroyed it with its parent.
QPointer<QDialog> g_duocastDialog;
QWidget *g_senderDockContent = nullptr;
QPushButton *g_dockStartButton = nullptr;
QPushButton *g_dockStopButton = nullptr;
bool g_senderDockRegistered = false;
QWidget *g_receiverStatsDockContent = nullptr;
bool g_receiverStatsDockRegistered = false;
QLabel *g_remoteCpuLabel = nullptr, *g_remoteFpsLabel = nullptr, *g_remoteRenderLabel = nullptr, *g_remoteMemoryLabel = nullptr, *g_remoteDiskLabel = nullptr;
QLabel *g_remoteRenderLagLabel = nullptr, *g_remoteEncodeSkipLabel = nullptr;
QLabel *g_remoteStreamStatusLabel = nullptr, *g_remoteStreamDroppedLabel = nullptr, *g_remoteStreamDataLabel = nullptr, *g_remoteStreamBitrateLabel = nullptr;
QLabel *g_remoteRecordingStatusLabel = nullptr, *g_remoteRecordingDataLabel = nullptr;
QLabel *g_duocastLinkLabel = nullptr, *g_duocastLandscapeLabel = nullptr, *g_duocastVerticalLabel = nullptr;
QLabel *g_duocastLiveAudioLabel = nullptr, *g_duocastVodAudioLabel = nullptr;
os_cpu_usage_info_t *g_cpuUsageInfo = nullptr;
std::uint64_t g_lastReceiverStreamBytes = 0;
std::uint64_t g_lastReceiverBitrateAtNs = 0;
std::uint64_t g_receiverBitrateBps = 0;
QLabel *g_senderStatusLabel = nullptr;
QLabel *g_receiverStatusLabel = nullptr;
QLabel *g_receiverPeerLabel = nullptr;
QLabel *g_peerLabel = nullptr;
QLabel *g_videoFpsLabel = nullptr;
QLabel *g_videoRateLabel = nullptr;
QLabel *g_liveRateLabel = nullptr;
QLabel *g_vodRateLabel = nullptr;
QLabel *g_verticalRateLabel = nullptr;
QLineEdit *g_destinationEdit = nullptr;
QSpinBox *g_portSpin = nullptr;
QSpinBox *g_receiverBufferSpin = nullptr;
QCheckBox *g_autoRecoverLowFpsCheck = nullptr;
QComboBox *g_liveTrackCombo = nullptr;
QComboBox *g_vodTrackCombo = nullptr;
QSlider *g_qualitySlider = nullptr;
QLabel *g_qualityPercentLabel = nullptr;
QLabel *g_videoRateHintLabel = nullptr;
QComboBox *g_videoEncoderCombo = nullptr;
QSpinBox *g_nvencBitrateSpin = nullptr;
QSpinBox *g_maxBitrateSpin = nullptr;
QComboBox *g_maxFpsCombo = nullptr;
QComboBox *g_videoResolutionCombo = nullptr;
QCheckBox *g_autostartCheck = nullptr;
QCheckBox *g_enableSenderCheck = nullptr;
QCheckBox *g_enableReceiverCheck = nullptr;
QCheckBox *g_sendVerticalCheck = nullptr;
QCheckBox *g_fallbackCheck = nullptr;
QComboBox *g_fallbackSceneCombo = nullptr;
QComboBox *g_verticalFallbackSceneCombo = nullptr;
QComboBox *g_landscapeLiveSceneCombo = nullptr;
QComboBox *g_verticalLiveSceneCombo = nullptr;
QCheckBox *g_forceAutoSwitchLiveCheck = nullptr;
QLabel *g_saveFeedbackLabel = nullptr;
QPushButton *g_startButton = nullptr;
QPushButton *g_stopButton = nullptr;

config_t *g_pluginConfig = nullptr;
obs_source_t *g_senderControlSource = nullptr;
obs_source_t *g_receiverControlSource = nullptr;
struct ControlSourceState { ToolView view; };
void stop_sender();
void update_sender_dock();
void create_sender_dock();
void create_receiver_stats_dock();
void update_receiver_stats_dock();
void update_receiver_host_stats();
bool start_sender(const std::string &host, int port, int live_track, int vod_track, int video_quality,
		  int max_bitrate_mbps, int max_fps, bool send_vertical, int resolution_mode,
		  int video_encoder_mode, int nvenc_bitrate_mbps);
const char *control_source_name(void *) { return "OBS DuoCast Settings"; }
void control_source_defaults(obs_data_t *settings);

config_t *profile_config()
{
	return g_pluginConfig;
}

std::vector<std::string> aitum_vertical_scenes()
{
	std::vector<std::string> result;
	obs_canvas_t *canvas = sender_vertical_canvas_ref();
	if (!canvas) return result;
	obs_canvas_enum_scenes(canvas, [](void *opaque, obs_source_t *source) {
		auto *names = static_cast<std::vector<std::string> *>(opaque);
		if (source && obs_source_is_scene(source)) {
			const char *name = obs_source_get_name(source);
			if (name && *name) names->emplace_back(name);
		}
		return true;
	}, &result);
	obs_canvas_release(canvas);
	return result;
}

bool aitum_vertical_canvas_size(int &width, int &height)
{
	obs_canvas_t *canvas = sender_vertical_canvas_ref();
	if (!canvas) return false;
	obs_video_info info{};
	const bool available = obs_canvas_get_video_info(canvas, &info);
	obs_canvas_release(canvas);
	if (!available || !info.base_width || !info.base_height) return false;
	width = static_cast<int>(info.base_width);
	height = static_cast<int>(info.base_height);
	return true;
}

std::string aitum_vertical_current_scene()
{
	int width = 0, height = 0;
	if (!aitum_vertical_canvas_size(width, height)) return {};
	proc_handler_t *handler = obs_get_proc_handler();
	if (!handler) return {};
	calldata_t call;
	calldata_init(&call);
	calldata_set_int(&call, "width", width);
	calldata_set_int(&call, "height", height);
	std::string name;
	if (proc_handler_call(handler, "aitum_vertical_get_scene", &call)) {
		const char *value = calldata_string(&call, "scene");
		if (value) name = value;
	}
	calldata_free(&call);
	return name;
}

bool aitum_vertical_activate_scene(const std::string &name)
{
	if (name.empty()) return false;
	// Validate against Aitum's canvas, not the ordinary OBS scene collection.
	// A stale/non-vertical scene name is ignored by Aitum and leaves its initial
	// scene active, which looks like DuoCast chose the first scene as fallback.
	obs_canvas_t *canvas = sender_vertical_canvas_ref();
	if (!canvas) {
		blog(LOG_WARNING, "DuoCast: cannot switch vertical fallback; Aitum Vertical canvas is unavailable");
		return false;
	}
	obs_source_t *verticalScene = obs_canvas_get_source_by_name(canvas, name.c_str());
	const bool isScene = verticalScene && obs_source_is_scene(verticalScene);
	if (verticalScene) obs_source_release(verticalScene);
	obs_canvas_release(canvas);
	if (!isScene) {
		blog(LOG_WARNING, "DuoCast: vertical fallback '%s' is not a scene on the Aitum Vertical canvas", name.c_str());
		return false;
	}
	int width = 0, height = 0;
	if (!aitum_vertical_canvas_size(width, height)) return false;
	// Aitum 1.6.x's proc handler passes only QString to a Qt slot declared as
	// SwitchScene(QString, bool). Qt does not apply the C++ default argument to
	// meta-object invocations, so that request is silently discarded. Invoke its
	// slot with both arguments on OBS's UI thread instead.
	auto *app = QApplication::instance();
	if (!app) return false;
	const QString sceneName = QString::fromUtf8(name.c_str());
	const bool queued = QMetaObject::invokeMethod(app, [sceneName, width, height] {
		auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		auto *dock = mainWindow ? mainWindow->findChild<QDockWidget *>(QStringLiteral("VerticalCanvasDock")) : nullptr;
		QWidget *canvasWidget = dock ? dock->widget() : nullptr;
		if (canvasWidget && canvasWidget->objectName() != QStringLiteral("contextContainer"))
			canvasWidget = canvasWidget->findChild<QWidget *>(QStringLiteral("contextContainer"));
		const QString expectedClass = QStringLiteral("CanvasDock");
		if (canvasWidget && QString::fromLatin1(canvasWidget->metaObject()->className()) == expectedClass) {
			const bool invoked = QMetaObject::invokeMethod(canvasWidget, "SwitchScene", Qt::DirectConnection,
								      Q_ARG(QString, sceneName), Q_ARG(bool, false));
			if (invoked)
				blog(LOG_INFO, "DuoCast: switched Aitum Vertical fallback to '%s' (%dx%d)",
				     sceneName.toUtf8().constData(), width, height);
			else
				blog(LOG_WARNING, "DuoCast: Aitum Vertical SwitchScene slot invocation failed");
		} else {
			blog(LOG_WARNING, "DuoCast: could not locate Aitum Vertical canvas dock for fallback scene '%s'",
			     sceneName.toUtf8().constData());
		}
	}, Qt::QueuedConnection);
	return queued;
}

struct ControlDraft {
	std::string destination = "192.168.50.102";
	int port = 17777;
	int live_track = 1;
	int vod_track = 2;
	int video_quality = 85;
	int video_encoder_mode = 0; // 0 = TurboJPEG MJPEG, 1 = NVENC H.264, 2 = NVENC HEVC
	int nvenc_bitrate_mbps = 80;
	int max_bitrate_mbps = 100;
	int max_fps = 0;
	int resolution_mode = 0; // 0 = OBS base canvas, 1 = OBS scaled output
	int receiver_sync_buffer_ms = 1000;
	bool auto_recover_low_fps = true;
	bool send_vertical = false;
	bool autostart = true;
	bool enable_sender = false;
	bool enable_receiver = false;
	bool fallback_enabled = false;
	bool force_auto_switch_live = false;
	std::string fallback_scene;
	std::string vertical_fallback_scene;
	std::string landscape_live_scene;
	std::string vertical_live_scene;
};

ControlDraft g_controlDraft;

ControlDraft control_draft_snapshot()
{
	std::lock_guard<std::mutex> lock(g_draftMutex);
	return g_controlDraft;
}

ControlDraft draft_from_settings(obs_data_t *settings)
{
	ControlDraft draft;
	const char *destination = obs_data_get_string(settings, "destination");
	const char *fallback = obs_data_get_string(settings, "fallback_scene");
	const char *vertical_fallback = obs_data_get_string(settings, "vertical_fallback_scene");
	const char *landscape_live = obs_data_get_string(settings, "landscape_live_scene");
	const char *vertical_live = obs_data_get_string(settings, "vertical_live_scene");
	draft.destination = destination && *destination ? destination : "192.168.50.102";
	draft.port = std::clamp(static_cast<int>(obs_data_get_int(settings, "port")), 1, 65535);
	draft.live_track = std::clamp(static_cast<int>(obs_data_get_int(settings, "live_track")), 1, 6);
	draft.vod_track = std::clamp(static_cast<int>(obs_data_get_int(settings, "vod_track")), 1, 6);
	draft.video_quality = std::clamp(static_cast<int>(obs_data_get_int(settings, "video_quality")), 1, 100);
	draft.video_encoder_mode = std::clamp(static_cast<int>(obs_data_get_int(settings, "video_encoder_mode")), 0, 2);
	draft.nvenc_bitrate_mbps = std::clamp(static_cast<int>(obs_data_get_int(settings, "nvenc_bitrate_mbps")), 1, 1000);
	draft.max_bitrate_mbps = std::clamp(static_cast<int>(obs_data_get_int(settings, "max_bitrate_mbps")), 0, 1000);
	draft.max_fps = std::clamp(static_cast<int>(obs_data_get_int(settings, "max_fps")), 0, 120);
	draft.resolution_mode = std::clamp(static_cast<int>(obs_data_get_int(settings, "resolution_mode")), 0, 1);
	draft.receiver_sync_buffer_ms = std::clamp(static_cast<int>(obs_data_get_int(settings, "receiver_sync_buffer_ms")), 0, 3000);
	draft.auto_recover_low_fps = obs_data_get_bool(settings, "auto_recover_low_fps");
	draft.send_vertical = obs_data_get_bool(settings, "send_vertical");
	draft.autostart = obs_data_get_bool(settings, "autostart");
	draft.enable_sender = obs_data_get_bool(settings, "enable_sender");
	draft.enable_receiver = obs_data_get_bool(settings, "enable_receiver");
	draft.fallback_enabled = obs_data_get_bool(settings, "fallback_enabled");
	draft.force_auto_switch_live = obs_data_get_bool(settings, "force_auto_switch_live");
	draft.fallback_scene = fallback ? fallback : "";
	draft.vertical_fallback_scene = vertical_fallback ? vertical_fallback : "";
	draft.landscape_live_scene = landscape_live ? landscape_live : "";
	draft.vertical_live_scene = vertical_live ? vertical_live : "";
	return draft;
}

bool persist_draft(bool sender_configured = false)
{
	config_t *cfg = profile_config();
	if (!cfg) return false;
	const auto draft = control_draft_snapshot();
	config_set_string(cfg, kConfigSection, kDestinationKey, draft.destination.c_str());
	config_set_int(cfg, kConfigSection, kPortKey, draft.port);
	config_set_int(cfg, kConfigSection, kLiveTrackKey, draft.live_track);
	config_set_int(cfg, kConfigSection, kVodTrackKey, draft.vod_track);
	config_set_int(cfg, kConfigSection, kVideoQualityKey, draft.video_quality);
	config_set_int(cfg, kConfigSection, kVideoEncoderModeKey, draft.video_encoder_mode);
	config_set_int(cfg, kConfigSection, kNvencBitrateKey, draft.nvenc_bitrate_mbps);
	config_set_int(cfg, kConfigSection, kMaxBitrateKey, draft.max_bitrate_mbps);
	config_set_int(cfg, kConfigSection, kMaxFpsKey, draft.max_fps);
	config_set_int(cfg, kConfigSection, kVideoResolutionKey, draft.resolution_mode);
	config_set_int(cfg, kConfigSection, kReceiverSyncBufferKey, draft.receiver_sync_buffer_ms);
	config_set_bool(cfg, kConfigSection, kAutoRecoverLowFpsKey, draft.auto_recover_low_fps);
	config_set_bool(cfg, kConfigSection, kSendVerticalKey, draft.send_vertical);
	config_set_bool(cfg, kConfigSection, kAutostartKey, draft.autostart);
	config_set_bool(cfg, kConfigSection, kEnableSenderKey, draft.enable_sender);
	config_set_bool(cfg, kConfigSection, kEnableReceiverKey, draft.enable_receiver);
	config_set_bool(cfg, kConfigSection, kFallbackEnabledKey, draft.fallback_enabled);
	config_set_bool(cfg, kConfigSection, kForceAutoSwitchLiveKey, draft.force_auto_switch_live);
	config_set_string(cfg, kConfigSection, kFallbackSceneKey, draft.fallback_scene.c_str());
	config_set_string(cfg, kConfigSection, kVerticalFallbackSceneKey, draft.vertical_fallback_scene.c_str());
	config_set_string(cfg, kConfigSection, kLandscapeLiveSceneKey, draft.landscape_live_scene.c_str());
	config_set_string(cfg, kConfigSection, kVerticalLiveSceneKey, draft.vertical_live_scene.c_str());
	receiver_set_sync_buffer_ms(draft.receiver_sync_buffer_ms);
	receiver_set_auto_recovery_enabled(draft.auto_recover_low_fps);
	if (sender_configured) config_set_bool(cfg, kConfigSection, kSenderConfiguredKey, true);
	return config_save_safe(cfg, "tmp", "bak") == CONFIG_SUCCESS;
}

bool apply_draft_to_source(obs_source_t *target = nullptr)
{
	if (!target) target = g_senderControlSource;
	if (!target) return persist_draft();
	const auto draft = control_draft_snapshot();
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "destination", draft.destination.c_str());
	obs_data_set_int(settings, "port", draft.port);
	obs_data_set_int(settings, "live_track", draft.live_track);
	obs_data_set_int(settings, "vod_track", draft.vod_track);
	obs_data_set_int(settings, "video_quality", draft.video_quality);
	obs_data_set_int(settings, "video_encoder_mode", draft.video_encoder_mode);
	obs_data_set_int(settings, "nvenc_bitrate_mbps", draft.nvenc_bitrate_mbps);
	obs_data_set_int(settings, "max_bitrate_mbps", draft.max_bitrate_mbps);
	obs_data_set_int(settings, "max_fps", draft.max_fps);
	obs_data_set_int(settings, "resolution_mode", draft.resolution_mode);
	obs_data_set_int(settings, "receiver_sync_buffer_ms", draft.receiver_sync_buffer_ms);
	obs_data_set_bool(settings, "auto_recover_low_fps", draft.auto_recover_low_fps);
	obs_data_set_bool(settings, "send_vertical", draft.send_vertical);
	obs_data_set_bool(settings, "autostart", draft.autostart);
	obs_data_set_bool(settings, "enable_sender", draft.enable_sender);
	obs_data_set_bool(settings, "enable_receiver", draft.enable_receiver);
	obs_data_set_bool(settings, "fallback_enabled", draft.fallback_enabled);
	obs_data_set_bool(settings, "force_auto_switch_live", draft.force_auto_switch_live);
	obs_data_set_string(settings, "fallback_scene", draft.fallback_scene.c_str());
	obs_data_set_string(settings, "vertical_fallback_scene", draft.vertical_fallback_scene.c_str());
	obs_data_set_string(settings, "landscape_live_scene", draft.landscape_live_scene.c_str());
	obs_data_set_string(settings, "vertical_live_scene", draft.vertical_live_scene.c_str());
	obs_source_update(target, settings);
	obs_data_release(settings);
	return persist_draft();
}

bool control_setting_changed(void *, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
 { std::lock_guard<std::mutex> lock(g_draftMutex); g_controlDraft = draft_from_settings(settings); }
 g_lastControlEditAt.store(os_gettime_ns() / 1000000ULL);
 return false;
}

bool save_settings_clicked(obs_properties_t *, obs_property_t *, void *opaque)
{
	const auto *state = static_cast<ControlSourceState *>(opaque);
	obs_source_t *target = state && state->view == ToolView::Receiver ? g_receiverControlSource : g_senderControlSource;
	g_settingsSaveOk = apply_draft_to_source(target);
 g_settingsSavedAt = os_gettime_ns() / 1000000ULL;
 blog(g_settingsSaveOk ? LOG_INFO : LOG_ERROR, "DuoCast: settings %s", g_settingsSaveOk ? "saved" : "could not be saved");
 if (g_senderControlSource) obs_source_update_properties(g_senderControlSource);
 if (g_receiverControlSource) obs_source_update_properties(g_receiverControlSource);
 return true;
}

bool start_sender_clicked(obs_properties_t *, obs_property_t *, void *)
{
 apply_draft_to_source();
 const auto draft = control_draft_snapshot();
 const bool started = start_sender(draft.destination, draft.port, draft.live_track, draft.vod_track,
	                                   draft.video_quality, draft.max_bitrate_mbps, draft.max_fps, draft.send_vertical,
	                                   draft.resolution_mode, draft.video_encoder_mode, draft.nvenc_bitrate_mbps);
 blog(started ? LOG_INFO : LOG_ERROR, "DuoCast: sender %s", started ? "started" : "could not start");
 if (g_senderControlSource) obs_source_update_properties(g_senderControlSource);
 if (g_receiverControlSource) obs_source_update_properties(g_receiverControlSource);
 return true;
}

bool stop_sender_clicked(obs_properties_t *, obs_property_t *, void *)
{
 stop_sender();
 if (g_senderControlSource) obs_source_update_properties(g_senderControlSource);
 if (g_receiverControlSource) obs_source_update_properties(g_receiverControlSource);
 return true;
}

void control_source_video_render(void *, gs_effect_t *)
{
	obs_render_main_texture();
}

uint32_t control_source_width(void *)
{
	obs_video_info info{};
	return obs_get_video_info(&info) ? info.output_width : 0;
}

uint32_t control_source_height(void *)
{
	obs_video_info info{};
	return obs_get_video_info(&info) ? info.output_height : 0;
}

void *control_source_create(obs_data_t *settings, obs_source_t *source)
{
 const char *mode = obs_data_get_string(settings, "tool_view");
 auto *state = new ControlSourceState{mode && strcmp(mode, "receiver") == 0 ? ToolView::Receiver : ToolView::Sender};
 if (state->view == ToolView::Receiver) g_receiverControlSource = source;
 else g_senderControlSource = source;
 { std::lock_guard<std::mutex> lock(g_draftMutex); g_controlDraft = draft_from_settings(settings); }
 return state;
}

void control_source_update(void *, obs_data_t *settings)
{
	{ std::lock_guard<std::mutex> lock(g_draftMutex); g_controlDraft = draft_from_settings(settings); }
	persist_draft();
}

void control_source_destroy(void *opaque)
{
 delete static_cast<ControlSourceState *>(opaque);
}

class DuoCastPreview final : public QWidget {
public:
	DuoCastPreview()
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setAutoFillBackground(false);
		setObjectName("canvasPreview");
		if (auto *handle = windowHandle()) {
			QObject::connect(handle, &QWindow::visibleChanged, this, [this](bool visible) {
				if (visible) create_display();
			});
			QObject::connect(handle, &QWindow::screenChanged, this, [this](QScreen *) {
				destroy_display();
				create_display();
			});
		}
	}
	~DuoCastPreview() override {
		destroy_display();
		std::lock_guard<std::mutex> lock(canvasMutex_);
		if (verticalCanvas_) obs_canvas_release(verticalCanvas_);
	}
	void setVerticalCanvas(obs_canvas_t *canvas)
	{
		std::lock_guard<std::mutex> lock(canvasMutex_);
		if (verticalCanvas_) obs_canvas_release(verticalCanvas_);
		verticalCanvas_ = canvas ? obs_canvas_get_ref(canvas) : nullptr;
		if (display_) obs_display_resize(display_, static_cast<uint32_t>(width()), static_cast<uint32_t>(height()));
	}
	void setPreviewEnabled(bool enabled)
	{
		if (enabled) {
			if (isVisible()) create_display();
		} else {
			destroy_display();
		}
	}
protected:
	void showEvent(QShowEvent *event) override { QWidget::showEvent(event); create_display(); }
	void paintEvent(QPaintEvent *event) override { QWidget::paintEvent(event); create_display(); }
	void resizeEvent(QResizeEvent *event) override {
		QWidget::resizeEvent(event);
		create_display();
		if (isVisible() && display_)
			obs_display_resize(display_, static_cast<uint32_t>(width()), static_cast<uint32_t>(height()));
	}
	QPaintEngine *paintEngine() const override { return nullptr; }
private:
	static void draw(void *opaque, uint32_t cx, uint32_t cy) {
		auto *self = static_cast<DuoCastPreview *>(opaque);
		if (!self) return;
		obs_video_info video{};
		obs_canvas_t *verticalCanvas = nullptr;
		{
			std::lock_guard<std::mutex> lock(self->canvasMutex_);
			if (self->verticalCanvas_) verticalCanvas = obs_canvas_get_ref(self->verticalCanvas_);
		}
		gs_viewport_push();
		gs_projection_push();
		obs_get_video_info(&video);
		obs_video_info verticalInfo{};
		const bool hasVertical = verticalCanvas && obs_canvas_get_video_info(verticalCanvas, &verticalInfo) &&
			verticalInfo.base_width && verticalInfo.base_height;
		if (cx && cy && video.base_width && video.base_height) {
			const uint32_t gap = hasVertical ? 14 : 0;
			const double commonHeight = std::min<double>(cy, 540.0);
			const double landscapeScale = hasVertical ? commonHeight / video.base_height
				: std::min({static_cast<double>(cx) / video.base_width, commonHeight / video.base_height,
					    960.0 / video.base_width, 540.0 / video.base_height});
			const double verticalScale = hasVertical ? commonHeight / verticalInfo.base_height : 0.0;
			const int landscapeW = static_cast<int>(std::lround(video.base_width * landscapeScale));
			const int landscapeH = static_cast<int>(std::lround(video.base_height * landscapeScale));
			const int verticalW = hasVertical ? static_cast<int>(std::lround(verticalInfo.base_width * verticalScale)) : 0;
			const int verticalH = hasVertical ? static_cast<int>(std::lround(verticalInfo.base_height * verticalScale)) : 0;
			const int combinedW = landscapeW + (hasVertical ? static_cast<int>(gap) + verticalW : 0);
			int x = static_cast<int>((cx - combinedW) / 2);
			const int landscapeY = static_cast<int>((cy - landscapeH) / 2);
			gs_ortho(0.0f, static_cast<float>(video.base_width), 0.0f,
				 static_cast<float>(video.base_height), -100.0f, 100.0f);
			gs_set_viewport(x, landscapeY, landscapeW, landscapeH);
			obs_render_main_texture();
			if (hasVertical) {
				x += landscapeW + static_cast<int>(gap);
				const int verticalY = static_cast<int>((cy - verticalH) / 2);
				gs_ortho(0.0f, static_cast<float>(verticalInfo.base_width), 0.0f,
					 static_cast<float>(verticalInfo.base_height), -100.0f, 100.0f);
				gs_set_viewport(x, verticalY, verticalW, verticalH);
				obs_render_canvas_texture(verticalCanvas);

				// Keep the two independent canvases visually distinct even when
				// both compositions have matching dark edges.
				const int dividerY = std::min(landscapeY, verticalY);
				const int dividerBottom = std::max(landscapeY + landscapeH, verticalY + verticalH);
				const int dividerWidth = 3;
				const int dividerHeight = std::max(1, dividerBottom - dividerY);
				const int dividerX = static_cast<int>((cx - combinedW) / 2) + landscapeW +
					(static_cast<int>(gap) - dividerWidth) / 2;
				gs_set_viewport(dividerX, dividerY, dividerWidth, dividerHeight);
				gs_ortho(0.0f, static_cast<float>(dividerWidth), 0.0f,
					 static_cast<float>(dividerHeight), -100.0f, 100.0f);
				gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
				if (solid) {
					struct vec4 dividerColor = {0.38f, 0.41f, 0.46f, 1.0f};
					gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &dividerColor);
					while (gs_effect_loop(solid, "Solid"))
						gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(dividerWidth), static_cast<uint32_t>(dividerHeight));
				}
			}
		}
		gs_projection_pop();
		gs_viewport_pop();
		if (verticalCanvas) obs_canvas_release(verticalCanvas);
	}
	void create_display() {
		if (display_ || !isVisible() || width() < 2 || height() < 2) return;
		auto *handle = windowHandle();
		if (!handle || !handle->isExposed()) return;
		gs_init_data init{};
		init.window.hwnd = reinterpret_cast<void *>(handle->winId());
		init.cx = static_cast<uint32_t>(width()); init.cy = static_cast<uint32_t>(height());
		init.format = GS_BGRA; init.zsformat = GS_ZS_NONE;
		display_ = obs_display_create(&init, 0xFF121519);
		if (display_) obs_display_add_draw_callback(display_, draw, this);
	}
	void destroy_display()
	{
		if (!display_) return;
		obs_display_remove_draw_callback(display_, draw, this);
		obs_display_destroy(display_);
		display_ = nullptr;
	}
	obs_display_t *display_ = nullptr;
	std::mutex canvasMutex_;
	obs_canvas_t *verticalCanvas_ = nullptr;
};

class DuoCastPreviewArea final : public QWidget {
public:
	DuoCastPreviewArea()
	{
		auto *layout = new QHBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->addStretch();
		preview_ = new DuoCastPreview;
		layout->addWidget(preview_);
		enablePreviewButton_ = new QPushButton("Enable Preview", this);
		enablePreviewButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		enablePreviewButton_->hide();
		layout->addWidget(enablePreviewButton_);
		layout->addStretch();
		setMinimumHeight(80);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		QObject::connect(enablePreviewButton_, &QPushButton::clicked, this, [this] { setPreviewEnabled(true); });
		QObject::connect(preview_, &QWidget::customContextMenuRequested, this,
				 [this](const QPoint &) { showPreviewContextMenu(QCursor::pos()); });
		preview_->setContextMenuPolicy(Qt::CustomContextMenu);
		// Keep the OBS graphics display out of the initial Tools-window startup
		// path. Users can enable it explicitly after the dialog is open.
		setPreviewEnabled(false);
	}
	~DuoCastPreviewArea() override { if (verticalCanvas_) obs_canvas_release(verticalCanvas_); }
	void setVerticalEnabled(bool enabled)
	{
		verticalEnabled_ = enabled && sender_vertical_canvas_available();
		obs_canvas_t *canvas = verticalEnabled_ ? sender_vertical_canvas_ref() : nullptr;
		if (verticalCanvas_) obs_canvas_release(verticalCanvas_);
		verticalCanvas_ = canvas ? obs_canvas_get_ref(canvas) : nullptr;
		preview_->setVerticalCanvas(canvas);
		if (canvas) obs_canvas_release(canvas);
		resize_preview();
	}
	int previewContentWidth() const { return preview_ ? preview_->width() : 0; }
protected:
	void contextMenuEvent(QContextMenuEvent *event) override { showPreviewContextMenu(event->globalPos()); }
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		resize_preview();
		if (verticalEnabled_) {
			const int requiredWidth = previewContentWidth() + 48;
			QWidget *dialog = window();
			dialog->setMinimumWidth(requiredWidth);
			if (dialog->width() < requiredWidth) dialog->resize(requiredWidth, dialog->height());
		}
	}
private:
	void resize_preview()
	{
		obs_video_info video{};
		if (obs_get_video_info(&video) && video.base_width && video.base_height) {
			obs_video_info vertical{};
			const bool hasVertical = verticalEnabled_ && verticalCanvas_ && obs_canvas_get_video_info(verticalCanvas_, &vertical) &&
				vertical.base_width && vertical.base_height;
			if (hasVertical) {
				const double previewHeight = std::min(540.0, static_cast<double>(std::max(1, height())));
				const double totalWidth = video.base_width * previewHeight / video.base_height +
					vertical.base_width * previewHeight / vertical.base_height + 14.0;
				preview_->setFixedSize(std::max(1, static_cast<int>(std::lround(totalWidth))),
					std::max(1, static_cast<int>(std::lround(previewHeight))));
			} else {
				const double scale = std::min({static_cast<double>(std::max(1, width())) / video.base_width,
					static_cast<double>(std::max(1, height())) / video.base_height,
					960.0 / video.base_width, 540.0 / video.base_height});
				preview_->setFixedSize(std::max(1, static_cast<int>(std::lround(video.base_width * scale))),
					std::max(1, static_cast<int>(std::lround(video.base_height * scale))));
			}
		}
	}
	void setPreviewEnabled(bool enabled)
	{
		previewEnabled_ = enabled;
		preview_->setVisible(enabled);
		enablePreviewButton_->setVisible(!enabled);
		preview_->setPreviewEnabled(enabled);
	}
	void showPreviewContextMenu(const QPoint &globalPosition)
	{
		QMenu menu(this);
		auto *enable = menu.addAction("Enable Preview");
		enable->setCheckable(true);
		enable->setChecked(previewEnabled_);
		QObject::connect(enable, &QAction::toggled, this, [this](bool checked) { setPreviewEnabled(checked); });
		menu.exec(globalPosition);
	}
	DuoCastPreview *preview_ = nullptr;
	QPushButton *enablePreviewButton_ = nullptr;
	bool previewEnabled_ = false;
	bool verticalEnabled_ = false;
	obs_canvas_t *verticalCanvas_ = nullptr;
};

class DuoCastReceiverCanvas final : public QWidget {
public:
	explicit DuoCastReceiverCanvas(int port, QWidget *parent = nullptr) : QWidget(parent), port_(port)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setAutoFillBackground(false);
		setMinimumSize(2, 2);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		resizeTimer_ = new QTimer(this);
		resizeTimer_->setSingleShot(true);
		QObject::connect(resizeTimer_, &QTimer::timeout, this, [this] { resizeDisplay(); });
	}
	~DuoCastReceiverCanvas() override { setActive(false); }
	void setActive(bool active)
	{
		if (active == active_) return;
		if (active) {
			auto *newLandscape = receiver_preview_source(port_, false);
			auto *newVertical = receiver_preview_source(port_, true);
			if (newLandscape) obs_source_inc_showing(newLandscape);
			if (newVertical) obs_source_inc_showing(newVertical);
			{
				std::lock_guard<std::mutex> lock(sourceMutex_);
				landscape_ = newLandscape;
				vertical_ = newVertical;
			}
			active_ = true;
			createDisplay();
		} else {
			releasePreview();
		}
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		if (active_) { createDisplay(); resizeDisplay(); }
	}
	void paintEvent(QPaintEvent *event) override { QWidget::paintEvent(event); if (active_) createDisplay(); }
	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		if (active_ && display_) resizeTimer_->start(150);
		else if (active_) createDisplay();
	}
	QPaintEngine *paintEngine() const override { return nullptr; }

private:
	static void draw(void *opaque, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<DuoCastReceiverCanvas *>(opaque);
		if (!self || !cx || !cy) return;
		obs_source_t *landscape = nullptr;
		obs_source_t *vertical = nullptr;
		{
			std::lock_guard<std::mutex> lock(self->sourceMutex_);
			if (self->landscape_) landscape = obs_source_get_ref(self->landscape_);
			if (self->vertical_) vertical = obs_source_get_ref(self->vertical_);
		}
		const bool showLandscape = landscape && receiver_landscape_feed_healthy() &&
			obs_source_get_width(landscape) && obs_source_get_height(landscape);
		const bool showVertical = vertical && receiver_vertical_feed_configured() && receiver_vertical_feed_healthy() &&
			obs_source_get_width(vertical) && obs_source_get_height(vertical);
		const uint32_t lw = showLandscape ? obs_source_get_width(landscape) : 0;
		const uint32_t lh = showLandscape ? obs_source_get_height(landscape) : 0;
		const uint32_t vw = showVertical ? obs_source_get_width(vertical) : 0;
		const uint32_t vh = showVertical ? obs_source_get_height(vertical) : 0;
		const double gap = showLandscape && showVertical ? 12.0 : 0.0;
		const double availableHeight = std::min(540.0, static_cast<double>(cy));
		double drawHeight = 0.0;
		if (showLandscape && showVertical) {
			const double combinedAspect = static_cast<double>(lw) / lh + static_cast<double>(vw) / vh;
			drawHeight = std::min(availableHeight, (static_cast<double>(cx) - gap) / combinedAspect);
		} else if (showLandscape) {
			drawHeight = std::min(availableHeight, std::min(static_cast<double>(cx) * lh / lw,
				static_cast<double>(cy)));
		} else if (showVertical) {
			drawHeight = std::min(availableHeight, std::min(static_cast<double>(cx) * vh / vw,
				static_cast<double>(cy)));
		}

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, static_cast<float>(cx), 0.0f, static_cast<float>(cy), -100.0f, 100.0f);
		if (drawHeight > 0.0) {
			const double landscapeScale = showLandscape ? drawHeight / lh : 0.0;
			const double verticalScale = showVertical ? drawHeight / vh : 0.0;
			const int landscapeW = showLandscape ? static_cast<int>(std::lround(lw * landscapeScale)) : 0;
			const int verticalW = showVertical ? static_cast<int>(std::lround(vw * verticalScale)) : 0;
			const int drawW = landscapeW + verticalW + static_cast<int>(gap);
			const int drawH = static_cast<int>(std::lround(drawHeight));
			int x = static_cast<int>((cx - drawW) / 2);
			const int y = static_cast<int>((cy - drawH) / 2);
			if (showLandscape) {
				gs_matrix_push();
				gs_matrix_identity();
				gs_matrix_translate3f(static_cast<float>(x), static_cast<float>(y), 0.0f);
				gs_matrix_scale3f(static_cast<float>(landscapeScale), static_cast<float>(landscapeScale), 1.0f);
				obs_source_video_render(landscape);
				gs_matrix_pop();
				x += landscapeW;
			}
			if (showVertical) {
				if (showLandscape) {
					const int dividerW = 3;
					gs_matrix_push();
					gs_matrix_identity();
					gs_matrix_translate3f(static_cast<float>(x + (static_cast<int>(gap) - dividerW) / 2), static_cast<float>(y), 0.0f);
					gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
					if (solid) {
						struct vec4 color = {0.38f, 0.41f, 0.46f, 1.0f};
						gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);
						while (gs_effect_loop(solid, "Solid")) gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(dividerW), static_cast<uint32_t>(drawH));
					}
					gs_matrix_pop();
					x += static_cast<int>(gap);
				}
				gs_matrix_push();
				gs_matrix_identity();
				gs_matrix_translate3f(static_cast<float>(x), static_cast<float>(y), 0.0f);
				gs_matrix_scale3f(static_cast<float>(verticalScale), static_cast<float>(verticalScale), 1.0f);
				obs_source_video_render(vertical);
				gs_matrix_pop();
			}
		}
		gs_projection_pop();
		gs_viewport_pop();
		if (landscape) obs_source_release(landscape);
		if (vertical) obs_source_release(vertical);
	}

	void releasePreview()
	{
		active_ = false;
		if (resizeTimer_) resizeTimer_->stop();
		if (display_) {
			obs_display_remove_draw_callback(display_, draw, this);
			obs_display_destroy(display_);
			display_ = nullptr;
		}
		obs_source_t *landscape = nullptr;
		obs_source_t *vertical = nullptr;
		{
			std::lock_guard<std::mutex> lock(sourceMutex_);
			landscape = landscape_; vertical = vertical_;
			landscape_ = nullptr; vertical_ = nullptr;
		}
		if (landscape) { obs_source_dec_showing(landscape); obs_source_release(landscape); }
		if (vertical) { obs_source_dec_showing(vertical); obs_source_release(vertical); }
	}
	void createDisplay()
	{
		if (!active_ || display_ || !isVisible() || width() < 2 || height() < 2) return;
		auto *handle = windowHandle();
		if (!handle || !handle->isExposed()) return;
		gs_init_data init{};
		init.window.hwnd = reinterpret_cast<void *>(handle->winId());
		init.cx = static_cast<uint32_t>(width()); init.cy = static_cast<uint32_t>(height());
		init.format = GS_BGRA; init.zsformat = GS_ZS_NONE;
		display_ = obs_display_create(&init, 0x00000000);
		if (display_) {
			displayWidth_ = init.cx; displayHeight_ = init.cy;
			obs_display_add_draw_callback(display_, draw, this);
		}
	}
	void resizeDisplay()
	{
		if (!active_ || !display_ || width() < 2 || height() < 2) return;
		auto *handle = windowHandle();
		if (!handle || !handle->isExposed()) return;
		const auto newWidth = static_cast<uint32_t>(width());
		const auto newHeight = static_cast<uint32_t>(height());
		if (newWidth == displayWidth_ && newHeight == displayHeight_) return;
		obs_display_resize(display_, newWidth, newHeight);
		displayWidth_ = newWidth; displayHeight_ = newHeight;
	}
	int port_;
	obs_display_t *display_ = nullptr;
	QTimer *resizeTimer_ = nullptr;
	uint32_t displayWidth_ = 0, displayHeight_ = 0;
	std::mutex sourceMutex_;
	obs_source_t *landscape_ = nullptr;
	obs_source_t *vertical_ = nullptr;
	bool active_ = false;
};

class DuoCastReceiverPreview final : public QWidget {
public:
	explicit DuoCastReceiverPreview(int port, QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName("receiverPreviewArea");
		setMinimumHeight(260);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		auto *layout = new QGridLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		canvas_ = new DuoCastReceiverCanvas(port, this);
		canvas_->hide();
		layout->addWidget(canvas_, 0, 0);
		button_ = new QPushButton("Enable Receiver Preview", this);
		button_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		layout->addWidget(button_, 0, 0, Qt::AlignCenter);
		waiting_ = new QLabel("Waiting for sender video", this);
		waiting_->setStyleSheet("background:rgba(32,34,37,210);padding:8px 12px;border-radius:4px;color:#d5d8dc;");
		waiting_->setAlignment(Qt::AlignCenter);
		waiting_->hide();
		layout->addWidget(waiting_, 0, 0, Qt::AlignCenter);
		QObject::connect(button_, &QPushButton::clicked, this, [this] { setPreviewEnabled(true); });
		setContextMenuPolicy(Qt::CustomContextMenu);
		QObject::connect(this, &QWidget::customContextMenuRequested, this,
				 [this](const QPoint &point) { showPreviewContextMenu(mapToGlobal(point)); });
		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] { updateWaitingState(); });
		timer->start(500);
	}
	~DuoCastReceiverPreview() override { if (canvas_) canvas_->setActive(false); }

private:
	void setPreviewEnabled(bool enabled)
	{
		if (enabled == enabled_) return;
		enabled_ = enabled;
		if (enabled_) {
			canvas_->setActive(true);
			canvas_->show();
			button_->hide();
			updateWaitingState();
		} else {
			canvas_->setActive(false);
			canvas_->hide();
			button_->show();
			waiting_->hide();
		}
	}
	void updateWaitingState()
	{
		if (!enabled_) return;
		const bool video = receiver_landscape_feed_healthy() ||
			(receiver_vertical_feed_configured() && receiver_vertical_feed_healthy());
		waiting_->setVisible(!video);
	}
	void showPreviewContextMenu(const QPoint &position)
	{
		QMenu menu(this);
		menu.addAction(enabled_ ? "Disable Receiver Preview" : "Enable Receiver Preview",
				[this] { setPreviewEnabled(!enabled_); });
		menu.exec(position);
	}
	DuoCastReceiverCanvas *canvas_ = nullptr;
	QPushButton *button_ = nullptr;
	QLabel *waiting_ = nullptr;
	bool enabled_ = false;
};

QFrame *metric_card(const QString &title, QLabel **value)
{
	auto *card = new QFrame;
	card->setObjectName("metricCard");
	card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	card->setFixedHeight(64);
	auto *layout = new QVBoxLayout(card);
	layout->setContentsMargins(10, 7, 10, 7);
	layout->setSpacing(3);
	auto *heading = new QLabel(title);
	heading->setObjectName("metricTitle");
	*value = new QLabel("—");
	(*value)->setObjectName("metricValue");
	layout->addWidget(heading);
	layout->addWidget(*value);
	return card;
}

void update_duocast_window()
{
	bool sending = false;
	{ std::lock_guard<std::mutex> lock(g_senderMutex); sending = g_senderOutput && obs_output_active(g_senderOutput); }
	const bool connected = sending && sender_output_connected();
	const auto draft = control_draft_snapshot();
	const bool senderEnabled = draft.enable_sender;
	if (g_dockStartButton) g_dockStartButton->setEnabled(!sending);
	if (g_dockStopButton) g_dockStopButton->setEnabled(sending);
	if (!g_duocastDialog) return;
	const bool receiverEnabled = draft.enable_receiver;
	if (g_senderStatusLabel) {
		g_senderStatusLabel->setText(!senderEnabled ? "Sender disabled" : connected ? "Sending · connected" : sending ? "Sending · waiting for receiver" : "Stopped");
		g_senderStatusLabel->setProperty("state", !senderEnabled ? "idle" : connected ? "good" : sending ? "pending" : "idle");
		g_senderStatusLabel->style()->unpolish(g_senderStatusLabel);
		g_senderStatusLabel->style()->polish(g_senderStatusLabel);
	}
	if (g_startButton) g_startButton->setEnabled(senderEnabled && !sending);
	if (g_stopButton) g_stopButton->setEnabled(sending);
	if (g_peerLabel) {
		g_peerLabel->setText(connected ? QString::fromStdString(draft.destination + ":" + std::to_string(draft.port)) : "No active receiver");
	}
	const auto rate_text = [](std::uint64_t kbps) { return QString::number(kbps / 1000.0, 'f', 2) + " Mbps"; };
	const auto audio_rate_text = [](std::uint64_t kbps) { return QString::number(kbps) + " Kbps"; };
	if (g_videoFpsLabel) g_videoFpsLabel->setText(QString::number(g_videoRate.load()) + " fps");
	if (g_videoRateLabel) g_videoRateLabel->setText(rate_text(g_videoKbps.load()));
	if (g_verticalRateLabel) g_verticalRateLabel->setText(QString("%1 fps · %2")
		.arg(g_verticalVideoRate.load()).arg(rate_text(g_verticalVideoKbps.load())));
	if (g_liveRateLabel) g_liveRateLabel->setText(audio_rate_text(g_liveAudioKbps.load()));
	if (g_vodRateLabel) g_vodRateLabel->setText(audio_rate_text(g_vodAudioKbps.load()));
	const auto peer = receiver_active_peer_address();
	const bool video_ok = receiver_video_feed_healthy();
	const auto receiver_state = !receiverEnabled ? "idle" : video_ok ? "good" : peer.empty() ? "pending" : "warning";
	if (g_receiverStatusLabel) {
		g_receiverStatusLabel->setText(!receiverEnabled ? "Receiver disabled" : video_ok ? "Receiving · connected" : peer.empty() ? "Waiting for sender" : "Sender connected · video stalled");
		g_receiverStatusLabel->setProperty("state", receiver_state);
		g_receiverStatusLabel->style()->unpolish(g_receiverStatusLabel);
		g_receiverStatusLabel->style()->polish(g_receiverStatusLabel);
	}
	if (g_receiverPeerLabel) g_receiverPeerLabel->setText(peer.empty() ? "No active sender" : QString::fromStdString(peer));
	if (g_saveFeedbackLabel && g_settingsSavedAt) {
		const auto age = (os_gettime_ns() / 1000000ULL) - g_settingsSavedAt;
		g_saveFeedbackLabel->setVisible(age < 2200);
	}
}

void update_sender_dock()
{
	bool sending = false;
	{
		std::lock_guard<std::mutex> lock(g_senderMutex);
		sending = g_senderOutput && obs_output_active(g_senderOutput);
	}
	const bool enabled = control_draft_snapshot().enable_sender;
	if (g_dockStartButton) g_dockStartButton->setEnabled(!sending);
	if (g_dockStopButton) g_dockStopButton->setEnabled(sending);
}

void create_sender_dock()
{
	auto *content = new QWidget;
	content->setObjectName("duocastSenderQuickControls");
	auto *layout = new QHBoxLayout(content);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);
	g_dockStartButton = new QPushButton("Start Sending", content);
	g_dockStopButton = new QPushButton("Stop Sending", content);
	g_dockStartButton->setMinimumWidth(130);
	g_dockStopButton->setMinimumWidth(130);
	layout->addWidget(g_dockStartButton);
	layout->addWidget(g_dockStopButton);
	QObject::connect(g_dockStartButton, &QPushButton::clicked, content, [] {
		auto draft = control_draft_snapshot();
		if (!draft.enable_sender) {
			draft.enable_sender = true;
			{
				std::lock_guard<std::mutex> lock(g_draftMutex);
				g_controlDraft = draft;
			}
			if (g_enableSenderCheck) {
				const QSignalBlocker blocker(g_enableSenderCheck);
				g_enableSenderCheck->setChecked(true);
			}
		}
		apply_draft_to_source();
		draft = control_draft_snapshot();
		const bool started = start_sender(draft.destination, draft.port, draft.live_track, draft.vod_track,
						  draft.video_quality, draft.max_bitrate_mbps, draft.max_fps,
						  draft.send_vertical, draft.resolution_mode, draft.video_encoder_mode,
						  draft.nvenc_bitrate_mbps);
		blog(started ? LOG_INFO : LOG_ERROR, "DuoCast quick control: sender %s",
		     started ? "started" : "could not start");
		update_duocast_window();
	});
	QObject::connect(g_dockStopButton, &QPushButton::clicked, content, [] {
		stop_sender();
		update_duocast_window();
	});
	auto *timer = new QTimer(content);
	QObject::connect(timer, &QTimer::timeout, content, update_sender_dock);
	timer->start(500);
	if (obs_frontend_add_dock_by_id("obs-duocast-sender-controls", "DuoCast Sender Controls", content)) {
		g_senderDockContent = content;
		g_senderDockRegistered = true;
	} else {
		g_dockStartButton = nullptr;
		g_dockStopButton = nullptr;
		delete content;
		blog(LOG_WARNING, "DuoCast: could not register the Sender Controls dock");
	}
	update_sender_dock();
}

QLabel *add_stats_cell(QGridLayout *grid, int row, int column, const QString &caption)
{
	auto *captionLabel = new QLabel(caption);
	captionLabel->setStyleSheet("color:#b7c0ce;");
	auto *valueLabel = new QLabel("—");
	valueLabel->setMinimumWidth(92);
	grid->addWidget(captionLabel, row, column * 2);
	grid->addWidget(valueLabel, row, column * 2 + 1, Qt::AlignRight);
	return valueLabel;
}

void create_receiver_stats_dock()
{
	auto *content = new QWidget;
	content->setObjectName("duocastReceiverStats");
	content->setMinimumWidth(620);
	auto *root = new QVBoxLayout(content);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(6);
	auto *generalTitle = new QLabel("Receiver OBS");
	generalTitle->setStyleSheet("font-weight:600;");
	root->addWidget(generalTitle);
	auto *general = new QGridLayout;
	general->setHorizontalSpacing(16);
	general->setVerticalSpacing(4);
	g_remoteCpuLabel = add_stats_cell(general, 0, 0, "CPU Usage");
	g_remoteFpsLabel = add_stats_cell(general, 0, 1, "FPS");
	g_remoteRenderLabel = add_stats_cell(general, 1, 0, "Average render time");
	g_remoteMemoryLabel = add_stats_cell(general, 1, 1, "Memory Usage");
	g_remoteRenderLagLabel = add_stats_cell(general, 2, 0, "Frames missed to rendering lag");
	g_remoteEncodeSkipLabel = add_stats_cell(general, 2, 1, "Skipped frames due to encoding lag");
	g_remoteDiskLabel = add_stats_cell(general, 3, 0, "Disk space available");
	root->addLayout(general);
	auto *outputTitle = new QLabel("Output");
	outputTitle->setStyleSheet("font-weight:600;");
	root->addWidget(outputTitle);
	auto *outputs = new QGridLayout;
	outputs->setHorizontalSpacing(14);
	outputs->setVerticalSpacing(4);
	const char *outputHeaders[] = {"Output", "Status", "Dropped Frames (Network)", "Total Data Output", "Bitrate"};
	for (int c = 0; c < 5; ++c) {
		auto *header = new QLabel(outputHeaders[c]);
		header->setStyleSheet("font-weight:600;color:#d7deea;");
		outputs->addWidget(header, 0, c);
	}
	outputs->addWidget(new QLabel("Stream"), 1, 0);
	g_remoteStreamStatusLabel = new QLabel("—"); outputs->addWidget(g_remoteStreamStatusLabel, 1, 1);
	g_remoteStreamDroppedLabel = new QLabel("—"); outputs->addWidget(g_remoteStreamDroppedLabel, 1, 2);
	g_remoteStreamDataLabel = new QLabel("—"); outputs->addWidget(g_remoteStreamDataLabel, 1, 3);
	g_remoteStreamBitrateLabel = new QLabel("—"); outputs->addWidget(g_remoteStreamBitrateLabel, 1, 4);
	outputs->addWidget(new QLabel("Recording"), 2, 0);
	g_remoteRecordingStatusLabel = new QLabel("—"); outputs->addWidget(g_remoteRecordingStatusLabel, 2, 1);
	outputs->addWidget(new QLabel("—"), 2, 2);
	g_remoteRecordingDataLabel = new QLabel("—"); outputs->addWidget(g_remoteRecordingDataLabel, 2, 3);
	outputs->addWidget(new QLabel("—"), 2, 4);
	root->addLayout(outputs);
	auto *duoTitle = new QLabel("DuoCast Transport");
	duoTitle->setStyleSheet("font-weight:600;");
	root->addWidget(duoTitle);
	auto *duo = new QGridLayout;
	duo->setHorizontalSpacing(16);
	duo->setVerticalSpacing(4);
	g_duocastLinkLabel = add_stats_cell(duo, 0, 0, "Receiver link");
	g_duocastLandscapeLabel = add_stats_cell(duo, 0, 1, "Landscape feed");
	g_duocastVerticalLabel = add_stats_cell(duo, 1, 0, "Vertical feed");
	g_duocastLiveAudioLabel = add_stats_cell(duo, 1, 1, "Live Mix Audio");
	g_duocastVodAudioLabel = add_stats_cell(duo, 2, 0, "VOD Mix Audio");
	root->addLayout(duo);
	root->addStretch();
	auto *timer = new QTimer(content);
	QObject::connect(timer, &QTimer::timeout, content, [] {
		// Frontend output APIs must run on Qt's UI thread, not libobs' graphics
		// tick callback. This timer also publishes telemetry if the dock is hidden.
		update_receiver_host_stats();
		update_receiver_stats_dock();
	});
	timer->start(1000);
	if (obs_frontend_add_dock_by_id("obs-duocast-receiver-stats", "DuoCast Receiver Stats", content)) {
		g_receiverStatsDockContent = content;
		g_receiverStatsDockRegistered = true;
	} else {
		delete content;
		g_remoteCpuLabel = g_remoteFpsLabel = g_remoteRenderLabel = g_remoteMemoryLabel = nullptr;
		g_remoteRenderLagLabel = g_remoteEncodeSkipLabel = g_remoteDiskLabel = nullptr;
		g_remoteStreamStatusLabel = g_remoteStreamDroppedLabel = g_remoteStreamDataLabel = g_remoteStreamBitrateLabel = nullptr;
		g_remoteRecordingStatusLabel = g_remoteRecordingDataLabel = nullptr;
		g_duocastLinkLabel = g_duocastLandscapeLabel = g_duocastVerticalLabel = nullptr;
		g_duocastLiveAudioLabel = g_duocastVodAudioLabel = nullptr;
		blog(LOG_WARNING, "DuoCast: could not register the Receiver Stats dock");
	}
	update_receiver_stats_dock();
}

void update_receiver_stats_dock()
{
	if (!g_receiverStatsDockRegistered) return;
	const auto remote = sender_remote_stats();
	const auto now = os_gettime_ns();
	const bool fresh = remote.received_at_ns && now >= remote.received_at_ns && now - remote.received_at_ns < 3000000000ULL;
	const auto &s = remote.stats;
	const auto set = [fresh](QLabel *label, const QString &value) { if (label) label->setText(fresh ? value : "—"); };
	set(g_remoteCpuLabel, QString::number(s.cpu_tenths_percent / 10.0, 'f', 1) + "%");
	set(g_remoteFpsLabel, QString::number(s.fps_milli / 1000.0, 'f', 2));
	set(g_remoteRenderLabel, QString::number(s.render_time_tenths_ms / 10.0, 'f', 1) + " ms");
	set(g_remoteMemoryLabel, QString::number(s.memory_mb) + " MB");
	auto frameText = [](std::uint32_t dropped, std::uint32_t total) {
		const double percent = total ? 100.0 * dropped / total : 0.0;
		return QString("%1 / %2 (%3%)").arg(dropped).arg(total).arg(percent, 0, 'f', 1);
	};
	set(g_remoteRenderLagLabel, frameText(s.render_lagged, s.render_total));
	set(g_remoteEncodeSkipLabel, frameText(s.encode_skipped, s.encode_total));
	set(g_remoteDiskLabel, QString::number(s.disk_free_tenths_gb / 10.0, 'f', 1) + " GB");
	const bool streaming = fresh && (s.flags & 1U);
	const bool recording = fresh && (s.flags & 2U);
	if (g_remoteStreamStatusLabel) g_remoteStreamStatusLabel->setText(!fresh ? "No receiver telemetry" :
		streaming ? ((s.flags & 4U) ? "Reconnecting" : "Live") : "Inactive");
	set(g_remoteStreamDroppedLabel, frameText(s.stream_dropped, s.stream_total));
	set(g_remoteStreamDataLabel, QString::number(s.stream_bytes / (1024.0 * 1024.0), 'f', 1) + " MiB");
	const auto bitrateText = [](std::uint32_t kbps) {
		return kbps >= 10000 ? QString::number(kbps / 1000.0, 'f', 1) + " Mb/s" : QString::number(kbps) + " kb/s";
	};
	set(g_remoteStreamBitrateLabel, bitrateText(s.stream_bitrate_kbps));
	if (g_remoteRecordingStatusLabel) g_remoteRecordingStatusLabel->setText(!fresh ? "—" : recording ? "Recording" : "Inactive");
	set(g_remoteRecordingDataLabel, QString::number(s.recording_bytes / (1024.0 * 1024.0), 'f', 1) + " MiB");
	if (g_duocastLinkLabel) g_duocastLinkLabel->setText(sender_output_connected() ? "Connected" : "Disconnected");
	auto senderRate = [&](QLabel *label, std::uint64_t fps, std::uint64_t kbps) {
		if (label) label->setText(QString("%1 fps · %2").arg(fps).arg(bitrateText(static_cast<std::uint32_t>(std::min<std::uint64_t>(kbps, UINT32_MAX)))));
	};
	senderRate(g_duocastLandscapeLabel, g_videoRate.load(), g_videoKbps.load());
	senderRate(g_duocastVerticalLabel, g_verticalVideoRate.load(), g_verticalVideoKbps.load());
	if (g_duocastLiveAudioLabel) g_duocastLiveAudioLabel->setText(QString::number(g_liveAudioKbps.load()) + " Kbps");
	if (g_duocastVodAudioLabel) g_duocastVodAudioLabel->setText(QString::number(g_vodAudioKbps.load()) + " Kbps");
}

void update_receiver_host_stats()
{
	obs_duocast::protocol::ReceiverStats stats;
	const auto now_ns = os_gettime_ns();
	struct obs_video_info videoInfo{};
	if (obs_get_video_info(&videoInfo) && videoInfo.fps_num && videoInfo.fps_den)
		stats.fps_milli = static_cast<std::uint32_t>(std::clamp(obs_get_active_fps() * 1000.0, 0.0, 100000.0));
	if (g_cpuUsageInfo)
		stats.cpu_tenths_percent = static_cast<std::uint32_t>(std::clamp(std::lround(os_cpu_usage_info_query(g_cpuUsageInfo) * 10.0), 0L, 1000L));
	stats.memory_mb = static_cast<std::uint32_t>(std::min<std::uint64_t>(os_get_proc_resident_size() / (1024ULL * 1024ULL), UINT32_MAX));
	stats.render_time_tenths_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(obs_get_average_frame_time_ns() / 100000ULL, UINT32_MAX));
	stats.render_lagged = obs_get_lagged_frames();
	stats.render_total = obs_get_total_frames();
	video_t *video = obs_get_video();
	if (video) {
		stats.encode_skipped = video_output_get_skipped_frames(video);
		stats.encode_total = video_output_get_total_frames(video);
	}
	obs_output_t *stream = obs_frontend_get_streaming_output();
	if (stream) {
		if (obs_output_active(stream)) {
			stats.flags |= 1U;
			if (obs_output_reconnecting(stream)) stats.flags |= 4U;
			stats.stream_dropped = static_cast<std::uint32_t>(std::max(0, obs_output_get_frames_dropped(stream)));
			stats.stream_total = static_cast<std::uint32_t>(std::max(0, obs_output_get_total_frames(stream)));
			stats.stream_bytes = obs_output_get_total_bytes(stream);
		}
		obs_output_release(stream);
	}
	if (g_lastReceiverBitrateAtNs && now_ns > g_lastReceiverBitrateAtNs && stats.stream_bytes >= g_lastReceiverStreamBytes) {
		const auto elapsed = now_ns - g_lastReceiverBitrateAtNs;
		const long double kbps = static_cast<long double>(stats.stream_bytes - g_lastReceiverStreamBytes) * 8.0e6L / elapsed;
		stats.stream_bitrate_kbps = static_cast<std::uint32_t>(std::clamp(kbps, 0.0L, static_cast<long double>(UINT32_MAX)));
	}
	g_lastReceiverBitrateAtNs = now_ns;
	g_lastReceiverStreamBytes = stats.stream_bytes;
	obs_output_t *recording = obs_frontend_get_recording_output();
	if (recording) {
		if (obs_output_active(recording)) stats.flags |= 2U;
		stats.recording_bytes = obs_output_get_total_bytes(recording);
		obs_output_release(recording);
	}
	config_t *profile = obs_frontend_get_profile_config();
	if (profile) {
		const char *mode = config_get_string(profile, "Output", "Mode");
		const char *path = mode && strcmp(mode, "Advanced") == 0
			? config_get_string(profile, "AdvOut", "RecFilePath")
			: config_get_string(profile, "SimpleOutput", "FilePath");
		if (path && *path) {
			const auto free_bytes = os_get_free_disk_space(path);
			stats.disk_free_tenths_gb = static_cast<std::uint32_t>(std::min<std::uint64_t>(free_bytes / (1024ULL * 1024ULL * 1024ULL / 10ULL), UINT32_MAX));
		}
	}
	receiver_update_remote_stats(stats);
}

void read_ui_into_draft()
{
	ControlDraft draft = control_draft_snapshot();
	if (g_destinationEdit) draft.destination = g_destinationEdit->text().trimmed().toStdString();
	if (g_enableSenderCheck) draft.enable_sender = g_enableSenderCheck->isChecked();
	if (g_enableReceiverCheck) draft.enable_receiver = g_enableReceiverCheck->isChecked();
	if (g_portSpin) draft.port = g_portSpin->value();
	if (g_liveTrackCombo) draft.live_track = g_liveTrackCombo->currentData().toInt();
	if (g_vodTrackCombo) draft.vod_track = g_vodTrackCombo->currentData().toInt();
	if (g_qualitySlider) draft.video_quality = g_qualitySlider->value();
	if (g_videoEncoderCombo) draft.video_encoder_mode = g_videoEncoderCombo->currentData().toInt();
	if (g_nvencBitrateSpin) draft.nvenc_bitrate_mbps = g_nvencBitrateSpin->value();
	if (g_maxBitrateSpin) draft.max_bitrate_mbps = g_maxBitrateSpin->value();
	if (g_maxFpsCombo) draft.max_fps = g_maxFpsCombo->currentData().toInt();
	if (g_videoResolutionCombo) draft.resolution_mode = g_videoResolutionCombo->currentData().toInt();
	if (g_autostartCheck) draft.autostart = g_autostartCheck->isChecked();
	if (g_sendVerticalCheck) draft.send_vertical = g_sendVerticalCheck->isChecked();
	if (g_fallbackCheck) draft.fallback_enabled = g_fallbackCheck->isChecked();
	if (g_receiverBufferSpin) draft.receiver_sync_buffer_ms = g_receiverBufferSpin->value();
	if (g_autoRecoverLowFpsCheck) draft.auto_recover_low_fps = g_autoRecoverLowFpsCheck->isChecked();
	if (g_forceAutoSwitchLiveCheck) draft.force_auto_switch_live = g_forceAutoSwitchLiveCheck->isChecked();
	if (g_fallbackSceneCombo) draft.fallback_scene = g_fallbackSceneCombo->currentData().toString().toStdString();
	if (g_verticalFallbackSceneCombo) draft.vertical_fallback_scene = g_verticalFallbackSceneCombo->currentData().toString().toStdString();
	if (g_landscapeLiveSceneCombo) draft.landscape_live_scene = g_landscapeLiveSceneCombo->currentData().toString().toStdString();
	if (g_verticalLiveSceneCombo) draft.vertical_live_scene = g_verticalLiveSceneCombo->currentData().toString().toStdString();
	{ std::lock_guard<std::mutex> lock(g_draftMutex); g_controlDraft = std::move(draft); }
}

void save_ui_settings()
{
	read_ui_into_draft();
	const bool ok = apply_draft_to_source();
	g_settingsSaveOk = ok; g_settingsSavedAt = os_gettime_ns() / 1000000ULL;
	if (g_saveFeedbackLabel) {
		g_saveFeedbackLabel->setText(ok ? "Settings saved" : "Could not save settings");
		g_saveFeedbackLabel->setStyleSheet(ok ? "color:#62d996;font-weight:600;" : "color:#ff7777;font-weight:600;");
		g_saveFeedbackLabel->setVisible(true);
	}
	blog(ok ? LOG_INFO : LOG_ERROR, "DuoCast: settings %s", ok ? "saved" : "could not be saved");
}

void show_duocast_tool(void *)
{
	if (g_duocastDialog) { g_duocastDialog->show(); g_duocastDialog->raise(); g_duocastDialog->activateWindow(); return; }
	blog(LOG_INFO, "DuoCast UI: opening Tools window");
	const auto draft = control_draft_snapshot();
	auto *dialog = new QDialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	g_duocastDialog = dialog;
	blog(LOG_INFO, "DuoCast UI: dialog created; building sender page");
	dialog->setWindowTitle("OBS DuoCast");
	dialog->setMinimumSize(900, 620);
	dialog->resize(1040, 820);
	dialog->setStyleSheet("QDialog { background:#202225; color:#f2f2f2; } QLabel { color:#e6e7e9; } QTabWidget::pane { border:1px solid #3a3d42; border-radius:5px; top:-1px; } QTabBar::tab { background:#292c30; color:#b9bdc4; padding:9px 22px; border:1px solid #3a3d42; border-bottom:0; } QTabBar::tab:selected { background:#34383e; color:white; } QFrame#metricCard { background:#292c30; border:1px solid #393d43; border-radius:6px; } QFrame#connectionStrip { background:#292c30; border:1px solid #393d43; border-radius:5px; } QLabel#metricTitle { color:#aeb4bd; font-size:8pt; } QLabel#metricValue { font-size:14pt; font-weight:600; color:#f6f7f8; } QLabel#senderStatus[state='good'], QLabel#receiverStatus[state='good'] { color:#62d996; font-weight:600; } QLabel#senderStatus[state='pending'], QLabel#receiverStatus[state='pending'], QLabel#receiverStatus[state='warning'] { color:#f0c36b; font-weight:600; } QLabel#senderStatus[state='idle'], QLabel#receiverStatus[state='idle'] { color:#aeb4bd; font-weight:600; } QPushButton { background:#3b4149; border:1px solid #4a515a; border-radius:4px; padding:7px 14px; } QPushButton:hover { background:#4a515a; } QPushButton:disabled { color:#777; } QLineEdit,QSpinBox,QComboBox { background:#292c30; border:1px solid #454950; border-radius:3px; padding:5px; min-height:22px; }");
	auto *root = new QVBoxLayout(dialog);
	auto *header = new QHBoxLayout;
	auto *title = new QLabel("DuoCast"); title->setStyleSheet("font-size:18pt;font-weight:600;");
	g_enableSenderCheck = new QCheckBox("Enable DuoCast Sender");
	g_enableSenderCheck->setChecked(draft.enable_sender);
	g_enableReceiverCheck = new QCheckBox("Enable DuoCast Receiver");
	g_enableReceiverCheck->setChecked(draft.enable_receiver);
	auto *version = new QLabel("OBS DuoCast 0.14.29 Beta"); version->setStyleSheet("color:#aeb4bd;");
	header->addWidget(title); header->addSpacing(18); header->addWidget(g_enableSenderCheck); header->addWidget(g_enableReceiverCheck); header->addStretch(); header->addWidget(version); root->addLayout(header);
	auto *tabs = new QTabWidget; root->addWidget(tabs, 1);
	auto *senderPage = new QWidget; auto *senderLayout = new QVBoxLayout(senderPage);
	auto *preview = new DuoCastPreviewArea; senderLayout->addWidget(preview, 1);
	// Match the canvas preview to the saved sender option on opening the UI.
	// Previously this was forced off, so the portrait feed was hidden even when
	// the sender was configured to transmit it.
	preview->setVerticalEnabled(draft.send_vertical);
	auto *stats = new QGridLayout; stats->setSpacing(10);
	stats->addWidget(metric_card("LANDSCAPE FPS", &g_videoFpsLabel),0,0); stats->addWidget(metric_card("LANDSCAPE VIDEO", &g_videoRateLabel),0,1);
	stats->addWidget(metric_card("VERTICAL FEED", &g_verticalRateLabel),0,2); stats->addWidget(metric_card("LIVE MIX AUDIO", &g_liveRateLabel),0,3);
	stats->addWidget(metric_card("VOD MIX AUDIO", &g_vodRateLabel),0,4);
	senderLayout->addLayout(stats);
	auto *senderForm = new QFormLayout; senderForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	g_destinationEdit = new QLineEdit(QString::fromStdString(draft.destination)); g_portSpin = new QSpinBox; g_portSpin->setRange(1,65535); g_portSpin->setValue(draft.port);
	g_liveTrackCombo = new QComboBox; g_vodTrackCombo = new QComboBox;
	for (int i=1;i<=6;++i) { const QString label = QString("Track %1").arg(i); g_liveTrackCombo->addItem(label,i); g_vodTrackCombo->addItem(label,i); }
	g_liveTrackCombo->setCurrentIndex(g_liveTrackCombo->findData(draft.live_track)); g_vodTrackCombo->setCurrentIndex(g_vodTrackCombo->findData(draft.vod_track));
	g_videoEncoderCombo = new QComboBox;
	g_videoEncoderCombo->addItem("MJPEG (TurboJPEG)", 0);
	g_videoEncoderCombo->addItem("NVENC H.264 (GPU texture)", 1);
	g_videoEncoderCombo->addItem("NVENC HEVC / H.265 (GPU texture)", 2);
	g_videoEncoderCombo->setCurrentIndex(g_videoEncoderCombo->findData(draft.video_encoder_mode));
	g_videoEncoderCombo->setToolTip("Choose MJPEG, NVENC H.264, or NVENC HEVC/H.265. HEVC requires sender NVENC and receiver decoder support.");
	auto *qualityRow = new QWidget; auto *qualityLayout = new QHBoxLayout(qualityRow); qualityLayout->setContentsMargins(0,0,0,0); qualityLayout->setSpacing(10);
	g_qualitySlider = new QSlider(Qt::Horizontal); g_qualitySlider->setRange(1,100); g_qualitySlider->setValue(draft.video_quality);
	g_qualitySlider->setToolTip("MJPEG mode only. Higher TurboJPEG quality preserves more detail and uses more bandwidth.");
	g_qualityPercentLabel = new QLabel(QString("%1%").arg(g_qualitySlider->value())); g_qualityPercentLabel->setMinimumWidth(42); g_qualityPercentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	qualityLayout->addWidget(g_qualitySlider,1); qualityLayout->addWidget(g_qualityPercentLabel);
	g_maxBitrateSpin = new QSpinBox; g_maxBitrateSpin->setRange(0, 1000); g_maxBitrateSpin->setSpecialValueText("No cap"); g_maxBitrateSpin->setSuffix(" Mbps");
	g_maxBitrateSpin->setValue(draft.max_bitrate_mbps);
	g_maxBitrateSpin->setToolTip("Shared network pacing limit for landscape video, optional portrait video, and both audio tracks. 0 disables pacing.");
	g_nvencBitrateSpin = new QSpinBox; g_nvencBitrateSpin->setRange(1, 1000); g_nvencBitrateSpin->setSuffix(" Mbps");
	g_nvencBitrateSpin->setValue(draft.nvenc_bitrate_mbps);
	g_nvencBitrateSpin->setToolTip("NVENC CBR video target per feed. If portrait is enabled, there are two encoders using this target.");
	g_videoRateHintLabel = new QLabel; g_videoRateHintLabel->setWordWrap(true);
	g_videoRateHintLabel->setStyleSheet("color:#aeb4bd; font-size:9pt;");
	g_maxFpsCombo = new QComboBox;
	g_maxFpsCombo->addItem("Canvas FPS (no cap)", 0);
	for (const int fps : {120, 60, 50, 30, 25, 24, 20, 15})
		g_maxFpsCombo->addItem(QString("%1 FPS").arg(fps), fps);
	const int fpsIndex = g_maxFpsCombo->findData(draft.max_fps);
	g_maxFpsCombo->setCurrentIndex(fpsIndex >= 0 ? fpsIndex : 0);
	g_maxFpsCombo->setToolTip("Applies to both outgoing feeds. Frames above the selected cap are skipped before encoding; NVENC uses OBS frame-rate pacing.");
	g_videoResolutionCombo = new QComboBox;
	g_videoResolutionCombo->addItem("OBS canvas resolution", 0);
	g_videoResolutionCombo->addItem("OBS output resolution", 1);
	g_videoResolutionCombo->setCurrentIndex(g_videoResolutionCombo->findData(draft.resolution_mode));
	g_videoResolutionCombo->setToolTip("Choose the landscape feed dimensions. Output resolution can reduce compression work while preserving 60 FPS.");
	g_autostartCheck = new QCheckBox("Start sending when OBS opens"); g_autostartCheck->setChecked(draft.autostart);
	g_sendVerticalCheck = new QCheckBox("Send Aitum Vertical canvas");
	g_sendVerticalCheck->setChecked(draft.send_vertical);
	g_sendVerticalCheck->setToolTip("Optional. DuoCast checks for an active Aitum Vertical canvas when you enable this option.");
	senderForm->addRow("Receiver address",g_destinationEdit); senderForm->addRow("Port",g_portSpin); senderForm->addRow("Live mix track",g_liveTrackCombo); senderForm->addRow("VOD mix track",g_vodTrackCombo); senderForm->addRow("Video encoder",g_videoEncoderCombo); senderForm->addRow("MJPEG quality",qualityRow); senderForm->addRow("NVENC bitrate per feed",g_nvencBitrateSpin); senderForm->addRow("Total network cap",g_maxBitrateSpin); senderForm->addRow("",g_videoRateHintLabel); senderForm->addRow("Max video FPS",g_maxFpsCombo); senderForm->addRow("Video resolution",g_videoResolutionCombo); senderForm->addRow("",g_sendVerticalCheck); senderForm->addRow("",g_autostartCheck); senderLayout->addLayout(senderForm);
	auto update_video_rate_controls = [senderForm, qualityRow] {
		const int mode = g_videoEncoderCombo ? g_videoEncoderCombo->currentData().toInt() : 0;
		const bool nvenc = mode > 0;
		g_qualitySlider->setEnabled(!nvenc);
		g_qualityPercentLabel->setEnabled(!nvenc);
		g_nvencBitrateSpin->setEnabled(nvenc);
		if (auto *label = senderForm->labelForField(qualityRow)) label->setVisible(!nvenc);
		qualityRow->setVisible(!nvenc);
		if (auto *label = senderForm->labelForField(g_nvencBitrateSpin)) label->setVisible(nvenc);
		g_nvencBitrateSpin->setVisible(nvenc);
		if (!nvenc) {
			g_videoRateHintLabel->setText("MJPEG quality controls compression per frame. The total network cap is shared by all video feeds and both audio tracks; lowering it can reduce delivered FPS.");
		} else {
			const int feeds = g_sendVerticalCheck && g_sendVerticalCheck->isChecked() ? 2 : 1;
			const int videoTarget = g_nvencBitrateSpin->value() * feeds;
			const int cap = g_maxBitrateSpin->value();
			QString message = QString("NVENC target is %1 Mbps per video feed (%2 Mbps combined video target%3). Total network cap includes all feeds and audio.")
				.arg(g_nvencBitrateSpin->value()).arg(videoTarget)
				.arg(feeds == 2 ? " with portrait" : "");
			if (cap == 0) message += " No cap is applied.";
			else if (cap <= videoTarget) message += QString(" Current %1 Mbps cap is below the combined video target and will throttle frames.").arg(cap);
			else message += QString(" Current cap leaves about %1 Mbps for audio and overhead.").arg(cap - videoTarget);
			g_videoRateHintLabel->setText(message);
		}
	};
	update_video_rate_controls();
	auto *sendButtons = new QHBoxLayout; sendButtons->setSpacing(8); g_startButton = new QPushButton("Start sending"); g_stopButton = new QPushButton("Stop sending"); sendButtons->addWidget(g_startButton); sendButtons->addWidget(g_stopButton); sendButtons->addStretch(); senderLayout->addLayout(sendButtons);
	auto *connectionStrip = new QFrame; connectionStrip->setObjectName("connectionStrip");
	auto *connectionLayout = new QHBoxLayout(connectionStrip); connectionLayout->setContentsMargins(10,5,10,5); connectionLayout->setSpacing(12);
	g_senderStatusLabel = new QLabel; g_senderStatusLabel->setObjectName("senderStatus");
	g_peerLabel = new QLabel("No active receiver"); g_peerLabel->setStyleSheet("color:#aeb4bd;");
	connectionLayout->addWidget(g_senderStatusLabel); connectionLayout->addStretch(); connectionLayout->addWidget(g_peerLabel);
	senderLayout->addWidget(connectionStrip);
	tabs->addTab(senderPage,"Sender");
	blog(LOG_INFO, "DuoCast UI: sender page built; building receiver page");
	auto *receiverPage = new QWidget; auto *receiverLayout = new QVBoxLayout(receiverPage);
	auto *receiverConnectionStrip = new QFrame; receiverConnectionStrip->setObjectName("connectionStrip");
	auto *receiverConnectionLayout = new QHBoxLayout(receiverConnectionStrip); receiverConnectionLayout->setContentsMargins(10,5,10,5); receiverConnectionLayout->setSpacing(12);
	g_receiverStatusLabel = new QLabel; g_receiverStatusLabel->setObjectName("receiverStatus");
	g_receiverPeerLabel = new QLabel("No active sender"); g_receiverPeerLabel->setStyleSheet("color:#aeb4bd;");
	receiverConnectionLayout->addWidget(g_receiverStatusLabel); receiverConnectionLayout->addStretch(); receiverConnectionLayout->addWidget(g_receiverPeerLabel);
	receiverLayout->addWidget(receiverConnectionStrip);
	auto *receiverPreview = new DuoCastReceiverPreview(17777); receiverLayout->addWidget(receiverPreview, 1);
	QFrame *receiverCard = new QFrame; receiverCard->setObjectName("metricCard"); auto *receiverForm = new QFormLayout(receiverCard);
	g_receiverBufferSpin = new QSpinBox;
	g_receiverBufferSpin->setRange(0, 3000);
	g_receiverBufferSpin->setSingleStep(50);
	g_receiverBufferSpin->setSuffix(" ms");
	g_receiverBufferSpin->setValue(draft.receiver_sync_buffer_ms);
	g_receiverBufferSpin->setToolTip("Holds compressed receiver packets so audio and video play from one shared timeline. Adds the selected delay. Changes apply when the sender reconnects.");
	receiverForm->addRow("A/V sync buffer", g_receiverBufferSpin);
	g_autoRecoverLowFpsCheck = new QCheckBox("Automatically recover a low-FPS feed");
	g_autoRecoverLowFpsCheck->setChecked(draft.auto_recover_low_fps);
	g_autoRecoverLowFpsCheck->setToolTip("When decoded video remains below 60% of its configured frame rate for five seconds, request a sender transport reconnect. Rearms after the feed stays healthy for ten seconds.");
	receiverForm->addRow("Feed recovery", g_autoRecoverLowFpsCheck);
	g_fallbackCheck = new QCheckBox("Switch to fallback scene when landscape video is lost"); g_fallbackCheck->setChecked(draft.fallback_enabled); g_fallbackSceneCombo = new QComboBox; g_fallbackSceneCombo->addItem("Select a scene","");
	// obs_frontend_get_scene_names returns one packed allocation containing both
	// the pointer table and all strings. Freeing individual entries corrupts the
	// heap while the Receiver page is being built.
	char **scenes = obs_frontend_get_scene_names(); if (scenes) { for (size_t i=0; scenes[i]; ++i) { QString scene = QString::fromUtf8(scenes[i]); g_fallbackSceneCombo->addItem(scene,scene); } bfree(scenes); }
	const int fallbackIndex = g_fallbackSceneCombo->findData(QString::fromStdString(draft.fallback_scene)); if (fallbackIndex >= 0) g_fallbackSceneCombo->setCurrentIndex(fallbackIndex);
	g_landscapeLiveSceneCombo = new QComboBox; g_landscapeLiveSceneCombo->addItem("Select a landscape live scene","");
	char **liveScenes = obs_frontend_get_scene_names(); if (liveScenes) { for (size_t i=0; liveScenes[i]; ++i) { QString scene = QString::fromUtf8(liveScenes[i]); g_landscapeLiveSceneCombo->addItem(scene,scene); } bfree(liveScenes); }
	const int liveIndex = g_landscapeLiveSceneCombo->findData(QString::fromStdString(draft.landscape_live_scene)); if (liveIndex >= 0) g_landscapeLiveSceneCombo->setCurrentIndex(liveIndex);
	g_verticalFallbackSceneCombo = new QComboBox; g_verticalFallbackSceneCombo->addItem("Aitum Vertical scenes load when selected", "");
	g_verticalFallbackSceneCombo->setEnabled(false);
	const int verticalFallbackIndex = g_verticalFallbackSceneCombo->findData(QString::fromStdString(draft.vertical_fallback_scene)); if (verticalFallbackIndex >= 0) g_verticalFallbackSceneCombo->setCurrentIndex(verticalFallbackIndex);
	g_verticalLiveSceneCombo = new QComboBox; g_verticalLiveSceneCombo->addItem("Aitum Vertical scenes load when selected", "");
	g_verticalLiveSceneCombo->setEnabled(false);
	const int verticalLiveIndex = g_verticalLiveSceneCombo->findData(QString::fromStdString(draft.vertical_live_scene)); if (verticalLiveIndex >= 0) g_verticalLiveSceneCombo->setCurrentIndex(verticalLiveIndex);
	g_forceAutoSwitchLiveCheck = new QCheckBox("Force Auto Switch to Live Scene"); g_forceAutoSwitchLiveCheck->setChecked(draft.force_auto_switch_live);
	g_forceAutoSwitchLiveCheck->setToolTip("While each received video feed is healthy, automatically return to its selected live scene if another scene is chosen.");
	receiverForm->addRow("",g_forceAutoSwitchLiveCheck); receiverForm->addRow("Landscape Live Scene",g_landscapeLiveSceneCombo); receiverForm->addRow("Aitum Vertical LiveScene",g_verticalLiveSceneCombo);
	receiverForm->addRow("Fallback",g_fallbackCheck); receiverForm->addRow("Landscape fallback scene",g_fallbackSceneCombo); receiverForm->addRow("Aitum Vertical fallback scene",g_verticalFallbackSceneCombo); receiverLayout->addWidget(receiverCard); receiverLayout->addStretch(); tabs->addTab(receiverPage,"Receiver");
	blog(LOG_INFO, "DuoCast UI: receiver page built; connecting controls");
	auto *footer = new QHBoxLayout; g_saveFeedbackLabel = new QLabel; g_saveFeedbackLabel->setStyleSheet("color:#62d996;font-weight:600;"); g_saveFeedbackLabel->setVisible(false); auto *save = new QPushButton("Save settings"); footer->addWidget(g_saveFeedbackLabel); footer->addStretch(); footer->addWidget(save); root->addLayout(footer);
	QObject::connect(save,&QPushButton::clicked,dialog,save_ui_settings);
	QObject::connect(g_startButton,&QPushButton::clicked,dialog,[] { read_ui_into_draft(); save_ui_settings(); const auto d=control_draft_snapshot(); start_sender(d.destination,d.port,d.live_track,d.vod_track,d.video_quality,d.max_bitrate_mbps,d.max_fps,d.send_vertical,d.resolution_mode,d.video_encoder_mode,d.nvenc_bitrate_mbps); update_duocast_window(); });
	QObject::connect(g_stopButton,&QPushButton::clicked,dialog,[] { stop_sender(); update_duocast_window(); });
	QObject::connect(tabs, &QTabWidget::currentChanged, dialog, [draft, verticalScenesLoaded = false](int index) mutable {
		if (index != 1 || verticalScenesLoaded) return;
		verticalScenesLoaded = true;
		blog(LOG_INFO, "DuoCast UI: loading Aitum Vertical scene choices");
		const auto scenes = aitum_vertical_scenes();
		auto fill = [&scenes](QComboBox *combo, const QString &placeholder, const std::string &selected) {
			if (!combo) return;
			combo->clear();
			combo->addItem(placeholder, "");
			for (const auto &scene : scenes)
				combo->addItem(QString::fromStdString(scene), QString::fromStdString(scene));
			combo->setEnabled(!scenes.empty());
			if (scenes.empty()) combo->setItemText(0, "Aitum Vertical canvas unavailable");
			const int selectedIndex = combo->findData(QString::fromStdString(selected));
			if (selectedIndex >= 0) combo->setCurrentIndex(selectedIndex);
		};
		fill(g_verticalFallbackSceneCombo, "Select an Aitum Vertical scene", draft.vertical_fallback_scene);
		fill(g_verticalLiveSceneCombo, "Select an Aitum Vertical live scene", draft.vertical_live_scene);
		blog(LOG_INFO, "DuoCast UI: Aitum Vertical scene choices loaded (%zu)", scenes.size());
	});
	QObject::connect(g_destinationEdit,&QLineEdit::textChanged,dialog,read_ui_into_draft);
	QObject::connect(g_portSpin,qOverload<int>(&QSpinBox::valueChanged),dialog,read_ui_into_draft);
	QObject::connect(g_qualitySlider,&QSlider::valueChanged,dialog,[](int value) { if (g_qualityPercentLabel) g_qualityPercentLabel->setText(QString("%1%").arg(value)); read_ui_into_draft(); });
	QObject::connect(g_videoEncoderCombo,qOverload<int>(&QComboBox::currentIndexChanged),dialog,[dialog, update_video_rate_controls](int) {
		update_video_rate_controls();
		read_ui_into_draft();
		save_ui_settings();
	});
	QObject::connect(g_nvencBitrateSpin,qOverload<int>(&QSpinBox::valueChanged),dialog,[update_video_rate_controls](int) { update_video_rate_controls(); read_ui_into_draft(); });
	QObject::connect(g_maxBitrateSpin,qOverload<int>(&QSpinBox::valueChanged),dialog,[update_video_rate_controls](int) { update_video_rate_controls(); read_ui_into_draft(); });
	QObject::connect(g_maxFpsCombo,qOverload<int>(&QComboBox::currentIndexChanged),dialog,read_ui_into_draft);
	QObject::connect(g_videoResolutionCombo,qOverload<int>(&QComboBox::currentIndexChanged),dialog,read_ui_into_draft);
	QObject::connect(g_autostartCheck,&QCheckBox::toggled,dialog,read_ui_into_draft);
	QObject::connect(g_sendVerticalCheck,&QCheckBox::toggled,dialog,[preview, dialog, update_video_rate_controls](bool enabled) {
		if (enabled && !sender_vertical_canvas_available()) {
			blog(LOG_WARNING, "DuoCast UI: Aitum Vertical sending requested but no canvas is available");
			const QSignalBlocker blocker(g_sendVerticalCheck);
			g_sendVerticalCheck->setChecked(false);
			return;
		}
		preview->setVerticalEnabled(enabled);
		update_video_rate_controls();
		const int requiredWidth = enabled ? preview->previewContentWidth() + 48 : 900;
		dialog->setMinimumWidth(requiredWidth);
		if (enabled && dialog->width() < requiredWidth) dialog->resize(requiredWidth, dialog->height());
		read_ui_into_draft();
	});
	QObject::connect(g_fallbackCheck,&QCheckBox::toggled,dialog,read_ui_into_draft);
	QObject::connect(g_receiverBufferSpin,qOverload<int>(&QSpinBox::valueChanged),dialog,read_ui_into_draft);
	QObject::connect(g_autoRecoverLowFpsCheck,&QCheckBox::toggled,dialog,read_ui_into_draft);
	QObject::connect(g_forceAutoSwitchLiveCheck,&QCheckBox::toggled,dialog,[dialog](bool) { read_ui_into_draft(); save_ui_settings(); });
	QObject::connect(g_landscapeLiveSceneCombo,qOverload<int>(&QComboBox::currentIndexChanged),dialog,[dialog](int) { read_ui_into_draft(); save_ui_settings(); });
	QObject::connect(g_verticalLiveSceneCombo,qOverload<int>(&QComboBox::currentIndexChanged),dialog,[dialog](int) { read_ui_into_draft(); save_ui_settings(); });
	QObject::connect(g_verticalFallbackSceneCombo,qOverload<int>(&QComboBox::currentIndexChanged),dialog,[dialog](int) { read_ui_into_draft(); });
	QObject::connect(g_enableSenderCheck,&QCheckBox::toggled,dialog,[dialog](bool enabled) {
		read_ui_into_draft();
		save_ui_settings();
		const auto d = control_draft_snapshot();
		if (!enabled) {
			stop_sender();
		} else {
			config_t *cfg = profile_config();
			if (d.autostart && cfg)
				start_sender(d.destination, d.port, d.live_track, d.vod_track, d.video_quality, d.max_bitrate_mbps,
					     d.max_fps, d.send_vertical, d.resolution_mode, d.video_encoder_mode,
					     d.nvenc_bitrate_mbps);
		}
		update_duocast_window();
	});
	QObject::connect(g_enableReceiverCheck,&QCheckBox::toggled,dialog,[dialog](bool enabled) {
		read_ui_into_draft();
		save_ui_settings();
		receiver_set_enabled(enabled);
		update_duocast_window();
	});
	auto *timer = new QTimer(dialog); QObject::connect(timer,&QTimer::timeout,dialog,update_duocast_window); timer->start(500);
	update_duocast_window();
	blog(LOG_INFO, "DuoCast UI: window constructed; showing dialog");
	dialog->show();
}

obs_properties_t *control_source_properties(void *opaque)
{
	auto *props = obs_properties_create();
	obs_properties_add_text(props, "duocast_version", "OBS DuoCast 0.14.29 (Beta)", OBS_TEXT_INFO);
	const auto *state = static_cast<ControlSourceState *>(opaque);
	if (state && state->view == ToolView::Receiver) {
		auto *receiver = obs_properties_create();
		const auto peer = receiver_active_peer_address();
		const bool videoHealthy = receiver_video_feed_healthy();
		const char *receiverState = videoHealthy ? "Video active" : !peer.empty() ? "Video stalled" : "Waiting for sender";
		const std::string receiverRow = std::string(peer.empty() ? "Active sender · none" : "Active sender · ") +
			(peer.empty() ? receiverState : peer + " · " + receiverState);
		auto *receiverStatus = obs_properties_add_text(receiver, "receiver_status", receiverRow.c_str(), OBS_TEXT_INFO);
		obs_property_text_set_info_type(receiverStatus, videoHealthy ? OBS_TEXT_INFO_NORMAL :
		                                !peer.empty() ? OBS_TEXT_INFO_ERROR : OBS_TEXT_INFO_WARNING);
		auto *fallback = obs_properties_add_bool(receiver, "fallback_enabled", "Switch to a fallback scene when landscape video is lost");
		auto *forceLive = obs_properties_add_bool(receiver, "force_auto_switch_live", "Force Auto Switch to Live Scene");
		auto *syncBuffer = obs_properties_add_int(receiver, "receiver_sync_buffer_ms", "A/V sync buffer (ms)", 0, 3000, 50);
		auto *autoRecover = obs_properties_add_bool(receiver, "auto_recover_low_fps", "Automatically recover a low-FPS feed");
		auto *scene = obs_properties_add_list(receiver, "fallback_scene", "Fallback scene", OBS_COMBO_TYPE_LIST,
		                                      OBS_COMBO_FORMAT_STRING);
		auto *landscapeLive = obs_properties_add_list(receiver, "landscape_live_scene", "Landscape Live Scene", OBS_COMBO_TYPE_LIST,
		                                             OBS_COMBO_FORMAT_STRING);
		auto *verticalScene = obs_properties_add_list(receiver, "vertical_fallback_scene", "Aitum Vertical fallback scene", OBS_COMBO_TYPE_LIST,
		                                               OBS_COMBO_FORMAT_STRING);
		auto *verticalLive = obs_properties_add_list(receiver, "vertical_live_scene", "Aitum Vertical LiveScene", OBS_COMBO_TYPE_LIST,
		                                             OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(scene, "Select a scene", "");
		obs_property_list_add_string(landscapeLive, "Select a landscape live scene", "");
		obs_property_list_add_string(verticalScene, "Select an Aitum Vertical scene", "");
		obs_property_list_add_string(verticalLive, "Select an Aitum Vertical live scene", "");
		struct obs_frontend_source_list scenes{};
		obs_frontend_get_scenes(&scenes);
		for (std::size_t i = 0; i < scenes.sources.num; ++i) {
			const char *name = obs_source_get_name(scenes.sources.array[i]);
			if (name) {
				obs_property_list_add_string(scene, name, name);
				obs_property_list_add_string(landscapeLive, name, name);
			}
		}
		obs_frontend_source_list_free(&scenes);
		const auto verticalScenes = aitum_vertical_scenes();
		for (const auto &name : verticalScenes) {
			obs_property_list_add_string(verticalScene, name.c_str(), name.c_str());
			obs_property_list_add_string(verticalLive, name.c_str(), name.c_str());
		}
		obs_properties_add_group(props, "receiver", "Receiver mode", OBS_GROUP_NORMAL, receiver);
		obs_properties_add_button2(props, "save_settings", "Save settings", save_settings_clicked, const_cast<ControlSourceState *>(state));
		obs_property_set_modified_callback2(fallback, control_setting_changed, nullptr);
		obs_property_set_modified_callback2(verticalScene, control_setting_changed, nullptr);
		obs_property_set_modified_callback2(forceLive, control_setting_changed, nullptr);
		obs_property_set_modified_callback2(syncBuffer, control_setting_changed, nullptr);
		obs_property_set_modified_callback2(autoRecover, control_setting_changed, nullptr);
		obs_property_set_modified_callback2(landscapeLive, control_setting_changed, nullptr);
		obs_property_set_modified_callback2(verticalLive, control_setting_changed, nullptr);
		obs_property_set_modified_callback2(scene, control_setting_changed, nullptr);
		return props;
	}
	bool sending = false;
	{
		std::lock_guard<std::mutex> lock(g_senderMutex);
		sending = g_senderOutput && obs_output_active(g_senderOutput);
	}
	const bool connected = sending && sender_output_connected();
	const char *status = !sending ? "Connection · Stopped" : connected ? "Connection · Connected" : "Connection · Waiting · auto-reconnect enabled";
	const auto draft = control_draft_snapshot();
	auto *sender = obs_properties_create();
	auto *connection = obs_properties_add_text(sender, "sender_status", status, OBS_TEXT_INFO);
	obs_property_text_set_info_type(connection, !sending ? OBS_TEXT_INFO_WARNING : connected ? OBS_TEXT_INFO_NORMAL : OBS_TEXT_INFO_WARNING);
	char receiver_status[320];
	snprintf(receiver_status, sizeof(receiver_status), "Receiver · %s:%d · %s", draft.destination.c_str(), draft.port,
	         connected ? "Active" : sending ? "Reconnecting" : "Not connected");
	obs_properties_add_text(sender, "active_receiver", receiver_status, OBS_TEXT_INFO);
	char videoStats[128], liveStats[128], vodStats[128];
	snprintf(videoStats, sizeof(videoStats), "Video · %llu fps · %.2f Mbps",
	         static_cast<unsigned long long>(g_videoRate.load()), g_videoKbps.load() / 1000.0);
	snprintf(liveStats, sizeof(liveStats), "Live Mix Audio · %llu Kbps · %llu packets/s",
	         static_cast<unsigned long long>(g_liveAudioKbps.load()), static_cast<unsigned long long>(g_liveAudioRate.load()));
	snprintf(vodStats, sizeof(vodStats), "VOD Mix Audio · %llu Kbps · %llu packets/s",
	         static_cast<unsigned long long>(g_vodAudioKbps.load()), static_cast<unsigned long long>(g_vodAudioRate.load()));
	obs_properties_add_text(sender, "video_stats", videoStats, OBS_TEXT_INFO);
	obs_properties_add_text(sender, "live_stats", liveStats, OBS_TEXT_INFO);
	obs_properties_add_text(sender, "vod_stats", vodStats, OBS_TEXT_INFO);
	if (g_settingsSavedAt && os_gettime_ns() / 1000000ULL - g_settingsSavedAt < 2500)
		obs_properties_add_text(props, "settings_saved", g_settingsSaveOk ? "Settings saved" : "Could not save settings", OBS_TEXT_INFO);
	auto *destination = obs_properties_add_text(sender, "destination", "Receiver address", OBS_TEXT_DEFAULT);
	auto *port = obs_properties_add_int(sender, "port", "Receiver port", 1, 65535, 1);
	auto *live = obs_properties_add_int(sender, "live_track", "Live mix track", 1, 6, 1);
	auto *vod = obs_properties_add_int(sender, "vod_track", "VOD mix track", 1, 6, 1);
	auto *resolution = obs_properties_add_list(sender, "resolution_mode", "Video resolution",
						   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(resolution, "OBS canvas resolution", 0);
	obs_property_list_add_int(resolution, "OBS output resolution", 1);
	auto *videoEncoder = obs_properties_add_list(sender, "video_encoder_mode", "Video encoder",
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(videoEncoder, "MJPEG (TurboJPEG)", 0);
	obs_property_list_add_int(videoEncoder, "NVENC H.264 (GPU texture)", 1);
	obs_property_list_add_int(videoEncoder, "NVENC HEVC / H.265 (GPU texture)", 2);
	auto *nvencBitrate = obs_properties_add_int(sender, "nvenc_bitrate_mbps", "NVENC video bitrate per feed (Mbps)", 1, 1000, 1);
	obs_property_set_long_description(nvencBitrate,
		"CBR video target for each NVENC feed. With portrait enabled, two encoders use this target. The total network cap independently paces all video and audio traffic.");
	auto *quality = obs_properties_add_int_slider(sender, "video_quality", "Video quality (%)", 1, 100, 1);
	obs_property_set_long_description(quality,
		"MJPEG mode only: fixed JPEG quality for every frame. Higher values preserve more detail and use more bandwidth.");
	auto *bitrateCap = obs_properties_add_int(sender, "max_bitrate_mbps", "Total network cap (Mbps; 0 = no cap)", 0, 1000, 1);
	obs_property_set_long_description(bitrateCap,
		"A shared pacing limit for landscape video, optional portrait video, and both audio tracks. Set to 0 for no pacing. If lower than the combined NVENC video targets, it will throttle frames.");
	auto *fpsCap = obs_properties_add_int(sender, "max_fps", "Max video FPS (0 = canvas FPS)", 0, 120, 1);
	obs_property_set_long_description(fpsCap,
		"Applies to both outgoing video feeds. Frames above the selected rate are skipped before encoding; NVENC uses OBS frame-rate pacing.");
	auto *autostart = obs_properties_add_bool(sender, "autostart", "Start sending automatically when OBS opens");
	auto *vertical = obs_properties_add_bool(sender, "send_vertical", "Send Aitum Vertical canvas");
	obs_property_set_enabled(vertical, sender_vertical_canvas_available());
	obs_property_set_long_description(vertical, "Requires Aitum Vertical with an active canvas. This adds a second encoded video feed and uses additional encoder and network resources.");
	auto *start = obs_properties_add_button2(sender, "start_sender", "Start sender", start_sender_clicked, nullptr);
	auto *stop = obs_properties_add_button2(sender, "stop_sender", "Stop sender", stop_sender_clicked, nullptr);
	obs_property_set_visible(start, !sending);
	obs_property_set_visible(stop, sending);
	obs_properties_add_group(props, "sender", "Sender mode", OBS_GROUP_NORMAL, sender);

	obs_properties_add_button2(props, "save_settings", "Save Settings", save_settings_clicked, const_cast<ControlSourceState *>(state));

	for (auto *property : {destination, port, live, vod, resolution, videoEncoder, nvencBitrate, quality, bitrateCap, fpsCap, autostart, vertical})
		obs_property_set_modified_callback2(property, control_setting_changed, nullptr);
	return props;
}

void register_control_source()
{
	static obs_source_info info{};
	info.id = "obs_duocast_control";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_DEPRECATED | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.output_flags |= OBS_SOURCE_VIDEO;
	info.get_name = control_source_name;
	info.create = control_source_create;
	info.destroy = control_source_destroy;
	info.update = control_source_update;
	info.get_defaults = control_source_defaults;
	info.get_properties = control_source_properties;
	info.video_render = control_source_video_render;
	info.get_width = control_source_width;
	info.get_height = control_source_height;
	obs_register_source(&info);
}

void control_source_defaults(obs_data_t *settings)
{
	config_t *cfg = profile_config();
	obs_data_set_default_string(settings, "destination", cfg ? config_get_string(cfg, kConfigSection, kDestinationKey) : "192.168.50.102");
	obs_data_set_default_int(settings, "port", cfg ? config_get_int(cfg, kConfigSection, kPortKey) : 17777);
	obs_data_set_default_int(settings, "live_track", cfg ? config_get_int(cfg, kConfigSection, kLiveTrackKey) : 1);
	obs_data_set_default_int(settings, "vod_track", cfg ? config_get_int(cfg, kConfigSection, kVodTrackKey) : 2);
	obs_data_set_default_int(settings, "video_quality", cfg ? config_get_int(cfg, kConfigSection, kVideoQualityKey) : 85);
	obs_data_set_default_int(settings, "video_encoder_mode", cfg ? config_get_int(cfg, kConfigSection, kVideoEncoderModeKey) : 0);
	obs_data_set_default_int(settings, "nvenc_bitrate_mbps", cfg ? config_get_int(cfg, kConfigSection, kNvencBitrateKey) : 80);
	obs_data_set_default_int(settings, "max_bitrate_mbps", cfg ? config_get_int(cfg, kConfigSection, kMaxBitrateKey) : 100);
	obs_data_set_default_int(settings, "max_fps", cfg ? config_get_int(cfg, kConfigSection, kMaxFpsKey) : 0);
	obs_data_set_default_int(settings, "resolution_mode", cfg ? config_get_int(cfg, kConfigSection, kVideoResolutionKey) : 0);
	obs_data_set_default_int(settings, "receiver_sync_buffer_ms", cfg ? config_get_int(cfg, kConfigSection, kReceiverSyncBufferKey) : 1000);
	obs_data_set_default_bool(settings, "auto_recover_low_fps", !cfg || config_get_bool(cfg, kConfigSection, kAutoRecoverLowFpsKey));
	obs_data_set_default_bool(settings, "send_vertical", cfg && config_get_bool(cfg, kConfigSection, kSendVerticalKey));
	obs_data_set_default_bool(settings, "enable_sender", cfg && config_get_bool(cfg, kConfigSection, kEnableSenderKey));
	obs_data_set_default_bool(settings, "enable_receiver", cfg && config_get_bool(cfg, kConfigSection, kEnableReceiverKey));
	obs_data_set_default_bool(settings, "autostart", !cfg || config_get_bool(cfg, kConfigSection, kAutostartKey));
	obs_data_set_default_bool(settings, "fallback_enabled", !cfg || config_get_bool(cfg, kConfigSection, kFallbackEnabledKey));
	obs_data_set_default_string(settings, "fallback_scene", cfg ? config_get_string(cfg, kConfigSection, kFallbackSceneKey) : "");
	obs_data_set_default_string(settings, "vertical_fallback_scene", cfg ? config_get_string(cfg, kConfigSection, kVerticalFallbackSceneKey) : "");
	obs_data_set_default_string(settings, "landscape_live_scene", cfg ? config_get_string(cfg, kConfigSection, kLandscapeLiveSceneKey) : "");
	obs_data_set_default_string(settings, "vertical_live_scene", cfg ? config_get_string(cfg, kConfigSection, kVerticalLiveSceneKey) : "");
	obs_data_set_default_bool(settings, "force_auto_switch_live", cfg && config_get_bool(cfg, kConfigSection, kForceAutoSwitchLiveKey));
	obs_data_set_default_string(settings, "duocast_version", "OBS DuoCast 0.14.29 (Beta)");
}

std::string current_scene_name()
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene) return {};
	const char *name = obs_source_get_name(scene);
	const std::string result = name ? name : "";
	obs_source_release(scene);
	return result;
}

bool activate_scene(const std::string &name)
{
	if (name.empty()) return false;
	struct obs_frontend_source_list scenes{};
	obs_frontend_get_scenes(&scenes);
	bool found = false;
	for (std::size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *scene = scenes.sources.array[i];
		const char *scene_name = obs_source_get_name(scene);
		if (scene_name && name == scene_name) {
			obs_frontend_set_current_scene(scene);
			found = true;
			break;
		}
	}
	obs_frontend_source_list_free(&scenes);
	return found;
}

void update_scene_fallback(bool failed, const std::string &target, bool vertical,
			   std::string &active_fallback, std::string &scene_before, std::uint64_t &last_request_ns)
{
	auto current = [&] { return vertical ? aitum_vertical_current_scene() : current_scene_name(); };
	auto activate = [&](const std::string &name) { return vertical ? aitum_vertical_activate_scene(name) : activate_scene(name); };
	if (!failed || target.empty()) {
		if (!active_fallback.empty() && !scene_before.empty() && current() == active_fallback)
			activate(scene_before);
		active_fallback.clear();
		scene_before.clear();
		last_request_ns = 0;
		return;
	}
	const auto current_scene = current();
	if (!active_fallback.empty()) {
		if (current_scene == active_fallback) {
			// It is already on the requested fallback. Reissuing this every
			// monitor tick causes repeated scene-change events (and transitions).
			if (target == active_fallback) return;
			if (activate(target)) {
				active_fallback = target;
				last_request_ns = os_gettime_ns();
			}
			return;
		}
		active_fallback.clear();
		scene_before.clear();
	}
	if (target == current_scene) return;
	const auto now = os_gettime_ns();
	if (last_request_ns && now >= last_request_ns && now - last_request_ns < 2000000000ULL)
		return;
	scene_before = current_scene;
	// Aitum can report an empty current scene while its canvas is initializing.
	// The previous-scene snapshot is only needed for restoration; it must not
	// prevent the configured fallback itself from being activated.
	if (scene_before != target && activate(target)) {
		active_fallback = target;
		last_request_ns = now;
	}
}

void enforce_live_scene(bool active, const std::string &target, bool vertical,
			std::string &last_target, std::uint64_t &last_request_ns)
{
	if (!active || target.empty()) {
		last_target.clear();
		last_request_ns = 0;
		return;
	}
	const auto current = vertical ? aitum_vertical_current_scene() : current_scene_name();
	if (current == target) {
		last_target.clear();
		last_request_ns = 0;
		return;
	}
	const auto now = os_gettime_ns();
	if (last_target == target && last_request_ns && now >= last_request_ns && now - last_request_ns < 750000000ULL)
		return;
	const bool queued = vertical ? aitum_vertical_activate_scene(target) : activate_scene(target);
	if (queued) {
		last_target = target;
		last_request_ns = now;
	}
}

void monitor_receiver_fallback()
{
	const auto draft = control_draft_snapshot();
	const bool receiverEnabled = draft.enable_receiver;
	const bool connected = receiverEnabled && receiver_video_connection_seen();
	const bool landscapeHealthy = receiverEnabled && receiver_landscape_feed_healthy();
	const bool verticalHealthy = receiverEnabled && receiver_vertical_feed_configured() && receiver_vertical_feed_healthy();
	const bool landscapeFailed = connected && !landscapeHealthy;
	const bool verticalFailed = connected && receiver_vertical_feed_configured() && !receiver_vertical_feed_healthy();
	update_scene_fallback(receiverEnabled && draft.fallback_enabled && landscapeFailed,
			      draft.fallback_scene, false, g_activeFallbackScene, g_sceneBeforeFallback, g_lastFallbackRequestNs);
	update_scene_fallback(verticalFailed, draft.vertical_fallback_scene, true,
			      g_activeVerticalFallbackScene, g_sceneBeforeVerticalFallback, g_lastVerticalFallbackRequestNs);
	const bool forceLive = receiverEnabled && draft.force_auto_switch_live;
	enforce_live_scene(forceLive && landscapeHealthy, draft.landscape_live_scene, false,
			   g_lastLandscapeLiveRequest, g_lastLandscapeLiveRequestNs);
	enforce_live_scene(forceLive && verticalHealthy, draft.vertical_live_scene, true,
			   g_lastVerticalLiveRequest, g_lastVerticalLiveRequestNs);
}

bool start_sender(const std::string &host, int port, int live_track, int vod_track, int video_quality,
		  int max_bitrate_mbps, int max_fps, bool send_vertical, int resolution_mode,
		  int video_encoder_mode, int nvenc_bitrate_mbps)
{
	if (!control_draft_snapshot().enable_sender) {
		blog(LOG_WARNING, "DuoCast: sender start blocked because DuoCast is disabled");
		return false;
	}
	std::lock_guard<std::mutex> lock(g_senderMutex);
	if (send_vertical && !sender_vertical_canvas_available()) {
		blog(LOG_WARNING, "DuoCast: Aitum Vertical canvas is unavailable; starting landscape feed only");
		send_vertical = false;
	}
	if (g_senderOutput && obs_output_active(g_senderOutput)) return true;
	sender_remote_stats_clear();
	if (g_senderOutput) {
		if (obs_output_active(g_senderOutput)) obs_output_stop(g_senderOutput);
		obs_output_release(g_senderOutput);
		g_senderOutput = nullptr;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "destination", host.c_str());
	obs_data_set_int(settings, "port", port);
	obs_data_set_int(settings, "live_mix", live_track);
	obs_data_set_int(settings, "vod_mix", vod_track);
	obs_data_set_int(settings, "video_quality", video_quality);
	obs_data_set_int(settings, "video_encoder_mode", std::clamp(video_encoder_mode, 0, 2));
	obs_data_set_int(settings, "nvenc_bitrate_mbps", std::clamp(nvenc_bitrate_mbps, 1, 1000));
	obs_data_set_int(settings, "max_bitrate_mbps", std::clamp(max_bitrate_mbps, 0, 1000));
	obs_data_set_int(settings, "max_fps", std::clamp(max_fps, 0, 120));
	obs_data_set_bool(settings, "send_vertical", send_vertical);
	obs_data_set_int(settings, "resolution_mode", std::clamp(resolution_mode, 0, 1));
	g_senderOutput = obs_output_create("obs_duocast_sender", "OBS DuoCast Sender", settings, nullptr);
	obs_data_release(settings);
	if (!g_senderOutput || !obs_output_start(g_senderOutput)) {
		if (g_senderOutput) {
			obs_output_release(g_senderOutput);
			g_senderOutput = nullptr;
		}
		return false;
	}
	if (!persist_draft(true))
		blog(LOG_ERROR, "DuoCast: sender started, but its autostart settings could not be saved");
	return true;
}

bool start_sender_from_saved_preferences()
{
	config_t *cfg = profile_config();
	if (!cfg) {
		blog(LOG_WARNING, "DuoCast: sender autostart skipped because its settings file is unavailable");
		return false;
	}
	if (!config_get_bool(cfg, kConfigSection, kEnableSenderKey)) {
		blog(LOG_INFO, "DuoCast: sender autostart skipped because Enable DuoCast Sender is off");
		return false;
	}
	if (!config_get_bool(cfg, kConfigSection, kAutostartKey)) {
		blog(LOG_INFO, "DuoCast: sender autostart skipped because Start sending when OBS opens is off");
		return false;
	}
	const char *host_value = config_get_string(cfg, kConfigSection, kDestinationKey);
	const std::string host = host_value ? host_value : "192.168.50.102";
	const int port = static_cast<int>(config_get_int(cfg, kConfigSection, kPortKey));
	const int live = static_cast<int>(config_get_int(cfg, kConfigSection, kLiveTrackKey));
	const int vod = static_cast<int>(config_get_int(cfg, kConfigSection, kVodTrackKey));
	const int quality = static_cast<int>(config_get_int(cfg, kConfigSection, kVideoQualityKey));
	const int video_encoder_mode = std::clamp(static_cast<int>(config_get_int(cfg, kConfigSection, kVideoEncoderModeKey)), 0, 2);
	const int nvenc_bitrate_mbps = std::clamp(static_cast<int>(config_get_int(cfg, kConfigSection, kNvencBitrateKey)), 1, 1000);
	const int max_bitrate = static_cast<int>(config_get_int(cfg, kConfigSection, kMaxBitrateKey));
	const int max_fps = static_cast<int>(config_get_int(cfg, kConfigSection, kMaxFpsKey));
	const bool send_vertical = config_get_bool(cfg, kConfigSection, kSendVerticalKey);
	const int resolution_mode = std::clamp(static_cast<int>(config_get_int(cfg, kConfigSection, kVideoResolutionKey)), 0, 1);
	const bool started = start_sender(host, std::clamp(port, 1, 65535), std::clamp(live, 1, 6),
					  std::clamp(vod, 1, 6), std::clamp(quality, 1, 100),
					  std::clamp(max_bitrate, 0, 1000), std::clamp(max_fps, 0, 120), send_vertical,
					  resolution_mode, video_encoder_mode, nvenc_bitrate_mbps);
	blog(started ? LOG_INFO : LOG_ERROR, "DuoCast: saved sender autostart %s", started ? "started" : "failed");
	return started;
}

void stop_sender()
{
	std::lock_guard<std::mutex> lock(g_senderMutex);
	if (!g_senderOutput) return;
	if (obs_output_active(g_senderOutput)) obs_output_stop(g_senderOutput);
	obs_output_release(g_senderOutput);
	g_senderOutput = nullptr;
}

void show_sender_tool(void *)
{
 if (g_senderControlSource) obs_frontend_open_source_properties(g_senderControlSource);
}

void show_receiver_tool(void *)
{
 if (g_receiverControlSource) obs_frontend_open_source_properties(g_receiverControlSource);
}

void duocast_tick(void *, float)
{
 const std::uint64_t now = os_gettime_ns();
 const std::uint64_t previous = g_lastRefreshAt.load();
 if (now - previous < 500000000ULL) return;
 g_lastRefreshAt.store(now);
 monitor_receiver_fallback();
	{
		std::lock_guard<std::mutex> lock(g_senderMutex);
		if (g_senderOutput && obs_output_active(g_senderOutput) && sender_output_connected()) {
			const SenderMetrics metrics = sender_output_metrics();
  if (g_haveMetricsBaseline && now > g_lastMetricsAt) {
   const auto elapsed = now - g_lastMetricsAt;
   const auto rate = [elapsed](std::uint64_t current, std::uint64_t before) {
    return (current - before) * 1000000000ULL / elapsed;
   };
   g_videoRate.store(rate(metrics.video_packets, g_lastMetrics.video_packets));
   g_videoKbps.store(rate(metrics.video_bytes, g_lastMetrics.video_bytes) * 8ULL / 1000ULL);
   g_liveAudioKbps.store(rate(metrics.live_audio_bytes, g_lastMetrics.live_audio_bytes) * 8ULL / 1000ULL);
   g_vodAudioKbps.store(rate(metrics.vod_audio_bytes, g_lastMetrics.vod_audio_bytes) * 8ULL / 1000ULL);
   g_verticalVideoRate.store(rate(metrics.vertical_video_packets, g_lastMetrics.vertical_video_packets));
   g_verticalVideoKbps.store(rate(metrics.vertical_video_bytes, g_lastMetrics.vertical_video_bytes) * 8ULL / 1000ULL);
  }
  g_lastMetrics = metrics;
  g_lastMetricsAt = now;
  g_haveMetricsBaseline = true;
		} else {
			g_haveMetricsBaseline = false;
			g_videoRate.store(0); g_liveAudioRate.store(0); g_vodAudioRate.store(0);
			g_videoKbps.store(0); g_liveAudioKbps.store(0); g_vodAudioKbps.store(0);
   g_verticalVideoKbps.store(0);
   g_verticalVideoRate.store(0);
		}
 }
	const auto lastEdit = g_lastControlEditAt.load();
	const auto nowMs = now / 1000000ULL;
	if (lastEdit == 0 || nowMs - lastEdit >= 2000) {
		if (g_senderControlSource) obs_source_update_properties(g_senderControlSource);
		if (g_receiverControlSource) obs_source_update_properties(g_receiverControlSource);
	}
}

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		blog(LOG_INFO, "DuoCast: stopping sender before OBS shutdown");
		stop_sender();
		blog(LOG_INFO, "DuoCast: sender stopped before OBS audio teardown");
		if (g_frontendCallbackRegistered) {
			obs_frontend_remove_event_callback(frontend_event, nullptr);
			g_frontendCallbackRegistered = false;
		}
		return;
	}
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) return;
	start_sender_from_saved_preferences();
}
} // namespace

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("OBS-DuoCast", "en-US")

bool obs_module_load(void)
{
	char *config_dir = obs_module_config_path(nullptr);
	if (config_dir) {
		os_mkdirs(config_dir);
		bfree(config_dir);
	}
	char *config_path = obs_module_config_path("duocast.ini");
	if (config_path) {
		if (config_open(&g_pluginConfig, config_path, CONFIG_OPEN_ALWAYS) != 0) g_pluginConfig = nullptr;
		bfree(config_path);
	}
	if (config_t *cfg = profile_config()) {
		if (config_t *profile = obs_frontend_get_profile_config()) {
			const char *string_keys[] = {kDestinationKey, kFallbackSceneKey, kVerticalFallbackSceneKey,
					     kLandscapeLiveSceneKey, kVerticalLiveSceneKey};
			for (const char *key : string_keys)
				if (!config_has_user_value(cfg, kConfigSection, key) && config_has_user_value(profile, kConfigSection, key))
					config_set_string(cfg, kConfigSection, key, config_get_string(profile, kConfigSection, key));
			const char *int_keys[] = {kPortKey, kLiveTrackKey, kVodTrackKey, kVideoQualityKey,
						  kMaxBitrateKey, kMaxFpsKey, kVideoResolutionKey, kReceiverSyncBufferKey};
			for (const char *key : int_keys)
				if (!config_has_user_value(cfg, kConfigSection, key) && config_has_user_value(profile, kConfigSection, key))
					config_set_int(cfg, kConfigSection, key, config_get_int(profile, kConfigSection, key));
			const char *bool_keys[] = {kAutostartKey, kSenderConfiguredKey, kSendVerticalKey, kEnableDuoCastKey, kAutoRecoverLowFpsKey,
					    kForceAutoSwitchLiveKey};
			for (const char *key : bool_keys)
				if (!config_has_user_value(cfg, kConfigSection, key) && config_has_user_value(profile, kConfigSection, key))
					config_set_bool(cfg, kConfigSection, key, config_get_bool(profile, kConfigSection, key));
		}
		const bool legacyEnabled = config_has_user_value(cfg, kConfigSection, kEnableDuoCastKey) &&
			config_get_bool(cfg, kConfigSection, kEnableDuoCastKey);
		if (!config_has_user_value(cfg, kConfigSection, kEnableSenderKey))
			config_set_bool(cfg, kConfigSection, kEnableSenderKey, legacyEnabled);
		if (!config_has_user_value(cfg, kConfigSection, kEnableReceiverKey))
			config_set_bool(cfg, kConfigSection, kEnableReceiverKey, legacyEnabled);
		config_set_default_string(cfg, kConfigSection, kDestinationKey, "192.168.50.102");
		config_set_default_int(cfg, kConfigSection, kPortKey, 17777);
		config_set_default_int(cfg, kConfigSection, kLiveTrackKey, 1);
		config_set_default_int(cfg, kConfigSection, kVodTrackKey, 2);
		// Convert the previous 2-31 encoder quality value to the matching slider
		// percentage once. New values are TurboJPEG quality, directly 1-100.
		if (config_get_int(cfg, kConfigSection, "VideoTransportVersion") < 2) {
			const int old_quality = static_cast<int>(config_get_int(cfg, kConfigSection, kVideoQualityKey));
			if (old_quality >= 2 && old_quality <= 31)
				config_set_int(cfg, kConfigSection, kVideoQualityKey,
					1 + static_cast<int>(std::lround((31 - old_quality) * 99.0 / 29.0)));
			else config_set_int(cfg, kConfigSection, kVideoQualityKey, 85);
			config_set_int(cfg, kConfigSection, "VideoTransportVersion", 2);
		}
		config_set_default_int(cfg, kConfigSection, kVideoQualityKey, 85);
		config_set_default_int(cfg, kConfigSection, kMaxBitrateKey, 100);
		config_set_default_int(cfg, kConfigSection, kMaxFpsKey, 0);
		config_set_default_int(cfg, kConfigSection, kVideoResolutionKey, 0);
		config_set_default_int(cfg, kConfigSection, kReceiverSyncBufferKey, 1000);
		config_set_default_bool(cfg, kConfigSection, kAutoRecoverLowFpsKey, true);
		// 0.14.7 diagnostic builds left vertical sending disabled by default.
		// Restore it once for existing installs; users can still turn it off later.
		constexpr char kVerticalSendRestoreVersion[] = "VerticalSendRestoreVersion";
		if (config_get_int(cfg, kConfigSection, kVerticalSendRestoreVersion) < 1) {
			config_set_bool(cfg, kConfigSection, kSendVerticalKey, true);
			config_set_int(cfg, kConfigSection, kVerticalSendRestoreVersion, 1);
		}
		config_set_default_bool(cfg, kConfigSection, kSendVerticalKey, true);
	config_set_default_string(cfg, kConfigSection, kFallbackSceneKey, "");
		config_set_default_bool(cfg, kConfigSection, kAutostartKey, true);
		config_set_default_bool(cfg, kConfigSection, kSenderConfiguredKey, false);
		config_set_default_bool(cfg, kConfigSection, kFallbackEnabledKey, true);
		config_set_default_bool(cfg, kConfigSection, kForceAutoSwitchLiveKey, false);
		config_set_default_string(cfg, kConfigSection, kLandscapeLiveSceneKey, "");
		config_set_default_string(cfg, kConfigSection, kVerticalLiveSceneKey, "");
		config_set_default_bool(cfg, kConfigSection, kEnableDuoCastKey, false);
		config_set_default_bool(cfg, kConfigSection, kEnableSenderKey, false);
		config_set_default_bool(cfg, kConfigSection, kEnableReceiverKey, false);
		config_save_safe(cfg, "tmp", "bak");
	}
	g_cpuUsageInfo = os_cpu_usage_info_start();
	receiver_set_sync_buffer_ms(g_pluginConfig ? static_cast<int>(config_get_int(g_pluginConfig, kConfigSection, kReceiverSyncBufferKey)) : 1000);
	receiver_set_auto_recovery_enabled(!g_pluginConfig || config_get_bool(g_pluginConfig, kConfigSection, kAutoRecoverLowFpsKey));
	receiver_set_enabled(g_pluginConfig && config_get_bool(g_pluginConfig, kConfigSection, kEnableReceiverKey));
	register_receiver_source();
	register_sender_output();
	register_control_source();
	obs_data_t *control_settings = obs_data_create();
	control_source_defaults(control_settings);
	obs_data_set_string(control_settings, "tool_view", "sender");
	g_senderControlSource = obs_source_create_private("obs_duocast_control", "DuoCast Sender", control_settings);
	obs_data_release(control_settings);
	control_settings = obs_data_create();
	control_source_defaults(control_settings);
	obs_data_set_string(control_settings, "tool_view", "receiver");
	g_receiverControlSource = obs_source_create_private("obs_duocast_control", "DuoCast Receiver", control_settings);
	obs_data_release(control_settings);
	obs_frontend_add_event_callback(frontend_event, nullptr);
	obs_add_tick_callback(duocast_tick, nullptr);
	g_frontendCallbackRegistered = true;
	obs_frontend_add_tools_menu_item("DuoCast", show_duocast_tool, nullptr);
	create_sender_dock();
	create_receiver_stats_dock();
	blog(LOG_INFO, "OBS DuoCast %s (Beta) loaded", "0.14.29");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "DuoCast: module unload: stopping callbacks and sender");
	obs_remove_tick_callback(duocast_tick, nullptr);
	if (g_frontendCallbackRegistered) {
		obs_frontend_remove_event_callback(frontend_event, nullptr);
		g_frontendCallbackRegistered = false;
	}
	stop_sender();
	blog(LOG_INFO, "DuoCast: module unload: sender stopped; disabling receiver");
	receiver_set_enabled(false);
	blog(LOG_INFO, "DuoCast: module unload: removing sender dock");
	if (g_senderDockRegistered) obs_frontend_remove_dock("obs-duocast-sender-controls");
	g_senderDockRegistered = false;
	g_senderDockContent = nullptr;
	if (g_receiverStatsDockRegistered) obs_frontend_remove_dock("obs-duocast-receiver-stats");
	g_receiverStatsDockRegistered = false;
	g_receiverStatsDockContent = nullptr;
	blog(LOG_INFO, "DuoCast: module unload: docks removed");
	if (g_cpuUsageInfo) {
		os_cpu_usage_info_destroy(g_cpuUsageInfo);
		g_cpuUsageInfo = nullptr;
	}
	g_dockStartButton = nullptr;
	g_dockStopButton = nullptr;
	// Keep the tools dialog alive when users close its window (QDialog hides by
	// default). Destroy it here while libobs graphics and the Qt application are
	// still alive, so its OBS display and draw callback are torn down in-order.
	// Its QPointer may already be null if OBS's main-window teardown destroyed
	// the parent-owned dialog before module unload.
	if (g_duocastDialog) {
		blog(LOG_INFO, "DuoCast: module unload: destroying tools dialog");
		delete g_duocastDialog.data();
		g_duocastDialog = nullptr;
		g_senderStatusLabel = nullptr;
		g_receiverStatusLabel = nullptr;
		g_receiverPeerLabel = nullptr;
		g_peerLabel = nullptr;
		g_videoFpsLabel = nullptr;
		g_videoRateLabel = nullptr;
		g_liveRateLabel = nullptr;
		g_vodRateLabel = nullptr;
		g_verticalRateLabel = nullptr;
		g_qualitySlider = nullptr;
		g_qualityPercentLabel = nullptr;
		g_maxBitrateSpin = nullptr;
		g_maxFpsCombo = nullptr;
		g_sendVerticalCheck = nullptr;
		g_enableSenderCheck = nullptr;
		g_enableReceiverCheck = nullptr;
		g_fallbackCheck = nullptr;
		g_fallbackSceneCombo = nullptr;
		g_verticalFallbackSceneCombo = nullptr;
		g_forceAutoSwitchLiveCheck = nullptr;
		g_landscapeLiveSceneCombo = nullptr;
		g_verticalLiveSceneCombo = nullptr;
		blog(LOG_INFO, "DuoCast: module unload: tools dialog destroyed");
	} else {
		blog(LOG_INFO, "DuoCast: module unload: tools dialog already destroyed by Qt");
	}
	if (g_senderControlSource) { obs_source_release(g_senderControlSource); g_senderControlSource = nullptr; }
	if (g_receiverControlSource) { obs_source_release(g_receiverControlSource); g_receiverControlSource = nullptr; }
	if (g_pluginConfig) { config_close(g_pluginConfig); g_pluginConfig = nullptr; }
	blog(LOG_INFO, "OBS DuoCast unloaded");
}


