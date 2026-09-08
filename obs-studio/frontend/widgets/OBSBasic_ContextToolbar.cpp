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

#include <components/ApplicationAudioCaptureToolbar.hpp>
#include <components/AudioCaptureToolbar.hpp>
#include <components/BrowserToolbar.hpp>
#include <components/ColorSourceToolbar.hpp>
#include <components/DeviceCaptureToolbar.hpp>
#include <components/DisplayCaptureToolbar.hpp>
#include <components/GameCaptureToolbar.hpp>
#include <components/ImageSourceToolbar.hpp>
#include <components/MediaControls.hpp>
#include <components/TextSourceToolbar.hpp>
#include <components/WindowCaptureToolbar.hpp>

#include <qt-wrappers.hpp>

#ifdef __ANDROID__
#include "OBSBasicControls.hpp" // P-17-e：主界面那颗前后置按钮挂在 controlsDock 里
#endif

void OBSBasic::copyActionsDynamicProperties()
{
	// Themes need the QAction dynamic properties
	for (QAction *x : ui->scenesToolbar->actions()) {
		QWidget *temp = ui->scenesToolbar->widgetForAction(x);

		if (!temp) {
			continue;
		}

		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y.constData(), x->property(y.constData()));
		}
	}

	for (QAction *x : ui->sourcesToolbar->actions()) {
		QWidget *temp = ui->sourcesToolbar->widgetForAction(x);

		if (!temp) {
			continue;
		}

		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y.constData(), x->property(y.constData()));
		}
	}
}

void OBSBasic::ClearContextBar()
{
	QLayoutItem *la = ui->emptySpace->layout()->itemAt(0);
	if (la) {
		delete la->widget();
		ui->emptySpace->layout()->removeItem(la);
	}
}

void OBSBasic::UpdateContextBarVisibility()
{
	int width = ui->centralwidget->size().width();

	ContextBarSize contextBarSizeNew;
	if (width >= 740) {
		contextBarSizeNew = ContextBarSize_Normal;
	} else if (width >= 600) {
		contextBarSizeNew = ContextBarSize_Reduced;
	} else {
		contextBarSizeNew = ContextBarSize_Minimized;
	}

	if (contextBarSize == contextBarSizeNew) {
		return;
	}

	contextBarSize = contextBarSizeNew;
	UpdateContextBarDeferred();
}

static bool is_network_media_source(obs_source_t *source, const char *id)
{
	if (strcmp(id, "ffmpeg_source") != 0) {
		return false;
	}

	OBSDataAutoRelease s = obs_source_get_settings(source);
	bool is_local_file = obs_data_get_bool(s, "is_local_file");

	return !is_local_file;
}

void OBSBasic::UpdateContextBarDeferred(bool force)
{
	QMetaObject::invokeMethod(this, &OBSBasic::UpdateContextBar, Qt::QueuedConnection, force);
}

void OBSBasic::SourceToolBarActionsSetEnabled()
{
	bool enable = false;
	bool disableProps = false;

	OBSSceneItem item = GetCurrentSceneItem();

	if (item) {
		OBSSource source = obs_sceneitem_get_source(item);
		disableProps = !obs_source_configurable(source);

		enable = true;
	}

	if (disableProps) {
		ui->actionSourceProperties->setEnabled(false);
	} else {
		ui->actionSourceProperties->setEnabled(enable);
	}

	ui->actionRemoveSource->setEnabled(enable);
	ui->actionSourceUp->setEnabled(enable);
	ui->actionSourceDown->setEnabled(enable);

	RefreshToolBarStyling(ui->sourcesToolbar);
}

