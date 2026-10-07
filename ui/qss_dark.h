#pragma once
// ui/qss_dark.h -- modern dark theme (v3): deep ink canvas, elevated glass cards,
// indigo→violet gradient accent, gradient primary/stop button, refined hover/press/focus
// micro-interactions. Pure QSS: no cpp logic touched, all objectName / dynamic-property
// selectors (startBtn, starting=true, topBar, clientCard, previewFrame, connectDialog) kept.
#include <QApplication>
#include <QPalette>
#include <QColor>

static const char kQssDark[] = R"QSS(
* {
    font-family: "Microsoft YaHei UI", "Segoe UI", "Microsoft YaHei";
    font-size: 13px;
    color: #E8EDF7;
}

/* ---------- canvas ---------- */
QMainWindow, QDialog, QWidget#centralRoot {
    background-color: #0A0E17;
}

/* ---------- header (status + local IP) ---------- */
QWidget#headerBar {
    background: transparent;
}

/* ---------- top control bar (elevated card) ---------- */
QWidget#topBar {
    background-color: #121826;
    border: 1px solid #222B3D;
    border-radius: 12px;
}
QWidget#topBar QLabel {
    color: #8B95A8;
    background: transparent;
    font-size: 12px;
}

/* ---------- line edits ---------- */
QLineEdit {
    background-color: #0C111C;
    border: 1px solid #27324A;
    border-radius: 8px;
    padding: 7px 12px;
    selection-background-color: #4C6FFF;
    selection-color: #FFFFFF;
    color: #E8EDF7;
}
QLineEdit:hover:not(:focus) {
    border: 1px solid #344363;
}
QLineEdit:focus {
    border: 1px solid #5B8DEF;
    background-color: #0D1420;
}
QLineEdit:disabled {
    color: #5C6679;
    background-color: #0E141F;
}

/* ---------- buttons ---------- */
/* IMPORTANT (Qt 6 / Fusion): QPushButton's PE_PanelButton draws its OWN background, which
   overrides QSS `background-image` (gradients do NOT work on buttons). Only `background-color`
   + `setAttribute(WA_StyledBackground)` reliably colours a button. Verified with an inline
   test button: background-color works, background-image(qlineargradient) is ignored. */
QPushButton {
    background-color: #1A2130;
    border: 1px solid #2A354D;
    border-radius: 8px;
    padding: 7px 18px;
    color: #E8EDF7;
}
QPushButton:hover {
    background-color: #222B3F;
    border: 1px solid #3A4A6B;
}
QPushButton:pressed {
    background-color: #141A28;
    border: 1px solid #2A354D;
}
QPushButton:focus {
    border: 1px solid #5B8DEF;
}
QPushButton:disabled {
    color: #5C6679;
    background-color: #141A26;
    border: 1px solid #222B3D;
}

/* primary action button -- indigo→violet gradient (start = gradient, stop = red) */
QPushButton#startBtn {
    background-color: #4C8DFF;
    border: 1px solid #5B8DEF;
    border-radius: 8px;
    color: #FFFFFF;
    font-weight: 600;
    padding: 7px 26px;
}
QPushButton#startBtn:hover {
    background-color: #5B9DFF;
    border: 1px solid #6BA3FF;
}
QPushButton#startBtn:pressed {
    background-color: #3D74D8;
    border: 1px solid #4C7AE0;
}
QPushButton#startBtn:disabled {
    background: #1A2130;
    border: 1px solid #2A354D;
    color: #5C6679;
}
/* streaming state: stop = red gradient */
QPushButton#startBtn[starting="true"] {
    background-color: #E5534B;
    border: 1px solid #E5534B;
}
QPushButton#startBtn[starting="true"]:hover {
    background-color: #F26B60;
    border: 1px solid #F26B60;
}
QPushButton#startBtn[starting="true"]:pressed {
    background-color: #C94138;
    border: 1px solid #C94138;
}

/* connect-dialog primary button (same gradient family) */
QPushButton#connectBtn {
    background-color: #4C8DFF;
    border: 1px solid #5B8DEF;
    border-radius: 8px;
    color: #FFFFFF;
    font-weight: 600;
    padding: 7px 24px;
}
QPushButton#connectBtn:hover {
    background-color: #5B9DFF;
    border: 1px solid #6BA3FF;
}
QPushButton#connectBtn:pressed {
    background-color: #3D74D8;
    border: 1px solid #4C7AE0;
}

