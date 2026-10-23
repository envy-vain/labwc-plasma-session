#include "bluetooth.h"

#include <QCheckBox>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QProcess>
#include <QHideEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {
const QString kBluez = QStringLiteral("org.bluez");
const QString kAdapter = QStringLiteral("org.bluez.Adapter1");
const QString kDevice = QStringLiteral("org.bluez.Device1");
const QString kBattery = QStringLiteral("org.bluez.Battery1");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kObjMgr = QStringLiteral("org.freedesktop.DBus.ObjectManager");

QIcon themed(const QString &name, const QIcon &fallback = {})
{
    return QIcon::fromTheme(name + QStringLiteral("-symbolic"), QIcon::fromTheme(name, fallback));
}

// What to call an unpaired device under its name, from BlueZ's Icon property.
QString kindOf(const QString &icon)
{
    if (icon.startsWith(QStringLiteral("audio-head")))
        return QStringLiteral("Headphones");
    if (icon == QStringLiteral("audio-card"))
        return QStringLiteral("Speaker");
    if (icon == QStringLiteral("input-keyboard"))
        return QStringLiteral("Keyboard");
    if (icon == QStringLiteral("input-mouse"))
        return QStringLiteral("Mouse");
    if (icon == QStringLiteral("input-gaming"))
        return QStringLiteral("Controller");
    if (icon == QStringLiteral("phone"))
        return QStringLiteral("Phone");
    if (icon == QStringLiteral("computer"))
        return QStringLiteral("Computer");
    return QStringLiteral("Not paired");
}

QVariantMap toMap(const QVariant &v)
{
    if (v.canConvert<QDBusArgument>()) {
        QVariantMap m;
        v.value<QDBusArgument>() >> m;
        return m;
    }
    return v.toMap();
}
}

class BtRow : public QFrame {
public:
    BtRow()
    {
        setObjectName(QStringLiteral("row"));
        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(10, 7, 10, 7);
        h->setSpacing(10);
        icon = new QLabel;
        icon->setFixedSize(24, 24);
        auto *t = new QVBoxLayout;
        t->setSpacing(0);
        name = new QLabel;
        sub = new QLabel;
        QPalette p = sub->palette();
        QColor c = p.color(QPalette::WindowText);
        c.setAlphaF(0.65);
        p.setColor(QPalette::WindowText, c);
        sub->setPalette(p);
        t->addWidget(name);
        t->addWidget(sub);
        button = new QPushButton;
        h->addWidget(icon);
        h->addLayout(t, 1);
        h->addWidget(button);
    }
    QLabel *icon, *name, *sub;
    QPushButton *button;
    QString iconName;
};

