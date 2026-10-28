# MCF8315D 配置参考

本文是 `include/config_opt.h` 里那张配置表的**逐项说明书**：每个寄存器放了什么、为什么放这个值、
哪些字段留在了默认值、以及哪些复位值其实是"坑"。

> 器件手册：**SLLSG06**。本文只写本工程用到的部分，字段的完整枚举请查手册第 7 章。

---

## 0. 为什么要把配置"显式写满"

MCF8315D 的影子寄存器复位值基本都是 `0x00000000`，但**复位值 ≠ 能用的值**。举几个本项目踩过的：

| 字段 | 复位值 | 实际需要 | 不配的后果 |
|---|---|---|---|
| `FG_DIV` | `0h` = 3× 电频率 | `1h` | 转速读数差 3 倍 |
| `ALIGN_TIME` | `10 ms` | `1 s` | 对齐电流还没建立就进开环 → 一启动就打滑 |
| `SLEEP_ENTRY_TIME` | `50 µs` | `200 ms` | PWM 最低占空比的关断时间被判成休眠 |
| `LOCK1/2/3_EN` | 全部关闭 | 全部开启 | 电机真卡住时没有任何堵转保护 |
| `MIN_VM_MOTOR` / `MAX_VM_MOTOR` | 无限制 | 6 V / 34 V | 母线异常时不会报故障 |
| `MOTOR_BEMF_CONST` | `0` | 标定值 | 器件每次给速度都先跑一遍 MPET |

所以本工程的做法是：**固件里带一整张表，上电无条件下发**。这样即使芯片是全新的、EEPROM 是空的，
接上 MCU 就是确定的行为，不依赖"出厂默认值恰好合适"。

---

## 1. 影子寄存器总表

`bit31` 是 PARITY 位，由器件维护，写入时一律**读-改-写**保留原值。

| 地址 | 名称 | 复位值 | 本项目值 | 本工程配置 |
|---|---|---|---|---|
| 0x080 | ISD_CONFIG | 0x00000000 | 0xC4B34C20 | ✅ |
| 0x082 | REV_DRIVE_CONFIG | 0x00000000 | 0x28310010 | ✅（只写主动制动参数）|
| 0x084 | MOTOR_STARTUP1 | 0x00000000 | 0x8D100F92 | ✅ |
| 0x086 | MOTOR_STARTUP2 | 0x00000000 | 0xD501C014 | ✅ |
| 0x088 | CLOSED_LOOP1 | 0x00000000 | 0x1C6481B8 | ✅ |
| 0x08A | CLOSED_LOOP2 | 0x00000000 | 0x16007402 | ✅ |
| 0x08C | CLOSED_LOOP3 | 0x00000000 | 0x05000000 | ✅ |
| 0x08E | CLOSED_LOOP4 | 0x00000000 | 0x01004E10 | ✅ |
| 0x090 | FAULT_CONFIG1 | 0x00000000 | 0xD6EDC09E | ✅ |
| 0x092 | FAULT_CONFIG2 | 0x00000000 | 0x754C49F8 | ✅ |
| 0x094 | SPEED_PROFILES1 | 0x00000000 | 0x00000000 | ❌（不用参考曲线）|
| 0x096 | SPEED_PROFILES2 | 0x00000000 | 0x00000000 | ❌ |
| 0x098 | SPEED_PROFILES3 | 0x00000000 | 0x80000002 | ❌ |
| 0x09A | SPEED_PROFILES4 | 0x00000000 | 0x80068000 | ❌ |
| 0x09C | SPEED_PROFILES5 | 0x00000000 | 0x80000010 | ❌ |
| 0x09E | SPEED_PROFILES6 | 0x00000000 | 0x00000000 | ❌ |
| 0x0A0 | INT_ALGO_1 | 0x00000000 | 0x89750005 | ✅ |
| 0x0A2 | INT_ALGO_2 | 0x00000000 | 0x800001A3 | ✅（只写制动取流斜率）|
| 0x0A4 | PIN_CONFIG | 0x00000000 | 0x08200309 | ✅ |
| 0x0A6 | DEVICE_CONFIG1 | 0x00000000 | 0x80100000 | ✅ |
| 0x0A8 | DEVICE_CONFIG2 | 0x00000000 | 0x03E8E200 | ✅ |
| 0x0AA | PERI_CONFIG1 | 0x00000000 | 0x83083C00 | ✅ |
| 0x0AC | GD_CONFIG1 | 0x00000000 | 0x1C4E1900 | ✅ |
| 0x0AE | GD_CONFIG2 | 0x00000000 | 0x81840000 | ✅ |

