/* =============================================================================
 *  MCF8315D + ESP32-C3  无刷电机驱动固件
 * -----------------------------------------------------------------------------
 *  功能：
 *    · 用 LEDC 在 SPEED 引脚输出 5 kHz PWM 作为速度指令
 *      占空比 0~100% 线性对应 0~MAX_SPEED（本工程 600 Hz 电频率 ≈ 5143 RPM）
 *    · FG 引脚脉冲计数测转速，nFAULT 下降沿中断即时打印故障
 *    · I2C 配置器件：上电自动下发参数表；串口 P 一键写入 EEPROM 固化
 *    · 串口控制台：输入 0-100 设转速百分比，h 查看全部命令
 *
 *  协议依据：
 *    MCF8315D 数据手册 SLLSG06 / I2C 编程指南 SLLA662 / EEPROM 读写 SLLA664
 *
 *  硬件连接（详见 include/config.h）：
 *    SDA=GPIO7  SCL=GPIO8  SPEED=GPIO5  nFAULT=GPIO10  FG=GPIO6
 * ========================================================================== */

#include "mcf8315d.h"
#include <esp_system.h>

/* ============================ 可调常量 ============================ */
#define FG_WINDOW_MS            (1000UL)  /* 测速采样窗口 */
#define DEVICE_PROBE_PERIOD_MS  (2000UL)  /* 掉线后探测默认地址的间隔 */
#define DEVICE_FULL_SCAN_MS     (10000UL) /* 全总线扫描的间隔 */
#define DEVICE_LOST_TOLERANCE   (3)       /* 连续读失败几次判定掉线 */

/* ============================ 运行状态 ============================ */
static volatile uint32_t g_fgPulses     = 0;
static uint32_t          g_fgLastPulses = 0;
static uint32_t          g_fgLastTick   = 0;
static float             g_fgRpm        = 0.0f;
static volatile bool     g_nfaultFlag   = false;

static bool     g_devFound  = false;
static bool     g_hasFault  = false;
static uint8_t  g_missCount = 0;
static uint8_t  g_retry     = 0;      /* 启动失败自动重试计数 */
static uint32_t g_lastFaultMs = 0;
static float    g_speedPct  = 0.0f;
static uint32_t g_bootMs    = 0;

static void IRAM_ATTR fgISR()     { g_fgPulses++; }
static void IRAM_ATTR nfaultISR() { g_nfaultFlag = true; }

/* 区分"正常上电"和"程序崩溃/欠压复位"：电机莫名停转时这是第一手线索 */
static const char *resetReasonText() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON（正常上电）";
        case ESP_RST_EXT:       return "EXT（外部复位脚）";
        case ESP_RST_SW:        return "SW（软件复位/烧录）";
        case ESP_RST_PANIC:     return "PANIC（程序异常）";
        case ESP_RST_INT_WDT:   return "INT_WDT（中断看门狗）";
        case ESP_RST_TASK_WDT:  return "TASK_WDT（任务看门狗）";
        case ESP_RST_WDT:       return "WDT（其他看门狗）";
        case ESP_RST_BROWNOUT:  return "BROWNOUT（欠压复位）";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP（深睡唤醒）";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

/* ============================ 故障解码 ============================ */
struct BitName {
    uint8_t     bit;
    const char *name;
};

static const BitName GD_BITS[] = {
    { 31, "DRIVER_FAULT" }, { 28, "OCP" },     { 26, "OVP" },
    { 23, "OTW" },          { 22, "OTS" },
    { 21, "OCP_HC" },       { 20, "OCP_LC" },  { 19, "OCP_HB" },
    { 18, "OCP_LB" },       { 17, "OCP_HA" },  { 16, "OCP_LA" },
    { 13, "BUCK_OCP" },     { 12, "BUCK_UV" }, { 11, "VCP_UV" },
};

