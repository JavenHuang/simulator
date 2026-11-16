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

    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(6);
    lay->addWidget(_title);
    lay->addWidget(_verdict);
    lay->addWidget(_action);
    lay->addWidget(_summary);
    lay->addStretch(1);

    setMinimumWidth(280);
    setMaximumWidth(360);

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
    _title->setText(QStringLiteral("终端 %1 · 等待数据…").arg(_curId));
    setRowStyle(_title, QStringLiteral("#5b6570"), false);
    _verdict->setText(QStringLiteral("—"));
    setRowStyle(_verdict, QStringLiteral("#6b7280"), true);
    _action->setText(QStringLiteral("等待终端连接…"));
    _summary->setText(QString());
}

void SimCalibPanel::rebuild()
{
    const QJsonObject o = _cache.value(_curId);
    if (o.isEmpty())
    {
        resetPlaceholder();
        return;
    }

    const QJsonObject rtk = o.value(QStringLiteral("rtk")).toObject();
    const QJsonObject pose = o.value(QStringLiteral("pose")).toObject();

    const qint64 lastMs = _lastMs.value(_curId, 0);
    const qint64 ageMs = (lastMs > 0) ? (QDateTime::currentMSecsSinceEpoch() - lastMs) : 0;
    const bool stale = (ageMs > 3000);

    // 标题
    _title->setText(QStringLiteral("终端 %1 · %2%3")
                        .arg(_curId)
                        .arg(o.value(QStringLiteral("ts_iso")).toString())
                        .arg(stale ? QStringLiteral("　【数据停了 %1s】").arg(ageMs / 1000)
                                   : QStringLiteral("")));
    setRowStyle(_title, stale ? QStringLiteral("#b32d2d") : QStringLiteral("#5b6570"), false);

    // 差分状态
    const bool seen = rtk.value(QStringLiteral("seen")).toBool();
    const double rtkAgeMs = rtk.value(QStringLiteral("age_ms")).toDouble();
    _verdict->setText(seen ? QStringLiteral("差分在线 · 延迟 %1ms").arg(static_cast<qint64>(rtkAgeMs))
                           : QStringLiteral("差分断连"));
    setRowStyle(_verdict, seen ? QStringLiteral("#1a7f37") : QStringLiteral("#b32d2d"), true);

    // 位置/角度
    const bool hasMap = pose.value(QStringLiteral("has_map")).toBool();
    _action->setText(hasMap ? QStringLiteral("地图参考已建立")
                            : QStringLiteral("未建立地图参考（发 mapreset）"));
    setRowStyle(_action, hasMap ? QStringLiteral("#1a7f37") : QStringLiteral("#9a6700"), true);

    // 位置/角度详细（从 lines 取）
    const QJsonArray lines = o.value(QStringLiteral("lines")).toArray();
    QString posLine;
    for (const QJsonValue &v : lines)
    {
        const QString s = v.toString();
        if (s.contains(QStringLiteral("地图")) || s.contains(QStringLiteral("ENU")))
        {
            posLine = s;
            break;
        }
    }
    _summary->setText(posLine);
}