> 表中"本项目值"是台架定稿时从器件读回的实际值，PARITY 位会随内容变化。
> 标 ❌ 的寄存器是器件里已有的值，本工程不写入 —— 它们要么与本功能无关（反向驱动/参考曲线），
> 要么属于高级调试项。**换一台电机重新标定时，建议先用 TI GUI 或 MPET 生成一版基线。**

---

## 2. 逐寄存器详解

### 2.1 ISD_CONFIG（0x080）—— 启动前置检测

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 30 | ISD_EN | **1** | 初始速度检测：启动前用三相电压测出转速/角度/方向 |
| 29 | BRAKE_EN | 0 | ISD 前的制动例行程序（会把转子拖住，本项目不需要）|
| 28 | HIZ_EN | 0 | ISD 前先让桥臂 Hi-Z 一段时间 |
| 27 | RVS_DR_EN | 0 | 反向驱动 |
| 26 | RESYNC_EN | **1** | 重同步：转子还在转时直接接上，不重走启动流程 |
| 25-22 | FW_DRV_RESYN_THR | 2（15%）| 高于此速度才允许直接进闭环，低于则走开环 |
| 21 | BRK_MODE | 0 | 制动方式：三高侧导通 |
| 19-17 | BRK_CURR_THR | 0 | 制动电流阈值（BRAKE_EN=0 时不生效）|
| 16-13 | BRK_TIME | 0 | 制动时间 10 ms |
| 12-9 | HIZ_TIME | 0 | Hi-Z 时间 10 ms |
| 8-6 | STAT_DETECT_THR | 0 | 判定"静止"的反电动势阈值 **50 mV** |

> **ISD + RESYNC 是必须开的**。关掉之后"运行中降速 → 再给速度"会出现明显顿挫：
> 器件会把还在滑行的转子当成静止，重新走一遍对齐+开环。

### 2.2 MOTOR_STARTUP1（0x084）—— 对齐

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 30-29 | MTR_STARTUP | 0 | 启动方式：**单次对齐**（Double Align 实测更差）|
| 28-25 | ALIGN_SLOW_RAMP_RATE | 6h | 电流上升斜率 50 A/s |
| 24-21 | ALIGN_TIME | **8h = 1 s** | 对齐保持时间。复位值只有 10 ms，**必须改** |
| 20-17 | ALIGN_OR_SLOW_CURRENT_ILIMIT | 8h = 2.187 A | 对齐电流。低了转子拉不到位，开环必打滑 |
| 16-9 | IPD_CLK_FREQ / IPD_CURR_THR | — | 本项目不用 IPD |
| 1 | ACTIVE_BRAKE_EN | **1** | 主动制动：减速时从母线取电烧在绕组里，快且不回灌 |

### 2.3 MOTOR_STARTUP2（0x086）—— 开环加速

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 30-27 | OL_ILIMIT | Ah = **2.812 A** | 开环电流上限。再高纹波峰值会顶到 3.75A 的 HW_LOCK |
| 26-23 | OL_ACC_A1 | Ah = **250 Hz/s** | 开环加速。**快比慢稳**：低速区观测器最弱，要快速穿过 |
| 22-19 | OL_ACC_A2 | 0 | 加速度系数（A1 的二次项），本项目不用 |
| 18 | AUTO_HANDOFF_EN | 0 | 用固定阈值换相，而不是自动判断 |
| 17-13 | OPN_CL_HANDOFF_THR | 14（15%）| 开环→闭环的换相阈值 = 15% × MAX_SPEED ≈ 90 Hz |
| 7-4 | SLOW_FIRST_CYC_FREQ | 1h | 开环第一周期频率 0.3% |
| 3 | FIRST_CYCLE_FREQ_SEL | 0 | 第一周期用 0 Hz |
| 2-0 | THETA_ERROR_RAMP_RATE | 0 | 估计角度向开环角度收敛的速率 |

