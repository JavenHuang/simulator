// ============================================================
// 下行报文（服务器 → 终端）—— 实现
//
// 打包与发送集中在这里，四个 send* 之外的差异只有报文类型与日志措辞；
// 因此抽出 publish() 与 sendMoveInternal() 两个公共出口，避免七处重复
// “建 header → pack → publish”。
// ============================================================
#include "SimDownlink.h"

#include <cmath>
#include <QDataStream>
#include <QDebug>

#include "Protocol/DogProtocol.h"
#include "ServerLink/MqLink.h"

namespace
{
const double kRadToDeg = 180.0 / 3.14159265358979323846;
const double kDegToRad = 3.14159265358979323846 / 180.0;
}  // namespace

SimDownlink::SimDownlink(MqLink *&mq, QMap<quint32, TermSession *> &sessions,
                         const double &maxSpeed, const double &arriveRadius,
                         const qint64 &taskTimeoutMs)
    : _mq(mq), _sessions(sessions), _maxSpeed(maxSpeed),
      _arriveRadius(arriveRadius), _taskTimeoutMs(taskTimeoutMs)
{
}

bool SimDownlink::publish(quint16 msgType, quint32 terminalId, const QByteArray &body)
{
    // 只有 _mq 指针不够：MqLink::publish 在链路未就绪时是"静默丢弃"，
    // 这里若照样返回 true，调用方会把 taskInFlight 置 1 并等一个永远不来的
    // feedback，自动巡航就此卡死到人工介入。必须先判连接状态。
    if (!_mq || !_mq->isConnected())
    {
        qWarning() << "[Sim] MQ 链路未就绪，报文丢弃 type=" << msgType;
        return false;
    }
    DogProtocol::MessageHeader header(msgType, terminalId);
    _mq->publish(QStringLiteral("cmd.%1").arg(terminalId),
                 DogProtocol::packMessage(header, body));
    return true;
}

bool SimDownlink::sendMoveInternal(quint32 terminalId, double x, double y, double maxSpeed,
                                   double arriveRadius, qint64 timeoutMs, const QString &tag)
{
    TermSession *s = _sessions.value(terminalId, nullptr);
    if (!s)
    {
        qWarning() << "[Sim] 终端" << terminalId << "不存在";
        return false;
    }

    DogProtocol::MoveCmd cmd;
    cmd.taskId = _nextTaskId++;
    cmd.x = x;
    cmd.y = y;
    cmd.maxSpeed = maxSpeed > 0 ? maxSpeed : _maxSpeed;
    cmd.arriveRadius = arriveRadius > 0 ? arriveRadius : _arriveRadius;
    cmd.timeoutMs = timeoutMs > 0 ? timeoutMs : _taskTimeoutMs;

    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << cmd;
    if (!publish(DogProtocol::MSG_MOVE_CMD, terminalId, body))
        return false;

    s->taskInFlight = true;
    s->currentTaskId = cmd.taskId;
    qInfo() << "[Sim] 下发" << tag << "→ 终端" << terminalId << "task" << cmd.taskId
            << (s->lastState.calibrated ? "map(" : "local(") << x << "," << y << ") speed="
            << cmd.maxSpeed;
    return true;
}

// ============================================================
// 手动命令接口
// ============================================================
bool SimDownlink::attitude(quint32 terminalId, int action)
{
    if (!_sessions.contains(terminalId))
    {
        qWarning() << "[Sim] 终端" << terminalId << "不存在";
        return false;
    }

    DogProtocol::AttitudeCmd cmd;
    cmd.action = static_cast<DogProtocol::AttitudeAction>(action);

    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << cmd;
    if (!publish(DogProtocol::MSG_ATTITUDE_CMD, terminalId, body))
        return false;

    qInfo() << "[Sim] 下发姿态 → 终端" << terminalId << "action=" << action;
    return true;
}

bool SimDownlink::move(quint32 terminalId, double x, double y,
                       double maxSpeed, double arriveRadius, qint64 timeoutMs)
{
    return sendMoveInternal(terminalId, x, y, maxSpeed, arriveRadius, timeoutMs,
                            QStringLiteral("机动"));
}

bool SimDownlink::waypoint(quint32 terminalId, double x, double y)
{
    return sendMoveInternal(terminalId, x, y, 0, 0, 0, QStringLiteral("航点"));
}

bool SimDownlink::rotate(quint32 terminalId, double angleRad, bool absolute,
                         double maxTurnRate, double toleranceRad, qint64 timeoutMs)
{
    TermSession *s = _sessions.value(terminalId, nullptr);
    if (!s)
    {
        qWarning() << "[Sim] 终端" << terminalId << "不存在";
        return false;
    }

    DogProtocol::RotateCmd cmd;
    cmd.taskId = _nextTaskId++;
    cmd.angleRad = angleRad;
    cmd.maxTurnRate = maxTurnRate;
    cmd.toleranceRad = toleranceRad;
    cmd.timeoutMs = timeoutMs > 0 ? timeoutMs : _taskTimeoutMs;

    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << cmd;
    const quint16 msgType = absolute ? DogProtocol::MSG_ROTATE_ABS_CMD
                                     : DogProtocol::MSG_ROTATE_REL_CMD;
    if (!publish(msgType, terminalId, body))
        return false;

    s->taskInFlight = true;
    s->currentTaskId = cmd.taskId;
    qInfo() << "[Sim] 下发旋转 → 终端" << terminalId << "task" << cmd.taskId
            << (absolute ? "绝对地图角=" : "相对增量=")
            << angleRad * kRadToDeg << "° maxRate="
            << cmd.maxTurnRate * kRadToDeg << "°/s tolerance="
            << cmd.toleranceRad * kRadToDeg << "°";
    return true;
}

