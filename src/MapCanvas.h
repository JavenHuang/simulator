#ifndef SIM_MAP_CANVAS_H
#define SIM_MAP_CANVAS_H

#include <QWidget>
#include <QVector>
#include <QPointF>

class SimController;
class QTimer;
class QKeyEvent;
class QFocusEvent;

// ============================================================
// 地图绘制控件（服务端 simulator 侧，非终端）
//
// 地图约定（与终端 [Map] 一致）：
//   +x 正东（屏幕右），+y 正南（屏幕下）
//   朝向角 0=正东，顺时针为正（0~360：0=东 90=南 180=西 270=北）
//   地图尺寸默认宽(x)14m x 高(y)23m，(0,0) 位于地图正中心
//   即 x∈[-7,7]、y∈[-11.5,11.5]
//
// 数据来源：终端 state.{id} 上报。calibrated=true 时 x/y/yaw 已是
// 地图坐标，直接显示；false 时以黄色图标显示原始局部坐标并提示。
//
// 键盘遥控：W/S/A/D 平移、Q/E 转向，按住期间由 _driveTimer 周期续发
// 速度指令（终端侧有死手超时，必须持续补发）。
// ============================================================
class SimMapCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit SimMapCanvas(QWidget *parent = nullptr);

    void setController(SimController *sim) { _sim = sim; }
    void setMapSize(double widthM, double heightM);
    void setTerminalId(quint32 id) { _terminalId = id; }
    // 按住 W/S/A/D 时的线速度（m/s），0 表示使用默认值
    void setDriveSpeed(double speed) { _driveSpeed = (speed > 0.0) ? speed : 0.3; }
    // 按住 Q/E 时的旋转角速度（rad/s），0 表示使用默认值
    void setTurnRate(double rate) { _turnRate = (rate > 0.0) ? rate : 0.6; }
    void clearTrail()
    {
        _trail.clear();
        _localTrail.clear();
        _lastTrailValid = false;
        _lastLocalTrailValid = false;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private slots:
    void onDriveTick();

private:
    void applyDrive();

    void sampleTrail(QVector<QPointF> &trail, QPointF &lastPt, bool &lastValid,
                     double x, double y, bool valid);

    SimController *_sim;
    double _mapW;
    double _mapH;
    quint32 _terminalId;
    QTimer *_driveTimer;   // 按住方向键期间周期续发速度指令（终端有死手超时）
    bool _keyW;            // W 是否按下（前进）
    bool _keyS;            // S 是否按下（后退）
    bool _keyA;            // A 是否按下（左平移）
    bool _keyD;            // D 是否按下（右平移）
    bool _keyQ;            // Q 是否按下（左转）
    bool _keyE;            // E 是否按下（右转）
    double _driveSpeed;    // 线速度幅值 m/s
    double _turnRate;      // 旋转角速度幅值 rad/s
    QVector<QPointF> _trail;      // 地图坐标轨迹
    QVector<QPointF> _localTrail; // 局部坐标轨迹（叠加对比用）
    QPointF _lastTrailPoint;
    bool _lastTrailValid;
    QPointF _lastLocalTrailPoint;
    bool _lastLocalTrailValid;
};

#endif  // SIM_MAP_CANVAS_H
