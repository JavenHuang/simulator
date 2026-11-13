# simulator

与 DogTerminal 配套的联调模拟器，充当任务服务器一侧：控制台运行、带地图窗口。Windows + Qt 5.12（MinGW）构建。

## 依赖终端工程源码（只读，不复制）

`simulator.pro` 中 `TERM_ROOT = $$PWD/../DogTerminal`，借用：

- `DogTerminal/src/Protocol`（协议编解码）
- `DogTerminal/src/ServerLink/MqLink`（RabbitMQ 链路封装）
- `DogTerminal/thirdparty/RabbitMq`（QAMQP 移植版）

因此两个仓库必须克隆到同一父目录，且目录名分别为 `DogTerminal` 与 `simulator`；否则 qmake 会报“未找到同级终端工程”。

## 组成

- `src/`：本工程源码（会话、上行上报、下行指令、地图窗口、标定面板）。
- `config/simulator.ini`：本工程自有配置，与终端各自独立。

## 构建与运行

```cmd
qmake simulator.pro
mingw32-make -j4
cd Bin\release && simulator.exe --config config\simulator.ini
```

`--nogui` 可关闭地图窗口，只跑控制台。
