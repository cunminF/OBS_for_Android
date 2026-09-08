/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

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

#include "OBSApp.hpp"

#include <components/Multiview.hpp>
#include <dialogs/LogUploadDialog.hpp>
#include <plugin-manager/PluginManager.hpp>
#include <utility/CrashHandler.hpp>
#include <utility/OBSEventFilter.hpp>
#include <utility/OBSProxyStyle.hpp>
#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
#include <utility/models/branches.hpp>
#endif
#include <widgets/OBSBasic.hpp>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
#include <qpa/qplatformnativeinterface.h>
#endif
#endif
#include <qt-wrappers.hpp>

#include <QCheckBox>
#include <QDesktopServices>
#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
#include <QFile>
#endif

#include <QSessionManager>
#ifndef _WIN32
#include <QSocketNotifier>
#endif

#ifdef __ANDROID__
#include "widgets/OBSAndroidDisplay.hpp" // 3-3 模态遮挡：obsAndroidDisplaySetVisible
#include "widgets/OBSAndroidPath.hpp" // 3-4：openUrl(本地路径) 的 Android 替代
#include <QAbstractScrollArea> // 3-4 触摸：长按坐标要按 viewport 系给（itemAt/exec 都吃这个系）
#include <QComboBox> // P-8 触摸：组合框弹层探针要认出按下落在的是不是 QComboBox
#include <QContextMenuEvent> // 3-4 触摸：长按合成右键
#include <QGestureEvent> // P-8 探针：认出 TapAndHold 手势
#include <QElapsedTimer>
#include <QTimer>
#include <android/log.h>
// P-1 装载轨迹：APK 不是 debuggable，blog() 只进日志文件不进 logcat，实测看不看得到全靠这条。
#define P1LOG(...) __android_log_print(ANDROID_LOG_INFO, "P1-LOAD", __VA_ARGS__)
#include <android-plugin-modules.h> // 构建期由 plugins/ 白名单生成
#endif

#include <chrono>

#ifdef _WIN32
#include <sstream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#endif

#include "moc_OBSApp.cpp"

using namespace std;

string currentLogFile;
string lastLogFile;
string lastCrashLogFile;

extern bool portable_mode;
extern bool safe_mode;
extern bool multi;
extern bool disable_3p_plugins;
extern bool opt_disable_updater;
extern bool opt_disable_missing_files_check;
extern string opt_starting_collection;
extern string opt_starting_profile;

// GPU hint exports for AMD/NVIDIA laptops
#ifdef _MSC_VER
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 1;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

namespace {

typedef struct UncleanLaunchAction {
	bool useSafeMode = false;
	bool sendCrashReport = false;
} UncleanLaunchAction;

UncleanLaunchAction handleUncleanShutdown(bool enableCrashUpload)
{
	UncleanLaunchAction launchAction;

	blog(LOG_WARNING, "Crash or unclean shutdown detected");

#ifdef __ANDROID__
	/* 永久分支（P-3 定案，不进回退清单）：Android 上进程被系统随时回收是常态，"没留干净退出标记"
	   不能当成崩溃证据 ⇒ 这个模态框会在几乎每次自主启动时挡住主窗口。这里不弹框，直接按"正常启动"
	   返回（launchAction 默认 useSafeMode=false、sendCrashReport=false）。
	   注：早期理由里还有一句"阶段 4 前无插件可禁用、safe mode 无意义"，那条已随白名单推进失效 ——
	   留着这分支靠的是上面那条平台事实，不是阶段进度。 */
	blog(LOG_WARNING, "Android: unclean-shutdown marker is meaningless; launching normally");
	Q_UNUSED(enableCrashUpload);
#else
	QMessageBox crashWarning;

	crashWarning.setIcon(QMessageBox::Warning);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
	crashWarning.setOption(QMessageBox::Option::DontUseNativeDialog);
#endif
	crashWarning.setWindowTitle(QTStr("CrashHandling.Dialog.Title"));
	crashWarning.setText(QTStr("CrashHandling.Labels.Text"));

	if (enableCrashUpload) {
		crashWarning.setInformativeText(QTStr("CrashHandling.Labels.PrivacyNotice"));

		QCheckBox *sendCrashReportCheckbox = new QCheckBox(QTStr("CrashHandling.Checkbox.SendReport"));
		crashWarning.setCheckBox(sendCrashReportCheckbox);
	}

	QPushButton *launchSafeButton =
		crashWarning.addButton(QTStr("CrashHandling.Buttons.LaunchSafe"), QMessageBox::AcceptRole);
	QPushButton *launchNormalButton =
		crashWarning.addButton(QTStr("CrashHandling.Buttons.LaunchNormal"), QMessageBox::RejectRole);

	crashWarning.setDefaultButton(launchNormalButton);

	crashWarning.exec();

	bool useSafeMode = crashWarning.clickedButton() == launchSafeButton;

	if (useSafeMode) {
		launchAction.useSafeMode = true;

		blog(LOG_INFO, "[Safe Mode] Safe mode launch selected, loading third-party plugins is disabled");
	} else {
		blog(LOG_WARNING, "[Safe Mode] Normal launch selected, loading third-party plugins is enabled");
	}

	bool sendCrashReport = (enableCrashUpload) ? crashWarning.checkBox()->isChecked() : false;

	if (sendCrashReport) {
		launchAction.sendCrashReport = true;

		blog(LOG_INFO, "User selected to send crash report");
	}
#endif

	return launchAction;
}

QAccessibleInterface *alignmentSelectorFactory(const QString &classname, QObject *object)
{
	if (classname == QLatin1String("AlignmentSelector")) {
		if (auto *w = qobject_cast<AlignmentSelector *>(object)) {
			return new AccessibleAlignmentSelector(w);
		}
	}
	return nullptr;
}
} // namespace

QObject *CreateShortcutFilter()
{
	return new OBSEventFilter([](QObject *obj, QEvent *event) {
		auto mouse_event = [](QMouseEvent &event) {
			if (!App()->HotkeysEnabledInFocus() && event.button() != Qt::LeftButton) {
				return true;
			}

			obs_key_combination_t hotkey = {0, OBS_KEY_NONE};
			bool pressed = event.type() == QEvent::MouseButtonPress;

			switch (event.button()) {
			case Qt::NoButton:
			case Qt::LeftButton:
			case Qt::RightButton:
			case Qt::AllButtons:
			case Qt::MouseButtonMask:
				return false;

			case Qt::MiddleButton:
				hotkey.key = OBS_KEY_MOUSE3;
				break;

#define MAP_BUTTON(i, j)                       \
	case Qt::ExtraButton##i:               \
		hotkey.key = OBS_KEY_MOUSE##j; \
		break;
				MAP_BUTTON(1, 4);
				MAP_BUTTON(2, 5);
				MAP_BUTTON(3, 6);
				MAP_BUTTON(4, 7);
				MAP_BUTTON(5, 8);
				MAP_BUTTON(6, 9);
				MAP_BUTTON(7, 10);
				MAP_BUTTON(8, 11);
				MAP_BUTTON(9, 12);
				MAP_BUTTON(10, 13);
				MAP_BUTTON(11, 14);
				MAP_BUTTON(12, 15);
				MAP_BUTTON(13, 16);
				MAP_BUTTON(14, 17);
				MAP_BUTTON(15, 18);
				MAP_BUTTON(16, 19);
				MAP_BUTTON(17, 20);
				MAP_BUTTON(18, 21);
				MAP_BUTTON(19, 22);
				MAP_BUTTON(20, 23);
				MAP_BUTTON(21, 24);
				MAP_BUTTON(22, 25);
				MAP_BUTTON(23, 26);
				MAP_BUTTON(24, 27);
#undef MAP_BUTTON
			}

			hotkey.modifiers = TranslateQtKeyboardEventModifiers(event.modifiers());

			obs_hotkey_inject_event(hotkey, pressed);
			return true;
		};

		auto key_event = [&](QKeyEvent *event) {
			int key = event->key();
			bool enabledInFocus = App()->HotkeysEnabledInFocus();

			if (key != Qt::Key_Enter && key != Qt::Key_Escape && key != Qt::Key_Return && !enabledInFocus)
				return true;

			QDialog *dialog = qobject_cast<QDialog *>(obj);

			obs_key_combination_t hotkey = {0, OBS_KEY_NONE};
			bool pressed = event->type() == QEvent::KeyPress;

			switch (key) {
			case Qt::Key_Shift:
			case Qt::Key_Control:
			case Qt::Key_Alt:
			case Qt::Key_Meta:
				break;

#ifdef __APPLE__
			case Qt::Key_CapsLock:
				// kVK_CapsLock == 57
				hotkey.key = obs_key_from_virtual_key(57);
				pressed = true;
				break;
#endif

			case Qt::Key_Enter:
			case Qt::Key_Escape:
			case Qt::Key_Return:
				if (dialog && pressed)
					return false;
				if (!enabledInFocus)
					return true;
				/* Falls through. */
			default:
				hotkey.key = obs_key_from_virtual_key(event->nativeVirtualKey());
			}

			if (event->isAutoRepeat())
				return true;

			hotkey.modifiers = TranslateQtKeyboardEventModifiers(event->modifiers());

			obs_hotkey_inject_event(hotkey, pressed);
			return true;
		};

		switch (event->type()) {
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonRelease:
			return mouse_event(*static_cast<QMouseEvent *>(event));

		/*case QEvent::MouseButtonDblClick:
		case QEvent::Wheel:*/
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
			return key_event(static_cast<QKeyEvent *>(event));

		default:
			return false;
		}
	});
}

string CurrentDateTimeString()
{
	time_t now = time(0);
	struct tm tstruct;
	char buf[80];
	tstruct = *localtime(&now);
	strftime(buf, sizeof(buf), "%Y-%m-%d, %X", &tstruct);
	return buf;
}

#define DEFAULT_LANG "en-US"

#ifndef _WIN32
std::array<int, 2> OBSApp::sigIntFileDescriptor{0, 0};
std::array<int, 2> OBSApp::sigTermFileDescriptor{0, 0};
std::array<int, 2> OBSApp::sigAbrtFileDescriptor{0, 0};
std::array<int, 2> OBSApp::sigQuitFileDescriptor{0, 0};
#endif

bool OBSApp::InitGlobalConfigDefaults()
{
	config_set_default_uint(appConfig, "General", "MaxLogs", 10);
	config_set_default_int(appConfig, "General", "InfoIncrement", -1);
	config_set_default_string(appConfig, "General", "ProcessPriority", "Normal");
	config_set_default_bool(appConfig, "General", "EnableAutoUpdates", true);

#if _WIN32
	config_set_default_string(appConfig, "Video", "Renderer", "Direct3D 11");
#else
#if defined(__APPLE__) && defined(__aarch64__)
	// TODO: Change this value to "Metal" once the renderer has reached production quality
	config_set_default_string(appConfig, "Video", "Renderer", "OpenGL");
#else
	config_set_default_string(appConfig, "Video", "Renderer", "OpenGL");
#endif
#endif

#ifdef _WIN32
	config_set_default_bool(appConfig, "Audio", "DisableAudioDucking", true);
#endif

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
	config_set_default_bool(appConfig, "General", "BrowserHWAccel", true);
#endif

#ifdef __APPLE__
	config_set_default_bool(appConfig, "Video", "DisableOSXVSync", true);
	config_set_default_bool(appConfig, "Video", "ResetOSXVSyncOnExit", true);
#endif

	return true;
}

bool OBSApp::InitGlobalLocationDefaults()
{
	char path[512];

	int len = GetAppConfigPath(path, sizeof(path), nullptr);
	if (len <= 0) {
		OBSErrorBox(NULL, "Unable to get global configuration path.");
		return false;
	}

	config_set_default_string(appConfig, "Locations", "Configuration", path);
	config_set_default_string(appConfig, "Locations", "SceneCollections", path);
	config_set_default_string(appConfig, "Locations", "Profiles", path);
	config_set_default_string(appConfig, "Locations", "PluginManagerSettings", path);

	return true;
}

