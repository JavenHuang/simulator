#ifndef SIM_DOWNLINK_H
#define SIM_DOWNLINK_H

#include <QMap>
#include <QString>

#include "SimSession.h"

class MqLink;

// ============================================================
// 下行报文（服务器 → 终端）
//
// 只做一件事：把一次操作翻译成 DogProtocol 报文并发到 cmd.<id>。
// 不判断业务时序、不推进航点、不调度重试——那些属于巡航与控制器。
// 四个统一约束：任务号由本类自增分配、任务一发出就占用会话任务槽、
// 参数留 0 表示"用配置默认值"、发送失败只记日志不改会话状态。
// ============================================================
class SimDownlink
{
public:
    SimDownlink(MqLink *&mq, QMap<quint32, TermSession *> &sessions,
                const double &maxSpeed, const double &arriveRadius,
                const qint64 &taskTimeoutMs);

    bool attitude(quint32 terminalId, int action);
    bool move(quint32 terminalId, double x, double y, double maxSpeed = 0,
              double arriveRadius = 0, qint64 timeoutMs = 0);
    // absolute=false：相对当前朝向旋转；absolute=true：转到绝对地图 yaw
    bool rotate(quint32 terminalId, double angleRad, bool absolute,
                double maxTurnRate = 0, double toleranceRad = 0, qint64 timeoutMs = 0);
    // 按终端最近上报的坐标系计算机体前后/左右目标点，forward>0 前进，left>0 左移
    bool moveRelative(quint32 terminalId, double forward, double left,
                      double maxSpeed = 0, double arriveRadius = 0);
    bool velocity(quint32 terminalId, double vx, double vy, double yawRate);
    bool stop(quint32 terminalId, bool emergency = false);
    bool param(quint32 terminalId, int speedLevel);
    bool mapCalib(quint32 terminalId, int action,
                  double targetX = 0.0, double targetY = 0.0);
    // 自动巡航专用：按配置默认参数下发一个航点，日志与手动机动区分
    bool waypoint(quint32 terminalId, double x, double y);

private:
    // 机动类报文的公共实现：校验会话、分配任务号、占用任务槽、按坐标系打印
    bool sendMoveInternal(quint32 terminalId, double x, double y, double maxSpeed,
                          double arriveRadius, qint64 timeoutMs, const QString &tag);
    bool publish(quint16 msgType, quint32 terminalId, const QByteArray &body);

    MqLink *&_mq;
    QMap<quint32, TermSession *> &_sessions;
    const double &_maxSpeed;
    const double &_arriveRadius;
    const qint64 &_taskTimeoutMs;

    quint32 _nextTaskId = 1;
    // 每个终端最近一次下发的速度，用于 W/S 连发时日志去重（仍会正常下发）
    QMap<quint32, QString> _lastVelKey;
};

#endif  // SIM_DOWNLINK_H