static const BitName CTL_BITS[] = {
    { 31, "CONTROLLER_FAULT" },      { 29, "IPD_FREQ_FAULT" },
    { 28, "IPD_T1_FAULT" },          { 27, "IPD_T2_FAULT" },
    { 25, "MPET_IPD_FAULT" },        { 24, "MPET_BEMF_FAULT" },
    { 23, "ABN_SPEED" },             { 22, "ABN_BEMF" },
    { 21, "NO_MTR" },                { 20, "MTR_LCK" },
    { 19, "LOCK_LIMIT" },            { 18, "HW_LOCK_LIMIT" },
    { 17, "MTR_UNDER_VOLTAGE" },     { 16, "MTR_OVER_VOLTAGE" },
    { 15, "SPEED_LOOP_SATURATION" }, { 14, "CURRENT_LOOP_SATURATION" },
    { 13, "MAX_SPEED_SATURATION" },  { 12, "BUS_POWER_LIMIT_SATURATION" },
    { 11, "EEPROM_WRITE_LOCK_SET" }, { 10, "EEPROM_READ_LOCK_SET" },
    { 6,  "I2C_CRC_FAULT_STATUS" },  { 5,  "EEPROM_ERR_STATUS" },
    { 3,  "WATCHDOG_FAULT" },
};

static const BitName EE_BITS[] = {
    { 4, "EEPROM_CRC_FLT_STS" }, { 2, "EEPROM_PARITY_FLT_STS" },
};

/* CONTROLLER_FAULT 的 bit15-12 是只读的"饱和提示位"，不是故障：
 * 速度环/电流环饱和、达到 MAX_SPEED 上限时都会置 1，属于正常工况。 */
#define CTL_FAULT_MASK (0xFFFF0FFFUL)

static void printBits(const char *title, uint32_t v, const BitName *t, size_t n) {
    Serial.printf("  %-26s = 0x%08lX\n", title, (unsigned long)v);
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (v & (1UL << t[i].bit)) {
            Serial.printf("        bit%-2u  %s\n", t[i].bit, t[i].name);
            any = true;
        }
    }
    if (!any) Serial.println("        （无故障位）");
}

/* 读并解码故障。doClear = true 时清故障并复读确认 */
static bool faultSnapshot(bool doClear) {
    uint32_t gd = 0, ct = 0, ee = 0, st = 0;

    if (!mcf::readReg(mcf::REG_GATE_DRIVER_FAULT, gd, mcf::DLEN_32) ||
        !mcf::readReg(mcf::REG_CONTROLLER_FAULT,  ct, mcf::DLEN_32)) {
        MCF_LOG(1, "读故障寄存器失败：%s\n", mcf::lastErrorText());
        mcf::dumpLastFrame();
        return false;
    }
    mcf::readReg(mcf::REG_EEPROM_FAULT, ee, mcf::DLEN_16);

    printBits("GATE_DRIVER_FAULT(0xE0)", gd, GD_BITS, sizeof(GD_BITS) / sizeof(GD_BITS[0]));
    printBits("CONTROLLER_FAULT(0xE2)",  ct, CTL_BITS, sizeof(CTL_BITS) / sizeof(CTL_BITS[0]));
    printBits("EEPROM_FAULT(0x32C)",     ee, EE_BITS, sizeof(EE_BITS) / sizeof(EE_BITS[0]));

    g_hasFault = (gd != 0) || (ct & CTL_FAULT_MASK) || (ee != 0);

    if (mcf::readReg(mcf::REG_ALGO_STATUS, st, mcf::DLEN_32)) {
        float duty = (float)((st >> 4) & 0x0FFF) / 4095.0f * 100.0f;
        float vmag = (float)((st >> 16) & 0xFFFF) / 32768.0f * 100.0f;
        Serial.printf("  ALGO_STATUS(0xE4)          = 0x%08lX  DUTY_CMD=%.1f%%  调制指数=%.1f%%\n",
                      (unsigned long)st, duty, vmag);
    }

    if (doClear && g_hasFault) {
        if (mcf::clearFaults()) {
            delay(220);                      /* 锁存故障最长 200ms 后真正清除 */
            uint32_t ct2 = 0, gd2 = 0;
            mcf::readReg(mcf::REG_CONTROLLER_FAULT, ct2, mcf::DLEN_32);
            mcf::readReg(mcf::REG_GATE_DRIVER_FAULT, gd2, mcf::DLEN_32);
            if (((ct2 & CTL_FAULT_MASK) == 0) && gd2 == 0) {
                Serial.println("  故障已清除。");
                g_hasFault = false;
            } else {
                Serial.println("  仍有故障位未清（持续性故障，先查硬件）：");
                printBits("CONTROLLER_FAULT(0xE2)", ct2, CTL_BITS, sizeof(CTL_BITS) / sizeof(CTL_BITS[0]));
            }
        } else {
            MCF_LOG(1, "清故障命令失败：%s\n", mcf::lastErrorText());
            mcf::dumpLastFrame();
        }
    }
    return true;
}

