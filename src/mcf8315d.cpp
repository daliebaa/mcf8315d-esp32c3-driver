#include "mcf8315d.h"

#if !I2C_USE_BITBANG
#include <Wire.h>
#endif

namespace mcf {

/* =====================================================================
 *  内部状态
 * ===================================================================== */
static uint8_t  g_addr    = 0x01;      /* 当前目标地址（7 位）*/
static bool     g_crc     = false;     /* 是否启用 CRC-8 */
static uint8_t  g_err     = ERR_NONE;
static uint32_t g_errCnt  = 0;

static uint8_t  g_lastFrame[16];
static size_t   g_lastLen = 0;
static size_t   g_rxLen   = 0;         /* 最近一次读事务收到的字节数 */

static void saveFrame(const uint8_t *f, size_t n) {
    memcpy(g_lastFrame, f, n);
    g_lastLen = n;
}

static uint8_t dataLenBytes(uint8_t dlen) {
    switch (dlen) {
        case DLEN_16: return 2;
        case DLEN_32: return 4;
        case DLEN_64: return 8;
        default:      return 0;
    }
}

/* =====================================================================
 *  1. CRC-8：x^8 + x^2 + x + 1（0x07），初值 0xFF，逐字节 MSB first
 * ===================================================================== */
uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* =====================================================================
 *  3. 总线底层
 * ===================================================================== */

#if I2C_USE_BITBANG
/* ---------------- 软件 I2C：可精确控制「字节间 >= 100us」 ---------------- */
#define BB_HALF_US ((I2C_SCL_FREQ_HZ) >= 200000UL ? 1 : (1000000UL / (2UL * (I2C_SCL_FREQ_HZ))))

static inline void bbSdaLow()  { pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN); digitalWrite(PIN_I2C_SDA, LOW); }
static inline void bbSdaFree() { pinMode(PIN_I2C_SDA, INPUT_PULLUP); }
static inline void bbSclLow()  { pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN); digitalWrite(PIN_I2C_SCL, LOW); }

/* 释放 SCL 并等待从机结束时钟拉伸（器件上限约 5ms）*/
static inline void bbSclFree() {
    pinMode(PIN_I2C_SCL, INPUT_PULLUP);
    uint32_t t0 = micros();
    while (digitalRead(PIN_I2C_SCL) == LOW) {
        if ((uint32_t)(micros() - t0) > 6000UL) break;
    }
}

static void bbStart() {
    bbSdaFree(); bbSclFree(); delayMicroseconds(BB_HALF_US);
    bbSdaLow();               delayMicroseconds(BB_HALF_US);
    bbSclLow();               delayMicroseconds(BB_HALF_US);
}

static void bbStop() {
    bbSdaLow();               delayMicroseconds(BB_HALF_US);
    bbSclFree();              delayMicroseconds(BB_HALF_US);
    bbSdaFree();              delayMicroseconds(BB_HALF_US);
}

static bool bbWriteByte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) bbSdaFree(); else bbSdaLow();
        delayMicroseconds(BB_HALF_US);
        bbSclFree();  delayMicroseconds(BB_HALF_US);
        bbSclLow();   delayMicroseconds(BB_HALF_US);
    }
    bbSdaFree();      delayMicroseconds(BB_HALF_US);   /* ACK 位 */
    bbSclFree();      delayMicroseconds(BB_HALF_US);
    bool ack = (digitalRead(PIN_I2C_SDA) == LOW);
    bbSclLow();       delayMicroseconds(BB_HALF_US);
    delayMicroseconds(I2C_BYTE_GAP_US);                /* 本器件硬要求：字节间隔 >=100us */
    return ack;
}

static uint8_t bbReadByte(bool sendAck) {
    uint8_t b = 0;
    bbSdaFree();
    for (int i = 7; i >= 0; i--) {
        delayMicroseconds(BB_HALF_US);
        bbSclFree();  delayMicroseconds(BB_HALF_US);
        if (digitalRead(PIN_I2C_SDA)) b |= (1u << i);
        bbSclLow();   delayMicroseconds(BB_HALF_US);
    }
    if (sendAck) bbSdaLow(); else bbSdaFree();
    delayMicroseconds(BB_HALF_US);
    bbSclFree();      delayMicroseconds(BB_HALF_US);
    bbSclLow();       delayMicroseconds(BB_HALF_US);
    bbSdaFree();
    delayMicroseconds(I2C_BYTE_GAP_US);
    return b;
}

