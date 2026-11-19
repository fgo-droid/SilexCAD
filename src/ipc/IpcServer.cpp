#include "IpcServer.hpp"

#include <QLocalServer>
#include <QLocalSocket>
#include <QVariant>

#include "IpcCommandDispatcher.hpp"

namespace silex::ipc {

IpcServer::IpcServer(IpcCommandDispatcher* dispatcher, QObject* parent)
    : QObject(parent), m_dispatcher(dispatcher), m_server(new QLocalServer(this))
{
    m_server->setSocketOptions(QLocalServer::UserAccessOption);  // current user only
    connect(m_server, &QLocalServer::newConnection, this, &IpcServer::onNewConnection);
}

IpcServer::~IpcServer() = default;

QString IpcServer::defaultName() { return QStringLiteral("cad_mcp"); }

QString IpcServer::endpointFor(const QString& name)
{
#ifdef Q_OS_WIN
    return QStringLiteral("\\\\.\\pipe\\") + name;
#else
    return QStringLiteral("/tmp/") + name + QStringLiteral(".sock");
#endif
}

bool IpcServer::listen(const QString& name, QString* error)
{
    const QString ep = endpointFor(name);
#ifndef Q_OS_WIN
    QLocalServer::removeServer(ep);  // stale socket file from a crashed instance
#endif
    if (!m_server->listen(ep)) {
        if (error)
            *error = m_server->errorString();
        return false;
    }
    return true;
}

QString IpcServer::endpoint() const { return m_server->fullServerName(); }

void IpcServer::onNewConnection()
{
    while (QLocalSocket* s = m_server->nextPendingConnection()) {
        connect(s, &QLocalSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
    }
}

void IpcServer::onReadyRead(QLocalSocket* socket)
{
    QByteArray buffer = socket->property("silex_buffer").toByteArray();
    buffer += socket->readAll();
    int nl;
    while ((nl = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(nl).trimmed();
        buffer.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        const std::string reply = m_dispatcher->handleLine(std::string(line.constData(), size_t(line.size())));
        socket->write(reply.data(), qint64(reply.size()));
        socket->write("\n", 1);
        socket->flush();
        if (m_dispatcher->quitRequested())
            break;
    }
    socket->setProperty("silex_buffer", buffer);
}

}  // namespace silex::ipc
