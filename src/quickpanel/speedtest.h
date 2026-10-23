#pragma once
// Speed test against Cloudflare's speed endpoints (same backend as speed.cloudflare.com):
// latency = median of small requests, then time-boxed parallel download and upload.
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

class SpeedTest : public QObject {
    Q_OBJECT
public:
    enum Phase { Idle, Ping, Download, Upload };
    explicit SpeedTest(QObject *parent = nullptr);
    bool running() const { return m_phase != Idle; }
    void start();
    void stop();

Q_SIGNALS:
    void ping(double ms, double jitterMs);
    void progress(SpeedTest::Phase phase, double mbps, double fraction); // fraction of the phase's time box
    void phaseDone(SpeedTest::Phase phase, double mbps);
    void finished();
    void failed(const QString &error);

private:
    void nextPing();
    void startTransfer(Phase p);
    void spawn();
    void tick();
    void endTransfer();
    void fail(const QString &error);
    void release();

    QNetworkAccessManager *m_nam = nullptr;
    Phase m_phase = Idle;
    QList<double> m_pings;
    QElapsedTimer m_clock;      // per ping / per phase
    QElapsedTimer m_flowClock;  // starts at the first byte, so connection setup isn't counted
    QTimer m_tick;
    QList<QNetworkReply *> m_replies;
    qint64 m_bytes = 0;
    QByteArray m_payload;
};