### 2.4 CLOSED_LOOP1（0x088）—— 环路主参数

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 30 | OVERMODULATION_ENABLE | 0 | 过调制：能再榨一点转速，代价是谐波和噪音 |
| 29-25 | CL_ACC | Eh = 500 Hz/s | 闭环加速斜率 |
| 23-19 | CL_DEC | **Ch = 300 Hz/s** | 减速斜率**限幅**。设小了主动制动再猛也没用（5Hz/s 时 50%→10% 要 36 秒）|
| 18-15 | PWM_FREQ_OUT | **9h = 55 kHz** | 相线载波。本电机 **50 kHz 是硬下限** |
| 14 | PWM_MODE | 0 | 连续 SVPWM |
| 13-12 | FG_SEL | 0 | ISD / 开环 / 闭环全程输出 FG |
| 11-8 | **FG_DIV** | **1h** | 每个电周期 1 个脉冲 → 7 对极 = **7 脉冲/机械转**。复位值 0h 是 3× 电频率 |
| 7 | FG_CONFIG | 0 | 驱动期间持续输出 FG |
| 6-4 | FG_BEMF_THR | 0 | FG_CONFIG=1 时才用 |
| 3 | **AVS_EN** | **1** | 防电压浪涌：把负 Iq 钳到 0，减速不回灌母线 |

### 2.5 CLOSED_LOOP2（0x08A）—— 停机方式与电机参数

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 30-28 | MTR_STOP | 1h | **续流**停机：电感能量在 MOS 里循环掉，不往母线回灌 |
| 27-24 | MTR_STOP_BRK_TIME | 6h = 10 ms | 制动时间（MTR_STOP=1 时基本无感）|
| 23-20 | ACT_SPIN_THR | 0 | 主动减速阈值（MTR_STOP=4 时才用）|
| 19-16 | BRAKE_SPEED_THRESHOLD | 0 | BRAKE 引脚/刹车模式的速度阈值 |
| 15-8 | MOTOR_RES | **0x74** ≈ 0.43 Ω | 相电阻码（表 6-2）|
| 7-0 | MOTOR_IND | **0x02** | 相电感码（表 6-3）。0x01~0x06 = "由 MPET 测量" |

### 2.6 CLOSED_LOOP3 / CLOSED_LOOP4（0x08C / 0x08E）—— 环路增益与限速

| 寄存器.位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 0x08C b30-23 | MOTOR_BEMF_CONST | **0x0A** = 1.5 mV/Hz | 反电动势常数（标定值，不是 MPET 值）|
| 0x08C b22-13 | CURR_LOOP_KP | **0** | **0 = 由器件按 R/L 自动计算** |
| 0x08C b12-3 | CURR_LOOP_KI | **0** | 同上 |
| 0x08C b2-0 | SPD_LOOP_KP 高 3 位 | 0 | |
| 0x08E b30-24 | SPD_LOOP_KP 低 7 位 | 1 | Kp = 0.01 × 码值 |
| 0x08E b23-14 | SPD_LOOP_KI | 1 | Ki = 0.1 × 码值 |
| 0x08E b13-0 | **MAX_SPEED** | **3600** | 最高电频率 = 码值/6 = **600 Hz** ≈ 5143 RPM |

> 速度环 Kp/Ki 是这台电机上能稳定的最小值；再大就会在低速段振荡。

### 2.7 FAULT_CONFIG1（0x090）—— 电流限值

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 30-27 | ILIMIT | Ah = **2.812 A** | 闭环运行电流限值 |
| 26-23 | HW_LOCK_ILIMIT | Dh = **3.75 A** | 硬件比较器阈值（4 位档位上限，已超器件额定 3.5A）|
| 22-19 | LOCK_ILIMIT | Dh = 3.75 A | 软件堵转电流阈值 |
| 18 | **EEP_FAULT_MODE** | **1** | 1 = EEPROM 校验失败只上报，不锁死 FET |
| 14-11 | LOCK_ILIMIT_DEG | 8h = 10 ms | 堵转去抖 |

> 六个阶段各有一个限流参数，漏配任何一段那一段就没人管：
> 对齐（0x084）、开环（0x086）、闭环（0x090）、堵转（0x090）、IPD（0x084）。

### 2.8 FAULT_CONFIG2（0x092）—— 堵转与母线保护

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 30 | LOCK1_EN | **1** | 异常转速堵转检测 |
| 29 | LOCK2_EN | **1** | 反电动势异常堵转检测 |
| 28 | LOCK3_EN | **1** | 无电机（电流过小）检测 |
| 27-25 | LOCK_ABN_SPEED | 2h = 150% | 高于 1.5× 目标速度判堵转 |
| 24-22 | ABNORMAL_BEMF_THR | 5h = 65% | 实际 BEMF 低于期望值 65% 判异常 |
| 21-19 | NO_MTR_THR | 1h = 0.046 A | 电流低于此值判"没接电机" |
| 18-16 | HW_LOCK_ILIMIT_MODE | 4h | 硬件限流后：自动重试 + 低侧制动 |
| 15-13 | HW_LOCK_ILIMIT_DEG | 2h = 2 µs | 比较器去抖 |
| 12-11 | VOLTAGE_HYSTERESIS | 1h = 1 V | 欠压/过压恢复回差 |
| 10-8 | MIN_VM_MOTOR | 1h = **6 V** | 运行最低母线电压（9V 供电留余量）|
| 7 | MIN_VM_MODE | 1 | 电压恢复后自动清除欠压故障 |
| 6-4 | MAX_VM_MOTOR | 7h = 34 V | 运行最高母线电压 |
| 3 | MAX_VM_MODE | 1 | 过压自动清除 |
| 2-0 | AUTO_RETRY_TIMES | **0 = 不限** | 器件自动重试次数；固件层另有 3 次上限 |