void OBSApp::InitUserConfigDefaults()
{
	config_set_default_bool(userConfig, "General", "ConfirmOnExit", true);

	config_set_default_string(userConfig, "General", "HotkeyFocusType", "NeverDisableHotkeys");

	config_set_default_bool(userConfig, "BasicWindow", "PreviewEnabled", true);
	config_set_default_bool(userConfig, "BasicWindow", "PreviewProgramMode", false);
	config_set_default_bool(userConfig, "BasicWindow", "SceneDuplicationMode", true);
	config_set_default_bool(userConfig, "BasicWindow", "SwapScenesMode", true);
	config_set_default_bool(userConfig, "BasicWindow", "SnappingEnabled", true);
	config_set_default_bool(userConfig, "BasicWindow", "ScreenSnapping", true);
	config_set_default_bool(userConfig, "BasicWindow", "SourceSnapping", true);
	config_set_default_bool(userConfig, "BasicWindow", "CenterSnapping", false);
	config_set_default_double(userConfig, "BasicWindow", "SnapDistance", 10.0);
	config_set_default_bool(userConfig, "BasicWindow", "SpacingHelpersEnabled", true);
	config_set_default_bool(userConfig, "BasicWindow", "RecordWhenStreaming", false);
	config_set_default_bool(userConfig, "BasicWindow", "KeepRecordingWhenStreamStops", false);
	config_set_default_bool(userConfig, "BasicWindow", "SysTrayEnabled", true);
	config_set_default_bool(userConfig, "BasicWindow", "SysTrayWhenStarted", false);
	config_set_default_bool(userConfig, "BasicWindow", "SaveProjectors", false);
	config_set_default_bool(userConfig, "BasicWindow", "ShowTransitions", true);
	config_set_default_bool(userConfig, "BasicWindow", "ShowListboxToolbars", true);
	config_set_default_bool(userConfig, "BasicWindow", "ShowStatusBar", true);
	config_set_default_bool(userConfig, "BasicWindow", "ShowSourceIcons", true);
	config_set_default_bool(userConfig, "BasicWindow", "ShowContextToolbars", true);
	config_set_default_bool(userConfig, "BasicWindow", "StudioModeLabels", true);
	config_set_default_bool(userConfig, "BasicWindow", "SideDocks", true);

	config_set_default_bool(userConfig, "BasicWindow", "VerticalVolumeControl", true);

	config_set_default_bool(userConfig, "BasicWindow", "MultiviewMouseSwitch", true);

	config_set_default_bool(userConfig, "BasicWindow", "MultiviewDrawNames", true);

	config_set_default_bool(userConfig, "BasicWindow", "MultiviewDrawAreas", true);

	config_set_default_bool(userConfig, "BasicWindow", "MediaControlsCountdownTimer", true);

	config_set_default_bool(App()->GetUserConfig(), "BasicWindow", "MixerShowInactive", false);
	config_set_default_bool(App()->GetUserConfig(), "BasicWindow", "MixerKeepInactiveLast", false);
	config_set_default_bool(App()->GetUserConfig(), "BasicWindow", "MixerShowHidden", false);
	config_set_default_bool(App()->GetUserConfig(), "BasicWindow", "MixerKeepHiddenLast", false);

	config_set_default_int(userConfig, "Appearance", "FontScale", 10);
	config_set_default_int(userConfig, "Appearance", "Density", 1);
}

static bool do_mkdir(const char *path)
{
	if (os_mkdirs(path) == MKDIR_ERROR) {
		OBSErrorBox(NULL, "Failed to create directory %s", path);
		return false;
	}

	return true;
}

static bool MakeUserDirs()
{
	char path[512];

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/basic") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/logs") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/profiler_data") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

#ifdef _WIN32
	if (GetAppConfigPath(path, sizeof(path), "obs-studio/crashes") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}
#endif

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/updates") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/plugin_config") <= 0) {
		return false;
	}
	if (!do_mkdir(path)) {
		return false;
	}

	return true;
}

constexpr std::string_view OBSProfileSubDirectory = "obs-studio/basic/profiles";
constexpr std::string_view OBSScenesSubDirectory = "obs-studio/basic/scenes";
constexpr std::string_view OBSPluginManagerSubDirectory = "obs-studio/plugin_manager";

static bool MakeUserProfileDirs()
{
	const std::filesystem::path userProfilePath =
		App()->userProfilesLocation / std::filesystem::u8path(OBSProfileSubDirectory);
	const std::filesystem::path userScenesPath =
		App()->userScenesLocation / std::filesystem::u8path(OBSScenesSubDirectory);
	const std::filesystem::path userPluginManagerPath =
		App()->userPluginManagerSettingsLocation / std::filesystem::u8path(OBSPluginManagerSubDirectory);

	if (!std::filesystem::exists(userProfilePath)) {
		try {
			std::filesystem::create_directories(userProfilePath);
		} catch (const std::filesystem::filesystem_error &error) {
			blog(LOG_ERROR, "Failed to create user profile directory '%s'\n%s",
			     userProfilePath.u8string().c_str(), error.what());
			return false;
		}
	}

	if (!std::filesystem::exists(userScenesPath)) {
		try {
			std::filesystem::create_directories(userScenesPath);
		} catch (const std::filesystem::filesystem_error &error) {
			blog(LOG_ERROR, "Failed to create user scene collection directory '%s'\n%s",
			     userScenesPath.u8string().c_str(), error.what());
			return false;
		}
	}

	if (!std::filesystem::exists(userPluginManagerPath)) {
		try {
			std::filesystem::create_directories(userPluginManagerPath);
		} catch (const std::filesystem::filesystem_error &error) {
			blog(LOG_ERROR, "Failed to create user plugin manager directory '%s'\n%s",
			     userPluginManagerPath.u8string().c_str(), error.what());
			return false;
		}
	}

	return true;
}

bool OBSApp::UpdatePre22MultiviewLayout(const char *layout)
{
	if (!layout) {
		return false;
	}

	if (astrcmpi(layout, "horizontaltop") == 0) {
		config_set_int(userConfig, "BasicWindow", "MultiviewLayout",
			       static_cast<int>(MultiviewLayout::HORIZONTAL_TOP_8_SCENES));
		return true;
	}

	if (astrcmpi(layout, "horizontalbottom") == 0) {
		config_set_int(userConfig, "BasicWindow", "MultiviewLayout",
			       static_cast<int>(MultiviewLayout::HORIZONTAL_BOTTOM_8_SCENES));
		return true;
	}

	if (astrcmpi(layout, "verticalleft") == 0) {
		config_set_int(userConfig, "BasicWindow", "MultiviewLayout",
			       static_cast<int>(MultiviewLayout::VERTICAL_LEFT_8_SCENES));
		return true;
	}

	if (astrcmpi(layout, "verticalright") == 0) {
		config_set_int(userConfig, "BasicWindow", "MultiviewLayout",
			       static_cast<int>(MultiviewLayout::VERTICAL_RIGHT_8_SCENES));
		return true;
	}

	return false;
}

bool OBSApp::InitGlobalConfig()
{
	char path[512];

	int len = GetAppConfigPath(path, sizeof(path), "obs-studio/global.ini");
	if (len <= 0) {
		return false;
	}

	int errorcode = appConfig.Open(path, CONFIG_OPEN_ALWAYS);
	if (errorcode != CONFIG_SUCCESS) {
		OBSErrorBox(NULL, "Failed to open global.ini: %d", errorcode);
		return false;
	}

	uint32_t lastVersion = config_get_int(appConfig, "General", "LastVersion");

	if (lastVersion && lastVersion < MAKE_SEMANTIC_VERSION(31, 0, 0)) {
		bool migratedUserSettings = config_get_bool(appConfig, "General", "Pre31Migrated");

		if (!migratedUserSettings) {
			bool migrated = MigrateGlobalSettings();

			config_set_bool(appConfig, "General", "Pre31Migrated", migrated);
			config_save_safe(appConfig, "tmp", nullptr);
		}
	}

	InitGlobalConfigDefaults();
	InitGlobalLocationDefaults();

	std::filesystem::path defaultUserConfigLocation =
		std::filesystem::u8path(config_get_default_string(appConfig, "Locations", "Configuration"));
	std::filesystem::path defaultUserScenesLocation =
		std::filesystem::u8path(config_get_default_string(appConfig, "Locations", "SceneCollections"));
	std::filesystem::path defaultUserProfilesLocation =
		std::filesystem::u8path(config_get_default_string(appConfig, "Locations", "Profiles"));
	std::filesystem::path defaultPluginManagerLocation =
		std::filesystem::u8path(config_get_default_string(appConfig, "Locations", "PluginManagerSettings"));

	if (IsPortableMode()) {
		userConfigLocation = std::move(defaultUserConfigLocation);
		userScenesLocation = std::move(defaultUserScenesLocation);
		userProfilesLocation = std::move(defaultUserProfilesLocation);
		userPluginManagerSettingsLocation = std::move(defaultPluginManagerLocation);
	} else {
		std::filesystem::path currentUserConfigLocation =
			std::filesystem::u8path(config_get_string(appConfig, "Locations", "Configuration"));
		std::filesystem::path currentUserScenesLocation =
			std::filesystem::u8path(config_get_string(appConfig, "Locations", "SceneCollections"));
		std::filesystem::path currentUserProfilesLocation =
			std::filesystem::u8path(config_get_string(appConfig, "Locations", "Profiles"));
		std::filesystem::path currentUserPluginManagerLocation =
			std::filesystem::u8path(config_get_string(appConfig, "Locations", "PluginManagerSettings"));

		userConfigLocation = (std::filesystem::exists(currentUserConfigLocation))
					     ? std::move(currentUserConfigLocation)
					     : std::move(defaultUserConfigLocation);
		userScenesLocation = (std::filesystem::exists(currentUserScenesLocation))
					     ? std::move(currentUserScenesLocation)
					     : std::move(defaultUserScenesLocation);
		userProfilesLocation = (std::filesystem::exists(currentUserProfilesLocation))
					       ? std::move(currentUserProfilesLocation)
					       : std::move(defaultUserProfilesLocation);
		userPluginManagerSettingsLocation = (std::filesystem::exists(currentUserPluginManagerLocation))
							    ? std::move(currentUserPluginManagerLocation)
							    : std::move(defaultPluginManagerLocation);
	}

	bool userConfigResult = InitUserConfig(userConfigLocation, lastVersion);

	return userConfigResult;
}

bool OBSApp::InitUserConfig(std::filesystem::path &userConfigLocation, uint32_t lastVersion)
{
	const std::string userConfigFile = userConfigLocation.u8string() + "/obs-studio/user.ini";

	int errorCode = userConfig.Open(userConfigFile.c_str(), CONFIG_OPEN_ALWAYS);

	if (errorCode != CONFIG_SUCCESS) {
		OBSErrorBox(nullptr, "Failed to open user.ini: %d", errorCode);
		return false;
	}

	MigrateLegacySettings(lastVersion);
	InitUserConfigDefaults();

	return true;
}

