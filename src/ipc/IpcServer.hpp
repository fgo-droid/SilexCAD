#pragma once
// Local IPC endpoint for external drivers (MCP bridge, scripts, tests).
//
// Transport: QLocalServer — Windows named pipe \\.\pipe\<name>, Unix socket /tmp/<name>.sock.
// Framing:   one JSON object per line ('\n').
//   request  {"id": 1, "method": "get_model_tree", "params": {...}}
//   response {"id": 1, "result": {...}}  |  {"id": 1, "error": {"code": -32000, "message": "..."}}
// Every request is executed on the Qt main thread (the thread that owns the document).

#include <QObject>
#include <QString>

class QLocalServer;
class QLocalSocket;

namespace silex::ipc {

class IpcCommandDispatcher;

class IpcServer : public QObject
{
    Q_OBJECT
public:
    IpcServer(IpcCommandDispatcher* dispatcher, QObject* parent = nullptr);
    ~IpcServer() override;

    /// Default endpoint name: "cad_mcp".
    static QString defaultName();
    /// Full platform path for a name (\\.\pipe\cad_mcp or /tmp/cad_mcp.sock).
    static QString endpointFor(const QString& name);

    bool listen(const QString& name, QString* error = nullptr);
    QString endpoint() const;
    int clientCount() const { return m_clients; }

signals:
    /// Ergonomics lot: drives the MCP indicator of the status bar.
    void clientCountChanged(int count);

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket* socket);

    IpcCommandDispatcher* m_dispatcher;
    QLocalServer* m_server;
    int m_clients = 0;
};

}  // namespace silex::ipc