/* ============================ 影子寄存器 dump ============================ */
static const char *shadowName(uint16_t a) {
    switch (a) {
        case 0x080: return "ISD_CONFIG";
        case 0x082: return "REV_DRIVE_CONFIG";
        case 0x084: return "MOTOR_STARTUP1";
        case 0x086: return "MOTOR_STARTUP2";
        case 0x088: return "CLOSED_LOOP1";
        case 0x08A: return "CLOSED_LOOP2";
        case 0x08C: return "CLOSED_LOOP3";
        case 0x08E: return "CLOSED_LOOP4";
        case 0x090: return "FAULT_CONFIG1";
        case 0x092: return "FAULT_CONFIG2";
        case 0x094: return "SPEED_PROFILES1";
        case 0x096: return "SPEED_PROFILES2";
        case 0x098: return "SPEED_PROFILES3";
        case 0x09A: return "SPEED_PROFILES4";
        case 0x09C: return "SPEED_PROFILES5";
        case 0x09E: return "SPEED_PROFILES6";
        case 0x0A0: return "INT_ALGO_1";
        case 0x0A2: return "INT_ALGO_2";
        case 0x0A4: return "PIN_CONFIG";
        case 0x0A6: return "DEVICE_CONFIG1";
        case 0x0A8: return "DEVICE_CONFIG2";
        case 0x0AA: return "PERI_CONFIG1";
        case 0x0AC: return "GD_CONFIG1";
        case 0x0AE: return "GD_CONFIG2";
        default:    return "?";
    }
}

static int shadowDump() {
    int fail = 0;
    Serial.println("  地址  寄存器名              值");
    for (uint16_t a = mcf::SHADOW_FIRST; a <= mcf::SHADOW_LAST; a += 2) {
        uint32_t v = 0;
        if (mcf::readReg(a, v, mcf::DLEN_32)) {
            Serial.printf("  0x%03X %-20s 0x%08lX\n", a, shadowName(a), (unsigned long)v);
        } else {
            Serial.printf("  0x%03X %-20s 读失败：%s\n", a, shadowName(a), mcf::lastErrorText());
            fail++;
        }
    }
    return fail;
}

/* ============================ 测速 / 停机 / 调速 ============================ */
static uint32_t speedPwmDuty(float pct) {
#if EN_SPEED_PWM && (PIN_MCF_SPEED != PIN_UNUSED)
    const uint32_t maxd = (1UL << SPEED_PWM_BITS) - 1UL;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    uint32_t d = (uint32_t)(pct / 100.0f * (float)maxd + 0.5f);
    return d > maxd ? maxd : d;
#else
    (void)pct;
    return 0;
#endif
}

static bool waitSysEnable() {
    uint32_t t0 = millis();
    uint32_t v  = 0;
    while ((uint32_t)(millis() - t0) < MCF_SYS_ENABLE_TIMEOUT_MS) {
        if (mcf::readReg(mcf::REG_ALGO_STATUS, v, mcf::DLEN_32) && (v & mcf::STS_SYS_ENABLE_FLAG)) {
            MCF_LOG(2, "SYS_ENABLE_FLAG = 1，器件就绪（%lums）\n", (unsigned long)(millis() - t0));
            return true;
        }
        delay(20);
    }
    MCF_LOG(1, "等 SYS_ENABLE_FLAG 超时：器件可能仍在从 EEPROM 拷贝参数，或未正常上电\n");
    return false;
}

static void doStop() {
#if EN_SPEED_PWM && (PIN_MCF_SPEED != PIN_UNUSED)
    /* 占空比降到最小保活值：SPEED 不能一直为低，否则超过 SLEEP_ENTRY_TIME
     * 器件会进 sleep，AVDD 关闭 → 上拉和 I2C 一起失联。
     * 同时清掉 I2C OVERRIDE，否则器件会继续用数字速度、无视 SPEED 引脚。 */
    g_speedPct = 0.0f;
    g_retry    = 0;
    ledcWrite(SPEED_PWM_CHANNEL, speedPwmDuty(SPEED_PWM_MIN_PCT));
    if (!mcf::writeReg(mcf::REG_ALGO_DEBUG1, 0, mcf::DLEN_32)) {
        MCF_LOG(1, "关闭 OVERRIDE 失败：%s\n", mcf::lastErrorText());
    }
#else
    if (mcf::stopMotor()) g_speedPct = 0.0f;
#endif
}