void OBSApp::MigrateLegacySettings(const uint32_t lastVersion)
{
	bool hasChanges = false;

	const uint32_t v19 = MAKE_SEMANTIC_VERSION(19, 0, 0);
	const uint32_t v21 = MAKE_SEMANTIC_VERSION(21, 0, 0);
	const uint32_t v23 = MAKE_SEMANTIC_VERSION(23, 0, 0);
	const uint32_t v24 = MAKE_SEMANTIC_VERSION(24, 0, 0);
	const uint32_t v24_1 = MAKE_SEMANTIC_VERSION(24, 1, 0);

	const map<uint32_t, string> defaultsMap{
		{{v19, "Pre19Defaults"}, {v21, "Pre21Defaults"}, {v23, "Pre23Defaults"}, {v24_1, "Pre24.1Defaults"}}};

	for (auto &[version, configKey] : defaultsMap) {
		if (!config_has_user_value(userConfig, "General", configKey.c_str())) {
			bool useOldDefaults = lastVersion && lastVersion < version;
			config_set_bool(userConfig, "General", configKey.c_str(), useOldDefaults);

			hasChanges = true;
		}
	}

	if (config_has_user_value(userConfig, "BasicWindow", "MultiviewLayout")) {
		const char *layout = config_get_string(userConfig, "BasicWindow", "MultiviewLayout");

		bool layoutUpdated = UpdatePre22MultiviewLayout(layout);

		hasChanges = hasChanges | layoutUpdated;
	}

	if (lastVersion && lastVersion < v24) {
		bool disableHotkeysInFocus = config_get_bool(userConfig, "General", "DisableHotkeysInFocus");

		if (disableHotkeysInFocus) {
			config_set_string(userConfig, "General", "HotkeyFocusType", "DisableHotkeysInFocus");
		}

		hasChanges = true;
	}

	if (hasChanges) {
		userConfig.SaveSafe("tmp");
	}
}

static constexpr string_view OBSGlobalIniPath = "/obs-studio/global.ini";
static constexpr string_view OBSUserIniPath = "/obs-studio/user.ini";

bool OBSApp::MigrateGlobalSettings()
{
	char path[512];

	int len = GetAppConfigPath(path, sizeof(path), nullptr);
	if (len <= 0) {
		OBSErrorBox(nullptr, "Unable to get global configuration path.");
		return false;
	}

	std::string legacyConfigFileString;
	legacyConfigFileString.reserve(strlen(path) + OBSGlobalIniPath.size());
	legacyConfigFileString.append(path).append(OBSGlobalIniPath);

	const std::filesystem::path legacyGlobalConfigFile = std::filesystem::u8path(legacyConfigFileString);

	std::string configFileString;
	configFileString.reserve(strlen(path) + OBSUserIniPath.size());
	configFileString.append(path).append(OBSUserIniPath);

	const std::filesystem::path userConfigFile = std::filesystem::u8path(configFileString);

	if (std::filesystem::exists(userConfigFile)) {
		OBSErrorBox(nullptr,
			    "Unable to migrate global configuration - user configuration file already exists.");
		return false;
	}

	try {
		std::filesystem::copy(legacyGlobalConfigFile, userConfigFile);
	} catch (const std::filesystem::filesystem_error &) {
		OBSErrorBox(nullptr, "Unable to migrate global configuration - copy failed.");
		return false;
	}

	return true;
}

bool OBSApp::InitLocale()
{
	ProfileScope("OBSApp::InitLocale");

	const char *lang = config_get_string(userConfig, "General", "Language");
	bool userLocale = config_has_user_value(userConfig, "General", "Language");
	if (!userLocale || !lang || lang[0] == '\0') {
		lang = DEFAULT_LANG;
	}

	locale = lang;

	// set basic default application locale
	if (!locale.empty()) {
		QLocale::setDefault(QLocale(QString::fromStdString(locale).replace('-', '_')));
	}

	string englishPath;
	if (!GetDataFilePath("locale/" DEFAULT_LANG ".ini", englishPath)) {
		OBSErrorBox(NULL, "Failed to find locale/" DEFAULT_LANG ".ini");
		return false;
	}

	textLookup = text_lookup_create(englishPath.c_str());
	if (!textLookup) {
		OBSErrorBox(NULL, "Failed to create locale from file '%s'", englishPath.c_str());
		return false;
	}

	bool defaultLang = astrcmpi(lang, DEFAULT_LANG) == 0;

	if (userLocale && defaultLang) {
		return true;
	}

	if (!userLocale && defaultLang) {
		for (auto &locale_ : GetPreferredLocales()) {
			if (locale_ == lang) {
				return true;
			}

			stringstream file;
			file << "locale/" << locale_ << ".ini";

			string path;
			if (!GetDataFilePath(file.str().c_str(), path)) {
				continue;
			}

			if (!text_lookup_add(textLookup, path.c_str())) {
				continue;
			}

			blog(LOG_INFO, "Using preferred locale '%s'", locale_.c_str());
			locale = locale_;

			// set application default locale to the new chosen one
			if (!locale.empty()) {
				QLocale::setDefault(QLocale(QString::fromStdString(locale).replace('-', '_')));
			}

			return true;
		}

		return true;
	}

	stringstream file;
	file << "locale/" << lang << ".ini";

	string path;
	if (GetDataFilePath(file.str().c_str(), path)) {
		if (!text_lookup_add(textLookup, path.c_str())) {
			blog(LOG_ERROR, "Failed to add locale file '%s'", path.c_str());
		}
	} else {
		blog(LOG_ERROR, "Could not find locale file '%s'", file.str().c_str());
	}

	return true;
}

#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
void ParseBranchesJson(const std::string &jsonString, vector<UpdateBranch> &out, std::string &error)
{
	JsonBranches branches;

	try {
		nlohmann::json json = nlohmann::json::parse(jsonString);
		branches = json.get<JsonBranches>();
	} catch (nlohmann::json::exception &e) {
		error = e.what();
		return;
	}

	for (const JsonBranch &json_branch : branches) {
#ifdef _WIN32
		if (!json_branch.windows) {
			continue;
		}
#elif defined(__APPLE__)
		if (!json_branch.macos) {
			continue;
		}
#endif

		UpdateBranch branch = {
			QString::fromStdString(json_branch.name),
			QString::fromStdString(json_branch.display_name),
			QString::fromStdString(json_branch.description),
			json_branch.enabled,
			json_branch.visible,
		};
		out.push_back(branch);
	}
}

bool LoadBranchesFile(vector<UpdateBranch> &out)
{
	string error;
	string branchesText;

	BPtr<char> branchesFilePath = GetAppConfigPathPtr("obs-studio/updates/branches.json");

	QFile branchesFile(branchesFilePath.Get());
	if (!branchesFile.open(QIODevice::ReadOnly)) {
		error = "Opening file failed.";
		goto fail;
	}

	branchesText = branchesFile.readAll().toStdString();
	if (branchesText.empty()) {
		error = "File empty.";
		goto fail;
	}

	ParseBranchesJson(branchesText, out, error);
	if (error.empty()) {
		return !out.empty();
	}

fail:
	blog(LOG_WARNING, "Loading branches from file failed: %s", error.c_str());
	return false;
}
#endif

void OBSApp::SetBranchData(const string &data)
{
#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
	string error;
	vector<UpdateBranch> result;

	ParseBranchesJson(data, result, error);

	if (!error.empty()) {
		blog(LOG_WARNING, "Reading branches JSON response failed: %s", error.c_str());
		return;
	}

	if (!result.empty()) {
		updateBranches = result;
	}

	branches_loaded = true;
#else
	UNUSED_PARAMETER(data);
#endif
}

std::vector<UpdateBranch> OBSApp::GetBranches()
{
	vector<UpdateBranch> out;
	/* Always ensure the default branch exists */
	out.push_back(UpdateBranch{"stable", "", "", true, true});

#if defined(_WIN32) || defined(ENABLE_SPARKLE_UPDATER)
	if (!branches_loaded) {
		vector<UpdateBranch> result;
		if (LoadBranchesFile(result)) {
			updateBranches = result;
		}

		branches_loaded = true;
	}
#endif

	/* Copy additional branches to result (if any) */
	if (!updateBranches.empty()) {
		out.insert(out.end(), updateBranches.begin(), updateBranches.end());
	}

	return out;
}

#ifdef __ANDROID__
/* Android 触摸/按键补齐。两件事性质相同 —— 平台把事件送进来了，但 OBS 那套桌面语义没人接 ——
 * 所以合在一个 app 级事件过滤器里：
 * ① 3-4 长按 = 右键。Android 上 Qt 把触摸转成鼠标事件（实测长按 1.2 s 只把列表项选中了，
 * 不出上下文菜单 —— TapAndHold 手势在 QWidget 这条路上没人接），而 OBS 的场景/源/混音器菜单
 * 全挂在 customContextMenuRequested 上，没有右键就永远点不出来。
 * 不拦原始事件：按下照常发给控件（选中、滚动行为都不变），只是同时起一个计时器；到点还没松手
 * 也没滑出阈值，就照 Qt 处理 Key_Menu 的做法，给"最近的声明了上下文菜单策略的祖先控件"送一条
 * QContextMenuEvent(Mouse)。触发后的那次松手**只在真的弹起了菜单时**吞掉（免得给列表补一次"点击"）；
 * 什么都没弹就照常交回控件 —— 否则长按等于把按钮的 clicked 吃掉，见 fire() 的返回值注释。
 * ② 返回键 = Escape，见下面 KeyPress 那一格（build 74）。
 * 不用 Q_OBJECT：只要 eventFilter 虚函数 + 成员 QTimer，省掉一次 moc。 */
