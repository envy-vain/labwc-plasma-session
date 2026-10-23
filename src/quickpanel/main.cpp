// quickpanel: native Wi-Fi, battery, Bluetooth and volume tray icons + Plasma-style popups for LXQt/labwc, in one resident
// process so they share a single Qt. Wi-Fi has a built-in speed test (only ever started by its button).
// Event-driven (NetworkManager / UPower D-Bus signals, no polling); `quickpanel wifi|battery` from a
// panel button or keybind reaches the running instance over a unix socket before Qt even starts.
// Installed as `wifi-panel` too, which means `quickpanel wifi`.
#include "battery.h"
#include "bluetooth.h"
#include "volume.h"
#include "popup.h"
#include "speedtest.h"

#include <NetworkManagerQt/AccessPoint>
#include <NetworkManagerQt/ActiveConnection>
#include <NetworkManagerQt/Connection>
#include <NetworkManagerQt/ConnectionSettings>
#include <NetworkManagerQt/Ipv4Setting>
#include <NetworkManagerQt/Manager>
#include <NetworkManagerQt/Settings>
#include <NetworkManagerQt/WiredDevice>
#include <NetworkManagerQt/WirelessDevice>
#include <NetworkManagerQt/WirelessNetwork>
#include <NetworkManagerQt/WirelessSetting>

#include <QApplication>
#include <QCheckBox>
#include <QDBusPendingCallWatcher>
#include <QElapsedTimer>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

using namespace NetworkManager;

namespace {

QString socketPath()
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    return QString::fromLocal8Bit(rt ? rt : "/tmp") + QStringLiteral("/quickpanel.sock");
}

// Plain POSIX so a toggle costs a few ms instead of a Qt startup.
bool sendToRunning(const char *cmd)
{
    const QByteArray path = socketPath().toLocal8Bit();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.constData(), sizeof(addr.sun_path) - 1);
    bool ok = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0
        && write(fd, cmd, strlen(cmd)) > 0;
    close(fd);
    return ok;
}

enum class Sec { Open, Psk, Sae, Enterprise, Wep };

Sec securityOf(const AccessPoint::Ptr &ap)
{
    if (!ap)
        return Sec::Open;
    const auto f = ap->wpaFlags() | ap->rsnFlags();
    if (f & AccessPoint::KeyMgmt8021x || f & AccessPoint::KeyMgmtEapSuiteB192)
        return Sec::Enterprise;
    if (f & AccessPoint::KeyMgmtPsk)
        return Sec::Psk; // includes WPA2/WPA3 transition networks
    if (f & AccessPoint::KeyMgmtSAE)
        return Sec::Sae;
    if (ap->capabilities() & AccessPoint::Privacy)
        return Sec::Wep;
    return Sec::Open;
}

QString securityName(Sec s)
{
    switch (s) {
    case Sec::Open: return QStringLiteral("Open");
    case Sec::Psk: return QStringLiteral("WPA2");
    case Sec::Sae: return QStringLiteral("WPA3");
    case Sec::Enterprise: return QStringLiteral("Enterprise");
    case Sec::Wep: return QStringLiteral("WEP");
    }
    return {};
}

int level(int strength) // 0..4, used to skip icon updates that wouldn't change anything
{
    return strength >= 80 ? 4 : strength >= 55 ? 3 : strength >= 30 ? 2 : strength >= 5 ? 1 : 0;
}

QIcon signalIcon(int strength, bool secure)
{
    static const char *names[] = {"none", "weak", "ok", "good", "excellent"};
    static const int breeze[] = {0, 20, 40, 80, 100};
    const int l = level(strength);
    const QString n = QString::fromLatin1(names[l]);
    return QIcon::fromTheme(
        QStringLiteral("network-wireless-signal-") + n + (secure ? QStringLiteral("-secure-symbolic") : QStringLiteral("-symbolic")),
        QIcon::fromTheme(QStringLiteral("network-wireless-signal-") + n + QStringLiteral("-symbolic"),
                         QIcon::fromTheme(QStringLiteral("network-wireless-%1%2").arg(breeze[l]).arg(secure ? QStringLiteral("-locked") : QString()))));
}

QString band(uint mhz)
{
    return mhz >= 5925 ? QStringLiteral("6 GHz") : mhz >= 4900 ? QStringLiteral("5 GHz") : QStringLiteral("2.4 GHz");
}

QString mbps(double v)
{
    return v >= 100 ? QString::number(v, 'f', 0) : v >= 10 ? QString::number(v, 'f', 1) : QString::number(v, 'f', 2);
}

QLabel *dimLabel(QWidget *parent = nullptr)
{
    auto *l = new QLabel(parent);
    QPalette p = l->palette();
    QColor c = p.color(QPalette::WindowText);
    c.setAlphaF(0.65);
    p.setColor(QPalette::WindowText, c);
    l->setPalette(p);
    return l;
}