std::optional<QWidget *> OBSBasic::createContextBarWidget(obs_source_t *source)
{
	uint32_t flags = obs_source_get_output_flags(source);
	const char *id = obs_source_get_unversioned_id(source);

	if (obs_source_load_state(id) != OBS_MODULE_ENABLED) {
		return std::nullopt;
	}

	if (flags & OBS_SOURCE_CONTROLLABLE_MEDIA) {
		if (!is_network_media_source(source, id)) {
			MediaControls *contextBarWidget = new MediaControls(ui->emptySpace);
			contextBarWidget->SetSource(source);
			return contextBarWidget;
		}
	} else if (strcmp(id, "browser_source") == 0) {
		BrowserToolbar *contextBarWidget = new BrowserToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "wasapi_input_capture") == 0 || strcmp(id, "wasapi_output_capture") == 0 ||
		   strcmp(id, "coreaudio_input_capture") == 0 || strcmp(id, "coreaudio_output_capture") == 0 ||
		   strcmp(id, "pulse_input_capture") == 0 || strcmp(id, "pulse_output_capture") == 0 ||
		   strcmp(id, "alsa_input_capture") == 0) {
		AudioCaptureToolbar *contextBarWidget = new AudioCaptureToolbar(ui->emptySpace, source);
		contextBarWidget->Init();
		return contextBarWidget;

	} else if (strcmp(id, "wasapi_process_output_capture") == 0) {
		ApplicationAudioCaptureToolbar *contextBarWidget =
			new ApplicationAudioCaptureToolbar(ui->emptySpace, source);
		contextBarWidget->Init();
		return contextBarWidget;

	} else if (strcmp(id, "window_capture") == 0 || strcmp(id, "xcomposite_input") == 0) {
		WindowCaptureToolbar *contextBarWidget = new WindowCaptureToolbar(ui->emptySpace, source);
		contextBarWidget->Init();
		return contextBarWidget;

	} else if (strcmp(id, "monitor_capture") == 0 || strcmp(id, "display_capture") == 0 ||
		   strcmp(id, "xshm_input") == 0) {
		DisplayCaptureToolbar *contextBarWidget = new DisplayCaptureToolbar(ui->emptySpace, source);
		contextBarWidget->Init();
		return contextBarWidget;

	} else if (strcmp(id, "dshow_input") == 0) {
		DeviceCaptureToolbar *contextBarWidget = new DeviceCaptureToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "game_capture") == 0) {
		GameCaptureToolbar *contextBarWidget = new GameCaptureToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "image_source") == 0) {
		ImageSourceToolbar *contextBarWidget = new ImageSourceToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "color_source") == 0) {
		ColorSourceToolbar *contextBarWidget = new ColorSourceToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "text_ft2_source") == 0 || strcmp(id, "text_gdiplus") == 0) {
		TextSourceToolbar *contextBarWidget = new TextSourceToolbar(ui->emptySpace, source);
		return contextBarWidget;
	}
	return std::nullopt;
}

void OBSBasic::UpdateContextBar(bool force)
{
	SourceToolBarActionsSetEnabled();

#ifdef __ANDROID__
	/* 刻意放在下面那道 contextContainer 可见性闸门之前：前后置按钮跟"上下文工具条显示不显示"无关，
	 * 只跟"当前场景里有没有相机源"有关。 */
	UpdateAndroidFlipCameraButton();
#endif

	if (!ui->contextContainer->isVisible() && !force) {
		return;
	}

	OBSSceneItem item = GetCurrentSceneItem();

	if (item) {
		obs_source_t *source = obs_sceneitem_get_source(item);

		bool updateNeeded = true;
		QLayoutItem *la = ui->emptySpace->layout()->itemAt(0);
		if (la) {
			if (SourceToolbar *toolbar = dynamic_cast<SourceToolbar *>(la->widget())) {
				if (toolbar->GetSource() == source) {
					updateNeeded = false;
				}
			} else if (MediaControls *toolbar = dynamic_cast<MediaControls *>(la->widget())) {
				if (toolbar->GetSource() == source) {
					updateNeeded = false;
				}
			}
		}

		const char *id = obs_source_get_unversioned_id(source);
		uint32_t flags = obs_source_get_output_flags(source);
		ui->sourceInteractButton->setVisible(flags & OBS_SOURCE_INTERACTION);

		if (contextBarSize >= ContextBarSize_Reduced && (updateNeeded || force)) {
			ClearContextBar();
			std::optional<QWidget *> contextBarWidget = createContextBarWidget(source);
			if (contextBarWidget.has_value()) {
				ui->emptySpace->layout()->addWidget(contextBarWidget.value());
			}
		} else if (contextBarSize == ContextBarSize_Minimized) {
			ClearContextBar();
		}

		QIcon icon;

		if (strcmp(id, "scene") == 0) {
			icon = GetSceneIcon();
		} else if (strcmp(id, "group") == 0) {
			icon = GetGroupIcon();
		} else {
			icon = GetSourceIcon(id);
		}

		QPixmap pixmap = icon.pixmap(QSize(16, 16));
		ui->contextSourceIcon->setPixmap(pixmap);
		ui->contextSourceIconSpacer->hide();
		ui->contextSourceIcon->show();

		const char *name = obs_source_get_name(source);
		ui->contextSourceLabel->setText(name);

		ui->sourceFiltersButton->setEnabled(true);
		ui->sourcePropertiesButton->setEnabled(obs_source_configurable(source));
	} else {
		ClearContextBar();
		ui->contextSourceIcon->hide();
		ui->contextSourceIconSpacer->show();
		ui->contextSourceLabel->setText(QTStr("ContextBar.NoSelectedSource"));

		ui->sourceFiltersButton->setEnabled(false);
		ui->sourcePropertiesButton->setEnabled(false);
		ui->sourceInteractButton->setVisible(false);
	}

	if (contextBarSize == ContextBarSize_Normal) {
		ui->sourcePropertiesButton->setText(QTStr("Properties"));
		ui->sourceFiltersButton->setText(QTStr("Filters"));
		ui->sourceInteractButton->setText(QTStr("Interact"));
	} else {
		ui->sourcePropertiesButton->setText("");
		ui->sourceFiltersButton->setText("");
		ui->sourceInteractButton->setText("");
	}
}