class AndroidInputFilter : public QObject {
public:
	explicit AndroidInputFilter(QObject *parent = nullptr) : QObject(parent)
	{
		timer.setSingleShot(true);
		/* 到点只是"长按到位"，不当场弹菜单 —— OBS 的上下文菜单结尾是阻塞的 popup.exec()，
		 * 手指还按着的时候弹出去，会被随后那发 touch 当成"点在菜单外"立刻 dismiss
		 * （2026-09-05 20:36 实测：sendEvent 同毫秒返回，一句 setVisible(false) 都没有）。
		 * 真正的弹出发放延到松手那一刻。 */
		connect(&timer, &QTimer::timeout, this, [this] { armed = true; });
	}

protected:
	bool eventFilter(QObject *watched, QEvent *ev) override
	{
		if (!watched->isWidgetType())
			return false;

		switch (ev->type()) {
		/* Android 的返回键 = Escape。这一条是"先量再修"量出来的，而且**推翻了动手前的猜测**：
		 * BACK 不是没送进 Qt —— build 73 实测（`.qoder/back-measure.sh`、留档 `.qoder/backprobe/`）
		 * 它以 QEvent::KeyPress、key=Qt::Key_Back(0x1000061，qnamespace.h:855)、nativeVirtualKey=0
		 * 正常进来，沿焦点链一路传播到底没人接（主界面那一拍 6 个控件各打一行、投影仪那一拍只到
		 * OBSProjector），而**同一个窗**上紧接着发 ESCAPE(111)=Qt::Key_Escape(0x1000000) 就被
		 * OBSProjector.cpp:63 那条 QAction 接走并关掉了窗 ⇒ 缺的只有"翻译"这一环：前端全树对
		 * Qt::Key_Back 零处理（grep frontend/ 命中 0）。
		 * 为什么换成 Escape 而不是直接 close()/reject()：投影仪的关闭链是
		 * EscapeTriggered → OBSBasic::DeleteProjector（键盘 :65 与触摸菜单 :310 两条路都走它），
		 * 自己 close() 会绕过它；而根界面上浮动 dock 之类也是顶层窗，close() 会把它直接收掉。
		 * 送 Escape 就等于把"桌面上按 Escape 会发生什么"原样搬过来：QDialog 靠 app_event_filter:266
		 * 放行 + 它自己的 keyPressEvent reject，投影仪/Multiview 靠那条快捷键。 */
		case QEvent::KeyPress: {
			QKeyEvent *ke = static_cast<QKeyEvent *>(ev);
			if (ke->key() != Qt::Key_Back)
				break;
			/* 菜单/popup 立着时 Qt 自己就把它撤了（09-07 00:26 实测 BACK 能关 popup 菜单），不抢。 */
			if (QApplication::activePopupWidget())
				break;
			QWidget *top = static_cast<QWidget *>(watched)->window();
			/* 焦点还在根界面上 ⇒ BACK 留给 Android。今天它在主界面上什么都不做（01:39 实测
			 * pid 不变、chrome 仍 39,42,51），"退回后台"是另一笔账，不混进这一条里改。 */
			if (top == static_cast<OBSApp *>(qApp)->GetMainWindow())
				break;
			backEaten = true;
			QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
			QApplication::sendEvent(top, &esc);
			obsAndroidDisplayLog("BACK→Escape：%s（模态=%d 送出后 accepted=%d）", top->metaObject()->className(),
					     (int)top->isModal(), (int)esc.isAccepted());
			return true;
		}
		case QEvent::KeyRelease:
			/* 按下那拍已经当成 Escape 用掉了；留着这次松手只会再走一遍"BACK 没人处理"。 */
			if (backEaten && static_cast<QKeyEvent *>(ev)->key() == Qt::Key_Back) {
				backEaten = false;
				return true;
			}
			break;
		/* ===== P-8 探针（先量再修）：组合框弹层在合成触摸下打不开 ==================================
		 * 要分开的两种长相，在截图上是同一张脸（都是"两秒后屏上没有弹层"）：
		 *   A 从未 Show ⇒ `QComboBox::mousePressEvent` 里那条 `d->popup()` 根本没跑到（事件没进来 / 被手势
		 *     判走 / 落点不是那个控件）；
		 *   B Show 了又立刻 Hide ⇒ 弹层立起来又被打死，看 Hide 隔了多少毫秒、松手落在谁身上。
		 * 四行各管一环：P8-PRESS（按下落到谁、是不是 QComboBox）、P8-G（有没有被判成 TapAndHold 手势）、
		 * P8-W（顶层窗 Show/Hide/Close/Expose 与几何）、P8-REL（松手时谁是 activePopupWidget）。
		 * 前缀一律 ASCII —— 设备端 grep 中文不可靠（仪器账第 7 次）。 */
		case QEvent::Show:
		case QEvent::Hide:
		case QEvent::Close:
		case QEvent::Expose: {
			QWidget *pw = static_cast<QWidget *>(watched);
			/* 主窗不进这条：它的 Expose 每轮都来，会把上面几行淹掉。 */
			if (!pw->isWindow() || pw == static_cast<OBSApp *>(qApp)->GetMainWindow())
				break;
			const Qt::WindowFlags flags = pw->windowFlags();
			const QRect geo = pw->geometry();
			const char *kind = ev->type() == QEvent::Show
						   ? "SHOW"
						   : ev->type() == QEvent::Hide
							     ? "HIDE"
							     : ev->type() == QEvent::Close ? "CLOSE" : "EXPOSE";
			obsAndroidDisplayLog(
				"P8-W %s cls=%s obj=%s geo=%dx%d+%d+%d popup=%d tool=%d dialog=%d vis=%d act=%d", kind,
				pw->metaObject()->className(), pw->objectName().toUtf8().constData(), geo.x(), geo.y(),
				geo.width(), geo.height(), int(flags.testFlag(Qt::Popup)), int(flags.testFlag(Qt::Tool)),
				int(flags.testFlag(Qt::Dialog)), int(pw->isVisible()),
				int(qApp->activePopupWidget() == pw));
			break;
		}
		case QEvent::Gesture: {
			QGestureEvent *ge = static_cast<QGestureEvent *>(ev);
			if (QGesture *g = ge->gesture(Qt::TapAndHoldGesture))
				obsAndroidDisplayLog("P8-G tapandhold state=%d recv=%s accepted=%d", (int)g->state(),
						       watched->metaObject()->className(), int(ev->isAccepted()));
			break;
		}
		case QEvent::MouseButtonPress: {
			QWidget *pw = static_cast<QWidget *>(watched);
			const QRect geo = pw->geometry();
			obsAndroidDisplayLog("P8-PRESS cls=%s obj=%s geo=%dx%d+%d+%d combo=%d win=%s",
					     pw->metaObject()->className(), pw->objectName().toUtf8().constData(), geo.x(),
					     geo.y(), geo.width(), geo.height(),
					     int(qobject_cast<QComboBox *>(pw) != nullptr),
					     pw->window() ? pw->window()->metaObject()->className() : "?");
			QMouseEvent *me = static_cast<QMouseEvent *>(ev);
			if (me->button() != Qt::LeftButton)
				break;
			/* P-8 修法 v3（组合框）。b83 实测（.qoder/p8-b83/p8.txt）：弹层从来不是"打不开"，
			 * 而是"开在松手之前的 1 ms 里、又被这发松手关掉"——
			 *   41.811 PRESS→QComboBox「simpleOutRecFormat」 / 41.815 SHOW→QComboBoxPrivateContainer
			 *   / 41.816 REL（落到主窗那个 QWidget）+ HIDE，**同一毫秒**。
			 * 同一处按满 400 ms（G2）弹层就活得好好的，截图 G2-combo.png 里两档混合容器都立着
			 * ⇒ 弹层本身能建、能画、能收后续点击，坏的只有"这发太快的松手"：
			 *   `QComboBoxPrivateContainer::eventFilter` 对落在弹层外的 MouseButtonRelease 一律 hidePopup，
			 *   而 Android 上这发松手送到的是主窗的 widget，不是刚建出来的弹层。
			 * ⇒ 最小干预：**不替 Qt 弹**（b83 证明 Qt 自己会弹），只记下"这发按下落在组合框上"，
			 *   到松手那一刻再看弹层立没立着（见下面 release 分支）。
			 * 为什么不在这里判定：v1 就是在这儿读 `activePopupWidget()` 想确认"弹起来了"，实测恒为 null
			 *   （Qt 登记活动弹层要到下一轮事件循环，SHOW 行里的 act=0 是同一件事）⇒ 判定必须放到松手时。
			 * 弹层已经立着时不记：那一下该由 Qt 的"点外面就关"收尾，不抢。
			 * 长按那套（armed/timer）这里一并跳过：弹层刚立起来，再补一发合成右键只会把它按死。 */
			if (QComboBox *cb = qobject_cast<QComboBox *>(pw)) {
				if (!qApp->activePopupWidget())
					comboPress = cb;
				break;
			}
			target = static_cast<QWidget *>(watched);
			pressGlobal = me->globalPosition().toPoint();
			armed = false;
			timer.start(HOLD_MS);
			break;
		}
		case QEvent::MouseMove: {
			QMouseEvent *me = static_cast<QMouseEvent *>(ev);
			if ((me->globalPosition().toPoint() - pressGlobal).manhattanLength() > MOVE_SLOP) {
				timer.stop(); // 这是拖动/滚动，不是长按
				armed = false;
			}
			break;
		}
		case QEvent::MouseButtonRelease:
		case QEvent::MouseButtonDblClick:
			if (ev->type() == QEvent::MouseButtonRelease) {
				QMouseEvent *re = static_cast<QMouseEvent *>(ev);
				QWidget *rp = static_cast<QWidget *>(watched);
				QWidget *pop = qApp->activePopupWidget();
				obsAndroidDisplayLog("P8-REL cls=%s at=(%d,%d) actPopup=%s eaten=%d", rp->metaObject()->className(),
							     re->globalPosition().toPoint().x(), re->globalPosition().toPoint().y(),
							     pop ? pop->metaObject()->className() : "none", int(comboPress != nullptr));
			}
			/* P-8 修法 v3（接上面 press 分支）：这发松手如果正好是"刚落在组合框上的那次按下"的尾巴、
			 * 而此刻弹层确实立着，就把它吃掉 —— Qt 那条"松手落在弹层外 ⇒ hidePopup"收不到事件，弹层留住。
			 * 判定放在这里（而不是按下时）是因为活动弹层要等下一轮事件循环才登记得上，见上面那段。
			 * 指针一进分支就清 ⇒ 最多多吃一发，绝不会波及后面不相干的松手（拖滚动条那类）。 */
			if (comboPress) {
				QWidget *pop = qApp->activePopupWidget();
				comboPress = nullptr;
				if (pop && pop->inherits("QComboBoxPrivateContainer")) {
					obsAndroidDisplayLog("P8-EAT 吃掉这发松手：弹层=%s 立着", pop->metaObject()->className());
					return true;
				}
			}
			timer.stop();
			if (armed) {
				armed = false;
				/* 只有真的弹起了菜单才吞这发松手。吞的理由（对列表那类成立）：既不给它补一次
				 * "点击"，也不让它以为按键还开着。但**没弹出任何东西的长按**本来就是"一次按得
				 * 久一点的点击" —— 一律吞掉就把只有 `clicked` 一个入口的按钮判了死刑（转场 "+"：
				 * 03:45 实测长按 1.2 s 三发全 `菜单立过=0`，而同一处一发 tap 就弹菜单）。 */
				if (!fire())
					return false;
				return true;
			}
			break;
		default:
			break;
		}
		return false;
	}

private:
	static constexpr int HOLD_MS = 500;
	static constexpr int MOVE_SLOP = 24;

