// ui/connect_dialog.cpp -- "connect to teacher" dialog (Qt port of the Win32 ConnDialog)
#include "ui/connect_dialog.h"
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QCoreApplication>
#include <QStringConverter>

ConnectDialog::ConnectDialog(QWidget* parent) : QDialog(parent) {
    setObjectName("connectDialog");
    setWindowTitle(QStringLiteral("screen_cast 学生端"));
    setFixedWidth(400);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("screen_cast 学生端"), this);
    title->setObjectName("dlgTitle");
    root->addWidget(title);

    auto* subtitle = new QLabel(QStringLiteral("输入教师端 IP，连接后即可观看教师屏幕"), this);
    subtitle->setObjectName("dlgSubtitle");
    root->addWidget(subtitle);

    auto* form = new QFormLayout();
    form->setSpacing(8);
    ipEdit_ = new QLineEdit(this);
    ipEdit_->setPlaceholderText(QStringLiteral("教师端 IP"));
    groupEdit_ = new QLineEdit(this);
    groupEdit_->setPlaceholderText(QStringLiteral("组播组(留空 = 单播)"));
    form->addRow(QStringLiteral("教师端 IP:"), ipEdit_);
    form->addRow(QStringLiteral("组播组:"), groupEdit_);
    root->addLayout(form);

    // defaults from the last-saved record
    QString savedIp, savedGroup;
    loadConfig(savedIp, savedGroup);
    ipEdit_->setText(savedIp);
    groupEdit_->setText(savedGroup);

    auto* btns = new QHBoxLayout();
    btns->addStretch(1);
    auto* ok = new QPushButton(QStringLiteral("连接"), this);
    ok->setObjectName("connectBtn");
    ok->setAttribute(Qt::WA_StyledBackground, true);   // let QSS background win over the Fusion panel
    ok->setDefault(true);
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    cancel->setAttribute(Qt::WA_StyledBackground, true);
    connect(ok, &QPushButton::clicked, this, &ConnectDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &ConnectDialog::reject);
    btns->addWidget(ok);
    btns->addWidget(cancel);
    root->addLayout(btns);
}

QString ConnectDialog::teacherIp() const { return ipEdit_->text().trimmed(); }
QString ConnectDialog::group() const { return groupEdit_->text().trimmed(); }

void ConnectDialog::accept() {
    if (teacherIp().isEmpty()) {
        ipEdit_->setFocus();
        ipEdit_->setPlaceholderText(QStringLiteral("请输入教师端 IP"));
        return;   // stay in the dialog
    }
    QDialog::accept();
}

// ---- client_config.txt: two UTF-8 lines (ip, group) next to the exe ----
QString configPath() {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("client_config.txt"));
}

void ConnectDialog::saveConfig(const QString& ip, const QString& group) {
    QFile f(configPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << ip << '\n' << group << '\n';
}

void ConnectDialog::loadConfig(QString& ip, QString& group) {
    ip.clear(); group.clear();
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ip = QStringLiteral("192.168.110.159");
        group = QStringLiteral("239.255.1.1");
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ip = ts.readLine().trimmed();
    group = ts.readLine().trimmed();
    if (ip.isEmpty()) ip = QStringLiteral("192.168.110.159");
    if (group.isEmpty()) group = QStringLiteral("239.255.1.1");
}