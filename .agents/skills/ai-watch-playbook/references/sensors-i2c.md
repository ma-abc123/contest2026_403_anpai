# I2C 传感器：后台探测模式、驱动事实、引脚归属

## 铁律：探测在后台线程，不在 LVGL 线程

`sifli_i2c.c:41` 的 **`I2C_XFER_TIMEOUT = 5000`（5 秒）** —— 这是每次 transfer 的
超时（`HAL_I2C_Master_Receive/Transmit/Mem_Read` 都用它）。对不在线设备一次
探测就**最长阻塞 5 秒**，不是 1 秒。

> 2026-10-28 更正：此前本文件与 SKILL.md 红线 #2 写的是「`I2C_TIMEOUT_ADDR` /
> `I2C_TIMEOUT_BUSY` 各 1000ms，阻塞 1-2s」。实测代码值是 5000ms/次，**低估 5 倍**。
> 结论方向不变（更要放后台），但"最坏情况可接受"的判断要改：5 秒冻结在手表上不可接受。

标准模式（MAX30102 探测沉淀，运动服务复用）：

```c
/* pthread 内 */
verdict: PENDING → OK / ABSENT
循环:
  if (verdict == OK) { usleep(100ms); continue; }   /* 在位后待机，
      由测量路径在读失败时把 verdict 打回 PENDING 触发重探 */
  open → read WHO_AM_I/PARTID → close → 发布 verdict
  absent → 5s 退避（100ms 切片，保证 join ≤100ms）
```

- 首探前先 300ms 切片延迟，给页面切换让路（探测持总线锁时任何同总线
  访问都会排在后面）。
- LVGL 线程在 verdict==PENDING 期间**零 I2C 接触**（open 也不要）；
  verdict==OK 后测量 fd 由 LVGL 线程持有，探测线程只待机。
- NuttX I2C 总线锁按 transfer 粒度串行化，跨线程共享总线是安全的——
  危险的只是"长时间持锁"（缺席设备超时、长突发事务）。

## 总线与引脚归属（黄山派）

| 总线 | 引脚 | 挂载 |
|---|---|---|
| /dev/i2c0 | PA37(SCL)/PA33(SDA) | FT6146 触摸（IRQ 见 CONFIG_TOUCH_IRQ_PIN） |
| /dev/i2c1 | PA40(SCL)/PA39(SDA) | 板载 LSM6DS3TR-C(0x6A) + MMC5603NJ(0x30) + LTR-303ALS-01(0x29)；外接 MAX30102 应接这里(0x57) |

- 板载三颗传感器都在 i2c1 上：**IMU LSM6DS3TR-C(0x6A) + 地磁 MMC5603NJ(0x30) +
  环境光 LTR-303ALS-01(0x29)**。其中只有 IMU 有 openvela 适配（`/dev/lsm6dsl0`），
  地磁与光感**驱动侧零适配**——但硬件就在板上、且已通电（共用 PA30 的 HR3V3）。
  立创硬件文档 "传感器" 一节是硬件存在性的权威来源。
- IMU 辅助：PA30 = 传感器 LDO 使能（高有效，**光感/地磁共用，不能单独关**）、
  PA31 = INT1。BSP `sf32lb52_lsm6ds3_initialize()` 已配好并注册 `/dev/lsm6dsl0`
  （先试 0x6A 再试 0x6B）。
- wiki 排针表是引脚归属的权威来源（丝印可能错：排针 14 脚丝印 PB_39
  实为 PA_39）；0x29/0x30 的"幽灵地址"就是真实板载传感器——**当时把它们当
  "幽灵"排除了，实际是漏适配的器件，不要再重复这个误判**。
- DHT22：PA20（30P 排针 24 脚）。**PA20 默认是振动马达 PWM，板上马达
  不能焊接**（LCKFB wiki 原文）。单总线空闲高，数据线外接 4.7-10k 上拉，
  内部上拉只作兜底；dhtxx 驱动强制 2s 采样周期，解析 16bit×0.1 格式
  含负温；`read()` 返回 `struct dhtxx_sensor_data_s {hum,temp,status}`。

## LTR-303ALS-01 光感：树内没有能直接用的驱动

