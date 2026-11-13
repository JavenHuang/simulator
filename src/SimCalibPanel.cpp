#include "SimCalibPanel.h"
#include "SimController.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace
{

// 判定码 → 颜色（绿=可以往下走，黄=在正确步骤上但还没到位，红=出错必须先处理，灰=开关没开）
QString verdictColor(const QString &code)
{
    if (code == QLatin1String("READY"))
        return QStringLiteral("#1a7f37");
    if (code == QLatin1String("RTK_STALE") || code == QLatin1String("DPSI_CHECK")
        || code == QLatin1String("ANCHOR_MISS") || code == QLatin1String("SOLVE_FAIL")
        || code == QLatin1String("REDO_LINE") || code == QLatin1String("NO_YAW"))
        return QStringLiteral("#b32d2d");
    if (code == QLatin1String("RTK_OFF") || code == QLatin1String("BASELINE")
        || code == QLatin1String("CALIB_OFF"))
        return QStringLiteral("#6b7280");
    return QStringLiteral("#9a6700");
}

QLabel *makeRow(QWidget *parent, bool mono, int pt)
{
    QLabel *l = new QLabel(parent);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(true);
    l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    if (mono)
        l->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    else
    {
        QFont f = l->font();
        f.setPointSize(pt);
        l->setFont(f);
    }
    return l;
}

void setRowStyle(QLabel *l, const QString &color, bool bold)
{
    QString css = QStringLiteral("color:%1;").arg(color);
    if (bold)
        css += QStringLiteral("font-weight:600;");
    l->setStyleSheet(css);
}

}  // namespace

SimCalibPanel::SimCalibPanel(SimController *sim, QWidget *parent)
    : QWidget(parent)
    , _sim(sim)
    , _curId(0)
    , _title(nullptr)
    , _verdict(nullptr)
    , _action(nullptr)
    , _summary(nullptr)
    , _staleTimer(nullptr)
{
    _title = makeRow(this, false, 10);
    setRowStyle(_title, QStringLiteral("#5b6570"), false);

    _verdict = makeRow(this, false, 12);
    _verdict->setWordWrap(true);

    _action = makeRow(this, false, 11);
    setRowStyle(_action, QStringLiteral("#1f2328"), true);

    _summary = makeRow(this, false, 11);
    setRowStyle(_summary, QStringLiteral("#1f2328"), true);

    // 达标清单：2 列 x 5 行，固定位置，原地改字
    QGridLayout *grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);
    for (int i = 0; i < 10; ++i)
    {
        QLabel *c = makeRow(this, false, 10);
        _checks.append(c);
        grid->addWidget(c, i / 2, i % 2);
    }

    QFrame *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);

    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(6);
    lay->addWidget(_title);
    lay->addWidget(_verdict);
    lay->addWidget(_action);
    lay->addWidget(line);
    lay->addWidget(_summary);
    lay->addLayout(grid);
    lay->addSpacing(2);

    // 详细 6 行（与终端 lines[2..7] 逐字一致）
    for (int i = 0; i < 6; ++i)
    {
        QLabel *r = makeRow(this, true, 9);
        _rows.append(r);
        lay->addWidget(r);
    }
    lay->addStretch(1);

    setMinimumWidth(560);
    setMaximumWidth(700);

    // 独立底色 + 细边框：和左侧地图分辨开，看起来是"一块固定面板"而不是日志区
    setObjectName(QStringLiteral("simCalibPanel"));
    setStyleSheet(QStringLiteral("#simCalibPanel { background:#fbfcfd; border:1px solid #d3dbe2;"
                                 " border-radius:8px; }"));

    resetPlaceholder();

    _staleTimer = new QTimer(this);
    _staleTimer->setInterval(1000);
    connect(_staleTimer, &QTimer::timeout, this, &SimCalibPanel::onStaleTick);
    _staleTimer->start();
}

void SimCalibPanel::setTerminalId(quint32 id)
{
    if (_curId == id)
        return;
    _curId = id;
    rebuild();
}

void SimCalibPanel::onPanelUpdated(quint32 terminalId)
{
    const QJsonObject obj = _sim ? _sim->calibPanelOf(terminalId) : QJsonObject();
    if (obj.isEmpty())
        return;

    _cache.insert(terminalId, obj);
    _lastMs.insert(terminalId, QDateTime::currentMSecsSinceEpoch());
    if (_curId == 0)
        _curId = terminalId;   // 还没指定终端时，谁先来就显示谁
    if (terminalId == _curId)
        rebuild();
}

