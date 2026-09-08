/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"
#include "ui-config.h"

#include "ColorSelect.hpp"
#include "OBSBasicControls.hpp"
#include "OBSBasicStats.hpp"
#include "plugin-manager/PluginManager.hpp"

#ifdef __ANDROID__
#include "OBSAndroidDisplay.hpp" // 3-4 校形探针日志（logcat tag OBS-display）
#include "OBSAndroidKeepAlive.hpp"
#include "OBSAndroidPermissions.hpp"
#include <QListView> // 3-4 命中区：列表行高
#include <QToolBar> // 3-4 命中区：场景/源两排工具按钮
#include <QToolButton>
#endif

#include <obs-module.h>

#ifdef YOUTUBE_ENABLED
#include <docks/YouTubeAppDock.hpp>
#endif
#include <dialogs/NameDialog.hpp>
#include <dialogs/OBSAbout.hpp>
#include <dialogs/OBSBasicAdvAudio.hpp>
#include <dialogs/OBSBasicFilters.hpp>
#include <dialogs/OBSBasicInteraction.hpp>
#include <dialogs/OBSBasicProperties.hpp>
#include <dialogs/OBSBasicTransform.hpp>
#include <models/SceneCollection.hpp>
#include <settings/OBSBasicSettings.hpp>
#include <utility/QuickTransition.hpp>
#include <utility/SceneRenameDelegate.hpp>
#if defined(_WIN32) || defined(WHATSNEW_ENABLED)
#include <utility/WhatsNewInfoThread.hpp>
#endif
#include <widgets/AudioMixer.hpp>
#include <widgets/OBSProjector.hpp>

#include <OBSStudioAPI.hpp>
#ifdef BROWSER_AVAILABLE
#include <browser-panel.hpp>
#endif
#ifdef ENABLE_WAYLAND
#include <obs-nix-platform.h>
#endif
#include <qt-wrappers.hpp>

#include <QActionGroup>
#include <QCoreApplication>
#include <QThread>
#include <QWidgetAction>

#include <mutex>
#ifdef _WIN32
#include <sstream>
#endif
#include <string>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#endif

#include "moc_OBSBasic.cpp"

using namespace std;

extern bool portable_mode;
extern bool disable_3p_plugins;
extern bool opt_studio_mode;
extern bool opt_always_on_top;
extern bool opt_minimize_tray;
extern std::string opt_starting_profile;
extern std::string opt_starting_collection;

extern bool safe_mode;
extern bool opt_start_recording;
extern bool opt_start_replaybuffer;
extern bool opt_start_virtualcam;

extern volatile long insideEventLoop;
extern bool restart;

extern bool EncoderAvailable(const char *encoder);

extern void RegisterTwitchAuth();
extern void RegisterRestreamAuth();
#ifdef YOUTUBE_ENABLED
extern void RegisterYoutubeAuth();
#endif

struct QCef;

extern QCef *cef;
extern bool cef_js_avail;

extern void DestroyPanelCookieManager();
extern void CheckExistingCookieId();

namespace {
std::once_flag saveOnceFlag;
}

static void AddExtraModulePaths()
{
	string plugins_path, plugins_data_path;
	char *s;

	s = getenv("OBS_PLUGINS_PATH");
	if (s) {
		plugins_path = s;
	}

	s = getenv("OBS_PLUGINS_DATA_PATH");
	if (s) {
		plugins_data_path = s;
	}

	if (!plugins_path.empty() && !plugins_data_path.empty()) {
#if defined(__APPLE__)
		plugins_path += "/%module%.plugin/Contents/MacOS";
		plugins_data_path += "/%module%.plugin/Contents/Resources";
		obs_add_module_path(plugins_path.c_str(), plugins_data_path.c_str());
#else
		string data_path_with_module_suffix;
		data_path_with_module_suffix += plugins_data_path;
		data_path_with_module_suffix += "/%module%";
		obs_add_module_path(plugins_path.c_str(), data_path_with_module_suffix.c_str());
#endif
	}

#ifdef __ANDROID__
	/* B-6：Android 上插件 .so 由 androiddeployqt 打进 APK 的 lib/<abi>/，安装后解到
	 * nativeLibraryDir（= applicationDirPath()）。W^X 只准从那里 dlopen，故 bin 指过去。
	 * 本平台 get_plugin_info 恒 true，扫描会把同目录里的 Qt/libobs 等 .so 也试开一遍，
	 * 它们没有 obs_module_load，obs_open_module 在登记成模块之前就返回、只打一行警告，不崩。
	 *
	 * P-1：data 不能跟着 bin 走同一个目录 —— PackageManager 只把 lib/<abi>/ 里以 .so
	 * 结尾的文件解出来，插件的 .effect/locale 压根不在 nativeLibraryDir 里（旧写法
	 * "<appdir>/%module%/data" 永远 stat 不到，obs_module_file() 恒返回 NULL，于是
	 * obs-transitions 的 fade_transition.effect 建不出转场源）。数据改走"APK 里的 Qt 资源
	 * → 启动时解到应用私有目录"那条路（见 AndroidRuntimeBootstrap.cpp 与
	 * frontend/cmake/os-android.cmake 的 :/obsplugindata），路径布局与
	 * libobs/obs-android.c 顶部注释、add_default_module_paths() 保持一致。 */
	{
		QByteArray bin = QCoreApplication::applicationDirPath().toUtf8();
		QByteArray data = qgetenv("OBS_ROOT_PATH") + "/share/obs/obs-plugins/%module%";
		if (!bin.isEmpty())
			obs_add_module_path(bin.constData(), data.constData());
	}
#else
	if (portable_mode) {
		return;
	}

	char base_module_dir[512];
#if defined(_WIN32)
	int ret = GetProgramDataPath(base_module_dir, sizeof(base_module_dir), "obs-studio/plugins/%module%");
#elif defined(__APPLE__)
	int ret = GetAppConfigPath(base_module_dir, sizeof(base_module_dir), "obs-studio/plugins/%module%.plugin");
#else
	int ret = GetAppConfigPath(base_module_dir, sizeof(base_module_dir), "obs-studio/plugins/%module%");
#endif

	if (ret <= 0) {
		return;
	}

	string path = base_module_dir;
#if defined(__APPLE__)
	/* User Application Support Search Path */
	obs_add_module_path((path + "/Contents/MacOS").c_str(), (path + "/Contents/Resources").c_str());

#ifndef __aarch64__
	/* Legacy System Library Search Path */
	char system_legacy_module_dir[PATH_MAX];
	GetProgramDataPath(system_legacy_module_dir, sizeof(system_legacy_module_dir), "obs-studio/plugins/%module%");
	std::string path_system_legacy = system_legacy_module_dir;
	obs_add_module_path((path_system_legacy + "/bin").c_str(), (path_system_legacy + "/data").c_str());

	/* Legacy User Application Support Search Path */
	char user_legacy_module_dir[PATH_MAX];
	GetAppConfigPath(user_legacy_module_dir, sizeof(user_legacy_module_dir), "obs-studio/plugins/%module%");
	std::string path_user_legacy = user_legacy_module_dir;
	obs_add_module_path((path_user_legacy + "/bin").c_str(), (path_user_legacy + "/data").c_str());
#endif
#else
#if ARCH_BITS == 64
	obs_add_module_path((path + "/bin/64bit").c_str(), (path + "/data").c_str());
#else
	obs_add_module_path((path + "/bin/32bit").c_str(), (path + "/data").c_str());
#endif
#endif
#endif /* __ANDROID__ */
}

/* First-party modules considered to be potentially unsafe to load in Safe Mode
 * due to them allowing external code (e.g. scripts) to modify OBS's state. */
static const unordered_set<string> unsafe_modules = {
	"frontend-tools", // Scripting
	"obs-websocket",  // Allows outside modifications
};

static void SetSafeModuleNames()
{
#ifndef SAFE_MODULES
	return;
#else
	string module;
	stringstream modules_(SAFE_MODULES);

	while (getline(modules_, module, '|')) {
		/* When only disallowing third-party plugins, still add
		 * "unsafe" bundled modules to the safe list. */
		if (disable_3p_plugins || !unsafe_modules.count(module)) {
			obs_add_safe_module(module.c_str());
		}
	}
#endif
}

static void SetCoreModuleNames()
{
#ifndef SAFE_MODULES
	throw "SAFE_MODULES not defined";
#else
	std::string safeModules = SAFE_MODULES;
	if (safeModules.empty()) {
		throw "SAFE_MODULES is empty";
	}
	string module;
	stringstream modules_(SAFE_MODULES);

	while (getline(modules_, module, '|')) {
		obs_add_core_module(module.c_str());
	}
#endif
}

extern void setupDockAction(QDockWidget *dock);

