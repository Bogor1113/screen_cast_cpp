#pragma once
// ui/connect_dialog.h -- "connect to teacher" dialog shown when the client starts with no
// command-line args. Remembers the last-used IP / multicast group in client_config.txt (two
// UTF-8 lines, next to the exe) and pre-fills the fields on the next launch.
#include <QDialog>

class QLineEdit;
class QLabel;

class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectDialog(QWidget* parent = nullptr);

    QString teacherIp() const;      // trimmed IP, guaranteed non-empty when accept() passed
    QString group() const;          // multicast group (may be empty = unicast)

    // persist / load the last connection record (client_config.txt next to the exe)
    static void saveConfig(const QString& ip, const QString& group);
    static void loadConfig(QString& ip, QString& group);

private:
    void accept() override;

    QLineEdit* ipEdit_ = nullptr;
    QLineEdit* groupEdit_ = nullptr;
};