- `ltr308_uorb.c` **不能**只改地址复用：LTR308 是 0x53 / PartID 0x06=0xB1 /
  数据 0x0D；LTR303 是 0x29 / PartID 0x86 / 数据 0x88-0x89 / 控制 0x80(CONTR)、
  0x85(MEAS_RATE) / 状态 0x8C。改地址只会得到 `-ENODEV`。
- 要做就新写 `ltr303_uorb.c`（或照 IMU 的做法注册 `/dev/ltr303` 字符设备）。
- **总线争用是真风险**：IMU 采样线程 25ms 一轮、每轮 ~42ms I2C，
  光感读若插进 IMU 事务中间会撞 5s transfer 超时 → IMU 报 `-EIO` + 总线复位 →
  跌倒算法出现采样空洞。必须串行化，1Hz 足够。
- **放大器警告**：锁是**每次 transfer** 粒度（`sifli_i2c.c:485/:571`），
  IMU 的 17 次 `readreg8` 之间是开放的。要真正隔离，必须在 motion 线程里用
  应用层 mutex 包住**整个 `sensor_read()` 调用**，只包单次 transfer 等于没加。

## I2C1 已超载：168%（2026-10-28 量化）

- `lsm6dsl_sensor_read()` 一次 = **17 个字节寄存器**（`lsm6dsl.c:853-960`），
  每个 `readreg8` = `i2c_write` + `i2c_read` = **2 次独立加锁** → **34 次/轮**。
- 400kHz 下 17 次读的**纯传输只有 2.08ms**，实测 42ms → **95% 是锁/HAL 开销**，
  分摊 ≈1.17ms/次加锁。
- `MOTION_PERIOD_MS=25` < 42ms → **占用率 168%，总线本来就超载**，采样线程一直在
  排队，这就是实际只有 ~15Hz 的机制解释。
- 推论：**4.9 提速必崩不是"提速"本身，而是总线早已没有余量**；任何新 I2C 设备
  （光感/地磁）都是往超载队列里加人。根治是 IMU burst 读（34 次锁 → 2 次），
  占用率降到个位数后光感与 50Hz 都不再是问题。



## 内置 lsm6dsl 驱动事实（bringup 级测试驱动）

- 设备 `/dev/lsm6dsl0`；接口：`SNIOC_START`（固定 accel 833Hz ±16g +
  gyro 2000dps 833Hz）、`SNIOC_STOP`、`SNIOC_LSM6DSLSENSORREAD`（一次读
  6 轴+温度）、`SNIOC_START_SELFTEST`。
- 读出已是物理单位：accel **mg**（0.488/LSB @±16g）、gyro **mdps**
  （70/LSB）。换算因子是**全局静态变量**——`SNIOC_START_SELFTEST` 会把它
  改坏（0.122/1000），永远别调 selftest。
- 无 ODR/量程 ioctl；`sensor_read` 原为 **17 次单字节寄存器读 = 34 次加锁**
  （~42ms，此前记的"22 次"是错的）。2026-10-28 已改为两次块读，见文末
  「IMU burst 读」一节。
- 结构体 `struct lsm6dsl_sensor_data_s`（include/nuttx/sensors/lsm6dsl.h）。

## 上报约定

传感器数据经 BLE f3 上报（帧 [version][sensor_type][payload]，见
references/ble.md）；上报频率受 indicate 确认开销限制，周期类一律走
TX 队列线程（见 ble.md），LVGL 定时器回调里只做 `ai_watch_ble_post()`。

## IMU 读出成本与 burst 读（2026-10-28 实测，含一次自我纠正）

**先纠正一个错数**：曾记录 `sensor_read` 一轮 42ms、I2C1 占用率 168%。
设备端探针实测**逐字节读只要 3.12ms**（max 6.90ms，偶发 95ms 尖峰），
44ms 循环里只占 ~7%。42ms 很可能把"整个循环周期"当成了"读耗时"。
**"总线超载 168%"不成立**，据此排的优先级要重估。

寄存器确实连续，可一次读两块：

- `0x20..0x2D` 连续 **14 字节**：温度 2 + 陀螺 6 + 加速度 6
- `0x40..0x42` 连续 **3 字节**：时间戳（`0x2E..0x3F` 属 sensor hub，不能合并）

`CTRL3_C.IF_INC` 默认=1（自增）。实测：

| 读法 | 单次读 | 循环周期 |
|---|---|---|
| 逐字节 17 次 | 3.12 ms | 44-49 ms |
| 块读 2 次 | 1.02 ms | 41 ms |