static void doSpeed(float pct) {
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
#if EN_SPEED_PWM && (PIN_MCF_SPEED != PIN_UNUSED)
    float dutyPct = (pct < SPEED_PWM_MIN_PCT) ? SPEED_PWM_MIN_PCT : pct;
    ledcWrite(SPEED_PWM_CHANNEL, speedPwmDuty(dutyPct));
    g_speedPct = pct;
    MCF_LOG(2, "速度 %.1f%%（PWM 占空比 %.1f%%）\n", pct, dutyPct);
#else
    if (mcf::setSpeedPercent(pct)) g_speedPct = pct;
#endif
}

static void fgUpdate() {
    uint32_t now = millis();
    if ((uint32_t)(now - g_fgLastTick) < FG_WINDOW_MS) return;
    uint32_t p = g_fgPulses;
    uint32_t d = p - g_fgLastPulses;
    g_fgLastPulses = p;
    g_fgLastTick   = now;
    float hz = (float)d * 1000.0f / (float)FG_WINDOW_MS;
    g_fgRpm = (FG_PULSES_PER_REV > 0.0f) ? (hz / FG_PULSES_PER_REV * 60.0f) : 0.0f;
}

/* ============================ 配置下发 / 固化 ============================ */
/* 把 MCF_STANDALONE_CFG 逐项读-改-写进影子 RAM。
 * 只改 mask 指定的位，bit31 PARITY 和其它字段原样保留。 */
static bool applyConfigTable(bool verbose) {
    size_t n = sizeof(MCF_STANDALONE_CFG) / sizeof(MCF_STANDALONE_CFG[0]);
    if (verbose) Serial.println(">> 下发参数表");
    bool ok = mcf::applyCfgTable(MCF_STANDALONE_CFG, n, verbose);
    if (verbose) Serial.printf("   %u 项，%s\n", (unsigned)n, ok ? "全部成功" : "有失败项");
    return ok;
}

/* 15 秒内输入大写 W 回车 = 确认，其它任意键 = 取消。
 * EEPROM 写错会把器件锁在无效配置里，所以保留这道闸。 */
static bool askConfirm(const char *what) {
    Serial.printf("\n>>> 即将执行：%s\n", what);
    Serial.println(">>> 请确认：1) 电机已停  2) VM 正常供电  3) 15 秒内输入大写 W 回车确认");
    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 15000UL) {
        if (Serial.available()) {
            char c = (char)Serial.read();
            if (c == 'W') {
                while (Serial.available()) Serial.read();
                Serial.println(">>> 已确认。");
                return true;
            }
            if (c == '\n' || c == '\r') continue;
            while (Serial.available()) Serial.read();
            Serial.println(">>> 已取消。");
            return false;
        }
        delay(10);
    }
    Serial.println(">>> 超时，已取消。");
    return false;
}

/* 写 EEPROM 并回读校验。写寿命 20000 次，别频繁调用。 */
static bool eepromWriteCore() {
    Serial.printf("   写 0x8A500000 -> 0xEA，等待 %lums ...\n",
                  (unsigned long)MCF_EEPROM_WRITE_WAIT_MS);
    if (!mcf::sendCmd(mcf::CMD_EEPROM_WRITE, MCF_EEPROM_WRITE_WAIT_MS, true)) {
        MCF_LOG(1, "   EEPROM 写入失败：%s\n", mcf::lastErrorText());
        mcf::dumpLastFrame();
        return false;
    }
    Serial.println("   命令已归零。");

    /* 再发一次 EEPROM 读，让器件把刚写的内容读回来做 CRC/parity 校验 */
    if (!mcf::sendCmd(mcf::CMD_EEPROM_READ, MCF_EEPROM_READ_WAIT_MS, true)) {
        MCF_LOG(1, "   EEPROM 回读失败：%s\n", mcf::lastErrorText());
        return false;
    }
    uint32_t ee = 0;
    if (mcf::readReg(mcf::REG_EEPROM_FAULT, ee, mcf::DLEN_16)) {
        if (ee & ((1UL << 4) | (1UL << 2))) {
            MCF_LOG(1, "   !! EEPROM 校验报错：%s%s\n",
                    (ee & (1UL << 4)) ? "CRC " : "", (ee & (1UL << 2)) ? "PARITY" : "");
            return false;
        }
        Serial.println("   EEPROM 校验通过（无 CRC / parity 故障）。");
    }
    return true;
}

