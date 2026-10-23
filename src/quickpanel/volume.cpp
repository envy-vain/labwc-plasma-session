#include "volume.h"
#include "sni.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QProcess>
#include <QRadioButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <pulse/context.h>
#include <pulse/glib-mainloop.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>

namespace {
constexpr int kMaxPercent = 150; // same ceiling as the volume keys (fnkey) and the old LXQt plugin
constexpr int kScrollStep = 5;

int percentOf(const pa_cvolume &v)
{
    return qRound(pa_cvolume_max(&v) * 100.0 / PA_VOLUME_NORM);
}

QString volumeIcon(int pct, bool mute, bool input)
{
    const QString base = input ? QStringLiteral("microphone-sensitivity-") : QStringLiteral("audio-volume-");
    const QString level = mute || pct == 0 ? QStringLiteral("muted") : pct < 34 ? QStringLiteral("low") : pct < 67 ? QStringLiteral("medium") : QStringLiteral("high");
    return base + level + QStringLiteral("-symbolic");
}

QString prop(pa_proplist *p, const char *key)
{
    const char *v = p ? pa_proplist_gets(p, key) : nullptr;
    return v ? QString::fromUtf8(v) : QString();
}
}

class VolRow : public QFrame {
public:
    explicit VolRow(bool device)
    {
        setObjectName(QStringLiteral("vrow"));
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(8, 6, 8, 6);
        v->setSpacing(2);
        if (device) {
            pick = new QRadioButton;
            v->addWidget(pick);
        } else {
            auto *h = new QHBoxLayout;
            h->setSpacing(8);
            appIcon = new QLabel;
            appIcon->setFixedSize(20, 20);
            name = new QLabel;
            h->addWidget(appIcon);
            h->addWidget(name, 1);
            v->addLayout(h);
        }
        auto *h = new QHBoxLayout;
        h->setSpacing(8);
        mute = new QToolButton;
        mute->setAutoRaise(true);
        mute->setIconSize(QSize(18, 18));
        slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, kMaxPercent);
        slider->setPageStep(kScrollStep);
        slider->setSingleStep(1);
        pct = new QLabel;
        pct->setMinimumWidth(QFontMetrics(pct->font()).horizontalAdvance(QStringLiteral("150%")) + 2);
        pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(mute);
        h->addWidget(slider, 1);
        h->addWidget(pct);
        v->addLayout(h);
    }
    QRadioButton *pick = nullptr;
    QLabel *appIcon = nullptr, *name = nullptr, *pct;
    QToolButton *mute;
    QSlider *slider;
    QString iconName, muteIcon;
};

