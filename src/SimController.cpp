// ============================================================
// simulator 控制器（装配与节拍）：构造、参数、启停、MQ 槽、
// 服务器心跳、自动巡航调度，以及把 GUI/命令行的下发调用转给 SimDownlink。
// 上行解析在 SimUplink，下行打包在 SimDownlink，对外 API 保持不变。
// ============================================================

#include "SimController.h"
#include "SimDownlink.h"
#include "ServerLink/MqLink.h"

#include <QDataStream>
#include <QDebug>
#include <QTimer>
#include <QCoreApplication>

SimController::SimController()
    : _mq(nullptr)
    , _hbTimer(nullptr)
    , _down(nullptr)
    , _uplink(nullptr)
    , _port(5672)
    , _maxSpeed(0.4)
    , _arriveRadius(0.3)
    , _taskTimeoutMs(60000)
    , _nextWaypointDelayMs(2000)
    , _autoCruise(false)
{
    // 数据对象先建好：它们只持有会话表与任务参数的引用，_mq 在 start() 里才落地
    _down = new SimDownlink(_mq, _sessions, _maxSpeed, _arriveRadius, _taskTimeoutMs);
    _uplink = new SimUplink(_sessions, this);
}

void SimController::setMqParams(const QString &host, quint16 port, const QString &vhost,
                                const QString &user, const QString &password,
                                const QString &exchange)
{
    _host = host; _port = port; _vhost = vhost;
    _user = user; _password = password; _exchange = exchange;
}

void SimController::setTerminalIds(const QList<quint32> &ids)
{
    _terminalIds = ids;
}

void SimController::setWaypoints(const QList<QPair<double, double> > &wps)
{
    _waypoints = wps;
}

void SimController::setTaskParams(double maxSpeed, double arriveRadius,
                                  qint64 taskTimeoutMs, int nextWaypointDelayMs)
{
    _maxSpeed = maxSpeed;
    _arriveRadius = arriveRadius;
    _taskTimeoutMs = taskTimeoutMs;
    _nextWaypointDelayMs = nextWaypointDelayMs;
}

void SimController::setAutoCruise(bool enable)
{
    _autoCruise = enable;
}

void SimController::start()
{
    _mq = new MqLink(this);
    // "#" 接收全部上行消息；本程序下发的 cmd.* 回环消息在 SimUplink 中过滤
    _mq->configure(_host, _port, _vhost, _user, _password, _exchange,
                   QStringLiteral("#"), 5000);
    connect(_mq, &MqLink::connected, this, &SimController::onMqConnected);
    connect(_mq, &MqLink::disconnected, this, &SimController::onMqDisconnected);
    connect(_mq, &MqLink::messageReceived, this, &SimController::onMqMessage);
    connect(_mq, &MqLink::errorText, this, &SimController::onMqError);
    _mq->start();

    _hbTimer = new QTimer(this);
    _hbTimer->setInterval(2000);
    connect(_hbTimer, &QTimer::timeout, this, &SimController::onDownHeartbeat);

    for (int i = 0; i < _terminalIds.size(); ++i)
        _sessions.insert(_terminalIds.at(i), new TermSession());

    qInfo() << "[Sim] 模拟器启动，被控终端:" << _terminalIds
            << "航点数:" << _waypoints.size()
            << (_autoCruise ? "【自动巡航】" : "【手动命令】");

    if (!_autoCruise)
        printHelp();
}

void SimController::stop()
{
    _hbTimer->stop();
    if (_mq)
    {
        _mq->stop();
        _mq->deleteLater();
        _mq = nullptr;
    }
    qDeleteAll(_sessions);
    _sessions.clear();
}

void SimController::onMqConnected()
{
    qInfo() << "[Sim] MQ链路就绪，启动心跳";
    _hbTimer->start();
}

void SimController::onMqDisconnected()
{
    qWarning() << "[Sim] MQ链路断开";
    _hbTimer->stop();
}

void SimController::onMqError(const QString &what)
{
    qWarning() << "[Sim] MQ错误:" << what;
}