块读只省 **~3ms / 44ms，约 9%**，属微优化，不是救命优化。

## 连接期 `Hardware error code 0`：不要把锅甩给 I2C（重要）

曾观察到"块读版连接崩溃更多"，据此认定 `I2C_M_NOSTOP`（`i2c_mem_ops`）
有害。**该结论不成立**：把驱动整体退回锚点（逐字节读）后同一时段重测，
仍是 5/10 建链崩溃；把真基线固件（4.8.25）在同一时段重测，也是 5/10 且
伴随 2 次 `Hardware error`。而更早另一时段基线是 6/8、0 次。

结论：**建链崩溃率由 LCPU 控制器的恢复相位主导，不是固件差异。**
刚 wedge 过、刚重新广播的窗口最差；同一固件换时段能差出两倍。所以：

- 判连接稳定性**必须同一时段交替测**，或多轮交替，否则全是噪声；
- 别拿单批 10 次的结果当证据下结论；
- `I2C_M_NOSTOP` / `i2c_mem_ops` 在本板 IMU 上**未被证实有害**，但也
  没有收益（耗时与两段式相同），保持两段式即可。

## LTR-303ALS-01 光感：应用层直读，已打通（2026-10-28）

- 地址 **0x29**，与 IMU 共用 I2C1 和 PA30 的 HR3V3（IMU 能跑就说明这条供电已通）。
- **寄存器映射（DS86-2013-0004，已用实机转储核对）**：

  | 地址 | 寄存器 | 出厂值 |
  |---|---|---|
  | 0x80 | ALS_CONTR | 0x00（待机） |
  | 0x85 | ALS_MEAS_RATE | 0x03（100ms 积分 / 500ms 重复） |
  | 0x86 | PART_ID | 0xA0 |
  | 0x87 | MANUFAC_ID | 0x05（Lite-On） |
  | 0x88-0x8B | CH1_0/CH1_1/CH0_0/CH0_1 | 0x00 |
  | 0x8C | ALS_STATUS | 0x00 |
  | 0x8F | INTERRUPT | 0x08 |

  **坑：0x81..0x84 是空洞，MEAS_RATE 在 0x85 而不是 0x81。** 按 LTR-308
  （`ltr308_uorb.c`，0x53/PART_ID 0x06/数据 0x0D）的习惯去猜会整体错位 4，
  读出来的"PART_ID"会是 0x00。**先转储 0x80..0x8F 再写代码**。
- 点灯判据：转储应为 `00 00 00 00 00 03 a0 05 00 00 00 00 00 00 00 08`
  （与出厂默认逐字节一致）。
- 不需要内核驱动：应用层 `open("/dev/i2c1")` + `I2CIOC_TRANSFER`，两条
  msg（先写寄存器地址、再 `I2C_M_READ` 读），与 IMU 的两段式读法同源。
- 生效：`ALS_CONTR=0x01`（gain 1x + active），`MEAS_RATE=0x03`。
- lux（gain 1x / 100ms）：`ratio=CH1/(CH0+CH1)`；
  <0.45 → `1.7743*CH0+1.1059*CH1`；<0.64 → `4.2785*CH0-1.9548*CH1`；
  <0.85 → `0.5926*CH0+0.1185*CH1`；否则 0。实测室内 37-42 lux。

## 面板亮度：透传 setpower 即可，不需要新设备节点

- CO5300 是自发光 AMOLED，**本板无背光 PWM**（`BSP_LCDC_USING_QADSPI`
  分支里 PA01 是 VADD_EN，不是 PWM）。唯一旋钮是面板寄存器 **0x51**
  （`LCD_SetBrightness()`，入参百分比，内部换算到 `REG_BRIGHTNESS_MAX`）。
- `sf32lb_lcd_setpower()` 原来把 1..100 全部塌缩成"开屏"，已改为在
  `DisplayOn` 之后调用 `p_ops->SetBrightness(&hlcdc, power)`。
- 应用层：`open("/dev/lcd0")` + `ioctl(LCDDEVIO_SETPOWER, 0..100)`
  （`lcd_dev.h` 里 `_LCDIOC(5)`）。全树只有 ioctl 调 `setpower`，启动路径
  不传中间值，所以改语义安全。
