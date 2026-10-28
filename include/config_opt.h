#ifndef MCF_CONFIG_OPT_H
#define MCF_CONFIG_OPT_H

#include <stdint.h>

/* =====================================================================
 *  config_opt.h —— 正式版配置（量产用，调试开关已全部移除）
 * ---------------------------------------------------------------------
 *  这里只有三类东西：
 *    1) 速度给定（MCU 用 PWM 驱动 SPEED 脚）
 *    2) 器件参数（全部是台架实测标定值，改任何一个都要重新验证）
 *    3) 脱机固化表 MCF_STANDALONE_CFG（上电自动下发 / 按 P 写 EEPROM）
 *
 *  历史：更早的调试版（MPET、速度扫描、PWM 频率对比、主动制动解码等）
 *        完整保留在 bak_debug_20261028/ 里，需要时去那边翻。
 * ===================================================================== */


/* ================= 速度给定：MCU 输出 PWM 给器件 SPEED 脚 =================
 * 正式板控制侧只走 SPEED / nFAULT / FG 三根线，器件 SPEED_MODE 必须 = 1h(PWM)。*/
#define EN_SPEED_PWM           (1)
  #define SPEED_PWM_FREQ_HZ    (5000UL) /* 5 kHz：器件在此频率能解析 11~13bit 占空比 */
  #define SPEED_PWM_BITS       (10)
  #define SPEED_PWM_CHANNEL    (0)
  #define SPEED_PWM_MIN_PCT    (1.0f)   /* "停机"时保留的最小占空比。
                                           SPEED 静态低电平超过 SLEEP_ENTRY_TIME(200ms)
                                           会让器件进 sleep（AVDD 关闭 → 上拉和 I2C 全失联）。
                                           留 1% 波形保活，此时 DUTY_CMD 仍为 0，电机不转。*/
#if EN_SPEED_PWM && (PIN_MCF_SPEED == PIN_UNUSED)
#error "EN_SPEED_PWM = 1 但 PIN_MCF_SPEED 是 PIN_UNUSED：config.h 里给它分配一个 GPIO。"
#endif

/* 上电是否自动把下面的配置表下发到器件影子 RAM。
 * 1 = 是（推荐）：
 *     芯片刚焊上去／EEPROM 被高温搞坏／换了新片子，MCU 一上电就把参数补齐，
 *     不依赖芯片里存的东西。固化到 EEPROM 只是为了让"不接 MCU 单独上电"时也能跑。
 * 0 = 否：完全信 EEPROM 里的内容（调试别处用 GUI 改过参数时才有意义）。*/
#define EN_APPLY_CFG_AT_BOOT   (1)

/* ================= 批量烧录模式（产线用）=================
 * 打开后固件变成一台"配置烧录机"：
 *   接上一片芯片 → 自动下发配置 + 写 EEPROM + 回读校验 → 打印成功/失败
 *   → 提示换下一片；芯片移走后自动进入下一轮。
 *
 * ★ 治具接线（一片一次）：
 *     GND、SDA、SCL、SPEED（必须接！）
 *   SPEED 悬空 → 器件 200ms 后休眠 → AVDD 关闭 → I2C 上拉跟着失效，
 *   扫描会一直扫不到器件。本固件在 SPEED 上持续输出 1% 占空比保活
 *   （1% 时 DUTY_CMD 仍为 0，电机不会转）。
 * ★ 目标板要给 VM 供电（器件内部从 VM 生成 AVDD/DVDD）。*/
#define EN_PROVISION_MODE      (1)

/* 后台故障轮询周期 */
#define MONITOR_PERIOD_MS      (500UL)


/* ================= 器件配置（全部为实测标定值）================= */

/* --- 停机方式 ---
 * MTR_STOP = 1h（续流）：停机那一下电感能量在 MOS 里循环掉，不往母线回灌。
 * 运行中的减速由 AVS 负责（见下），不用主动制动，避免绕组发热。*/