/* 一键固化：停机 -> 下发参数 -> 写 EEPROM -> 校验。
 * 换片、重焊、EEPROM 被高温破坏之后跑一次即可恢复。 */
static void doProvision() {
    if (!g_devFound) { Serial.println("  器件不在线，先检查接线或输入 s 重新扫描"); return; }
    Serial.println(">> 固化流程：停机 -> 下发参数表 -> 写 EEPROM -> 校验");
    if (!askConfirm("把当前参数写入器件 EEPROM（写寿命 20000 次）")) return;

    doStop();
    delay(50);                               /* 改影子寄存器要求器件处于 idle */
    if (!applyConfigTable(true)) {
        Serial.println("   参数下发有失败项，已中止（EEPROM 未写入）。");
        return;
    }
    if (!eepromWriteCore()) {
        Serial.println("   固化失败，EEPROM 未更新。");
        return;
    }
    Serial.println("   完成。断电重启后新配置生效。");
}

/* ============================ 批量烧录（产线）============================ */
#if EN_PROVISION_MODE
/* 状态机：等芯片 → 烧录 → 等移走 → 等芯片 …
 * "移走"用连续 3 次探测无应答判定，避免 I2C 抖动误判成换片。 */
static uint32_t g_provOk    = 0;
static uint32_t g_provFail  = 0;
static uint32_t g_batchTick = 0;

/* 先探默认地址；找不到再每 3 秒全扫一次（新片的出厂地址不一定是 0x01） */
static bool findChip(uint8_t &addr) {
    if (mcf::probeAddress(MCF_TARGET_ADDR_DEFAULT)) {
        addr = MCF_TARGET_ADDR_DEFAULT;
        return true;
    }
    static uint32_t tScan = 0;
    if ((uint32_t)(millis() - tScan) < 3000UL) return false;
    tScan = millis();
    uint8_t found[4];
    if (mcf::scanBus(found, 4) > 0) { addr = found[0]; return true; }
    return false;
}

/* 烧录一片：等就绪 → 停机 → 下发配置 → 写 EEPROM → 回读校验 */
static bool provisionOneChip() {
    if (!waitSysEnable()) return false;
    doStop();
    delay(50);
    if (!applyConfigTable(true))       return false;
    if (!eepromWriteCore())            return false;

    uint32_t ee = 0, v = 0;
    if (mcf::readReg(mcf::REG_EEPROM_FAULT, ee, mcf::DLEN_16) &&
        (ee & ((1UL << 4) | (1UL << 2)))) {
        MCF_LOG(1, "   EEPROM 故障位未清（0x%04lX）\n", (unsigned long)ee);
        return false;
    }
    if (!mcf::readReg(mcf::REG_CLOSED_LOOP4, v, mcf::DLEN_32)) return false;
    if ((v & 0x00003FFFUL) != ((uint32_t)CFG_MAX_SPEED_ELEC_HZ * 6UL)) {
        MCF_LOG(1, "   MAX_SPEED 回读不符（0x%08lX）\n", (unsigned long)v);
        return false;
    }
    return true;
}

static void batchTask() {
    static uint8_t st = 0;          /* 0 = 等芯片，1 = 等移走 */
    static uint8_t miss = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - g_batchTick) < 300UL) return;
    g_batchTick = now;

    uint8_t addr = 0;
    bool present = findChip(addr);

    if (!present) {
        if (st == 1 && ++miss >= 3) {
            st   = 0;
            miss = 0;
            g_devFound = false;
            Serial.println();
            Serial.println("----------------------------------------");
            Serial.println("  请放下一片（换片后自动开始烧录）");
            Serial.printf("  累计：成功 %lu 片，失败 %lu 片\n",
                          (unsigned long)g_provOk, (unsigned long)g_provFail);
            Serial.println("----------------------------------------");
        }
        return;
    }
    miss = 0;
    if (st == 1) return;              /* 同一片仍在位，不重复写，省 EEPROM 寿命 */

    mcf::setAddress(addr);
    g_devFound = true;
    Serial.println();
    Serial.println("========================================");
    Serial.printf(" 检测到器件 0x%02X，开始烧录第 %lu 片\n",
                  addr, (unsigned long)(g_provOk + g_provFail + 1));
    Serial.println("========================================");

    bool ok = provisionOneChip();
    if (ok) { g_provOk++;   Serial.println(">>> 结果：成功  PASS  <<<"); }
    else    { g_provFail++; Serial.println(">>> 结果：失败  FAIL  <<<"); }
    doStop();                         /* SPEED 回到 1% 保活，等换片 */
    st = 1;
}
#endif /* EN_PROVISION_MODE */