	/* 返回值 = "这发长按真的立起过菜单吗"。松手分支要用（见 eventFilter 里 MouseButtonRelease
	 * 那段）：立过才吞松手，没立就把它交回控件 —— 否则只有 `clicked` 一个入口的按钮
	 * （转场 "+" 就是）在手指按久一点时变成"点了没反应"。03:45 实测：三条手势形态各长按 1.2 s，
	 * `锚「transitionAdd」` 一次都没新增、`菜单立过=0`。三条出口各自量耗时 > 0 就算立过 ——
	 * OBS 的菜单结尾是阻塞的 popup.exec()，同毫秒返回的那条路根本没弹东西。 */
	bool fire()
	{
		QWidget *w = target.data();
		if (!w)
			return false;

		/* 按下事件落在的是 QAbstractScrollArea 的 viewport，而策略设在列表控件自己身上；
		 * 往上找到那个声明了策略的祖先，pos 也按它的坐标系给 —— OBS 的 handler 里是 itemAt(pos)。
		 * build 80 在这条向下的路之前先补一次**向下的几何落点**：04:42 实测预览区那发按下的接收者
		 * 是**顶层窗自己**（`w「OBSBasic」1067x667+0+0 顶层=1`），而 `preview` 那条
		 * `Qt::CustomContextMenu` 设在它**自己身上**（`forms/OBSBasic.ui:240`）—— 既不在 w 上、
		 * 也不在 w 的祖先里 ⇒ 只往上爬永远找不到它。`childAt` 是纯几何的下钻（不看事件派发怎么
		 * 选接收者），拿同一个 pressGlobal 在 w 的子树里再落一次点。**只在往下落空了之后才走老路**，
		 * 且老路 `policyUp(w->parentWidget())` 与 build 79 逐字同形 ⇒ 现在绿着的那些腿一步没动。 */
		auto policyUp = [](QWidget *x) {
			while (x) {
				const Qt::ContextMenuPolicy policy = x->contextMenuPolicy();
				if (policy == Qt::CustomContextMenu || policy == Qt::ActionsContextMenu)
					return x;
				x = x->parentWidget();
			}
			return static_cast<QWidget *>(nullptr);
		};
		QWidget *const hit = w->childAt(w->mapFromGlobal(pressGlobal));
		QWidget *menu = hit ? policyUp(hit) : nullptr;
		if (!menu)
			menu = policyUp(w->parentWidget());
		if (!menu)
			menu = w;

		/* 给 3-4 探针一个能 grep 的确定信号：光看"菜单有没有弹"分不清是长按没触发，
		 * 还是触发了但那个控件压根没挂菜单。
		 * 行尾那三个字段是 04:1x 之后加的：只有 className 时"接收=OBSBasic"这一格无法自证 ——
		 * 预览中心那三个落点报的就是 OBSBasic，可落点明明在显示面自己的那个原生子窗（#7）里。
		 * ⇒ 补 objectName（同一张 .ui 里独一份）、它在父系里的几何、它自己是不是顶层窗、
		 *   以及它所属顶层窗的 objectName。**一律追加在行尾**，前面 `%s@(%d,%d) → %s` 那截不动 ——
		 *   `.qoder/longpress-hit-probe.sh` 与 `menupos-probe.sh` 的正则都吃这个前缀。 */
		const QRect wGeo = w->geometry();
		const QWidget *wWin = w->window();
		obsAndroidDisplayLog("3-4 长按→右键：%s@(%d,%d) → %s %s〔w「%s」%dx%d+%d+%d 顶层=%d 属于「%s」〕",
				     w->metaObject()->className(), pressGlobal.x(), pressGlobal.y(),
				     menu->metaObject()->className(),
				     menu == w ? "（未找到菜单策略祖先，投给事件原点）" : "",
				     qUtf8Printable(w->objectName()), wGeo.width(), wGeo.height(), wGeo.x(),
				     wGeo.y(), int(w->isWindow()), qUtf8Printable(wWin ? wWin->objectName() : QString()));

		/* 单独一行、不动上面那个前缀：记下"向下的几何落点找到了谁、最后采用的是谁"。
		 * 这一格是 build 80 那次改动唯一的判据来源 —— 没有它，"菜单起来了"和"菜单为什么
		 * 起来了"在日志里是同一张脸。 */
		const QString hitDesc = hit ? QStringLiteral("「%1」%2").arg(hit->objectName(),
									     QString::fromUtf8(
										     hit->metaObject()->className()))
					    : QStringLiteral("空");
		const QByteArray hitUtf8 = hitDesc.toUtf8();
		const QByteArray tailUtf8 =
			(menu == w ? QStringLiteral("（落点也没找到策略持有者）") : QString()).toUtf8();
		obsAndroidDisplayLog("3-4 长按→下钻：childAt=%s 采用=「%s」%s", hitUtf8.constData(),
				     qUtf8Printable(menu->objectName()), tailUtf8.constData());

		/* 坐标系：QAbstractScrollArea 那一类（SourceTree/SceneTree 都是）的
		 * `customContextMenuRequested` 按 Qt 文档给的是 **viewport 系**坐标 —— OBS 的 handler 里
		 * 就是 `itemAt(pos)`，而 `menu` 是带 2px 边框的外层控件。照外层算会整体偏一个边框宽：
		 * 02:32 实测手指在逻辑(160,76)、菜单却起于(162,78)；行边界上这一偏还会选错行。 */
		QWidget *coordBase = menu;
		if (QAbstractScrollArea *scroll = qobject_cast<QAbstractScrollArea *>(menu))
			coordBase = scroll->viewport();
		const QPoint local = coordBase->mapFromGlobal(pressGlobal);
		QContextMenuEvent ce(QContextMenuEvent::Mouse, local, pressGlobal);
		/* 分岔口不是 isAccepted()：QContextMenuEvent 造出来就是 accepted 的，CustomContextMenu
		 * 那条分支也只是 emit 一下就 return，谁都没碰过这个标志位 —— 20:36 那句"已受理"是假信号。
		 * 真正的判据是**耗时**：OBS 的 CreateSourcePopupMenu 结尾是阻塞的 popup.exec()，菜单只要
		 * 立起来，sendEvent 就要待到菜单关掉才返回（手指抬起时）；同毫秒返回 = 菜单压根没弹。 */
		QElapsedTimer held;
		held.start();
		QApplication::sendEvent(menu, &ce);
		const long long spent = (long long)held.elapsed();
		obsAndroidDisplayLog("3-4 长按→右键：sendEvent 返回 用时=%lldms 弹出菜单=%s", spent,
				     qApp->activePopupWidget() ? "在场" : "已关");

		if (spent > 0)
			return true; // 阻塞过 = 菜单真的立起来过
		/* 0ms 返回就补一发直发 —— 20:45 实测过：sendEvent 到 SourceTree/SceneTree 这类
		 * CustomContextMenu 控件，Android 上**不会**走到 OBSBasic 的 handler（0ms 返回、无菜单），
		 * 而 `invokeMethod("customContextMenuRequested")` 一发就中（菜单 33 项，藏面正常）。
		 * sendEvent 还是留着先走一遍：contextMenuEvent 覆写型控件（如 VolumeControl）只认事件不认信号。 */
		{
			obsAndroidDisplayLog("3-4 长按→右键：0ms 返回 → 直发 customContextMenuRequested(%d,%d)",
					     local.x(), local.y());
			/* 判据不能是 invokeMethod 的返回值：`customContextMenuRequested` 是 **QWidget**
			 * 自己声明的信号，任何 widget 上都"存在"，invokeMethod 恒返回 true —— 18:58 实测
			 * 那条 !ok 分支永远进不去，投影仪长按什么也没弹。
			 * 换成和上面同一把尺子：**耗时**。OBS 的 handler 结尾是阻塞的 popup.exec()，菜单
			 * 立起来 invokeMethod 就要等菜单关掉才返回；0ms = 信号发了但没人接。 */
			QElapsedTimer held2;
			held2.start();
			const bool ok = QMetaObject::invokeMethod(menu, "customContextMenuRequested",
								  Q_ARG(QPoint, local));
			const long long spent2 = (long long)held2.elapsed();
			if (ok && spent2 > 0) {
				obsAndroidDisplayLog("3-4 长按→右键：直发已处理 用时=%lldms（菜单弹过又关了）", spent2);
				return true;
			}
			obsAndroidDisplayLog("3-4 长按→右键：直发无人接（ok=%d 用时=%lldms）→ 补发右键到 %s",
					     ok, spent2, w->metaObject()->className());
			/* 还有一格：既不吃 QContextMenuEvent、直发信号又没人接的控件 —— `OBSProjector`
			 * 就是（它的右键菜单写在 mousePressEvent 的 RightButton 分支里，结尾同样是
			 * 阻塞的 popup.exec()）。Android 上顶层窗没有标题栏、KEYCODE_BACK 又实测关不掉
			 * 任何顶层窗 ⇒ 长按弹那个含"关闭"的菜单是投影仪**唯一**能触摸关掉它的入口。 */
			/* 补发右键。注意别指望把光标挪到手指处：19:03 实测 Android 上
			 * `QCursor::setPos()` 是空操作（setPos(533,333) 之后 pos() 仍返回上一次 tap 的
			 * 377,279），而菜单定位读的就是 pos() —— 所以修在菜单那一头
			 * （OBSProjector::mousePressEvent 改用 event->globalPosition()）。 */
			QElapsedTimer held3;
			held3.start();
			QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(pressGlobal),
					  Qt::RightButton, Qt::RightButton, Qt::NoModifier);
			QApplication::sendEvent(w, &press); // 到这里阻塞，直到菜单被点掉
			QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(pressGlobal),
					    Qt::RightButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(w, &release);
			const long long spent3 = (long long)held3.elapsed();
			obsAndroidDisplayLog("3-4 长按→右键：补发的右键菜单已关 用时=%lldms", spent3);
			return spent3 > 0;
		}
	}

	QTimer timer{this};
	QPointer<QWidget> target;
	QPoint pressGlobal;
	bool armed = false;
	bool backEaten = false;
	QPointer<QComboBox> comboPress; // P-8：这发按下落在组合框上（松手时据此决定吃不吃）
};
#endif // __ANDROID__

OBSApp::OBSApp(int &argc, char **argv, profiler_name_store_t *store)
	: QApplication(argc, argv),
	  profilerNameStore(store),
	  appLaunchUUID_(QUuid::createUuid())
{
	installNativeEventFilter(new OBS::NativeEventFilter);

#ifdef __ANDROID__
	/* 装在 app 上：所有控件的鼠标**和键盘**事件都过这一道（跟着进程活，不用管析构）。
	 * ① 3-4 长按合成右键；② build 74 返回键当 Escape 用。 */
	installEventFilter(new AndroidInputFilter(this));
#endif

	/* fix float handling */
#if defined(Q_OS_UNIX)
	if (!setlocale(LC_NUMERIC, "C")) {
		blog(LOG_WARNING, "Failed to set LC_NUMERIC to C locale");
	}
#endif

#ifndef _WIN32
	// Add POSIX signal handlers:
	// * SIGINT
	// * SIGTERM
	// * SIGABRT
	// * SIGQUIT

	using SignalCallback = decltype(&OBSApp::processSigInt);

	auto connectSignal = [this](std::array<int, 2> &fileDescriptor, QPointer<QSocketNotifier> &notifier,
				    SignalCallback callback) -> void {
		socketpair(AF_UNIX, SOCK_STREAM, 0, fileDescriptor.data());
		notifier = new QSocketNotifier(fileDescriptor[1], QSocketNotifier::Read, this);
		connect(notifier, &QSocketNotifier::activated, this, callback);
	};

	connectSignal(sigIntFileDescriptor, sigIntNotifier, &OBSApp::processSigInt);
	connectSignal(sigTermFileDescriptor, sigTermNotifier, &OBSApp::processSigTerm);
	connectSignal(sigAbrtFileDescriptor, sigAbrtNotifier, &OBSApp::processSigAbrt);
	connectSignal(sigQuitFileDescriptor, sigQuitNotifier, &OBSApp::processSigQuit);
#endif
	connect(qApp, &QGuiApplication::commitDataRequest, this, &OBSApp::commitData, Qt::DirectConnection);

	if (multi) {
		crashHandler_ = std::make_unique<OBS::CrashHandler>();
	} else {
		crashHandler_ = std::make_unique<OBS::CrashHandler>(appLaunchUUID_);
	}

	sleepInhibitor = os_inhibit_sleep_create("OBS Video/audio");

#ifndef __APPLE__
	setWindowIcon(QIcon::fromTheme("obs", QIcon(":/res/images/obs.png")));
#endif

	setDesktopFileName("com.obsproject.Studio");

	pluginManager_ = std::make_unique<OBS::PluginManager>();
}

OBSApp::~OBSApp()
{
	if (libobs_initialized) {
		applicationShutdown();
	}
};

static void move_basic_to_profiles(void)
{
	char path[512];

	if (GetAppConfigPath(path, 512, "obs-studio/basic") <= 0) {
		return;
	}

	const std::filesystem::path basicPath = std::filesystem::u8path(path);

	if (!std::filesystem::exists(basicPath)) {
		return;
	}

	const std::filesystem::path profilesPath =
		App()->userProfilesLocation / std::filesystem::u8path("obs-studio/basic/profiles");

	if (std::filesystem::exists(profilesPath)) {
		return;
	}

	try {
		std::filesystem::create_directories(profilesPath);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_ERROR, "Failed to create profiles directory for migration from basic profile\n%s",
		     error.what());
		return;
	}

	const std::filesystem::path newProfilePath = profilesPath / std::filesystem::u8path(Str("Untitled"));

	for (auto &entry : std::filesystem::directory_iterator(basicPath)) {
		if (entry.is_directory()) {
			continue;
		}

		if (entry.path().filename().u8string() == "scenes.json") {
			continue;
		}

		if (!std::filesystem::exists(newProfilePath)) {
			try {
				std::filesystem::create_directory(newProfilePath);
			} catch (const std::filesystem::filesystem_error &error) {
				blog(LOG_ERROR, "Failed to create profile directory for 'Untitled'\n%s", error.what());
				return;
			}
		}

		const filesystem::path destinationFile = newProfilePath / entry.path().filename();

		const auto copyOptions = std::filesystem::copy_options::overwrite_existing;

		try {
			std::filesystem::copy(entry.path(), destinationFile, copyOptions);
		} catch (const std::filesystem::filesystem_error &error) {
			blog(LOG_ERROR, "Failed to copy basic profile file '%s' to new profile 'Untitled'\n%s",
			     entry.path().filename().u8string().c_str(), error.what());

			return;
		}
	}
}

