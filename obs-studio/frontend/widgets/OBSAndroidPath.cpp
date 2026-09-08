#include "OBSAndroidPath.hpp"

#include "OBSAndroidDisplay.hpp" // obsAndroidDisplayLog：与 3-3/3-4 其余探针同一个 tag，一条 logcat 看全

#include <qt-wrappers.hpp> // OBSMessageBox

#include <QApplication>
#include <QClipboard>

void obsAndroidShowLocalPath(QWidget *parent, const QString &title, const QString &path)
{
	QClipboard *clipboard = QApplication::clipboard();
	clipboard->setText(path);

	// 回读自证：Android 上剪贴板写入可能被平台规则挡掉，所以"已复制"这句话只在回读一致时才说。
	const bool copied = (clipboard->text(QClipboard::Clipboard) == path);
	obsAndroidDisplayLog("3-4 openUrl 替代：%s → %s | 剪贴板回读=%s", title.toUtf8().constData(),
			     path.toUtf8().constData(), copied ? "一致" : "不一致");

	OBSMessageBox::information(parent, title,
				   QString("%1\n\nAndroid 上没有文件管理器。%2")
					   .arg(path, copied ? "\n\n路径已复制到剪贴板，可粘贴到其他应用。" : ""));
}