/* ============================ 串口控制台 ============================ */
static void helpPrint() {
    Serial.println();
    Serial.println("---- 串口命令（回车结尾）----");
    Serial.println("  0-100        设转速百分比（PWM 占空比），例如 50");
    Serial.println("  0            停机");
    Serial.println("  p            打印状态");
    Serial.println("  f            读并解码故障");
    Serial.println("  c            清故障");
    Serial.println("  d            打印影子寄存器 0x080-0x0AE");
    Serial.println("  s            重新扫描 I2C 地址");
    Serial.println("  P            把参数表固化到器件 EEPROM（需要 W 确认）");
    Serial.println("  h            本帮助");
    Serial.println("------------------------------");
}

static void statusPrint() {
    Serial.printf("  目标地址    : 0x%02X %s\n", mcf::address(), g_devFound ? "" : "(未确认)");
    Serial.printf("  速度指令    : %.1f%%\n", g_speedPct);
    Serial.printf("  FG 转速     : %.0f RPM\n", g_fgRpm);
    Serial.printf("  故障        : %s\n", g_hasFault ? "有（输入 f 查看）" : "无");
    Serial.printf("  错误次数    : %lu（最近：%s）\n",
                  (unsigned long)mcf::errorCount(), mcf::lastErrorText());
    Serial.printf("  运行时间    : %lu s\n", (unsigned long)((millis() - g_bootMs) / 1000));
}

static void scanAndLock() {
    uint8_t found[8];
    Serial.println("  扫描 I2C 0x00-0x7F ...");
    Serial.flush();
    int n = mcf::scanBus(found, 8);
    Serial.printf("  命中 %d 个：", n);
    for (int i = 0; i < n && i < 8; i++) Serial.printf(" 0x%02X", found[i]);
    Serial.println();
    Serial.flush();
    if (n == 0) {
        g_devFound = false;
        Serial.println("  没扫到器件，排查：SDA/SCL 接线、5.1k 上拉、VM/AVDD 是否上电、");
        Serial.println("  器件是否处于 sleep（SPEED 引脚长时间为低会休眠）");
        return;
    }
    g_devFound = true;
    mcf::setAddress(found[0]);
}

