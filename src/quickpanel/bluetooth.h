#pragma once
// Bluetooth tray icon + popup straight on BlueZ D-Bus (replaces blueman's two Python processes):
// adapter on/off, paired devices with connect/disconnect and battery level. Objects and properties
// are cached from GetManagedObjects and kept current from InterfacesAdded/Removed and PropertiesChanged.
// "Pair new device" scans and pairs right in the popup (Just Works / NoInputNoOutput, which is what
// headphones and speakers use), so blueman is no longer needed for pairing.
#include "popup.h"

#include <QDBusMessage>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QSize>
#include <QVariantMap>

class QCheckBox;
class QPushButton;
class QLabel;
class QSystemTrayIcon;
class QTimer;
class QVBoxLayout;
class BtRow;

class BluetoothPanel : public Popup {
    Q_OBJECT
public:
    BluetoothPanel();

protected:
    void aboutToOpen() override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;

private Q_SLOTS:
    void propertiesChanged(const QDBusMessage &msg);
    void interfacesAdded(const QDBusMessage &msg);
    void interfacesRemoved(const QDBusMessage &msg);
    void bluezOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);

private:
    using Interfaces = QMap<QString, QVariantMap>;
    void load();
    void schedule();
    void refresh();
    void updateTray();
    void setPowered(bool on, bool retried = false);
    void call(const QString &path, const QString &method);
    void setScanning(bool on);
    void pair(const QString &path);
    void remap();

    QMap<QString, Interfaces> objects; // object path -> interface -> properties
    QString adapter;
    QSet<QString> busy;                // devices with a connect/disconnect/pair in flight
    QSet<QString> pairing;             // subset of busy: Pair() in flight
    bool scanning = false;
    bool pairableBefore = false;       // adapter's Pairable before we switched it on, restored after
    bool remapping = false;
    QElapsedTimer remapAt;             // ignore focus-loss events for a moment after a re-map
    QSize mappedSize;                  // size the popup was shown at; labwc only places it on map
    QString error;
    QString trayKey;

    QSystemTrayIcon *tray;
    QCheckBox *toggle;
    QVBoxLayout *list;
    QLabel *empty, *note;
    QHash<QString, BtRow *> rows, availRows;
    QTimer *refreshTimer, *scanTimer;
    QWidget *scanBox;
    QVBoxLayout *avail;
    QLabel *availEmpty;
    QPushButton *pairBtn;
};