static void move_basic_to_scene_collections(void)
{
	char path[512];

	if (GetAppConfigPath(path, 512, "obs-studio/basic") <= 0) {
		return;
	}

	const std::filesystem::path basicPath = std::filesystem::u8path(path);

	if (!std::filesystem::exists(basicPath)) {
		return;
	}

	const std::filesystem::path sceneCollectionPath =
		App()->userScenesLocation / std::filesystem::u8path("obs-studio/basic/scenes");

	if (std::filesystem::exists(sceneCollectionPath)) {
		return;
	}

	try {
		std::filesystem::create_directories(sceneCollectionPath);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_ERROR,
		     "Failed to create scene collection directory for migration from basic scene collection\n%s",
		     error.what());
		return;
	}

	const std::filesystem::path sourceFile = basicPath / std::filesystem::u8path("scenes.json");
	const std::filesystem::path destinationFile =
		(sceneCollectionPath / std::filesystem::u8path(Str("Untitled"))).replace_extension(".json");

	try {
		std::filesystem::rename(sourceFile, destinationFile);
	} catch (const std::filesystem::filesystem_error &error) {
		blog(LOG_ERROR, "Failed to rename basic scene collection file:\n%s", error.what());
		return;
	}
}

void OBSApp::AppInit()
{
	ProfileScope("OBSApp::AppInit");

	QAccessible::installFactory(alignmentSelectorFactory);

	if (!MakeUserDirs()) {
		throw "Failed to create required user directories";
	}
	if (!InitGlobalConfig()) {
		throw "Failed to initialize global config";
	}
	if (!InitLocale()) {
		throw "Failed to load locale";
	}
	if (!InitTheme()) {
		throw "Failed to load theme";
	}

	config_set_default_string(userConfig, "Basic", "Profile", Str("Untitled"));
	config_set_default_string(userConfig, "Basic", "ProfileDir", Str("Untitled"));
	config_set_default_string(userConfig, "Basic", "SceneCollection", Str("Untitled"));
	config_set_default_string(userConfig, "Basic", "SceneCollectionFile", Str("Untitled"));
	config_set_default_bool(userConfig, "Basic", "ConfigOnNewProfile", true);

	const std::string_view profileName{config_get_string(userConfig, "Basic", "Profile")};

	if (profileName.empty()) {
		config_set_string(userConfig, "Basic", "Profile", Str("Untitled"));
		config_set_string(userConfig, "Basic", "ProfileDir", Str("Untitled"));
	}

	const std::string_view sceneCollectionName{config_get_string(userConfig, "Basic", "SceneCollection")};

	if (sceneCollectionName.empty()) {
		config_set_string(userConfig, "Basic", "SceneCollection", Str("Untitled"));
		config_set_string(userConfig, "Basic", "SceneCollectionFile", Str("Untitled"));
	}

#ifdef _WIN32
	bool disableAudioDucking = config_get_bool(appConfig, "Audio", "DisableAudioDucking");
	if (disableAudioDucking) {
		DisableAudioDucking(true);
	}
#endif

#ifdef __APPLE__
	if (config_get_bool(appConfig, "Video", "DisableOSXVSync")) {
		EnableOSXVSync(false);
	}
#endif

	UpdateHotkeyFocusSetting(false);

	move_basic_to_profiles();
	move_basic_to_scene_collections();

	if (!MakeUserProfileDirs()) {
		throw "Failed to create profile directories";
	}
}

void OBSApp::checkForUncleanShutdown()
{
	bool hasUncleanShutdown = crashHandler_->hasUncleanShutdown();
	bool hasNewCrashLog = crashHandler_->hasNewCrashLog();

	if (hasUncleanShutdown) {
		UncleanLaunchAction launchAction = handleUncleanShutdown(hasNewCrashLog);

		safe_mode = launchAction.useSafeMode;

		if (launchAction.sendCrashReport) {
			crashHandler_->uploadLastCrashLog();
		}
	}
}

const char *OBSApp::GetRenderModule() const
{
#if defined(_WIN32)
	const char *renderer = config_get_string(appConfig, "Video", "Renderer");

	return (astrcmpi(renderer, "Direct3D 11") == 0) ? DL_D3D11 : DL_OPENGL;
#elif defined(__APPLE__) && defined(__aarch64__)
	const char *renderer = config_get_string(appConfig, "Video", "Renderer");

	return (astrcmpi(renderer, "Metal") == 0) ? DL_METAL : DL_OPENGL;
#else
	return DL_OPENGL;
#endif
}

static bool StartupOBS(const char *locale, profiler_name_store_t *store)
{
	char path[512];

	if (GetAppConfigPath(path, sizeof(path), "obs-studio/plugin_config") <= 0) {
		return false;
	}

	return obs_startup(locale, path, store);
}

inline void OBSApp::ResetHotkeyState(bool inFocus)
{
	obs_hotkey_enable_background_press((inFocus && enableHotkeysInFocus) || (!inFocus && enableHotkeysOutOfFocus));
}

void OBSApp::UpdateHotkeyFocusSetting(bool resetState)
{
	enableHotkeysInFocus = true;
	enableHotkeysOutOfFocus = true;

	const char *hotkeyFocusType = config_get_string(userConfig, "General", "HotkeyFocusType");

	if (astrcmpi(hotkeyFocusType, "DisableHotkeysInFocus") == 0) {
		enableHotkeysInFocus = false;
	} else if (astrcmpi(hotkeyFocusType, "DisableHotkeysOutOfFocus") == 0) {
		enableHotkeysOutOfFocus = false;
	}

	if (resetState) {
		ResetHotkeyState(applicationState() == Qt::ApplicationActive);
	}
}

void OBSApp::DisableHotkeys()
{
	enableHotkeysInFocus = false;
	enableHotkeysOutOfFocus = false;
	ResetHotkeyState(applicationState() == Qt::ApplicationActive);
}

void OBSApp::Exec(VoidFunc func)
{
	func();
}

static void ui_task_handler(obs_task_t task, void *param, bool wait)
{
	auto doTask = [=]() {
		/* to get clang-format to behave */
		task(param);
	};
	QMetaObject::invokeMethod(App(), &OBSApp::Exec, wait ? WaitConnection() : Qt::AutoConnection, doTask);
}

bool OBSApp::OBSInit()
{
	ProfileScope("OBSApp::OBSInit");

	qRegisterMetaType<VoidFunc>("VoidFunc");

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
	if (QApplication::platformName() == "xcb") {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		auto native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();

		obs_set_nix_platform_display(native->display());
#endif

		obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);

		blog(LOG_INFO, "Using EGL/X11");
	}

#ifdef ENABLE_WAYLAND
	if (QApplication::platformName().contains("wayland")) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		auto native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();

		obs_set_nix_platform_display(native->display());
#endif

		obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
		setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

		blog(LOG_INFO, "Platform: Wayland");
	}
#endif

#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
	QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
	obs_set_nix_platform_display(native->nativeResourceForIntegration("display"));
#endif
#endif

#ifdef __APPLE__
	setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
#endif

	if (!StartupOBS(locale.c_str(), GetProfilerNameStore())) {
		return false;
	}

	libobs_initialized = true;

	obs_set_ui_task_handler(ui_task_handler);

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
	bool browserHWAccel = config_get_bool(appConfig, "General", "BrowserHWAccel");

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "BrowserHWAccel", browserHWAccel);
	obs_apply_private_data(settings);

	blog(LOG_INFO, "Current Date/Time: %s", CurrentDateTimeString().c_str());

	blog(LOG_INFO, "Browser Hardware Acceleration: %s", browserHWAccel ? "true" : "false");
#endif
#ifdef _WIN32
	bool hideFromCapture = config_get_bool(userConfig, "BasicWindow", "HideOBSWindowsFromCapture");
	blog(LOG_INFO, "Hide OBS windows from screen capture: %s", hideFromCapture ? "true" : "false");
#endif

	blog(LOG_INFO, "Qt Version: %s (runtime), %s (compiled)", qVersion(), QT_VERSION_STR);
	blog(LOG_INFO, "Portable mode: %s", portable_mode ? "true" : "false");

	if (safe_mode) {
		blog(LOG_WARNING, "Safe Mode enabled.");
	} else if (disable_3p_plugins) {
		blog(LOG_WARNING, "Third-party plugins disabled.");
	}

	setQuitOnLastWindowClosed(false);

	thumbnailManager = new ThumbnailManager(this);

	mainWindow = new OBSBasic();

	mainWindow->setAttribute(Qt::WA_DeleteOnClose, true);

	mainWindow->OBSInit();

	connect(OBSBasic::Get(), &OBSBasic::mainWindowClosed, crashHandler_.get(),
		&OBS::CrashHandler::applicationShutdownHandler);

	connect(this, &QGuiApplication::applicationStateChanged, this,
		[this](Qt::ApplicationState state) { ResetHotkeyState(state == Qt::ApplicationActive); });
	ResetHotkeyState(applicationState() == Qt::ApplicationActive);

	connect(crashHandler_.get(), &OBS::CrashHandler::crashLogUploadFailed, this,
		[this](const QString &errorMessage) {
			emit this->logUploadFailed(OBS::LogFileType::CrashLog, errorMessage);
		});

	connect(crashHandler_.get(), &OBS::CrashHandler::crashLogUploadFinished, this,
		[this](const QString &fileUrl) { emit this->logUploadFinished(OBS::LogFileType::CrashLog, fileUrl); });

	return true;
}

string OBSApp::GetVersionString(bool platform) const
{
	stringstream ver;

	ver << obs_get_version_string();

	if (platform) {
		ver << " (";
#ifdef _WIN32
		if (sizeof(void *) == 8) {
			ver << "64-bit, ";
		} else {
			ver << "32-bit, ";
		}

		ver << "windows)";
#elif __APPLE__
		ver << "mac)";
#elif __OpenBSD__
		ver << "openbsd)";
#elif __FreeBSD__
		ver << "freebsd)";
#else /* assume linux for the time being */
		ver << "linux)";
#endif
	}

	return ver.str();
}

bool OBSApp::IsPortableMode()
{
	return portable_mode;
}

bool OBSApp::IsUpdaterDisabled()
{
	return opt_disable_updater;
}

bool OBSApp::IsMissingFilesCheckDisabled()
{
	return opt_disable_missing_files_check;
}

#ifdef __APPLE__
#define INPUT_AUDIO_SOURCE "coreaudio_input_capture"
#define OUTPUT_AUDIO_SOURCE "coreaudio_output_capture"
#elif _WIN32
#define INPUT_AUDIO_SOURCE "wasapi_input_capture"
#define OUTPUT_AUDIO_SOURCE "wasapi_output_capture"
#else
#define INPUT_AUDIO_SOURCE "pulse_input_capture"
#define OUTPUT_AUDIO_SOURCE "pulse_output_capture"
#endif

const char *OBSApp::InputAudioSource() const
{
	return INPUT_AUDIO_SOURCE;
}

const char *OBSApp::OutputAudioSource() const
{
	return OUTPUT_AUDIO_SOURCE;
}

const char *OBSApp::GetLastLog() const
{
	return lastLogFile.c_str();
}

const char *OBSApp::GetCurrentLog() const
{
	return currentLogFile.c_str();
}

