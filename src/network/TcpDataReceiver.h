#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonObject>
#include <QMap>
#include <QByteArray>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include "network/NetworkAccess.h"

class TcpDataReceiver : public QObject
{
    Q_OBJECT

public:
    explicit TcpDataReceiver(QObject *parent = nullptr);
    ~TcpDataReceiver() override;

    bool listen(const QHostAddress &address, quint16 port);
    void stop();
    bool isRunning() const;
    quint16 port() const;
    int clientCount() const;
    // 无数据超过该时长的连接将被断开（默认 5 分钟），用于回收半开连接
    void setIdleTimeoutMs(int ms);
    // 来源 IP 白名单，空列表表示不限制；对新连接生效
    void setAllowList(const QList<NetworkAccess::Subnet> &allowList) { m_allowList = allowList; }

    static constexpr int MaxLineBytes = 64 * 1024;        // 协议最大报文约 20 KB
    static constexpr qint64 MaxPendingWriteBytes = 1024 * 1024; // 客户端只发不收时回包积压上限

    // Synchronous commit hook. Returning true means the message has been
    // durably accepted and may be acknowledged to the sender.
    std::function<bool(const QJsonObject &, QString &)> batchHandler;
    // 异步提交：入库在其他线程完成后调用 done（必须回到本对象所在线程调用）。设置后优先于 batchHandler。
    // 同一连接的报文按到达顺序提交，done 也须按提交顺序调用，回包顺序与报文顺序一致。
    using BatchDone = std::function<void(bool accepted, const QString &error)>;
    std::function<void(const QJsonObject &, BatchDone)> asyncBatchHandler;

signals:
    void runningChanged(bool running);
    void clientCountChanged(int count);
    void logMessage(const QString &message);
    void dataBatchReceived(const QJsonObject &batch);

private slots:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();
    void dropIdleClients();

private:
    struct ClientState {
        QByteArray buffer;
        QElapsedTimer lastActivity;
    };
    void dropClient(QTcpSocket *sock, const QString &reason);
    void handleLine(QTcpSocket *sock, const QByteArray &line);
    void reply(QTcpSocket *sock, const QByteArray &line);
    void replyResult(QTcpSocket *sock, int carrierId, qsizetype wheelCount, bool accepted, const QString &error);
    void rejectConnection(QTcpSocket *sock, const QByteArray &message);

    static constexpr int MaxClients = 32;
    QTcpServer m_server;
    QMap<QTcpSocket*, ClientState> m_clientBuffers;
    QTimer m_idleTimer;
    int m_idleTimeoutMs = 5 * 60 * 1000;
    QList<NetworkAccess::Subnet> m_allowList;
};