static bool busWrite(const uint8_t *f, size_t n) {
    bbStart();
    for (size_t i = 0; i < n; i++) {
        if (!bbWriteByte(f[i])) { bbStop(); return false; }
    }
    bbStop();
    return true;
}

static bool busWriteRead(const uint8_t *f, size_t wn, uint8_t *rb, size_t rn) {
    bbStart();
    for (size_t i = 0; i < wn; i++) {
        if (!bbWriteByte(f[i])) { bbStop(); return false; }
    }
    bbStart();                                   /* 重复起始 */
    if (!bbWriteByte((uint8_t)((g_addr << 1) | 0x01))) { bbStop(); return false; }
    for (size_t i = 0; i < rn; i++) {
        rb[i] = bbReadByte(i + 1 < rn);          /* 最后一个字节回 NACK */
    }
    g_rxLen = rn;
    bbStop();
    return true;
}

static bool busProbe(uint8_t a) {
    bbStart();
    bool ack = bbWriteByte((uint8_t)((a << 1) | 0x00));
    bbStop();
    return ack;
}

#else
/* ---------------- 硬件 I2C（Wire）---------------- */

static bool busWrite(const uint8_t *f, size_t n) {
    Wire.beginTransmission(g_addr);
    Wire.write(f + 1, n - 1);                    /* f[0] 是「地址+W」，由 beginTransmission 产生 */
    return Wire.endTransmission(true) == 0;
}

static bool busWriteRead(const uint8_t *f, size_t wn, uint8_t *rb, size_t rn) {
    Wire.beginTransmission(g_addr);
    Wire.write(f + 1, wn - 1);
    if (Wire.endTransmission(false) != 0) return false;   /* false = 发重复起始，不发 STOP */
    size_t got = Wire.requestFrom((uint16_t)g_addr, (size_t)rn, (bool)true);
    g_rxLen = got;
    if (got != rn) return false;
    for (size_t i = 0; i < rn; i++) rb[i] = (uint8_t)Wire.read();
    return true;
}

static bool busProbe(uint8_t a) {
    Wire.beginTransmission(a);
    return Wire.endTransmission(true) == 0;
}

#if I2C_BUS_RECOVER_ON_BOOT
/* 上电补 9 个 SCL 脉冲，解开「从机把 SDA 拉死」的总线死锁 */
static void i2cBusRecover() {
    pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(10);
    for (int i = 0; i < 9 && digitalRead(PIN_I2C_SDA) == LOW; i++) {
        digitalWrite(PIN_I2C_SCL, LOW);  delayMicroseconds(10);
        digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(10);
    }
    pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);     /* 补一个 STOP */
    digitalWrite(PIN_I2C_SDA, LOW);   delayMicroseconds(10);
    digitalWrite(PIN_I2C_SCL, HIGH);  delayMicroseconds(10);
    digitalWrite(PIN_I2C_SDA, HIGH);  delayMicroseconds(10);
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
}
#endif
#endif   /* I2C_USE_BITBANG */

/* =====================================================================
 *  4. 对外 API
 * ===================================================================== */

bool busBegin() {
    if (PIN_I2C_SDA == PIN_UNUSED || PIN_I2C_SCL == PIN_UNUSED) {
        g_err = ERR_ARG;
        return false;
    }
#if I2C_USE_BITBANG
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, INPUT_PULLUP);
#else
#if I2C_BUS_RECOVER_ON_BOOT
    i2cBusRecover();
#endif
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_SCL_FREQ_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);
#endif
    g_addr = MCF_TARGET_ADDR_DEFAULT;
    g_crc  = (MCF_CRC_EN_DEFAULT != 0);
    g_err  = ERR_NONE;
    return true;
}

int scanBus(uint8_t *found, int maxFound) {
    int n = 0;
    for (uint16_t a = 0x00; a <= 0x7F; a++) {
        if (busProbe((uint8_t)a)) {
            if (n < maxFound) found[n] = (uint8_t)a;
            n++;
        }
    }
    return n;
}

bool probeAddress(uint8_t addr7) { return busProbe((uint8_t)(addr7 & 0x7F)); }

bool    setAddress(uint8_t addr7) { g_addr = (uint8_t)(addr7 & 0x7F); return true; }
uint8_t address()                 { return g_addr; }
void    setCrcEnable(bool on)     { g_crc = on; }
bool    crcEnabled()              { return g_crc; }