/* dialog buttons never fall back to system-white */
QDialog QPushButton, QMessageBox QPushButton {
    background-color: #1A2130;
    border: 1px solid #2A354D;
    border-radius: 8px;
    padding: 7px 22px;
    color: #E8EDF7;
}
QDialog QPushButton:hover, QMessageBox QPushButton:hover {
    background-color: #222B3F;
    border: 1px solid #3A4A6B;
}
QDialog QPushButton:pressed, QMessageBox QPushButton:pressed {
    background-color: #141A28;
}
QDialog QPushButton:focus, QMessageBox QPushButton:focus {
    border: 1px solid #5B8DEF;
}

/* ---------- checkboxes (rounded chips) ---------- */
QCheckBox {
    color: #C6CCE0;
    spacing: 8px;
    background: transparent;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid #3A4568;
    background-color: #0C111C;
}
QCheckBox::indicator:hover {
    border: 1px solid #5B8DEF;
}
QCheckBox::indicator:checked {
    background-image: qlineargradient(x1: 0, y1: 0, x2: 1, y2: 1, stop: 0 #4C8DFF, stop: 1 #7C5CFF);
    border: 1px solid #6BA3FF;
}
QCheckBox:disabled {
    color: #5C6679;
}

/* ---------- combo box ---------- */
QComboBox {
    background-color: #0C111C;
    border: 1px solid #27324A;
    border-radius: 8px;
    padding: 7px 12px;
    min-width: 110px;
    color: #E8EDF7;
}
QComboBox:hover {
    border: 1px solid #344363;
    background-color: #0D1420;
}
QComboBox:focus {
    border: 1px solid #5B8DEF;
}
QComboBox:disabled {
    color: #5C6679;
    background-color: #0E141F;
}
QComboBox::drop-down {
    border: none;
    width: 28px;
}
QComboBox::down-arrow {
    image: none;
    border-left: 5px solid transparent;
    border-right: 5px solid transparent;
    border-top: 6px solid #8B95A8;
    margin-right: 10px;
}
QComboBox::down-arrow:hover {
    border-top: 6px solid #E8EDF7;
}
QComboBox QAbstractItemView {
    background-color: #161C2A;
    border: 1px solid #2E3750;
    border-radius: 8px;
    selection-background-color: #1D2A44;
    selection-color: #FFFFFF;
    color: #C6CCE0;
    outline: 0;
    padding: 4px;
}
QComboBox QAbstractItemView::item {
    padding: 7px 12px;
    border-radius: 5px;
    min-height: 24px;
    color: #C6CCE0;
}
QComboBox QAbstractItemView::item:hover {
    background-color: #202942;
    color: #E8EDF7;
}
QComboBox QAbstractItemView::item:selected {
    background-color: #2B4A6F;
    color: #FFFFFF;
}

/* ---------- client list (narrow sidebar card with flowing chip tags) ---------- */
QWidget#clientCard {
    background-color: #121826;
    border: 1px solid #222B3D;
    border-radius: 12px;
}
QLabel#clientHeader {
    color: #E8EDF7;
    font-size: 15px;
    font-weight: 600;
    padding-left: 4px;
    padding-bottom: 10px;
    border-bottom: 1px solid #222B3D;
}
/* client list: flowing pill chips (replaces the plain QListWidget) */
QScrollArea#clientScroll {
    background: transparent;
    border: none;
}
QScrollArea#clientScroll QWidget#qt_scrollarea_viewport,
QWidget#clientListHost {
    background: transparent;
}
QLabel#clientChip {
    background-color: #1A2130;
    color: #C6CCE0;
    border: 1px solid #2E3A55;
    border-radius: 15px;
    padding: 5px 13px;
    font-size: 13px;
}
QLabel#clientChip:hover {
    background-color: #222B3F;
    border: 1px solid #3E4A6B;
    color: #E8EDF7;
}

/* ---------- scrollbars ---------- */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: #2E3750;
    border-radius: 5px;
    min-height: 30px;
}
QScrollBar::handle:vertical:hover {
    background: #3E4A6B;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
}