void OBSApp::openCrashLogDirectory() const
{
	std::filesystem::path crashLogDirectory = crashHandler_->getCrashLogDirectory();

	if (crashLogDirectory.empty()) {
		return;
	}

	QString crashLogDirectoryString = QString::fromStdString(crashLogDirectory.u8string());

#ifdef __ANDROID__
	obsAndroidShowLocalPath(GetMainWindow(), "打开崩溃日志文件夹", crashLogDirectoryString);
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(crashLogDirectoryString));
#endif
}

void OBSApp::uploadLastAppLog() const
{
	OBSBasic *basicWindow = static_cast<OBSBasic *>(GetMainWindow());

	basicWindow->UploadLog("obs-studio/logs", GetLastLog(), OBS::LogFileType::LastAppLog);
}

void OBSApp::uploadCurrentAppLog() const
{
	OBSBasic *basicWindow = static_cast<OBSBasic *>(GetMainWindow());

	basicWindow->UploadLog("obs-studio/logs", GetCurrentLog(), OBS::LogFileType::CurrentAppLog);
}

void OBSApp::uploadLastCrashLog()
{
	crashHandler_->uploadLastCrashLog();
}

OBS::LogFileState OBSApp::getLogFileState(OBS::LogFileType type) const
{
	switch (type) {
	case OBS::LogFileType::CrashLog: {
		bool hasNewCrashLog = crashHandler_->hasNewCrashLog();

		return (hasNewCrashLog) ? OBS::LogFileState::New : OBS::LogFileState::Uploaded;
	}
	case OBS::LogFileType::CurrentAppLog:
	case OBS::LogFileType::LastAppLog:
		return OBS::LogFileState::New;
	default:
		return OBS::LogFileState::NoState;
	}
}

bool OBSApp::TranslateString(const char *lookupVal, const char **out) const
{
	for (obs_frontend_translate_ui_cb cb : translatorHooks) {
		if (cb(lookupVal, out)) {
			return true;
		}
	}

	return text_lookup_getstr(App()->GetTextLookup(), lookupVal, out);
}

QStyle *OBSApp::GetInvisibleCursorStyle()
{
	if (!invisibleCursorStyle) {
		invisibleCursorStyle = std::make_unique<OBSInvisibleCursorProxyStyle>();
	}
	return invisibleCursorStyle.get();
}

#ifdef __ANDROID__
/* 3-3 模态遮挡。预览面是 addContentView 到 Activity 内容层、还 setZOrderOnTop(true) 的独立
 * SurfaceView —— 它在 Qt 的窗口之上，对话框物理上盖不住它，不藏就会看见"设置窗口底下露出一块
 * 实时画面"（菜单更糟：整个看不见）。
 * 判据用 Qt 自己维护的 activeModalWidget()/activePopupWidget() 现算，**不做 Show/Hide 计数**：
 * 计数只要漏一条 Hide（widget 直接析构、reject() 与 close() 混用、deleteLater 时序）就永久停在
 * "已藏"，是那种最难复现的脏状态；现算无状态、幂等，代价只是两次指针判空。 */
static void androidSyncPreviewVisibility()
{
	bool covered = qApp->activeModalWidget() || qApp->activePopupWidget();

	/* 只认"模态/弹出"不够：添加源那个窗口（`OBSBasic_SceneItems.cpp:831` 走的是 `show()` 不是
	 * `exec()`，非模态）2026-09-05 20:09 实测弹出来时一句 藏面 都没有，预览面照样压在它右半边，
	 * 列表项看不见也点不着。独立窗口层的道理是"**只要有别家窗口在场，这块面就该让位**"，
	 * 所以判据直接扫顶层窗口。
	 * 排除：主窗口自己（面归它），以及 Tool/ToolTip/Popup/Splash/SubWindow/Desktop 这类系统浮层
	 * —— tooltip 也是独立顶层窗口，算进来的话鼠标一停面就闪一下。
	 * （Qt 6.9 的 WindowType 里没有 StatusBar 这一项，别照抄 Qt5 的枚举名。） */
	if (!covered) {
		const QWidget *main = static_cast<OBSApp *>(qApp)->GetMainWindow();
		/* B-7：投影仪/Multiview 自己就是"当前持有那块面的窗"（可移交认领，见
		 * OBSAndroidDisplay.cpp 的 claimCommon）。不放行它，判据就会在投影仪一开的那一刻
		 * 把它脚下这块面藏掉 —— 投影仪整个内容就是这块面，等于开出来必黑。
		 * 只放行主人那一个窗：主预览（OBSBasicPreview，非顶层窗）当主人时 owner 不在这个循环里，
		 * 设置/添加源那些对话框照旧藏面，3-3 的原语义没被削弱。 */
		const QWidget *faceOwner = qobject_cast<QWidget *>(obsAndroidDisplayLifecycleOwner());
		const auto tops = qApp->topLevelWidgets();
		for (const QWidget *w : tops) {
			if (!w->isVisible() || !w->isWindow() || w == main || w == faceOwner)
				continue;
			switch (w->windowType()) {
			case Qt::Tool:
			case Qt::ToolTip:
			case Qt::Popup:
			case Qt::SplashScreen:
			case Qt::SubWindow:
			case Qt::Desktop:
				continue;
			default:
				break;
			}
			covered = true;
			break;
		}
	}

	obsAndroidDisplaySetVisible(!covered);
}

/* Android 触摸/按键补齐（长按=右键、BACK=Escape）—— AndroidInputFilter 定义在 OBSApp 构造函数之前（构造时要 install）。 */

static void androidResyncPreviewOnWindowToggle(QObject *receiver, QEvent *e)
{
	const QEvent::Type type = e->type();
	if (type != QEvent::Show && type != QEvent::Hide)
		return;
	if (!receiver->isWidgetType())
		return;
	if (!static_cast<QWidget *>(receiver)->isWindow()) // dock/工具栏这些子控件的变化不算窗口
		return;

	/* 当场算一次，再挂一次"事件循环下一轮"的复查：对话框的 Show 先到、Qt 置 modal 指针在后
	 * （关闭时反过来），只算一次会读到半更新的状态。pending 把同一轮里连着几次窗口开关并成
	 * 一次检查 —— 反正查的是"现在有没有模态在场"这个真相，合并不丢信息。 */
	static bool pending = false;
	if (!pending) {
		pending = true;
		QTimer::singleShot(0, qApp, [] {
			pending = false;
			androidSyncPreviewVisibility();
		});
	}
	androidSyncPreviewVisibility();
}
#endif // __ANDROID__

// Global handler to receive all QEvent::Show events so we can apply
// display affinity on any newly created windows and dialogs without
// caring where they are coming from (e.g. plugins).
bool OBSApp::notify(QObject *receiver, QEvent *e)
{
	QWidget *w;
	QWindow *window;
	int windowType;

#ifdef __ANDROID__
	/* 放最前面、且不引入新局部变量：下面那条 goto skip 链跨不过"跳过初始化"，Java 那边
	 * 自己已经查过 isWidgetType()，不必借用这条链的前置判断。 */
	androidResyncPreviewOnWindowToggle(receiver, e);
#endif

	if (!receiver->isWidgetType()) {
		goto skip;
	}

	if (e->type() != QEvent::Show) {
		goto skip;
	}

	w = qobject_cast<QWidget *>(receiver);

	if (!w->isWindow()) {
		goto skip;
	}

	window = w->windowHandle();
	if (!window) {
		goto skip;
	}

	windowType = window->flags() & Qt::WindowType::WindowType_Mask;

	if (windowType == Qt::WindowType::Dialog || windowType == Qt::WindowType::Window ||
	    windowType == Qt::WindowType::Tool) {
		OBSBasic *main = OBSBasic::Get();
		if (main) {
			main->SetDisplayAffinity(window);
		}
	}

skip:
	return QApplication::notify(receiver, e);
}

string GenerateTimeDateFilename(const char *extension, bool noSpace)
{
	time_t now = time(0);
	char file[256] = {};
	struct tm *cur_time;

	cur_time = localtime(&now);
	snprintf(file, sizeof(file), "%d-%02d-%02d%c%02d-%02d-%02d.%s", cur_time->tm_year + 1900, cur_time->tm_mon + 1,
		 cur_time->tm_mday, noSpace ? '_' : ' ', cur_time->tm_hour, cur_time->tm_min, cur_time->tm_sec,
		 extension);

	return string(file);
}

string GenerateSpecifiedFilename(const char *extension, bool noSpace, const char *format)
{
	BPtr<char> filename = os_generate_formatted_filename(extension, !noSpace, format);
	return string(filename);
}

static void FindBestFilename(string &strPath, bool noSpace)
{
	int num = 2;

	if (!os_file_exists(strPath.c_str())) {
		return;
	}

	const char *ext = strrchr(strPath.c_str(), '.');
	if (!ext) {
		return;
	}

	int extStart = int(ext - strPath.c_str());
	for (;;) {
		string testPath = strPath;
		string numStr;

		numStr = noSpace ? "_" : " (";
		numStr += to_string(num++);
		if (!noSpace) {
			numStr += ")";
		}

		testPath.insert(extStart, numStr);

		if (!os_file_exists(testPath.c_str())) {
			strPath = testPath;
			break;
		}
	}
}

static void ensure_directory_exists(string &path)
{
	replace(path.begin(), path.end(), '\\', '/');

	size_t last = path.rfind('/');
	if (last == string::npos) {
		return;
	}

	string directory = path.substr(0, last);
	os_mkdirs(directory.c_str());
}

static void remove_reserved_file_characters(string &s)
{
	replace(s.begin(), s.end(), '\\', '/');
	replace(s.begin(), s.end(), '*', '_');
	replace(s.begin(), s.end(), '?', '_');
	replace(s.begin(), s.end(), '"', '_');
	replace(s.begin(), s.end(), '|', '_');
	replace(s.begin(), s.end(), ':', '_');
	replace(s.begin(), s.end(), '>', '_');
	replace(s.begin(), s.end(), '<', '_');
}

string GetFormatString(const char *format, const char *prefix, const char *suffix)
{
	string f;

	f = format;

	if (prefix && *prefix) {
		string str_prefix = prefix;

		if (str_prefix.back() != ' ') {
			str_prefix += " ";
		}

		size_t insert_pos = 0;
		size_t tmp;

		tmp = f.find_last_of('/');
		if (tmp != string::npos && tmp > insert_pos) {
			insert_pos = tmp + 1;
		}

		tmp = f.find_last_of('\\');
		if (tmp != string::npos && tmp > insert_pos) {
			insert_pos = tmp + 1;
		}

		f.insert(insert_pos, str_prefix);
	}

	if (suffix && *suffix) {
		if (*suffix != ' ') {
			f += " ";
		}
		f += suffix;
	}

	remove_reserved_file_characters(f);

	return f;
}

string GetFormatExt(const char *container)
{
	string ext = container;
	if (ext == "fragmented_mp4" || ext == "hybrid_mp4") {
		ext = "mp4";
	} else if (ext == "fragmented_mov" || ext == "hybrid_mov") {
		ext = "mov";
	} else if (ext == "hls") {
		ext = "m3u8";
	} else if (ext == "mpegts") {
		ext = "ts";
	}

	return ext;
}

string GetOutputFilename(const char *path, const char *container, bool noSpace, bool overwrite, const char *format)
{
	OBSBasic *main = OBSBasic::Get();

	os_dir_t *dir = path && path[0] ? os_opendir(path) : nullptr;

	if (!dir) {
		if (main->isVisible()) {
			OBSMessageBox::warning(main, QTStr("Output.BadPath.Title"), QTStr("Output.BadPath.Text"));
		} else {
			main->SysTrayNotify(QTStr("Output.BadPath.Text"), QSystemTrayIcon::Warning);
		}
		return "";
	}

	os_closedir(dir);

	string strPath;
	strPath += path;

	char lastChar = strPath.back();
	if (lastChar != '/' && lastChar != '\\') {
		strPath += "/";
	}

	string ext = GetFormatExt(container);
	strPath += GenerateSpecifiedFilename(ext.c_str(), noSpace, format);
	ensure_directory_exists(strPath);
	if (!overwrite) {
		FindBestFilename(strPath, noSpace);
	}

	return strPath;
}