OBSBasic::OBSBasic(QWidget *parent) : OBSMainWindow(parent), undo_s(ui), ui(new Ui::OBSBasic)
{
	collections = {};

	setAttribute(Qt::WA_NativeWindow);

#ifdef TWITCH_ENABLED
	RegisterTwitchAuth();
#endif
#ifdef RESTREAM_ENABLED
	RegisterRestreamAuth();
#endif
#ifdef YOUTUBE_ENABLED
	RegisterYoutubeAuth();
#endif

	setAcceptDrops(true);

	setContextMenuPolicy(Qt::CustomContextMenu);

	QEvent::registerEventType(QEvent::User + QEvent::Close);

	api = InitializeAPIInterface(this);

	ui->setupUi(this);
	ui->previewDisabledWidget->setVisible(false);

	/* Set up streaming connections */
	connect(
		this, &OBSBasic::StreamingStarting, this, [this] { this->streamingStarting = true; },
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::StreamingStarted, this, [this] { this->streamingStarting = false; },
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::StreamingStopped, this, [this] { this->streamingStarting = false; },
		Qt::DirectConnection);

	/* Set up recording connections */
	connect(
		this, &OBSBasic::RecordingStarted, this,
		[this]() {
			this->recordingStarted = true;
			this->recordingPaused = false;
		},
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::RecordingPaused, this, [this]() { this->recordingPaused = true; },
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::RecordingUnpaused, this, [this]() { this->recordingPaused = false; },
		Qt::DirectConnection);
	connect(
		this, &OBSBasic::RecordingStopped, this,
		[this]() {
			this->recordingStarted = false;
			this->recordingPaused = false;
		},
		Qt::DirectConnection);

#ifdef __ANDROID__
	/* 3-5 前台保活：有输出在跑就把进程钉在前台档（HOME 切后台不再被当成缓存进程回收 ——
	 * 8.20 实测的 `has died: cch CRE`）。
	 *
	 * 这里**不**做"Started 起 / Stopped 停"的配对计数：推流可以在 StreamingStopFail 那条路上
	 * 停掉、录制可以因写盘错误直接 Stopped，配对少一条就永久漏着通知。改成"读一次真相
	 * （StreamingActive()/RecordingActive()）再决定起停"，幂等、不会漏。
	 *
	 * 但"信号来了当场读"build 52 实测是错的：`RecordingStopped` 发出的那一刻，
	 * `obs_output_active(fileOutput)` **还是 true**（日志里那一次复核又 start 了一遍，
	 * 连录三次后 Java 侧 describe 仍是 stop=0，通知永远挂着 —— libobs 要到 `stop_thread`
	 * 函数尾才 `os_atomic_set_bool(&output->active, false)`，前端那句 `emit RecordingStopped()`
	 * 排在它前面）。改成：状态信号只把复核排回 GUI 线程事件循环（＝排在当前这层信号栈之后）
	 * 并打开 1 秒看门狗，看门狗读到 idle 才停服务、顺手关自己 —— build 53 实测下一个 tick
	 * 就读到 false 并 `stopService #1` + `onDestroy`。代价 = 输出结束后最多 1 秒才松手，
	 * 换来不依赖信号配对（`StreamingStopFail` 那条路仍被看门狗兜住）。
	 * 排队这一步还有个附带好处：`QTimer` 只能被它的主人线程动，复核统一在 GUI 线程做就不用操心这个。 */
	QTimer *keepAliveWatchdog = new QTimer(this);
	keepAliveWatchdog->setInterval(1000);

	auto applyKeepAlive = [this, keepAliveWatchdog](const char *why) {
		const bool streaming = StreamingActive();
		const bool recording = RecordingActive();
		const char *state = (streaming && recording) ? "streaming+recording"
			      : streaming                     ? "streaming"
			      : recording                     ? "recording"
			                                      : "idle";
		QString detail;
		obsAndroidKeepAliveSetRunning(streaming || recording, state, &detail);
		/* 看门狗每秒复核一次，只在状态真的变了时才占日志。本 lambda 只在 GUI 线程跑，static 不用加锁。 */
		static QString lastState;
		const QString now = QString::fromUtf8(state);
		if (now != lastState) {
			lastState = now;
			blog(LOG_INFO, "OBSBasic[Android]: 3-5 保活复核（%s）→ %s", why, detail.toUtf8().constData());
		}
		if (streaming || recording) {
			if (!keepAliveWatchdog->isActive()) {
				keepAliveWatchdog->start();
			}
		} else {
			keepAliveWatchdog->stop();
		}
	};
	/* 排队而不是就地执行：要的是"当前这层信号栈跑完之后"再读真相，那一刻 active 才落定。 */
	auto syncKeepAlive = [this, applyKeepAlive](const char *why) {
		QMetaObject::invokeMethod(this, [applyKeepAlive, why] { applyKeepAlive(why); }, Qt::QueuedConnection);
	};
	connect(keepAliveWatchdog, &QTimer::timeout, this, [applyKeepAlive] { applyKeepAlive("watchdog"); });
	connect(this, &OBSBasic::StreamingStarted, this, [syncKeepAlive] { syncKeepAlive("StreamingStarted"); },
		Qt::DirectConnection);
	connect(this, &OBSBasic::StreamingStopping, this, [syncKeepAlive] { syncKeepAlive("StreamingStopping"); },
		Qt::DirectConnection);
	connect(this, &OBSBasic::StreamingStopped, this, [syncKeepAlive] { syncKeepAlive("StreamingStopped"); },
		Qt::DirectConnection);
	connect(this, &OBSBasic::RecordingStarted, this, [syncKeepAlive] { syncKeepAlive("RecordingStarted"); },
		Qt::DirectConnection);
	connect(this, &OBSBasic::RecordingStopping, this, [syncKeepAlive] { syncKeepAlive("RecordingStopping"); },
		Qt::DirectConnection);
	connect(this, &OBSBasic::RecordingStopped, this, [syncKeepAlive] { syncKeepAlive("RecordingStopped"); },
		Qt::DirectConnection);

	/* P-17：先把相机权限总线接进 libobs，再弹首启那一批申请框 —— 采集源插件只链 libobs，
	 * 没有这一句它就问不到 CAMERA（obs_android_camera_ready() 恒 false）。 */
	obsAndroidPermissionsRegisterCameraBus();

	/* P-18-a：屏幕采集同意总线同一批接线（obs_android_screen_* 的两格）。它不弹框，只是把
	 * "问令牌/要令牌"两条腿插进 libobs，等采集源插件来问；Java 那边对应 ObsProjectionHost。 */
	obsAndroidPermissionsRegisterScreenBus();

	/* 3-5 第 3 项：运行时权限批量申请（RECORD_AUDIO + POST_NOTIFICATIONS + CAMERA，第三项是 P-17
	 * 加进来的）。已在 Java 侧做过"缺了才弹"，每次都调这一句是安全的。 */
	obsAndroidPermissionsRequestStartup();
#endif

	/* Add controls dock */
	OBSBasicControls *controls = new OBSBasicControls(this);
	controlsDock = new OBSDock(this);
	controlsDock->setObjectName(QString::fromUtf8("controlsDock"));
	controlsDock->setWindowTitle(QTStr("Basic.Main.Controls"));
	/* Parenting is done there so controls will be deleted alongside controlsDock */
	controlsDock->setWidget(controls);

	connect(controls, &OBSBasicControls::StreamButtonClicked, this, &OBSBasic::StreamActionTriggered);

	connect(controls, &OBSBasicControls::StartStreamMenuActionClicked, this, &OBSBasic::StartStreaming);
	connect(controls, &OBSBasicControls::StopStreamMenuActionClicked, this, &OBSBasic::StopStreaming);
	connect(controls, &OBSBasicControls::ForceStopStreamMenuActionClicked, this, &OBSBasic::ForceStopStreaming);

	connect(controls, &OBSBasicControls::BroadcastButtonClicked, this, &OBSBasic::BroadcastButtonClicked);

	connect(controls, &OBSBasicControls::RecordButtonClicked, this, &OBSBasic::RecordActionTriggered);
	connect(controls, &OBSBasicControls::PauseRecordButtonClicked, this, &OBSBasic::RecordPauseToggled);

	connect(controls, &OBSBasicControls::ReplayBufferButtonClicked, this, &OBSBasic::ReplayBufferActionTriggered);
	connect(controls, &OBSBasicControls::SaveReplayBufferButtonClicked, this, &OBSBasic::ReplayBufferSave);

	connect(controls, &OBSBasicControls::VirtualCamButtonClicked, this, &OBSBasic::VirtualCamActionTriggered);
	connect(controls, &OBSBasicControls::VirtualCamConfigButtonClicked, this, &OBSBasic::OpenVirtualCamConfig);

	connect(controls, &OBSBasicControls::StudioModeButtonClicked, this, &OBSBasic::TogglePreviewProgramMode);

	connect(controls, &OBSBasicControls::SettingsButtonClicked, this, &OBSBasic::on_action_Settings_triggered);

#ifdef __ANDROID__
	connect(controls, &OBSBasicControls::FlipCameraButtonClicked, this, &OBSBasic::FlipAndroidCamera);
	/* 按钮起始是隐藏的（构造函数里不知道场景长什么样）；这里先刷一次，后面每次换场景/换选中源
	 * 都由 UpdateContextBar 顶上那一句接手。 */
	UpdateAndroidFlipCameraButton();
#endif

	/* Set up transitions combobox connections */
	connect(this, &OBSBasic::TransitionAdded, this, [this](const QString &name, const QString &uuid) {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->addItem(name, uuid);
	});
	connect(this, &OBSBasic::TransitionRenamed, this, [this](const QString &uuid, const QString &newName) {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->setItemText(ui->transitions->findData(uuid), newName);
	});
	connect(this, &OBSBasic::TransitionRemoved, this, [this](const QString &uuid) {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->removeItem(ui->transitions->findData(uuid));
	});
	connect(this, &OBSBasic::TransitionsCleared, this, [this]() {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->clear();
	});

	connect(this, &OBSBasic::CurrentTransitionChanged, this, [this](const QString &uuid) {
		QSignalBlocker sb(ui->transitions);
		ui->transitions->setCurrentIndex(ui->transitions->findData(uuid));
	});

	connect(ui->transitions, &QComboBox::currentIndexChanged, this,
		[this]() { SetCurrentTransition(ui->transitions->currentData().toString()); });

	connect(this, &OBSBasic::TransitionDurationChanged, this, [this](int duration) {
		QSignalBlocker sb(ui->transitionDuration);
		ui->transitionDuration->setValue(duration);
	});

	connect(ui->transitionDuration, &QSpinBox::valueChanged, this,
		[this](int value) { SetTransitionDuration(value); });

	/* Main window default layout */
	setDockCornersVertical(true);

	/* Scenes and Sources dock on left
	 * This specific arrangement can't be set up in Qt Designer */
	addDockWidget(Qt::LeftDockWidgetArea, ui->scenesDock);
	splitDockWidget(ui->scenesDock, ui->sourcesDock, Qt::Vertical);
	int sideDockWidth = std::min(width() * 30 / 100, 320);
	resizeDocks({ui->scenesDock, ui->sourcesDock}, {sideDockWidth, sideDockWidth}, Qt::Horizontal);
	addDockWidget(Qt::BottomDockWidgetArea, controlsDock);

	startingDockLayout = saveState();

	statsDock = new OBSDock();
	statsDock->setObjectName(QStringLiteral("statsDock"));
	statsDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			       QDockWidget::DockWidgetFloatable);
	statsDock->setWindowTitle(QTStr("Basic.Stats"));
	addDockWidget(Qt::BottomDockWidgetArea, statsDock);
	statsDock->setVisible(false);
	statsDock->setFloating(true);
	statsDock->resize(700, 200);

	copyActionsDynamicProperties();

	qRegisterMetaType<int64_t>("int64_t");
	qRegisterMetaType<uint32_t>("uint32_t");
	qRegisterMetaType<OBSScene>("OBSScene");
	qRegisterMetaType<OBSSceneItem>("OBSSceneItem");
	qRegisterMetaType<OBSSource>("OBSSource");
	qRegisterMetaType<obs_hotkey_id>("obs_hotkey_id");
	qRegisterMetaType<SavedProjectorInfo *>("SavedProjectorInfo *");

	ui->scenes->setAttribute(Qt::WA_MacShowFocusRect, false);
	ui->sources->setAttribute(Qt::WA_MacShowFocusRect, false);

	bool sceneGrid = config_get_bool(App()->GetUserConfig(), "BasicWindow", "gridMode");
	ui->scenes->SetGridMode(sceneGrid);

	if (sceneGrid) {
		ui->actionSceneGridMode->setChecked(true);
	} else {
		ui->actionSceneListMode->setChecked(true);
	}

	ui->scenes->setItemDelegate(new SceneRenameDelegate(ui->scenes));

	auto displayResize = [this]() {
		struct obs_video_info ovi;

		if (obs_get_video_info(&ovi)) {
			ResizePreview(ovi.base_width, ovi.base_height);
		}

		UpdateContextBarVisibility();
		UpdatePreviewControls();
		dpi = devicePixelRatioF();
	};
	dpi = devicePixelRatioF();

	connect(windowHandle(), &QWindow::screenChanged, this, displayResize);
	connect(ui->preview, &OBSQTDisplay::DisplayResized, this, displayResize);

	/* TODO: Move these into window-basic-preview */
	/* Preview Scaling label */
	connect(ui->preview, &OBSBasicPreview::scalingChanged, ui->previewScalePercent,
		&OBSPreviewScalingLabel::PreviewScaleChanged);

	/* Preview Scaling dropdown */
	connect(ui->preview, &OBSBasicPreview::scalingChanged, ui->previewScalingMode,
		&OBSPreviewScalingComboBox::PreviewScaleChanged);

	connect(ui->preview, &OBSBasicPreview::fixedScalingChanged, ui->previewScalingMode,
		&OBSPreviewScalingComboBox::PreviewFixedScalingChanged);

	connect(ui->previewScalingMode, &OBSPreviewScalingComboBox::currentIndexChanged, this,
		&OBSBasic::PreviewScalingModeChanged);

	/* Preview Controls */
	connect(ui->previewXScrollBar, &QScrollBar::sliderMoved, ui->preview, &OBSBasicPreview::xScrollBarChanged);
	connect(ui->previewYScrollBar, &QScrollBar::valueChanged, ui->preview, &OBSBasicPreview::yScrollBarChanged);

	connect(ui->previewZoomInButton, &QPushButton::clicked, ui->preview, &OBSBasicPreview::increaseScalingLevel);
	connect(ui->previewZoomOutButton, &QPushButton::clicked, ui->preview, &OBSBasicPreview::decreaseScalingLevel);

	/* Preview Actions */
	connect(ui->actionScaleWindow, &QAction::triggered, this, &OBSBasic::setPreviewScalingWindow);
	connect(ui->actionScaleCanvas, &QAction::triggered, this, &OBSBasic::setPreviewScalingCanvas);
	connect(ui->actionScaleOutput, &QAction::triggered, this, &OBSBasic::setPreviewScalingOutput);

	connect(ui->actionPreviewZoomIn, &QAction::triggered, ui->preview, &OBSBasicPreview::increaseScalingLevel);
	connect(ui->actionPreviewZoomOut, &QAction::triggered, ui->preview, &OBSBasicPreview::decreaseScalingLevel);
	connect(ui->actionPreviewResetZoom, &QAction::triggered, ui->preview, &OBSBasicPreview::resetScalingLevel);

	connect(this, &OBSBasic::CanvasResized, ui->previewScalingMode, &OBSPreviewScalingComboBox::CanvasResized);
	connect(this, &OBSBasic::OutputResized, ui->previewScalingMode, &OBSPreviewScalingComboBox::OutputResized);

	delete shortcutFilter;
	shortcutFilter = CreateShortcutFilter();
	installEventFilter(shortcutFilter);

	stringstream name;
	name << "OBS " << App()->GetVersionString();
	blog(LOG_INFO, "%s", name.str().c_str());
	blog(LOG_INFO, "---------------------------------");

	UpdateTitleBar();

	connect(ui->scenes->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, &OBSBasic::SceneNameEdited);

	cpuUsageInfo = os_cpu_usage_info_start();
	cpuUsageTimer = new QTimer(this);
	connect(cpuUsageTimer.data(), &QTimer::timeout, ui->statusbar, &OBSBasicStatusBar::UpdateCPUUsage);
	cpuUsageTimer->start(3000);

	diskFullTimer = new QTimer(this);
	connect(diskFullTimer, &QTimer::timeout, this, &OBSBasic::CheckDiskSpaceRemaining);

	renameScene = new QAction(QTStr("Rename"), ui->scenesDock);
	renameScene->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(renameScene, &QAction::triggered, this, &OBSBasic::EditSceneName);
	ui->scenesDock->addAction(renameScene);

	renameSource = new QAction(QTStr("Rename"), ui->sourcesDock);
	renameSource->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(renameSource, &QAction::triggered, this, &OBSBasic::EditSceneItemName);
	ui->sourcesDock->addAction(renameSource);

#ifdef __APPLE__
	renameScene->setShortcut({Qt::Key_Return});
	renameSource->setShortcut({Qt::Key_Return});

	ui->actionRemoveSource->setShortcuts({Qt::Key_Backspace, Qt::Key_Delete});
	ui->actionRemoveScene->setShortcuts({Qt::Key_Backspace, Qt::Key_Delete});

	ui->actionCheckForUpdates->setMenuRole(QAction::AboutQtRole);
	ui->action_Settings->setMenuRole(QAction::PreferencesRole);
	ui->actionShowMacPermissions->setMenuRole(QAction::ApplicationSpecificRole);
	delete ui->actionE_xit;
