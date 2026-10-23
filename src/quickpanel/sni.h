#pragma once
// Minimal StatusNotifierItem (tray icon) on its own session-bus connection. Exists because
// QSystemTrayIcon drops scroll events; the volume icon needs them. Icon by theme name only.
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QList>
#include <QObject>

struct SniPixmap {
    int width = 0, height = 0;
    QByteArray data;
};
struct SniToolTip {
    QString iconName;
    QList<SniPixmap> pixmaps;
    QString title, text;
};
Q_DECLARE_METATYPE(SniPixmap)
Q_DECLARE_METATYPE(SniToolTip)
QDBusArgument &operator<<(QDBusArgument &a, const SniPixmap &p);
const QDBusArgument &operator>>(const QDBusArgument &a, SniPixmap &p);
QDBusArgument &operator<<(QDBusArgument &a, const SniToolTip &t);
const QDBusArgument &operator>>(const QDBusArgument &a, SniToolTip &t);

class Sni : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")
    Q_PROPERTY(QString Category READ category CONSTANT)
    Q_PROPERTY(QString Id READ id CONSTANT)
    Q_PROPERTY(QString Title READ title CONSTANT)
    Q_PROPERTY(QString Status READ status CONSTANT)
    Q_PROPERTY(int WindowId READ windowId CONSTANT)
    Q_PROPERTY(QString IconName READ iconName)
    Q_PROPERTY(QString IconThemePath READ iconThemePath CONSTANT)
    Q_PROPERTY(bool ItemIsMenu READ itemIsMenu CONSTANT)
    Q_PROPERTY(QDBusObjectPath Menu READ menu CONSTANT)
    Q_PROPERTY(SniToolTip ToolTip READ toolTip)

public:
    Sni(const QString &id, const QString &title, QObject *parent = nullptr);
    void setIcon(const QString &name);
    void setToolTip(const QString &title, const QString &text);

    QString category() const { return QStringLiteral("Hardware"); }
    QString id() const { return m_id; }
    QString title() const { return m_title; }
    QString status() const { return QStringLiteral("Active"); }
    int windowId() const { return 0; }
    QString iconName() const { return m_icon; }
    QString iconThemePath() const { return {}; }
    bool itemIsMenu() const { return false; }
    QDBusObjectPath menu() const { return QDBusObjectPath(QStringLiteral("/NO_DBUSMENU")); }
    SniToolTip toolTip() const { return {m_icon, {}, m_tipTitle, m_tipText}; }

public Q_SLOTS: // exported D-Bus methods
    void Activate(int x, int y);
    void SecondaryActivate(int x, int y);
    void ContextMenu(int x, int y);
    void Scroll(int delta, const QString &orientation);

Q_SIGNALS:
    Q_SCRIPTABLE void NewIcon(); // exported D-Bus signals
    Q_SCRIPTABLE void NewToolTip();
    void activated(); // local only
    void middleClicked();
    void scrolled(int steps); // +1 = up

private:
    void registerWithWatcher();
    QDBusConnection m_bus;
    QString m_id, m_title, m_icon, m_tipTitle, m_tipText, m_service;
    int m_scrollAccum = 0;
};