### 2.9 INT_ALGO_1（0x0A0）—— ISD 时间常数

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 28-27 | SPEED_PIN_GLITCH_FILTER | 1h = 0.2 µs | SPEED 输入毛刺滤波 |
| 26 | FAST_ISD_EN | 0 | 快速 ISD |
| 25-24 | ISD_STOP_TIME | 1h = 5 ms | 判定"停转"的保持时间 |
| 23-22 | ISD_RUN_TIME | 1h = 5 ms | 判定"在转"的保持时间 |
| 21-20 | ISD_TIMEOUT | 3h = 2000 ms | ISD 测不出时的超时 |
| 19-17 | AUTO_HANDOFF_MIN_BEMF | 2h | 自动换相才用（本项目关闭）|

### 2.10 PIN_CONFIG（0x0A4）—— 引脚功能

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 26-22 | LEAD_ANGLE | 0 | 换相超前角 0° |
| 10-9 | FG_IDLE_CONFIG | 1 | 停机时 FG 拉高 |
| 8-7 | FG_FAULT_CONFIG | 2 | 故障时 FG 拉低 |
| 5 | BRAKE_PIN_MODE | 0 | 低侧制动 |
| 1-0 | **SPEED_MODE** | **1h** | **PWM 调速**（0=模拟 1=PWM 3=频率）|

### 2.14 主动制动 Active Braking（跨 4 个寄存器）

减速时若只靠 AVS，器件会把负 Iq 钳到 0——完全不刹车，只能等风叶自己滑下来。
主动制动解决这个：**主动从母线取电，连同转子动能一起烧在绕组里**，
所以减速快，而且不会把能量泵回电源（代价就是热）。

| 寄存器.位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 0x084 b1 | ACTIVE_BRAKE_EN | 1 | 总开关 |
| 0x082 b22-20 | ACTIVE_BRAKE_CURRENT_LIMIT | 3h = 1.875 A | 制动时从母线取流的上限 |
| 0x082 b19-10 | ACTIVE_BRAKE_KP | 0x040 = 0.5 | 制动电流环 Kp = 码值/2^7 |
| 0x082 b9-0 | ACTIVE_BRAKE_KI | 0x010 = 0.03125 | Ki = 码值/2^9 |
| 0x0AA b16-13 | SPEED_DELTA_LIMIT_ENTRY | 1h = 5% | 速度差大于此值才进主动制动，否则走 AVS |
| 0x0AA b12-10 | ACTIVE_BRAKE_MOD_INDEX_LIMIT | 7h = 100% | 调制指数低于此值才允许制动；有 VM 尖峰就往下调 |
| 0x0A0 b30-29 | SPEED_DELTA_LIMIT_EXIT | 0h = 2.5% | 距目标小于此值退出制动，交给 AVS 收尾 |
| 0x0A2 b5-3 | BUS_CURRENT_SLEW_RATE | 4h = 500 A/s | 取流从 0 爬到上限的斜率 |
| 0x0AA b21 | BUS_POWER_LIMIT_ENABLE | 0 | **=1 时主动制动被禁用** |

**必须同时把 `CL_DEC` 设大**（本项目 Ch = 300 Hz/s）。`CL_DEC` 是减速参考值的斜坡限幅，
设成 5 Hz/s 时主动制动再猛也发挥不出来：实测 50%→10% 要 36 秒，改成 300 Hz/s 后只要 **2 秒**。

实测（9V 母线、无大电容）：100%→20% 约 4 秒、50%→10% 约 2 秒，全程无过压故障。

---

### 2.11 DEVICE_CONFIG1 / DEVICE_CONFIG2（0x0A6 / 0x0A8）

