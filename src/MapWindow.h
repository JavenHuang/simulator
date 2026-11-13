#ifndef SIM_MAP_WINDOW_H
#define SIM_MAP_WINDOW_H

#include <QWidget>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>

#include "MapCanvas.h"   // SimMapCanvas：地图绘制控件（本窗口的左侧主体）

class SimController;
class SimCalibPanel;

// ============================================================
// 服务端地图显示窗口外壳
//
// 结构：横向 root 布局 = 左侧（地图画布 + 端子下拉 + 按钮 + 命令框）
//                       + 右侧常驻标定面板 SimCalibPanel
// 地图坐标约定见 MapCanvas.h。
// 界面底部命令框与控制台命令共用 SimController::executeCommand。
// ============================================================
class SimMapWindow : public QWidget
{
    Q_OBJECT
public:
    explicit SimMapWindow(SimController *sim,
                          double widthM, double heightM,
                          QWidget *parent = nullptr);

    // 键盘遥控速度：线速度 m/s、旋转角速度 rad/s（<=0 用默认值）
    void setDriveSpeed(double speed)
    {
        if (_canvas)
            _canvas->setDriveSpeed(speed);
    }
    void setTurnRate(double rate)
    {
        if (_canvas)
            _canvas->setTurnRate(rate);
    }

private slots:
    void onTerminalChanged(int index);
    void onResetReference();
    void onClearTrail();
    void onSendCommand();

private:
    SimController *_sim;
    SimMapCanvas *_canvas;
    SimCalibPanel *_calibPanel;   // 右侧常驻标定面板（跟随端子下拉框）
    QComboBox *_terminalBox;
    QLineEdit *_cmdEdit;
};

#endif  // SIM_MAP_WINDOW_H