QString wiredLabel(const QString &driver)
{
    static const QSet<QString> phone{QStringLiteral("rndis_host"), QStringLiteral("cdc_ncm"), QStringLiteral("ipheth")};
    static const QSet<QString> usb{QStringLiteral("r8152"), QStringLiteral("ax88179_178a"), QStringLiteral("asix"),
                                   QStringLiteral("cdc_ether"), QStringLiteral("smsc95xx"), QStringLiteral("r8153_ecm")};
    return phone.contains(driver) ? QStringLiteral("Phone (USB tethering)") : usb.contains(driver) ? QStringLiteral("USB Ethernet") : QStringLiteral("Ethernet");
}

// One wired adapter: name, state/speed/IP, connect or disconnect.
class WiredRow : public QFrame {
public:
    WiredRow()
    {
        setObjectName(QStringLiteral("row"));
        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(10, 7, 10, 7);
        h->setSpacing(10);
        icon = new QLabel;
        icon->setFixedSize(24, 24);
        icon->setPixmap(QIcon::fromTheme(QStringLiteral("network-wired-symbolic"), QIcon::fromTheme(QStringLiteral("network-wired"))).pixmap(24, 24));
        auto *t = new QVBoxLayout;
        t->setSpacing(0);
        name = new QLabel;
        sub = dimLabel();
        t->addWidget(name);
        t->addWidget(sub);
        button = new QPushButton;
        h->addWidget(icon);
        h->addLayout(t, 1);
        h->addWidget(button);
    }
    QLabel *icon, *name, *sub;
    QPushButton *button;
};

// One network in the list; expands in place for password / connect / forget.
class Row : public QFrame {
    Q_OBJECT
public:
    explicit Row(QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("row"));
        setCursor(Qt::PointingHandCursor);
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(10, 7, 10, 7);
        v->setSpacing(6);
        auto *h = new QHBoxLayout;
        h->setSpacing(10);
        icon = new QLabel;
        icon->setFixedSize(24, 24);
        auto *texts = new QVBoxLayout;
        texts->setSpacing(0);
        name = new QLabel;
        sub = dimLabel();
        texts->addWidget(name);
        texts->addWidget(sub);
        h->addWidget(icon);
        h->addLayout(texts, 1);
        quick = new QPushButton;
        quick->setCursor(Qt::ArrowCursor);
        h->addWidget(quick);
        v->addLayout(h);

        extra = new QWidget;
        auto *e = new QVBoxLayout(extra);
        e->setContentsMargins(34, 0, 0, 2);
        e->setSpacing(6);
        error = new QLabel;
        error->setStyleSheet(QStringLiteral("color:#e5534b"));
        error->setWordWrap(true);
        pw = new QLineEdit;
        pw->setEchoMode(QLineEdit::Password);
        pw->setPlaceholderText(QStringLiteral("Password"));
        pw->setClearButtonEnabled(true);
        auto *show = pw->addAction(QIcon::fromTheme(QStringLiteral("view-visible-symbolic"), QIcon::fromTheme(QStringLiteral("view-visible"))), QLineEdit::TrailingPosition);
        show->setCheckable(true);
        connect(show, &QAction::toggled, pw, [this](bool on) { pw->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
        auto *btns = new QHBoxLayout;
        forget = new QPushButton(QStringLiteral("Forget"));
        connectB = new QPushButton(QStringLiteral("Connect"));
        connectB->setDefault(true);
        btns->addWidget(forget);
        btns->addStretch();
        btns->addWidget(connectB);
        e->addWidget(error);
        e->addWidget(pw);
        e->addLayout(btns);
        v->addWidget(extra);
        extra->hide();
        connect(pw, &QLineEdit::returnPressed, connectB, &QPushButton::click);
    }

    void mouseReleaseEvent(QMouseEvent *ev) override
    {
        if (ev->button() == Qt::LeftButton)
            Q_EMIT clicked();
    }

    QString ssid;
    QLabel *icon, *name, *sub, *error;
    QPushButton *quick, *forget, *connectB;
    QLineEdit *pw;
    QWidget *extra;
    int iconKey = -1;

Q_SIGNALS:
    void clicked();
};

class WifiPanel : public Popup {
    Q_OBJECT
public:
    WifiPanel()
        : Popup(QStringLiteral("Wi-Fi"))
    {
        resize(420, 560);
        setObjectName(QStringLiteral("panel"));
        setStyleSheet(QStringLiteral(
            "#row{border-radius:8px}"
            "#row:hover{background:palette(midlight)}"
            "#row[active=\"true\"]{background:palette(alternate-base)}"
            "#card{border-radius:10px;background:palette(alternate-base)}"
            "QScrollArea{border:none;background:transparent}"
            "#list{background:transparent}"
            "QProgressBar{max-height:4px;border:none;border-radius:2px;background:palette(midlight)}"
            "QProgressBar::chunk{border-radius:2px;background:palette(highlight)}"));

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(10);

        // Header: Wi-Fi switch, rescan, settings
        auto *head = new QHBoxLayout;
        auto *title = new QLabel(QStringLiteral("<b>Wi-Fi</b>"));
        QFont tf = title->font();
        tf.setPointSizeF(tf.pointSizeF() * 1.25);
        title->setFont(tf);
        toggle = new QCheckBox(QStringLiteral("On"));
        auto *rescan = toolButton(QStringLiteral("view-refresh"), QStringLiteral("Rescan"));
        auto *settings = toolButton(QStringLiteral("configure"), QStringLiteral("Network settings (hidden networks, VPN, enterprise)"));
        head->addWidget(title);
        head->addStretch();
        head->addWidget(toggle);
        head->addWidget(rescan);
        head->addWidget(settings);
        root->addLayout(head);
        connect(toggle, &QCheckBox::clicked, this, [](bool on) { setWirelessEnabled(on); });
        connect(rescan, &QToolButton::clicked, this, [this] { scan(true); });
        connect(settings, &QToolButton::clicked, this, [this] {
            hide();
            QProcess::startDetached(QStringLiteral("nm-connection-editor"), {});
        });

        wiredBox = new QWidget;
        wiredLay = new QVBoxLayout(wiredBox);
        wiredLay->setContentsMargins(0, 0, 0, 0);
        wiredLay->setSpacing(2);
        root->addWidget(wiredBox);
        wiredBox->hide();

        buildCard(root);

        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *list = new QWidget;
        list->setObjectName(QStringLiteral("list"));
        listLay = new QVBoxLayout(list);
        listLay->setContentsMargins(0, 0, 0, 0);
        listLay->setSpacing(2);
        listLay->addStretch();
        scroll->setWidget(list);
        root->addWidget(scroll, 1);
        emptyLabel = dimLabel();
        emptyLabel->setAlignment(Qt::AlignCenter);
        root->addWidget(emptyLabel);

        refreshTimer.setSingleShot(true);
        refreshTimer.setInterval(120); // coalesce bursts of NM signals into one update
        connect(&refreshTimer, &QTimer::timeout, this, &WifiPanel::refresh);

        tray = new QSystemTrayIcon(this);
        connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::MiddleClick)
                toggleVisible();
        });

        connect(notifier(), &Notifier::wirelessEnabledChanged, this, &WifiPanel::schedule);
        connect(notifier(), &Notifier::deviceAdded, this, &WifiPanel::findDevice);
        connect(notifier(), &Notifier::deviceRemoved, this, &WifiPanel::findDevice);
        connect(settingsNotifier(), &SettingsNotifier::connectionAdded, this, &WifiPanel::schedule);
        connect(settingsNotifier(), &SettingsNotifier::connectionRemoved, this, &WifiPanel::schedule);
        findDevice();
        refresh();
        tray->show();
    }