#define CFG_MTR_STOP           (1)
#define CFG_MTR_STOP_BRK       (6)    /* 0-4=1ms 5=5ms 6=10ms 7=50ms 8=100ms … */
#define CFG_ACT_SPIN_THR       (0)    /* 仅 MTR_STOP=4 时有用，当前不用 */
#define CFG_BRAKE_SPD_THR      (0)

/* --- 速度指令来源 ---
 * SPEED_MODE = 1h(PWM)。PIN_CONFIG 0xA4 b1-0。不能用 2h(I2C)：正式板没接那根线。*/
#define CFG_SPEED_MODE         (1)
  #define CFG_PWM_RANGE_SEL    (0)    /* 0 = 325Hz~100kHz（主机 5kHz 落这里）*/
  #define CFG_SLEEP_ENTRY_IDX  (3)    /* SLEEP_ENTRY_TIME：3h = 200ms。
                                         必须大于"最低占空比时的关断时间"，默认 50µs 会误判。*/
#define CFG_DEV_MODE           (0)    /* 0 = Standby（I2C 保持可用）*/

/* --- Buck（你的接法：电阻模式 22Ω、输出 3.3V、只驱动一个 LED）---
 * ★ 电阻模式外部负载必须 < 10mA，绝对不能给 ESP32 供电。
 * ★ BUCK_SEL=3.3V 时不支持 AVDD 电源时序，BUCK_PS_DIS 必须 = 1，
 *   否则 AVDD 会改从 VBK 取电，buck 一起来 AVDD 跟着抖，I2C 上拉全挂在上面。*/
#define CFG_BUCK_DIS           (0)
#define CFG_BUCK_SEL           (0)    /* 0=3.3V 1=5.0V 2=4.0V 3=5.7V */
#define CFG_BUCK_CL            (1)    /* 1=150mA 档（电阻模式必须）*/
#define CFG_BUCK_PS_DIS        (1)
#if (CFG_BUCK_SEL == 0 || CFG_BUCK_SEL == 2) && (CFG_BUCK_PS_DIS == 0)
#error "BUCK_SEL=3.3V/4.0V 不支持 AVDD 电源时序，CFG_BUCK_PS_DIS 必须 = 1（手册 §6.3.3.4）。"
#endif

/* --- 母线电压 / 过压保护 ---
 * VM = 9V 供电：BUS_VOLT 选 0h(15V 档)，OVP 选 22V（真出现回灌时更早拦下）。*/
#define CFG_OVP_EN             (1)
#define CFG_OVP_SEL            (1)    /* 0=34V 1=22V */
#define CFG_BUS_VOLT           (0)    /* 0=15V 1=30V 2=40V */

/* --- 转速上限（CLOSED_LOOP4 0x8E b13-0，14 位）---
 * 编码：电机最高【电频率】(Hz) = MAX_SPEED / 6
 *       电频率 = 目标RPM / 60 × 极对数（本电机 7 对极）
 * PWM 占空比 100% 对应这个转速。
 * 实测（装扇叶、VM=9V、ILIMIT=2.812A）：
 *   400 Hz(3429 RPM) = 原设定，风量偏小
 *   600 Hz(5143 RPM) = 能稳定跑满，无故障，调制指数仅 ~25%，← 当前
 *   800 Hz(6857 RPM) = 加速途中冲过 HW_LOCK_ILIMIT(3.75A) 跳闸，上不去*/
#define CFG_MAX_SPEED_ELEC_HZ  (600)
#define CFG_OVERMOD_EN         (0)    /* 过调制：再榨一点转速，代价是谐波/噪声变大 */

/* --- 防电压浪涌 AVS（CLOSED_LOOP1 0x88 b3）---
 * 速度指令突降时电机 BEMF 高于施加电压，机械能会回灌母线把 VM 顶高。
 * AVS 把负 Iq 参考钳到 0：不制动、不回灌，靠负载自然滑下来 —— 绕组里没电流，
 * 所以几乎不额外发热。你的母线没有大电容/耗散，这一项必须开。*/