void SimCalibPanel::onStaleTick()
{
    // 每秒只做一件事：如果缓存里已有面板，就重算一次"停了多久"。
    // 没消息时不刷新文字，避免无谓重绘。
    if (!_cache.isEmpty())
        rebuild();
}

void SimCalibPanel::resetPlaceholder()
{
    _title->setText(QStringLiteral("终端 %1 · 等待 calib 面板（1Hz）…").arg(_curId));
    setRowStyle(_title, QStringLiteral("#5b6570"), false);
    _verdict->setText(QStringLiteral("—"));
    setRowStyle(_verdict, QStringLiteral("#6b7280"), true);
    _action->setText(QStringLiteral("还没收到该终端的面板消息：确认终端已启动、"
                                    "终端日志出现 [Panel] 标定面板已配置，且 MQ 通"));

    _summary->setText(QStringLiteral("达标清单：等待数据"));
    for (int i = 0; i < _checks.size(); ++i)
    {
        _checks[i]->setText(QStringLiteral("· …"));
        setRowStyle(_checks[i], QStringLiteral("#9aa3ad"), false);
    }
    for (int i = 0; i < _rows.size(); ++i)
        _rows[i]->setText(QString());
}

void SimCalibPanel::rebuild()
{
    const QJsonObject o = _cache.value(_curId);
    if (o.isEmpty())
    {
        resetPlaceholder();
        return;
    }

    const QJsonObject v = o.value(QStringLiteral("verdict")).toObject();
    const QJsonObject rtk = o.value(QStringLiteral("rtk")).toObject();
    const QJsonObject zero = o.value(QStringLiteral("zero")).toObject();
    const QJsonObject calib = o.value(QStringLiteral("calib")).toObject();
    const QJsonObject pose = o.value(QStringLiteral("pose")).toObject();

    const qint64 lastMs = _lastMs.value(_curId, 0);
    const qint64 ageMs = (lastMs > 0) ? (QDateTime::currentMSecsSinceEpoch() - lastMs) : 0;
    const bool stale = (ageMs > 3000);

    // ---- 标题行 ----
    QString title = QStringLiteral("终端 %1 · 标定面板 · 第 %2 帧 · %3")
                        .arg(_curId)
                        .arg(static_cast<qint64>(o.value(QStringLiteral("seq")).toDouble()))
                        .arg(o.value(QStringLiteral("ts_iso")).toString());
    if (stale)
        title += QStringLiteral("　【数据停了 %1s，以下是最后一次，可能已过期】")
                     .arg(ageMs / 1000);
    _title->setText(title);
    setRowStyle(_title, stale ? QStringLiteral("#b32d2d") : QStringLiteral("#5b6570"), false);

    // ---- 判定 ----
    const QString code = v.value(QStringLiteral("code")).toString();
    const QString head = v.value(QStringLiteral("head")).toString();
    _verdict->setText(QStringLiteral("%1　%2").arg(code).arg(head));
    setRowStyle(_verdict, verdictColor(code), true);

    // ---- 下一步 ----
    _action->setText(QStringLiteral("下一步：%1").arg(v.value(QStringLiteral("action")).toString()));

    // ---- 达标清单（服务端按同一份数据独立复算，不看结论只看得分项）----
    const bool seen = rtk.value(QStringLiteral("seen")).toBool();
    const double rtkAgeMs = rtk.value(QStringLiteral("age_ms")).toDouble();
    const double holdMs = rtk.value(QStringLiteral("hold_ms")).toDouble(1500.0);
    const bool bValid = zero.value(QStringLiteral("valid")).toBool();
    const QString bSrc = zero.value(QStringLiteral("source")).toString();
    const double bDeg = zero.value(QStringLiteral("b_deg")).toDouble();
    const bool cValid = calib.value(QStringLiteral("valid")).toBool();
    const int anchors = static_cast<int>(calib.value(QStringLiteral("anchors")).toDouble());
    const int minSamples = static_cast<int>(calib.value(QStringLiteral("min_samples")).toDouble());
    const double pathM = calib.value(QStringLiteral("path_m")).toDouble();
    const double minPathM = calib.value(QStringLiteral("min_path_m")).toDouble();
    const double rmsM = calib.value(QStringLiteral("rms_m")).toDouble();
    const double maxRmsM = calib.value(QStringLiteral("max_rms_m")).toDouble();
    const double dpsi = pose.value(QStringLiteral("dpsi_deg")).toDouble();
    const bool dpsiValid = pose.value(QStringLiteral("dpsi_valid")).toBool();
    const double staticSec = pose.value(QStringLiteral("static_sec")).toDouble();
    const double driftDps = pose.value(QStringLiteral("static_yaw_drift_dps")).toDouble();
    const bool hasMap = pose.value(QStringLiteral("has_map")).toBool();
    const double fusedVsRtk = pose.value(QStringLiteral("fused_vs_rtk_m")).toDouble();
    const bool useForPose = pose.value(QStringLiteral("use_for_pose")).toBool();

    int passCount = 0;
    const int total = 10;
    int idx = 0;
    auto mark = [&](bool ok, const QString &text, bool na = false)
    {
        if (idx >= _checks.size())
            return;
        if (!na && ok)
            ++passCount;
        const QString prefix = na ? QStringLiteral("– ")
                                  : (ok ? QStringLiteral("✓ ") : QStringLiteral("✗ "));
        _checks[idx]->setText(prefix + text);
        setRowStyle(_checks[idx],
                    na ? QStringLiteral("#9aa3ad")
                       : (ok ? QStringLiteral("#1a7f37") : QStringLiteral("#b32d2d")),
                    false);
        ++idx;
    };

    mark(seen && rtkAgeMs <= holdMs,
         QStringLiteral("差分在流 %1ms").arg(static_cast<qint64>(rtkAgeMs)));
    mark(bValid,
         bValid ? QStringLiteral("b 已取 %1°（%2）")
                      .arg(bDeg, 0, 'f', 1)
                      .arg(bSrc == QLatin1String("line") ? QStringLiteral("直线")
                                                         : QStringLiteral("参考"))
                : QStringLiteral("b 未取"));
    mark(cValid, cValid ? QStringLiteral("定标已解 k=%1")
                                    .arg(calib.value(QStringLiteral("k")).toDouble(), 0, 'f', 3)
                          : QStringLiteral("定标未解"));
    mark(anchors >= minSamples,
         QStringLiteral("锚点 %1/%2").arg(anchors).arg(minSamples));
    mark(pathM >= minPathM,
         QStringLiteral("直线里程 %1/%2m").arg(pathM, 0, 'f', 2).arg(minPathM, 0, 'f', 2));
    mark(cValid && rmsM <= maxRmsM,
         QStringLiteral("RMS %1/%2m").arg(rmsM, 0, 'f', 3).arg(maxRmsM, 0, 'f', 2));
    mark(!dpsiValid || dpsi <= 10.0,
         dpsiValid ? QStringLiteral("Δψ %1° ≤10°").arg(dpsi, 0, 'f', 1)
                   : QStringLiteral("Δψ 暂不可测"),
         !dpsiValid);
    mark(staticSec >= 20.0 && std::fabs(driftDps) <= 0.15,
         staticSec >= 20.0 ? QStringLiteral("静止漂 %1°/s").arg(driftDps, 0, 'f', 2)
                           : QStringLiteral("静止漂 待测（需静止20s）"),
         staticSec < 20.0);
    mark(hasMap && fusedVsRtk <= 0.5,
         hasMap ? QStringLiteral("融合贴住 %1m").arg(fusedVsRtk, 0, 'f', 2)
                : QStringLiteral("融合 待测（无地图参考）"),
         !hasMap);
    mark(useForPose,
         useForPose ? QStringLiteral("上报走融合")
                    : QStringLiteral("上报走里程计(基线)"));

    _summary->setText(QStringLiteral("达标 %1/%2　（✓ 已达标　✗ 未达标　– 待测/不适用）")
                          .arg(passCount).arg(total));
    setRowStyle(_summary, passCount == total ? QStringLiteral("#1a7f37") : QStringLiteral("#1f2328"), true);

    // ---- 详细 6 行：直接用终端发来的 lines[2..7] ----
    const QJsonArray lines = o.value(QStringLiteral("lines")).toArray();
    const QString gate = v.value(QStringLiteral("gate")).toString();
    for (int i = 0; i < _rows.size(); ++i)
    {
        const int srcIdx = i + 2;
        QString text;
        if (srcIdx < lines.size())
            text = lines.at(srcIdx).toString();
        if (text.isEmpty())
        {
            _rows[i]->setText(QString());
            continue;
        }
        _rows[i]->setText(text);
        const bool isGateRow = (i == _rows.size() - 1);
        setRowStyle(_rows[i],
                    (isGateRow && gate != QStringLiteral("无")) ? QStringLiteral("#9a6700")
                                                                : QStringLiteral("#202124"),
                    false);
    }
}
