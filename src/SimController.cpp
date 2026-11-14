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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QDateTime>
#include <cmath>

namespace
{
// la 帧里的数值既可能是 number 也可能是 "1.23" 字符串，且不同服务端键名不同，
// 这里按别名列表逐个试，取到第一个可用值。
double rtkNum(const QJsonObject &o, const QStringList &keys, bool *found)
{
    for (int i = 0; i < keys.size(); ++i)
    {
        const QJsonValue v = o.value(keys.at(i));
        if (v.isDouble())
        {
            if (found) *found = true;
            return v.toDouble();
        }
        if (v.isString())
        {
            bool ok = false;
            const double d = v.toString().trimmed().toDouble(&ok);
            if (ok)
            {
                if (found) *found = true;
                return d;
            }
        }
    }
    if (found) *found = false;
    return 0.0;
}
}  // namespace

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
    // la 差分帧：终端只收不发，SimUplink 不认识它；这里单独截下来做地图诊断图层
    if (routingKey == QLatin1String("la")
        || routingKey.endsWith(QLatin1String(".la"))
        || routingKey.endsWith(QLatin1String("/la")))
    {
        handleRtkFrame(payload);
        return;
    }

    // 分流、解包、打印都由解析器负责；需要业务动作时它回调本类
    _uplink->onMessage(routingKey, payload);
}

// ------------------------------------------------------------
// la 差分帧：服务端接收诊断层
// 只做"读出来、存下来、打到控制台"，不参与任何控制决策。
// 键名兼容终端 RtkFrameReader 的同一套别名（x/posX/east、y/posY/south、
// yaw/heading/angle/azimuth/dir），值可以是 number 也可以是数字字符串。
// ------------------------------------------------------------
void SimController::handleRtkFrame(const QByteArray &payload)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
    {
        static int s_parseErr = 0;
        if (s_parseErr++ < 5)
            qWarning().noquote() << "[Rtk] la 帧不是合法 JSON:" << err.errorString()
                                 << "原文前 120B:" << QString::fromUtf8(payload.left(120));
        return;
    }

    QJsonObject o = doc.object();
    // 兼容位姿套在 data 子对象里的服务端
    if (!o.contains(QStringLiteral("x")) && o.value(QStringLiteral("data")).isObject())
        o = o.value(QStringLiteral("data")).toObject();

    bool okX = false;
    bool okY = false;
    bool okYaw = false;
    const double x = rtkNum(o, QStringList() << QStringLiteral("x") << QStringLiteral("posX")
                                             << QStringLiteral("east"), &okX);
    const double y = rtkNum(o, QStringList() << QStringLiteral("y") << QStringLiteral("posY")
                                             << QStringLiteral("south"), &okY);
    const double yawDeg = rtkNum(o, QStringList() << QStringLiteral("yaw")
                                                  << QStringLiteral("heading")
                                                  << QStringLiteral("angle")
                                                  << QStringLiteral("azimuth")
                                                  << QStringLiteral("dir"), &okYaw);
    if (!okX || !okY)
    {
        static int s_fieldErr = 0;
        if (s_fieldErr++ < 5)
            qWarning().noquote() << "[Rtk] la 帧缺少 x/y:" << QString::fromUtf8(payload.left(160));
        return;
    }

    // 差分 ID → 被控终端；取不到或对不上本程序被控终端时，单终端联调直接归给它
    bool okId = false;
    const double idVal = rtkNum(o, QStringList() << QStringLiteral("diffId")
                                                 << QStringLiteral("diff_id")
                                                 << QStringLiteral("rtkId")
                                                 << QStringLiteral("deviceId")
                                                 << QStringLiteral("id"), &okId);
    quint32 tid = okId ? static_cast<quint32>(idVal) : 0u;
    if (!_sessions.contains(tid))
        tid = _terminalIds.isEmpty() ? 0u : _terminalIds.first();
    if (tid == 0)
        return;

    const QPointF pt(x, y);
    double jump = 0.0;
    if (_rtkLast.contains(tid))
    {
        const QPointF d = pt - _rtkLast.value(tid);
        jump = std::sqrt(d.x() * d.x() + d.y() * d.y());
    }
    _rtkLast[tid] = pt;
    _rtkJump[tid] = jump;
    if (okYaw)
        _rtkYawDeg[tid] = yawDeg;
    QVector<QPointF> &tr = _rtkTrail[tid];
    tr.append(pt);
    if (tr.size() > 800)
        tr.remove(0, 200);
    _rtkFrames[tid] = _rtkFrames.value(tid) + 1;

    // 打印策略：前 20 帧逐帧打（现场确认链路通），之后 2s 一行汇总；
    // 单帧跳变 > 0.5m 另打一行（300ms 限流），这是现场要看见的核心现象
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 last = _rtkLastLogMs.value(tid, 0);
    const int n = _rtkFrames.value(tid);
    const bool bigJump = (jump > 0.5);
    bool print = false;
    if (n <= 20)
        print = true;
    else if (bigJump && now - last >= 300)
        print = true;
    else if (now - last >= 2000)
        print = true;

    if (print)
    {
        _rtkLastLogMs[tid] = now;
        qInfo().noquote()
            << QStringLiteral("[Rtk] 差分原始点 #%1 x=%2 y=%3 yaw=%4° 跳变=%5m%6")
                   .arg(n)
                   .arg(x, 0, 'f', 3)
                   .arg(y, 0, 'f', 3)
                   .arg(yawDeg, 0, 'f', 1)
                   .arg(jump, 0, 'f', 3)
                   .arg(bigJump ? QStringLiteral("  <-- 单帧跳变") : QString());
    }
}

void SimController::clearRtkTrail()
{
    _rtkTrail.clear();
    _rtkLast.clear();
    _rtkJump.clear();
    _rtkFrames.clear();
    _rtkLastLogMs.clear();
    qInfo() << "[Rtk] 差分原始点轨迹已清空";
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