BluetoothPanel::BluetoothPanel()
    : Popup(QStringLiteral("Bluetooth"))
{
    qDBusRegisterMetaType<Interfaces>();
    qDBusRegisterMetaType<QMap<QDBusObjectPath, Interfaces>>();

    resize(380, 10);
    setStyleSheet(QStringLiteral("#row{border-radius:8px}#row:hover{background:palette(midlight)}"
                                 "#row[connected=\"true\"]{background:palette(alternate-base)}"
                                 "QScrollArea{border:none;background:transparent}#availList{background:transparent}"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);
    root->setSizeConstraint(QLayout::SetFixedSize);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>Bluetooth</b>"));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.25);
    title->setFont(tf);
    toggle = new QCheckBox(QStringLiteral("On"));
    auto *settings = new QToolButton;
    settings->setIcon(themed(QStringLiteral("configure")));
    settings->setToolTip(QStringLiteral("Bluetooth settings"));
    settings->setAutoRaise(true);
    head->addWidget(title);
    head->addStretch();
    head->addWidget(toggle);
    head->addWidget(settings);
    root->addLayout(head);
    connect(toggle, &QCheckBox::clicked, this, [this](bool on) { setPowered(on); });
    // Advanced settings: KDE's Bluetooth module (Qt, follows the theme) instead of blueman's GTK window.
    connect(settings, &QToolButton::clicked, this, [this] {
        hide();
        QProcess::startDetached(QStringLiteral("kcmshell6"), {QStringLiteral("kcm_bluetooth")});
    });

    auto *listW = new QWidget;
    list = new QVBoxLayout(listW);
    list->setContentsMargins(0, 0, 0, 0);
    list->setSpacing(2);
    listW->setMinimumWidth(356);
    root->addWidget(listW);
    empty = dimLabel();
    empty->setAlignment(Qt::AlignCenter);
    empty->setMinimumHeight(40);
    root->addWidget(empty);

    // Nearby unpaired devices, shown only while searching. Fixed height so devices streaming in
    // don't resize the window (labwc only positions it when it is mapped).
    scanBox = new QWidget;
    auto *sv = new QVBoxLayout(scanBox);
    sv->setContentsMargins(0, 4, 0, 0);
    sv->setSpacing(6);
    auto *availTitle = new QLabel(QStringLiteral("<b>Nearby devices</b>"));
    auto *hint = dimLabel();
    hint->setWordWrap(true);
    hint->setText(QStringLiteral("Put your headphones in pairing mode. On most, hold the power button until the light flashes."));
    auto *availW = new QWidget;
    availW->setObjectName(QStringLiteral("availList"));
    auto *aw = new QVBoxLayout(availW);
    aw->setContentsMargins(0, 0, 0, 0);
    aw->setSpacing(2);
    avail = new QVBoxLayout;
    avail->setSpacing(2);
    aw->addLayout(avail);
    availEmpty = dimLabel();
    availEmpty->setAlignment(Qt::AlignCenter);
    availEmpty->setMinimumHeight(40);
    availEmpty->setText(QStringLiteral("Searching…"));
    aw->addWidget(availEmpty);
    aw->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFixedHeight(230);
    scroll->setWidget(availW);
    sv->addWidget(availTitle);
    sv->addWidget(hint);
    sv->addWidget(scroll);
    scanBox->hide();
    root->addWidget(scanBox);

    pairBtn = new QPushButton(themed(QStringLiteral("list-add")), QStringLiteral("Pair new device…"));
    root->addWidget(pairBtn);
    connect(pairBtn, &QPushButton::clicked, this, [this] { setScanning(!scanning); });
    scanTimer = new QTimer(this);
    scanTimer->setSingleShot(true);
    scanTimer->setInterval(90000);
    connect(scanTimer, &QTimer::timeout, this, [this] { setScanning(false); });
    note = dimLabel();
    note->setWordWrap(true);
    note->hide();
    root->addWidget(note);

    refreshTimer = new QTimer(this);
    refreshTimer->setSingleShot(true);
    refreshTimer->setInterval(80);
    connect(refreshTimer, &QTimer::timeout, this, &BluetoothPanel::refresh);

    tray = new QSystemTrayIcon(this);
    connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::MiddleClick)
            toggleVisible();
    });

    auto bus = QDBusConnection::systemBus();
    // Empty path = every BlueZ object; the message carries which one changed.
    bus.connect(kBluez, QString(), kProps, QStringLiteral("PropertiesChanged"), this, SLOT(propertiesChanged(QDBusMessage)));
    bus.connect(kBluez, QStringLiteral("/"), kObjMgr, QStringLiteral("InterfacesAdded"), this, SLOT(interfacesAdded(QDBusMessage)));
    bus.connect(kBluez, QStringLiteral("/"), kObjMgr, QStringLiteral("InterfacesRemoved"), this, SLOT(interfacesRemoved(QDBusMessage)));
    auto *w = new QDBusServiceWatcher(kBluez, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(w, &QDBusServiceWatcher::serviceOwnerChanged, this, &BluetoothPanel::bluezOwnerChanged);
    load();
}

void BluetoothPanel::load()
{
    objects.clear();
    auto msg = QDBusMessage::createMethodCall(kBluez, QStringLiteral("/"), kObjMgr, QStringLiteral("GetManagedObjects"));
    QDBusMessage r = QDBusConnection::systemBus().call(msg, QDBus::Block, 2000);
    if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
        QMap<QDBusObjectPath, Interfaces> all;
        r.arguments().first().value<QDBusArgument>() >> all;
        for (auto it = all.begin(); it != all.end(); ++it)
            objects.insert(it.key().path(), it.value());
    }
    schedule();
}

