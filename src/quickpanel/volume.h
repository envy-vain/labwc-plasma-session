#pragma once
// Volume tray icon + Plasma-style popup on libpulse (PipeWire's pulse server): output/input devices
// (click to make default), per-app volumes, mute. Runs on Qt's GLib event loop through
// pa_glib_mainloop and keeps state current from pulse subscribe events, so nothing polls.
// Tray: click = popup, middle click = mute, scroll = ±5 %.
#include "popup.h"

#include <QHash>
#include <QMap>
#include <pulse/volume.h>

struct pa_context;
struct pa_glib_mainloop;
struct pa_sink_info;
struct pa_source_info;
struct pa_sink_input_info;
struct pa_server_info;
class QLabel;
class QTimer;
class QVBoxLayout;
class Sni;
class VolRow;

class VolumePanel : public Popup {
    Q_OBJECT
public:
    VolumePanel();
    ~VolumePanel() override;

protected:
    void aboutToOpen() override;

private:
    enum Kind { Sink, Source, Stream };
    struct Item {
        Kind kind;
        uint32_t index;
        QString name, label, icon;
        pa_cvolume vol;
        bool mute;
        uint32_t sink = 0;
    };

    void connectPulse();
    void schedule();
    void refresh();
    void updateTray();
    void setVolume(const Item &it, double fraction);
    void setMute(const Item &it, bool mute);
    void setDefault(const Item &it);
    const Item *defaultSink() const;
    void syncRows(QVBoxLayout *lay, QLabel *header, const QList<const Item *> &items);

    static void onState(pa_context *c, void *self);
    static void onEvent(pa_context *c, int type, uint32_t idx, void *self);
    static void onSink(pa_context *c, const pa_sink_info *i, int eol, void *self);
    static void onSource(pa_context *c, const pa_source_info *i, int eol, void *self);
    static void onStream(pa_context *c, const pa_sink_input_info *i, int eol, void *self);
    static void onServer(pa_context *c, const pa_server_info *i, void *self);

    pa_glib_mainloop *loop = nullptr;
    pa_context *ctx = nullptr;
    QMap<uint32_t, Item> sinks, sources, streams;
    QString defSink, defSource;

    Sni *tray;
    QString trayIcon;
    QVBoxLayout *outLay, *inLay, *appLay;
    QLabel *outH, *inH, *appH, *note;
    QHash<QString, VolRow *> rows; // "k:index"
    QTimer *refreshTimer;
};
