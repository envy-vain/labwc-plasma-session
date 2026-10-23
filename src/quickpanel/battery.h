#pragma once
// Battery tray icon + popup: charge, time to full/empty, draw, health, power profile, Eco mode
// (ultra-low-power) and the charge limit. Event-driven from UPower / power-profiles-daemon D-Bus
// PropertiesChanged signals, values cached so updates never do a round trip.
#include "popup.h"

#include <QVariantMap>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSystemTrayIcon;
class QTimer;

class BatteryPanel : public Popup {
    Q_OBJECT
public:
    BatteryPanel();

protected:
    void aboutToOpen() override;

private Q_SLOTS:
    void displayChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void batteryChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
    void profileChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void updateTray();
    void updatePopup();
    void setProfile(const QString &profile);
    void checkEco();

    QVariantMap disp, bat; // cached UPower properties (DisplayDevice = combined, bat = the real battery)
    QString batPath, profile, trayIconName;
    bool eco = false;

    QSystemTrayIcon *tray;
    QLabel *icon, *percent, *status, *details, *note;
    QProgressBar *level;
    QButtonGroup *profiles;
    QPushButton *ecoB;
    QCheckBox *limit;
    QTimer *ecoPoll;
};
