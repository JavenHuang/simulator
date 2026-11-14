// ============================================================
// simulator 命令入口：帮助文本与单行命令解析
//
// 只做"文本 → 调用"的翻译：拆词、校验参数个数、单位换算（角度），
// 具体报文的打包与发送在 SimDownlink。控制台 stdin 与 GUI 命令框
// 共用 executeCommand，因此这里不能阻塞、不能弹窗。
// ============================================================

#include "SimController.h"

#include <QDebug>
#include <QStringList>
#include <QCoreApplication>

void SimController::printHelp()
{
    qInfo() << "";
    qInfo() << "========== 可用命令 ==========";
    qInfo() << "  attitude <id> <action>   姿态: 1=站立 2=趴下 3=平衡 4=锁定 5=急停";
    qInfo() << "  control <id>             申请SDK控制权（AutoAcquireControl=false 时手动触发）";
    qInfo() << "  move <id> <x> <y> [speed] [radius]  机动到坐标点（地图坐标系）";
    qInfo() << "  fwd <id> <米> [speed] [radius]    沿机头方向相对前进(+)/后退(-)";
    qInfo() << "  side <id> <米> [speed] [radius]   机体左右相对移动: 左(+)/右(-)";
    qInfo() << "  rotate <id> <delta_deg> [rate_deg_s] [tolerance_deg]  相对旋转: 左(+)/右(-)";
    qInfo() << "  rotateTo <id> <target_deg> [rate_deg_s] [tolerance_deg]  转到绝对朝向（地图角 0=正东 顺时针）";
    qInfo() << "  velocity <id> <vx> <vy> <yawRate>  速度控制";
    qInfo() << "  stop <id> [emergency]    停止（emergency=1急停）";
    qInfo() << "  param <id> <level>       速度档位 1=慢 2=正常 3=快";
    qInfo() << "  mapreset <id>            以狗当前位姿重建地图参考（当前点=(0,0) 朝向=0°正东）";
    qInfo() << "  mapline <id> <Bx> <By>   直线标定结束，B 为终点地图坐标";
    qInfo() << "  mapcheck <id> <Bx> <By>  直线标定预检（只计算不修改，返回残余角/尺度）";
    qInfo() << "  mapoff <id>              关闭地图坐标，回到原始局部坐标";
    qInfo() << "  auto                     切换到自动巡航模式";
    qInfo() << "  manual                   切换到手动命令模式";
    qInfo() << "  rtkclear                 清空地图上的差分原始点轨迹（现场重新走直线前用）";
    qInfo() << "  help                     显示本帮助";
    qInfo() << "  quit                     退出";
    qInfo() << "==============================";
    qInfo() << "";
}

