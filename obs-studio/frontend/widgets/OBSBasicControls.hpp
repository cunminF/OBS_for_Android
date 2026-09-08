#pragma once

#include "ui_OBSBasicControls.h"

#include <QFrame>
#include <QPointer>
#include <QScopedPointer>

#include <memory>

class OBSBasic;

class OBSBasicControls : public QFrame {
	Q_OBJECT

	std::unique_ptr<Ui::OBSBasicControls> ui;

	QScopedPointer<QMenu> streamButtonMenu;
	QPointer<QAction> startStreamAction;
	QPointer<QAction> stopStreamAction;

private slots:
	void StreamingPreparing();
	void StreamingStarting(bool broadcastAutoStart);
	void StreamingStarted(bool withDelay);
	void StreamingStopping();
	void StreamingStopped(bool withDelay);

	void BroadcastStreamReady(bool ready);
	void BroadcastStreamActive();
	void BroadcastStreamStarted(bool autoStop);

	void RecordingStarted(bool pausable);
	void RecordingPaused();
	void RecordingUnpaused();
	void RecordingStopping();
	void RecordingStopped();

	void ReplayBufferStarted();
	void ReplayBufferStopping();
	void ReplayBufferStopped();

	void VirtualCamStarted();
	void VirtualCamStopped();

	void UpdateStudioModeState(bool enabled);

	void EnableBroadcastFlow(bool enabled);
	void EnableReplayBufferButtons(bool enabled);
	void EnableVirtualCamButtons();

public:
	OBSBasicControls(OBSBasic *main);
	inline ~OBSBasicControls() {}

#ifdef __ANDROID__
	/* P-17-e：主界面那颗"前后置"按钮。父对象就是本控件（跟着 controlsDock 一起没），
	 * 指针只给 OBSBasic 用来切显隐 —— 当前用哪颗镜头的真相在源的设置里，不在这里。 */
	QPushButton *androidFlipCameraButton = nullptr;
#endif

signals:
	void StreamButtonClicked();
	void BroadcastButtonClicked();
	void RecordButtonClicked();
	void PauseRecordButtonClicked();
	void ReplayBufferButtonClicked();
	void SaveReplayBufferButtonClicked();
	void VirtualCamButtonClicked();
	void VirtualCamConfigButtonClicked();
	void StudioModeButtonClicked();
	void SettingsButtonClicked();
#ifdef __ANDROID__
	void FlipCameraButtonClicked();
#endif

	void StartStreamMenuActionClicked();
	void StopStreamMenuActionClicked();
	void ForceStopStreamMenuActionClicked();
};