#else
	renameScene->setShortcut({Qt::Key_F2});
	renameSource->setShortcut({Qt::Key_F2});
#endif

#ifdef __linux__
	ui->actionE_xit->setShortcut(Qt::CTRL | Qt::Key_Q);
#endif

#ifndef ENABLE_IDIAN_PLAYGROUND
	ui->idianPlayground->setVisible(false);
#endif

	auto addNudge = [this](const QKeySequence &seq, MoveDir direction, int distance) {
		QAction *nudge = new QAction(ui->preview);
		nudge->setShortcut(seq);
		nudge->setShortcutContext(Qt::WidgetShortcut);
		ui->preview->addAction(nudge);
		connect(nudge, &QAction::triggered, this,
			[this, distance, direction]() { Nudge(distance, direction); });
	};

	addNudge(Qt::Key_Up, MoveDir::Up, 1);
	addNudge(Qt::Key_Down, MoveDir::Down, 1);
	addNudge(Qt::Key_Left, MoveDir::Left, 1);
	addNudge(Qt::Key_Right, MoveDir::Right, 1);
	addNudge(Qt::SHIFT | Qt::Key_Up, MoveDir::Up, 10);
	addNudge(Qt::SHIFT | Qt::Key_Down, MoveDir::Down, 10);
	addNudge(Qt::SHIFT | Qt::Key_Left, MoveDir::Left, 10);
	addNudge(Qt::SHIFT | Qt::Key_Right, MoveDir::Right, 10);

	/* Setup dock toggle action
	 * And hide all docks before restoring parent geometry */
#define SETUP_DOCK(dock)                                    \
	setupDockAction(dock);                              \
	ui->menuDocks->addAction(dock->toggleViewAction()); \
	dock->setVisible(false);

	SETUP_DOCK(ui->scenesDock);
	SETUP_DOCK(ui->sourcesDock);
	SETUP_DOCK(ui->mixerDock);
	SETUP_DOCK(ui->transitionsDock);
	SETUP_DOCK(controlsDock);
	SETUP_DOCK(statsDock);
#undef SETUP_DOCK

	// Register shortcuts for Undo/Redo
	ui->actionMainUndo->setShortcut(Qt::CTRL | Qt::Key_Z);
	QList<QKeySequence> shrt;
	shrt << QKeySequence((Qt::CTRL | Qt::SHIFT) | Qt::Key_Z) << QKeySequence(Qt::CTRL | Qt::Key_Y);
	ui->actionMainRedo->setShortcuts(shrt);

	ui->actionMainUndo->setShortcutContext(Qt::ApplicationShortcut);
	ui->actionMainRedo->setShortcutContext(Qt::ApplicationShortcut);

	QPoint curPos;

	//restore parent window geometry
	const char *geometry = config_get_string(App()->GetUserConfig(), "BasicWindow", "geometry");
	if (geometry != NULL) {
		QByteArray byteArray = QByteArray::fromBase64(QByteArray(geometry));
		restoreGeometry(byteArray);

		QRect windowGeometry = normalGeometry();
		if (!WindowPositionValid(windowGeometry)) {
			QRect rect = QGuiApplication::primaryScreen()->geometry();
			setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size(), rect));
		}

		curPos = pos();
	} else {
		QRect desktopRect = QGuiApplication::primaryScreen()->geometry();
		QSize adjSize = desktopRect.size() / 2 - size() / 2;
		curPos = QPoint(adjSize.width(), adjSize.height());
	}

	QPoint curSize(width(), height());

	QPoint statsDockSize(statsDock->width(), statsDock->height());
	QPoint statsDockPos = curSize / 2 - statsDockSize / 2;
	QPoint newPos = curPos + statsDockPos;
	statsDock->move(newPos);

	ui->actionReleaseNotes->setVisible(true);

	ui->previewDisabledWidget->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(ui->enablePreviewButton, &QPushButton::clicked, this, &OBSBasic::TogglePreview);

	connect(ui->scenes, &SceneTree::scenesReordered, ui->scenes,
		[]() { OBSProjector::UpdateMultiviewProjectors(); });

	connect(App(), &OBSApp::StyleChanged, this, [this]() { OnEvent(OBS_FRONTEND_EVENT_THEME_CHANGED); });
#ifndef __APPLE__
	connect(App(), &OBSApp::aboutToQuit, this, &OBSBasic::closeWindow);
#endif

	QActionGroup *actionGroup = new QActionGroup(this);
	actionGroup->addAction(ui->actionSceneListMode);
	actionGroup->addAction(ui->actionSceneGridMode);

	UpdatePreviewSafeAreas();
	UpdatePreviewSpacingHelpers();
	UpdatePreviewOverflowSettings();
}

static const double scaled_vals[] = {1.0, 1.25, (1.0 / 0.75), 1.5, (1.0 / 0.6), 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 0.0};

#ifdef __APPLE__ // macOS
#define DEFAULT_CONTAINER "hybrid_mov"
#else // Windows/Linux
#define DEFAULT_CONTAINER "hybrid_mp4"
#endif

