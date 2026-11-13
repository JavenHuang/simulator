#ifndef SIM_CALIB_PANEL_H
#define SIM_CALIB_PANEL_H

#include <QWidget>
#include <QMap>
#include <QJsonObject>
#include <QVector>

class QLabel;
class QTimer;
class SimController;

// ============================================================
// 服务端"标定与判定"常驻面板
//
// 数据来源：终端 calib.{id} 上的 JSON（robotdog.calib_panel，1Hz）。
// 显示方式：固定行、原地覆盖 —— 不追加、不滚动、不建日志列表。
//   ① 判定（verdict.code + head，按严重程度着色）
//   ② 下一步该干什么（verdict.action）
//   ③ 达标清单（10 项 ✓/✗，一眼看出卡在哪一步）
//   ④ 详细 6 行（差分/零位/标定/位置/融合/闸门），与终端 lines[2..7] 逐字一致
// 断流处理：超过 3s 没收到新面板，整块变灰并显示"数据停了 Ns"，
//           不把过期数据当成当前状态。
// ============================================================
class SimCalibPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SimCalibPanel(SimController *sim, QWidget *parent = nullptr);

    // 切换显示哪个终端（跟随地图窗口的端子下拉框）
    void setTerminalId(quint32 id);

public slots:
    // 终端来了一份新面板：只取自己关心的那个终端
    void onPanelUpdated(quint32 terminalId);

private slots:
    void onStaleTick();

private:
    void rebuild();
    void resetPlaceholder();

    SimController *_sim;
    quint32 _curId;

    QLabel *_title;      // 标题（终端 + 更新时间 + 帧号）
    QLabel *_verdict;    // 判定
    QLabel *_action;     // 下一步
    QLabel *_summary;    // 达标 7/10
    QVector<QLabel *> _checks;   // 10 项达标清单
    QVector<QLabel *> _rows;     // 6 行详细数据

    QMap<quint32, QJsonObject> _cache;    // 每终端最后一份面板
    QMap<quint32, qint64> _lastMs;        // 收到时的墙钟时间
    QTimer *_staleTimer;
};

#endif  // SIM_CALIB_PANEL_H