void BluetoothPanel::bluezOwnerChanged(const QString &, const QString &, const QString &)
{
    load(); // bluetoothd restarted or went away
}

void BluetoothPanel::propertiesChanged(const QDBusMessage &msg)
{
    const auto args = msg.arguments();
    if (args.size() < 2)
        return;
    const QString iface = args[0].toString();
    if (iface != kAdapter && iface != kDevice && iface != kBattery)
        return; // e.g. MediaTransport volume changes: ignore without touching the UI
    auto obj = objects.find(msg.path());
    if (obj == objects.end())
        return;
    QVariantMap &props = (*obj)[iface];
    const QVariantMap changed = toMap(args[1]);
    for (auto it = changed.begin(); it != changed.end(); ++it)
        props.insert(it.key(), it.value());
    if (args.size() > 2)
        for (const auto &k : args[2].toStringList())
            props.remove(k);
    // RSSI etc. stream in while scanning; only redraw for things we show.
    static const QSet<QString> shown{QStringLiteral("Powered"), QStringLiteral("Connected"), QStringLiteral("Paired"),
                                     QStringLiteral("Alias"), QStringLiteral("Name"), QStringLiteral("Icon"), QStringLiteral("Percentage")};
    for (auto it = changed.begin(); it != changed.end(); ++it)
        if (shown.contains(it.key()) || (scanning && it.key() == QStringLiteral("RSSI")))
            return schedule();
}

void BluetoothPanel::interfacesAdded(const QDBusMessage &msg)
{
    const auto args = msg.arguments();
    if (args.size() < 2)
        return;
    const QString path = args[0].value<QDBusObjectPath>().path();
    Interfaces ifs;
    args[1].value<QDBusArgument>() >> ifs;
    Interfaces &cur = objects[path];
    for (auto it = ifs.begin(); it != ifs.end(); ++it)
        cur.insert(it.key(), it.value());
    schedule();
}

void BluetoothPanel::interfacesRemoved(const QDBusMessage &msg)
{
    const auto args = msg.arguments();
    if (args.size() < 2)
        return;
    const QString path = args[0].value<QDBusObjectPath>().path();
    auto obj = objects.find(path);
    if (obj == objects.end())
        return;
    for (const auto &i : args[1].toStringList())
        obj->remove(i);
    if (obj->isEmpty())
        objects.erase(obj);
    schedule();
}

void BluetoothPanel::schedule()
{
    if (!refreshTimer->isActive())
        refreshTimer->start();
}

void BluetoothPanel::aboutToOpen()
{
    error.clear();
    refresh();
}

void BluetoothPanel::updateTray()
{
    const bool on = !adapter.isEmpty() && objects[adapter][kAdapter].value(QStringLiteral("Powered")).toBool();
    QStringList connected;
    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
        const auto d = it->value(kDevice);
        if (d.value(QStringLiteral("Connected")).toBool()) {
            QString n = d.value(QStringLiteral("Alias")).toString();
            const auto b = it->value(kBattery);
            if (b.contains(QStringLiteral("Percentage")))
                n += QStringLiteral(" (%1%)").arg(b.value(QStringLiteral("Percentage")).toUInt());
            connected << n;
        }
    }
    tray->setVisible(!adapter.isEmpty());
    const QString key = !on ? QStringLiteral("off") : connected.isEmpty() ? QStringLiteral("on") : QStringLiteral("conn");
    if (key != trayKey) {
        trayKey = key;
        const QIcon active = themed(QStringLiteral("bluetooth-active"), themed(QStringLiteral("bluetooth")));
        tray->setIcon(key == QStringLiteral("off") ? themed(QStringLiteral("bluetooth-disabled"), active)
                      : key == QStringLiteral("conn") ? themed(QStringLiteral("bluetooth-paired"), active)
                                                      : active);
    }
    tray->setToolTip(!on ? QStringLiteral("Bluetooth off") : connected.isEmpty() ? QStringLiteral("Bluetooth on")
                                                          : QStringLiteral("Bluetooth: ") + connected.join(QStringLiteral(", ")));
}

