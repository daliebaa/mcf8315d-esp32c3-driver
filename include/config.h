#ifndef MCF_CONFIG_H
#define MCF_CONFIG_H

#include <stdint.h>

/* =====================================================================
 *  config.h —— 引脚与固定参数
 * ---------------------------------------------------------------------
 *  功能开关和设备参数在 config_opt.h，这里只放「接到哪个脚」和「时序参数」。
 * ===================================================================== */

#define FW_NAME    "ESP32C3-MCF8315D"
#define FW_VERSION "2.0.0"

/* ============================ 引脚配置 ============================
 * 平台：ESP32-C3（DevKitM-1 / SuperMini 等）
 *       原生 USB 走 GPIO18/19，因此 UART0(GPIO20/21) 空闲。
 *
 * 板上已占用、必须避开的引脚：
 *   GPIO8   板载蓝色 LED（3.3V—5.1k—LED—GPIO8，低电平点亮）
 *   GPIO9   BOOT 按键，同时是 strapping 脚
 *   GPIO2   strapping 脚
 *   GPIO18/19  原生 USB D-/D+
 *   GPIO11-17  内部 SPI Flash
 * ================================================================ */
#define PIN_UNUSED     (-1)

/* --- I2C 总线 ---
 * ⚠ 本板 SCL 用了 GPIO8，和板载 LED 冲突：LED 那条 5.1k 通路会额外把 SCL 上拉，
 *   电气上无害，但不能再把 GPIO8 当灯用。
 * ⚠ 器件掉电时 I/O 不得阻塞总线（手册 §5.5），建议 SDA/SCL 各串 22~33Ω。*/
#define PIN_I2C_SDA    (7)
#define PIN_I2C_SCL    (8)

/* --- 器件反馈输入（均为开漏，需外部或内部上拉到 3.3V）--- */
#define PIN_MCF_NFAULT (10)  /* 故障时被器件拉低 */
#define PIN_MCF_FG     (6)   /* 转速脉冲 */

/* --- 速度给定：MCU 用 PWM 驱动器件的 SPEED/WAKE 脚 ---
 * 正式板控制侧只保留 SPEED / nFAULT / FG 三根线，器件 SPEED_MODE 配成 1h(PWM)。
 * SPEED 内部有 1M 下拉，主机侧不用再加。
 * ⚠ SPEED 静态低电平超过 SLEEP_ENTRY_TIME 器件会进 sleep，AVDD 随之关闭。*/
#define PIN_MCF_SPEED  (5)

/* --- 可选控制引脚（本板由硬件决定，宏为 PIN_UNUSED 时固件不碰）--- */
#define PIN_MCF_DRVOFF (PIN_UNUSED)  /* 板上直接接地 */
#define PIN_MCF_BRAKE  (4)           /* 制动输入，低电平=不制动 */

/* --- 编译期引脚合法性检查 --- */
#define MCF_PIN_OK(p) \
    (((p) == PIN_UNUSED) || ((p) >= 0 && (p) <= 21 && ((p) < 11 || (p) > 17) && (p) != 2 && (p) != 9 && (p) != 18 && (p) != 19))

static_assert(MCF_PIN_OK(PIN_I2C_SDA),    "PIN_I2C_SDA 非法");
static_assert(MCF_PIN_OK(PIN_I2C_SCL),    "PIN_I2C_SCL 非法");
static_assert(MCF_PIN_OK(PIN_MCF_NFAULT), "PIN_MCF_NFAULT 非法");
static_assert(MCF_PIN_OK(PIN_MCF_FG),     "PIN_MCF_FG 非法");
static_assert(MCF_PIN_OK(PIN_MCF_SPEED),  "PIN_MCF_SPEED 非法");
static_assert(MCF_PIN_OK(PIN_MCF_DRVOFF), "PIN_MCF_DRVOFF 非法");
static_assert(MCF_PIN_OK(PIN_MCF_BRAKE),  "PIN_MCF_BRAKE 非法");

/* ============================ 串口 ============================ */
#define SERIAL_BAUD    (115200)
#define SERIAL_WAIT_MS (3000UL)   /* USB CDC 等待主机打开串口的最长时间 */

/* ============================ I2C ============================
 * 器件要求每传输 1 字节之间间隔 >= 100us（手册 §6.6.2）。
 * 80 kHz 下 9 bit = 112.5us，默认值天然满足。 */
#define I2C_SCL_FREQ_HZ         (80000UL)
#define I2C_TIMEOUT_MS          (20)     /* 必须 > 5ms：器件时钟拉伸上限 */
#define I2C_USE_BITBANG         (0)      /* 1 = 软件 I2C（可精确控字节间隔） */
#define I2C_BYTE_GAP_US         (100)    /* 仅软件 I2C 生效 */
#define I2C_BUS_RECOVER_ON_BOOT (1)      /* 上电先补 9 个 SCL 脉冲解总线死锁 */

/* ============================ 器件寻址 ============================
 * 数据手册与编程指南对默认地址的说法不一致（0h vs 1h），以实测为准：
 * 本器件实为 0x01。*/
#define MCF_TARGET_ADDR_DEFAULT (0x01)
#define MCF_CRC_EN_DEFAULT      (0)   /* 1 = 所有事务启用 CRC-8 */

/* ============================ 时序参数 ============================ */
#define MCF_CMD_RETRY_MAX        (5)      /* NACK 重试次数 */
#define MCF_CMD_POLL_TIMEOUT_MS  (1000)   /* 命令回读 0xEA 的超时 */
#define MCF_EEPROM_READ_WAIT_MS  (200)    /* EEPROM 读出等待 */
#define MCF_EEPROM_WRITE_WAIT_MS (750)    /* EEPROM 写入等待 */
#define MCF_SYS_ENABLE_TIMEOUT_MS (2000)  /* 等 SYS_ENABLE_FLAG 的超时 */

/* ============================ 运行参数 ============================ */
#define MCF_SPEED_USE_OVERRIDE (1)    /* 1 = 走 ALGO_DEBUG1 的 OVERRIDE 位 */
#define MCF_STOP_ON_BOOT       (1)    /* 上电先无条件停机 */

/* FT 测速：每机械转的 FG 脉冲数 = 极对数 / FG_DIV。
 * 本电机 7 对极，器件 FG_DIV = 1h（CLOSED_LOOP1 0x88 b11-8），故为 7。
 * 改 FG_DIV 后这里必须同步改，否则转速显示会按倍数虚高/虚低。*/
#define FG_PULSES_PER_REV      (7.0f)

/* ============================ 日志 ============================ */
#define MCF_LOG_LEVEL     (3)   /* 0=关 1=错误 2=概要 3=详细 */
#define MCF_LOG_TIMESTAMP (1)

#include "config_opt.h"

#endif /* MCF_CONFIG_H */