vector<pair<string, string>> GetLocaleNames()
{
	string path;
	if (!GetDataFilePath("locale.ini", path)) {
		throw "Could not find locale.ini path";
	}

	ConfigFile ini;
	if (ini.Open(path.c_str(), CONFIG_OPEN_EXISTING) != 0) {
		throw "Could not open locale.ini";
	}

	size_t sections = config_num_sections(ini);

	vector<pair<string, string>> names;
	names.reserve(sections);
	for (size_t i = 0; i < sections; i++) {
		const char *tag = config_get_section(ini, i);
		const char *name = config_get_string(ini, tag, "Name");
		names.emplace_back(tag, name);
	}

	return names;
}

#if defined(__APPLE__) || defined(__linux__)
#define BASE_PATH ".."
#else
#define BASE_PATH "../.."
#endif

#define CONFIG_PATH BASE_PATH "/config"

#if defined(ENABLE_PORTABLE_CONFIG) || defined(_WIN32)
#define ALLOW_PORTABLE_MODE 1
#else
#define ALLOW_PORTABLE_MODE 0
#endif

int GetAppConfigPath(char *path, size_t size, const char *name)
{
#if ALLOW_PORTABLE_MODE
	if (portable_mode) {
		if (name && *name) {
			return snprintf(path, size, CONFIG_PATH "/%s", name);
		} else {
			return snprintf(path, size, CONFIG_PATH);
		}
	} else {
		return os_get_config_path(path, size, name);
	}
#else
	return os_get_config_path(path, size, name);
#endif
}

char *GetAppConfigPathPtr(const char *name)
{
#if ALLOW_PORTABLE_MODE
	if (portable_mode) {
		char path[512];

		if (snprintf(path, sizeof(path), CONFIG_PATH "/%s", name) > 0) {
			return bstrdup(path);
		} else {
			return NULL;
		}
	} else {
		return os_get_config_path_ptr(name);
	}
#else
	return os_get_config_path_ptr(name);
#endif
}

int GetProgramDataPath(char *path, size_t size, const char *name)
{
	return os_get_program_data_path(path, size, name);
}

char *GetProgramDataPathPtr(const char *name)
{
	return os_get_program_data_path_ptr(name);
}

bool GetFileSafeName(const char *name, std::string &file)
{
	size_t base_len = strlen(name);
	size_t len = os_utf8_to_wcs(name, base_len, nullptr, 0);
	std::wstring wfile;

	if (!len) {
		return false;
	}

	wfile.resize(len);
	os_utf8_to_wcs(name, base_len, &wfile[0], len + 1);

	for (size_t i = wfile.size(); i > 0; i--) {
		size_t im1 = i - 1;

		if (iswspace(wfile[im1])) {
			wfile[im1] = '_';
		} else if (wfile[im1] != '_' && !iswalnum(wfile[im1])) {
			wfile.erase(im1, 1);
		}
	}

	if (wfile.size() == 0) {
		wfile = L"characters_only";
	}

	len = os_wcs_to_utf8(wfile.c_str(), wfile.size(), nullptr, 0);
	if (!len) {
		return false;
	}

	file.resize(len);
	os_wcs_to_utf8(wfile.c_str(), wfile.size(), &file[0], len + 1);
	return true;
}

bool GetClosestUnusedFileName(std::string &path, const char *extension)
{
	size_t len = path.size();
	if (extension) {
		path += ".";
		path += extension;
	}

	if (!os_file_exists(path.c_str())) {
		return true;
	}

	int index = 1;

	do {
		path.resize(len);
		path += std::to_string(++index);
		if (extension) {
			path += ".";
			path += extension;
		}
	} while (os_file_exists(path.c_str()));

	return true;
}

bool WindowPositionValid(QRect rect)
{
	for (QScreen *screen : QGuiApplication::screens()) {
		if (screen->availableGeometry().intersects(rect)) {
			return true;
		}
	}
	return false;
}

#ifndef _WIN32
// Static signal handlers
void OBSApp::sigIntSignalHandler(int)
{
	char tmp = 1;
	::send(sigIntFileDescriptor[0], &tmp, sizeof(tmp), 0);
}

void OBSApp::sigTermSignalHandler(int)
{
	char tmp = 1;
	::send(sigTermFileDescriptor[0], &tmp, sizeof(tmp), 0);
}

void OBSApp::sigAbrtSignalHandler(int)
{
	char tmp = 1;
	::send(sigAbrtFileDescriptor[0], &tmp, sizeof(tmp), 0);
}

void OBSApp::sigQuitSignalHandler(int)
{
	char tmp = 1;
	::send(sigQuitFileDescriptor[0], &tmp, sizeof(tmp), 0);
}

// App instance signal processors
void OBSApp::processSigInt()
{
	if (!sigIntNotifier->isEnabled()) {
		return;
	}

	sigIntNotifier->setEnabled(false);

	char tmp;
	::recv(sigIntFileDescriptor[1], &tmp, sizeof(tmp), 0);

	sigIntNotifier->setEnabled(true);

#ifndef __APPLE__
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();
		main->close();
	}
#else
	quit();
#endif
}

void OBSApp::processSigTerm()
{
	if (!sigTermNotifier->isEnabled()) {
		return;
	}

	sigTermNotifier->setEnabled(false);

	char tmp;
	::recv(sigTermFileDescriptor[1], &tmp, sizeof(tmp), 0);

	sigTermNotifier->setEnabled(true);

#ifndef __APPLE__
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();
	}
#endif
	quit();
}

void OBSApp::processSigAbrt()
{
	if (!sigAbrtNotifier->isEnabled()) {
		return;
	}

	sigAbrtNotifier->setEnabled(false);

	char tmp;
	::recv(sigAbrtFileDescriptor[1], &tmp, sizeof(tmp), 0);

	sigAbrtNotifier->setEnabled(true);

#ifndef __APPLE__
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();
	}
#endif
	quit();
}

void OBSApp::processSigQuit()
{
	if (!sigQuitNotifier->isEnabled()) {
		return;
	}

	sigQuitNotifier->setEnabled(false);

	char tmp;
	::recv(sigQuitFileDescriptor[1], &tmp, sizeof(tmp), 0);

	sigQuitNotifier->setEnabled(true);

#ifndef __APPLE__
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();
	}
#endif
	quit();
}
#else
// App instance signal processor stub methods used on Windows for OBSApp API compliance
void OBSApp::processSigInt()
{
	return;
}

void OBSApp::processSigTerm()
{
	return;
}

void OBSApp::processSigAbrt()
{
	return;
}

void OBSApp::processSigQuit()
{
	return;
}
#endif

void OBSApp::commitData(QSessionManager &manager)
{
	OBSBasic *main = OBSBasic::Get();
	if (main) {
		main->saveAll();

		if (manager.allowsInteraction() && main->shouldPromptForClose()) {
			manager.cancel();
		}
	}
}

void OBSApp::applicationShutdown() noexcept
{
#ifdef _WIN32
	bool disableAudioDucking = config_get_bool(appConfig, "Audio", "DisableAudioDucking");
	if (disableAudioDucking) {
		DisableAudioDucking(false);
	}
#else
	auto disconnectSignal = [this](std::array<int, 2> &fileDescriptor,
				       QPointer<QSocketNotifier> &notifier) -> void {
		notifier->setEnabled(false);

		std::array<int, 2> tempFileDescriptor = std::exchange(fileDescriptor, {0, 0});
		::close(tempFileDescriptor[0]);
		::close(tempFileDescriptor[1]);
	};

	disconnectSignal(sigIntFileDescriptor, sigIntNotifier);
	disconnectSignal(sigTermFileDescriptor, sigTermNotifier);
	disconnectSignal(sigAbrtFileDescriptor, sigAbrtNotifier);
	disconnectSignal(sigQuitFileDescriptor, sigQuitNotifier);
#endif

#ifdef __APPLE__
	bool vsyncDisabled = config_get_bool(appConfig, "Video", "DisableOSXVSync");
	bool resetVSync = config_get_bool(appConfig, "Video", "ResetOSXVSyncOnExit");
	if (vsyncDisabled && resetVSync) {
		EnableOSXVSync(true);
	}
#endif

	os_inhibit_sleep_set_active(sleepInhibitor, false);
	os_inhibit_sleep_destroy(sleepInhibitor);

	if (libobs_initialized) {
		obs_shutdown();
		libobs_initialized = false;
	}
}

void OBSApp::addLogLine(int logLevel, const QString &message)
{
	emit logLineAdded(logLevel, message);
}

void OBSApp::loadAppModules(struct obs_module_failure_info &mfi)
{
	pluginManager_->preLoad();
	blog(LOG_INFO, "---------------------------------");
	obs_load_all_modules2(&mfi);
#ifdef __ANDROID__
	/* extractNativeLibs=false → APK 里的插件 .so 不落盘，obs_load_all_modules2 的目录扫描
	 * 什么都扫不到（实测 /proc/<pid>/maps 里应用自己的库全是 ".../base.apk" 的 mmap，
	 * nativeLibraryDir 是 total 0 的空目录，find 全盘也搜不到 image-source.so）。
	 * 改按裸文件名 dlopen：调用方所在的 linker namespace 已含 base.apk!/lib/<abi>/，
	 * 故 obs_open_module("x.so", …) 能命中 APK 内的插件 —— B-6 的 color_source 能画出红色
	 * 就是从这条路上来的，目前也是唯一被验证过的插件装载路径。
	 * 名单在构建期由 plugins/ 白名单生成（cmake/os-android.cmake + cmake/templates/
	 * android-plugin-modules.h.in），加插件只改白名单一处。
	 *
	 * data_path 必须是真实目录：obs-module.c 的 obs_open_module 直接 bstrdup(data_path)
	 * 存进 module->data_path，不做 %module% 替换；obs_find_module_file 只把
	 * "<data_path>/<file>" 拼起来 stat。原先传 "/" ⇒ obs-transitions 装载成功也取不到
	 * 任何 .effect，转场类型照样为 0。数据由 :/obsplugindata 在启动时解到
	 * $OBS_ROOT_PATH/share/obs/obs-plugins/<模块>（见 AndroidRuntimeBootstrap.cpp）。
	 *
	 * 逐个先打一条再打结果：blog() 到不了 logcat，装载中途崩也要能看出卡在哪个插件。 */
	{
		const QByteArray dataRoot = qgetenv("OBS_ROOT_PATH") + "/share/obs/obs-plugins/";
		for (const char *const name : OBS_ANDROID_PLUGIN_MODULES) {
			const QByteArray bin = QByteArray(name) + ".so";
			const QByteArray data = dataRoot + name;
			P1LOG("装载 %s (data=%s)", bin.constData(), data.constData());
			obs_module_t *mod = nullptr;
			const int rc = obs_open_module(&mod, bin.constData(), data.constData());
			if (rc != MODULE_SUCCESS) {
				P1LOG("  %s open 失败 rc=%d", bin.constData(), rc);
				continue;
			}
			if (!obs_init_module(mod))
				P1LOG("  %s open 成功但 obs_init_module 失败（obs_module_load 返回 false）", bin.constData());
		}
	}
#endif
	blog(LOG_INFO, "---------------------------------");
	obs_log_loaded_modules();
	blog(LOG_INFO, "---------------------------------");
	obs_post_load_modules();
	pluginManager_->postLoad();
}

void OBSApp::pluginManagerOpenDialog()
{
	pluginManager_->open();
}