bool OBSBasic::InitBasicConfigDefaults()
{
	QList<QScreen *> screens = QGuiApplication::screens();

	if (!screens.size()) {
		OBSErrorBox(NULL, "There appears to be no monitors.  Er, this "
				  "technically shouldn't be possible.");
		return false;
	}

	QScreen *primaryScreen = QGuiApplication::primaryScreen();

	uint32_t cx = primaryScreen->size().width();
	uint32_t cy = primaryScreen->size().height();

	cx *= devicePixelRatioF();
	cy *= devicePixelRatioF();

	bool oldResolutionDefaults = config_get_bool(App()->GetUserConfig(), "General", "Pre19Defaults");

	/* use 1920x1080 for new default base res if main monitor is above
	 * 1920x1080, but don't apply for people from older builds -- only to
	 * new users */
	if (!oldResolutionDefaults && (cx * cy) > (1920 * 1080)) {
		cx = 1920;
		cy = 1080;
	}

	bool changed = false;

	/* ----------------------------------------------------- */
	/* move over old FFmpeg track settings                   */
	if (config_has_user_value(activeConfiguration, "AdvOut", "FFAudioTrack") &&
	    !config_has_user_value(activeConfiguration, "AdvOut", "Pre22.1Settings")) {

		int track = (int)config_get_int(activeConfiguration, "AdvOut", "FFAudioTrack");
		config_set_int(activeConfiguration, "AdvOut", "FFAudioMixes", 1LL << (track - 1));
		config_set_bool(activeConfiguration, "AdvOut", "Pre22.1Settings", true);
		changed = true;
	}

	/* ----------------------------------------------------- */
	/* move over mixer values in advanced if older config */
	if (config_has_user_value(activeConfiguration, "AdvOut", "RecTrackIndex") &&
	    !config_has_user_value(activeConfiguration, "AdvOut", "RecTracks")) {

		uint64_t track = config_get_uint(activeConfiguration, "AdvOut", "RecTrackIndex");
		track = 1ULL << (track - 1);
		config_set_uint(activeConfiguration, "AdvOut", "RecTracks", track);
		config_remove_value(activeConfiguration, "AdvOut", "RecTrackIndex");
		changed = true;
	}

	/* ----------------------------------------------------- */
	/* set twitch chat extensions to "both" if prev version  */
	/* is under 24.1                                         */
	if (config_get_bool(App()->GetUserConfig(), "General", "Pre24.1Defaults") &&
	    !config_has_user_value(activeConfiguration, "Twitch", "AddonChoice")) {
		config_set_int(activeConfiguration, "Twitch", "AddonChoice", 3);
		changed = true;
	}

	/* ----------------------------------------------------- */
	/* move bitrate enforcement setting to new value         */
	if (config_has_user_value(activeConfiguration, "SimpleOutput", "EnforceBitrate") &&
	    !config_has_user_value(activeConfiguration, "Stream1", "IgnoreRecommended") &&
	    !config_has_user_value(activeConfiguration, "Stream1", "MovedOldEnforce")) {
		bool enforce = config_get_bool(activeConfiguration, "SimpleOutput", "EnforceBitrate");
		config_set_bool(activeConfiguration, "Stream1", "IgnoreRecommended", !enforce);
		config_set_bool(activeConfiguration, "Stream1", "MovedOldEnforce", true);
		changed = true;
	}

	/* ----------------------------------------------------- */
	/* enforce minimum retry delay of 1 second prior to 27.1 */
	if (config_has_user_value(activeConfiguration, "Output", "RetryDelay")) {
		int retryDelay = config_get_uint(activeConfiguration, "Output", "RetryDelay");
		if (retryDelay < 1) {
			config_set_uint(activeConfiguration, "Output", "RetryDelay", 1);
			changed = true;
		}
	}

	/* ----------------------------------------------------- */
	/* Migrate old container selection (if any) to new key.  */

	auto MigrateFormat = [&](const char *section) {
		bool has_old_key = config_has_user_value(activeConfiguration, section, "RecFormat");
		bool has_new_key = config_has_user_value(activeConfiguration, section, "RecFormat2");
		if (!has_new_key && !has_old_key) {
			return;
		}

		string old_format =
			config_get_string(activeConfiguration, section, has_new_key ? "RecFormat2" : "RecFormat");
		string new_format = old_format;
		if (old_format == "ts") {
			new_format = "mpegts";
		} else if (old_format == "m3u8") {
			new_format = "hls";
		} else if (old_format == "fmp4") {
			new_format = "fragmented_mp4";
		} else if (old_format == "fmov") {
			new_format = "fragmented_mov";
		}

		if (new_format != old_format || !has_new_key) {
			config_set_string(activeConfiguration, section, "RecFormat2", new_format.c_str());
			changed = true;
		}
	};

	MigrateFormat("AdvOut");
	MigrateFormat("SimpleOutput");

	/* ----------------------------------------------------- */
	/* Migrate output scale setting to GPU scaling options.  */

	if (config_get_bool(activeConfiguration, "AdvOut", "Rescale") &&
	    !config_has_user_value(activeConfiguration, "AdvOut", "RescaleFilter")) {
		config_set_int(activeConfiguration, "AdvOut", "RescaleFilter", OBS_SCALE_BILINEAR);
	}

	if (config_get_bool(activeConfiguration, "AdvOut", "RecRescale") &&
	    !config_has_user_value(activeConfiguration, "AdvOut", "RecRescaleFilter")) {
		config_set_int(activeConfiguration, "AdvOut", "RecRescaleFilter", OBS_SCALE_BILINEAR);
	}

	/* ----------------------------------------------------- */

	if (changed) {
		activeConfiguration.SaveSafe("tmp");
	}

	/* ----------------------------------------------------- */

	config_set_default_string(activeConfiguration, "Output", "Mode", "Simple");

	config_set_default_bool(activeConfiguration, "Stream1", "IgnoreRecommended", false);
	config_set_default_bool(activeConfiguration, "Stream1", "EnableMultitrackVideo", false);
	config_set_default_bool(activeConfiguration, "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto", true);
	config_set_default_bool(activeConfiguration, "Stream1", "MultitrackVideoMaximumVideoTracksAuto", true);

	config_set_default_string(activeConfiguration, "SimpleOutput", "FilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecFormat2", DEFAULT_CONTAINER);
	config_set_default_uint(activeConfiguration, "SimpleOutput", "VBitrate", 6000);
	config_set_default_uint(activeConfiguration, "SimpleOutput", "ABitrate", 160);
	config_set_default_bool(activeConfiguration, "SimpleOutput", "UseAdvanced", false);
	config_set_default_string(activeConfiguration, "SimpleOutput", "Preset", "veryfast");
	config_set_default_string(activeConfiguration, "SimpleOutput", "NVENCPreset2", "p5");
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecQuality", "Stream");
	config_set_default_bool(activeConfiguration, "SimpleOutput", "RecRB", false);
	config_set_default_int(activeConfiguration, "SimpleOutput", "RecRBTime", 20);
	config_set_default_int(activeConfiguration, "SimpleOutput", "RecRBSize", 512);
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecRBPrefix", "Replay");
	config_set_default_string(activeConfiguration, "SimpleOutput", "StreamAudioEncoder", "aac");
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecAudioEncoder", "aac");
	config_set_default_uint(activeConfiguration, "SimpleOutput", "RecTracks", (1 << 0));

	config_set_default_bool(activeConfiguration, "AdvOut", "ApplyServiceSettings", true);
	config_set_default_bool(activeConfiguration, "AdvOut", "UseRescale", false);
	config_set_default_uint(activeConfiguration, "AdvOut", "TrackIndex", 1);
	config_set_default_uint(activeConfiguration, "AdvOut", "VodTrackIndex", 2);
	config_set_default_string(activeConfiguration, "AdvOut", "Encoder", "obs_x264");

	config_set_default_string(activeConfiguration, "AdvOut", "RecType", "Standard");

	config_set_default_string(activeConfiguration, "AdvOut", "RecFilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(activeConfiguration, "AdvOut", "RecFormat2", DEFAULT_CONTAINER);
	config_set_default_bool(activeConfiguration, "AdvOut", "RecUseRescale", false);
	config_set_default_uint(activeConfiguration, "AdvOut", "RecTracks", (1 << 0));
	config_set_default_string(activeConfiguration, "AdvOut", "RecEncoder", "none");
	config_set_default_uint(activeConfiguration, "AdvOut", "FLVTrack", 1);
	config_set_default_uint(activeConfiguration, "AdvOut", "StreamMultiTrackAudioMixes", 1);
	config_set_default_bool(activeConfiguration, "AdvOut", "FFOutputToFile", true);
	config_set_default_string(activeConfiguration, "AdvOut", "FFFilePath", GetDefaultVideoSavePath().c_str());
	config_set_default_string(activeConfiguration, "AdvOut", "FFExtension", "mp4");
	config_set_default_uint(activeConfiguration, "AdvOut", "FFVBitrate", 6000);
	config_set_default_uint(activeConfiguration, "AdvOut", "FFVGOPSize", 250);
	config_set_default_bool(activeConfiguration, "AdvOut", "FFUseRescale", false);
	config_set_default_bool(activeConfiguration, "AdvOut", "FFIgnoreCompat", false);
	config_set_default_uint(activeConfiguration, "AdvOut", "FFABitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "FFAudioMixes", 1);

	config_set_default_uint(activeConfiguration, "AdvOut", "Track1Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track2Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track3Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track4Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track5Bitrate", 160);
	config_set_default_uint(activeConfiguration, "AdvOut", "Track6Bitrate", 160);

	config_set_default_uint(activeConfiguration, "AdvOut", "RecSplitFileTime", 15);
	config_set_default_uint(activeConfiguration, "AdvOut", "RecSplitFileSize", 2048);

	config_set_default_bool(activeConfiguration, "AdvOut", "RecRB", false);
	config_set_default_uint(activeConfiguration, "AdvOut", "RecRBTime", 20);
	config_set_default_int(activeConfiguration, "AdvOut", "RecRBSize", 512);

	config_set_default_uint(activeConfiguration, "Video", "BaseCX", cx);
	config_set_default_uint(activeConfiguration, "Video", "BaseCY", cy);

	/* don't allow BaseCX/BaseCY to be susceptible to defaults changing */
	if (!config_has_user_value(activeConfiguration, "Video", "BaseCX") ||
	    !config_has_user_value(activeConfiguration, "Video", "BaseCY")) {
		config_set_uint(activeConfiguration, "Video", "BaseCX", cx);
		config_set_uint(activeConfiguration, "Video", "BaseCY", cy);
		config_save_safe(activeConfiguration, "tmp", nullptr);
	}

	config_set_default_string(activeConfiguration, "Output", "FilenameFormatting", "%CCYY-%MM-%DD %hh-%mm-%ss");

	config_set_default_bool(activeConfiguration, "Output", "DelayEnable", false);
	config_set_default_uint(activeConfiguration, "Output", "DelaySec", 20);
	config_set_default_bool(activeConfiguration, "Output", "DelayPreserve", true);

	config_set_default_bool(activeConfiguration, "Output", "Reconnect", true);
	config_set_default_uint(activeConfiguration, "Output", "RetryDelay", 2);
	config_set_default_uint(activeConfiguration, "Output", "MaxRetries", 25);

	config_set_default_string(activeConfiguration, "Output", "BindIP", "default");
	config_set_default_string(activeConfiguration, "Output", "IPFamily", "IPv4+IPv6");
	config_set_default_bool(activeConfiguration, "Output", "NewSocketLoopEnable", false);
	config_set_default_bool(activeConfiguration, "Output", "LowLatencyEnable", false);

	int i = 0;
	uint32_t scale_cx = cx;
	uint32_t scale_cy = cy;

	/* use a default scaled resolution that has a pixel count no higher
	 * than 1280x720 */
	while (((scale_cx * scale_cy) > (1280 * 720)) && scaled_vals[i] > 0.0) {
		double scale = scaled_vals[i++];
		scale_cx = uint32_t(double(cx) / scale);
		scale_cy = uint32_t(double(cy) / scale);
	}

	config_set_default_uint(activeConfiguration, "Video", "OutputCX", scale_cx);
	config_set_default_uint(activeConfiguration, "Video", "OutputCY", scale_cy);

	/* don't allow OutputCX/OutputCY to be susceptible to defaults
	 * changing */
	if (!config_has_user_value(activeConfiguration, "Video", "OutputCX") ||
	    !config_has_user_value(activeConfiguration, "Video", "OutputCY")) {
		config_set_uint(activeConfiguration, "Video", "OutputCX", scale_cx);
		config_set_uint(activeConfiguration, "Video", "OutputCY", scale_cy);
		config_save_safe(activeConfiguration, "tmp", nullptr);
	}

	config_set_default_uint(activeConfiguration, "Video", "FPSType", 0);
	config_set_default_string(activeConfiguration, "Video", "FPSCommon", "30");
	config_set_default_uint(activeConfiguration, "Video", "FPSInt", 30);
	config_set_default_uint(activeConfiguration, "Video", "FPSNum", 30);
	config_set_default_uint(activeConfiguration, "Video", "FPSDen", 1);
	config_set_default_string(activeConfiguration, "Video", "ScaleType", "bicubic");
	config_set_default_string(activeConfiguration, "Video", "ColorFormat", "NV12");
	config_set_default_string(activeConfiguration, "Video", "ColorSpace", "709");
	config_set_default_string(activeConfiguration, "Video", "ColorRange", "Partial");
	config_set_default_uint(activeConfiguration, "Video", "SdrWhiteLevel", 300);
	config_set_default_uint(activeConfiguration, "Video", "HdrNominalPeakLevel", 1000);

	config_set_default_string(activeConfiguration, "Audio", "MonitoringDeviceId", "default");
	config_set_default_string(activeConfiguration, "Audio", "MonitoringDeviceName",
				  Str("Basic.Settings.Advanced.Audio.MonitoringDevice"
				      ".Default"));
	config_set_default_uint(activeConfiguration, "Audio", "SampleRate", 48000);
	config_set_default_string(activeConfiguration, "Audio", "ChannelSetup", "Stereo");
	config_set_default_double(activeConfiguration, "Audio", "MeterDecayRate", VOLUME_METER_DECAY_FAST);
	config_set_default_uint(activeConfiguration, "Audio", "PeakMeterType", 0);

#ifdef __ANDROID__
	/* Android（P-6④ / P-7 实测）：录像只有 hybrid 两档容器能用 —— SimpleOutput 与 AdvancedOutput 把
	 * 非 hybrid 一律映射到 `ffmpeg_muxer`，而那个伴生进程按决策 8 根本没编 ⇒ 选了就是"点录制 → 0.8 秒后
	 * 静默失败"，UI 上毫无提示；`RecQuality=Lossless` 更狠，`SimpleOutput.cpp:49-58` 直接把容器硬写成 avi。
	 * 设置页的下拉已经裁过（`OBSBasicSettings.cpp::LoadFormats` 的 `#ifndef __ANDROID__`），但那挡得住
	 * **在这台机器上改设置的人**、挡不住从桌面拷来的 profile：那句 `RecFormat2=mkv` 会绕过下拉直达
	 * `ConfigureRecording`。09-07 实测过更绕的一种：下拉显示"混合 MP4"、配置里却仍是 mkv，因为
	 * `SaveComboData()` 外面套着 `if (WidgetChanged(widget))`（而 `OutputsChanged()` 又让 loading 期间的
	 * 程序化改动一律不算改动）⇒ 用户没亲手点过那一项，就永远不会把修好的值写回去。
	 * ⇒ 在 profile 刚落地成内存配置的这一步纠正（`SetCurrentProfile` 末尾原有的 `SaveSafe("tmp")` 负责写盘）。 */
	auto clampContainer = [this](const char *section) {
		const char *cur = config_get_string(activeConfiguration, section, "RecFormat2");
		if (strcmp(cur, "hybrid_mp4") == 0 || strcmp(cur, "hybrid_mov") == 0) {
			return;
		}
		blog(LOG_WARNING, "Android 配置纠正：[%s] RecFormat2='%s' 本平台不可用（ffmpeg_muxer 未随包）⇒ 改用 '%s'",
		     section, cur, DEFAULT_CONTAINER);
		config_set_string(activeConfiguration, section, "RecFormat2", DEFAULT_CONTAINER);
	};
	clampContainer("SimpleOutput");
	clampContainer("AdvOut");

	const char *qual = config_get_string(activeConfiguration, "SimpleOutput", "RecQuality");
	if (strcmp(qual, "Lossless") == 0) {
		blog(LOG_WARNING, "Android 配置纠正：SimpleOutput RecQuality='Lossless' 会把容器硬写成 avi"
				  "（SimpleOutput.cpp:49-58）⇒ 改用 'Stream'");
		config_set_string(activeConfiguration, "SimpleOutput", "RecQuality", "Stream");
	}

	/* P-12：另一条独立入口 —— 高级输出的「录制 → 类型」选「自定义输出 (FFmpeg)」时，`AdvOut/RecType`
	 * 一旦落成 "FFmpeg"，`AdvancedOutput::CreateStreaming` 之外的整条录制管线（`AdvancedOutput.cpp:62`、
	 * `OBSBasic_Recording.cpp:37/386`）都会改走 `ffmpeg_muxer`，绕开上面那两张裁过的容器表。设置页里
	 * 这一档已经从下拉收掉（`OBSBasicSettings.cpp` 构造函数的 `#ifdef`），但下拉只挡得住在这台机器上点的人，
	 * 挡不住从桌面拷来的 profile ⇒ 与 `clampContainer` 同一层纠正。 */
	const char *recType = config_get_string(activeConfiguration, "AdvOut", "RecType");
	if (strcmp(recType, "Standard") != 0) {
		blog(LOG_WARNING, "Android 配置纠正：AdvOut RecType='%s' 本平台不可用（ffmpeg_muxer 未随包）⇒ 改用 'Standard'",
		     recType);
		config_set_string(activeConfiguration, "AdvOut", "RecType", "Standard");
	}
#endif

	CheckExistingCookieId();

	return true;
}

void OBSBasic::InitBasicConfigDefaults2()
{
	bool oldEncDefaults = config_get_bool(App()->GetUserConfig(), "General", "Pre23Defaults");
	bool useNV = EncoderAvailable("ffmpeg_nvenc") && !oldEncDefaults;

	config_set_default_string(activeConfiguration, "SimpleOutput", "StreamEncoder",
				  useNV ? SIMPLE_ENCODER_NVENC : SIMPLE_ENCODER_X264);
	config_set_default_string(activeConfiguration, "SimpleOutput", "RecEncoder",
				  useNV ? SIMPLE_ENCODER_NVENC : SIMPLE_ENCODER_X264);

	const char *aac_default = "ffmpeg_aac";
	if (EncoderAvailable("CoreAudio_AAC")) {
		aac_default = "CoreAudio_AAC";
	} else if (EncoderAvailable("libfdk_aac")) {
		aac_default = "libfdk_aac";
	}

	config_set_default_string(activeConfiguration, "AdvOut", "AudioEncoder", aac_default);
	config_set_default_string(activeConfiguration, "AdvOut", "RecAudioEncoder", aac_default);
}

bool OBSBasic::InitBasicConfig()
{
	ProfileScope("OBSBasic::InitBasicConfig");

	RefreshProfiles(true);

	const std::string currentProfileName{config_get_string(App()->GetUserConfig(), "Basic", "Profile")};
	const std::optional<OBSProfile> currentProfile = GetProfileByName(currentProfileName);
	const std::optional<OBSProfile> foundProfile = GetProfileByName(opt_starting_profile);

	try {
		if (foundProfile) {
			ActivateProfile(foundProfile.value());
		} else if (currentProfile) {
			ActivateProfile(currentProfile.value());
		} else {
			const OBSProfile &newProfile = CreateProfile(currentProfileName);
			ActivateProfile(newProfile);
		}
	} catch (const std::logic_error &) {
		OBSErrorBox(NULL, "Failed to open basic.ini: %d", -1);
		return false;
	}

	return true;
}

void OBSBasic::InitOBSCallbacks()
{
	ProfileScope("OBSBasic::InitOBSCallbacks");

	signalHandlers.reserve(signalHandlers.size() + 6);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_create", OBSBasic::SourceCreated, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_remove", OBSBasic::SourceRemoved, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_rename", OBSBasic::SourceRenamed, this);
	signalHandlers.emplace_back(
		obs_get_signal_handler(), "source_filter_add",
		[](void *data, calldata_t *) {
			QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::UpdateEditMenu,
						  Qt::QueuedConnection);
		},
		this);
	signalHandlers.emplace_back(
		obs_get_signal_handler(), "source_filter_remove",
		[](void *data, calldata_t *) {
			QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), &OBSBasic::UpdateEditMenu,
						  Qt::QueuedConnection);
		},
		this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "canvas_remove", OBSBasic::CanvasRemoved, this);
}

#define STARTUP_SEPARATOR "==== Startup complete ==============================================="
#define SHUTDOWN_SEPARATOR "==== Shutting down =================================================="

#define UNSUPPORTED_ERROR                                                     \
	"Failed to initialize video:\n\nRequired graphics API functionality " \
	"not found.  Your GPU may not be supported."

