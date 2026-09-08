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

#include "platform.hpp"

#include <OBSApp.hpp>

#include <obs-config.h>

#include <util/base.h>
#include <util/platform.h>

#include <QFileInfo>
#include <QLocale>
#include <QWidget>
#include <QStandardPaths>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>

using namespace std;

/* 资源根目录的约定与 libobs 侧完全一致（见 libobs/obs-android.c 顶部注释）：
 * Java 层把 APK 里的资源解包到应用私有目录，再用 OBS_ROOT_PATH 环境变量公布出来，
 * 于是 frontend 的数据就落在 $OBS_ROOT_PATH/share/obs/obs-studio/ 之下 ——
 * 与 Linux 上 <prefix>/share/obs/obs-studio/ 同构。 */
#define ROOT_PATH_ENV "OBS_ROOT_PATH"
#define INSTALL_DATA_PATH OBS_INSTALL_DATA_PATH "/obs-studio/"

static inline bool check_path(const char *data, const char *path, string &output)
{
	ostringstream str;
	str << path << data;
	output = str.str();

	blog(LOG_DEBUG, "Attempted path: %s", output.c_str());

	return os_file_exists(output.c_str());
}

bool GetDataFilePath(const char *data, string &output)
{
	const char *root = getenv(ROOT_PATH_ENV);
	if (root && root[0]) {
		const string root_data = string(root) + "/" + OBS_DATA_PATH + "/obs-studio/";
		if (check_path(data, root_data.c_str(), output)) {
			return true;
		}
	}

	if (const char *env = getenv("OBS_DATA_PATH")) {
		if (env[0] && check_path(data, (string(env) + "/obs-studio/").c_str(), output)) {
			return true;
		}
	}

	if (check_path(data, OBS_DATA_PATH "/obs-studio/", output)) {
		return true;
	}

	if (check_path(data, INSTALL_DATA_PATH, output)) {
		return true;
	}

	return false;
}

string GetDefaultVideoSavePath()
{
	/* 这个值会被写进 SimpleOutput/FilePath 的默认配置（OBSBasic.cpp:750），
	 * 所以必须是真能写进去的目录，不能只是"语义上正确"的媒体目录。 */
	QString path = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (!QFileInfo(path).isDir()) {
		path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	}

	blog(LOG_INFO, "Default video save path: %s", path.toStdString().c_str());

	return path.toStdString();
}

vector<string> GetPreferredLocales()
{
	const QStringList ui_languages = QLocale::system().uiLanguages();
	const auto obs_locales = GetLocaleNames();

	auto ui_to_obs = [&obs_locales](const QString &ui) {
		const string language = ui.toStdString();
		string lang_match;

		for (const auto &locale_pair : obs_locales) {
			const string &locale = locale_pair.first;

			if (locale == language.substr(0, locale.size())) {
				return locale;
			}

			if (lang_match.size()) {
				continue;
			}

			if (locale.substr(0, 2) == language.substr(0, 2)) {
				lang_match = locale;
			}
		}

		return lang_match;
	};

	vector<string> result;
	result.reserve(ui_languages.size());

	for (const QString &ui_language : ui_languages) {
		string match = ui_to_obs(ui_language);
		if (!match.size()) {
			continue;
		}

		if (find(begin(result), end(result), match) != end(result)) {
			continue;
		}

		result.emplace_back(match);
	}

	return result;
}

/* Android 没有"窗口永远压在其他应用之上"这种窗口层概念：要跨应用置顶得换
 * TYPE_APPLICATION_OVERLAY 窗口 + SYSTEM_ALERT_WINDOW 权限，Qt 给不了。
 * 这里只把意图记在窗口属性上，让"总是置顶"菜单的勾选状态保持自洽。
 *
 * 不能照抄 platform-x11.cpp 那版 setWindowFlags() + show()：改窗口标志会触发
 * Qt 重建窗口，ANativeWindow 随之销毁，挂在它上面的 gs_swapchain 就没了（S1 实测：
 * surfaceDestroyed 是唯一可靠的销毁信号）。 */
static const char kAlwaysOnTopKey[] = "_obsAndroidAlwaysOnTop";

bool IsAlwaysOnTop(QWidget *window)
{
	return window->property(kAlwaysOnTopKey).toBool();
}

void SetAlwaysOnTop(QWidget *window, bool enable)
{
	window->setProperty(kAlwaysOnTopKey, enable);
}

bool SetDisplayAffinitySupported(void)
{
	return false;
}

/* Qt 6.9.3 的 QStyleHints 只有 colorScheme()，没有高对比度开关（桌面版同样没有，
 * 所以 x11 才去查 XDG portal）。Android 的高对比度文字要读 Settings.Secure，
 * 得等 Java 胶水层（计划 3-5）再实现；返回 false 的效果只是不强制切到
 * com.obsproject.System 高对比主题，功能不受影响。 */
bool HighContrastEnabled()
{
	return false;
}

void TaskbarOverlayInit() {}
void TaskbarOverlaySetStatus(TaskbarOverlayStatus) {}

/* 清单里 QtActivity 已经是 launchMode="singleTop"，系统保证不会起第二个实例；
 * 而且 Android 上没有跨进程命名互斥量的对等物（/tmp 不可用，抽象窗口层也没有）。 */
void CheckIfAlreadyRunning(bool &already_running)
{
	already_running = false;
}