#define CFG_AVS_EN             (1)

/* --- 加/减速斜率（CLOSED_LOOP1 0x88 b29-25 / b23-19，单位 Hz/s 电频率）---
 *   0h=0.5 1h=1 2h=2.5 3h=5 4h=7.5 5h=10 6h=20 7h=40 8h=60 9h=80 Ah=100 Bh=200
 *   Ch=300 Dh=400 Eh=500 Fh=600 … 14h=2000 … 1Fh=无限制
 * CL_ACC=Eh(500Hz/s)：0→600Hz 约 1.2 s，实测起步干脆且不冲电流。
 * CL_DEC=3h(5Hz/s)：AVS 开着时减速不走这个值（走自然滑行），设小是万一关 AVS 的保险。*/
#define CFG_CL_ACC_IDX         (0x0E)
#define CFG_CL_DEC_IDX         (0x03)

/* --- PWM 载波频率 + 抖动（降噪的主要手段）---
 * CLOSED_LOOP1 0x88 b18-15：0h=10k 1h=15k 2h=20k 3h=25k 4h=30k 5h=35k
 *                          6h=40k 7h=45k 8h=50k 9h=55k Ah=60k
 * ★ 手册 §6.3.17：L/R 越低的电机越需要高开关频率压电流纹波。这台电机电感极低：
 *     50/55/60 kHz → 正常；10~45 kHz → 转一会就 HW_LOCK_LIMIT(0xE2 b18) 停机。
 *   ⇒ 50 kHz 是硬下限，别再往下调。
 * ★ 噪音实测：55 kHz + ±5% 抖动 最安静（60 kHz 会有一阵阵尖锐声）。*/
#define CFG_PWM_FREQ_IDX       (0x09) /* 9h = 55 kHz */
#define CFG_PWM_DITHER_DEPTH   (1)    /* DEVICE_CONFIG2 0xA8 b10-9：0=关 1=±5% 2=±7.5% 3=±10% */
#define CFG_PWM_DITHER_MODE    (0)    /* DEVICE_CONFIG1 0xA6 b26：0=三角 1=随机 */
#if (CFG_PWM_FREQ_IDX == 0x08) && (CFG_PWM_DITHER_DEPTH == 3)
#warning "50 kHz + ±10% 抖动会下探到 45 kHz —— 那一段实测会触发 HW_LOCK_LIMIT 停机。"
#endif

/* --- 电机参数（实测标定值，直接写影子 RAM）---
 * ★ 这几项非零 → 器件不再每次给速度都跑 MPET（手册图 6-51）。
 * ★ 不能用 MPET 的结果：MPET 在这台低电感小惯量电机上把 Ke 测成 0x02(0.7mV/Hz)，
 *   只有真实值的一半 → 转速估低 → 给 20% 会一路加速到 ~46% 才失步；
 *   且"机械参数测量"阶段会卡在滑行（这台电机滑行时速度反馈是 0x7FFFFFFF）。
 * ★ 换电机必须重新标定这五个值。*/
#define CFG_MOTOR_RES_CODE     (0x74) /* MOTOR_RES：约 0.43Ω/相 */
#define CFG_MOTOR_IND_CODE     (0x02) /* MOTOR_IND */
#define CFG_MOTOR_BEMF_CODE    (0x0A) /* MOTOR_BEMF_CONST：1.5 mV/Hz（标定值）*/
#define CFG_SPD_LOOP_KP_CODE   (0x01) /* Kp = 0.01 × 码值 */
#define CFG_SPD_LOOP_KI_CODE   (0x01) /* Ki = 0.1  × 码值 */

/* 开环→闭环换相阈值（MOTOR_STARTUP2 0x86 b17-13）：码值+1 = 占 MAX_SPEED 的百分比。
 * 14h = 15% → 600Hz 下 = 90Hz ≈ 771 RPM。
 * ★ 实测太低（5%）BEMF 太小，观测器报 ABN_BEMF 失步；太高（32%）会先冲过头再回落。*/
