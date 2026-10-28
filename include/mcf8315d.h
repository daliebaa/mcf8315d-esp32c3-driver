#ifndef MCF8315D_H
#define MCF8315D_H

#include <Arduino.h>
#include "config.h"

/* ============================================================
 *  mcf8315d.h —— MCF8315D I2C 协议驱动
 *  依据：TI SLLSG06（数据手册）/ SLLA664（EEPROM 读写）/ SLLA662（I2C 编程）
 *  所有命令字节、位定义、CRC 值均已对照官方文档核对
 *
 *  编译环境：PlatformIO + espressif32 + Arduino core 2.0.17（-std=gnu++11）
 * ============================================================ */

/* ---- 统一日志宏：MCF_LOG(级别, 格式, ...) ---- */
namespace mcf {
inline void logPrefix(uint8_t lvl) {
#if MCF_LOG_TIMESTAMP
    Serial.printf("[%8lu] ", (unsigned long)millis());
#endif
    Serial.printf("[%s] ", lvl >= 4 ? "FRM" : (lvl == 3 ? "INF" : (lvl == 2 ? "SUM" : "ERR")));
}
}  // namespace mcf

#define MCF_LOG(lvl, ...)                            \
    do {                                             \
        if ((MCF_LOG_LEVEL) >= (lvl)) {              \
            mcf::logPrefix(lvl);                     \
            Serial.printf(__VA_ARGS__);              \
        }                                            \
    } while (0)

