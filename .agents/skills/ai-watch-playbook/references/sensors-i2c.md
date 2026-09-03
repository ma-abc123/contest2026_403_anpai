# I2C 传感器：后台探测模式、驱动事实、引脚归属

## 铁律：探测在后台线程，不在 LVGL 线程

SiFli HAL 的 `I2C_TIMEOUT_ADDR` / `I2C_TIMEOUT_BUSY` 各 **1000ms**：对不在线
设备一次探测（1-2 条消息）就阻塞 1-2s。在 LVGL 线程探测 = 页面冻结。

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

- IMU 辅助：PA30 = 传感器 LDO 使能（高有效）、PA31 = INT。BSP
  `sf32lb52_lsm6ds3_initialize()` 已配好并注册 `/dev/lsm6dsl0`（先试 0x6A
  再试 0x6B）。
- wiki 排针表是引脚归属的权威来源（丝印可能错：排针 14 脚丝印 PB_39
  实为 PA_39）；0x29/0x30 的"幽灵地址"就是真实板载传感器。
- DHT22：PA20（30P 排针 24 脚）。**PA20 默认是振动马达 PWM，板上马达
  不能焊接**（LCKFB wiki 原文）。单总线空闲高，数据线外接 4.7-10k 上拉，
  内部上拉只作兜底；dhtxx 驱动强制 2s 采样周期，解析 16bit×0.1 格式
  含负温；`read()` 返回 `struct dhtxx_sensor_data_s {hum,temp,status}`。

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
