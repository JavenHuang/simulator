// ============================================================
// SimMapWindow：窗口外壳（地图 + 终端选择 + 命令输入 + 标定面板）
// 绘制控件已拆到 MapCanvas.h / MapCanvas.cpp。
// ============================================================

#include "MapWindow.h"
#include "SimController.h"
#include "SimCalibPanel.h"
#include "Protocol/DogProtocol.h"

#include <QPainter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QTimer>
#include <QLabel>
#include <QKeyEvent>
#include <QFocusEvent>
#include <cmath>

// ============================================================
// SimMapWindow：窗口外壳（地图 + 终端选择 + 按钮）
// ============================================================
SimMapWindow::SimMapWindow(SimController *sim, double widthM, double heightM,
                           QWidget *parent)
    : QWidget(parent)
    , _sim(sim)
    , _canvas(new SimMapCanvas(this))
    , _calibPanel(new SimCalibPanel(sim, this))
{
    setWindowTitle(QStringLiteral("机器狗地图监视（%1m x %2m，(0,0)居中，朝向 0=正东 顺时针）+ 标定面板")
                       .arg(widthM, 0, 'f', 0)
                       .arg(heightM, 0, 'f', 0));
    // 竖向地图（高>宽），窗口按高度优先布局；右侧再挂 560 宽的标定面板
    resize(1340, 940);

    _canvas->setController(_sim);
    _canvas->setMapSize(widthM, heightM);

    _terminalBox = new QComboBox(this);
    const QList<quint32> ids = _sim->terminalIds();
    for (int i = 0; i < ids.size(); ++i)
        _terminalBox->addItem(QStringLiteral("终端 %1").arg(ids.at(i)),
                              static_cast<int>(ids.at(i)));
    if (!ids.isEmpty())
    {
        _canvas->setTerminalId(ids.first());
        _calibPanel->setTerminalId(ids.first());
    }
    connect(_sim, &SimController::calibPanelUpdated,
            _calibPanel, &SimCalibPanel::onPanelUpdated);

    QPushButton *resetBtn = new QPushButton(QStringLiteral("设为原点(0,0 朝向0°)"), this);
    resetBtn->setToolTip(QStringLiteral("以狗当前位姿重建地图参考：当前位置=(0,0)，"
                                        "当前朝向=0°(正东)。相当于 mapreset 命令。"));
    QPushButton *clearBtn = new QPushButton(QStringLiteral("清除轨迹"), this);

    QHBoxLayout *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("显示终端:"), this));
    bar->addWidget(_terminalBox);
    bar->addWidget(resetBtn);
    bar->addWidget(clearBtn);
    bar->addStretch(1);

    // 常用命令快捷按钮：目标点固定为地图 (2, 0)，与 mapcheck/mapline 命令等价
    QPushButton *checkBtn = new QPushButton(QStringLiteral("mapcheck (2,0)"), this);
    checkBtn->setToolTip(QStringLiteral("直线标定预检：只计算残余朝向和尺度，不修改参考。"
                                        "等价于 mapcheck <id> 2 0"));
    QPushButton *lineBtn = new QPushButton(QStringLiteral("mapline (2,0)"), this);
    lineBtn->setToolTip(QStringLiteral("以地图 (2,0) 为终点做直线标定并使参考生效。"
                                       "等价于 mapline <id> 2 0"));
    QPushButton *ctrlBtn = new QPushButton(QStringLiteral("取得控制权"), this);
    ctrlBtn->setToolTip(QStringLiteral("下发申请SDK控制权（姿态命令 action=10），"
                                       "等价于 control <id>"));
    QLabel *driveTip = new QLabel(QStringLiteral("W/S=前后 A/D=左右平移 Q/E=左右旋转（点击地图获取键盘焦点）"), this);

    QHBoxLayout *quickBar = new QHBoxLayout;
    quickBar->addWidget(checkBtn);
    quickBar->addWidget(lineBtn);
    quickBar->addWidget(ctrlBtn);
    quickBar->addWidget(driveTip);
    quickBar->addStretch(1);

    // 命令输入：与控制台共用 SimController::executeCommand，
    // 反馈（下发日志/错误提示）输出到运行控制台
    _cmdEdit = new QLineEdit(this);
    _cmdEdit->setPlaceholderText(QStringLiteral("输入命令回车下发，如: fwd 1 1 / move 1 0 2 / help"));
    QPushButton *sendBtn = new QPushButton(QStringLiteral("发送"), this);
    QHBoxLayout *cmdBar = new QHBoxLayout;
    cmdBar->addWidget(new QLabel(QStringLiteral("命令:"), this));
    cmdBar->addWidget(_cmdEdit, 1);
    cmdBar->addWidget(sendBtn);

    QVBoxLayout *lay = new QVBoxLayout;
    lay->addWidget(_canvas, 1);
    lay->addLayout(bar);
    lay->addLayout(quickBar);
    lay->addLayout(cmdBar);

    // 左侧地图列 + 右侧标定面板：面板固定宽、内容原地覆盖，不参与滚动
    QHBoxLayout *root = new QHBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->addLayout(lay, 1);
    root->addWidget(_calibPanel, 0);

    connect(_terminalBox, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &SimMapWindow::onTerminalChanged);
    connect(resetBtn, &QPushButton::clicked, this, &SimMapWindow::onResetReference);
    connect(clearBtn, &QPushButton::clicked, this, &SimMapWindow::onClearTrail);
    connect(sendBtn, &QPushButton::clicked, this, &SimMapWindow::onSendCommand);
    connect(_cmdEdit, &QLineEdit::returnPressed, this, &SimMapWindow::onSendCommand);
    connect(checkBtn, &QPushButton::clicked, this, [this]()
    {
        const quint32 id = static_cast<quint32>(_terminalBox->currentData().toInt());
        if (id != 0)
            _sim->sendMapCalib(id, DogProtocol::MC_CHECK_LINE, 2.0, 0.0);
    });
    connect(lineBtn, &QPushButton::clicked, this, [this]()
    {
        const quint32 id = static_cast<quint32>(_terminalBox->currentData().toInt());
        if (id != 0)
            _sim->sendMapCalib(id, DogProtocol::MC_END_LINE, 2.0, 0.0);
    });
    connect(ctrlBtn, &QPushButton::clicked, this, [this]()
    {
        const quint32 id = static_cast<quint32>(_terminalBox->currentData().toInt());
        if (id != 0)
            _sim->sendAttitude(id, DogProtocol::AA_REQUEST_CONTROL);
    });

    // 窗口显示后把键盘焦点交给地图，保证 W/S 立即可用
    QTimer::singleShot(0, _canvas, [this]() { _canvas->setFocus(); });
}

void SimMapWindow::onTerminalChanged(int index)
{
    const quint32 id = static_cast<quint32>(_terminalBox->itemData(index).toInt());
    _canvas->setTerminalId(id);
    _canvas->clearTrail();
    _calibPanel->setTerminalId(id);
}

void SimMapWindow::onResetReference()
{
    const quint32 id = static_cast<quint32>(_terminalBox->currentData().toInt());
    if (id == 0)
        return;
    // 与 mapreset 命令一致：MC_RESET 不带终点坐标
    _sim->sendMapCalib(id, DogProtocol::MC_RESET);
}

void SimMapWindow::onClearTrail()
{
    _canvas->clearTrail();
}

void SimMapWindow::onSendCommand()
{
    const QString line = _cmdEdit->text().trimmed();
    if (line.isEmpty())
        return;
    // 与控制台命令同一入口（同在主线程，直接调用）
    _sim->executeCommand(line);
    _cmdEdit->clear();
    _cmdEdit->setFocus();
}