#define CFG_OPN_CL_HANDOFF_IDX (14)

/* --- 电流限值（供电预算 3A）---
 * 通用 4 位档位：
 *   0h=0.078 1h=0.156 2h=0.312 3h=0.625 4h=0.937 5h=1.25 6h=1.562 7h=1.875
 *   8h=2.187 9h=2.5   Ah=2.812 ←3A 以内最高档  Bh=3.125  Ch=3.437  Dh=3.75
 * ★ 器件在不同阶段用不同参数，任何一个漏设那一段就没人管：
 *     对齐→ALIGN_OR_SLOW_CURRENT_ILIMIT  IPD→IPD_CURR_THR
 *     开环→OL_ILIMIT   闭环→ILIMIT      堵转→LOCK/HW_LOCK_ILIMIT */
#define CFG_ILIMIT_IDX         (0x0A) /* 闭环运行 2.812A */
#define CFG_OL_ILIMIT_IDX      (0x0A) /* 开环加速 2.812A（带扇叶低了这个起不来）*/
#define CFG_ALIGN_ILIMIT_IDX   (0x08) /* 对齐 2.187A（对齐时电机不转、电流全变热）*/
#define CFG_LOCK_ILIMIT_IDX    (0x0D) /* 堵转检测(ADC) 3.75A —— 必须高于启动电流峰值，
                                         否则带载启动会误报 HW_LOCK */
#define CFG_HW_LOCK_ILIMIT_IDX (0x0D) /* 堵转检测(比较器) 3.75A */
#define CFG_IPD_CURR_IDX       (0x07) /* IPD 电流阈值 1.562A（5 位档位，另一套表）*/
#define CFG_LOCK_DEG_IDX       (0x08) /* 堵转去抖：8h = 10ms（滤掉换向尖峰）*/
#define CFG_ALIGN_RAMP_IDX     (0x06) /* 对齐电流上升斜率 6h = 50 A/s */

/* --- 启动时序（MOTOR_STARTUP1/2）---
 * 无感启动是这台电机最容易出问题的一环，实测结论：
 *   · 对齐电流必须够（2.187A）：低了转子没被真正拉到对齐位置，
 *     开环一启动就打滑 → ABN_BEMF + MTR_LCK
 *   · 开环加速要【快】（250 Hz/s）：低速区反电动势小、观测器最弱，
 *     快速穿过去比慢慢爬稳定得多（25 Hz/s 实测失败率明显更高）
 *   · 开环电流 2.812A 是上限：再高（3.125A）纹波峰值会顶到 3.75A 的
 *     HW_LOCK 硬件比较器，直接跳闸 */
#define CFG_OL_ACC_IDX         (0x0A) /* OL_ACC_A1（MOTOR_STARTUP2 0x86 b26-23）
                                         0h=0.1 4h=5 5h=10 6h=25 7h=50 8h=75
                                         9h=100 Ah=250 Hz/s ← 当前 */
/* 对齐时间（MOTOR_STARTUP1 0x84 b24-21）
 *   0h=10ms 1h=50ms 2h=100ms 3h=200ms 4h=300ms 5h=400ms 6h=500ms 7h=750ms
 *   8h=1s ← 当前  9h=1.5s Ah=2s …
 * ★ 复位值是 10ms，比"电流从 0 爬到对齐值"所需的时间还短，必须显式改。
 *   实测 1s 与 2s 的启动成功率没有差别，取快的。*/
#define CFG_ALIGN_TIME_IDX     (0x08)

/* --- 初始速度检测 ISD + 重同步（ISD_CONFIG 0x80，手册 §6.3.10.1 / §6.3.10.2）---
 * 转子还在滑行时重新给速度，器件先用三相电压实测转速/角度/方向，然后直接接上，
 * 而不是重新走一遍"对齐 + 开环"。
 * ★ 不打开这两项时，运行中"降速 → 再给速度"会明显顿一下：
 *   实测转速会掉到几十 RPM 再重新爬，听感就是抽一下。*/