#define UNKNOWN_ERROR                                                  \
	"Failed to initialize video.  Your GPU may not be supported, " \
	"or your graphics drivers may need to be updated."

static inline void LogEncoders()
{
	constexpr uint32_t hide_flags = OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL;

	auto list_encoders = [](obs_encoder_type type) {
		size_t idx = 0;
		const char *encoder_type;

		while (obs_enum_encoder_types(idx++, &encoder_type)) {
			if (obs_get_encoder_caps(encoder_type) & hide_flags ||
			    obs_get_encoder_type(encoder_type) != type) {
				continue;
			}

			blog(LOG_INFO, "\t- %s (%s)", encoder_type, obs_encoder_get_display_name(encoder_type));
		}
	};

	blog(LOG_INFO, "---------------------------------");
	blog(LOG_INFO, "Available Encoders:");
	blog(LOG_INFO, "  Video Encoders:");
	list_encoders(OBS_ENCODER_VIDEO);
	blog(LOG_INFO, "  Audio Encoders:");
	list_encoders(OBS_ENCODER_AUDIO);
}

void OBSBasic::OBSInit()
{
	ProfileScope("OBSBasic::OBSInit");

	if (!InitBasicConfig()) {
		throw "Failed to load basic.ini";
	}
	if (!ResetAudio()) {
		throw "Failed to initialize audio";
	}

	int ret = 0;

	ret = ResetVideo();

	switch (ret) {
	case OBS_VIDEO_MODULE_NOT_FOUND:
		throw "Failed to initialize video:  Graphics module not found";
	case OBS_VIDEO_NOT_SUPPORTED:
		throw UNSUPPORTED_ERROR;
	case OBS_VIDEO_INVALID_PARAM:
		throw "Failed to initialize video:  Invalid parameters";
	default:
		if (ret != OBS_VIDEO_SUCCESS) {
			throw UNKNOWN_ERROR;
		}
	}

	/* load audio monitoring */
	if (obs_audio_monitoring_available()) {
		const char *device_name = config_get_string(activeConfiguration, "Audio", "MonitoringDeviceName");
		const char *device_id = config_get_string(activeConfiguration, "Audio", "MonitoringDeviceId");

		obs_set_audio_monitoring_device(device_name, device_id);

		blog(LOG_INFO, "Audio monitoring device:\n\tname: %s\n\tid: %s", device_name, device_id);
	}

	InitOBSCallbacks();
	InitHotkeys();
	ui->preview->Init();

	/* hack to prevent elgato from loading its own QtNetwork that it tries
	 * to ship with */
#if defined(_WIN32) && !defined(_DEBUG)
	LoadLibraryW(L"Qt6Network");
#endif
	struct obs_module_failure_info mfi;

	// Safe Mode disables third-party plugins so we don't need to add each path outside the OBS bundle/installation.
	if (safe_mode || disable_3p_plugins) {
		SetSafeModuleNames();
	} else {
		AddExtraModulePaths();
	}

	// Core modules are not allowed to be disabled by the user via plugin manager.
	SetCoreModuleNames();

	/* Modules can access frontend information (i.e. profile and scene collection data) during their initialization, and some modules (e.g. obs-websockets) are known to use the filesystem location of the current profile in their own code.

     Thus the profile and scene collection discovery needs to happen before any access to that information (but after initializing global settings) to ensure legacy code gets valid path information.
     */
	RefreshSceneCollections(true);

	App()->loadAppModules(mfi);

	BPtr<char *> failed_modules = mfi.failed_modules;

#ifdef BROWSER_AVAILABLE
	cef = obs_browser_init_panel();
	cef_js_avail = cef && obs_browser_qcef_version() >= 3;
#endif

	vcamEnabled = (obs_get_output_flags(VIRTUAL_CAM_ID) & OBS_OUTPUT_VIDEO) != 0;
	if (vcamEnabled) {
		emit VirtualCamEnabled();
	}

	UpdateProfileEncoders();

	LogEncoders();

	blog(LOG_INFO, STARTUP_SEPARATOR);

	if (!InitService()) {
		throw "Failed to initialize service";
	}
#ifdef __ANDROID__
	/* 判据：装了 rtmp-services 之后这条应恒为 rtmp_common/rtmp。 */
	if (obs_service_t *svc = GetService()) {
		obsAndroidDisplayLog("P-2 服务态：'%s' 类型=%s 协议=%s", obs_service_get_name(svc), obs_service_get_id(svc),
				     obs_service_get_protocol(svc));
	}
#endif

	ResetOutputs();
	CreateHotkeys();

	InitPrimitives();

	sceneDuplicationMode = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SceneDuplicationMode");
	swapScenesMode = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SwapScenesMode");
	editPropertiesMode = config_get_bool(App()->GetUserConfig(), "BasicWindow", "EditPropertiesMode");

	if (!opt_studio_mode) {
		SetPreviewProgramMode(config_get_bool(App()->GetUserConfig(), "BasicWindow", "PreviewProgramMode"));
	} else {
		SetPreviewProgramMode(true);
		opt_studio_mode = false;
	}

#define SET_VISIBILITY(name, control)                                                                \
	do {                                                                                         \
		if (config_has_user_value(App()->GetUserConfig(), "BasicWindow", name)) {            \
			bool visible = config_get_bool(App()->GetUserConfig(), "BasicWindow", name); \
			ui->control->setChecked(visible);                                            \
		}                                                                                    \
	} while (false)

	SET_VISIBILITY("ShowListboxToolbars", toggleListboxToolbars);
	SET_VISIBILITY("ShowStatusBar", toggleStatusBar);
#undef SET_VISIBILITY

	bool sourceIconsVisible = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowSourceIcons");
	ui->toggleSourceIcons->setChecked(sourceIconsVisible);

	bool contextVisible = config_get_bool(App()->GetUserConfig(), "BasicWindow", "ShowContextToolbars");
	ui->toggleContextBar->setChecked(contextVisible);
	ui->contextContainer->setVisible(contextVisible);
	if (contextVisible) {
		UpdateContextBar(true);
	}
	UpdateEditMenu();

	{
		ProfileScope("OBSBasic::Load");
		const std::string sceneCollectionName{
			config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};
		std::optional<OBS::SceneCollection> configuredCollection =
			GetSceneCollectionByName(sceneCollectionName);
		std::optional<OBS::SceneCollection> foundCollection = GetSceneCollectionByName(opt_starting_collection);

		if (foundCollection) {
			ActivateSceneCollection(foundCollection.value());
		} else if (configuredCollection) {
			ActivateSceneCollection(configuredCollection.value());
		} else {
			disableSaving--;
			SetupNewSceneCollection(sceneCollectionName);
			disableSaving++;
		}

		if (foundCollection || configuredCollection) {
			disableSaving--;
			OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
			OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED);
			OnEvent(OBS_FRONTEND_EVENT_SCENE_CHANGED);
			OnEvent(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED);
			disableSaving++;
		}
	}

	loaded = true;

	previewEnabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "PreviewEnabled");

	if (!previewEnabled && !IsPreviewProgramMode()) {
		QMetaObject::invokeMethod(this, &OBSBasic::EnablePreviewDisplay, Qt::QueuedConnection, previewEnabled);
	} else if (!previewEnabled && IsPreviewProgramMode()) {
		QMetaObject::invokeMethod(this, &OBSBasic::EnablePreviewDisplay, Qt::QueuedConnection, true);
	}

	disableSaving--;

	auto addDisplay = [this](OBSQTDisplay *window) {
		obs_display_add_draw_callback(window->GetDisplay(), OBSBasic::RenderMain, this);

		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			ResizePreview(ovi.base_width, ovi.base_height);
		}
	};

	connect(ui->preview, &OBSQTDisplay::DisplayCreated, this, addDisplay);

	/* Show the main window, unless the tray icon isn't available
	 * or neither the setting nor flag for starting minimized is set. */
	bool sysTrayEnabled = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayEnabled");
	bool sysTrayWhenStarted = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SysTrayWhenStarted");
	bool hideWindowOnStart = QSystemTrayIcon::isSystemTrayAvailable() && sysTrayEnabled &&
				 (opt_minimize_tray || sysTrayWhenStarted);

#ifdef _WIN32
	SetWin32DropStyle(this);

	if (!hideWindowOnStart) {
		show();
	}
#endif

	bool alwaysOnTop = config_get_bool(App()->GetUserConfig(), "BasicWindow", "AlwaysOnTop");

#ifdef ENABLE_WAYLAND
	bool isWayland = obs_get_nix_platform() == OBS_NIX_PLATFORM_WAYLAND;
#else
	bool isWayland = false;
#endif

	if (!isWayland && (alwaysOnTop || opt_always_on_top)) {
		SetAlwaysOnTop(this, true);
		ui->actionAlwaysOnTop->setChecked(true);
	} else if (isWayland) {
		if (opt_always_on_top) {
			blog(LOG_INFO, "Always On Top not available on Wayland, ignoring.");
		}
		ui->actionAlwaysOnTop->setEnabled(false);
		ui->actionAlwaysOnTop->setVisible(false);
	}

#ifndef _WIN32
	if (!hideWindowOnStart) {
		show();
	}
#endif

	// Set up Audio Mixer dock
	AudioMixer *audioMixer = new AudioMixer(this);
	ui->mixerDock->setWidget(audioMixer);
	ui->mixerDock->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(ui->mixerDock, &QDockWidget::customContextMenuRequested, audioMixer, &AudioMixer::showMixerContextMenu);

	/* setup stats dock */
	OBSBasicStats *statsDlg = new OBSBasicStats(statsDock, false);
	statsDock->setWidget(statsDlg);

	/* ----------------------------- */
	/* add custom browser docks      */
#if defined(BROWSER_AVAILABLE) && defined(YOUTUBE_ENABLED)
	YouTubeAppDock::CleanupYouTubeUrls();
#endif

#ifdef BROWSER_AVAILABLE
	if (cef) {
		QAction *action = new QAction(QTStr("Basic.MainMenu.Docks."
						    "CustomBrowserDocks"),
					      this);
		ui->menuDocks->insertAction(ui->scenesDock->toggleViewAction(), action);
		connect(action, &QAction::triggered, this, &OBSBasic::ManageExtraBrowserDocks);
		ui->menuDocks->insertSeparator(ui->scenesDock->toggleViewAction());

		LoadExtraBrowserDocks();
	}
#endif

#ifdef YOUTUBE_ENABLED
	/* setup YouTube app dock */
	if (YouTubeAppDock::IsYTServiceSelected()) {
		NewYouTubeAppDock();
	}
#endif

	const char *dockStateStr = config_get_string(App()->GetUserConfig(), "BasicWindow", "DockState");

	if (!dockStateStr) {
		on_resetDocks_triggered(true);
	} else {
		QByteArray dockState = QByteArray::fromBase64(QByteArray(dockStateStr));
		if (!restoreState(dockState)) {
			on_resetDocks_triggered(true);
		}
	}

