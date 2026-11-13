#ifndef SIM_UPLINK_H
#define SIM_UPLINK_H

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QString>

#include "SimSession.h"

// ============================================================
// 上行事件回调
//
// 解析器只管"读出来、打出来"，遇到需要业务动作的时刻（终端就绪、
// 任务结束、面板刷新）通过本接口交回控制器；这样解析侧不必知道
// 自动巡航开没开、GUI 怎么刷，也不必包含 QObject。
// ============================================================
class SimUplinkHost
{
public:
    virtual ~SimUplinkHost() {}

    // 终端已就绪（status=READY、已连狗、已取得控制权）
    virtual void onTerminalReady(quint32 terminalId) = 0;
    // 任务结束，result 为 DogProtocol::MoveResult
    virtual void onTaskFinished(quint32 terminalId, int result) = 0;
    // 收到一帧 calib 面板（GUI 需原地刷新）
    virtual void onPanelUpdated(quint32 terminalId) = 0;
};

// ============================================================
// 上行报文（终端 → 服务器）
//
// 职责：分流路由键、按协议解包、节流打印、缓存标定面板并只在判定码
// 变化时打一行日志。不构造下行报文、不推进航点、不调度重试。
// 三种二进制报文（状态/任务反馈/心跳）走 DogProtocol；calib.<id> 是
// 裸 JSON，必须在 unpackMessage 之前分流，否则会解析失败被丢掉。
// ============================================================
class SimUplink
{
public:
    SimUplink(QMap<quint32, TermSession *> &sessions, SimUplinkHost *host);

    // 收到了特定终端的一条上行消息
    void onMessage(const QString &routingKey, const QByteArray &payload);
    // 该终端最近一份标定面板；无则为空对象
    QJsonObject panelOf(quint32 terminalId) const
    {
        return _calibPanels.value(terminalId);
    }

private:
    void handleStateReport(quint32 terminalId, const QByteArray &body);
    void handleFeedback(quint32 terminalId, const QByteArray &body);
    void handleUpHeartbeat(quint32 terminalId, const QByteArray &body);
    void handleCalibPanel(const QString &routingKey, const QByteArray &payload);

    QMap<quint32, TermSession *> &_sessions;
    SimUplinkHost *_host;

    // 每个终端最近一份标定面板 JSON，以及最近一次判定码（只在判定变化时打一行日志）
    QMap<quint32, QJsonObject> _calibPanels;
    QMap<quint32, QString> _calibLastCode;
};

#endif  // SIM_UPLINK_H