protected:
    void aboutToOpen() override
    {
        scan(false);
        refresh();
    }

private:
    QToolButton *toolButton(const QString &icon, const QString &tip)
    {
        auto *b = new QToolButton;
        b->setIcon(QIcon::fromTheme(icon + QStringLiteral("-symbolic"), QIcon::fromTheme(icon)));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    }

    void buildCard(QVBoxLayout *root)
    {
        card = new QFrame;
        card->setObjectName(QStringLiteral("card"));
        auto *v = new QVBoxLayout(card);
        v->setContentsMargins(12, 10, 12, 12);
        v->setSpacing(8);
        auto *h = new QHBoxLayout;
        cardIcon = new QLabel;
        cardIcon->setFixedSize(32, 32);
        auto *t = new QVBoxLayout;
        t->setSpacing(0);
        cardName = new QLabel;
        QFont f = cardName->font();
        f.setBold(true);
        cardName->setFont(f);
        cardInfo = dimLabel();
        cardInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cardInfo->setWordWrap(true); // keep the popup at its normal width when the details are long
        t->addWidget(cardName);
        t->addWidget(cardInfo);
        auto *disc = discB = new QPushButton(QStringLiteral("Disconnect"));
        h->addWidget(cardIcon);
        h->addLayout(t, 1);
        h->addWidget(disc);
        wifiHead = new QWidget;
        h->setContentsMargins(0, 0, 0, 0);
        wifiHead->setLayout(h);
        v->addWidget(wifiHead);
        connect(disc, &QPushButton::clicked, this, [this] {
            if (dev)
                dev->disconnectInterface();
        });

        // Speed test: three result tiles + a run/stop button
        auto *tiles = new QHBoxLayout;
        tiles->setSpacing(6);
        auto tile = [&](const QString &caption, QLabel *&value) {
            auto *w = new QVBoxLayout;
            w->setSpacing(0);
            auto *c = dimLabel();
            c->setText(caption);
            value = new QLabel(QStringLiteral("–"));
            QFont vf = value->font();
            vf.setPointSizeF(vf.pointSizeF() * 1.35);
            vf.setBold(true);
            value->setFont(vf);
            w->addWidget(c);
            w->addWidget(value);
            tiles->addLayout(w, 1);
        };
        tile(QStringLiteral("Ping"), pingL);
        tile(QStringLiteral("Download"), downL);
        tile(QStringLiteral("Upload"), upL);
        runB = new QPushButton(QStringLiteral("Speed test"));
        runB->setIcon(QIcon::fromTheme(QStringLiteral("speedometer"), QIcon::fromTheme(QStringLiteral("media-playback-start"))));
        tiles->addWidget(runB, 0, Qt::AlignBottom);
        v->addLayout(tiles);
        bar = new QProgressBar;
        bar->setTextVisible(false);
        bar->setRange(0, 1000);
        bar->hide();
        testInfo = dimLabel();
        testInfo->setWordWrap(true);
        testInfo->hide();
        v->addWidget(bar);
        v->addWidget(testInfo);
        root->addWidget(card);

        test = new SpeedTest(this);
        connect(runB, &QPushButton::clicked, this, [this] {
            if (test->running()) {
                test->stop();
                testInfo->setText(QStringLiteral("Stopped"));
                return;
            }
            pingL->setText(QStringLiteral("…"));
            downL->setText(QStringLiteral("–"));
            upL->setText(QStringLiteral("–"));
            testInfo->setText(QStringLiteral("Measuring latency…"));
            testInfo->show();
            bar->setValue(0);
            bar->show();
            runB->setText(QStringLiteral("Stop"));
            test->start();
        });
        connect(test, &SpeedTest::ping, this, [this](double ms, double jitter) {
            pingL->setText(QString::number(ms, 'f', 0) + QStringLiteral(" ms"));
            pingL->setToolTip(QStringLiteral("Jitter %1 ms").arg(jitter, 0, 'f', 1));
            testInfo->setText(QStringLiteral("Testing download…"));
        });
        connect(test, &SpeedTest::progress, this, [this](SpeedTest::Phase p, double v, double frac) {
            (p == SpeedTest::Download ? downL : upL)->setText(mbps(v) + QStringLiteral(" Mb/s"));
            bar->setValue(int(((p == SpeedTest::Upload ? 1 : 0) + frac) * 500));
        });
        connect(test, &SpeedTest::phaseDone, this, [this](SpeedTest::Phase p, double v) {
            (p == SpeedTest::Download ? downL : upL)->setText(mbps(v) + QStringLiteral(" Mb/s"));
            if (p == SpeedTest::Download)
                testInfo->setText(QStringLiteral("Testing upload…"));
            else
                testInfo->setText(QStringLiteral("Done · %1 · Cloudflare").arg(QTime::currentTime().toString(QStringLiteral("HH:mm"))));
        });
        connect(test, &SpeedTest::finished, this, [this] {
            runB->setText(QStringLiteral("Speed test"));
            bar->hide();
        });
        connect(test, &SpeedTest::failed, this, [this](const QString &err) {
            testInfo->setText(QStringLiteral("Speed test failed: ") + err);
            if (pingL->text() == QStringLiteral("…"))
                pingL->setText(QStringLiteral("–"));
        });
    }

    void findDevice()
    {
        WirelessDevice::Ptr found;
        QList<WiredDevice::Ptr> eth;
        for (const auto &d : networkInterfaces()) {
            if (!d->managed())
                continue;
            if (d->type() == Device::Wifi && !found)
                found = d.objectCast<WirelessDevice>();
            else if (d->type() == Device::Ethernet)
                eth << d.objectCast<WiredDevice>();
        }
        if (eth != wired) {
            for (const auto &w : std::as_const(wired))
                w->disconnect(this);
            wired = eth;
            for (const auto &w : std::as_const(wired)) {
                auto *d = w.data();
                connect(d, &WiredDevice::carrierChanged, this, &WifiPanel::schedule);
                connect(d, &WiredDevice::bitRateChanged, this, &WifiPanel::schedule);
                connect(d, &Device::stateChanged, this, &WifiPanel::schedule);
                connect(d, &Device::ipV4ConfigChanged, this, &WifiPanel::schedule);
            }
            schedule();
        }
        if (found == dev)
            return;
        if (dev)
            dev->disconnect(this);
        dev = found;
        if (dev) {
            auto *d = dev.data();
            connect(d, &WirelessDevice::networkAppeared, this, [this](const QString &ssid) { watchNetwork(ssid); schedule(); });
            connect(d, &WirelessDevice::networkDisappeared, this, &WifiPanel::schedule);
            connect(d, &WirelessDevice::activeAccessPointChanged, this, &WifiPanel::schedule);
            connect(d, &WirelessDevice::bitRateChanged, this, &WifiPanel::schedule);
            connect(d, &Device::ipV4ConfigChanged, this, &WifiPanel::schedule);
            connect(d, &Device::stateChanged, this, &WifiPanel::deviceState);
            for (const auto &n : dev->networks())
                watchNetwork(n->ssid());
        }
        schedule();
    }

    void watchNetwork(const QString &ssid)
    {
        if (auto n = dev ? dev->findNetwork(ssid) : WirelessNetwork::Ptr())
            connect(n.data(), &WirelessNetwork::signalStrengthChanged, this, &WifiPanel::schedule, Qt::UniqueConnection);
    }

    void schedule()
    {
        if (!refreshTimer.isActive())
            refreshTimer.start();
    }

    void scan(bool force)
    {
        // NM rate-limits scans itself; skip if one ran in the last 10 s unless asked explicitly.
        if (dev && isWirelessEnabled() && (force || !dev->lastScan().isValid() || dev->lastScan().secsTo(QDateTime::currentDateTime()) > 10))
            dev->requestScan();
    }

    void deviceState(Device::State now, Device::State, Device::StateChangeReason reason)
    {
        if (pending.isEmpty())
            return schedule();
        if (now == Device::Activated) {
            pending.clear();
            expanded.clear();
            errorText.clear();
        } else if (now == Device::Failed || (now == Device::Disconnected && reason != Device::NoReason && reason != Device::UserRequestedReason)) {
            const bool auth = reason == Device::NoSecretsReason || reason == Device::AuthSupplicantDisconnectReason
                || reason == Device::AuthSupplicantFailedReason || reason == Device::AuthSupplicantTimeoutReason;
            errorText = auth ? QStringLiteral("Couldn't connect, wrong password?") : QStringLiteral("Couldn't connect to this network");
            expanded = pending;
            needPassword = auth;
            pending.clear();
        }
        schedule();
    }

    QHash<QString, Connection::Ptr> savedConnections() const
    {
        QHash<QString, Connection::Ptr> out;
        for (const auto &c : listConnections()) {
            auto s = c->settings();
            if (s->connectionType() != ConnectionSettings::Wireless)
                continue;
            auto w = s->setting(Setting::Wireless).staticCast<WirelessSetting>();
            // A hotspot profile shares its SSID with the network it broadcasts; activating it would start the AP.
            if (w->mode() != WirelessSetting::Infrastructure)
                continue;
            out.insert(QString::fromUtf8(w->ssid()), c);
        }
        return out;
    }

    void refresh()
    {
        const bool on = isWirelessEnabled();
        toggle->setChecked(on);

        AccessPoint::Ptr active = dev ? dev->activeAccessPoint() : AccessPoint::Ptr();
        hotspot = dev && dev->state() == Device::Activated && dev->mode() == WirelessDevice::ApMode;
        const bool connected = !hotspot && dev && active && dev->state() == Device::Activated;
        const QString activeSsid = active ? active->ssid() : QString();
        WiredDevice::Ptr wiredUp;
        for (const auto &w : std::as_const(wired))
            if (w->state() == Device::Activated) {
                wiredUp = w;
                break;
            }
        updateTray(on, connected, active, wiredUp);

        // Card: Wi-Fi details when on Wi-Fi, speed test whenever anything is connected
        card->setVisible(connected || hotspot || wiredUp);
        wifiHead->setVisible(connected || hotspot);
        if (!isVisible())
            return;
        refreshWired();
        discB->setText(hotspot ? QStringLiteral("Stop hotspot") : QStringLiteral("Disconnect"));
        if (hotspot) {
            cardIcon->setPixmap(QIcon::fromTheme(QStringLiteral("network-wireless-hotspot-symbolic"), QIcon::fromTheme(QStringLiteral("network-wireless-hotspot"))).pixmap(32, 32));
            cardName->setText(QStringLiteral("Hotspot · ") + hotspotSsid());
            const int n = hotspotClients();
            QStringList info{n == 1 ? QStringLiteral("1 device connected") : QStringLiteral("%1 devices connected").arg(n)};
            if (active)
                info << band(active->frequency());
            if (wiredUp)
                info << QStringLiteral("sharing ") + wiredLabel(wiredUp->driver());
            cardInfo->setText(info.join(QStringLiteral(" · ")));
        } else if (connected) {
            const int s = active->signalStrength();
            cardIcon->setPixmap(signalIcon(s, securityOf(active) != Sec::Open).pixmap(32, 32));
            cardName->setText(activeSsid);
            QStringList info{QStringLiteral("%1%").arg(s), band(active->frequency())};
            if (dev->bitRate() > 0)
                info << QStringLiteral("%1 Mb/s link").arg(dev->bitRate() / 1000);
            const auto addrs = dev->ipV4Config().addresses();
            if (!addrs.isEmpty())
                info << addrs.first().ip().toString();
            cardInfo->setText(info.join(QStringLiteral(" · ")));
        } else if (!hotspot && !wiredUp && test->running()) {
            test->stop();
        }

        const auto saved = savedConnections();
        struct Item { WirelessNetwork::Ptr net; int score; };
        QList<Item> items;
        if (dev && on && !hotspot) // the radio is busy being the access point
            for (const auto &n : dev->networks()) {
                if (n->ssid().isEmpty() || (connected && n->ssid() == activeSsid))
                    continue; // hidden networks: use settings; the connected one is the card above
                int score = n->signalStrength();
                if (saved.contains(n->ssid()))
                    score += 1000;
                items.push_back({n, score});
            }
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.score > b.score; });

        // Reuse rows so an open password field survives updates; re-add in sorted order.
        QHash<QString, Row *> old;
        old.swap(rows);
        while (listLay->count() > 1)
            listLay->takeAt(0);
        int idx = 0;
        for (const auto &it : items) {
            const QString ssid = it.net->ssid();
            Row *r = old.take(ssid);
            if (!r)
                r = makeRow(ssid);
            rows.insert(ssid, r);
            updateRow(r, it.net, saved.value(ssid), ssid == activeSsid && connected);
            listLay->insertWidget(idx++, r);
        }
        qDeleteAll(old);

        emptyLabel->setText(!dev ? QStringLiteral("No Wi-Fi adapter") : !on ? QStringLiteral("Wi-Fi is off")
                            : hotspot ? QStringLiteral("Wi-Fi is running the hotspot")
                            : connected ? QStringLiteral("No other networks in range") : QStringLiteral("Searching for networks…"));
        emptyLabel->setVisible(items.isEmpty());
    }

    void updateTray(bool on, bool connected, const AccessPoint::Ptr &ap, const WiredDevice::Ptr &eth)
    {
        if (eth) {
            if (trayKey != -3) {
                trayKey = -3;
                tray->setIcon(QIcon::fromTheme(QStringLiteral("network-wired-symbolic"), QIcon::fromTheme(QStringLiteral("network-wired"))));
            }
            QString tip = QStringLiteral("%1 · %2 Mb/s").arg(wiredLabel(eth->driver())).arg(eth->bitRate() / 1000);
            if (hotspot)
                tip += QStringLiteral("\nHotspot ") + hotspotSsid() + QStringLiteral(" on");
            else if (connected)
                tip += QStringLiteral("\n%1 · %2%").arg(ap->ssid()).arg(ap->signalStrength());
            tray->setToolTip(tip);
            return;
        }
        if (hotspot) {
            if (trayKey != -4) {
                trayKey = -4;
                tray->setIcon(QIcon::fromTheme(QStringLiteral("network-wireless-hotspot-symbolic"), QIcon::fromTheme(QStringLiteral("network-wireless-hotspot"))));
            }
            tray->setToolTip(QStringLiteral("Hotspot ") + hotspotSsid() + QStringLiteral(" on"));
            return;
        }
        const int key = !on ? -2 : !connected ? -1 : level(ap->signalStrength());
        if (key != trayKey) {
            trayKey = key;
            tray->setIcon(key == -2 ? QIcon::fromTheme(QStringLiteral("network-wireless-disconnected-symbolic"), QIcon::fromTheme(QStringLiteral("network-wireless-disconnected")))
                          : key == -1 ? QIcon::fromTheme(QStringLiteral("network-wireless-offline-symbolic"), QIcon::fromTheme(QStringLiteral("network-wireless-0")))
                                      : signalIcon(ap->signalStrength(), false));
        }
        tray->setToolTip(!on ? QStringLiteral("Wi-Fi off") : !connected ? QStringLiteral("Wi-Fi: not connected")
                                                            : QStringLiteral("%1 · %2%").arg(ap->ssid()).arg(ap->signalStrength()));
    }

    QString hotspotSsid() const
    {
        if (auto ac = dev ? dev->activeConnection() : ActiveConnection::Ptr())
            return ac->id().trimmed();
        return QStringLiteral("hotspot");
    }

    // Devices on the hotspot = complete ARP entries on the Wi-Fi interface (only read while the popup is open)
    int hotspotClients() const
    {
        QFile f(QStringLiteral("/proc/net/arp"));
        if (!dev || !f.open(QIODevice::ReadOnly))
            return 0;
        const QByteArray ifname = dev->interfaceName().toUtf8();
        int n = 0;
        for (const QByteArray &line : f.readAll().split('\n')) {
            const auto cols = line.simplified().split(' ');
            if (cols.size() >= 6 && cols[5] == ifname && cols[2] == "0x2")
                ++n;
        }
        return n;
    }

    void refreshWired()
    {
        // Show adapters with a cable/link or that NM is using; an empty built-in port is just noise.
        QList<WiredDevice::Ptr> shown;
        for (const auto &w : std::as_const(wired))
            if (w->carrier() || (w->state() > Device::Disconnected && w->state() <= Device::Activated))
                shown << w;
        QHash<QString, WiredRow *> old;
        old.swap(wiredRows);
        while (wiredLay->count())
            wiredLay->takeAt(0);
        for (const auto &w : std::as_const(shown)) {
            WiredRow *r = old.take(w->uni());
            if (!r) {
                r = new WiredRow;
                const QString uni = w->uni();
                connect(r->button, &QPushButton::clicked, this, [this, uni] { toggleWired(uni); });
            }
            wiredRows.insert(w->uni(), r);
            const auto st = w->state();
            const bool up = st == Device::Activated;
            const bool busy = st > Device::Disconnected && st < Device::Activated;
            r->name->setText(wiredLabel(w->driver()) + QStringLiteral(" (") + w->interfaceName() + QLatin1Char(')'));
            QStringList info{up ? QStringLiteral("Connected") : busy ? QStringLiteral("Connecting…") : QStringLiteral("Disconnected")};
            if (w->bitRate() > 0)
                info << QStringLiteral("%1 Mb/s").arg(w->bitRate() / 1000); // NM gives Kb/s despite the docs
            if (up) {
                const auto addrs = w->ipV4Config().addresses();
                if (!addrs.isEmpty())
                    info << addrs.first().ip().toString();
            }
            r->sub->setText(info.join(QStringLiteral(" · ")));
            r->button->setText(up || busy ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
            if (r->property("active").toBool() != up) {
                r->setProperty("active", up);
                r->style()->unpolish(r);
                r->style()->polish(r);
            }
            wiredLay->addWidget(r);
        }
        qDeleteAll(old);
        wiredBox->setVisible(!shown.isEmpty());
    }

    void toggleWired(const QString &uni)
    {
        WiredDevice::Ptr w;
        for (const auto &d : std::as_const(wired))
            if (d->uni() == uni)
                w = d;
        if (!w)
            return;
        const auto st = w->state();
        if (st > Device::Disconnected && st <= Device::Activated) {
            w->disconnectInterface();
            return;
        }
        // Reuse a profile NM already has for this adapter, else let NM create a default DHCP one.
        const auto conns = w->availableConnections();
        if (!conns.isEmpty()) {
            watch(activateConnection(conns.first()->path(), w->uni(), QString()));
        } else {
            NMVariantMapMap s;
            s[QStringLiteral("connection")][QStringLiteral("id")] = wiredLabel(w->driver());
            s[QStringLiteral("connection")][QStringLiteral("type")] = QStringLiteral("802-3-ethernet");
            s[QStringLiteral("connection")][QStringLiteral("uuid")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
            watch(addAndActivateConnection(s, w->uni(), QString()));
        }
    }

    Row *makeRow(const QString &ssid)
    {
        auto *r = new Row(nullptr);
        r->ssid = ssid;
        connect(r, &Row::clicked, this, [this, ssid] {
            expanded = expanded == ssid ? QString() : ssid;
            if (expanded != ssid)
                errorText.clear();
            needPassword = false;
            refresh();
            if (Row *row = rows.value(ssid); row && row->pw->isVisible())
                row->pw->setFocus();
        });
        connect(r->quick, &QPushButton::clicked, this, [this, ssid] {
            const auto saved = savedConnections();
            auto net = dev ? dev->findNetwork(ssid) : WirelessNetwork::Ptr();
            if (!net)
                return;
            AccessPoint::Ptr active = dev->activeAccessPoint();
            if (active && active->ssid() == ssid && dev->state() == Device::Activated) {
                dev->disconnectInterface();
                return;
            }
            const Sec sec = securityOf(net->referenceAccessPoint());
            if (sec == Sec::Enterprise || sec == Sec::Wep) {
                hide();
                QProcess::startDetached(QStringLiteral("nm-connection-editor"), {QStringLiteral("--create"), QStringLiteral("--type=802-11-wireless")});
                return;
            }
            if (saved.contains(ssid) || sec == Sec::Open)
                connectTo(ssid, QString());
            else {
                expanded = ssid;
                needPassword = true;
                refresh();
                if (Row *row = rows.value(ssid))
                    row->pw->setFocus();
            }
        });
        connect(r->connectB, &QPushButton::clicked, this, [this, r] { connectTo(r->ssid, r->pw->text()); });
        connect(r->forget, &QPushButton::clicked, this, [this, ssid] {
            if (auto c = savedConnections().value(ssid))
                c->remove();
            expanded.clear();
            schedule();
        });
        return r;
    }

    void updateRow(Row *r, const WirelessNetwork::Ptr &net, const Connection::Ptr &saved, bool isActive)
    {
        const auto ap = net->referenceAccessPoint();
        const Sec sec = securityOf(ap);
        const int s = net->signalStrength();
        const int key = level(s) * 2 + (sec != Sec::Open);
        if (key != r->iconKey) {
            r->iconKey = key;
            r->icon->setPixmap(signalIcon(s, sec != Sec::Open).pixmap(24, 24));
        }
        r->name->setText(net->ssid());
        QString state = isActive ? QStringLiteral("Connected") : pending == r->ssid ? QStringLiteral("Connecting…") : saved ? QStringLiteral("Saved") : securityName(sec);
        QString band_ = ap ? band(ap->frequency()) : QString();
        r->sub->setText(QStringLiteral("%1 · %2% · %3").arg(state).arg(s).arg(band_));
        if (r->property("active").toBool() != isActive) {
            r->setProperty("active", isActive);
            r->style()->unpolish(r);
            r->style()->polish(r);
        }
        r->quick->setText(isActive ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
        r->quick->setEnabled(pending != r->ssid);

        const bool open = expanded == r->ssid;
        const bool showPw = open && sec != Sec::Open && sec != Sec::Enterprise && sec != Sec::Wep && (!saved || needPassword);
        r->extra->setVisible(open && (showPw || saved || !errorText.isEmpty()));
        r->pw->setVisible(showPw);
        r->connectB->setVisible(open && !isActive && (showPw || !saved));
        r->forget->setVisible(open && saved);
        r->error->setVisible(open && !errorText.isEmpty());
        r->error->setText(errorText);
        if (!open)
            r->pw->clear();
    }

    void connectTo(const QString &ssid, const QString &password)
    {
        if (!dev)
            return;
        auto net = dev->findNetwork(ssid);
        if (!net)
            return;
        const auto ap = net->referenceAccessPoint();
        const Sec sec = securityOf(ap);
        const auto conn = savedConnections().value(ssid);
        pending = ssid;
        errorText.clear();

        if (conn && password.isEmpty()) {
            watch(activateConnection(conn->path(), dev->uni(), ap->uni()));
        } else if (conn) {
            // Saved but its password was wrong/missing: store the new one, then connect.
            NMVariantMapMap s = conn->settings()->toMap();
            s[QStringLiteral("802-11-wireless-security")][QStringLiteral("psk")] = password;
            const QString path = conn->path(), devUni = dev->uni(), apUni = ap->uni();
            auto *w = new QDBusPendingCallWatcher(conn->update(s), this);
            connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, path, devUni, apUni] {
                w->deleteLater();
                if (w->isError())
                    return failed(w->error().message());
                watch(activateConnection(path, devUni, apUni));
            });
        } else {
            NMVariantMapMap s;
            s[QStringLiteral("connection")][QStringLiteral("id")] = ssid;
            s[QStringLiteral("connection")][QStringLiteral("type")] = QStringLiteral("802-11-wireless");
            s[QStringLiteral("connection")][QStringLiteral("uuid")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
            s[QStringLiteral("802-11-wireless")][QStringLiteral("ssid")] = ap ? ap->rawSsid() : ssid.toUtf8();
            s[QStringLiteral("802-11-wireless")][QStringLiteral("mode")] = QStringLiteral("infrastructure");
            if (sec == Sec::Psk || sec == Sec::Sae) {
                s[QStringLiteral("802-11-wireless-security")][QStringLiteral("key-mgmt")] = sec == Sec::Sae ? QStringLiteral("sae") : QStringLiteral("wpa-psk");
                s[QStringLiteral("802-11-wireless-security")][QStringLiteral("psk")] = password;
            }
            watch(addAndActivateConnection(s, dev->uni(), ap->uni()));
        }
        needPassword = false;
        schedule();
    }

    template<typename Reply>
    void watch(const Reply &reply)
    {
        auto *w = new QDBusPendingCallWatcher(reply, this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
            w->deleteLater();
            if (w->isError())
                failed(w->error().message());
        });
    }

    void failed(const QString &msg)
    {
        errorText = msg;
        expanded = pending;
        pending.clear();
        schedule();
    }

    WirelessDevice::Ptr dev;
    QList<WiredDevice::Ptr> wired;
    bool hotspot = false;
    QWidget *wiredBox, *wifiHead;
    QVBoxLayout *wiredLay;
    QHash<QString, WiredRow *> wiredRows;
    QSystemTrayIcon *tray;
    int trayKey = -100;
    QCheckBox *toggle;
    QFrame *card;
    QPushButton *discB;
    QLabel *cardIcon, *cardName, *cardInfo, *pingL, *downL, *upL, *testInfo, *emptyLabel;
    QPushButton *runB;
    QProgressBar *bar;
    SpeedTest *test;
    QVBoxLayout *listLay;
    QHash<QString, Row *> rows;
    QString expanded, pending, errorText;
    bool needPassword = false;
    QTimer refreshTimer;
};

} // namespace

