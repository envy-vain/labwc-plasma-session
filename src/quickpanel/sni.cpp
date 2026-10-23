#include "sni.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusServiceWatcher>

QDBusArgument &operator<<(QDBusArgument &a, const SniPixmap &p)
{
    a.beginStructure();
    a << p.width << p.height << p.data;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, SniPixmap &p)
{
    a.beginStructure();
    a >> p.width >> p.height >> p.data;
    a.endStructure();
    return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const SniToolTip &t)
{
    a.beginStructure();
    a << t.iconName << t.pixmaps << t.title << t.text;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, SniToolTip &t)
{
    a.beginStructure();
    a >> t.iconName >> t.pixmaps >> t.title >> t.text;
    a.endStructure();
    return a;
}

Sni::Sni(const QString &id, const QString &title, QObject *parent)
    : QObject(parent)
    , m_bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("quickpanel-sni-") + id))
    , m_id(id)
    , m_title(title)
{
    qDBusRegisterMetaType<SniPixmap>();
    qDBusRegisterMetaType<QList<SniPixmap>>();
    qDBusRegisterMetaType<SniToolTip>();
    // Counter well above Qt's own (it numbers its tray icons from 1) to keep names unique.
    static int n = 100;
    m_service = QStringLiteral("org.kde.StatusNotifierItem-%1-%2").arg(QCoreApplication::applicationPid()).arg(++n);
    m_bus.registerObject(QStringLiteral("/StatusNotifierItem"), this,
                         QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots | QDBusConnection::ExportScriptableSignals);
    m_bus.registerService(m_service);
    registerWithWatcher();
    // Panel restarted -> its watcher is new and has forgotten us.
    auto *w = new QDBusServiceWatcher(QStringLiteral("org.kde.StatusNotifierWatcher"), m_bus, QDBusServiceWatcher::WatchForRegistration, this);
    connect(w, &QDBusServiceWatcher::serviceRegistered, this, &Sni::registerWithWatcher);
}

void Sni::registerWithWatcher()
{
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.kde.StatusNotifierWatcher"), QStringLiteral("/StatusNotifierWatcher"),
                                              QStringLiteral("org.kde.StatusNotifierWatcher"), QStringLiteral("RegisterStatusNotifierItem"));
    msg << m_service;
    m_bus.asyncCall(msg);
}

void Sni::setIcon(const QString &name)
{
    if (name == m_icon)
        return;
    m_icon = name;
    Q_EMIT NewIcon();
}

void Sni::setToolTip(const QString &title, const QString &text)
{
    if (title == m_tipTitle && text == m_tipText)
        return;
    m_tipTitle = title;
    m_tipText = text;
    Q_EMIT NewToolTip();
}

void Sni::Activate(int, int) { Q_EMIT activated(); }
void Sni::SecondaryActivate(int, int) { Q_EMIT middleClicked(); }
void Sni::ContextMenu(int, int) { Q_EMIT activated(); }

void Sni::Scroll(int delta, const QString &orientation)
{
    if (orientation.compare(QStringLiteral("vertical"), Qt::CaseInsensitive) != 0)
        return;
    // Wheels send ±120 per notch, touchpads send small deltas: accumulate to whole steps.
    m_scrollAccum += delta;
    const int steps = m_scrollAccum / 120;
    if (steps) {
        m_scrollAccum -= steps * 120;
        Q_EMIT scrolled(steps);
    }
}