#ifdef __ANDROID__
	/* 3-4 16:10 校形。上面恢复回来的 DockState 是上一轮 saveState() 存的原样，那套比例是给桌面
	 * 键鼠的：1600x1000 实测下排占 325 px（顶它的是控制按钮那一竖列 5 行，本轮已改两列）、
	 * 空混音器还横吃 710 px，预览面因此左右各黑 179 px。
	 * 恢复完再按屏幕尺寸重给一遍。每轮启动都强制 —— Android 上 dock 分隔条拖不动，没有"用户手动值"
	 * 会被覆盖。resizeDocks 会被各 dock 的 minimumSizeHint 顶住，实际落点看下面那条回读。
	 *
	 * 左栏给到 30% 不是审美：命中区抬到 48 dp 以后，场景/源那两排工具栏是 6 个钮 = 288 逻辑像素，
	 * 34i 实测 24%（=265）装不下，**下移按钮被裁掉点不到了**。下排三个 dock 按"左栏剩下的宽度"分
	 * （不是整窗宽）—— 34i 那版按整窗宽算，加起来 992 > 可用的 799，等于让 Qt 自己去归一化，
	 * 落点不可控。比例沿用 34i 实测落点：混音器 51 / 转场 16 / 控制 33。 */
	{
		const int w = width();
		const int h = height();
		const int side = w * 30 / 100;
		const int bottom = h * 20 / 100;
		const int restW = w - side;
		const int mixerW = restW * 51 / 100;
		const int transW = restW * 16 / 100;
		const int controlsW = restW * 33 / 100;
		resizeDocks({ui->scenesDock, ui->sourcesDock}, {side, side}, Qt::Horizontal);
		resizeDocks({ui->mixerDock, ui->transitionsDock, controlsDock},
			    {mixerW, transW, controlsW}, Qt::Horizontal);
		resizeDocks({ui->mixerDock, ui->transitionsDock, controlsDock}, {bottom, bottom, bottom},
			    Qt::Vertical);
		obsAndroidDisplayLog("3-4 校形：窗口 %dx%d → 请求 左栏%d 下排高%d（下排总宽%d 控制%d）", w, h, side,
				     bottom, restW, controlsW);
		QTimer::singleShot(0, this, [this] {
			obsAndroidDisplayLog("3-4 校形：落定 混音器=%dx%d 转场=%dx%d 控制=%dx%d",
					     ui->mixerDock->width(), ui->mixerDock->height(),
					     ui->transitionsDock->width(), ui->transitionsDock->height(),
					     controlsDock->width(), controlsDock->height());
		});
	}

	/* 3-4 触摸命中区。Android 触摸目标下限 48 dp，Qt 在 Android 上的逻辑像素就是 dp（1600x1000 物理
	 * = 1067x667 逻辑，密度 1.5 —— 34h 用 updateRect 的 px/逻辑比反推过）。而 .ui 里
	 * scenesToolbar/sourcesToolbar 的 iconSize 是 16x16，够不上。
	 * 工具按钮用 setMinimumSize 而不是样式表：样式表一沾 QToolButton，主题画的那层底就没了。
	 *
	 * 两个列表的行高走两条路，因为是两套机制：
	 *  - 源列表：SourceTreeDelegate::sizeHint 直接返回行高，样式表盖不过它（34i 实测 30→30 无效），
	 *    那一刀夹在 SourceTreeDelegate.cpp；34j 实测 48 ✓。
	 *  - 场景列表：走默认 QStyledItemDelegate，样式表说了算（34i 实测 25→58 ✓）；34j 回读还是 25，
	 *    量了一下选中行那条蓝带只有 40 物理 px = 27 dp，所以这条得留着。
	 *    已知代价：SceneTree::SetGridMode() 会整个覆盖 widget 样式表，切一次网格模式这条就没了。 */
	{
		/* 只给"真挂在 toolbar 上的动作按钮"设最小尺寸，别用 findChildren<QToolButton*>：
		 * 那还会捞到 QToolBar 的扩展钮（">>"，平时不可见）—— 两排工具栏各 5 个动作，探针却都数到 6，
		 * 多的就是它。给它也塞 48x48 下限，toolbar 算溢出时要替它留出 48 px，真实宽度不够就把
		 * 后面的动作全收进扩展菜单：34k 实测 16:9→16:10 实时改分辨率后，两排从 5 个钮掉到 2 个
		 * （b34k-03-1610-after-rotate.png）。走 actions()+widgetForAction 才只碰得到真按钮。 */
		auto bumpToolbar = [](QToolBar *bar) {
			bar->setIconSize(QSize(24, 24));
			int bumped = 0;
			const auto actions = bar->actions();
			for (QAction *act : actions) {
				if (act->isSeparator())
					continue;
				if (auto *b = qobject_cast<QToolButton *>(bar->widgetForAction(act))) {
					b->setMinimumSize(48, 48);
					++bumped;
				}
			}
			return bumped;
		};
		const int sceneButtons = bumpToolbar(ui->scenesToolbar);
		const int sourceButtons = bumpToolbar(ui->sourcesToolbar);

		ui->scenes->setStyleSheet(ui->scenes->styleSheet() + " QListView::item { min-height: 48px; }");

		obsAndroidDisplayLog("3-4 命中区：动作钮 场景=%d 源=%d → 48dp；行高 场景=%d 源=%d", sceneButtons,
				     sourceButtons, ui->scenes->sizeHintForRow(0), ui->sources->sizeHintForRow(0));
		QTimer::singleShot(0, this, [this] {
			auto visibleCount = [](QToolBar *bar) {
				int n = 0;
				const auto actions = bar->actions();
				for (QAction *act : actions) {
					if (act->isSeparator())
						continue;
					const QWidget *b = bar->widgetForAction(act);
					if (b && b->isVisible())
						++n;
				}
				return n;
			};
			const QToolButton *b = ui->scenesToolbar->findChild<QToolButton *>();
			obsAndroidDisplayLog("3-4 命中区：落定 行高 场景=%d 源=%d 首个工具钮=%dx%d 可见动作钮 场景=%d/%d"
					     " 源=%d/%d 工具栏宽=%d",
					     ui->scenes->sizeHintForRow(0), ui->sources->sizeHintForRow(0),
					     b ? b->width() : -1, b ? b->height() : -1,
					     visibleCount(ui->scenesToolbar), ui->scenesToolbar->actions().size(),
					     visibleCount(ui->sourcesToolbar), ui->sourcesToolbar->actions().size(),
					     ui->scenesToolbar->width());
		});
	}
#endif

	bool pre23Defaults = config_get_bool(App()->GetUserConfig(), "General", "Pre23Defaults");
	if (pre23Defaults) {
		bool resetDockLock23 = config_get_bool(App()->GetUserConfig(), "General", "ResetDockLock23");
		if (!resetDockLock23) {
			config_set_bool(App()->GetUserConfig(), "General", "ResetDockLock23", true);
			config_remove_value(App()->GetUserConfig(), "BasicWindow", "DocksLocked");
			config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
		}
	}

	bool docksLocked = config_get_bool(App()->GetUserConfig(), "BasicWindow", "DocksLocked");
	on_lockDocks_toggled(docksLocked);
	ui->lockDocks->blockSignals(true);
	ui->lockDocks->setChecked(docksLocked);
	ui->lockDocks->blockSignals(false);

	bool sideDocks = config_get_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks");
	ui->sideDocks->setChecked(sideDocks);
	setDockCornersVertical(sideDocks);

	SystemTray(true);

	TaskbarOverlayInit();

#ifdef __APPLE__
	disableColorSpaceConversion(this);
#endif

	bool has_last_version = config_has_user_value(App()->GetAppConfig(), "General", "LastVersion");
	bool first_run = config_get_bool(App()->GetUserConfig(), "General", "FirstRun");

	if (!first_run) {
		config_set_bool(App()->GetUserConfig(), "General", "FirstRun", true);
		config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
	}

	if (!first_run && !has_last_version && !Active()) {
#ifdef __ANDROID__
		/* P-6⑥ 复核（2026-09-06，双 ABI `pm clear` 首启实测）：这条留着，但 P-3 那三条理由只剩一条半，
		   而且这条分支是**头一回被实证真跑过** —— `pm clear` 后那次冷启动日志里正好出现 1 次下面那行、
		   第二次启动归零（FirstRun 已落盘），两帧中心区 PSNR=inf ⇒ 首启没有模态框压住主窗。逐条：
		   ① 仍然成立，且比 P-3 更硬：向导是 `setModal(true)` + `exec()` 的阻塞嵌套事件循环
		      （OBSBasic_MainControls.cpp:663），而下面这行日志打在 `3-3 setVisible(true)` 之前 43 ms（arm64）
		      / 8 ms（x86_64）⇒ 主窗还没露面就会先进嵌套循环，正是 3-3 那一类病灶。
		   ② 只剩一半：「要 RTMP 协议在场」作废（obs-outputs 自 P-5 进包、RTMP 端到端自 P-7 双 ABI 可用）；
		      真的是「服务端测试要打公网」—— `AutoConfig::CanTestServer()`（wizards/AutoConfig.cpp:236）
		      对非 Twitch 一律 return true，会拿已配好的服务真连对端。
		   ③ **原先是错的**：`h264_mediacodec`/`hevc_mediacodec` 在整棵树里不存在，`TestHardwareEncoding()`
		      只认 nvenc/qsv/amf/videotoolbox ⇒ Android 上 `hardwareEncodingAvailable=false`，编码档根本不跑；
		      它真跑起来创建的是 `obs_x264`（AutoConfigTestPage.cpp:121 与 :493），P-1 起就在包里。
		   触发条件 `!first_run && !has_last_version && !Active()` 只在全新配置的那一次成立，但 Android 上
		   `has_last_version` 恒假（AppConfig 落到的 global.ini 是 0 字节）⇒ 实际只有 FirstRun 一道闸：
		   要撤这条，先把向导改成非模态、或把首启入口收掉，否则一个模态 exec() 会挡在还没露面的主窗前面。 */
		blog(LOG_INFO, "Android: skipping auto-configuration wizard on first run");
#else
		QMetaObject::invokeMethod(this, &OBSBasic::on_autoConfigure_triggered, Qt::QueuedConnection);
#endif
	}

#if (defined(_WIN32) || defined(__APPLE__)) && (OBS_RELEASE_CANDIDATE > 0 || OBS_BETA > 0)
	/* Automatically set branch to "beta" the first time a pre-release build is run. */
	if (!config_get_bool(App()->GetAppConfig(), "General", "AutoBetaOptIn")) {
		config_set_string(App()->GetAppConfig(), "General", "UpdateBranch", "beta");
		config_set_bool(App()->GetAppConfig(), "General", "AutoBetaOptIn", true);
		config_save_safe(App()->GetAppConfig(), "tmp", nullptr);
	}
#endif
	TimedCheckForUpdates();

	emit userSettingChanged("BasicWindow", "VerticalVolumeControl");

	if (config_get_bool(activeConfiguration, "General", "OpenStatsOnStartup")) {
		on_stats_triggered();
	}

	OBSBasicStats::InitializeValues();

	/* ----------------------- */
	/* Add multiview menu      */

	ui->viewMenu->addSeparator();

	connect(ui->viewMenu->menuAction(), &QAction::hovered, this, &OBSBasic::updateMultiviewProjectorMenu);
	OBSBasic::updateMultiviewProjectorMenu();

	ui->sources->UpdateIcons();

#if !defined(_WIN32)
	delete ui->actionRepair;
	ui->actionRepair = nullptr;
#if !defined(__APPLE__)
	delete ui->actionShowCrashLogs;
	delete ui->actionUploadLastCrashLog;
	delete ui->menuCrashLogs;
	delete ui->actionCheckForUpdates;
	ui->actionShowCrashLogs = nullptr;
	ui->actionUploadLastCrashLog = nullptr;
	ui->menuCrashLogs = nullptr;
	ui->actionCheckForUpdates = nullptr;
#endif
#endif

#ifdef __APPLE__
	/* Remove OBS' Fullscreen Interface menu in favor of the one macOS adds by default */
	delete ui->actionFullscreenInterface;
	ui->actionFullscreenInterface = nullptr;
#else
	/* Don't show menu to raise macOS-only permissions dialog */
	delete ui->actionShowMacPermissions;
	ui->actionShowMacPermissions = nullptr;
#endif

#if defined(_WIN32) || defined(__APPLE__)
	if (App()->IsUpdaterDisabled()) {
		ui->actionCheckForUpdates->setEnabled(false);
#if defined(_WIN32)
		ui->actionRepair->setEnabled(false);
#endif
	}
#endif

#ifndef WHATSNEW_ENABLED
	delete ui->actionShowWhatsNew;
	ui->actionShowWhatsNew = nullptr;
#endif

	if (safe_mode) {
		ui->actionRestartSafe->setText(QTStr("Basic.MainMenu.Help.RestartNormal"));
	}

	UpdatePreviewProgramIndicators();
	OnFirstLoad();

	if (!hideWindowOnStart) {
		activateWindow();
	}

	/* ------------------------------------------- */
	/* display warning message for failed modules  */

	if (mfi.count) {
		QString failed_plugins;

		char **plugin = mfi.failed_modules;
		while (*plugin) {
			failed_plugins += *plugin;
			failed_plugins += "\n";
			plugin++;
		}

		QString failed_msg = QTStr("PluginsFailedToLoad.Text").arg(failed_plugins);
		OBSMessageBox::warning(this, QTStr("PluginsFailedToLoad.Title"), failed_msg);
	}
}

void OBSBasic::OnFirstLoad()
{
	OnEvent(OBS_FRONTEND_EVENT_FINISHED_LOADING);

#ifdef WHATSNEW_ENABLED
	/* Attempt to load init screen if available */
	if (cef) {
		WhatsNewInfoThread *wnit = new WhatsNewInfoThread();
		connect(wnit, &WhatsNewInfoThread::Result, this, &OBSBasic::ReceivedIntroJson, Qt::QueuedConnection);

		introCheckThread.reset(wnit);
		introCheckThread->start();
	}
#endif

	Auth::Load();

	bool showLogViewerOnStartup = config_get_bool(App()->GetUserConfig(), "LogViewer", "ShowLogStartup");

	if (showLogViewerOnStartup) {
		on_actionViewCurrentLog_triggered();
	}
}

OBSBasic::~OBSBasic() {}

