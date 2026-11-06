#include "Transport.h"
#include <QSettings>
#include <QTcpSocket>
#include <QTimer>

void Transport::feed(const QByteArray& b) {
    buf_.append(b);
    int i;
    while ((i = buf_.indexOf('\n')) >= 0) {
        QString line = QString::fromUtf8(buf_.left(i)).trimmed();
        buf_.remove(0, i + 1);
        if (!line.isEmpty()) emit lineReceived(line);
    }
    if (buf_.size() > 8192) buf_.clear();
}

// ---------------------------------------------------------------- TCP
class TcpTransport : public Transport {
public:
    TcpTransport(const QString& h, quint16 p, QObject* parent) : Transport(parent), host_(h), port_(p) {
        connect(&sock_, &QTcpSocket::readyRead, this, [this] { feed(sock_.readAll()); });
        connect(&sock_, &QTcpSocket::disconnected, this, &Transport::disconnected);
    }
    bool open(QString* err) override {
        sock_.connectToHost(host_, port_);
        if (!sock_.waitForConnected(4000)) {
            if (err) *err = sock_.errorString();
            return false;
        }
        sock_.setSocketOption(QAbstractSocket::LowDelayOption, 1);
        return true;
    }
    void close() override { sock_.disconnectFromHost(); }
    bool isOpen() const override { return sock_.state() == QAbstractSocket::ConnectedState; }

protected:
    void rawWrite(const QByteArray& b) override { sock_.write(b); }

private:
    QTcpSocket sock_;
    QString host_;
    quint16 port_;
};

Transport* makeTcpTransport(const QString& host, quint16 port, QObject* parent) { return new TcpTransport(host, port, parent); }

// ---------------------------------------------------------------- Serial
#ifdef HAVE_QTSERIALPORT
#include <QSerialPort>
#include <QSerialPortInfo>

class SerialTransport : public Transport {
public:
    SerialTransport(const QString& p, int baud, QObject* parent) : Transport(parent) {
        sp_.setPortName(p);
        sp_.setBaudRate(baud);
        connect(&sp_, &QSerialPort::readyRead, this, [this] { feed(sp_.readAll()); });
        connect(&sp_, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::ResourceError) emit disconnected();
        });
    }
    bool open(QString* err) override {
        if (!sp_.open(QIODevice::ReadWrite)) {
            if (err) *err = sp_.errorString();
            return false;
        }
        sp_.setDataTerminalReady(false);
        sp_.setRequestToSend(false);
        return true;
    }
    void close() override { sp_.close(); }
    bool isOpen() const override { return sp_.isOpen(); }

protected:
    void rawWrite(const QByteArray& b) override { sp_.write(b); }

private:
    QSerialPort sp_;
};

QStringList availableSerialPorts() {
    QStringList l;
    for (const auto& i : QSerialPortInfo::availablePorts()) l << i.portName();
    return l;
}

#elif defined(Q_OS_WIN)
#include <windows.h>

class SerialTransport : public Transport {
public:
    SerialTransport(const QString& p, int baud, QObject* parent) : Transport(parent), port_(p), baud_(baud) {
        connect(&poll_, &QTimer::timeout, this, &SerialTransport::readSome);
    }
    ~SerialTransport() override { close(); }
    bool open(QString* err) override {
        QString name = "\\\\.\\" + port_;
        h_ = CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            if (err) *err = QString("CreateFile gagal (kode %1); port dipakai program lain?").arg(GetLastError());
            return false;
        }
        DCB dcb = {};
        dcb.DCBlength = sizeof(dcb);
        GetCommState(h_, &dcb);
        dcb.BaudRate = DWORD(baud_);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        if (!SetCommState(h_, &dcb)) {
            if (err) *err = "SetCommState gagal";
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
            return false;
        }
        COMMTIMEOUTS to = {};
        to.ReadIntervalTimeout = MAXDWORD;  // ReadFile kembali segera
        to.WriteTotalTimeoutConstant = 1000;
        SetCommTimeouts(h_, &to);
        SetupComm(h_, 8192, 8192);
        PurgeComm(h_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        poll_.start(4);
        return true;
    }
    void close() override {
        poll_.stop();
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }
    bool isOpen() const override { return h_ != INVALID_HANDLE_VALUE; }

protected:
    void rawWrite(const QByteArray& b) override {
        if (h_ == INVALID_HANDLE_VALUE) return;
        DWORD done = 0;
        const char* p = b.constData();
        DWORD left = DWORD(b.size());
        while (left) {
            if (!WriteFile(h_, p, left, &done, nullptr) || !done) return;
            p += done;
            left -= done;
        }
    }

private:
    void readSome() {
        if (h_ == INVALID_HANDLE_VALUE) return;
        char tmp[1024];
        DWORD n = 0;
        if (!ReadFile(h_, tmp, sizeof(tmp), &n, nullptr)) {
            close();
            emit disconnected();
            return;
        }
        if (n) feed(QByteArray(tmp, int(n)));
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
    QString port_;
    int baud_;
    QTimer poll_;
};

QStringList availableSerialPorts() {
    QSettings reg("HKEY_LOCAL_MACHINE\\HARDWARE\\DEVICEMAP\\SERIALCOMM", QSettings::NativeFormat);
    QStringList l;
    for (const QString& k : reg.allKeys()) l << reg.value(k).toString();
    l.sort();
    return l;
}
#else
class SerialTransport : public Transport {
public:
    SerialTransport(const QString&, int, QObject* p) : Transport(p) {}
    bool open(QString* err) override {
        if (err) *err = "Serial tidak didukung (pasang Qt SerialPort)";
        return false;
    }
    void close() override {}
    bool isOpen() const override { return false; }

protected:
    void rawWrite(const QByteArray&) override {}
};
QStringList availableSerialPorts() { return {}; }
#endif

Transport* makeSerialTransport(const QString& port, int baud, QObject* parent) { return new SerialTransport(port, baud, parent); }
