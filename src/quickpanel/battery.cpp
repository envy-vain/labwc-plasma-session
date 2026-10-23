#include "battery.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
const QString kUPower = QStringLiteral("org.freedesktop.UPower");
const QString kDevice = QStringLiteral("org.freedesktop.UPower.Device");
const QString kDisplayPath = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");
const QString kPPD = QStringLiteral("org.freedesktop.UPower.PowerProfiles");
const QString kPPDPath = QStringLiteral("/org/freedesktop/UPower/PowerProfiles");
const QString kProps = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kEcoFlag = QStringLiteral("/run/ultra-low-power");
const QString kEcoCmd = QStringLiteral("/usr/local/bin/ultra-low-power");

enum State { Unknown, Charging, Discharging, Empty, Full, PendingCharge, PendingDischarge };

QVariantMap getAll(const QString &service, const QString &path, const QString &iface)
{
    auto msg = QDBusMessage::createMethodCall(service, path, kProps, QStringLiteral("GetAll"));
    msg << iface;
    QDBusReply<QVariantMap> r = QDBusConnection::systemBus().call(msg, QDBus::Block, 2000);
    return r.isValid() ? r.value() : QVariantMap();
}

QString duration(qint64 secs)
{
    const qint64 m = (secs + 30) / 60;
    if (m < 60)
        return QStringLiteral("%1 min").arg(m);
    return m % 60 ? QStringLiteral("%1 h %2 min").arg(m / 60).arg(m % 60) : QStringLiteral("%1 h").arg(m / 60);
}

// Line under the percentage, e.g. "Discharging · 5 h 31 min left"
QString statusText(const QVariantMap &d)
{
    const auto st = d.value(QStringLiteral("State")).toUInt();
    const qint64 toFull = d.value(QStringLiteral("TimeToFull")).toLongLong();
    const qint64 toEmpty = d.value(QStringLiteral("TimeToEmpty")).toLongLong();
    switch (st) {
    case Charging:
        return toFull > 0 ? QStringLiteral("Charging · full in ") + duration(toFull) : QStringLiteral("Charging");
    case Discharging:
        return toEmpty > 0 ? QStringLiteral("On battery · ") + duration(toEmpty) + QStringLiteral(" left") : QStringLiteral("On battery");
    case Full:
        return QStringLiteral("Fully charged");
    case PendingCharge:
        return QStringLiteral("Plugged in · not charging");
    case Empty:
        return QStringLiteral("Empty");
    default:
        return QStringLiteral("Plugged in");
    }
}

QPushButton *segment(const QString &text, const QString &icon)
{
    auto *b = new QPushButton(QIcon::fromTheme(icon), text);
    b->setCheckable(true);
    b->setMinimumHeight(34);
    return b;
}
}

