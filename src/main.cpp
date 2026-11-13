#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QSettings>
#include <QFileInfo>
#include <QDebug>
#include <QTextStream>
#include <QThread>

#include "SimController.h"
#include "MapWindow.h"
#include "Protocol/DogProtocol.h"

// ============================================================
// 服务器联调模拟器入口（控制台 + 地图显示窗口 + 界面命令输入）
// 用法: simulator --config <ini路径> [--auto] [--nogui]
// 默认配置: 应用目录/config/simulator.ini
// --auto: 自动巡航模式（默认手动命令模式）
// --nogui: 不显示地图窗口（纯控制台）
// 命令解析统一在 SimController::executeCommand（控制台/GUI 共用）
// ============================================================

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("DogSimulator"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("服务器联调模拟器：验证机器狗终端全链路"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption cfgOpt(QStringList() << QStringLiteral("c") << QStringLiteral("config"),
                              QStringLiteral("配置文件路径(INI)"),
                              QStringLiteral("config"),
                              QStringLiteral("config/simulator.ini"));
    parser.addOption(cfgOpt);
    QCommandLineOption autoOpt(QStringLiteral("auto"),
                               QStringLiteral("自动巡航模式（默认手动命令）"));
    parser.addOption(autoOpt);
    QCommandLineOption noGuiOpt(QStringLiteral("nogui"),
                                QStringLiteral("不显示地图窗口（纯控制台）"));
    parser.addOption(noGuiOpt);
    parser.process(app);

    QString iniPath = parser.value(cfgOpt);
    QFileInfo iniInfo(iniPath);
    if (iniInfo.isRelative())
        iniPath = QCoreApplication::applicationDirPath() + QStringLiteral("/") + iniPath;
    if (!QFileInfo::exists(iniPath))
    {
        qCritical() << "配置文件不存在:" << iniPath;
        return 1;
    }

    QSettings s(iniPath, QSettings::IniFormat);
    const QString host = s.value("RabbitMq/Host").toString();
    if (host.trimmed().isEmpty())
    {
        qCritical() << "配置缺少 RabbitMq/Host";
        return 1;
    }
    const quint16 port = static_cast<quint16>(s.value("RabbitMq/Port", 5672).toUInt());
    const QString vhost = s.value("RabbitMq/Vhost", "/").toString();
    const QString user = s.value("RabbitMq/User", "guest").toString();
    const QString pass = s.value("RabbitMq/Password", "guest").toString();
    const QString exchange = s.value("RabbitMq/Exchange", "kdiss.dog").toString();

    // 被控终端ID列表："1" 或 "1,2"
    QList<quint32> ids;
    const QStringList idStrs = s.value("Control/TerminalIds", "1").toStringList();
    for (int i = 0; i < idStrs.size(); ++i)
    {
        const quint32 id = idStrs.at(i).trimmed().toUInt();
        if (id > 0)
            ids.append(id);
    }
    if (ids.isEmpty())
    {
        qCritical() << "配置 Control/TerminalIds 无效";
        return 1;
    }

    // 航点列表（狗原始局部坐标）："2:0|2:2|0:2|0:0"
    QList<QPair<double, double> > wps;
    const QString wpRaw = s.value("Control/Waypoints", "2:0|2:2|0:2|0:0").toString();
    const QStringList wpStrs = wpRaw.split('|', QString::SkipEmptyParts);
    for (int i = 0; i < wpStrs.size(); ++i)
    {
        const QStringList xy = wpStrs.at(i).trimmed().split(':');
        if (xy.size() != 2)
            continue;
        wps.append(qMakePair(xy.at(0).trimmed().toDouble(), xy.at(1).trimmed().toDouble()));
    }
    if (wps.isEmpty())
    {
        qCritical() << "配置 Control/Waypoints 无效";
        return 1;
    }

    SimController sim;
    sim.setMqParams(host, port, vhost, user, pass, exchange);
    sim.setTerminalIds(ids);
    sim.setWaypoints(wps);
    sim.setTaskParams(s.value("Control/MaxSpeed", 0.4).toDouble(),
                      s.value("Control/ArriveRadius", 0.3).toDouble(),
                      s.value("Control/TaskTimeoutMs", 60000).toLongLong(),
                      s.value("Control/NextWaypointDelayMs", 2000).toInt());
    sim.setAutoCruise(parser.isSet(autoOpt));
    sim.start();

    // 地图显示窗口（终端上报 calibrated=true 时即为地图坐标，直接绘制）
    // 地图尺寸从 [MapUI] 读取，默认宽(x)14m x 高(y)23m，(0,0) 位于地图正中心
    if (!parser.isSet(noGuiOpt))
    {
        const double mapW = s.value("MapUI/WidthM", 14.0).toDouble();
        const double mapH = s.value("MapUI/HeightM", 23.0).toDouble();
        SimMapWindow *mapWin = new SimMapWindow(&sim, mapW, mapH);
        mapWin->setDriveSpeed(s.value("Control/DriveSpeed", 0.3).toDouble());
        mapWin->setTurnRate(s.value("Control/TurnRate", 0.6).toDouble());
        mapWin->show();
    }

    // 命令行输入：stdin 必须放到后台线程读取。
    // QTextStream::readLine() 在终端下是阻塞调用，如果放在主线程的 QTimer 里调用，
    // "等待用户输入"期间整个事件循环会停摆 —— 下行心跳和任务定时器全部冻结，
    // 终端会因此判定"服务器消息超时"并取消正在执行的任务。
    QThread *inputThread = QThread::create([&sim]() {
        QTextStream in(stdin);
        while (!in.atEnd())
        {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty())
                continue;
            QMetaObject::invokeMethod(&sim, "executeCommand", Qt::QueuedConnection,
                                      Q_ARG(QString, line));
        }
    });
    inputThread->start();

    sim.executeCommand(QStringLiteral("help"));

    return app.exec();
}
