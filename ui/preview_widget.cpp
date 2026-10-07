// ui/preview_widget.cpp -- teacher preview pane painting (QPainter version of the old GDI Preview_Draw)
#include "ui/preview_widget.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QImage>
#include <algorithm>
#include <cstring>

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void PreviewWidget::paintEvent(QPaintEvent* ev) {
    (void)ev;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    QRect rc = rect();
    // rounded panel base
    QPainterPath panel;
    panel.addRoundedRect(QRectF(rc).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    p.fillPath(panel, QColor(0x1A, 0x1D, 0x2E));

    std::vector<uint8_t> img;
    int iw = 0, ih = 0;
    std::vector<RECT> dr;
    bool showDirty = false;
    if (pb_) {
        std::lock_guard<std::mutex> lk(pb_->mtx);
        img = pb_->bgra; iw = pb_->w; ih = pb_->h; dr = pb_->dirty;
        showDirty = pb_->showDirty.load();
    }

    if (iw <= 0 || ih <= 0 || img.empty()) {
        p.setPen(QColor(0x8A, 0x92, 0xB0));
        p.drawText(rc, Qt::AlignCenter, QStringLiteral("等待画面…(启动推流后显示桌面预览)"));
        p.setPen(QColor(streaming_ ? 0x2A : 0x26, streaming_ ? 0x6E : 0x2B, streaming_ ? 0x93 : 0x45));
        p.setBrush(Qt::NoBrush);
        p.drawPath(panel);
        return;
    }

    // keep aspect ratio, centered
    double s = std::min((double)rc.width() / iw, (double)rc.height() / ih);
    int dw = (int)(iw * s), dh = (int)(ih * s);
    int dx = rc.left() + (rc.width() - dw) / 2, dy = rc.top() + (rc.height() - dh) / 2;

    // clip to the rounded panel, then draw the picture inside
    p.save();
    p.setClipPath(panel);
    // preview buffer is BGRA byte order (B,G,R,A); on little-endian that is exactly the
    // memory layout of Format_ARGB32 (0xAARRGGBB stored as BB GG RR AA). Qt 6 removed the
    // legacy Format_BGRA8888 name, so use Format_ARGB32 here.
    QImage qimg(img.data(), iw, ih, iw * 4, QImage::Format_ARGB32);
    p.drawImage(QRect(dx, dy, dw, dh), qimg);

    // differential changed regions (green border, thumbnail coords -> widget coords)
    if (showDirty && !dr.empty()) {
        QPen pen(QColor(0, 220, 90), 2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (const auto& r : dr) {
            int x0 = dx + (int)(r.left * s), y0 = dy + (int)(r.top * s);
            int x1 = dx + (int)(r.right * s), y1 = dy + (int)(r.bottom * s);
            if (x1 - x0 < 3) x1 = x0 + 3;
            if (y1 - y0 < 3) y1 = y0 + 3;
            p.drawRect(x0, y0, x1 - x0, y1 - y0);
        }
    }
    p.restore();

    // border: accent-blue while streaming, soft gray when idle
    p.setPen(QPen(QColor(streaming_ ? 0x3E : 0x26, streaming_ ? 0x8F : 0x2B, streaming_ ? 0xB4 : 0x45), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(panel);
}