void OBSBasic::applicationShutdown() noexcept
{
	/* clear out UI event queue */
	QApplication::sendPostedEvents(nullptr);
#ifndef __APPLE__
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
#endif

	if (updateCheckThread && updateCheckThread->isRunning()) {
		updateCheckThread->wait();
	}

	if (patronJsonThread && patronJsonThread->isRunning()) {
		patronJsonThread->wait();
	}

	delete screenshotData;
	delete previewProjectorSource;
	delete previewProjectorMain;
	delete sourceProjector;
	delete sceneProjectorMenu;
	delete scaleFilteringMenu;
	delete blendingModeMenu;
	delete colorMenu;
	delete colorWidgetAction;
	delete colorSelect;
	delete deinterlaceMenu;
	delete perSceneTransitionMenu;
	delete shortcutFilter;
	delete programOptions;
	delete program;

	/* XXX: any obs data must be released before calling obs_shutdown.
	 * currently, we can't automate this with C++ RAII because of the
	 * delicate nature of obs_shutdown needing to be freed before the UI
	 * can be freed, and we have no control over the destruction order of
	 * the Qt UI stuff, so we have to manually clear any references to
	 * libobs. */
	delete cpuUsageTimer;
	os_cpu_usage_info_destroy(cpuUsageInfo);

	obs_hotkey_set_callback_routing_func(nullptr, nullptr);
	ClearHotkeys();

	service = nullptr;
	outputHandler.reset();

	delete interaction;
	delete properties;
	delete filters;
	delete transformWindow;
	delete advAudioWindow;
	delete about;
	delete remux;

	obs_display_remove_draw_callback(ui->preview->GetDisplay(), OBSBasic::RenderMain, this);

	obs_enter_graphics();
	gs_vertexbuffer_destroy(box);
	gs_vertexbuffer_destroy(boxLeft);
	gs_vertexbuffer_destroy(boxTop);
	gs_vertexbuffer_destroy(boxRight);
	gs_vertexbuffer_destroy(boxBottom);
	gs_vertexbuffer_destroy(circle);
	gs_vertexbuffer_destroy(actionSafeMargin);
	gs_vertexbuffer_destroy(graphicsSafeMargin);
	gs_vertexbuffer_destroy(fourByThreeSafeMargin);
	gs_vertexbuffer_destroy(leftLine);
	gs_vertexbuffer_destroy(topLine);
	gs_vertexbuffer_destroy(rightLine);
	obs_leave_graphics();

	/* When shutting down, sometimes source references can get in to the
	 * event queue, and if we don't forcibly process those events they
	 * won't get processed until after obs_shutdown has been called.  I
	 * really wish there were a more elegant way to deal with this via C++,
	 * but Qt doesn't use C++ in a normal way, so you can't really rely on
	 * normal C++ behavior for your data to be freed in the order that you
	 * expect or want it to. */
	QApplication::sendPostedEvents(nullptr);

#ifdef BROWSER_AVAILABLE
	DestroyPanelCookieManager();
	delete cef;
	cef = nullptr;
#endif

	handledShutdown = true;
}

void OBSBasic::toggleMixerLayout()
{
	bool vertical = config_get_bool(App()->GetUserConfig(), "BasicWindow", "VerticalVolumeControl");
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "VerticalVolumeControl", !vertical);

	emit userSettingChanged("BasicWindow", "VerticalVolumeControl");
}

static inline int AttemptToResetVideo(struct obs_video_info *ovi)
{
	return obs_reset_video(ovi);
}

static inline enum obs_scale_type GetScaleType(ConfigFile &activeConfiguration)
{
	const char *scaleTypeStr = config_get_string(activeConfiguration, "Video", "ScaleType");

	if (astrcmpi(scaleTypeStr, "bilinear") == 0) {
		return OBS_SCALE_BILINEAR;
	} else if (astrcmpi(scaleTypeStr, "lanczos") == 0) {
		return OBS_SCALE_LANCZOS;
	} else if (astrcmpi(scaleTypeStr, "area") == 0) {
		return OBS_SCALE_AREA;
	} else {
		return OBS_SCALE_BICUBIC;
	}
}

static inline enum video_format GetVideoFormatFromName(const char *name)
{
	if (astrcmpi(name, "I420") == 0) {
		return VIDEO_FORMAT_I420;
	} else if (astrcmpi(name, "NV12") == 0) {
		return VIDEO_FORMAT_NV12;
	} else if (astrcmpi(name, "I444") == 0) {
		return VIDEO_FORMAT_I444;
	} else if (astrcmpi(name, "I010") == 0) {
		return VIDEO_FORMAT_I010;
	} else if (astrcmpi(name, "P010") == 0) {
		return VIDEO_FORMAT_P010;
	} else if (astrcmpi(name, "P216") == 0) {
		return VIDEO_FORMAT_P216;
	} else if (astrcmpi(name, "P416") == 0) {
		return VIDEO_FORMAT_P416;
	}
#if 0 //currently unsupported
	else if (astrcmpi(name, "YVYU") == 0)
		return VIDEO_FORMAT_YVYU;
	else if (astrcmpi(name, "YUY2") == 0)
		return VIDEO_FORMAT_YUY2;
	else if (astrcmpi(name, "UYVY") == 0)
		return VIDEO_FORMAT_UYVY;
#endif
	else {
		return VIDEO_FORMAT_BGRA;
	}
}

static inline enum video_colorspace GetVideoColorSpaceFromName(const char *name)
{
	enum video_colorspace colorspace = VIDEO_CS_SRGB;
	if (strcmp(name, "601") == 0) {
		colorspace = VIDEO_CS_601;
	} else if (strcmp(name, "709") == 0) {
		colorspace = VIDEO_CS_709;
	} else if (strcmp(name, "2100PQ") == 0) {
		colorspace = VIDEO_CS_2100_PQ;
	} else if (strcmp(name, "2100HLG") == 0) {
		colorspace = VIDEO_CS_2100_HLG;
	}

	return colorspace;
}

int OBSBasic::ResetVideo()
{
	if (outputHandler && outputHandler->Active()) {
		return OBS_VIDEO_CURRENTLY_ACTIVE;
	}

	ProfileScope("OBSBasic::ResetVideo");

	struct obs_video_info ovi;
	int ret;

	GetConfigFPS(ovi.fps_num, ovi.fps_den);

	const char *colorFormat = config_get_string(activeConfiguration, "Video", "ColorFormat");
	const char *colorSpace = config_get_string(activeConfiguration, "Video", "ColorSpace");
	const char *colorRange = config_get_string(activeConfiguration, "Video", "ColorRange");

	ovi.graphics_module = App()->GetRenderModule();
	ovi.base_width = (uint32_t)config_get_uint(activeConfiguration, "Video", "BaseCX");
	ovi.base_height = (uint32_t)config_get_uint(activeConfiguration, "Video", "BaseCY");
	ovi.output_width = (uint32_t)config_get_uint(activeConfiguration, "Video", "OutputCX");
	ovi.output_height = (uint32_t)config_get_uint(activeConfiguration, "Video", "OutputCY");
	ovi.output_format = GetVideoFormatFromName(colorFormat);
	ovi.colorspace = GetVideoColorSpaceFromName(colorSpace);
	ovi.range = astrcmpi(colorRange, "Full") == 0 ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	ovi.adapter = config_get_uint(App()->GetUserConfig(), "Video", "AdapterIdx");
	ovi.gpu_conversion = true;
	ovi.scale_type = GetScaleType(activeConfiguration);

	if (ovi.base_width < 32 || ovi.base_height < 32) {
		ovi.base_width = 1920;
		ovi.base_height = 1080;
		config_set_uint(activeConfiguration, "Video", "BaseCX", 1920);
		config_set_uint(activeConfiguration, "Video", "BaseCY", 1080);
	}

	if (ovi.output_width < 32 || ovi.output_height < 32) {
		ovi.output_width = ovi.base_width;
		ovi.output_height = ovi.base_height;
		config_set_uint(activeConfiguration, "Video", "OutputCX", ovi.base_width);
		config_set_uint(activeConfiguration, "Video", "OutputCY", ovi.base_height);
	}

	ret = AttemptToResetVideo(&ovi);
	if (ret == OBS_VIDEO_CURRENTLY_ACTIVE) {
		blog(LOG_WARNING, "Tried to reset when already active");
		return ret;
	}

	if (ret == OBS_VIDEO_SUCCESS) {
		ResizePreview(ovi.base_width, ovi.base_height);
		if (program) {
			ResizeProgram(ovi.base_width, ovi.base_height);
		}

		const float sdr_white_level = (float)config_get_uint(activeConfiguration, "Video", "SdrWhiteLevel");
		const float hdr_nominal_peak_level =
			(float)config_get_uint(activeConfiguration, "Video", "HdrNominalPeakLevel");
		obs_set_video_levels(sdr_white_level, hdr_nominal_peak_level);
		OBSBasicStats::InitializeValues();
		OBSProjector::UpdateMultiviewProjectors();

		if (!collections.empty()) {
			const OBS::SceneCollection currentSceneCollection = OBSBasic::GetCurrentSceneCollection();

			bool usingAbsoluteCoordinates = currentSceneCollection.getCoordinateMode() ==
							OBS::SceneCoordinateMode::Absolute;
			OBS::Rect migrationResolution = currentSceneCollection.getMigrationResolution();

			OBS::Rect videoResolution = OBS::Rect(ovi.base_width, ovi.base_height);

			bool canMigrate = usingAbsoluteCoordinates ||
					  (!migrationResolution.isZero() && migrationResolution != videoResolution);

			ui->actionRemigrateSceneCollection->setEnabled(canMigrate);
		} else {
			ui->actionRemigrateSceneCollection->setEnabled(false);
		}

		emit CanvasResized(ovi.base_width, ovi.base_height);
		emit OutputResized(ovi.output_width, ovi.output_height);
	}

	return ret;
}

bool OBSBasic::ResetAudio()
{
	ProfileScope("OBSBasic::ResetAudio");

	struct obs_audio_info2 ai = {};
	ai.samples_per_sec = config_get_uint(activeConfiguration, "Audio", "SampleRate");

	const char *channelSetupStr = config_get_string(activeConfiguration, "Audio", "ChannelSetup");

	if (strcmp(channelSetupStr, "Mono") == 0) {
		ai.speakers = SPEAKERS_MONO;
	} else if (strcmp(channelSetupStr, "2.1") == 0) {
		ai.speakers = SPEAKERS_2POINT1;
	} else if (strcmp(channelSetupStr, "4.0") == 0) {
		ai.speakers = SPEAKERS_4POINT0;
	} else if (strcmp(channelSetupStr, "4.1") == 0) {
		ai.speakers = SPEAKERS_4POINT1;
	} else if (strcmp(channelSetupStr, "5.1") == 0) {
		ai.speakers = SPEAKERS_5POINT1;
	} else if (strcmp(channelSetupStr, "7.1") == 0) {
		ai.speakers = SPEAKERS_7POINT1;
	} else {
		ai.speakers = SPEAKERS_STEREO;
	}

	bool lowLatencyAudioBuffering = config_get_bool(App()->GetUserConfig(), "Audio", "LowLatencyAudioBuffering");
	if (lowLatencyAudioBuffering) {
		ai.max_buffering_ms = 20;
		ai.fixed_buffering = true;
	}

	return obs_reset_audio2(&ai);
}

void OBSBasic::close()
{
	if (isClosePromptOpen() || isClosing()) {
		return;
	}

	OBSMainWindow::close();
}

void OBSBasic::closeEvent(QCloseEvent *event)
{
	if (isClosePromptOpen() || isClosing()) {
		return;
	}

	if (isVisible()) {
		config_set_string(App()->GetUserConfig(), "BasicWindow", "geometry",
				  saveGeometry().toBase64().constData());
	}

	if (!isReadyToClose()) {
		event->ignore();

		QTimer::singleShot(1000, this, &OBSBasic::close);
		return;
	}

	if (remux && !remux->close()) {
		event->ignore();
		restart = false;

		return;
	}

	if (shouldPromptForClose()) {
		event->ignore();
		restart = false;

		if (!isClosePromptOpen()) {
			bool shouldClose = promptToClose();

			if (shouldClose) {
				closeWindow();
			}
		}

		return;
	}

	QWidget::closeEvent(event);
	if (!event->isAccepted()) {
		return;
	}

	closeWindow();
}

bool OBSBasic::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG &msg = *static_cast<MSG *>(message);
	switch (msg.message) {
	case WM_MOVE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnMove();
		}
		break;
	case WM_DISPLAYCHANGE:
		for (OBSQTDisplay *const display : findChildren<OBSQTDisplay *>()) {
			display->OnDisplayChange();
		}
	}
#else
	UNUSED_PARAMETER(message);
#endif

	return false;
}

void OBSBasic::changeEvent(QEvent *event)
{
	if (event->type() == QEvent::WindowStateChange) {
		QWindowStateChangeEvent *stateEvent = (QWindowStateChangeEvent *)event;

		if (isMinimized()) {
			if (trayIcon && trayIcon->isVisible() && sysTrayMinimizeToTray()) {
				ToggleShowHide();
				return;
			}

			if (previewEnabled) {
				EnablePreviewDisplay(false);
			}
		} else if (stateEvent->oldState() & Qt::WindowMinimized && isVisible()) {
			if (previewEnabled) {
				EnablePreviewDisplay(true);
			}
		}
	}
}

void OBSBasic::GetFPSCommon(uint32_t &num, uint32_t &den) const
{
	const char *val = config_get_string(activeConfiguration, "Video", "FPSCommon");

	if (strcmp(val, "10") == 0) {
		num = 10;
		den = 1;
	} else if (strcmp(val, "20") == 0) {
		num = 20;
		den = 1;
	} else if (strcmp(val, "24 NTSC") == 0) {
		num = 24000;
		den = 1001;
	} else if (strcmp(val, "25 PAL") == 0) {
		num = 25;
		den = 1;
	} else if (strcmp(val, "29.97") == 0) {
		num = 30000;
		den = 1001;
	} else if (strcmp(val, "48") == 0) {
		num = 48;
		den = 1;
	} else if (strcmp(val, "50 PAL") == 0) {
		num = 50;
		den = 1;
	} else if (strcmp(val, "59.94") == 0) {
		num = 60000;
		den = 1001;
	} else if (strcmp(val, "60") == 0) {
		num = 60;
		den = 1;
	} else {
		num = 30;
		den = 1;
	}
}

