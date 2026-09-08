#include "SourceTree.hpp"
#include "SourceTreeDelegate.hpp"
#include "moc_SourceTreeDelegate.cpp"

SourceTreeDelegate::SourceTreeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize SourceTreeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	SourceTree *tree = qobject_cast<SourceTree *>(parent());
	QWidget *item = tree->indexWidget(index);

	if (!item) {
		return QStyledItemDelegate::sizeHint(option, index);
	}

	QSize hint = item->sizeHint();
#ifdef __ANDROID__
	/* 3-4 命中区：源列表一行只有 30 逻辑像素（=30 dp，Android 触摸下限 48）。样式表那条
	 * QListView::item{min-height} 在这儿没用 —— 本函数返回的就是行高，视图层的话被它盖掉。
	 * 34i 实测：加了样式表行高还是 30→30。所以在这唯一的出口上夹一刀。 */
	hint.setHeight(qMax(hint.height(), 48));
#endif
	return hint;
}
