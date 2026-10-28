# MCF8315D + ESP32-C3 无刷电机驱动固件

用 ESP32-C3 通过 **I2C 配置 + PWM 调速**驱动 TI MCF8315D 无感 FOC 三相 BLDC 驱动器。
控制侧只占用 **SPEED / nFAULT / FG 三根线**，适合做成独立的调速板。

本工程是在一台 9V 低电感、低 Ke 的轴流风机上实测调通的，配置文件里保留了每项参数的
取值依据（多数是实测结论，不是手册复位值）。

**文档**

| 文档 | 内容 |
|---|---|
| [docs/config_reference.md](docs/config_reference.md) | **配置参考**：24 个影子寄存器逐项说明、复位值陷阱、标定顺序 |
| [docs/speed_pwm.md](docs/speed_pwm.md) | SPEED 引脚 PWM 调速接口（接线/频率/占空比/休眠/代码示例）|

---

## 1. 硬件连接

| MCF8315D 引脚 | ESP32-C3 | 说明 |
|---|---|---|
| SDA | GPIO7 | 开漏，5.1k 上拉到 **AVDD** |
| SCL | GPIO8 | 同上。⚠ 本板 GPIO8 还接着板载 LED，见"注意事项" |
| SPEED/WAKE | GPIO5 | MCU 输出 5 kHz PWM，占空比 = 转速百分比 |
| nFAULT | GPIO10 | 开漏，下降沿中断 |
| FG | GPIO6 | 开漏，转速脉冲 |
| DRVOFF | 接地 | 本工程不使用软件控制 |
| BRAKE | GPIO4 | 低电平 = 不制动 |
| EXT_WD / EXT_CLK | 接地 | 不使用外部看门狗/时钟 |

供电要点：

- 器件 **AVDD（pin 17）是内部 3.3V LDO，对外只有 20mA**，I2C 上拉挂在这里（4 根约 2.6mA 没问题）
- **Buck 是电阻模式（22Ω、3.3V），外部负载必须 < 10mA**，只能点一个 LED，绝不能给 MCU 供电
- `BUCK_SEL = 3.3V` 时 `BUCK_PS_DIS` 必须为 1，否则 AVDD 会改从 buck 取电

---

## 2. 编译与烧录

```bash
pio run                 # 编译
pio run -t upload       # 烧录（platformio.ini 里默认走 USB CDC）
pio device monitor      # 115200
```

依赖：PlatformIO + `espressif32` + Arduino framework，`ARDUINO_USB_CDC_ON_BOOT=1`。

串口打开时 **DTR 必须拉高**，否则 ESP32-C3 的 USB CDC 不会输出（只收不发）。

---

## 3. 串口命令

| 命令 | 作用 |
|---|---|
| `0`-`100` | 设转速百分比（= SPEED 引脚 PWM 占空比），`0` 停机 |
| `p` | 打印状态（地址/转速/故障/错误计数） |
| `f` | 读并解码故障寄存器 0xE0 / 0xE2 / 0x32C |
| `c` | 清故障 |
| `d` | 打印影子寄存器 0x080–0x0AE |
| `s` | 重新扫描 I2C 地址 |
| `P` | **把参数表固化到器件 EEPROM**（需再输入大写 `W` 确认） |
| `r AAAA` | 读 32 位寄存器（调试） |
| `k AAAA MASK VALUE` | 读-改-写寄存器，只动 MASK 内的位、保留 PARITY（在线调参） |
| `h` | 帮助 |

---

## 4. 关键设计说明

### 4.1 速度给定

器件 `SPEED_MODE = PWM`，SPEED 脚占空比线性映射到 `0 ~ MAX_SPEED`。
本工程 `MAX_SPEED = 600 Hz` 电频率 ≈ **5143 RPM**（7 对极），即 100% 占空比对应此转速。

> 接线、频率选择、占空比映射、休眠保活、代码示例等，见 **[docs/speed_pwm.md](docs/speed_pwm.md)**。

### 4.2 停机与减速：不回灌母线

母线没有大电容/耗散，所以：

