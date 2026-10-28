# 用 ESP32-C3 驱动 MCF8315D

MCF8315D 是 TI 的无感 FOC 三相 BLDC 驱动芯片。本工程用 ESP32-C3 通过 **I2C 配置**它，
再用一根 **PWM** 控制转速——控制侧只占 SPEED / nFAULT / FG 三根线。

下面所有参数都是在台架（9V 母线、7 对极风机、限流 3A）上试出来的，不是手册复位值。
换电机基本都要重新标定，顺序见 `config_opt.h` 的注释。

| 文档 | 内容 |
|---|---|
| [docs/config_reference.md](docs/config_reference.md) | 24 个影子寄存器逐项说明、复位值陷阱、写入流程 |
| [docs/speed_pwm.md](docs/speed_pwm.md) | 只给 SPEED 脚喂 PWM 的接口说明（频率/占空比/休眠/代码） |
| [release/](release/) | 产线批量烧录固件（接一片烧一片） |

## 编译与烧录

```bash
pio run
pio run -t upload
pio device monitor      # 115200，注意 DTR 要拉高，否则 USB CDC 不出数据
```

串口命令：`0`-`100` 设转速、`0` 停机、`p` 状态、`f` 故障、`c` 清故障、
`d` 寄存器全量、`P` 固化到 EEPROM、`r`/`k` 在线读写寄存器、`h` 帮助。

## 关键参数

| 项 | 值 | 说明 |
|---|---|---|
| PWM 载波 | 55 kHz + ±5% 抖动 | 这颗电机低于 50 kHz 会因纹波跳 `HW_LOCK` |
| 转速上限 | 600 Hz 电频率 ≈ 5143 RPM | 占空比 100% 对应此值 |
| 电流 | 对齐 2.187A / 开环 2.812A / 闭环 2.812A | 再高纹波会顶到 3.75A 硬件比较器 |
| 启动 | 对齐 1s → 开环 250Hz/s → 15% 换相 | 开环要快，慢了反而打滑 |
| 减速 | 续流停机 + AVS | 不回灌母线，靠负载自然滑行 |

配置表共 65 项，上电自动下发——**芯片是新的、EEPROM 是空的也能跑**。

## 注意事项

- **GPIO8** 是板载 LED 引脚，本板兼作 SCL：不能再当灯用，也不能输出低电平
- **GPIO9** 是 BOOT strapping 脚，别接 BRAKE（上电被拉高 = 制动）
- **Sleep 会关闭 AVDD**：SPEED 静默超过 200ms 就休眠，I2C 一起失联。停机时固件保留 1% 占空比保活
- **散热**：TSSOP-24 在 2.8A 下全靠底部焊盘，正式板务必铺铜 + 过孔

## 参考

SLLSG06（数据手册）/ SLLA662（I2C 编程）/ SLLA664（EEPROM）

## 许可

MIT，见 [LICENSE](LICENSE)。
