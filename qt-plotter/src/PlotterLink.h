// Protokol teks plotter: antrean balasan berurutan, job stop-and-wait dengan retry saat "full"/"busy".
#pragma once
#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QTimer>

class Transport;

class PlotterLink : public QObject {
    Q_OBJECT
public:
    explicit PlotterLink(QObject* parent = nullptr);
    void attach(Transport* t);   // mengambil kepemilikan
    void detach();
    bool connected() const;

    void send(const QString& cmd);                          // balasan lewat sinyal reply()
    void stop();                                            // STOP segera + batalkan job
    void startJob(const QStringList& lines, bool waitIdle); // waitIdle: selesai setelah plotter diam
    void abortJob();
    bool jobActive() const { return jobRunning_; }

signals:
    void log(const QString& line);
    void reply(const QString& cmd, const QStringList& data, const QString& status);
    void asyncMessage(const QString& msg);
    void connectedChanged(bool on);
    void status(double x, double y, bool pen, bool busy, int queued, bool job);
    void limits(bool xMin, bool yMin, bool xMax, bool yMax);
    void jobProgress(int done, int total);
    void jobFinished(bool ok, const QString& msg);

private:
    struct InFlight {
        QString cmd;
        QStringList data;
        bool job = false, poll = false, tail = false;
    };
    void onLine(const QString& line);
    void write(const QString& cmd, bool job, bool poll, bool tail);
    void handleReply(const InFlight& f, const QString& status);
    void jobNext();
    void finishJob(bool ok, const QString& msg);
    void parseStatus(const QStringList& data, bool* idle);

    Transport* tr_ = nullptr;
    QQueue<InFlight> inflight_;
    QTimer pollTimer_;
    QStringList jobLines_;
    int jobIdx_ = 0;
    bool jobRunning_ = false, jobWait_ = false, jobTail_ = false;
};
