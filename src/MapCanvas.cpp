// ============================================================
// SimMapCanvas：地图绘制控件（paintEvent / 键盘遥控 / 轨迹采样）
// 从原 MapWindow.cpp（637 行、两个类混编）拆出；窗口容器见 MapWindow.cpp。
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
#include "MapCanvas.h"

namespace
{
const double kRadToDeg = 180.0 / 3.14159265358979323846;
const double kPiHalf = 3.14159265358979323846 / 2.0;

// 度数归一化到 [0, 360)
double degNorm360(double deg)
{
    deg = std::fmod(deg, 360.0);
    if (deg < 0.0)
        deg += 360.0;
    return deg;
}
}  // namespace

// ============================================================
// SimMapCanvas：地图绘制
// ============================================================
SimMapCanvas::SimMapCanvas(QWidget *parent)
    : QWidget(parent)
    , _sim(nullptr)
    , _mapW(14.0)
    , _mapH(23.0)
    , _terminalId(0)
    , _driveTimer(nullptr)
    , _keyW(false)
    , _keyS(false)
    , _keyA(false)
    , _keyD(false)
    , _keyQ(false)
    , _keyE(false)
    , _driveSpeed(0.3)
    , _turnRate(0.6)
    , _lastTrailValid(false)
    , _lastLocalTrailValid(false)
{
    setMinimumSize(620, 700);
    // 需要键盘焦点才能接收 W/S/A/D/Q/E；点击地图即自动获得焦点
    setFocusPolicy(Qt::StrongFocus);

    // 界面刷新：100ms 足够流畅，也降低 CPU 占用
    QTimer *t = new QTimer(this);
    connect(t, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
    t->start(100);

    // 按住方向键期间每 100ms 续发速度指令，避免终端死手保护自动零速
    // W/S 前后、A/D 左右平移、Q/E 左右旋转，可同时按（斜向 / 边走边转）
    _driveTimer = new QTimer(this);
    _driveTimer->setInterval(100);
    connect(_driveTimer, &QTimer::timeout, this, &SimMapCanvas::onDriveTick);
}

void SimMapCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat())
    {
        event->accept();
        return;
    }
    switch (event->key())
    {
    case Qt::Key_W:
        _keyW = true;
        break;
    case Qt::Key_S:
        _keyS = true;
        break;
    case Qt::Key_A:
        _keyA = true;
        break;
    case Qt::Key_D:
        _keyD = true;
        break;
    case Qt::Key_Q:
        _keyQ = true;
        break;
    case Qt::Key_E:
        _keyE = true;
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    applyDrive();
    event->accept();
}

void SimMapCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat())
    {
        event->accept();
        return;
    }
    switch (event->key())
    {
    case Qt::Key_W:
        _keyW = false;
        break;
    case Qt::Key_S:
        _keyS = false;
        break;
    case Qt::Key_A:
        _keyA = false;
        break;
    case Qt::Key_D:
        _keyD = false;
        break;
    case Qt::Key_Q:
        _keyQ = false;
        break;
    case Qt::Key_E:
        _keyE = false;
        break;
    default:
        QWidget::keyReleaseEvent(event);
        return;
    }
    applyDrive();
    event->accept();
}

// 按当前按键状态下发机体系速度（终端按 JoyLinearScale/JoyTurnScale 归一化后转成遥控量）：
//   W=前进(+vx)  S=后退(-vx)
//   A=左移(+vy)  D=右移(-vy)      机体系 +y 指向左
//   Q=左转(+yawRate)  E=右转(-yawRate)   yawRate 正为航向增大（逆时针/左转）
// 三类分量可任意组合，全松则立即停车并停发。
void SimMapCanvas::applyDrive()
{
    if (!_sim || _terminalId == 0)
        return;

    const int fwd = (_keyW ? 1 : 0) - (_keyS ? 1 : 0);
    const int lat = (_keyA ? 1 : 0) - (_keyD ? 1 : 0);
    const int rot = (_keyQ ? 1 : 0) - (_keyE ? 1 : 0);

    if (fwd == 0 && lat == 0 && rot == 0)
    {
        _driveTimer->stop();
        _sim->sendStop(_terminalId, false);  // 松开立即零速
        return;
    }
    if (!_driveTimer->isActive())
        _driveTimer->start();
    _sim->sendVelocity(_terminalId, fwd * _driveSpeed, lat * _driveSpeed, rot * _turnRate);
}

void SimMapCanvas::onDriveTick()
{
    applyDrive();
}