/* ---------------- 写寄存器 ---------------- */
bool writeReg(uint16_t addr, uint32_t value, uint8_t dlen) {
    uint8_t nData = dataLenBytes(dlen);
    if (nData == 0) { g_err = ERR_LEN; return false; }

    uint8_t frame[16];
    size_t  n = 0;
    frame[n++] = (uint8_t)((g_addr << 1) | 0x00);              /* 首字节 R/W 恒为 0 */
    frame[n++] = (uint8_t)((0u << 7) | (g_crc ? 0x40u : 0u) | ((uint32_t)dlen << 4));
    frame[n++] = (uint8_t)((addr >> 8) & 0x0F);                /* MEM_PAGE = 0 */
    frame[n++] = (uint8_t)(addr & 0xFF);
    for (uint8_t i = 0; i < nData; i++) {                      /* 数据 LSB 字节先发 */
        frame[n++] = (uint8_t)((value >> (8u * i)) & 0xFFu);
    }
    if (g_crc) frame[n++] = crc8(frame, n);
    saveFrame(frame, n);

    for (uint8_t t = 0; t <= MCF_CMD_RETRY_MAX; t++) {
        if (busWrite(frame, n)) { g_err = ERR_NONE; return true; }
        g_err = ERR_NO_ACK;
        delayMicroseconds(200);
    }
    g_errCnt++;
    return false;
}

/* ---------------- 读寄存器（核心） ----------------
 * attempts = 尝试次数；=1 表示不重试（给周期轮询用，失败时不刷屏）*/
static bool readRegCore(uint16_t addr, uint32_t &value, uint8_t dlen, uint8_t attempts) {
    uint8_t nData = dataLenBytes(dlen);
    if (nData == 0) { g_err = ERR_LEN; return false; }

    uint8_t frame[4];
    frame[0] = (uint8_t)((g_addr << 1) | 0x00);
    frame[1] = (uint8_t)((1u << 7) | (g_crc ? 0x40u : 0u) | ((uint32_t)dlen << 4));  /* OP_R/W = 1 读 */
    frame[2] = (uint8_t)((addr >> 8) & 0x0F);
    frame[3] = (uint8_t)(addr & 0xFF);
    saveFrame(frame, sizeof(frame));

    uint8_t rb[9];
    size_t  want = (size_t)nData + (g_crc ? 1u : 0u);

    for (uint8_t t = 0; t < attempts; t++) {
        g_rxLen = 0;
        if (!busWriteRead(frame, sizeof(frame), rb, want)) {
            g_err = (g_rxLen != 0) ? ERR_TIMEOUT : ERR_NO_ACK;
            continue;
        }
        if (g_crc) {                                        /* 读事务 CRC 含「地址+读」字节 */
            uint8_t chk[16];
            size_t  m;
            memcpy(chk, frame, sizeof(frame));
            m = sizeof(frame);
            chk[m++] = (uint8_t)((g_addr << 1) | 0x01);
            memcpy(chk + m, rb, nData);
            m += nData;
            if (crc8(chk, m) != rb[nData]) { g_err = ERR_CRC; continue; }
        }
        value = 0;
        for (uint8_t i = 0; i < nData; i++) {
            value |= ((uint32_t)rb[i]) << (8u * i);
        }
        g_err = ERR_NONE;
        return true;
    }
    g_errCnt++;
    return false;
}

bool readReg(uint16_t addr, uint32_t &value, uint8_t dlen) {
    return readRegCore(addr, value, dlen, (uint8_t)(MCF_CMD_RETRY_MAX + 1));
}

/* 只尝试一次的读：给周期轮询用。
 * 器件没接时每次失败会在底层打一条 Wire 报错，用这个能把刷屏降到最低。*/
bool readRegOnce(uint16_t addr, uint32_t &value, uint8_t dlen) {
    return readRegCore(addr, value, dlen, 1);
}

/* ---------------- 命令 ---------------- */
bool sendCmd(uint32_t cmd, uint32_t waitMs, bool waitZero) {
    if (!writeReg(REG_ALGO_CTRL1, cmd, DLEN_32)) return false;
    if (waitMs) delay(waitMs);
    if (!waitZero) return true;

    uint32_t t0 = millis();
    uint32_t v  = 0xFFFFFFFFUL;
    while ((uint32_t)(millis() - t0) < MCF_CMD_POLL_TIMEOUT_MS) {
        if (readReg(REG_ALGO_CTRL1, v, DLEN_32) && v == 0) return true;
        delay(5);
    }
    g_err = ERR_VERIFY;
    g_errCnt++;
    return false;
}