- `MTR_STOP = 1`（续流）：停机那一下电感能量在 MOS 里循环掉
- `AVS_EN = 1`：速度指令下降时把负 Iq 钳到 0，**不制动、不回灌**，靠负载自然滑行
- 主动制动（`ACTIVE_BRAKE_EN`）默认关闭：它能快速刹停且不回灌，但能量全部变成绕组和 MOS 的热

### 4.3 载波频率与噪音

低电感电机必须用高开关频率压电流纹波（手册 §6.3.17）。实测这台电机：

| PWM 载波 | 结果 |
|---|---|
| 10–45 kHz | 运行一会就报 `HW_LOCK_LIMIT`（纹波峰值顶到 3.75A 比较器） |
| 50 / 55 / 60 kHz | 正常 |
| **55 kHz + ±5% PWM 抖动** | **听感最安静**（60 kHz 有一阵阵尖锐声） |

PWM 抖动（spread spectrum，手册 §6.3.18）把开关频率在 55kHz 附近来回摆动，
把能量摊到一段频带里，从"尖锐单音"变成"轻微沙沙声"。

### 4.4 无感启动

转子静止时没有反电动势，无感驱动必须先 **对齐（Align）+ 开环加速**，
等反电动势足够大（`OPN_CL_HANDOFF_THR = 15%` ≈ 90 Hz）才切到闭环。

实测结论：

- **对齐电流不能太低**（2.187A）：低了转子没被真正拉到位，开环一启动就打滑
- **开环加速要快**（250 Hz/s）：低速区观测器最弱，快速穿过去比慢慢爬稳定
- **开环电流上限被硬件锁死**：2.812A 时纹波峰值已经接近 3.75A 的 `HW_LOCK` 比较器，
  再高（试过 3.125A）会持续跳闸
- **`ISD_EN` + `RESYNC_EN` 必须开**：转子还在滑行时重新给速度，器件会先实测转速/角度
  再直接接上，否则每次"降速→再给速度"都会明显顿一下
- 固件还带**启动失败自动重试**（最多 3 次）：失败后停机、等转子滑停、清故障、重新给速度。
  只对启动类故障（ABN_BEMF / ABN_SPEED / MTR_LCK / LOCK / HW_LOCK / NO_MTR）重试，
  过压/过温等一律不重试

### 4.5 电流限值（六段全覆盖）

器件在不同阶段用不同参数，漏一个那一段就没人管：

| 阶段 | 参数 | 值 |
|---|---|---|
| 对齐 | ALIGN_OR_SLOW_CURRENT_ILIMIT | 2.187 A |
| 开环加速 | OL_ILIMIT | 2.812 A |
| 闭环运行 | ILIMIT | 2.812 A |
| 堵转检测 | LOCK/HW_LOCK_ILIMIT | 3.75 A（4 位档位上限） |

### 4.6 完整配置表（65 项）

`config_opt.h` 里的 `MCF_STANDALONE_CFG` 是一张**读-改-写**表：每项 = `{地址, 位掩码, 目标值, 说明}`，
只改掩码内的位，`bit31 PARITY` 和其它字段原样保留。

| 寄存器 | 项数 | 内容 |
|---|---|---|
| 0x080 ISD_CONFIG | 4 | 初始速度检测 + 重同步 |
| 0x084 MOTOR_STARTUP1 | 5 | 对齐方式 / 电流 / 时间 / 斜率 |
| 0x086 MOTOR_STARTUP2 | 4 | 开环电流 / 加速度 / 换相阈值 |
| 0x088 CLOSED_LOOP1 | 8 | 载波 / 抖动 / 加减速 / AVS / FG |
| 0x08A CLOSED_LOOP2 | 6 | 停机方式 / 电机 R、L |
| 0x08C + 0x08E | 5 | Ke、速度环 Kp/Ki、MAX_SPEED |
| 0x090 FAULT_CONFIG1 | 5 | 各阶段电流限值、EEP_FAULT_MODE |
| 0x092 FAULT_CONFIG2 | 13 | 三种堵转检测 + 母线欠压/过压保护 |
| 0x0A0 INT_ALGO_1 | 3 | ISD 时间常数、SPEED 脚滤波 |
| 0x0A4 / 0xA6 / 0xA8 / 0xAA / 0xAC / 0xAE | 12 | 引脚 / 器件 / Buck / 方向 / 过压 |