void SimController::executeCommand(const QString &line)
{
    const QStringList parts = line.simplified().split(' ', QString::SkipEmptyParts);
    if (parts.isEmpty())
        return;

    const QString cmd = parts.at(0).toLower();

    if (cmd == "quit" || cmd == "exit" || cmd == "q")
    {
        qInfo() << "[Sim] 退出";
        QCoreApplication::quit();
        return;
    }
    if (cmd == "help" || cmd == "h" || cmd == "?")
    {
        printHelp();
        return;
    }
    if (cmd == "rtkclear")
    {
        clearRtkTrail();
        return;
    }
    if (cmd == "auto")
    {
        setAutoCruise(true);
        qInfo() << "[Sim] 已切换到自动巡航模式";
        return;
    }
    if (cmd == "manual")
    {
        setAutoCruise(false);
        qInfo() << "[Sim] 已切换到手动命令模式";
        return;
    }

    // 需要终端ID的命令
    if (parts.size() < 2)
    {
        qWarning() << "[Sim] 参数不足，输入 help 查看用法";
        return;
    }
    bool ok = false;
    const quint32 id = parts.at(1).toUInt(&ok);
    if (!ok || id == 0)
    {
        qWarning() << "[Sim] 终端ID无效:" << parts.at(1);
        return;
    }

    if (cmd == "attitude")
    {
        if (parts.size() < 3)
        {
            qWarning() << "[Sim] 用法: attitude <id> <action>  (1=站立 2=趴下 3=平衡 4=锁定 5=急停)";
            return;
        }
        sendAttitude(id, parts.at(2).toInt());
    }
    else if (cmd == "control" || cmd == "take" || cmd == "request")
    {
        // 服务端主动申请 SDK 控制权：终端收到后走 beginControlAcquire 流程，
        // 狗端 functionMode 变为 sdk 后终端才确认取得控制权
        qInfo() << "[Sim] 下发申请控制权 → 终端" << id;
        sendAttitude(id, DogProtocol::AA_REQUEST_CONTROL);
    }
    else if (cmd == "move")
    {
        if (parts.size() < 4)
        {
            qWarning() << "[Sim] 用法: move <id> <x> <y> [speed] [radius]";
            return;
        }
        sendMove(id, parts.at(2).toDouble(), parts.at(3).toDouble(),
                 parts.size() > 4 ? parts.at(4).toDouble() : 0,
                 parts.size() > 5 ? parts.at(5).toDouble() : 0);
    }
    else if (cmd == "rotate" || cmd == "rotateto")
    {
        if (parts.size() < 3)
        {
            qWarning() << "[Sim] 用法: rotate <id> <delta_deg> [rate_deg_s] [tolerance_deg]  |  "
                          "rotateTo <id> <target_deg> [rate_deg_s] [tolerance_deg]";
            return;
        }
        const double kDegToRad = 3.14159265358979323846 / 180.0;
        sendRotate(id, parts.at(2).toDouble() * kDegToRad, cmd == "rotateto",
                   (parts.size() > 3 ? parts.at(3).toDouble() : 0.0) * kDegToRad,
                   (parts.size() > 4 ? parts.at(4).toDouble() : 0.0) * kDegToRad);
    }
    else if (cmd == "fwd" || cmd == "side")
    {
        if (parts.size() < 3)
        {
            qWarning() << "[Sim] 用法: fwd <id> <米> [speed] [radius]  |  side <id> <米> [speed] [radius]";
            return;
        }
        const double dist = parts.at(2).toDouble();
        // fwd: 正=前进 负=后退；side: 正=左移 负=右移
        sendMoveRelative(id, (cmd == "fwd") ? dist : 0.0, (cmd == "side") ? dist : 0.0,
                         parts.size() > 3 ? parts.at(3).toDouble() : 0,
                         parts.size() > 4 ? parts.at(4).toDouble() : 0);
    }
    else if (cmd == "velocity")
    {
        if (parts.size() < 5)
        {
            qWarning() << "[Sim] 用法: velocity <id> <vx> <vy> <yawRate>";
            return;
        }
        sendVelocity(id, parts.at(2).toDouble(), parts.at(3).toDouble(),
                     parts.at(4).toDouble());
    }
    else if (cmd == "stop")
    {
        sendStop(id, parts.size() > 2 && parts.at(2).toInt() == 1);
    }
    else if (cmd == "param")
    {
        if (parts.size() < 3)
        {
            qWarning() << "[Sim] 用法: param <id> <level>  (1=慢 2=正常 3=快)";
            return;
        }
        sendParam(id, parts.at(2).toInt());
    }
    else if (cmd == "mapreset")
    {
        sendMapCalib(id, DogProtocol::MC_RESET);
    }
    else if (cmd == "mapline" || cmd == "mapcheck")
    {
        if (parts.size() < 4)
        {
            qWarning() << "[Sim] 用法: mapline|mapcheck <id> <Bx> <By>  (终点地图坐标)";
            return;
        }
        const bool checkOnly = (cmd == "mapcheck");
        sendMapCalib(id, checkOnly ? DogProtocol::MC_CHECK_LINE
                                   : DogProtocol::MC_END_LINE,
                     parts.at(2).toDouble(), parts.at(3).toDouble());
    }
    else if (cmd == "mapoff")
    {
        sendMapCalib(id, DogProtocol::MC_DISABLE);
    }
    else
    {
        qWarning() << "[Sim] 未知命令:" << cmd << "，输入 help 查看用法";
    }
}