VolumePanel::VolumePanel()
    : Popup(QStringLiteral("Volume"))
{
    resize(380, 10);
    setStyleSheet(QStringLiteral("#vrow{border-radius:8px}#vrow:hover{background:palette(midlight)}"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(6);
    root->setSizeConstraint(QLayout::SetFixedSize);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>Volume</b>"));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.25);
    title->setFont(tf);
    auto *settings = new QToolButton;
    settings->setIcon(QIcon::fromTheme(QStringLiteral("configure-symbolic"), QIcon::fromTheme(QStringLiteral("configure"))));
    settings->setToolTip(QStringLiteral("Mixer (pavucontrol)"));
    settings->setAutoRaise(true);
    head->addWidget(title);
    head->addStretch();
    head->addWidget(settings);
    root->addLayout(head);
    connect(settings, &QToolButton::clicked, this, [this] {
        hide();
        QProcess::startDetached(QStringLiteral("pavucontrol-qt"), {});
    });

    auto section = [&](const QString &text, QLabel *&h, QVBoxLayout *&lay) {
        h = dimLabel();
        h->setText(text);
        root->addSpacing(4);
        root->addWidget(h);
        auto *w = new QWidget;
        w->setMinimumWidth(356);
        lay = new QVBoxLayout(w);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(2);
        root->addWidget(w);
    };
    section(QStringLiteral("Output"), outH, outLay);
    section(QStringLiteral("Input"), inH, inLay);
    section(QStringLiteral("Applications"), appH, appLay);
    note = dimLabel();
    note->setWordWrap(true);
    note->hide();
    root->addWidget(note);

    refreshTimer = new QTimer(this);
    refreshTimer->setSingleShot(true);
    refreshTimer->setInterval(30);
    connect(refreshTimer, &QTimer::timeout, this, &VolumePanel::refresh);

    tray = new Sni(QStringLiteral("quickpanel-volume"), QStringLiteral("Volume"), this);
    connect(tray, &Sni::activated, this, &Popup::toggleVisible);
    connect(tray, &Sni::middleClicked, this, [this] {
        if (const Item *s = defaultSink())
            setMute(*s, !s->mute);
    });
    connect(tray, &Sni::scrolled, this, [this](int steps) {
        if (const Item *s = defaultSink())
            setVolume(*s, qBound(0, percentOf(s->vol) + steps * kScrollStep, kMaxPercent) / 100.0);
    });

    loop = pa_glib_mainloop_new(nullptr); // default GMainContext = Qt's event loop
    connectPulse();
}

VolumePanel::~VolumePanel()
{
    if (ctx) {
        pa_context_disconnect(ctx);
        pa_context_unref(ctx);
    }
    if (loop)
        pa_glib_mainloop_free(loop);
}

void VolumePanel::connectPulse()
{
    if (ctx) {
        pa_context_unref(ctx);
        ctx = nullptr;
    }
    sinks.clear();
    sources.clear();
    streams.clear();
    pa_proplist *pl = pa_proplist_new();
    pa_proplist_sets(pl, PA_PROP_APPLICATION_NAME, "quickpanel");
    pa_proplist_sets(pl, PA_PROP_APPLICATION_ICON_NAME, "audio-volume-high");
    ctx = pa_context_new_with_proplist(pa_glib_mainloop_get_api(loop), "quickpanel", pl);
    pa_proplist_free(pl);
    pa_context_set_state_callback(ctx, &VolumePanel::onState, this);
    pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFAIL, nullptr); // NOFAIL: waits for the server to (re)appear
}

void VolumePanel::onState(pa_context *c, void *p)
{
    auto *self = static_cast<VolumePanel *>(p);
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        pa_context_set_subscribe_callback(c, reinterpret_cast<pa_context_subscribe_cb_t>(&VolumePanel::onEvent), self);
        pa_operation_unref(pa_context_subscribe(c, pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SERVER), nullptr, nullptr));
        pa_operation_unref(pa_context_get_server_info(c, &VolumePanel::onServer, self));
        pa_operation_unref(pa_context_get_sink_info_list(c, &VolumePanel::onSink, self));
        pa_operation_unref(pa_context_get_source_info_list(c, &VolumePanel::onSource, self));
        pa_operation_unref(pa_context_get_sink_input_info_list(c, &VolumePanel::onStream, self));
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        // pipewire-pulse restarted: start over shortly (not from inside its own callback)
        QTimer::singleShot(1000, self, &VolumePanel::connectPulse);
        self->schedule();
        break;
    default:
        break;
    }
}