逐字段含义见 **[docs/config_reference.md](docs/config_reference.md)**。

**为什么要配这么多**：器件复位值基本全是 0，但"0"不等于"能用"——
`FG_DIV` 复位值是 3× 电频率（转速读数差 3 倍）、`ALIGN_TIME` 复位值只有 10 ms
（对齐电流还没建立就进开环）、`LOCK1/2/3_EN` 复位值全关（电机卡住没有任何保护）。

固件每次上电都会把这张表重下发一遍，所以**芯片是全新的、EEPROM 是空的，接上 MCU 也能正常工作**；
EEPROM 只是为了让"不接 MCU 单独上电"时也能跑。

> 产线批量烧录用的是同一份代码，把 `config_opt.h` 的 `EN_PROVISION_MODE` 改成 `1` 即可
> （接一片烧一片，自动判定换片）。默认 `0`，走正常调速固件。

---

## 5. 换芯片 / 重新焊接后怎么恢复配置

参数以两种方式存在：

1. **MCU 固件里**（`MCF_STANDALONE_CFG`）：每次上电自动下发到器件影子 RAM，
   所以即使芯片是全新的、EEPROM 是空的，接上 MCU 就能跑
2. **器件 EEPROM 里**：固件里执行 `P` 命令写入，用于"不接 MCU 单独上电"的场合

芯片热风枪吹下来重焊、或换新片后：

```
上电 → 串口输入 P → 再输入 W 确认 → 断电重启
```

> 热风枪高温可能影响 EEPROM 内容，所以重焊后建议重走一次 `P`。

---

## 6. 换电机要改哪里

按顺序标定（`R`/`L`/`Ke` 非零后器件才不会再自动跑 MPET）：

1. `CFG_MOTOR_RES_CODE` / `CFG_MOTOR_IND_CODE` / `CFG_MOTOR_BEMF_CODE`
   —— 相电阻 / 电感 / 反电动势常数（可用 MPET 测，但低电感电机上 MPET 的 Ke 可能偏小，
   需要实测标定）
2. `CFG_MAX_SPEED_ELEC_HZ` —— 电频率 = RPM/60 × 极对数
3. `CFG_ILIMIT_IDX` / `CFG_OL_ILIMIT_IDX` / `CFG_ALIGN_ILIMIT_IDX` —— 按供电与电机额定
4. `CFG_PWM_FREQ_IDX` —— 电感越低越要高，从 25k 起往上试
5. `CFG_OPN_CL_HANDOFF_IDX` / `CFG_OL_ACC_IDX` / `CFG_ALIGN_ILIMIT_IDX` —— 启动三段，
   用 `f` 看故障码调（失步 → 加对齐电流/加快开环；HW_LOCK → 降电流/提载波）
6. `FG_PULSES_PER_REV` —— 每机械转的 FG 脉冲数（= 极对数 / FG_DIV）

---

## 7. 注意事项

- **GPIO8 是板载 LED 引脚**：本板 SCL 也用它，LED 那条 5.1k 会额外上拉 SCL。
  电气无害，但不能再把 GPIO8 当灯用，也不要让它输出低电平（会把 I2C 时钟拉死）。
- **GPIO9 是 BOOT strapping 脚**，不要接 BRAKE（高电平 = 制动，上电时会被拉高）。
- **Sleep 会关闭 AVDD**：SPEED 引脚静默超过 `SLEEP_ENTRY_TIME`（本工程 200ms）就休眠，
  I2C 上拉一起失效。所以停机时固件仍输出 1% 占空比保活。
- **散热**：TSSOP-24（PWP）在 2.8A 连续电流下全靠底部散热焊盘。
  调试板上热焊盘焊接不实会很快发烫，正式板务必铺铜 + 打过孔。
- 器件峰值相电流额定 **3.5A**（PWP），`ILIMIT` 4 位档位最高只能到 3.75A，已超额定。

---

## 8. 参考资料

- MCF8315D 数据手册 **SLLSG06**
- MCF8315D I2C 编程指南 **SLLA662**
- MCF8315D EEPROM 读写 **SLLA664**