void BluetoothPanel::refresh()
{
    adapter.clear();
    for (auto it = objects.cbegin(); it != objects.cend(); ++it)
        if (it->contains(kAdapter)) {
            adapter = it.key();
            break;
        }
    updateTray();
    if (!isVisible())
        return;

    const bool on = !adapter.isEmpty() && objects[adapter][kAdapter].value(QStringLiteral("Powered")).toBool();
    toggle->setEnabled(!adapter.isEmpty());
    toggle->setChecked(on);

    struct Dev { QString path; QVariantMap p; int battery; };
    QList<Dev> devs;
    if (on)
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const auto d = it->value(kDevice);
            if (d.isEmpty() || !d.value(QStringLiteral("Paired")).toBool() || d.value(QStringLiteral("Adapter")).value<QDBusObjectPath>().path() != adapter)
                continue;
            const auto b = it->value(kBattery);
            devs.push_back({it.key(), d, b.contains(QStringLiteral("Percentage")) ? int(b.value(QStringLiteral("Percentage")).toUInt()) : -1});
        }
    std::sort(devs.begin(), devs.end(), [](const Dev &a, const Dev &b) {
        const bool ca = a.p.value(QStringLiteral("Connected")).toBool(), cb = b.p.value(QStringLiteral("Connected")).toBool();
        if (ca != cb)
            return ca;
        return a.p.value(QStringLiteral("Alias")).toString().localeAwareCompare(b.p.value(QStringLiteral("Alias")).toString()) < 0;
    });

    QHash<QString, BtRow *> old;
    old.swap(rows);
    while (list->count())
        list->takeAt(0);
    for (const auto &d : devs) {
        BtRow *r = old.take(d.path);
        if (!r) {
            r = new BtRow;
            const QString path = d.path;
            connect(r->button, &QPushButton::clicked, this, [this, path] {
                const bool c = objects[path][kDevice].value(QStringLiteral("Connected")).toBool();
                call(path, c ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
            });
        }
        rows.insert(d.path, r);
        const bool connected = d.p.value(QStringLiteral("Connected")).toBool();
        const QString icon = d.p.value(QStringLiteral("Icon")).toString();
        if (icon != r->iconName || r->icon->pixmap().isNull()) {
            r->iconName = icon;
            r->icon->setPixmap(themed(icon.isEmpty() ? QStringLiteral("bluetooth") : icon, themed(QStringLiteral("bluetooth"))).pixmap(24, 24));
        }
        r->name->setText(d.p.value(QStringLiteral("Alias")).toString());
        QString sub = busy.contains(d.path) ? (connected ? QStringLiteral("Disconnecting…") : QStringLiteral("Connecting…"))
                      : connected           ? QStringLiteral("Connected")
                                            : QStringLiteral("Paired");
        if (d.battery >= 0)
            sub += QStringLiteral(" · %1% battery").arg(d.battery);
        r->sub->setText(sub);
        r->button->setText(connected ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
        r->button->setEnabled(!busy.contains(d.path));
        if (r->property("connected").toBool() != connected) {
            r->setProperty("connected", connected);
            r->style()->unpolish(r);
            r->style()->polish(r);
        }
        list->addWidget(r);
    }
    qDeleteAll(old);

    // Nearby unpaired devices (only while searching). Sorted by name, not signal, so rows don't jump.
    QHash<QString, BtRow *> oldAvail;
    oldAvail.swap(availRows);
    while (avail->count())
        delete avail->takeAt(0);
    int shownAvail = 0;
    if (scanning && on) {
        QList<Dev> av;
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            const auto d = it->value(kDevice);
            if (d.isEmpty() || d.value(QStringLiteral("Paired")).toBool() || d.value(QStringLiteral("Adapter")).value<QDBusObjectPath>().path() != adapter)
                continue;
            // RSSI = heard during this search; cached devices from older scans have none.
            const bool seen = d.contains(QStringLiteral("RSSI")) || d.value(QStringLiteral("Connected")).toBool() || pairing.contains(it.key());
            if (seen && d.contains(QStringLiteral("Name")))
                av.push_back({it.key(), d, -1});
        }
        std::sort(av.begin(), av.end(), [this](const Dev &a, const Dev &b) {
            const bool pa = pairing.contains(a.path), pb = pairing.contains(b.path);
            if (pa != pb)
                return pa;
            return a.p.value(QStringLiteral("Alias")).toString().localeAwareCompare(b.p.value(QStringLiteral("Alias")).toString()) < 0;
        });
        for (const auto &d : av) {
            BtRow *r = oldAvail.take(d.path);
            if (!r) {
                r = new BtRow;
                const QString path = d.path;
                connect(r->button, &QPushButton::clicked, this, [this, path] { pair(path); });
            }
            availRows.insert(d.path, r);
            const QString icon = d.p.value(QStringLiteral("Icon")).toString();
            if (icon != r->iconName || r->icon->pixmap().isNull()) {
                r->iconName = icon;
                r->icon->setPixmap(themed(icon.isEmpty() ? QStringLiteral("bluetooth") : icon, themed(QStringLiteral("bluetooth"))).pixmap(24, 24));
            }
            r->name->setText(d.p.value(QStringLiteral("Alias")).toString());
            r->sub->setText(pairing.contains(d.path) ? QStringLiteral("Pairing…") : kindOf(icon));
            r->button->setText(QStringLiteral("Pair"));
            r->button->setEnabled(!busy.contains(d.path));
            avail->addWidget(r);
            ++shownAvail;
        }
    }
    qDeleteAll(oldAvail);
    availEmpty->setVisible(shownAvail == 0);

    empty->setText(adapter.isEmpty() ? QStringLiteral("No Bluetooth adapter") : !on ? QStringLiteral("Bluetooth is off") : QStringLiteral("No paired devices"));
    empty->setVisible(devs.isEmpty() && !(scanning && on));
    pairBtn->setEnabled(on);
    note->setText(error);
    note->setVisible(!error.isEmpty());

    // The layout resizes the window itself; labwc only places it on map, so a taller popup would hang
    // off the bottom of the screen. Re-map it so the window rule puts it back in the corner.
    layout()->activate();
    if (!mappedSize.isValid())
        mappedSize = size();
    else if (size() != mappedSize)
        remap();
}