// 失焦时可能收不到 keyRelease，主动清零，避免狗一直前进
void SimMapCanvas::focusOutEvent(QFocusEvent *event)
{
    _keyW = false;
    _keyS = false;
    _keyA = false;
    _keyD = false;
    _keyQ = false;
    _keyE = false;
    applyDrive();
    QWidget::focusOutEvent(event);
}

void SimMapCanvas::setMapSize(double widthM, double heightM)
{
    _mapW = (widthM > 1.0) ? widthM : 23.0;
    _mapH = (heightM > 1.0) ? heightM : 14.0;
    update();
}

void SimMapCanvas::sampleTrail(QVector<QPointF> &trail, QPointF &lastPt,
                               bool &lastValid, double x, double y, bool valid)
{
    if (!valid)
    {
        lastValid = false;
        return;
    }
    const QPointF pt(x, y);
    if (lastValid)
    {
        const double dx = pt.x() - lastPt.x();
        const double dy = pt.y() - lastPt.y();
        if (dx * dx + dy * dy < 0.0025)   // 位移 < 5cm 不采样
            return;
    }
    lastPt = pt;
    lastValid = true;
    trail.append(pt);
    if (trail.size() > 4000)
        trail.remove(0, 1000);
}

void SimMapCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // ---- 布局：四周留出刻度边距，地图区等比缩放并居中 ----
    const int margin = 46;
    const double availW = width() - 2.0 * margin;
    const double availH = height() - 2.0 * margin;
    const double pxPerM = qMin(availW / _mapW, availH / _mapH);
    const double mapPxW = _mapW * pxPerM;
    const double mapPxH = _mapH * pxPerM;
    const double ox = (width() - mapPxW) / 2.0;
    const double oy = (height() - mapPxH) / 2.0;
    QRectF mapRect(ox, oy, mapPxW, mapPxH);

    p.fillRect(rect(), QColor(247, 249, 251));
    p.fillRect(mapRect, QColor(255, 255, 255));

    // 地图范围：x∈[-W/2, W/2]，y∈[-H/2, H/2]，(0,0) 位于正中心
    const double cx = ox + mapPxW / 2.0;
    const double cy = oy + mapPxH / 2.0;

    // ---- 网格：1m 细线，5m 粗线 + 刻度 ----
    QPen finePen(QColor(225, 230, 236), 1);
    QPen majorPen(QColor(190, 198, 208), 1);
    QPen axisPen(QColor(90, 100, 112), 1);
    const int halfWm = static_cast<int>(std::floor(_mapW / 2.0));
    const int halfHm = static_cast<int>(std::floor(_mapH / 2.0));
    for (int m = -halfWm; m <= halfWm; ++m)
    {
        const double x = cx + m * pxPerM;
        p.setPen((m % 5 == 0) ? majorPen : finePen);
        p.drawLine(QPointF(x, oy), QPointF(x, oy + mapPxH));
    }
    for (int m = -halfHm; m <= halfHm; ++m)
    {
        const double y = cy + m * pxPerM;
        p.setPen((m % 5 == 0) ? majorPen : finePen);
        p.drawLine(QPointF(ox, y), QPointF(ox + mapPxW, y));
    }
    p.setPen(axisPen);
    p.drawRect(mapRect);

    // ---- 刻度标注：x 向右为东，y 向下为南 ----
    p.setPen(QColor(110, 120, 132));
    QFont f = font();
    f.setPointSizeF(8.5);
    p.setFont(f);
    const int xStep = (pxPerM >= 28) ? 2 : 5;
    for (int m = -halfWm; m <= halfWm; m += xStep)
        p.drawText(QPointF(cx + m * pxPerM - 6, oy + mapPxH + 14), QString::number(m));
    const int yStep = (pxPerM >= 28) ? 2 : 5;
    for (int m = -halfHm; m <= halfHm; m += yStep)
        p.drawText(QPointF(ox - 30, cy + m * pxPerM + 4), QString::number(m));
    p.setPen(QColor(70, 130, 180));
    p.drawText(QPointF(ox + mapPxW - 52, oy + mapPxH + 30), QStringLiteral("东 +x →"));
    p.drawText(QPointF(cx - 22, oy + mapPxH + 30), QStringLiteral("↓ 南 +y"));
    p.setPen(QColor(160, 168, 178));
    p.drawText(QPointF(cx + 5, cy + 4), QStringLiteral("(0,0)"));

    // ---- 终端状态 ----
    bool hasPose = false;
    double mapX = 0.0;
    double mapY = 0.0;
    double mapYawRad = 0.0;
    double bodyVx = 0.0;
    double localX = 0.0;
    double localY = 0.0;
    double localYawRad = 0.0;
    bool calibrated = false;
    QString statusText = QStringLiteral("等待终端 %1 上报...").arg(_terminalId);

    if (_sim && _terminalId > 0)
    {
        const TermSession *s = _sim->sessionOf(_terminalId);
        if (s && s->hasState)
        {
            hasPose = true;
            calibrated = s->lastState.calibrated;
            mapX = s->lastState.x;
            mapY = s->lastState.y;
            mapYawRad = s->lastState.yaw;
            bodyVx = s->lastState.bodyVx;
            localX = s->lastState.localX;
            localY = s->lastState.localY;
            localYawRad = s->lastState.localYaw;
            // 轨迹仅采样地图坐标；未标定时位姿直接用上报值显示
            sampleTrail(_trail, _lastTrailPoint, _lastTrailValid,
                        mapX, mapY, calibrated);
            // 局部轨迹：仅已标定时叠加才有对比意义
            sampleTrail(_localTrail, _lastLocalTrailPoint, _lastLocalTrailValid,
                        localX, localY, calibrated);
        }
    }

    // ---- 轨迹 ----
    if (!_trail.isEmpty())
    {
        QPen trailPen(QColor(66, 133, 244, 180), 2);
        p.setPen(trailPen);
        QPolygonF poly;
        for (int i = 0; i < _trail.size(); ++i)
            poly.append(QPointF(cx + _trail.at(i).x() * pxPerM,
                                cy + _trail.at(i).y() * pxPerM));
        p.drawPolyline(poly);
        // 轨迹起点标记
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(66, 133, 244, 120));
        p.drawEllipse(poly.first(), 4, 4);
    }

    // ---- 局部坐标轨迹（绿色，叠加对比用；仅已标定时绘制） ----
    if (calibrated && !_localTrail.isEmpty())
    {
        QPen trailPen(QColor(46, 160, 67, 170), 2, Qt::DashLine);
        p.setPen(trailPen);
        QPolygonF poly;
        for (int i = 0; i < _localTrail.size(); ++i)
            poly.append(QPointF(cx + _localTrail.at(i).x() * pxPerM,
                                cy + _localTrail.at(i).y() * pxPerM));
        p.drawPolyline(poly);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(46, 160, 67, 110));
        p.drawEllipse(poly.first(), 4, 4);
    }

    // ---- 狗图标：位置圆点 + 朝向箭头（0=右/东，顺时针） ----
    if (hasPose)
    {
        double px = cx + mapX * pxPerM;
        double py = cy + mapY * pxPerM;
        const bool outOfMap = (std::fabs(mapX) > _mapW / 2.0
                               || std::fabs(mapY) > _mapH / 2.0);
        // 超出地图范围时钳制到边缘显示，避免丢失
        px = qBound(ox, px, ox + mapPxW);
        py = qBound(oy, py, oy + mapPxH);

        const double a = mapYawRad;
        const double len = 26.0;
        const double halfW = 9.0;
        const QPointF dir(std::cos(a), std::sin(a));
        const QPointF perp(-std::sin(a), std::cos(a));
        const QPointF tip(px + dir.x() * len, py + dir.y() * len);
        const QPointF base(px - dir.x() * 3.0, py - dir.y() * 3.0);
        QPolygonF arrow;
        arrow << tip
              << QPointF(base.x() + perp.x() * halfW, base.y() + perp.y() * halfW)
              << QPointF(base.x() - perp.x() * halfW, base.y() - perp.y() * halfW);

        p.setPen(Qt::NoPen);
        p.setBrush(outOfMap ? QColor(220, 60, 60) : (calibrated ? QColor(224, 90, 30)
                                                                : QColor(210, 170, 40)));
        p.drawPolygon(arrow);
        p.setBrush(QColor(60, 64, 70));
        p.drawEllipse(QPointF(px, py), 4.5, 4.5);
        if (outOfMap)
        {
            p.setPen(QPen(QColor(220, 60, 60), 2));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(px, py), 14, 14);
        }

        // ---- 局部坐标图标（绿色，与橙色地图图标叠加对比；仅已标定时绘制） ----
        if (calibrated)
        {
            const double lx = cx + localX * pxPerM;
            const double ly = cy + localY * pxPerM;
            // 局部 yaw 逆时针为正，屏幕角取负
            const double la = -localYawRad;
            const double llen = 22.0;
            const double lhalfW = 8.0;
            const QPointF ldir(std::cos(la), std::sin(la));
            const QPointF lperp(-std::sin(la), std::cos(la));
            const QPointF ltip(lx + ldir.x() * llen, ly + ldir.y() * llen);
            const QPointF lbase(lx - ldir.x() * 3.0, ly - ldir.y() * 3.0);
            QPolygonF larrow;
            larrow << ltip
                   << QPointF(lbase.x() + lperp.x() * lhalfW, lbase.y() + lperp.y() * lhalfW)
                   << QPointF(lbase.x() - lperp.x() * lhalfW, lbase.y() - lperp.y() * lhalfW);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(46, 160, 67));
            p.drawPolygon(larrow);
            p.setBrush(QColor(30, 120, 45));
            p.drawEllipse(QPointF(lx, ly), 4.5, 4.5);
        }

        // ---- 坐标系罗盘（右上角）：灰=地图系(固定)，绿=局部系 +x 轴 ----
        // 局部系 +x 轴的地图角 = mapYaw + localYaw（由 α=refYaw-localYaw-π/2 反解）
        {
            const double r = 34.0;
            const double compX = ox + mapPxW - r - 12.0;
            const double compY = oy + r + 12.0;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 220));
            p.drawEllipse(QPointF(compX, compY), r, r);
            p.setPen(QPen(QColor(150, 158, 168), 1));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(compX, compY), r, r);
            // 地图系：东(0°)/南(90°) 两个短灰箭头
            for (int k = 0; k < 2; ++k)
            {
                const double a = k * kPiHalf;
                const QPointF d(std::cos(a), std::sin(a));
                p.setPen(QPen(QColor(120, 128, 138), 2));
                p.drawLine(QPointF(compX, compY),
                           QPointF(compX + d.x() * (r - 10), compY + d.y() * (r - 10)));
            }
            // 局部系 +x 轴（绿）：仅已标定时绘制
            if (calibrated)
            {
                const double axisA = mapYawRad + localYawRad;
                const QPointF gd(std::cos(axisA), std::sin(axisA));
                p.setPen(QPen(QColor(46, 160, 67), 2));
                p.drawLine(QPointF(compX, compY),
                           QPointF(compX + gd.x() * (r - 10), compY + gd.y() * (r - 10)));
            }
            p.setPen(QColor(80, 88, 98));
            p.drawText(QPointF(compX - r + 2, compY + r + 12),
                       QStringLiteral("罗盘: 灰=地图系 绿=局部+x轴"));
        }
    }

    // ---- 信息面板 ----
    p.setPen(QColor(50, 56, 64));
    f.setPointSizeF(9.5);
    p.setFont(f);
    QString line2;
    QString line3;
    QString line4;
    if (hasPose)
    {
        if (calibrated)
        {
            line2 = QStringLiteral("地图: x=%1m  y=%2m  朝向=%3° (0=东 顺时针)")
                        .arg(mapX, 0, 'f', 2)
                        .arg(mapY, 0, 'f', 2)
                        .arg(degNorm360(mapYawRad * kRadToDeg), 0, 'f', 1);
            // 局部叠加对比：局部值 = 相对参考点的局部位移（与地图 O 点同起点）
            line3 = QStringLiteral("局部(相对参考): x=%1m  y=%2m  yaw=%3°   数值差(地图-局部): dx=%4 dy=%5")
                        .arg(localX, 0, 'f', 2)
                        .arg(localY, 0, 'f', 2)
                        .arg(localYawRad * kRadToDeg, 0, 'f', 1)
                        .arg(mapX - localX, 0, 'f', 2)
                        .arg(mapY - localY, 0, 'f', 2);
            // 局部系 +x 轴对应的地图角（= 地图系与局部系的旋转差）
            line4 = QStringLiteral("局部系+x轴 地图角=%1°   机体系速度 vx=%2m/s   电量=%3%")
                        .arg(degNorm360((mapYawRad + localYawRad) * kRadToDeg), 0, 'f', 1)
                        .arg(bodyVx, 0, 'f', 2)
                        .arg(_sim && _terminalId > 0 && _sim->sessionOf(_terminalId)
                                 ? _sim->sessionOf(_terminalId)->lastState.batteryPercent
                                 : -1);
        }
        else
        {
            line2 = QStringLiteral("未标定（终端未启用地图坐标，显示原始局部坐标）");
            line4 = QStringLiteral("机体系速度 vx=%1m/s   电量=%2%")
                        .arg(bodyVx, 0, 'f', 2)
                        .arg(_sim && _terminalId > 0 && _sim->sessionOf(_terminalId)
                                 ? _sim->sessionOf(_terminalId)->lastState.batteryPercent
                                 : -1);
        }
    }
    int ty = oy - 26;
    p.drawText(QPointF(ox, ty), statusText);
    if (hasPose)
    {
        ty += 16;
        p.drawText(QPointF(ox, ty), line2);
        if (calibrated)
        {
            ty += 16;
            p.drawText(QPointF(ox, ty), line3);
        }
        ty += 16;
        p.drawText(QPointF(ox, ty), line4);
    }
}
