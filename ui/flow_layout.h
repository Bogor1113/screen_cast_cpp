#pragma once
// ui/flow_layout.h -- a simple flowing layout: places child widgets left-to-right and
// wraps to the next line when the row is full (like a "tag cloud"). Qt has no built-in
// flow layout, this is the classic Qt example implementation adapted for the client chips.
#include <QLayout>
#include <QStyle>
#include <QWidget>

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent, int margin = 0, int hSpacing = 8, int vSpacing = 8)
        : QLayout(parent), hSpace(hSpacing), vSpace(vSpacing) {
        setContentsMargins(margin, margin, margin, margin);
    }
    ~FlowLayout() override {
        while (QLayoutItem* item = takeAt(0)) delete item;
    }

    void addItem(QLayoutItem* item) override { itemList.append(item); }
    int count() const override { return itemList.size(); }
    QLayoutItem* itemAt(int index) const override {
        return (index >= 0 && index < itemList.size()) ? itemList.at(index) : nullptr;
    }
    QLayoutItem* takeAt(int index) override {
        return (index >= 0 && index < itemList.size()) ? itemList.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), true); }
    QSize minimumSize() const override {
        QSize size;
        for (const QLayoutItem* item : itemList)
            size = size.expandedTo(item->minimumSize());
        const QMargins m = contentsMargins();
        return size + QSize(m.left() + m.right(), m.top() + m.bottom());
    }
    QSize sizeHint() const override { return minimumSize(); }
    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

private:
    int doLayout(const QRect& rect, bool testOnly) const {
        int left, top, right, bottom;
        getContentsMargins(&left, &top, &right, &bottom);
        const QRect eff = rect.adjusted(+left, +top, -right, -bottom);
        int x = eff.x();
        int y = eff.y();
        int lineHeight = 0;
        for (QLayoutItem* item : itemList) {
            const QSize sz = item->sizeHint();
            if (x + sz.width() > eff.right() + 1 && x > eff.x()) {
                x = eff.x();
                y = y + lineHeight + vSpace;
                lineHeight = 0;
            }
            if (!testOnly) item->setGeometry(QRect(QPoint(x, y), sz));
            x += sz.width() + hSpace;
            lineHeight = qMax(lineHeight, sz.height());
        }
        return y + lineHeight - rect.y() + bottom;
    }

    QList<QLayoutItem*> itemList;
    int hSpace, vSpace;
};
