// ============================================================
// 上行报文（终端 → 服务器）—— 实现
//
// 分流顺序不能反：cmd.*（自己下发的回环）先丢、calib.*（裸 JSON）先分走，
// 剩下的才按 DogProtocol 解包。状态打印按每 10 包一次节流，默认 10Hz 上报
// 即 1Hz 日志；"终端就绪"的判断放在节流之后，与拆分前触发频率一致。
// ============================================================
#include "SimUplink.h"

#include <QDataStream>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include "Protocol/DogProtocol.h"

namespace
{
// SDK ControlMode → 可读文本（与终端 [DogState] 模式 行的口径一致）
QString postureText(quint8 v)
{
    switch (v)
    {
    case 1: return QStringLiteral("stand_up");
    case 2: return QStringLiteral("stand_down");
    case 3: return QStringLiteral("lock");
    case 4: return QStringLiteral("emergency");
    case 5: return QStringLiteral("move");
    case 6: return QStringLiteral("balance_stand");
    case 7: return QStringLiteral("init");
    default: return QStringLiteral("unknown");
    }
}
}  // namespace

SimUplink::SimUplink(QMap<quint32, TermSession *> &sessions, SimUplinkHost *host)
    : _sessions(sessions), _host(host)
{
}

void SimUplink::onMessage(const QString &routingKey, const QByteArray &payload)
{
    // 过滤自己下发的 cmd.* 回环
    if (routingKey.startsWith(QStringLiteral("cmd.")))
        return;

    // calib.{id}：终端标定面板，是 JSON 不是 DogProtocol 二进制，必须在 unpack 之前分流，
    // 否则会解析失败被丢掉（这就是"服务端看不到面板"的直接原因）
    if (routingKey.startsWith(QStringLiteral("calib.")))
    {
        handleCalibPanel(routingKey, payload);
        return;
    }

    DogProtocol::MessageHeader header;
    QByteArray body;
    if (!DogProtocol::unpackMessage(payload, header, body))
        return;

    const quint32 id = header.terminalId;
    switch (header.msgType)
    {
    case DogProtocol::MSG_STATE_REPORT:  handleStateReport(id, body);  break;
    case DogProtocol::MSG_TASK_FEEDBACK: handleFeedback(id, body);     break;
    case DogProtocol::MSG_HEARTBEAT:     handleUpHeartbeat(id, body);  break;
    default: break;
    }
}

void SimUplink::handleStateReport(quint32 terminalId, const QByteArray &body)
{
    QDataStream in(body);
    in.setVersion(QDataStream::Qt_5_12);
    DogProtocol::StateReport report;
    in >> report;
    if (in.status() != QDataStream::Ok)
        return;

    TermSession *s = _sessions.value(terminalId, nullptr);
    if (!s)
        return;  // 非本模拟器控制的终端
    s->lastState = report.state;
    s->hasState = true;

    // 状态打印节流：每10包打印一次（默认10Hz → 1Hz）
    if (++s->reportCount % 10 != 1)
        return;

    const DogProtocol::DogState &st = report.state;
    // z 为机身高度：站立/移动约 0.35m，趴下约 0.10m
    // posture 为狗上报的 ControlMode，取自 StateReport 报文（无需额外请求）
    qInfo().noquote() << QStringLiteral("[Sim][终端%1] status=%2 dog=%3 ctrl=%4 pos(%5,%6) z=%7 yaw=%8 bat=%9% posture=%10(%11) func=%12(%13) calib=%14")
                         .arg(terminalId)
                         .arg(st.status)
                         .arg(st.dogConnected ? 1 : 0)
                         .arg(st.controlAcquired ? 1 : 0)
                         .arg(st.x, 0, 'f', 2)
                         .arg(st.y, 0, 'f', 2)
                         .arg(st.z, 0, 'f', 2)
                         .arg(st.yaw, 0, 'f', 2)
                         .arg(st.batteryPercent)
                         .arg(postureText(st.controlMode))
                         .arg(st.controlMode)
                         .arg(st.functionMode)
                         .arg(st.speedLevel)
                         .arg(st.calibrated ? 1 : 0);

    // 就绪且无任务：交给控制器决定要不要发下一个航点
    if (st.status == DogProtocol::TS_READY && st.dogConnected
        && st.controlAcquired && !s->taskInFlight)
    {
        _host->onTerminalReady(terminalId);
    }
}

