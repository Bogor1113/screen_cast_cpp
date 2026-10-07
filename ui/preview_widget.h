#pragma once
// ui/preview_widget.h -- teacher preview pane (Qt): paints the thread-safe PreviewBuffer
#include <QWidget>
#include "ui/preview.h"

class QPaintEvent;

// shows the desktop preview: paints the PreviewBuffer (BGRA thumbnail) keeping aspect
// ratio, centered, with optional dirty-region outlines. Painted at ~15fps by the window timer.
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    // attach the shared preview buffer (written by the capture thread via Preview_Update)
    void setBuffer(PreviewBuffer* pb) { pb_ = pb; }

    // "LIVE" ring state: border turns accent-blue while streaming
    void setStreaming(bool on) { streaming_ = on; update(); }

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    PreviewBuffer* pb_ = nullptr;
    bool streaming_ = false;
};