static char *skipSpace(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void handleLine(char *line) {
    char *s = skipSpace(line);
    if (*s == 0) return;

    /* 纯数字 = 速度百分比 */
    if (*s >= '0' && *s <= '9') {
        long v = strtol(s, NULL, 10);
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        if (v == 0) doStop();
        else        doSpeed((float)v);
        return;
    }

    switch (*s) {
        case 'h': helpPrint(); break;
        case 'p': statusPrint(); break;
        case 'f': faultSnapshot(false); break;
        case 'c': faultSnapshot(true); break;
        case 't': doStop(); Serial.println("  已停机"); break;
        case 'd': shadowDump(); break;
        case 's': scanAndLock(); break;
        case 'P': doProvision(); break;

        /* 调试用：直接读写影子寄存器（调参时不必反复重烧固件）。
         * r AAAA         读 32 位
         * k AAAA MASK V  读-改-写，只动 MASK 内的位，保留 PARITY */
        case 'r': {
            char *a = skipSpace(s + 1);
            uint16_t addr = (uint16_t)strtoul(a, NULL, 16);
            uint32_t v = 0;
            if (mcf::readReg(addr, v, mcf::DLEN_32))
                Serial.printf("  [0x%03X] = 0x%08lX\n", addr, (unsigned long)v);
            else
                MCF_LOG(1, "读 0x%03X 失败：%s\n", (unsigned)addr, mcf::lastErrorText());
            break;
        }

        case 'k': {
            char    *a = skipSpace(s + 1);
            char    *e = NULL;
            uint16_t addr = (uint16_t)strtoul(a, &e, 16);
            uint32_t mask = (uint32_t)strtoul(skipSpace(e), &e, 16);
            uint32_t val  = (uint32_t)strtoul(skipSpace(e), NULL, 16);
            if (mcf::modifyReg(addr, mask, val)) {
                uint32_t rb = 0;
                Serial.printf("  [0x%03X] mask=0x%08lX value=0x%08lX -> ", (unsigned)addr,
                              (unsigned long)mask, (unsigned long)val);
                if (mcf::readReg(addr, rb, mcf::DLEN_32))
                    Serial.printf("0x%08lX\n", (unsigned long)rb);
                else
                    Serial.println("(回读失败)");
            } else {
                MCF_LOG(1, "写 0x%03X 失败：%s\n", (unsigned)addr, mcf::lastErrorText());
            }
            break;
        }

        default:
            Serial.printf("  未知命令：%s（输入 h 查看帮助）\n", s);
            break;
    }
}

static void consolePoll() {
    static char   buf[64];
    static size_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf[len] = 0;
            handleLine(buf);
            len = 0;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

/* ============================ 后台任务 ============================ */
/* 启动类故障：无感启动偶发失步时器件会自己重试，但也可能停在故障态。
 * 这里只对这些位自动清故障并把速度指令重新给一遍；
 * 过压 / 过流（硬件）等其它故障一律不重试，必须人工确认。 */
#define START_FAULT_MASK ((1UL << 22) | (1UL << 23) | (1UL << 21) | (1UL << 20) | \
                          (1UL << 19) | (1UL << 18))
#define START_RETRY_MAX  (3)

static void monitorTask() {
    if (!g_devFound) return;
    static uint32_t tLast = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - tLast) < MONITOR_PERIOD_MS) return;
    tLast = now;

    uint32_t ct = 0, gd = 0;
    if (mcf::readRegOnce(mcf::REG_CONTROLLER_FAULT, ct, mcf::DLEN_32) &&
        mcf::readRegOnce(mcf::REG_GATE_DRIVER_FAULT, gd, mcf::DLEN_32)) {
        g_missCount = 0;
        bool f = ((ct & CTL_FAULT_MASK) != 0) || (gd != 0);
        if (f && !g_hasFault) {
            MCF_LOG(1, "检测到新故障：\n");
            faultSnapshot(false);
        }
        g_hasFault = f;

        if (f) {
            g_lastFaultMs = now;
            if (g_speedPct > 0.0f && (ct & START_FAULT_MASK) && g_retry < START_RETRY_MAX) {
                float   pct = g_speedPct;
                uint8_t r   = (uint8_t)(g_retry + 1);
                MCF_LOG(1, "启动失败，自动重试 %u/%u\n", (unsigned)r, (unsigned)START_RETRY_MAX);
                doStop();                       /* 速度指令归零，器件退回待机 */
                /* 等转子滑停再重启：在还在转的转子上重走启动流程几乎必然再失败一次。
                 * 最多等 1.5s，滑得慢就直接交给 ISD/重同步去接。 */
                for (int i = 0; i < 10 && g_fgRpm > 150.0f; i++) delay(150);
                g_retry = r;
                if (mcf::clearFaults()) {
                    delay(150);
                    doSpeed(pct);
                }
            }
        } else if (g_speedPct > 0.0f && (uint32_t)(now - g_lastFaultMs) > 10000UL) {
            g_retry = 0;      /* 连续正常运行 10 秒，重试计数归零 */
        }
    } else if (++g_missCount >= DEVICE_LOST_TOLERANCE) {
        MCF_LOG(1, "连续 %u 次读失败，判定器件掉线，等待重连\n", (unsigned)g_missCount);
        g_devFound  = false;
        g_missCount = 0;
        g_hasFault  = true;
    }
}

/* 器件上线（上电扫到 / 运行中重新接上）后的初始化 */
static void bringDeviceOnline(uint8_t addr) {
    mcf::setAddress(addr);
    g_devFound  = true;
    g_missCount = 0;
    Serial.printf("*** 器件 0x%02X 上线 ***\n", addr);

    waitSysEnable();
#if MCF_STOP_ON_BOOT
    doStop();
#endif
#if EN_APPLY_CFG_AT_BOOT && !EN_PROVISION_MODE
    applyConfigTable(false);
#endif
    faultSnapshot(false);
    statusPrint();
}