void SimController::onMqMessage(const QString &routingKey, const QByteArray &payload)
{
    // 分流、解包、打印都由解析器负责；需要业务动作时它回调本类
    _uplink->onMessage(routingKey, payload);
}

void SimController::onDownHeartbeat()
{
    if (!_mq)
        return;

    DogProtocol::HeartbeatMsg hb;
    hb.status = DogProtocol::TS_READY;

    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << hb;

    for (int i = 0; i < _terminalIds.size(); ++i)
    {
        const quint32 id = _terminalIds.at(i);
        DogProtocol::MessageHeader header(DogProtocol::MSG_HEARTBEAT, id);
        _mq->publish(QStringLiteral("cmd.%1").arg(id),
                     DogProtocol::packMessage(header, body));
    }
}

// ============================================================
// 上行事件：这里只做业务决策，不做报文处理
// ============================================================
void SimController::onTerminalReady(quint32 terminalId)
{
    if (!_autoCruise)
        return;
    sendMoveCmd(terminalId);
}

void SimController::onTaskFinished(quint32 terminalId, int result)
{
    if (!_autoCruise)
        return;

    if (result == DogProtocol::MR_ARRIVED)
    {
        // 到达后延迟下发下一个航点
        QTimer::singleShot(_nextWaypointDelayMs, this, [this, terminalId]() {
            sendMoveCmd(terminalId);
        });
    }
    else if (result == DogProtocol::MR_REJECTED || result == DogProtocol::MR_FAILED
             || result == DogProtocol::MR_TIMEOUT)
    {
        // 失败/超时 5s 后重试当前航点
        QTimer::singleShot(5000, this, [this, terminalId]() {
            sendMoveCmd(terminalId);
        });
    }
}

void SimController::onPanelUpdated(quint32 terminalId)
{
    emit calibPanelUpdated(terminalId);
}

void SimController::sendMoveCmd(quint32 terminalId)
{
    TermSession *s = _sessions.value(terminalId, nullptr);
    if (!s || _waypoints.isEmpty() || s->taskInFlight)
        return;

    // 终端未就绪（未连狗/未取得控制权）则稍后重试
    const DogProtocol::DogState &st = s->lastState;
    if (!st.dogConnected || !st.controlAcquired)
        return;

    const QPair<double, double> wp = _waypoints.at(s->wpIndex % _waypoints.size());
    s->wpIndex = (s->wpIndex + 1) % qMax(1, _waypoints.size());
    _down->waypoint(terminalId, wp.first, wp.second);
}

// ============================================================
// 下发接口：保持原签名给 GUI/命令行用，实现全部转调
// ============================================================
bool SimController::sendAttitude(quint32 terminalId, int action)
{
    return _down->attitude(terminalId, action);
}

bool SimController::sendMove(quint32 terminalId, double x, double y,
                             double maxSpeed, double arriveRadius, qint64 timeoutMs)
{
    return _down->move(terminalId, x, y, maxSpeed, arriveRadius, timeoutMs);
}

bool SimController::sendRotate(quint32 terminalId, double angleRad, bool absolute,
                               double maxTurnRate, double toleranceRad, qint64 timeoutMs)
{
    return _down->rotate(terminalId, angleRad, absolute, maxTurnRate, toleranceRad, timeoutMs);
}

bool SimController::sendMoveRelative(quint32 terminalId, double forward, double left,
                                     double maxSpeed, double arriveRadius)
{
    return _down->moveRelative(terminalId, forward, left, maxSpeed, arriveRadius);
}

bool SimController::sendVelocity(quint32 terminalId, double vx, double vy, double yawRate)
{
    return _down->velocity(terminalId, vx, vy, yawRate);
}

bool SimController::sendStop(quint32 terminalId, bool emergency)
{
    return _down->stop(terminalId, emergency);
}

bool SimController::sendParam(quint32 terminalId, int speedLevel)
{
    return _down->param(terminalId, speedLevel);
}

bool SimController::sendMapCalib(quint32 terminalId, int action,
                                 double targetX, double targetY)
{
    return _down->mapCalib(terminalId, action, targetX, targetY);
}