namespace mcf {

/* ---------- 数据长度 DLEN（控制字 CW21-CW20）---------- */
enum Dlen {
    DLEN_16 = 0x0,  /* 00b = 16 bit */
    DLEN_32 = 0x1,  /* 01b = 32 bit */
    DLEN_64 = 0x2,  /* 10b = 64 bit（连续两次 32 位，第二地址自动 +2）*/
};

/* ---------- 错误码 ---------- */
enum Err {
    ERR_NONE = 0,
    ERR_NO_ACK,    /* 目标地址无应答 */
    ERR_TIMEOUT,   /* 读回字节数不足 */
    ERR_CRC,       /* CRC 校验不符 */
    ERR_LEN,       /* 数据长度与 DLEN 不符 */
    ERR_VERIFY,    /* 回读值不符 / 命令未归零 */
    ERR_ARG,       /* 参数非法 */
};

/* ---------- 影子 RAM 地址（MEM_SEC = MEM_PAGE = 0x0）---------- */
constexpr uint16_t REG_ISD_CONFIG        = 0x080;
constexpr uint16_t REG_REV_DRIVE_CONFIG  = 0x082;
constexpr uint16_t REG_MOTOR_STARTUP1    = 0x084;
constexpr uint16_t REG_MOTOR_STARTUP2    = 0x086;
constexpr uint16_t REG_CLOSED_LOOP1      = 0x088;
constexpr uint16_t REG_CLOSED_LOOP2      = 0x08A;
constexpr uint16_t REG_CLOSED_LOOP3      = 0x08C;
constexpr uint16_t REG_CLOSED_LOOP4      = 0x08E;
constexpr uint16_t REG_FAULT_CONFIG1     = 0x090;
constexpr uint16_t REG_FAULT_CONFIG2     = 0x092;
constexpr uint16_t REG_SPEED_PROFILES1   = 0x094;  /* 数据手册里也叫 REF_PROFILES1 */
constexpr uint16_t REG_INT_ALGO_1        = 0x0A0;
constexpr uint16_t REG_INT_ALGO_2        = 0x0A2;
constexpr uint16_t REG_PIN_CONFIG        = 0x0A4;
constexpr uint16_t REG_DEVICE_CONFIG1    = 0x0A6;
constexpr uint16_t REG_DEVICE_CONFIG2    = 0x0A8;
constexpr uint16_t REG_PERI_CONFIG1      = 0x0AA;
constexpr uint16_t REG_GD_CONFIG1        = 0x0AC;
constexpr uint16_t REG_GD_CONFIG2        = 0x0AE;

/* ---------- 状态与控制寄存器 ---------- */
constexpr uint16_t REG_GATE_DRIVER_FAULT = 0x0E0;
constexpr uint16_t REG_CONTROLLER_FAULT  = 0x0E2;
constexpr uint16_t REG_ALGO_STATUS       = 0x0E4;
constexpr uint16_t REG_MTR_PARAMS        = 0x0E6;
constexpr uint16_t REG_ALGO_STATUS_MPET  = 0x0E8;
constexpr uint16_t REG_ALGO_CTRL1        = 0x0EA;
constexpr uint16_t REG_ALGO_DEBUG1       = 0x0EC;
constexpr uint16_t REG_ALGO_DEBUG2       = 0x0EE;
constexpr uint16_t REG_CURRENT_PI        = 0x0F0;
constexpr uint16_t REG_SPEED_PI          = 0x0F2;
constexpr uint16_t REG_DAC_1             = 0x0F4;
constexpr uint16_t REG_DAC_2             = 0x0F6;
constexpr uint16_t REG_EEPROM_SECURITY   = 0x0F8;
constexpr uint16_t REG_EEPROM_FAULT      = 0x32C;  /* 16 位 */

/* ---------- ALGO_CTRL1(0xEA) 命令字 ---------- */
constexpr uint32_t CMD_EEPROM_WRITE  = 0x8A500000UL;  /* bit31 + 访问密钥 0xA5 */
constexpr uint32_t CMD_EEPROM_READ   = 0x40000000UL;  /* bit30 */
constexpr uint32_t CMD_CLR_FLT       = 0x30000000UL;  /* bit29 + bit28 */
constexpr uint32_t CMD_CLR_RETRY     = 0x10000000UL;  /* bit28 */
constexpr uint32_t CMD_WDT_TICKLE    = 0x00000400UL;  /* bit10 */
constexpr uint32_t CMD_STOP          = 0x80000000UL;  /* 写 0xEC：OVERRIDE=1 + 速度 0 */

/* ---------- 状态位 ---------- */
constexpr uint32_t STS_SYS_ENABLE_FLAG = (1UL << 2);  /* ALGO_STATUS(0xE4) bit2 */

/* ---------- 影子 RAM 范围 ---------- */
constexpr uint16_t SHADOW_FIRST = 0x080;
constexpr uint16_t SHADOW_LAST  = 0x0AE;


/* ---------------- 协议层 API ---------------- */

/* 初始化总线（含可选的总线恢复），返回 false 表示参数非法 */
bool busBegin();

/* 扫描 0x00–0x7F，命中地址写入 found[]，返回命中数量 */
int scanBus(uint8_t *found, int maxFound);

/* 探测单个地址是否有器件应答。
 * 走 beginTransmission/endTransmission，不产生底层 Wire 报错，适合反复轮询。*/
bool probeAddress(uint8_t addr7);

/* 设定 / 读取当前目标地址 */
bool    setAddress(uint8_t addr7);
uint8_t address();

/* CRC 开关 */
void setCrcEnable(bool on);
bool crcEnabled();

/* 读写寄存器 */
bool writeReg(uint16_t addr, uint32_t value, uint8_t dlen = DLEN_32);
bool readReg(uint16_t addr, uint32_t &value, uint8_t dlen = DLEN_32);

/* 不重试的读（尝试 1 次）：给周期轮询用，避免器件掉线时刷屏 */
bool readRegOnce(uint16_t addr, uint32_t &value, uint8_t dlen = DLEN_32);

/* 向 ALGO_CTRL1(0xEA) 发命令
 * waitMs   : 命令后固定等待时间
 * waitZero : true 时轮询回读 0xEA 直到为 0（成功判据） */
bool sendCmd(uint32_t cmd, uint32_t waitMs, bool waitZero);

/* 常用操作 */
bool stopMotor();                       /* 写 0x80000000 -> 0xEC */
bool clearFaults();                     /* 写 0x30000000 -> 0xEA */
bool setSpeedPercent(float percent);    /* 写 ALGO_DEBUG1(0xEC) bit30-16，I2C 调速用 */

/* 读-改-写影子寄存器：只改 mask 指定的位，其余位（含 bit31 PARITY）原样保留。
 * 写 EEPROM 影子寄存器请一律用它，别用 writeReg 写整值，
 * 否则会冲掉其它字段和 PARITY。*/
bool modifyReg(uint16_t addr, uint32_t mask, uint32_t value);

/* 批量应用读-改-写配置表（对应 config_opt.h 的 MCF_STANDALONE_CFG）*/
bool applyCfgTable(const McfCfgEntry *tbl, size_t n, bool verify);

/* 错误信息 */
uint8_t     lastError();
const char *lastErrorText();
uint32_t    errorCount();
void        dumpLastFrame();            /* 打印最近一次失败帧（十六进制）*/

/* CRC-8（多项式 0x07，TI 协议用）：仅在 MCF_CRC_EN_DEFAULT = 1 时需要 */
uint8_t crc8(const uint8_t *data, size_t len);

}  // namespace mcf

#endif /* MCF8315D_H */