void BluetoothPanel::remap()
{
    if (!isVisible() || remapping)
        return;
    remapping = true;
    hide();
    remapping = false;
    remapAt.start();
    QTimer::singleShot(0, this, [this] {
        show();
        refresh();
        raise();
        activateWindow();
    });
}

void BluetoothPanel::hideEvent(QHideEvent *e)
{
    mappedSize = QSize();
    if (!remapping && scanning)
        setScanning(false);
    Popup::hideEvent(e);
}

void BluetoothPanel::changeEvent(QEvent *e)
{
    // The unmap/map of a re-map can deliver a stale "lost focus" right after the window is back.
    if (e->type() == QEvent::ActivationChange && remapAt.isValid() && remapAt.elapsed() < 700)
        return QWidget::changeEvent(e);
    Popup::changeEvent(e);
}

void BluetoothPanel::setScanning(bool on)
{
    if (on == scanning || (on && adapter.isEmpty()))
        return;
    scanning = on;
    if (on)
        error.clear();
    scanBox->setVisible(on);
    pairBtn->setText(on ? QStringLiteral("Stop searching") : QStringLiteral("Pair new device…"));
    pairBtn->setIcon(themed(on ? QStringLiteral("process-stop") : QStringLiteral("list-add")));
    if (on)
        scanTimer->start();
    else
        scanTimer->stop();
    // Pairable is the kernel's "bondable" flag: with it off, Pair() still works but makes a one-time
    // link and stores no key, so the device is forgotten as soon as it disconnects. Blueman used to
    // switch it on; keep it on only while the pairing list is open.
    auto p = QDBusMessage::createMethodCall(kBluez, adapter, kProps, QStringLiteral("Set"));
    p << kAdapter << QStringLiteral("Pairable") << QVariant::fromValue(QDBusVariant(on));
    if (on) // pair() can follow right away, so make sure it has landed
        QDBusConnection::systemBus().call(p);
    else
        QDBusConnection::systemBus().asyncCall(p);
    auto msg = QDBusMessage::createMethodCall(kBluez, adapter, kAdapter, on ? QStringLiteral("StartDiscovery") : QStringLiteral("StopDiscovery"));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, on] {
        w->deleteLater();
        // StopDiscovery errors (e.g. "not started by you") don't matter; a failed start does.
        if (on && w->isError() && w->error().name() != QStringLiteral("org.bluez.Error.InProgress")) {
            error = QStringLiteral("Couldn't search for devices: ") + w->error().message();
            setScanning(false);
        }
        schedule();
    });
    schedule();
}