bool SimDownlink::moveRelative(quint32 terminalId, double forward, double left,
                               double maxSpeed, double arriveRadius)
{
    TermSession *s = _sessions.value(terminalId, nullptr);
    if (!s || !s->hasState)
    {
        qWarning() << "[Sim] 终端" << terminalId << "尚未上报位姿，无法相对移动";
        return false;
    }

    // 按终端上报的坐标系计算目标点（calibrated=true 时为地图坐标）
    //   地图: 角 0=正东、顺时针为正，前进=(cos a, sin a)，左=(sin a, -cos a)
    //   局部: yaw 逆时针为正，      前进=(cos yaw, sin yaw)，左=(-sin yaw, cos yaw)
    const double yaw = s->lastState.yaw;
    const double c = std::cos(yaw);
    const double sn = std::sin(yaw);
    double tx;
    double ty;
    if (s->lastState.calibrated)
    {
        tx = s->lastState.x + forward * c + left * sn;
        ty = s->lastState.y + forward * sn - left * c;
    }
    else
    {
        tx = s->lastState.x + forward * c - left * sn;
        ty = s->lastState.y + forward * sn + left * c;
    }

    qInfo().noquote() << QStringLiteral("[Sim] 相对移动 → 终端%1 当前%2(%3,%4) yaw=%5° 前%6 左%7 → 目标(%8,%9)")
                         .arg(terminalId)
                         .arg(s->lastState.calibrated ? QStringLiteral("地图")
                                                      : QStringLiteral("局部"))
                         .arg(s->lastState.x, 0, 'f', 2)
                         .arg(s->lastState.y, 0, 'f', 2)
                         .arg(yaw * kRadToDeg, 0, 'f', 1)
                         .arg(forward, 0, 'f', 2)
                         .arg(left, 0, 'f', 2)
                         .arg(tx, 0, 'f', 2)
                         .arg(ty, 0, 'f', 2);

    return move(terminalId, tx, ty, maxSpeed, arriveRadius);
}

bool SimDownlink::velocity(quint32 terminalId, double vx, double vy, double yawRate)
{
    if (!_sessions.contains(terminalId))
    {
        qWarning() << "[Sim] 终端" << terminalId << "不存在";
        return false;
    }

    DogProtocol::VelocityCmd cmd;
    cmd.vx = vx;
    cmd.vy = vy;
    cmd.yawRate = yawRate;

    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << cmd;
    if (!publish(DogProtocol::MSG_VELOCITY_CMD, terminalId, body))
        return false;

    // W/S 按住时 10Hz 连发相同速度，日志只在数值变化时打印一次
    const QString velKey = QStringLiteral("%1,%2,%3")
                               .arg(vx, 0, 'f', 3)
                               .arg(vy, 0, 'f', 3)
                               .arg(yawRate, 0, 'f', 3);
    if (_lastVelKey.value(terminalId) != velKey)
    {
        _lastVelKey[terminalId] = velKey;
        qInfo() << "[Sim] 下发速度 → 终端" << terminalId
                << "vx=" << vx << "vy=" << vy << "yawRate=" << yawRate;
    }
    return true;
}

bool SimDownlink::stop(quint32 terminalId, bool emergency)
{
    if (!_sessions.contains(terminalId))
    {
        qWarning() << "[Sim] 终端" << terminalId << "不存在";
        return false;
    }

    DogProtocol::StopCmd cmd;
    cmd.emergency = emergency ? 1 : 0;

    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << cmd;
    if (!publish(DogProtocol::MSG_STOP_CMD, terminalId, body))
        return false;

    qInfo() << "[Sim] 下发停止 → 终端" << terminalId << (emergency ? "【急停】" : "【正常停止】");
    return true;
}

bool SimDownlink::mapCalib(quint32 terminalId, int action, double targetX, double targetY)
{
    if (!_sessions.contains(terminalId))
    {
        qWarning() << "[Sim] 终端" << terminalId << "不存在";
        return false;
    }

    DogProtocol::MapCalibCmd cmd;
    cmd.action = action;
    cmd.taskId = _nextTaskId++;
    cmd.targetX = targetX;
    cmd.targetY = targetY;

    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << cmd;
    if (!publish(DogProtocol::MSG_MAP_CALIB_CMD, terminalId, body))
        return false;

    QString actionText;
    switch (action)
    {
    case DogProtocol::MC_RESET:    actionText = QStringLiteral("重置开机参考"); break;
    case DogProtocol::MC_END_LINE:
        actionText = QStringLiteral("直线标定结束 终点地图(%1, %2)")
                         .arg(targetX, 0, 'f', 2)
                         .arg(targetY, 0, 'f', 2);
        break;
    case DogProtocol::MC_DISABLE:  actionText = QStringLiteral("关闭地图坐标"); break;
    default:                       actionText = QStringLiteral("未知动作 %1").arg(action); break;
    }
    qInfo().noquote() << QStringLiteral("[Sim] 下发地图标定 → 终端%1 task%2 %3")
                             .arg(terminalId)
                             .arg(cmd.taskId)
                             .arg(actionText);
    return true;
}

bool SimDownlink::param(quint32 terminalId, int speedLevel)
{
    if (!_sessions.contains(terminalId))
    {
        qWarning() << "[Sim] 终端" << terminalId << "不存在";
        return false;
    }

    DogProtocol::ParamCmd cmd;
    cmd.speedLevel = speedLevel;

    QByteArray body;
    QDataStream out(&body, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << cmd;
    if (!publish(DogProtocol::MSG_PARAM_CMD, terminalId, body))
        return false;

    qInfo() << "[Sim] 下发参数 → 终端" << terminalId << "speedLevel=" << speedLevel;
    return true;
}
