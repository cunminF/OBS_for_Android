#include "OBSBasicControls.hpp"
#include "OBSBasic.hpp"
#include "qt-wrappers.hpp"

#ifdef __ANDROID__
#include <QGridLayout> // 3-4 16:10 校形：控制按钮改两列
#include <QTimer> // 3-4：☰ 菜单里第二层 exec() 要推到下一轮事件循环
#include <QPainter> // 3-4：☰ 图标自己画（设备字体没有 U+2630）
#include <QPixmap>
#include "OBSAndroidDisplay.hpp" // 3-4 探针日志（logcat tag OBS-display）
#include <functional>
#endif

#include "moc_OBSBasicControls.cpp"

OBSBasicControls::OBSBasicControls(OBSBasic *main) : QFrame(nullptr), ui(new Ui::OBSBasicControls)
{
	/* Create UI elements */
	ui->setupUi(this);

	streamButtonMenu.reset(new QMenu());
	startStreamAction = streamButtonMenu->addAction(QTStr("Basic.Main.StartStreaming"));
	stopStreamAction = streamButtonMenu->addAction(QTStr("Basic.Main.StopStreaming"));
	QAction *forceStopStreamAction = streamButtonMenu->addAction(QTStr("Basic.Main.ForceStopStreaming"));

	/* Transfer buttons signals as OBSBasicControls signals */
	connect(
		ui->streamButton, &QPushButton::clicked, this, [this]() { emit this->StreamButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->broadcastButton, &QPushButton::clicked, this, [this]() { emit this->BroadcastButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->recordButton, &QPushButton::clicked, this, [this]() { emit this->RecordButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->pauseRecordButton, &QPushButton::clicked, this, [this]() { emit this->PauseRecordButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->replayBufferButton, &QPushButton::clicked, this,
		[this]() { emit this->ReplayBufferButtonClicked(); }, Qt::DirectConnection);
	connect(
		ui->saveReplayButton, &QPushButton::clicked, this,
		[this]() { emit this->SaveReplayBufferButtonClicked(); }, Qt::DirectConnection);
	connect(
		ui->virtualCamButton, &QPushButton::clicked, this, [this]() { emit this->VirtualCamButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->virtualCamConfigButton, &QPushButton::clicked, this,
		[this]() { emit this->VirtualCamConfigButtonClicked(); }, Qt::DirectConnection);
	connect(
		ui->modeSwitch, &QPushButton::clicked, this, [this]() { emit this->StudioModeButtonClicked(); },
		Qt::DirectConnection);
	connect(
		ui->settingsButton, &QPushButton::clicked, this, [this]() { emit this->SettingsButtonClicked(); },
		Qt::DirectConnection);

#ifdef __ANDROID__
	/* 3-4 触摸：`OBSBasic.ui:711` 那个 QMenuBar 在 Android 上整个不渲染 —— 2026-09-05 20:05 实测，
	 * 状态条以下整条顶边是空的，没有菜单文字，文件/编辑/视图/项目/帮助（"显示录制文件""显示日志"
	 * "退出""工作室模式"这些的唯一入口）全都点不到。这里补一个 ☰ 按钮把它们搬进弹出菜单。
	 *
	 * 只做"triggered 里再 exec() 子菜单"，不做 `androidMenu->addMenu(sub)` —— 后者会把 sub 从
	 * menubar 抢走，menubar 一空就可能把布局里那几十像素吐回去，预览区跟着抖；本轮只要菜单可达。
	 * QAction 用的还是 OBSBasic 那一批，启用/禁用、勾选、槽连接、动态改文案全都自动跟着走。
	 * singleShot(0) 是把第二层 exec() 推到外层菜单关掉之后，别套嵌循环。 */
	auto *menuButton = new QPushButton(this);
	menuButton->setObjectName("androidMenuButton");

	/* 图标不能指望字体：U+2630 在设备字体里没有，20:26 实测按钮上是个豆腐块。画三条杠最省事。
	 * 颜色取按钮自己的 ButtonText —— 但主题是在 OBSApp::SetTheme 里 setPalette 的，可能晚于本构造函数，
	 * 所以再补一次 singleShot(0) 用落定后的 palette 重画。 */
	auto rebuildIcon = [menuButton] {
		QColor ink = menuButton->palette().color(QPalette::ButtonText);
		if (!ink.isValid())
			ink = QColor(0xcc, 0xcc, 0xcc);
		QPixmap pm(48, 48);
		pm.fill(Qt::transparent);
		QPainter p(&pm);
		p.setPen(QPen(ink, 5, Qt::SolidLine, Qt::FlatCap));
		for (int i = 0; i < 3; ++i) {
			const int y = 12 + i * 12;
			p.drawLine(10, y, 38, y);
		}
		p.end();
		menuButton->setIcon(QIcon(pm));
		menuButton->setIconSize(QSize(24, 24));
	};
	rebuildIcon();
	QTimer::singleShot(0, menuButton, rebuildIcon);

	QMenu *androidMenu = new QMenu(menuButton);
	const auto barActions = main->menuBar()->actions();
	for (QAction *act : barActions) {
		QMenu *sub = act->menu();
		if (!sub || !sub->menuAction()->isVisible())
			continue;
		QAction *item = androidMenu->addAction(sub->menuAction()->text());
		connect(
			item, &QAction::triggered, menuButton,
			[sub, menuButton] {
				const QPoint at = menuButton->mapToGlobal(QPoint(0, menuButton->height()));
				QTimer::singleShot(0, menuButton, [sub, at] {
					obsAndroidDisplayLog("3-4 ☰ 展开子菜单 %s（%d 项）@(%d,%d)",
							     sub->title().toUtf8().constData(),
							     int(sub->actions().size()), at.x(), at.y());
					sub->exec(at);
				});
			});
	}
	menuButton->setMenu(androidMenu);
	ui->buttonsVLayout->insertWidget(0, menuButton);
	obsAndroidDisplayLog("3-4 ☰ 菜单入口：菜单栏 %d 项 → 镜像出 %d 个子菜单", int(barActions.size()),
			     int(androidMenu->actions().size()));

	/* P-17-e 一键前后置。不塞进 ☰：手机上这是最高频的单一动作（自拍↔拍桌面来回翻），走菜单要两下。
	 * 这里也不缓存"现在是前还是后" —— 那份真相唯一存在的地方是源的 settings.facing（插件在
	 * open_capture_locked 里按真正打开的镜头回写），前端点了就翻转它。两边各存一份迟早各说各话。
	 * 起始隐藏：当前场景里有没有这颗源，得由 OBSBasic 查过场景才知道（UpdateContextBar 里刷）。 */
	auto *flipButton = new QPushButton(this);
	flipButton->setObjectName("androidFlipCameraButton");
	flipButton->setText(QString::fromUtf8("前后置")); // 无对应 locale 键，且只在 Android 分支出现
	flipButton->setVisible(false);
	androidFlipCameraButton = flipButton;
	/* 必须真的挂进 buttonsVLayout：下面校形那段是先 harvest 再按偏好重排，没在竖列里出现过的控件
	 * 会被 order.removeAll() 判 false 而**不进任何行** —— 那样它只是 controls 的一个散养子 widget，
	 * 一显形就盖在 (0,0) 上。 */
	ui->buttonsVLayout->insertWidget(1, flipButton);
	connect(
		flipButton, &QPushButton::clicked, this, [this]() { emit this->FlipCameraButtonClicked(); },
		Qt::DirectConnection);

	/* 3-4 16:10 校形。下排三个 dock 的高度是被"控制按钮"这一竖列顶住的 —— 1600x1000 实测下排
	 * 325 px（☰/直播/录制/工作室/设置 五行 + 标题栏），而同一排里混音器空着却占 710 px 宽。
	 * 预览面因此只有 1136x491 = 2.31:1，16:9 的画面在里面左右各黑 150 px。
	 * 这里把这一竖列改成两列（行数 5→3），控件一个都不动：还是 .ui 里那批 QPushButton，
	 * 信号、文案、studio 模式的动态显隐全都照旧；只换摆法。
	 *
	 * 别用 QGridLayout：34g 实测两列网格在这排 314 逻辑像素宽里活不下来 —— 网格按每个控件的
	 * sizeHint 最小值定列宽，两列各要 ~150，加上分隔条就溢出，第二列被压成 0 宽，
	 * **设置按钮直接看不见**（截图 b34g-01-169.png）。所以改成"每行一个 QHBoxLayout +
	 * 行内两个按钮"，并把横向策略统一压成 Ignored（= 别拿文字宽度来要最小宽度），两列才真等分。 */
	{
		QList<QWidget *> order;
		std::function<void(QLayout *)> harvest = [&](QLayout *l) {
			while (QLayoutItem *it = l->takeAt(0)) {
				if (QWidget *w = it->widget())
					order.append(w);
				else if (QLayout *sub = it->layout())
					harvest(sub);
				delete it;
			}
		};
		harvest(ui->buttonsVLayout);

		/* 34h 实测：只把 ☰/设置 提到前面还不够 —— QBoxLayout 会把隐藏控件那份格子让给同排兄弟，
		 * 所以 (直播, 隐藏的广播) 里直播又铺满整行，静息行数还是 4，预览面只多出 9 px。
		 * 要真减行，得让**静息可见**的按钮自己凑成一对：直播+录制 一行、工作室模式 一行 → 3 行。
		 * 录音中/回放中冒出来的按钮落在后面的行里，一行里只有一个可见就铺满，不影响静息布局。 */
		const QList<QWidget *> preference = {menuButton,
						     flipButton,
						     ui->settingsButton,
						     ui->streamButton,
						     ui->recordButton,
						     ui->modeSwitch,
						     ui->broadcastButton,
						     ui->pauseRecordButton,
						     ui->replayBufferButton,
						     ui->saveReplayButton,
						     ui->virtualCamButton,
						     ui->virtualCamConfigButton};
		QList<QWidget *> paired;
		for (QWidget *w : preference) {
			if (order.removeAll(w))
				paired.append(w);
		}
		paired.append(order); // .ui 里多出来的、没列进偏好表的控件照旧排在尾部
		order = paired;

		for (QWidget *w : order)
			w->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

		for (int i = 0; i < order.size(); i += 2) {
			auto *row = new QHBoxLayout;
			row->setContentsMargins(0, 0, 0, 0);
			row->setSpacing(2);
			row->addWidget(order[i], 1);
			if (i + 1 < order.size())
				row->addWidget(order[i + 1], 1);
			ui->buttonsVLayout->addLayout(row);
		}
		ui->buttonsVLayout->addStretch(1); // 按钮保持自然高度，别跟着 dock 一起被拉长
		obsAndroidDisplayLog("3-4 校形：控制按钮 %d 个控件 → %d 行两列", int(order.size()),
				     (int(order.size()) + 1) / 2);
	}
#endif

	/* Transfer menu actions signals as OBSBasicControls signals */
	connect(
		startStreamAction.get(), &QAction::triggered, this,
		[this]() { emit this->StartStreamMenuActionClicked(); }, Qt::DirectConnection);
	connect(
		stopStreamAction.get(), &QAction::triggered, this,
		[this]() { emit this->StopStreamMenuActionClicked(); }, Qt::DirectConnection);
	connect(
		forceStopStreamAction, &QAction::triggered, this,
		[this]() { emit this->ForceStopStreamMenuActionClicked(); }, Qt::DirectConnection);

	/* Set up default visibility */
	ui->broadcastButton->setVisible(false);
	ui->pauseRecordButton->setVisible(false);
	ui->replayBufferButton->setVisible(false);
	ui->saveReplayButton->setVisible(false);
	ui->virtualCamButton->setVisible(false);
	ui->virtualCamConfigButton->setVisible(false);

	/* Set up state update connections */
	connect(main, &OBSBasic::StreamingPreparing, this, &OBSBasicControls::StreamingPreparing);
	connect(main, &OBSBasic::StreamingStarting, this, &OBSBasicControls::StreamingStarting);
	connect(main, &OBSBasic::StreamingStarted, this, &OBSBasicControls::StreamingStarted);
	connect(main, &OBSBasic::StreamingStopping, this, &OBSBasicControls::StreamingStopping);
	connect(main, &OBSBasic::StreamingStopped, this, &OBSBasicControls::StreamingStopped);

	connect(main, &OBSBasic::BroadcastStreamReady, this, &OBSBasicControls::BroadcastStreamReady);
	connect(main, &OBSBasic::BroadcastStreamActive, this, &OBSBasicControls::BroadcastStreamActive);
	connect(main, &OBSBasic::BroadcastStreamStarted, this, &OBSBasicControls::BroadcastStreamStarted);

	connect(main, &OBSBasic::RecordingStarted, this, &OBSBasicControls::RecordingStarted);
	connect(main, &OBSBasic::RecordingPaused, this, &OBSBasicControls::RecordingPaused);
	connect(main, &OBSBasic::RecordingUnpaused, this, &OBSBasicControls::RecordingUnpaused);
	connect(main, &OBSBasic::RecordingStopping, this, &OBSBasicControls::RecordingStopping);
	connect(main, &OBSBasic::RecordingStopped, this, &OBSBasicControls::RecordingStopped);

	connect(main, &OBSBasic::ReplayBufStarted, this, &OBSBasicControls::ReplayBufferStarted);
	connect(main, &OBSBasic::ReplayBufStopping, this, &OBSBasicControls::ReplayBufferStopping);
	connect(main, &OBSBasic::ReplayBufStopped, this, &OBSBasicControls::ReplayBufferStopped);

	connect(main, &OBSBasic::VirtualCamStarted, this, &OBSBasicControls::VirtualCamStarted);
	connect(main, &OBSBasic::VirtualCamStopped, this, &OBSBasicControls::VirtualCamStopped);

	connect(main, &OBSBasic::PreviewProgramModeChanged, this, &OBSBasicControls::UpdateStudioModeState);

	/* Set up enablement connection */
	connect(main, &OBSBasic::BroadcastFlowEnabled, this, &OBSBasicControls::EnableBroadcastFlow);
	connect(main, &OBSBasic::ReplayBufEnabled, this, &OBSBasicControls::EnableReplayBufferButtons);
	connect(main, &OBSBasic::VirtualCamEnabled, this, &OBSBasicControls::EnableVirtualCamButtons);
}

void OBSBasicControls::StreamingPreparing()
{
	ui->streamButton->setEnabled(false);
	ui->streamButton->setText(QTStr("Basic.Main.PreparingStream"));
}

void OBSBasicControls::StreamingStarting(bool broadcastAutoStart)
{
	ui->streamButton->setText(QTStr("Basic.Main.Connecting"));

	if (!broadcastAutoStart) {
		// well, we need to disable button while stream is not active
		ui->broadcastButton->setEnabled(false);

		ui->broadcastButton->setText(QTStr("Basic.Main.StartBroadcast"));

		ui->broadcastButton->setProperty("broadcastState", "ready");
		ui->broadcastButton->style()->unpolish(ui->broadcastButton);
		ui->broadcastButton->style()->polish(ui->broadcastButton);
	}
}

void OBSBasicControls::StreamingStarted(bool withDelay)
{
	ui->streamButton->setEnabled(true);
	setClasses(ui->streamButton, "state-active");
	ui->streamButton->setText(QTStr("Basic.Main.StopStreaming"));

	if (withDelay) {
		ui->streamButton->setMenu(streamButtonMenu.get());
		startStreamAction->setVisible(false);
		stopStreamAction->setVisible(true);
	}
}

void OBSBasicControls::StreamingStopping()
{
	ui->streamButton->setText(QTStr("Basic.Main.StoppingStreaming"));
}

void OBSBasicControls::StreamingStopped(bool withDelay)
{
	ui->streamButton->setEnabled(true);
	setClasses(ui->streamButton, "");
	ui->streamButton->setText(QTStr("Basic.Main.StartStreaming"));

	if (withDelay) {
		if (!ui->streamButton->menu()) {
			ui->streamButton->setMenu(streamButtonMenu.get());
		}

		startStreamAction->setVisible(true);
		stopStreamAction->setVisible(false);
	} else {
		ui->streamButton->setMenu(nullptr);
	}
}

void OBSBasicControls::BroadcastStreamReady(bool ready)
{
	setClasses(ui->broadcastButton, ready ? "state-active" : "");
}

void OBSBasicControls::BroadcastStreamActive()
{
	ui->broadcastButton->setEnabled(true);
}

void OBSBasicControls::BroadcastStreamStarted(bool autoStop)
{
	ui->broadcastButton->setText(QTStr(autoStop ? "Basic.Main.AutoStopEnabled" : "Basic.Main.StopBroadcast"));
	if (autoStop) {
		ui->broadcastButton->setEnabled(false);
	}

	ui->broadcastButton->setProperty("broadcastState", "active");
	ui->broadcastButton->style()->unpolish(ui->broadcastButton);
	ui->broadcastButton->style()->polish(ui->broadcastButton);
}

void OBSBasicControls::RecordingStarted(bool pausable)
{
	setClasses(ui->recordButton, "state-active");
	ui->recordButton->setText(QTStr("Basic.Main.StopRecording"));

	if (pausable) {
		ui->pauseRecordButton->setVisible(pausable);
		RecordingUnpaused();
	}
}

void OBSBasicControls::RecordingPaused()
{
	QString text = QTStr("Basic.Main.UnpauseRecording");

	setClasses(ui->pauseRecordButton, "icon-media-pause state-active");
	ui->pauseRecordButton->setAccessibleName(text);
	ui->pauseRecordButton->setToolTip(text);

	ui->saveReplayButton->setEnabled(false);
}

void OBSBasicControls::RecordingUnpaused()
{
	QString text = QTStr("Basic.Main.PauseRecording");

	setClasses(ui->pauseRecordButton, "icon-media-pause");
	ui->pauseRecordButton->setAccessibleName(text);
	ui->pauseRecordButton->setToolTip(text);

	ui->saveReplayButton->setEnabled(true);
}

void OBSBasicControls::RecordingStopping()
{
	ui->recordButton->setText(QTStr("Basic.Main.StoppingRecording"));
}

void OBSBasicControls::RecordingStopped()
{
	setClasses(ui->recordButton, "");
	ui->recordButton->setText(QTStr("Basic.Main.StartRecording"));

	ui->pauseRecordButton->setVisible(false);
}

void OBSBasicControls::ReplayBufferStarted()
{
	setClasses(ui->replayBufferButton, "state-active");
	ui->replayBufferButton->setText(QTStr("Basic.Main.StopReplayBuffer"));

	ui->saveReplayButton->setVisible(true);
}

void OBSBasicControls::ReplayBufferStopping()
{
	ui->replayBufferButton->setText(QTStr("Basic.Main.StoppingReplayBuffer"));
}

void OBSBasicControls::ReplayBufferStopped()
{
	setClasses(ui->replayBufferButton, "");
	ui->replayBufferButton->setText(QTStr("Basic.Main.StartReplayBuffer"));

	ui->saveReplayButton->setVisible(false);
}

void OBSBasicControls::VirtualCamStarted()
{
	setClasses(ui->virtualCamButton, "state-active");
	ui->virtualCamButton->setText(QTStr("Basic.Main.StopVirtualCam"));
}

void OBSBasicControls::VirtualCamStopped()
{
	setClasses(ui->virtualCamButton, "");
	ui->virtualCamButton->setText(QTStr("Basic.Main.StartVirtualCam"));
}

void OBSBasicControls::UpdateStudioModeState(bool enabled)
{
	setClasses(ui->modeSwitch, enabled ? "state-active" : "");
}

void OBSBasicControls::EnableBroadcastFlow(bool enabled)
{
	ui->broadcastButton->setVisible(enabled);
	ui->broadcastButton->setEnabled(enabled);

	ui->broadcastButton->setText(QTStr("Basic.Main.SetupBroadcast"));

	ui->broadcastButton->setProperty("broadcastState", "idle");
	ui->broadcastButton->style()->unpolish(ui->broadcastButton);
	ui->broadcastButton->style()->polish(ui->broadcastButton);
}

void OBSBasicControls::EnableReplayBufferButtons(bool enabled)
{
	ui->replayBufferButton->setVisible(enabled);
}

void OBSBasicControls::EnableVirtualCamButtons()
{
	ui->virtualCamButton->setVisible(true);
	ui->virtualCamConfigButton->setVisible(true);
}