void SimUplink::handleFeedback(quint32 terminalId, const QByteArray &body)
{
    QDataStream in(body);
    in.setVersion(QDataStream::Qt_5_12);
    DogProtocol::TaskFeedback fb;
    in >> fb;
    if (in.status() != QDataStream::Ok)
        return;

    TermSession *s = _sessions.value(terminalId, nullptr);
    if (!s)
        return;

    qInfo().noquote() << QStringLiteral("[Sim][终端%1] 任务反馈 task=%2 result=%3 remain=%4 msg=%5")
                         .arg(terminalId)
                         .arg(fb.taskId)
                         .arg(fb.result)
                         .arg(QString::number(fb.remainingDist, 'f', 2))
                         .arg(fb.message);

    if (fb.taskId != s->currentTaskId)
        return;

    switch (fb.result)
    {
    case DogProtocol::MR_ARRIVED:
    case DogProtocol::MR_REJECTED:
    case DogProtocol::MR_FAILED:
    case DogProtocol::MR_TIMEOUT:
        s->taskInFlight = false;
        _host->onTaskFinished(terminalId, static_cast<int>(fb.result));
        break;
    default:
        break;  // MR_ACCEPTED / MR_MOVING 等待后续反馈
    }
}

void SimUplink::handleUpHeartbeat(quint32 terminalId, const QByteArray &body)
{
    QDataStream in(body);
    in.setVersion(QDataStream::Qt_5_12);
    DogProtocol::HeartbeatMsg hb;
    in >> hb;
    if (in.status() != QDataStream::Ok)
        return;
    TermSession *s = _sessions.value(terminalId, nullptr);
    if (!s)
        return;
    s->lastState.status = static_cast<DogProtocol::TerminalStatus>(hb.status);
}

void SimUplink::handleCalibPanel(const QString &routingKey, const QByteArray &payload)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
    {
        qWarning().noquote() << QStringLiteral("[Sim] calib 面板 JSON 解析失败（%1）：%2")
                                    .arg(routingKey, err.errorString());
        return;
    }

    const QJsonObject obj = doc.object();

    // 终端 ID 优先取 JSON 字段，缺失则回落到路由键 calib.{id}
    quint32 id = static_cast<quint32>(obj.value(QStringLiteral("terminal_id")).toDouble());
    if (id == 0)
        id = routingKey.mid(QStringLiteral("calib.").size()).toUInt();

    _calibPanels.insert(id, obj);
    // GUI 面板每次都要刷（数值每秒在变），日志只在判定变化时打
    _host->onPanelUpdated(id);

    const QJsonObject verdict = obj.value(QStringLiteral("verdict")).toObject();
    const QString code = verdict.value(QStringLiteral("code")).toString();
    if (_calibLastCode.value(id) == code)
        return;
    _calibLastCode.insert(id, code);

    qInfo().noquote() << QStringLiteral("[Sim][终端%1] 标定判定 %2｜%3 → 下一步：%4")
                             .arg(id)
                             .arg(code)
                             .arg(verdict.value(QStringLiteral("head")).toString())
                             .arg(verdict.value(QStringLiteral("action")).toString());
    const QString gate = verdict.value(QStringLiteral("gate")).toString();
    if (!gate.isEmpty() && gate != QStringLiteral("无"))
        qWarning().noquote() << QStringLiteral("[Sim][终端%1] 闸门未清：%2").arg(id).arg(gate);
}