int main(int argc, char **argv)
{
    // quickpanel [wifi|battery|bluetooth|volume|hide|grab-<popup> <png>]   toggle a popup (default wifi)
    // quickpanel --daemon                                                start resident, hidden
    const bool daemon = argc > 1 && strcmp(argv[1], "--daemon") == 0;
    QByteArray cmd = argc > 1 && !daemon ? QByteArray(argv[1]) : QByteArrayLiteral("wifi");
    for (int i = 2; i < argc; ++i)
        cmd += ' ' + QByteArray(argv[i]);
    if (sendToRunning(daemon ? "noop" : cmd.constData()))
        return 0;

    QApplication::setDesktopFileName(QStringLiteral("quickpanel")); // Wayland app_id, matched by the labwc window rule
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    WifiPanel wifi;
    BatteryPanel battery;
    BluetoothPanel bluetooth;
    VolumePanel volume;
    const QHash<QByteArray, Popup *> popups{{"wifi", &wifi}, {"toggle", &wifi}, {"battery", &battery}, {"bluetooth", &bluetooth}, {"volume", &volume}};

    auto handle = [&](const QByteArray &c) {
        if (Popup *p = popups.value(c)) {
            for (Popup *o : popups)
                if (o != p)
                    o->hide();
            p->toggleVisible();
        } else if (c == "hide") {
            for (Popup *o : popups)
                o->hide();
        } else if (c.startsWith("grab-")) { // debug: "grab-<popup> <png>" renders a popup to a PNG
            const int sp = c.indexOf(' ');
            if (Popup *p = popups.value(c.mid(5, sp - 5)); p && sp > 0)
                p->grab().save(QString::fromLocal8Bit(c.mid(sp + 1)));
        }
    };

    QLocalServer::removeServer(socketPath());
    QLocalServer server;
    server.listen(socketPath());
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (auto *s = server.nextPendingConnection()) {
            QObject::connect(s, &QLocalSocket::readyRead, s, [s, &handle] {
                handle(s->readAll().trimmed());
                s->disconnectFromServer();
            });
            QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
        }
    });

    if (!daemon)
        handle(cmd);
    return app.exec();
}

#include "main.moc"