#define CFG_ISD_EN             (1)   /* ISD_EN（0x80 b30）*/
#define CFG_RESYNC_EN          (1)   /* RESYNC_EN（0x80 b26）*/
#define CFG_FW_DRV_RESYN_IDX   (2)   /* FW_DRV_RESYN_THR（0x80 b25-22）：重同步进闭环的最低速度
                                        0h=5% 1h=10% 2h=15% ← 当前（低于此速度改走开环）*/
#define CFG_ISD_BRAKE_EN       (0)   /* BRAKE_EN（0x80 b29）：启动前制动
                                        0 = 不做（能量不往绕组里灌）*/

/* --- 旋转方向 ---
 * 24 脚封装没有 DIR 引脚，方向只能用 I2C 的 DIR_INPUT 字段（PERI_CONFIG1 0xAA b20-19）：
 *   1h = 顺时针 OUTA-OUTB-OUTC   2h = 逆时针 OUTA-OUTC-OUTB
 * DIR_CHANGE_MODE = 0：检测到方向变化时先按停机方式停住再重新启动（安全）。*/
#define CFG_DIR_SEL            (0)    /* 0 = 顺时针 1 = 逆时针 */
#define CFG_DIR_CHANGE_MODE    (0)

/* --- nFAULT / FG 内部上拉 ---
 * 你板上 FG/nFAULT 已有外部 5.1k 上拉到 AVDD，所以内部上拉 = 0，否则打架。
 * 上正式板时可以改成 1 省掉两个外部电阻，但要注意：
 *   1) 改这一位必须写 EEPROM 并重新上电才生效；
 *   2) 上拉挂在 AVDD 上，sleep 会关掉 AVDD，所以主机侧也建议开内部上拉做冗余。*/
#define CFG_PULLUP_EN           (0)
#define CFG_PULLUP_ON_AVDD      (1)   /* 上拉接在 AVDD → 禁止 DEV_MODE=Sleep */
#if CFG_PULLUP_ON_AVDD && (CFG_DEV_MODE != 0)
#error "上拉接在 AVDD 上时不能选 Sleep：Sleep 会关闭 AVDD LDO，I2C 上拉一起消失。"
#endif


/* ================= 影子寄存器「读-改-写」配置表 =================
 * 每项 = { 地址, 位掩码 mask, 目标值 value, 说明 }
 * 写入时只改 mask 指定的位，其余位（含 bit31 PARITY）原样保留，
 * 所以不会冲掉别处设过的东西。上电自动下发，按 P 写 EEPROM。
 * ------------------------------------------------------------------- */
struct McfCfgEntry {
    uint16_t    addr;
    uint32_t    mask;
    uint32_t    value;
    const char *note;
};

