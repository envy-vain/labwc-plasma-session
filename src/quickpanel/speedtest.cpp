#include "speedtest.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <algorithm>
#include <malloc.h>

namespace {
constexpr int kPings = 9;            // first one is a warm-up (DNS + TLS), not counted
constexpr int kPhaseMs = 8000;       // time box per direction
constexpr int kDownStreams = 4;
constexpr int kUpStreams = 3;
constexpr qint64 kDownChunk = 25'000'000;
constexpr int kUpChunk = 4'000'000;

QNetworkRequest request(const QString &url)
{
    QNetworkRequest r{QUrl(url)};
    r.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("wifi-panel/1.0"));
    r.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    r.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    r.setTransferTimeout(15000);
    return r;
}
}

SpeedTest::SpeedTest(QObject *parent)
    : QObject(parent)
{
    m_tick.setInterval(250);
    connect(&m_tick, &QTimer::timeout, this, &SpeedTest::tick);
}

void SpeedTest::start()
{
    if (running())
        return;
    m_phase = Ping;
    m_pings.clear();
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this); // created per run, see release()
    nextPing();
}

void SpeedTest::stop()
{
    if (!running())
        return;
    m_phase = Idle;
    m_tick.stop();
    const auto replies = m_replies;
    m_replies.clear();
    for (auto *r : replies)
        r->abort();
    release();
    Q_EMIT finished();
}

void SpeedTest::fail(const QString &error)
{
    stop();
    Q_EMIT failed(error);
}

void SpeedTest::nextPing()
{
    m_clock.start();
    auto *r = m_nam->get(request(QStringLiteral("https://speed.cloudflare.com/__down?bytes=0")));
    m_replies = {r};
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        m_replies.removeOne(r);
        if (m_phase != Ping)
            return;
        if (r->error() != QNetworkReply::NoError)
            return fail(r->errorString());
        m_pings << m_clock.nsecsElapsed() / 1e6;
        if (m_pings.size() < kPings)
            return nextPing();

        QList<double> p = m_pings.mid(1);
        double jitter = 0;
        for (qsizetype i = 1; i < p.size(); ++i)
            jitter += std::abs(p[i] - p[i - 1]);
        jitter /= p.size() - 1;
        std::sort(p.begin(), p.end());
        Q_EMIT ping(p[p.size() / 2], jitter);
        startTransfer(Download);
    });
}

void SpeedTest::startTransfer(Phase p)
{
    m_phase = p;
    m_bytes = 0;
    m_flowClock.invalidate();
    if (p == Upload && m_payload.isEmpty())
        m_payload = QByteArray(kUpChunk, '0');
    m_clock.start();
    const int streams = p == Download ? kDownStreams : kUpStreams;
    for (int i = 0; i < streams; ++i)
        spawn();
    m_tick.start();
}

void SpeedTest::spawn()
{
    QNetworkReply *r;
    if (m_phase == Download) {
        r = m_nam->get(request(QStringLiteral("https://speed.cloudflare.com/__down?bytes=%1").arg(kDownChunk)));
        r->setReadBufferSize(256 * 1024);
        connect(r, &QIODevice::readyRead, this, [this, r] {
            if (!m_flowClock.isValid())
                m_flowClock.start();
            m_bytes += r->skip(r->bytesAvailable()); // count and discard, nothing is kept in memory
        });
    } else {
        auto req = request(QStringLiteral("https://speed.cloudflare.com/__up"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        r = m_nam->post(req, m_payload);
        auto *last = new qint64(0);
        connect(r, &QNetworkReply::uploadProgress, this, [this, last](qint64 sent, qint64) {
            if (!m_flowClock.isValid())
                m_flowClock.start();
            m_bytes += sent - *last;
            *last = sent;
        });
        connect(r, &QObject::destroyed, this, [last] { delete last; });
    }
    m_replies << r;
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (!m_replies.removeOne(r))
            return; // aborted by us
        if (r->error() != QNetworkReply::NoError && m_bytes == 0)
            return fail(r->errorString());
        if (m_clock.elapsed() < kPhaseMs)
            spawn(); // keep the pipe full until the time box ends
    });
}

void SpeedTest::tick()
{
    const double secs = m_flowClock.isValid() ? m_flowClock.nsecsElapsed() / 1e9 : 0;
    const double mbps = secs > 0.2 ? m_bytes * 8 / secs / 1e6 : 0;
    Q_EMIT progress(m_phase, mbps, std::min(1.0, m_clock.elapsed() / double(kPhaseMs)));
    if (m_clock.elapsed() >= kPhaseMs)
        endTransfer();
}

void SpeedTest::endTransfer()
{
    m_tick.stop();
    const double secs = m_flowClock.isValid() ? m_flowClock.nsecsElapsed() / 1e9 : 0;
    const double mbps = secs > 0 ? m_bytes * 8 / secs / 1e6 : 0;
    const Phase done = m_phase;
    const auto replies = m_replies;
    m_replies.clear();
    for (auto *r : replies)
        r->abort();
    Q_EMIT phaseDone(done, mbps);
    if (done == Download) {
        startTransfer(Upload);
    } else {
        m_phase = Idle;
        release();
        Q_EMIT finished();
    }
}

// Drop the network stack (TLS sessions, buffers) and the upload payload between runs and give the
// memory back to the OS, so the idle tray app stays small.
void SpeedTest::release()
{
    m_payload = QByteArray();
    if (m_nam) {
        m_nam->deleteLater();
        m_nam = nullptr;
    }
    QTimer::singleShot(500, [] { malloc_trim(0); });
}