void BluetoothPanel::pair(const QString &path)
{
    busy.insert(path);
    pairing.insert(path);
    error.clear();
    schedule();
    // Searching slows pairing down on many adapters; stop the radio scan but keep the list up.
    QDBusConnection::systemBus().asyncCall(QDBusMessage::createMethodCall(kBluez, adapter, kAdapter, QStringLiteral("StopDiscovery")));
    scanTimer->stop();
    auto msg = QDBusMessage::createMethodCall(kBluez, path, kDevice, QStringLiteral("Pair"));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 60000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, path] {
        w->deleteLater();
        busy.remove(path);
        pairing.remove(path);
        const QString name = objects[path][kDevice].value(QStringLiteral("Alias")).toString();
        if (w->isError() && w->error().name() != QStringLiteral("org.bluez.Error.AlreadyExists")) {
            error = QStringLiteral("Couldn't pair with %1: %2. Make sure it's in pairing mode and try again.").arg(name, w->error().message());
            if (scanning) // resume the radio scan for another try
                QDBusConnection::systemBus().asyncCall(QDBusMessage::createMethodCall(kBluez, adapter, kAdapter, QStringLiteral("StartDiscovery")));
            schedule();
            return;
        }
        // Trust it so it can reconnect on its own next time, then connect.
        auto t = QDBusMessage::createMethodCall(kBluez, path, kProps, QStringLiteral("Set"));
        t << kDevice << QStringLiteral("Trusted") << QVariant::fromValue(QDBusVariant(true));
        QDBusConnection::systemBus().asyncCall(t);
        setScanning(false);
        call(path, QStringLiteral("Connect"));
    });
}

void BluetoothPanel::setPowered(bool on, bool retried)
{
    if (adapter.isEmpty())
        return;
    auto msg = QDBusMessage::createMethodCall(kBluez, adapter, kProps, QStringLiteral("Set"));
    msg << kAdapter << QStringLiteral("Powered") << QVariant::fromValue(QDBusVariant(on));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, on, retried] {
        w->deleteLater();
        if (!w->isError())
            return;
        // Soft-blocked by rfkill (airplane mode / blueman's power-off): BlueZ only says "Failed",
        // so unblock and retry once (the user is in the rfkill group, no root needed).
        if (on && !retried) {
            auto *p = new QProcess(this);
            connect(p, &QProcess::finished, this, [this, p] {
                p->deleteLater();
                QTimer::singleShot(300, this, [this] { setPowered(true, true); });
            });
            p->start(QStringLiteral("rfkill"), {QStringLiteral("unblock"), QStringLiteral("bluetooth")});
            return;
        }
        error = QStringLiteral("Couldn't turn Bluetooth %1: %2").arg(on ? QStringLiteral("on") : QStringLiteral("off"), w->error().message());
        schedule();
    });
}

void BluetoothPanel::call(const QString &path, const QString &method)
{
    busy.insert(path);
    error.clear();
    schedule();
    auto msg = QDBusMessage::createMethodCall(kBluez, path, kDevice, method);
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 30000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, path] {
        w->deleteLater();
        busy.remove(path);
        if (w->isError())
            error = objects[path][kDevice].value(QStringLiteral("Alias")).toString() + QStringLiteral(": ") + w->error().message();
        schedule();
    });
}
