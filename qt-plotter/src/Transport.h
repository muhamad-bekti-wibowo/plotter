// Transport baris-demi-baris: Serial (QSerialPort atau fallback Win32) dan TCP.
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class Transport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool open(QString* err) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    void sendLine(const QString& s) { rawWrite((s + "\n").toUtf8()); }
signals:
    void lineReceived(const QString& line);
    void disconnected();
protected:
    virtual void rawWrite(const QByteArray& b) = 0;
    void feed(const QByteArray& b);
private:
    QByteArray buf_;
};

Transport* makeTcpTransport(const QString& host, quint16 port, QObject* parent);
Transport* makeSerialTransport(const QString& port, int baud, QObject* parent);
QStringList availableSerialPorts();