bool stopMotor()      { return writeReg(REG_ALGO_DEBUG1, CMD_STOP, DLEN_32); }
bool clearFaults()    { return sendCmd(CMD_CLR_FLT, 0, true); }

bool setSpeedPercent(float percent) {
    if (percent < 0.0f)   percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    uint32_t v = (uint32_t)(percent * 32768.0f / 100.0f + 0.5f);
    if (v > 0x7FFFu) v = 0x7FFFu;
    uint32_t word = ((v & 0x7FFFu) << 16);          /* DIGITAL_SPEED_CTRL = bit30-16 */
#if MCF_SPEED_USE_OVERRIDE
    word |= (1UL << 31);                            /* OVERRIDE = bit31 */
#endif
    return writeReg(REG_ALGO_DEBUG1, word, DLEN_32);
}

/* ---------------- 读-改-写影子寄存器 ----------------
 * 只改 mask 指定的位；bit31(PARITY) 一律保留读回的原值。
 * 注意：数据手册没有给出 PARITY 的计算方法（只写了字段名叫 "Parity bit"），
 *       所以这里不自己算，保留原值，写完后回读验证 + 查 EEPROM_ERR_STATUS。*/
bool modifyReg(uint16_t addr, uint32_t mask, uint32_t value) {
    uint32_t old = 0;
    if (!readReg(addr, old, DLEN_32)) return false;

    uint32_t next = (old & ~mask) | (value & mask);
    next = (next & 0x7FFFFFFFUL) | (old & 0x80000000UL);   /* 保留 PARITY */

    if (next == old) {                                      /* 没变化，不用写 */
        MCF_LOG(4, "  0x%03X 无需修改（已是目标值）\n", (unsigned)addr);
        return true;
    }
    if (!writeReg(addr, next, DLEN_32)) return false;

    uint32_t chk = 0;
    if (readReg(addr, chk, DLEN_32)) {
        uint32_t want = (next & ~mask) | (value & mask);
        bool ok = ((chk & ~mask & 0x7FFFFFFFUL) == (want & ~mask & 0x7FFFFFFFUL));
        if (!ok) { g_err = ERR_VERIFY; return false; }
    }
    return true;
}

bool applyCfgTable(const McfCfgEntry *tbl, size_t n, bool verify) {
    bool allOk = true;
    for (size_t i = 0; i < n; i++) {
        const McfCfgEntry &e = tbl[i];
        bool ok = modifyReg(e.addr, e.mask, e.value);
        if (!ok) allOk = false;
        MCF_LOG(ok ? 3 : 1, "  配置[%u] 0x%03X [%s] %s\n",
                (unsigned)(i + 1), (unsigned)e.addr, e.note ? e.note : "",
                ok ? "OK" : "FAIL");
        if (!ok) {
            MCF_LOG(1, "      失败原因：%s\n", lastErrorText());
        }
        (void)verify;                                        /* modifyReg 内部已回读验证 */
    }
    return allOk;
}

/* ---------------- 错误信息 ---------------- */
uint8_t  lastError()  { return g_err; }
uint32_t errorCount() { return g_errCnt; }

const char *lastErrorText() {
    switch (g_err) {
        case ERR_NONE:    return "OK";
        case ERR_NO_ACK:  return "无应答（地址不对 / 器件未上电 / 上拉缺失）";
        case ERR_TIMEOUT: return "读回字节数不足（超时或时钟拉伸过长）";
        case ERR_CRC:     return "CRC 校验不符";
        case ERR_LEN:     return "数据长度与 DLEN 不符";
        case ERR_VERIFY:  return "命令未归零 / 回读值不符";
        case ERR_ARG:     return "参数非法";
        default:          return "未知错误";
    }
}

void dumpLastFrame() {
    Serial.printf("  最近一次帧(%u 字节)：", (unsigned)g_lastLen);
    for (size_t i = 0; i < g_lastLen; i++) Serial.printf(" %02X", g_lastFrame[i]);
    Serial.println();
}

}  // namespace mcf