BatteryPanel::BatteryPanel()
    : Popup(QStringLiteral("Battery"))
{
    resize(380, 10); // height follows the content
    setStyleSheet(QStringLiteral(
        "#card{border-radius:10px;background:palette(alternate-base)}"
        "QProgressBar{max-height:6px;border:none;border-radius:3px;background:palette(midlight)}"
        "QProgressBar::chunk{border-radius:3px;background:palette(highlight)}"
        "QPushButton:checked{background:palette(highlight);color:palette(highlighted-text)}"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);
    root->setSizeConstraint(QLayout::SetFixedSize);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>Battery</b>"));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.25);
    title->setFont(tf);
    auto *settings = new QToolButton;
    settings->setIcon(QIcon::fromTheme(QStringLiteral("configure-symbolic"), QIcon::fromTheme(QStringLiteral("configure"))));
    settings->setToolTip(QStringLiteral("Power management settings"));
    settings->setAutoRaise(true);
    head->addWidget(title);
    head->addStretch();
    head->addWidget(settings);
    root->addLayout(head);
    connect(settings, &QToolButton::clicked, this, [this] {
        hide();
        QProcess::startDetached(QStringLiteral("lxqt-config-powermanagement"), {});
    });

    // Charge card
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    card->setMinimumWidth(356);
    auto *cv = new QVBoxLayout(card);
    cv->setContentsMargins(12, 10, 12, 12);
    cv->setSpacing(8);
    auto *ch = new QHBoxLayout;
    ch->setSpacing(12);
    icon = new QLabel;
    icon->setFixedSize(48, 48);
    auto *texts = new QVBoxLayout;
    texts->setSpacing(0);
    percent = new QLabel;
    QFont pf = percent->font();
    pf.setPointSizeF(pf.pointSizeF() * 2);
    pf.setBold(true);
    percent->setFont(pf);
    status = new QLabel;
    details = dimLabel();
    texts->addWidget(percent);
    texts->addWidget(status);
    texts->addWidget(details);
    ch->addWidget(icon, 0, Qt::AlignTop);
    ch->addLayout(texts, 1);
    cv->addLayout(ch);
    level = new QProgressBar;
    level->setTextVisible(false);
    level->setRange(0, 100);
    cv->addWidget(level);
    root->addWidget(card);

    // Power mode
    auto *modeL = dimLabel();
    modeL->setText(QStringLiteral("Power mode"));
    root->addWidget(modeL);
    auto *seg = new QHBoxLayout;
    seg->setSpacing(6);
    profiles = new QButtonGroup(this);
    profiles->setExclusive(true);
    const struct { const char *id, *label, *icon; } modes[] = {
        {"power-saver", "Saver", "power-profile-power-saver-symbolic"},
        {"balanced", "Balanced", "power-profile-balanced-symbolic"},
        {"performance", "Performance", "power-profile-performance-symbolic"},
    };
    for (const auto &m : modes) {
        auto *b = segment(QString::fromLatin1(m.label), QString::fromLatin1(m.icon));
        b->setProperty("profile", QString::fromLatin1(m.id));
        profiles->addButton(b);
        seg->addWidget(b, 1);
    }
    root->addLayout(seg);
    connect(profiles, &QButtonGroup::buttonClicked, this, [this](QAbstractButton *b) { setProfile(b->property("profile").toString()); });

    ecoB = segment(QStringLiteral("Eco · ultra low power"), QStringLiteral("battery-profile-powersave-symbolic"));
    ecoB->setToolTip(QStringLiteral("3 W sustained / 12 W burst CPU cap (ultra-low-power)"));
    root->addWidget(ecoB);
    ecoPoll = new QTimer(this);
    ecoPoll->setInterval(400);
    connect(ecoPoll, &QTimer::timeout, this, &BatteryPanel::checkEco);
    connect(ecoB, &QPushButton::clicked, this, [this] {
        ecoB->setChecked(eco); // reflect the real state until the script has switched it
        ecoB->setEnabled(false);
        QProcess::startDetached(kEcoCmd, {QStringLiteral("toggle")});
        ecoPoll->setProperty("left", 75); // up to 30 s, the pkexec prompt may be showing
        ecoPoll->start();
    });

    limit = new QCheckBox;
    root->addWidget(limit);
    connect(limit, &QCheckBox::clicked, this, [this](bool on) {
        auto msg = QDBusMessage::createMethodCall(kUPower, batPath, kDevice, QStringLiteral("EnableChargeThreshold"));
        msg << on;
        auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, on] {
            w->deleteLater();
            if (w->isError()) {
                limit->setChecked(!on);
                note->setText(QStringLiteral("Couldn't change the charge limit: ") + w->error().message());
                note->show();
            }
        });
    });

    note = dimLabel();
    note->setWordWrap(true);
    note->hide();
    root->addWidget(note);

    // Find the real battery (for health and charge limit); DisplayDevice is the combined view.
    auto bus = QDBusConnection::systemBus();
    QDBusReply<QList<QDBusObjectPath>> devs = bus.call(QDBusMessage::createMethodCall(kUPower, QStringLiteral("/org/freedesktop/UPower"), kUPower, QStringLiteral("EnumerateDevices")));
    if (devs.isValid())
        for (const auto &p : devs.value()) {
            auto props = getAll(kUPower, p.path(), kDevice);
            if (props.value(QStringLiteral("Type")).toUInt() == 2 && props.value(QStringLiteral("PowerSupply")).toBool()) {
                batPath = p.path();
                bat = props;
                break;
            }
        }
    disp = getAll(kUPower, kDisplayPath, kDevice);
    profile = getAll(kPPD, kPPDPath, kPPD).value(QStringLiteral("ActiveProfile")).toString();
    eco = QFile::exists(kEcoFlag);

    bus.connect(kUPower, kDisplayPath, kProps, QStringLiteral("PropertiesChanged"), this, SLOT(displayChanged(QString, QVariantMap, QStringList)));
    if (!batPath.isEmpty())
        bus.connect(kUPower, batPath, kProps, QStringLiteral("PropertiesChanged"), this, SLOT(batteryChanged(QString, QVariantMap, QStringList)));
    bus.connect(kPPD, kPPDPath, kProps, QStringLiteral("PropertiesChanged"), this, SLOT(profileChanged(QString, QVariantMap, QStringList)));

    tray = new QSystemTrayIcon(this);
    connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::MiddleClick)
            toggleVisible();
    });
    updateTray(); // shows the tray icon only when there is a battery (desktops and VMs have none)
}

void BatteryPanel::aboutToOpen()
{
    eco = QFile::exists(kEcoFlag); // changed outside us (keybind / panel button)? cheap stat, only on open
    note->hide();
    updatePopup();
}

