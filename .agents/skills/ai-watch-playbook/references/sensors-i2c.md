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
- 无 ODR/量程 ioctl；`sensor_read` = 22 次单字节寄存器读（~42ms）→
  25ms 轮询周期实际只有 **~15Hz**。要更高率需改突发读（0x22..0x2D
  一条事务）或绕过驱动裸 I2C。
- 结构体 `struct lsm6dsl_sensor_data_s`（include/nuttx/sensors/lsm6dsl.h）。

## 上报约定

传感器数据经 BLE f3 上报（帧 [version][sensor_type][payload]，见
references/ble.md）；上报频率受 indicate 确认开销限制，周期类一律走
TX 队列线程（见 ble.md），LVGL 定时器回调里只做 `ai_watch_ble_post()`。