void OBSBasic::GetFPSInteger(uint32_t &num, uint32_t &den) const
{
	num = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSInt");
	den = 1;
}

void OBSBasic::GetFPSFraction(uint32_t &num, uint32_t &den) const
{
	num = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSNum");
	den = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSDen");
}

void OBSBasic::GetFPSNanoseconds(uint32_t &num, uint32_t &den) const
{
	num = 1000000000;
	den = (uint32_t)config_get_uint(activeConfiguration, "Video", "FPSNS");
}

void OBSBasic::GetConfigFPS(uint32_t &num, uint32_t &den) const
{
	uint32_t type = config_get_uint(activeConfiguration, "Video", "FPSType");

	if (type == 1) { //"Integer"
		GetFPSInteger(num, den);
	} else if (type == 2) { //"Fraction"
		GetFPSFraction(num, den);
	}
	/*
	 * 	else if (false) //"Nanoseconds", currently not implemented
	 *		GetFPSNanoseconds(num, den);
	 */
	else {
		GetFPSCommon(num, den);
	}
}

config_t *OBSBasic::Config() const
{
	return activeConfiguration;
}

void OBSBasic::saveAll()
{
	if (isVisible()) {
		config_set_string(App()->GetUserConfig(), "BasicWindow", "geometry",
				  saveGeometry().toBase64().constData());
	}

	std::call_once(saveOnceFlag, [this]() {
		Auth::Save();
		SaveProjectNow();

		config_set_string(App()->GetUserConfig(), "BasicWindow", "DockState",
				  saveState().toBase64().constData());

#ifdef BROWSER_AVAILABLE
		if (cef) {
			SaveExtraBrowserDocks();
		}
#endif
	});

	config_set_int(App()->GetAppConfig(), "General", "LastVersion", LIBOBS_API_VER);
	config_save_safe(App()->GetAppConfig(), "tmp", nullptr);

	config_set_bool(App()->GetUserConfig(), "BasicWindow", "PreviewEnabled", previewEnabled);
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "AlwaysOnTop", ui->actionAlwaysOnTop->isChecked());
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SceneDuplicationMode", sceneDuplicationMode);
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SwapScenesMode", swapScenesMode);
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "EditPropertiesMode", editPropertiesMode);
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "PreviewProgramMode", IsPreviewProgramMode());
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "DocksLocked", ui->lockDocks->isChecked());
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks", ui->sideDocks->isChecked());
	config_save_safe(App()->GetUserConfig(), "tmp", nullptr);
}

bool OBSBasic::isReadyToClose()
{
	/* Wait for multitrack video stream to start/finish processing in the background */
	if (setupStreamingGuard.valid() &&
	    setupStreamingGuard.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
		return false;
	}

	/* Do not close window if inside of a temporary event loop because we
	 * could be inside of an Auth::LoadUI call.  Keep trying once per
	 * second until we've exit any known sub-loops. */
	if (os_atomic_load_long(&insideEventLoop) != 0) {
		return false;
	}

#ifdef YOUTUBE_ENABLED
	/* Also don't close the window if the youtube stream check is active */
	if (youtubeStreamCheckThread) {
		return false;
	}
#endif

	return true;
}

bool OBSBasic::shouldPromptForClose()
{
	bool confirmOnExit = config_get_bool(App()->GetUserConfig(), "General", "ConfirmOnExit");
	if (confirmOnExit && outputHandler && outputHandler->Active() && !clearingFailed) {
		return true;
	}

	return false;
}

bool OBSBasic::promptToClose()
{
	isClosePromptOpen_ = true;

	SetShowing(true);
	QMessageBox::StandardButton button =
		OBSMessageBox::question(this, QTStr("ConfirmExit.Title"), QTStr("ConfirmExit.Text"),
					QMessageBox::StandardButtons(QMessageBox::Ok | QMessageBox::Cancel));

	if (button == QMessageBox::Cancel) {
		isClosePromptOpen_ = false;
		return false;
	}

	isClosePromptOpen_ = false;
	return true;
}

void OBSBasic::closeWindow()
{
	if (isClosing()) {
		return;
	}

	blog(LOG_INFO, SHUTDOWN_SEPARATOR);

	isClosing_ = true;

	/* While closing, a resize event to OBSQTDisplay could be triggered.
	 * The graphics thread on macOS dispatches a lambda function to be
	 * executed asynchronously in the main thread. However, the display is
	 * sometimes deleted before the lambda function is actually executed.
	 * To avoid such a case, destroy displays earlier than others such as
	 * deleting browser docks. */
	ui->preview->DestroyDisplay();
	if (program) {
		program->DestroyDisplay();
	}

	if (outputHandler->VirtualCamActive()) {
		outputHandler->StopVirtualCam();
	}

	if (introCheckThread) {
		introCheckThread->wait();
	}
	if (whatsNewInitThread) {
		whatsNewInitThread->wait();
	}
	if (updateCheckThread) {
		updateCheckThread->wait();
	}
	if (logUploadThread) {
		logUploadThread->wait();
	}
	if (devicePropertiesThread && devicePropertiesThread->isRunning()) {
		devicePropertiesThread->wait();
		devicePropertiesThread.reset();
	}

	QApplication::sendPostedEvents(nullptr);

	signalHandlers.clear();
	delete extraBrowsers;

	saveAll();

	auth.reset();

#ifdef BROWSER_AVAILABLE
	ClearExtraBrowserDocks();

#ifdef YOUTUBE_ENABLED
	if (youtubeAppDock) {
		RemoveDockWidget(youtubeAppDock->objectName());
		youtubeAppDock = nullptr;
	}
#endif
#endif

	OnEvent(OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN);

	disableSaving++;

	/* Clear all scene data (dialogs, widgets, widget sub-items, scenes,
	 * sources, etc) so that all references are released before shutdown */
	ClearSceneData();

	OnEvent(OBS_FRONTEND_EVENT_EXIT);

	// Destroys the frontend API so plugins can't continue calling it
	obs_frontend_set_callbacks_internal(nullptr);
	api = nullptr;

	applicationShutdown();

#ifndef __APPLE__
	deleteLater();

	emit mainWindowClosed();

	QMetaObject::invokeMethod(App(), &OBSApp::quit, Qt::QueuedConnection);
#else
	QMetaObject::invokeMethod(App(), &OBSApp::quit, Qt::QueuedConnection);
#endif
}

void OBSBasic::UpdateEditMenu()
{
	QModelIndexList items = GetAllSelectedSourceItems();
	int totalCount = items.count();
	size_t filter_count = 0;

	if (totalCount == 1) {
		OBSSceneItem sceneItem = ui->sources->Get(GetTopSelectedSourceItem());
		OBSSource source = obs_sceneitem_get_source(sceneItem);
		filter_count = obs_source_filter_count(source);
	}

	int videoCount = 0;
	bool canTransformMultiple = false;
	for (int i = 0; i < totalCount; i++) {
		OBSSceneItem item = ui->sources->Get(items.value(i).row());
		OBSSource source = obs_sceneitem_get_source(item);
		const uint32_t flags = obs_source_get_output_flags(source);
		const bool hasVideo = (flags & OBS_SOURCE_VIDEO) != 0;
		if (hasVideo && !obs_sceneitem_locked(item)) {
			canTransformMultiple = true;
		}

		if (hasVideo) {
			videoCount++;
		}
	}
	const bool canTransformSingle = videoCount == 1 && totalCount == 1;

	OBSSceneItem curItem = GetCurrentSceneItem();
	bool locked = curItem && obs_sceneitem_locked(curItem);

	ui->actionCopySource->setEnabled(totalCount > 0);
	ui->actionEditTransform->setEnabled(canTransformSingle && !locked);
	ui->actionCopyTransform->setEnabled(canTransformSingle);
	ui->actionPasteTransform->setEnabled(canTransformMultiple && hasCopiedTransform && videoCount > 0);
	ui->actionCopyFilters->setEnabled(filter_count > 0);
	ui->actionPasteFilters->setEnabled(!obs_weak_source_expired(copyFiltersSource()) && totalCount > 0);
	auto pasteType = getItemPasteType();
	ui->actionPasteRef->setEnabled(pasteType == OBS::ItemPasteType::Reference ||
				       pasteType == OBS::ItemPasteType::Both);
	ui->actionPasteDup->setEnabled(pasteType == OBS::ItemPasteType::Duplicate ||
				       pasteType == OBS::ItemPasteType::Both);

	ui->actionMoveUp->setEnabled(totalCount > 0);
	ui->actionMoveDown->setEnabled(totalCount > 0);
	ui->actionMoveToTop->setEnabled(totalCount > 0);
	ui->actionMoveToBottom->setEnabled(totalCount > 0);

	ui->actionResetTransform->setEnabled(canTransformMultiple);
	ui->actionRotate90CW->setEnabled(canTransformMultiple);
	ui->actionRotate90CCW->setEnabled(canTransformMultiple);
	ui->actionRotate180->setEnabled(canTransformMultiple);
	ui->actionFlipHorizontal->setEnabled(canTransformMultiple);
	ui->actionFlipVertical->setEnabled(canTransformMultiple);
	ui->actionFitToScreen->setEnabled(canTransformMultiple);
	ui->actionStretchToScreen->setEnabled(canTransformMultiple);
	ui->actionCenterToScreen->setEnabled(canTransformMultiple);
	ui->actionVerticalCenter->setEnabled(canTransformMultiple);
	ui->actionHorizontalCenter->setEnabled(canTransformMultiple);
}

void OBSBasic::UpdateTitleBar()
{
	stringstream name;

	const char *profile = config_get_string(App()->GetUserConfig(), "Basic", "Profile");
	const char *sceneCollection = config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection");

	name << "OBS ";
	if (previewProgramMode) {
		name << "Studio ";
	}

	name << App()->GetVersionString(false);
	if (safe_mode) {
		name << " (" << Str("TitleBar.SafeMode") << ")";
	}
	if (App()->IsPortableMode()) {
		name << " - " << Str("TitleBar.PortableMode");
	}

	name << " - " << Str("TitleBar.Profile") << ": " << profile;
	name << " - " << Str("TitleBar.Scenes") << ": " << sceneCollection;

	setWindowTitle(QT_UTF8(name.str().c_str()));
}

OBSBasic *OBSBasic::Get()
{
	return reinterpret_cast<OBSBasic *>(App()->GetMainWindow());
}

void OBSBasic::UpdatePatronJson(const std::string &text, const std::string &error)
{
	if (!error.empty()) {
		return;
	}

	patronJson = text;
}

void OBSBasic::SetDisplayAffinity(QWindow *window)
{
	if (!SetDisplayAffinitySupported()) {
		return;
	}

	bool hideFromCapture = config_get_bool(App()->GetUserConfig(), "BasicWindow", "HideOBSWindowsFromCapture");

	// Don't hide projectors, those are designed to be visible / captured
	if (window->property("isOBSProjectorWindow") == true) {
		return;
	}

#ifdef _WIN32
	HWND hwnd = (HWND)window->winId();

	DWORD curAffinity;
	if (GetWindowDisplayAffinity(hwnd, &curAffinity)) {
		if (hideFromCapture && curAffinity != WDA_EXCLUDEFROMCAPTURE) {
			SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
		} else if (!hideFromCapture && curAffinity != WDA_NONE) {
			SetWindowDisplayAffinity(hwnd, WDA_NONE);
		}
	}

#else
	// TODO: Implement for other platforms if possible. Don't forget to
	// implement SetDisplayAffinitySupported too!
	UNUSED_PARAMETER(hideFromCapture);
#endif
}

void OBSBasic::OnEvent(enum obs_frontend_event event)
{
	if (api) {
		api->on_event(event);
	}
}

OBSPromptResult OBSBasic::PromptForName(const OBSPromptRequest &request, const OBSPromptCallback &callback)
{
	OBSPromptResult result;

	for (;;) {
		result.success = false;

		if (request.withOption && !request.optionPrompt.empty()) {
			result.optionValue = request.optionValue;

			result.success = NameDialog::AskForNameWithOption(
				this, request.title.c_str(), request.prompt.c_str(), result.promptValue,
				request.optionPrompt.c_str(), result.optionValue,
				(request.promptValue.empty() ? nullptr : request.promptValue.c_str()));

		} else {
			result.success = NameDialog::AskForName(
				this, request.title.c_str(), request.prompt.c_str(), result.promptValue,
				(request.promptValue.empty() ? nullptr : request.promptValue.c_str()));
		}

		if (!result.success) {
			break;
		}

		if (result.promptValue.empty()) {
			OBSMessageBox::warning(this, QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
			continue;
		}

		if (!callback(result)) {
			OBSMessageBox::warning(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
			continue;
		}

		break;
	}

	return result;
}

void OBSBasic::on_actionOpenPluginManager_triggered()
{
	App()->pluginManagerOpenDialog();
}