void BatteryPanel::displayChanged(const QString &, const QVariantMap &changed, const QStringList &invalidated)
{
    for (auto it = changed.begin(); it != changed.end(); ++it)
        disp.insert(it.key(), it.value());
    if (!invalidated.isEmpty())
        disp = getAll(kUPower, kDisplayPath, kDevice);
    updateTray();
    if (isVisible())
        updatePopup();
}

void BatteryPanel::batteryChanged(const QString &, const QVariantMap &changed, const QStringList &invalidated)
{
    for (auto it = changed.begin(); it != changed.end(); ++it)
        bat.insert(it.key(), it.value());
    if (!invalidated.isEmpty())
        bat = getAll(kUPower, batPath, kDevice);
    if (isVisible())
        updatePopup();
}

void BatteryPanel::profileChanged(const QString &, const QVariantMap &changed, const QStringList &)
{
    if (!changed.contains(QStringLiteral("ActiveProfile")))
        return;
    profile = changed.value(QStringLiteral("ActiveProfile")).toString();
    if (isVisible())
        updatePopup();
}

void BatteryPanel::updateTray()
{
    const bool present = disp.value(QStringLiteral("IsPresent")).toBool();
    if (tray->isVisible() != present)
        tray->setVisible(present);
    const int pct = qRound(disp.value(QStringLiteral("Percentage")).toDouble());
    const auto st = disp.value(QStringLiteral("State")).toUInt();
    const bool plugged = st == Charging || st == Full || st == PendingCharge;
    const QString name = QStringLiteral("battery-level-%1%2-symbolic").arg(qBound(0, (pct + 5) / 10 * 10, 100)).arg(plugged ? QStringLiteral("-charging") : QString());
    if (name != trayIconName) { // percentage changes ~every few minutes, the icon only every 10%
        trayIconName = name;
        const QIcon i = QIcon::fromTheme(name, QIcon::fromTheme(disp.value(QStringLiteral("IconName")).toString()));
        tray->setIcon(i);
        icon->setPixmap(i.pixmap(48, 48));
    }
    tray->setToolTip(QStringLiteral("Battery %1% · ").arg(pct) + statusText(disp));
}

void BatteryPanel::updatePopup()
{
    const double pct = disp.value(QStringLiteral("Percentage")).toDouble();
    percent->setText(QStringLiteral("%1%").arg(qRound(pct)));
    status->setText(statusText(disp));
    level->setValue(qRound(pct));

    QStringList d;
    const double rate = disp.value(QStringLiteral("EnergyRate")).toDouble();
    const auto st = disp.value(QStringLiteral("State")).toUInt();
    if (rate > 0.05)
        d << (st == Charging ? QStringLiteral("+%1 W") : QStringLiteral("%1 W")).arg(rate, 0, 'f', 1);
    if (bat.contains(QStringLiteral("Capacity")))
        d << QStringLiteral("Health %1%").arg(qRound(bat.value(QStringLiteral("Capacity")).toDouble()));
    details->setText(d.join(QStringLiteral(" · ")));
    details->setVisible(!d.isEmpty());

    for (auto *b : profiles->buttons()) { // no power-profiles-daemon (TLP, tuned): no profile buttons
        b->setChecked(b->property("profile").toString() == profile);
        b->setVisible(!profile.isEmpty());
    }
    ecoB->setChecked(eco);
    ecoB->setVisible(QFile::exists(kEcoCmd));

    const bool supported = bat.value(QStringLiteral("ChargeThresholdSupported")).toBool();
    limit->setVisible(supported);
    limit->setText(QStringLiteral("Limit charging to %1% (protects the battery)").arg(bat.value(QStringLiteral("ChargeEndThreshold")).toUInt()));
    limit->setChecked(bat.value(QStringLiteral("ChargeThresholdEnabled")).toBool());
}

void BatteryPanel::setProfile(const QString &p)
{
    auto msg = QDBusMessage::createMethodCall(kPPD, kPPDPath, kProps, QStringLiteral("Set"));
    msg << kPPD << QStringLiteral("ActiveProfile") << QVariant::fromValue(QDBusVariant(p));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
        w->deleteLater();
        if (w->isError()) {
            note->setText(QStringLiteral("Couldn't change power mode: ") + w->error().message());
            note->show();
            updatePopup(); // put the buttons back
        }
    });
}

void BatteryPanel::checkEco()
{
    const bool now = QFile::exists(kEcoFlag);
    const int left = ecoPoll->property("left").toInt() - 1;
    ecoPoll->setProperty("left", left);
    if (now != eco || left <= 0) {
        eco = now;
        ecoPoll->stop();
        ecoB->setEnabled(true);
        if (isVisible())
            updatePopup();
    }
}