| 寄存器.位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 0xA6 b26 | PWM_DITHER_MODE | 0 | 三角波抖动 |
| 0xA6 b25-20 | I2C_TARGET_ADDR | 1 | 器件 I2C 地址 0x01（本项目不写，靠扫描）|
| 0xA6 b2 | PULLUP_ENABLE | 0 | 外部已上拉则不打开内部上拉 |
| 0xA6 b1-0 | BUS_VOLT | 0 | 母线电压档 15 V（9V 供电）|
| 0xA8 b15-14 | SLEEP_ENTRY_TIME | 3h = **200 ms** | SPEED 持续低电平多久进休眠 |
| 0xA8 b13 | DYNAMIC_CSA_GAIN_EN | 1 | 电流采样增益自适应 |
| 0xA8 b11 | DEV_MODE | 0 | Standby（Sleep 会关 AVDD，I2C 失联）|
| 0xA8 b10-9 | **PWM_DITHER_DEPTH** | **1h = ±5%** | 载波抖动深度，降噪核心 |

### 2.12 PERI_CONFIG1（0x0AA）—— 输入与方向

| 位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 21 | BUS_POWER_LIMIT_ENABLE | 0 | 置 1 会禁用主动制动 |
| 20-19 | DIR_INPUT | 1h | 顺时针（2h = 逆时针）。24 脚封装没有 DIR 引脚 |
| 18 | DIR_CHANGE_MODE | 0 | 换向时先停机再启动 |
| 17 | SPEED_LIMIT_ENABLE | 0 | 速度上限功能 |
| 16-13 | ACTIVE_BRAKE_SPEED_DELTA_LIMIT_ENTRY | 0 | 主动制动未用 |
| 12-10 | ACTIVE_BRAKE_MOD_INDEX_LIMIT | 0 | 同上 |
| 9 | **SPEED_RANGE_SEL** | **0** | SPEED 输入量程 0 = 325 Hz~100 kHz |
| 8-7 | INPUT_REFERENCE_MODE | 0 | |

### 2.13 GD_CONFIG1 / GD_CONFIG2（0x0AC / 0x0AE）

| 寄存器.位 | 字段 | 本项目值 | 说明 |
|---|---|---|---|
| 0xAC b27-26 | SLEW_RATE | 3h = 200 V/µs | 相线压摆率 |
| 0xAC b19 | OVP_SEL | 1 | 过压阈值 **22 V**（9V 供电，早拦）|
| 0xAC b18 | OVP_EN | 1 | 过压保护使能 |
| 0xAC b17 | OTW_REP | 1 | 过温告警上报 |
| 0xAE b24 | BUCK_PS_DIS | **1** | 3.3V 档不支持 AVDD 电源时序，必须为 1 |
| 0xAE b23 | BUCK_CL | 1 | 电阻模式必须选 150mA 档 |
| 0xAE b22-21 | BUCK_SEL | 0 | 3.3 V |
| 0xAE b20 | BUCK_DIS | 0 | Buck 使能 |

---

## 3. 配置写入流程（SLLA664）

1. **器件必须处于 idle**（电机未被驱动）—— 本工程写配置前先停机、等 50 ms
2. 用**读-改-写**（`k` 命令 / `modifyReg()`）而不是整值写，保留 `bit31 PARITY` 和其它字段
3. 每一项写完回读比对
4. 要掉电保持：写 `ALGO_CTRL1(0xEA) = 0x8A500000`，等 750 ms
5. 再发 `0x40000000`（EEPROM 读）让器件做 CRC/parity 自检，读 `EEPROM_FAULT(0x32C)` 确认
6. **断电重启**后 EEPROM 内容才真正生效（影子 RAM 是立即生效的）

---

## 4. 换电机时的修改顺序

1. `MOTOR_RES` / `MOTOR_IND` / `MOTOR_BEMF_CONST` —— 参数标定
   （低电感电机上 MPET 的 Ke 可能测成真实值的一半，需要人工标定）
2. `MAX_SPEED` —— 电频率 = RPM ÷ 60 × 极对数
3. `FG_DIV` + `FG_PULSES_PER_REV` —— 两者必须自洽
4. `ILIMIT` / `OL_ILIMIT` / `ALIGN_ILIMIT` / `LOCK_ILIMIT` —— 按电机与供电能力
5. `PWM_FREQ_OUT` —— 电感越低越要高，从 20~25 kHz 起往上试
6. 启动三段：`ALIGN_ILIMIT` → `OL_ACC_A1` → `OPN_CL_HANDOFF_THR`
   - 报 `ABN_BEMF` / `MTR_LCK` → 加大对齐电流、加快开环加速
   - 报 `HW_LOCK_LIMIT` → 降低开环电流、提高载波频率
7. `FAULT_CONFIG2` 的堵转阈值按实际负载微调
