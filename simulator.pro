# ============================================================
# 服务器联调模拟器（控制台 + 地图显示窗口）
#
# 工程布局：与终端工程同级
#   D:/Work/KDISS/DogTerminal   终端工程（本工程只借用它的协议/MQ 源码）
#   D:/Work/KDISS/simulator     本工程（自有 Bin 与 config，互不干扰）
#
# 借用（只读，不复制）：DogTerminal/src/Protocol、src/ServerLink/MqLink、
#                       DogTerminal/thirdparty/RabbitMq
# 自有：src/ 本工程源码、config/ 本工程配置、Bin/ 本工程产物
#
# 构建: qmake && make -j4
# 产物: Bin/simulator（config 随构建自动复制到 Bin/config）
# 运行: cd Bin && ./simulator --config config/simulator.ini
#       --nogui 可关闭地图窗口
# ============================================================
QT += widgets network

TEMPLATE = app
CONFIG += console c++14
CONFIG -= app_bundle

TARGET = simulator

# 终端工程根目录（同级），只用于引用共享的协议与 MQ 源码
TERM_ROOT = $$PWD/../DogTerminal
exists($$TERM_ROOT/src/Protocol/DogProtocol.cpp) {
    # 共享源码齐全
} else {
    error("未找到同级终端工程：$$TERM_ROOT（请确认 D:/Work/KDISS/DogTerminal 存在）")
}

# ---------- 产物输出目录（本工程独占，不再与终端共用） ----------
# Release 输出到 Bin，Debug 输出到 Bin/debug（禁止混放，原因同终端工程）
BIN_DIR = $$PWD/Bin
CONFIG(debug, debug|release) {
    OUT_DIR = $$BIN_DIR/debug
} else {
    OUT_DIR = $$BIN_DIR/release
}
DESTDIR = $$OUT_DIR

# ---------- 构建时创建输出目录并同步本工程的 config ----------
# qmake 不会为 DESTDIR 自动建目录，需在链接前创建
win32 {
    QMAKE_PRE_LINK += if not exist $$shell_quote($$shell_path($$OUT_DIR)) $$QMAKE_MKDIR $$shell_quote($$shell_path($$OUT_DIR))
    # xcopy /i 自动创建 config 目录并覆盖同名文件
    QMAKE_POST_LINK += $$QMAKE_COPY_DIR $$shell_quote($$shell_path($$PWD/config)) $$shell_quote($$shell_path($$OUT_DIR/config))
} else {
    QMAKE_PRE_LINK += $$QMAKE_MKDIR $$shell_quote($$shell_path($$OUT_DIR))
    QMAKE_POST_LINK += $$QMAKE_MKDIR $$shell_quote($$shell_path($$OUT_DIR/config)) $$escape_expand(\\n\\t)
    # 复制目录内容（config/. 避免生成 config/config 嵌套）
    QMAKE_POST_LINK += $$QMAKE_COPY_DIR $$shell_quote($$shell_path($$PWD/config/.)) $$shell_quote($$shell_path($$OUT_DIR/config))
}

INCLUDEPATH += $$PWD $$PWD/src $$TERM_ROOT $$TERM_ROOT/src
DEPENDPATH += $$PWD $$PWD/src $$TERM_ROOT $$TERM_ROOT/src

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    src/main.cpp \
    src/SimController.cpp \
    src/SimControllerCommands.cpp \
    src/SimDownlink.cpp \
    src/SimUplink.cpp \
    src/MapWindow.cpp \
    src/MapCanvas.cpp \
    src/SimCalibPanel.cpp \
    $$TERM_ROOT/src/Protocol/DogProtocol.cpp \
    $$TERM_ROOT/src/ServerLink/MqLink.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpauthenticator.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpchannel.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpchannelhash.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpclient.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpexchange.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpframe.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpmessage.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpqueue.cpp \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqptable.cpp

HEADERS += \
    src/SimController.h \
    src/SimSession.h \
    src/SimDownlink.h \
    src/SimUplink.h \
    src/MapWindow.h \
    src/MapCanvas.h \
    src/SimCalibPanel.h \
    $$TERM_ROOT/src/Protocol/DogProtocol.h \
    $$TERM_ROOT/src/ServerLink/MqLink.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpauthenticator.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpchannel.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpchannel_p.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpchannelhash_p.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpclient.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpclient_p.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpexchange.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpexchange_p.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpframe_p.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpglobal.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpmessage.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpmessage_p.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpqueue.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqpqueue_p.h \
    $$TERM_ROOT/thirdparty/RabbitMq/qamqptable.h