void VolumePanel::onEvent(pa_context *c, int t, uint32_t idx, void *p)
{
    auto *self = static_cast<VolumePanel *>(p);
    const int facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    const bool removed = (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE;
    switch (facility) {
    case PA_SUBSCRIPTION_EVENT_SINK:
        if (removed)
            self->sinks.remove(idx), self->schedule();
        else
            pa_operation_unref(pa_context_get_sink_info_by_index(c, idx, &VolumePanel::onSink, self));
        break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
        if (removed)
            self->sources.remove(idx), self->schedule();
        else
            pa_operation_unref(pa_context_get_source_info_by_index(c, idx, &VolumePanel::onSource, self));
        break;
    case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
        if (removed)
            self->streams.remove(idx), self->schedule();
        else
            pa_operation_unref(pa_context_get_sink_input_info(c, idx, &VolumePanel::onStream, self));
        break;
    case PA_SUBSCRIPTION_EVENT_SERVER:
        pa_operation_unref(pa_context_get_server_info(c, &VolumePanel::onServer, self));
        break;
    }
}

void VolumePanel::onSink(pa_context *, const pa_sink_info *i, int eol, void *p)
{
    if (eol || !i)
        return;
    auto *self = static_cast<VolumePanel *>(p);
    QString icon = prop(i->proplist, PA_PROP_DEVICE_ICON_NAME);
    self->sinks.insert(i->index, {Sink, i->index, QString::fromUtf8(i->name), QString::fromUtf8(i->description), icon, i->volume, bool(i->mute)});
    self->schedule();
}

void VolumePanel::onSource(pa_context *, const pa_source_info *i, int eol, void *p)
{
    if (eol || !i || i->monitor_of_sink != PA_INVALID_INDEX)
        return; // monitors aren't microphones
    auto *self = static_cast<VolumePanel *>(p);
    self->sources.insert(i->index, {Source, i->index, QString::fromUtf8(i->name), QString::fromUtf8(i->description), QString(), i->volume, bool(i->mute)});
    self->schedule();
}

void VolumePanel::onStream(pa_context *, const pa_sink_input_info *i, int eol, void *p)
{
    if (eol || !i)
        return;
    auto *self = static_cast<VolumePanel *>(p);
    QString app = prop(i->proplist, PA_PROP_APPLICATION_NAME);
    if (app.isEmpty())
        app = QString::fromUtf8(i->name);
    QString icon = prop(i->proplist, PA_PROP_APPLICATION_ICON_NAME);
    if (icon.isEmpty())
        icon = prop(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY).toLower();
    Item it{Stream, i->index, app, app, icon, i->volume, bool(i->mute), i->sink};
    const QString media = prop(i->proplist, PA_PROP_MEDIA_NAME);
    if (!media.isEmpty() && media != app)
        it.label = app + QStringLiteral(" · ") + media;
    self->streams.insert(i->index, it);
    self->schedule();
}

void VolumePanel::onServer(pa_context *, const pa_server_info *i, void *p)
{
    if (!i)
        return;
    auto *self = static_cast<VolumePanel *>(p);
    self->defSink = QString::fromUtf8(i->default_sink_name);
    self->defSource = QString::fromUtf8(i->default_source_name);
    self->schedule();
}

void VolumePanel::schedule()
{
    if (!refreshTimer->isActive())
        refreshTimer->start();
}

const VolumePanel::Item *VolumePanel::defaultSink() const
{
    for (const auto &s : sinks)
        if (s.name == defSink)
            return &s;
    return sinks.isEmpty() ? nullptr : &sinks.first();
}

void VolumePanel::aboutToOpen()
{
    note->hide();
    refresh();
}

void VolumePanel::updateTray()
{
    const Item *s = defaultSink();
    if (!s) {
        tray->setIcon(QStringLiteral("audio-volume-muted-symbolic"));
        tray->setToolTip(QStringLiteral("Volume"), QStringLiteral("No output device"));
        return;
    }
    const int pct = percentOf(s->vol);
    tray->setIcon(volumeIcon(pct, s->mute, false));
    tray->setToolTip(QStringLiteral("Volume %1%%2").arg(pct).arg(s->mute ? QStringLiteral(" (muted)") : QString()), s->label);
}

void VolumePanel::refresh()
{
    updateTray();
    if (!isVisible())
        return;
    QList<const Item *> outs, ins, apps;
    for (const auto &s : sinks)
        outs << &s;
    for (const auto &s : sources)
        ins << &s;
    for (const auto &s : streams)
        apps << &s;
    syncRows(outLay, outH, outs);
    syncRows(inLay, inH, ins);
    syncRows(appLay, appH, apps);
    note->setVisible(ctx && pa_context_get_state(ctx) != PA_CONTEXT_READY);
    note->setText(QStringLiteral("Connecting to the sound server…"));
}

void VolumePanel::syncRows(QVBoxLayout *lay, QLabel *header, const QList<const Item *> &items)
{
    while (lay->count())
        delete lay->takeAt(0); // the layout item only; the row widget is reused
    // Drop rows of this kind that no longer exist
    const Kind kind = lay == outLay ? Sink : lay == inLay ? Source : Stream;
    const QString prefix = QString::number(kind) + QLatin1Char(':');
    QSet<QString> keep;
    for (const Item *it : items)
        keep.insert(prefix + QString::number(it->index));
    for (auto i = rows.begin(); i != rows.end();)
        if (i.key().startsWith(prefix) && !keep.contains(i.key())) {
            delete i.value();
            i = rows.erase(i);
        } else {
            ++i;
        }

    for (const Item *it : items) {
        const QString key = prefix + QString::number(it->index);
        VolRow *r = rows.value(key);
        if (!r) {
            r = new VolRow(kind != Stream);
            rows.insert(key, r);
            const uint32_t idx = it->index;
            auto find = [this, kind, idx]() -> const Item * {
                const auto &m = kind == Sink ? sinks : kind == Source ? sources : streams;
                auto f = m.constFind(idx);
                return f == m.cend() ? nullptr : &*f;
            };
            connect(r->slider, &QSlider::valueChanged, this, [this, find](int v) {
                if (const Item *i = find())
                    setVolume(*i, v / 100.0);
            });
            connect(r->mute, &QToolButton::clicked, this, [this, find] {
                if (const Item *i = find())
                    setMute(*i, !i->mute);
            });
            if (r->pick)
                connect(r->pick, &QRadioButton::clicked, this, [this, find] {
                    if (const Item *i = find())
                        setDefault(*i);
                });
        }
        const int pct = percentOf(it->vol);
        if (!r->slider->isSliderDown()) { // don't fight the user's drag with our own echo
            QSignalBlocker b(r->slider);
            r->slider->setValue(pct);
        }
        r->pct->setText(QStringLiteral("%1%").arg(pct));
        const QString mi = volumeIcon(pct, it->mute, kind == Source);
        if (mi != r->muteIcon) {
            r->muteIcon = mi;
            r->mute->setIcon(QIcon::fromTheme(mi));
        }
        r->mute->setToolTip(it->mute ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
        if (r->pick) {
            r->pick->setText(it->label);
            QSignalBlocker b(r->pick);
            r->pick->setChecked(it->name == (kind == Sink ? defSink : defSource));
        } else {
            r->name->setText(it->label);
            if (it->icon != r->iconName || r->appIcon->pixmap().isNull()) {
                r->iconName = it->icon;
                r->appIcon->setPixmap(QIcon::fromTheme(it->icon, QIcon::fromTheme(QStringLiteral("application-x-executable"))).pixmap(20, 20));
            }
        }
        lay->addWidget(r);
    }
    header->setVisible(!items.isEmpty() || kind == Sink);
    lay->parentWidget()->setVisible(!items.isEmpty());
}

void VolumePanel::setVolume(const Item &it, double fraction)
{
    if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY)
        return;
    pa_cvolume cv = it.vol;
    pa_cvolume_scale(&cv, pa_volume_t(qRound(fraction * PA_VOLUME_NORM))); // keeps the balance
    pa_operation *op = it.kind == Sink ? pa_context_set_sink_volume_by_index(ctx, it.index, &cv, nullptr, nullptr)
                       : it.kind == Source ? pa_context_set_source_volume_by_index(ctx, it.index, &cv, nullptr, nullptr)
                                           : pa_context_set_sink_input_volume(ctx, it.index, &cv, nullptr, nullptr);
    if (op)
        pa_operation_unref(op);
    if (it.mute && fraction > 0)
        setMute(it, false); // moving the slider up unmutes, like Plasma
}

void VolumePanel::setMute(const Item &it, bool m)
{
    if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY)
        return;
    pa_operation *op = it.kind == Sink ? pa_context_set_sink_mute_by_index(ctx, it.index, m, nullptr, nullptr)
                       : it.kind == Source ? pa_context_set_source_mute_by_index(ctx, it.index, m, nullptr, nullptr)
                                           : pa_context_set_sink_input_mute(ctx, it.index, m, nullptr, nullptr);
    if (op)
        pa_operation_unref(op);
}

void VolumePanel::setDefault(const Item &it)
{
    if (!ctx || pa_context_get_state(ctx) != PA_CONTEXT_READY)
        return;
    const QByteArray n = it.name.toUtf8();
    pa_operation *op = it.kind == Sink ? pa_context_set_default_sink(ctx, n.constData(), nullptr, nullptr)
                                       : pa_context_set_default_source(ctx, n.constData(), nullptr, nullptr);
    if (op)
        pa_operation_unref(op);
}
