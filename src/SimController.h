#ifndef SIM_CONTROLLER_H
#define SIM_CONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QMap>
#include <QString>
#include <QJsonObject>
#include <QVector>
#include <QPointF>

#include "Protocol/DogProtocol.h"
#include "SimSession.h"
#include "SimUplink.h"

// ============================================================
// 服务器联调模拟器（纯控制台 + 可选 GUI）
//
// 本类只负责"装配与节拍"：读参数、建链路、起心跳、把上行交给 SimUplink、
// 把下行交给 SimDownlink，并在两者之间做业务决策（自动巡航何时发下一个航点）。
// 报文怎么解、怎么打、怎么打包，都不在这里。
// ============================================================
class MqLink;
class SimDownlink;

class SimController : public QObject, public SimUplinkHost
{
    Q_OBJECT
public:
    SimController();

    void setMqParams(const QString &host, quint16 port, const QString &vhost,
                     const QString &user, const QString &password,
                     const QString &exchange);
    void setTerminalIds(const QList<quint32> &ids);
    void setWaypoints(const QList<QPair<double, double> > &wps);
    void setTaskParams(double maxSpeed, double arriveRadius,
                       qint64 taskTimeoutMs, int nextWaypointDelayMs);
    void setAutoCruise(bool enable);

    void start();
    void stop();

    // 标定面板（calib.{id} JSON）最近一份，供 GUI 面板直接读取
    QJsonObject calibPanelOf(quint32 terminalId) const
    {
        return _uplink->panelOf(terminalId);
    }

    // ------------------------------------------------------------
    // 差分原始点（诊断用）：服务端在 MQ 上收到的每一帧 la 报文都原样记录
    // 一份（不做 k/α 变换、不做平滑），供地图单独画一层。
    // 用途：区分"差分点自己在跳"与"终端融合/标定算错"——
    //   走直线时若这一层点在地图上来回跳格，而终端上传的地图轨迹平滑，
    //   则量化台阶是源头；若这一层点平滑而地图轨迹发毛，则问题在融合。
    // 坐标：直接是 la 帧里的原始 x/y（差分系，单位 m），未做任何变换。
    // ------------------------------------------------------------
    QVector<QPointF> rtkTrail(quint32 terminalId) const
    {
        return _rtkTrail.value(terminalId);
    }
    QPointF rtkLastPoint(quint32 terminalId) const
    {
        return _rtkLast.value(terminalId);
    }
    // 最近一帧相对上一帧的位移（m）——直观的"跳变量"
    double rtkLastJumpM(quint32 terminalId) const
    {
        return _rtkJump.value(terminalId);
    }
    double rtkLastYawDeg(quint32 terminalId) const
    {
        return _rtkYawDeg.value(terminalId);
    }
    int rtkFrameCount(quint32 terminalId) const
    {
        return _rtkFrames.value(terminalId);
    }
    // 清空差分点轨迹（现场重新走直线前用）
    void clearRtkTrail();

signals:
    // 收到终端 calib 面板（带 terminalId，GUI 用 calibPanelOf(id) 取最新 JSON 原地刷新）
    void calibPanelUpdated(quint32 terminalId);

public slots:
    // 命令入口：控制台 stdin 与 GUI 命令框统一走这里（主线程执行）
    void executeCommand(const QString &line);

private:
    void printHelp();

public:
    // 手动命令接口（返回是否成功发送），实现全部转调到 SimDownlink
    // 供 GUI/命令行查询（均在主线程调用）
    QList<quint32> terminalIds() const { return _terminalIds; }
    // 返回 nullptr 表示该终端尚无会话
    const TermSession *sessionOf(quint32 terminalId) const
    {
        return _sessions.value(terminalId, nullptr);
    }

    bool sendAttitude(quint32 terminalId, int action);
    bool sendMove(quint32 terminalId, double x, double y,
                  double maxSpeed = 0, double arriveRadius = 0, qint64 timeoutMs = 0);
    // absolute=false：相对当前朝向旋转；absolute=true：转到绝对地图 yaw
    // （地图角 0=正东，顺时针为正，终端 [Map] Enabled=false 时为局部 yaw）
    bool sendRotate(quint32 terminalId, double angleRad, bool absolute,
                    double maxTurnRate = 0, double toleranceRad = 0,
                    qint64 timeoutMs = 0);
    // 相对移动：按终端最近上报的原始局部位姿，计算机体前后/左右目标点
    // forward > 0 前进，left > 0 左移
    bool sendMoveRelative(quint32 terminalId, double forward, double left,
                          double maxSpeed = 0, double arriveRadius = 0);
    bool sendVelocity(quint32 terminalId, double vx, double vy, double yawRate);
    bool sendStop(quint32 terminalId, bool emergency = false);
    bool sendParam(quint32 terminalId, int speedLevel);
    // 地图标定：action 见 DogProtocol::MapCalibAction
    // MC_END_LINE 时 targetX/targetY 为终点地图坐标
    bool sendMapCalib(quint32 terminalId, int action,
                      double targetX = 0.0, double targetY = 0.0);

private slots:
    void onMqConnected();
    void onMqDisconnected();
    void onMqMessage(const QString &routingKey, const QByteArray &payload);
    void onMqError(const QString &what);
    void onDownHeartbeat();      // 服务器下行心跳

private:
    // SimUplinkHost：上行解析器遇到需要业务动作的时刻回调这里
    void onTerminalReady(quint32 terminalId) override;
    void onTaskFinished(quint32 terminalId, int result) override;
    void onPanelUpdated(quint32 terminalId) override;

    // 自动巡航：按航点列表顺序取下一个点，占满一圈后回到起点
    void sendMoveCmd(quint32 terminalId);

    // la 差分帧：解析、入轨迹、按需打印（不参与控制逻辑）
    void handleRtkFrame(const QByteArray &payload);

    MqLink *_mq;
    QTimer *_hbTimer;
    SimDownlink *_down;
    SimUplink *_uplink;

    QString _host;
    quint16 _port;
    QString _vhost;
    QString _user;
    QString _password;
    QString _exchange;

    QList<quint32> _terminalIds;
    QList<QPair<double, double> > _waypoints;
    double _maxSpeed;
    double _arriveRadius;
    qint64 _taskTimeoutMs;
    int _nextWaypointDelayMs;
    bool _autoCruise;

    QMap<quint32, TermSession *> _sessions;

    // 差分原始点缓存（每终端一条轨迹 + 最近帧指标）
    QMap<quint32, QVector<QPointF> > _rtkTrail;
    QMap<quint32, QPointF> _rtkLast;
    QMap<quint32, double> _rtkJump;
    QMap<quint32, double> _rtkYawDeg;
    QMap<quint32, int> _rtkFrames;
    QMap<quint32, qint64> _rtkLastLogMs;
};

#endif  // SIM_CONTROLLER_H