/* 器件未接 / 掉线时安静等待：先探默认地址，偶尔全扫一遍 */
static void deviceWaitTask() {
    if (g_devFound) return;
    static uint32_t tProbe = 0, tFull = 0, tAlive = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - tAlive) >= 5000UL) {
        tAlive = now;
        Serial.printf("[%lu] 等待器件上线（未扫到 I2C 应答）\n", (unsigned long)(now / 1000));
    }
    if ((uint32_t)(now - tProbe) < DEVICE_PROBE_PERIOD_MS) return;
    tProbe = now;

    uint8_t found[8];
    int n = 0;
    if (mcf::probeAddress(MCF_TARGET_ADDR_DEFAULT)) {
        found[0] = MCF_TARGET_ADDR_DEFAULT;
        n = 1;
    } else if ((uint32_t)(now - tFull) >= DEVICE_FULL_SCAN_MS) {
        tFull = now;
        n = mcf::scanBus(found, 8);
    }
    if (n > 0) bringDeviceOnline(found[0]);
}

/* ============================ setup / loop ============================ */
void setup() {
    Serial.begin(SERIAL_BAUD);
    uint32_t t0 = millis();
    while (!Serial && (uint32_t)(millis() - t0) < SERIAL_WAIT_MS) delay(10);
    delay(200);
    g_bootMs = millis();

    Serial.println();
    Serial.println("========================================");
    Serial.printf(" %s  v%s\n", FW_NAME, FW_VERSION);
    Serial.println(" MCF8315D + ESP32-C3 无刷电机驱动");
    Serial.println("========================================");
    Serial.printf(" 复位原因：%s\n", resetReasonText());

    /* 可选控制引脚（本板均未使用，宏为 PIN_UNUSED 时自动跳过） */
    if (PIN_MCF_DRVOFF != PIN_UNUSED) { pinMode(PIN_MCF_DRVOFF, OUTPUT); digitalWrite(PIN_MCF_DRVOFF, LOW); }
    if (PIN_MCF_BRAKE  != PIN_UNUSED) { pinMode(PIN_MCF_BRAKE,  OUTPUT); digitalWrite(PIN_MCF_BRAKE,  LOW); }

    /* SPEED 引脚输出 PWM */
    ledcSetup(SPEED_PWM_CHANNEL, SPEED_PWM_FREQ_HZ, SPEED_PWM_BITS);
    ledcAttachPin(PIN_MCF_SPEED, SPEED_PWM_CHANNEL);
    ledcWrite(SPEED_PWM_CHANNEL, speedPwmDuty(SPEED_PWM_MIN_PCT));
    Serial.printf(" SPEED: GPIO%d 输出 %lu Hz PWM（停机保活 %.1f%%）\n",
                  PIN_MCF_SPEED, (unsigned long)SPEED_PWM_FREQ_HZ, SPEED_PWM_MIN_PCT);

    /* 反馈引脚 */
    pinMode(PIN_MCF_NFAULT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_MCF_NFAULT), nfaultISR, FALLING);
    pinMode(PIN_MCF_FG, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_MCF_FG), fgISR, FALLING);
    g_fgLastTick = millis();

    /* I2C */
    if (!mcf::busBegin()) {
        Serial.println("!! I2C 引脚未配置，停机");
        for (;;) delay(1000);
    }
    Serial.printf(" I2C @ %lu Hz%s\n", (unsigned long)I2C_SCL_FREQ_HZ,
                  mcf::crcEnabled() ? "，CRC 启用" : "");

    scanAndLock();
    if (!g_devFound) {
        Serial.println(" 未找到器件，主循环会每 2 秒重试。修好接线后也可输入 s 手动扫描。");
        return;
    }
    bringDeviceOnline(mcf::address());

    Serial.println();
    Serial.println(" 就绪：输入 0-100 设置转速，h 查看命令。");
    Serial.println(" 新焊接/更换的芯片请先执行一次 P 固化配置。");
    Serial.println("========================================");
}

void loop() {
#if EN_PROVISION_MODE
    batchTask();
#endif
    consolePoll();
    deviceWaitTask();
    monitorTask();
    fgUpdate();

    if (g_nfaultFlag) {
        g_nfaultFlag = false;
        MCF_LOG(1, "nFAULT 引脚被拉低\n");
        faultSnapshot(false);
    }

    /* 运行中每 2 秒报告一次 */
    static uint32_t tLog = 0;
    uint32_t now = millis();
    if (g_devFound && (uint32_t)(now - tLog) >= 2000UL) {
        tLog = now;
        MCF_LOG(3, "FG 转速 %.0f RPM，指令 %.1f%%，故障 %s，错误累计 %lu\n",
                g_fgRpm, g_speedPct, g_hasFault ? "有" : "无",
                (unsigned long)mcf::errorCount());
    }

    delay(2);
}