static const McfCfgEntry MCF_STANDALONE_CFG[] = {
    /* 停机方式（CLOSED_LOOP2 0x8A）*/
    { 0x08A, 0x70000000UL, ((uint32_t)CFG_MTR_STOP)      << 28, "MTR_STOP（1=续流，不往母线回灌）" },
    { 0x08A, 0x0F000000UL, ((uint32_t)CFG_MTR_STOP_BRK)  << 24, "MTR_STOP_BRK_TIME" },
    { 0x08A, 0x00F00000UL, ((uint32_t)CFG_ACT_SPIN_THR)  << 20, "ACT_SPIN_THR" },
    { 0x08A, 0x000F0000UL, ((uint32_t)CFG_BRAKE_SPD_THR) << 16, "BRAKE_SPEED_THRESHOLD" },
    /* 初始速度检测 + 重同步（ISD_CONFIG 0x080）—— 消除"滑行中重新给速度"的顿挫 */
    { 0x080, 0x40000000UL, ((uint32_t)CFG_ISD_EN)          << 30, "ISD_EN（初始速度检测）" },
    { 0x080, 0x20000000UL, ((uint32_t)CFG_ISD_BRAKE_EN)    << 29, "BRAKE_EN（启动前不制动）" },
    { 0x080, 0x04000000UL, ((uint32_t)CFG_RESYNC_EN)       << 26, "RESYNC_EN（重同步）" },
    { 0x080, 0x03C00000UL, ((uint32_t)CFG_FW_DRV_RESYN_IDX) << 22, "FW_DRV_RESYN_THR（15%）" },
    /* 开环加速（MOTOR_STARTUP2 0x086 b26-23）*/
    { 0x086, 0x07800000UL, ((uint32_t)CFG_OL_ACC_IDX)      << 23, "OL_ACC_A1（开环加速 250Hz/s）" },
    /* 速度指令来源（PIN_CONFIG 0xA4 b1-0）*/
    { 0x0A4, 0x00000003UL, ((uint32_t)CFG_SPEED_MODE),           "SPEED_MODE（1=PWM）" },
    { 0x0AA, 0x00000200UL, ((uint32_t)CFG_PWM_RANGE_SEL) << 9,   "SPEED_RANGE_SEL（PWM 输入量程）" },
    { 0x0A8, 0x0000C000UL, ((uint32_t)CFG_SLEEP_ENTRY_IDX) << 14, "SLEEP_ENTRY_TIME（SPEED 静默多久算关机）" },
    { 0x0A8, 0x00000800UL, ((uint32_t)CFG_DEV_MODE)      << 11,  "DEV_MODE（0=Standby）" },
    /* PWM 抖动（降噪）*/
    { 0x0A8, 0x00000600UL, ((uint32_t)CFG_PWM_DITHER_DEPTH) << 9, "PWM_DITHER_DEPTH（±5%）" },
    { 0x0A6, 0x04000000UL, ((uint32_t)CFG_PWM_DITHER_MODE) << 26, "PWM_DITHER_MODE（0=三角）" },
    /* Buck（GD_CONFIG2 0xAE）*/
    { 0x0AE, 0x01000000UL, ((uint32_t)CFG_BUCK_PS_DIS)   << 24, "BUCK_PS_DIS" },
    { 0x0AE, 0x00800000UL, ((uint32_t)CFG_BUCK_CL)       << 23, "BUCK_CL（电阻模式必须=1）" },
    { 0x0AE, 0x00600000UL, ((uint32_t)CFG_BUCK_SEL)      << 21, "BUCK_SEL（3.3V）" },
    { 0x0AE, 0x00100000UL, ((uint32_t)CFG_BUCK_DIS)      << 20, "BUCK_DIS" },
    /* 过压保护（GD_CONFIG1 0xAC）*/
    { 0x0AC, 0x000C0000UL, (((uint32_t)CFG_OVP_SEL) << 19) | (((uint32_t)CFG_OVP_EN) << 18), "OVP_EN / OVP_SEL" },
    /* 母线电压档位（DEVICE_CONFIG1 0xA6 b1-0）*/
    { 0x0A6, 0x00000003UL, ((uint32_t)CFG_BUS_VOLT),             "BUS_VOLT（0=15V 档）" },
    /* 最高转速（CLOSED_LOOP4 0x8E b13-0）*/
    { 0x08E, 0x00003FFFUL, ((uint32_t)CFG_MAX_SPEED_ELEC_HZ) * 6UL, "MAX_SPEED（电频率 Hz × 6）" },
    /* 过调制 / AVS / 加减速 / PWM 频率（CLOSED_LOOP1 0x88）*/
    { 0x088, 0x40000000UL, ((uint32_t)CFG_OVERMOD_EN) << 30,     "OVERMODULATION_ENABLE" },
    { 0x088, 0x3E000000UL, ((uint32_t)CFG_CL_ACC_IDX) << 25,     "CL_ACC" },
    { 0x088, 0x00F80000UL, ((uint32_t)CFG_CL_DEC_IDX) << 19,     "CL_DEC" },
    { 0x088, 0x00078000UL, ((uint32_t)CFG_PWM_FREQ_IDX) << 15,   "PWM_FREQ_OUT（55 kHz）" },
    { 0x088, 0x00000008UL, ((uint32_t)CFG_AVS_EN) << 3,          "AVS_EN（防 VM 浪涌）" },
    /* 电机参数（标定值；非零后器件不再跑 MPET）*/
    { 0x08A, 0x0000FF00UL, ((uint32_t)CFG_MOTOR_RES_CODE)  <<  8, "MOTOR_RES" },
    { 0x08A, 0x000000FFUL, (uint32_t)CFG_MOTOR_IND_CODE,          "MOTOR_IND" },
    { 0x08C, 0x7F800000UL, ((uint32_t)CFG_MOTOR_BEMF_CODE) << 23, "MOTOR_BEMF_CONST（1.5mV/Hz）" },
    { 0x08C, 0x00000007UL, 0UL,                                   "SPD_LOOP_KP 的 3 个 MSB" },
    { 0x08E, 0x7F000000UL, ((uint32_t)CFG_SPD_LOOP_KP_CODE) << 24, "SPD_LOOP_KP 低 7 位" },
    { 0x08E, 0x00FFC000UL, ((uint32_t)CFG_SPD_LOOP_KI_CODE) << 14, "SPD_LOOP_KI" },
    { 0x086, 0x0003E000UL, ((uint32_t)CFG_OPN_CL_HANDOFF_IDX) << 13, "OPN_CL_HANDOFF_THR（15%）" },
    /* 电流限值（六个阶段全部限住）*/
    { 0x090, 0x78000000UL, ((uint32_t)CFG_ILIMIT_IDX)        << 27, "ILIMIT（闭环）" },
    { 0x090, 0x07800000UL, ((uint32_t)CFG_HW_LOCK_ILIMIT_IDX) << 23, "HW_LOCK_ILIMIT（比较器）" },
    { 0x090, 0x00780000UL, ((uint32_t)CFG_LOCK_ILIMIT_IDX)   << 19, "LOCK_ILIMIT（ADC）" },
    { 0x090, 0x00007800UL, ((uint32_t)CFG_LOCK_DEG_IDX)      << 11, "LOCK_ILIMIT_DEG（10ms）" },
    { 0x086, 0x78000000UL, ((uint32_t)CFG_OL_ILIMIT_IDX)     << 27, "OL_ILIMIT（开环）" },
    { 0x084, 0x001E0000UL, ((uint32_t)CFG_ALIGN_ILIMIT_IDX)  << 17, "ALIGN_OR_SLOW_CURRENT_ILIMIT" },
    { 0x084, 0x00003E00UL, ((uint32_t)CFG_IPD_CURR_IDX)      <<  9, "IPD_CURR_THR" },
    { 0x084, 0x1E000000UL, ((uint32_t)CFG_ALIGN_RAMP_IDX)    << 25, "ALIGN_SLOW_RAMP_RATE" },
    { 0x084, 0x01E00000UL, ((uint32_t)CFG_ALIGN_TIME_IDX)    << 21, "ALIGN_TIME（1s，复位值只有 10ms）" },
    /* 方向（PERI_CONFIG1 0xAA）*/
    { 0x0AA, 0x00180000UL, ((uint32_t)(CFG_DIR_SEL ? 2UL : 1UL)) << 19, "DIR_INPUT（1h=顺时针 2h=逆时针）" },
    { 0x0AA, 0x00040000UL, ((uint32_t)CFG_DIR_CHANGE_MODE) << 18, "DIR_CHANGE_MODE（0=先停机）" },
    /* 内部上拉（DEVICE_CONFIG1 0xA6 b2）*/
    { 0x0A6, 0x00000004UL, ((uint32_t)CFG_PULLUP_EN) << 2,       "PULLUP_ENABLE（外部已上拉则=0）" },
};

#endif /* MCF_CONFIG_OPT_H */