void OBSBasic::ShowContextBar()
{
	on_toggleContextBar_toggled(true);
	ui->toggleContextBar->setChecked(true);
}

void OBSBasic::HideContextBar()
{
	on_toggleContextBar_toggled(false);
	ui->toggleContextBar->setChecked(false);
}

void OBSBasic::on_toggleContextBar_toggled(bool visible)
{
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "ShowContextToolbars", visible);
	this->ui->contextContainer->setVisible(visible);
	UpdateContextBar(true);
}

#ifdef __ANDROID__
/* ==== P-17-e 主界面一键前后置 ====
 * 三个事实决定这段的形状：
 *  1) 源 id 是 plugins/android-camera/camera2-input.c 里 obs_source_info.id = "android_camera_input"
 *     （unversioned 同名）。
 *  2) 朝向存在源设置的 "facing"，取值就是 NDK 的 ACAMERA_LENS_FACING_*（前置 0 / 后置 1 / 外接 2，
 *     构建用的 NDK 30.0.16138531 里 camera/NdkCameraMetadataTags.h 的 enum acamera_lens_facing_t）。
 *     前端 include 不到那颗 NDK 头，所以这里只能落数字 —— 换 NDK 版本时这两个常量要跟。
 *  3) 插件挑相机是"显式 camera_id 优先，其次按 facing"（camera2-input.c:575 的 choose_camera()：
 *     :584 比 id、:596 比朝向、:603 才报警退回第一台），所以翻朝向必须同时把 camera_id 清回自动，
 *     否则在属性面板里点过具体那台的人按了没反应。 */

static const char *const ANDROID_CAMERA_ID = "android_camera_input";
static const long long ANDROID_FACING_FRONT = 0;
static const long long ANDROID_FACING_BACK = 1;

static bool enumAndroidCamera(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	obs_source_t **out = static_cast<obs_source_t **>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!*out && src && strcmp(obs_source_get_unversioned_id(src), ANDROID_CAMERA_ID) == 0)
		*out = obs_source_get_ref(src); /* sceneitem 给的是借用指针，这里要自带一份 */
	return true;
}

/* 当前场景顶层的第一颗相机源，自带引用（调用方 release）；没有则 nullptr。
 * 只看顶层不下钻进分组：源被收进分组后，这颗按钮翻的是主界面上看不见的东西，不如不出现。 */
static obs_source_t *firstAndroidCamera(OBSScene scene)
{
	obs_source_t *found = nullptr;
	if (scene)
		obs_scene_enum_items(scene, enumAndroidCamera, &found);
	return found;
}

void OBSBasic::UpdateAndroidFlipCameraButton()
{
	OBSBasicControls *controls =
		controlsDock ? qobject_cast<OBSBasicControls *>(controlsDock->widget()) : nullptr;
	if (!controls || !controls->androidFlipCameraButton)
		return;

	OBSSourceAutoRelease cam = firstAndroidCamera(GetCurrentScene());
	controls->androidFlipCameraButton->setVisible(cam != nullptr);
}

void OBSBasic::FlipAndroidCamera()
{
	OBSSourceAutoRelease cam = firstAndroidCamera(GetCurrentScene());
	if (!cam) {
		blog(LOG_WARNING, "android-camera: 一键前后置点了，但当前场景里没有相机源");
		return;
	}

	obs_data_t *s = obs_source_get_settings(cam);
	const long long cur = obs_data_get_int(s, "facing");
	/* 只有"确实是前置"才翻到后置；-1（自动档）和 2（外接）一律翻向前置 —— 自动档实际开成的多半是
	 * 后置那颗，翻向前置才有肉眼看得见的变化，否则按一下没反应会被当成按钮坏了。 */
	const long long next = (cur == ANDROID_FACING_FRONT) ? ANDROID_FACING_BACK : ANDROID_FACING_FRONT;

	obs_data_set_string(s, "camera_id", "");
	obs_data_set_int(s, "facing", next);
	obs_source_update(cam, s);
	obs_data_release(s);

	blog(LOG_INFO, "android-camera: 一键前后置 —— 源 '%s' 朝向 %lld → %lld（0=前置 1=后置 2=外接 -1=自动）",
	     obs_source_get_name(cam), cur, next);

	/* 按钮上刻意不显示"现在是前还是后"：facing 只是"想要哪一面"，真正开成哪颗要等视频线程跑完
	 * update、由插件在 open_capture_locked 里把实况回写进同一份设置。显示一个会滞后一拍的中间态，
	 * 不如只显示动作。 */
	SaveProject();
}
#endif

