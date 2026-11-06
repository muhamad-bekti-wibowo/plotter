#include "PlotterLink.h"
#include "Transport.h"
#include <QRegularExpression>

static bool isTerminal(const QString& l) {
    return l == "+" || l == "busy" || l == "full" || l.startsWith("ok") || l.startsWith("error");
}

PlotterLink::PlotterLink(QObject* parent) : QObject(parent) {
    pollTimer_.setInterval(700);
    connect(&pollTimer_, &QTimer::timeout, this, [this] {
        if (tr_ && inflight_.size() < 2 && !jobTail_) write("ST", false, true, false);
    });
}

bool PlotterLink::connected() const { return tr_ && tr_->isOpen(); }

void PlotterLink::attach(Transport* t) {
    detach();
    tr_ = t;
    tr_->setParent(this);
    connect(tr_, &Transport::lineReceived, this, &PlotterLink::onLine);
    connect(tr_, &Transport::disconnected, this, [this] {
        finishJob(false, "koneksi terputus");
        detach();
    });
    pollTimer_.start();
    emit connectedChanged(true);
}

void PlotterLink::detach() {
    if (!tr_) return;
    pollTimer_.stop();
    Transport* t = tr_;
    tr_ = nullptr;
    t->disconnect(this);
    t->close();
    t->deleteLater();
    inflight_.clear();
    if (jobRunning_) finishJob(false, "koneksi ditutup");
    emit connectedChanged(false);
}

void PlotterLink::write(const QString& cmd, bool job, bool poll, bool tail) {
    if (!tr_ || !tr_->isOpen()) return;
    InFlight f;
    f.cmd = cmd;
    f.job = job;
    f.poll = poll;
    f.tail = tail;
    inflight_.enqueue(f);
    tr_->sendLine(cmd);
    if (!job && !poll && !tail) emit log("> " + cmd);
}

void PlotterLink::send(const QString& cmd) { write(cmd, false, false, false); }

void PlotterLink::stop() {
    if (jobRunning_) finishJob(false, "dihentikan pengguna");
    write("STOP", false, false, false);
}

void PlotterLink::onLine(const QString& line) {
    if (line.startsWith('!')) {
        emit asyncMessage(line.mid(1).trimmed());
        emit log(line);
        return;
    }
    if (inflight_.isEmpty()) {
        emit log("< " + line);
        return;
    }
    if (!isTerminal(line)) {
        inflight_.head().data << line;
        if (!inflight_.head().poll) emit log("< " + line);
        return;
    }
    InFlight f = inflight_.dequeue();
    if (!f.poll && !f.job) emit log("< " + line);
    handleReply(f, line);
}

void PlotterLink::parseStatus(const QStringList& data, bool* idle) {
    static const QRegularExpression re(R"(^ST q=(\d+) busy=(\d) x=(-?[\d.]+) y=(-?[\d.]+) A=-?\d+ B=-?\d+ pen=(\d) lim=(\d)(\d)(\d)(\d) job=(\d))");
    for (const QString& d : data) {
        auto m = re.match(d);
        if (!m.hasMatch()) continue;
        int q = m.captured(1).toInt();
        bool busy = m.captured(2) == "1";
        *idle = (q == 0 && !busy);
        emit status(m.captured(3).toDouble(), m.captured(4).toDouble(), m.captured(5) == "1", busy, q, m.captured(10) == "1");
        emit limits(m.captured(6) == "1", m.captured(7) == "1", m.captured(8) == "1", m.captured(9) == "1");
        return;
    }
}

void PlotterLink::handleReply(const InFlight& f, const QString& st) {
    if (f.poll || f.tail) {
        bool idle = false;
        parseStatus(f.data, &idle);
        if (f.tail && jobRunning_) {
            if (idle) finishJob(true, "selesai");
            else QTimer::singleShot(250, this, [this] { if (jobRunning_ && jobTail_) write("ST", false, false, true); });
        }
        return;
    }
    if (f.job) {
        if (!jobRunning_) return;
        if (st == "+" || st.startsWith("ok")) {
            jobIdx_++;
            emit jobProgress(jobIdx_, jobLines_.size());
            jobNext();
        } else if (st == "busy" || st == "full") {
            QTimer::singleShot(40, this, &PlotterLink::jobNext);
        } else {
            finishJob(false, st + "  (" + f.cmd + ")");
        }
        return;
    }
    emit reply(f.cmd, f.data, st);
}

void PlotterLink::startJob(const QStringList& lines, bool waitIdle) {
    if (!connected() || jobRunning_) return;
    jobLines_ = lines;
    jobIdx_ = 0;
    jobWait_ = waitIdle;
    jobTail_ = false;
    jobRunning_ = true;
    emit jobProgress(0, lines.size());
    jobNext();
}

void PlotterLink::jobNext() {
    if (!jobRunning_ || !connected()) return;
    if (jobIdx_ >= jobLines_.size()) {
        if (jobWait_) {
            jobTail_ = true;
            write("ST", false, false, true);
        } else {
            finishJob(true, "selesai");
        }
        return;
    }
    write(jobLines_[jobIdx_], true, false, false);
}

void PlotterLink::abortJob() { finishJob(false, "dibatalkan"); }

void PlotterLink::finishJob(bool ok, const QString& msg) {
    if (!jobRunning_) return;
    jobRunning_ = false;
    jobTail_ = false;
    emit jobFinished(ok, msg);
}