/* ---------- preview ---------- */
QWidget#previewFrame {
    background-color: #121826;
    border: 1px solid #222B3D;
    border-radius: 12px;
}
QWidget#previewFrame[starting="true"] {
    border: 1px solid #5B8DEF;
}

/* ---------- status bar ---------- */
QStatusBar {
    background-color: #0E1320;
    border-top: 1px solid #222B3D;
    color: #8B95A8;
}
QStatusBar QLabel {
    color: #8B95A8;
}

/* ---------- message box / tooltip ---------- */
QMessageBox {
    background-color: #121826;
}
QMessageBox QLabel {
    color: #E8EDF7;
}
QToolTip {
    background-color: #1A2130;
    color: #E8EDF7;
    border: 1px solid #3A4568;
    padding: 5px 10px;
}

/* ---------- menu (client right-click) ---------- */
QMenu {
    background-color: #161C2A;
    border: 1px solid #2E3750;
    border-radius: 8px;
    padding: 4px;
}
QMenu::item {
    padding: 7px 24px 7px 12px;
    border-radius: 5px;
    color: #C6CCE0;
}
QMenu::item:selected {
    background-color: #202942;
    color: #E8EDF7;
}
QMenu::separator {
    height: 1px;
    background: #222B3D;
    margin: 4px 6px;
}

/* ---------- connect dialog ---------- */
QDialog#connectDialog QLabel {
    color: #8B95A8;
}
QDialog#connectDialog QLabel#dlgTitle {
    color: #E8EDF7;
    font-size: 20px;
    font-weight: 600;
}
QDialog#connectDialog QLabel#dlgSubtitle {
    color: #8B95A8;
    font-size: 12px;
}
)QSS";

// ---------------------------------------------------------------------------
// Popup menus -- MUST be applied with a widget-level setStyleSheet().
//
// The QMenu block inside kQssDark above is NOT enough on its own: Fusion paints a menu
// through PE_PanelMenu using the PALETTE colour, so the menu background comes from the
// system window colour while the text still comes from the stylesheet. On a machine whose
// Windows is set to the LIGHT theme that yields a white menu with near-white labels
// ("白底看不清字体"). Same Qt6/Fusion escape hatch the buttons and combo box already use.
// ---------------------------------------------------------------------------
static const char kPopupMenuQss[] = R"QSS(
QMenu {
    background-color: #161C2A;
    border: 1px solid #2E3750;
    border-radius: 8px;
    padding: 4px;
    color: #C6CCE0;
}
QMenu::item {
    padding: 7px 28px 7px 14px;
    border-radius: 5px;
    color: #C6CCE0;
}
QMenu::item:selected {
    background-color: #202942;
    color: #E8EDF7;
}
QMenu::item:disabled {
    color: #5C6679;
}
QMenu::separator {
    height: 1px;
    background: #222B3D;
    margin: 4px 6px;
}
)QSS";

// ---------------------------------------------------------------------------
// Pin the application palette to this theme's colours.
//
// Widgets Qt paints from the palette rather than from the stylesheet -- menu popups,
// tool tips, item views, message-box buttons -- would otherwise follow the Windows
// light/dark setting and break out of the dark UI (see kPopupMenuQss above for the
// concrete symptom). Call right after setStyle("Fusion"), before setStyleSheet().
// ---------------------------------------------------------------------------
inline void applyDarkPalette(QApplication& app) {
    const QColor card(0x12, 0x18, 0x26);      // #121826  window / menu surface
    const QColor input(0x0C, 0x11, 0x1C);     // #0C111C  editable fields
    const QColor button(0x1A, 0x21, 0x30);    // #1A2130  buttons
    const QColor text(0xE8, 0xED, 0xF7);      // #E8EDF7  primary text
    const QColor dim(0x5C, 0x66, 0x79);       // #5C6679  disabled text
    const QColor accent(0x4C, 0x8D, 0xFF);    // #4C8DFF  selection

    QPalette p = app.palette();
    p.setColor(QPalette::Window, card);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, input);
    p.setColor(QPalette::AlternateBase, card);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(Qt::white));
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, QColor(Qt::white));
    p.setColor(QPalette::ToolTipBase, button);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Link, QColor(0x5B, 0x8D, 0xEF));
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    app.setPalette(p);
}
