#ifndef SIM_SESSION_H
#define SIM_SESSION_H

#include <QtGlobal>

#include "Protocol/DogProtocol.h"

// ============================================================
// 单个被控终端在本模拟器里的会话状态
//
// 只放数据、不放行为：上行报文写 lastState / reportCount，
// 下行报文写 taskInFlight / currentTaskId，自动巡航写 wpIndex。
// 谁改谁负责，避免"状态存在谁手里、谁能改"说不清。
// ============================================================
struct TermSession
{
    DogProtocol::DogState lastState;
    bool hasState = false;       // 是否收到过状态上报（相对移动依赖它）
    int reportCount = 0;         // 状态包计数（用于节流打印）
    bool taskInFlight = false;   // 当前有任务未完结
    quint32 currentTaskId = 0;
    int wpIndex = 0;             // 下一个航点索引
};

#endif  // SIM_SESSION_H
