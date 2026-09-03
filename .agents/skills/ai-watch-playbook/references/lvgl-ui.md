# LVGL UI：线程规则、生命周期、图标、字体、条幅

## 线程规则（最高优先级）

- LVGL 非线程安全：**一切 UI 调用只在主循环线程**。后台线程（BLE、传感器、
  采样）只写共享数据（mutex 或单生产者单消费者队列），主循环每帧 process()
  后渲染。
- 主循环骨架（ai_watch_main.c）：`clock_gettime` → 按键轮询 → 触摸/时间 →
  `ai_watch_ble_process()` → 各功能 process() → `lv_timer_handler()` →
  `usleep(idle)`。idle 取 timer 返回值并钳到 ≤10ms，保证轮询节奏。
- 主循环里出现任何同步 I/O（ioctl、read 阻塞、BLE 发送）都是事故：
  症状 = 触摸死、KEY2 活。

## 初始化与显示

- `lv_nuttx_init` 的 `info.input_path = NULL` 跳过触控，启动后每秒轮询
  `/dev/input0` 出现再 `lv_nuttx_touchscreen_create` 动态添加（触摸节点比
  LCD 晚 ready）。`lv_nuttx_deinit` 必须在 `lv_deinit` 之前调用。
- `lv_nuttx_init` 首次可能因 LCD 驱动首笔 DMA 未完成而失败：重试循环
  （deinit → lv_deinit → 200ms → 重来）。
- 屏幕切换用 `lv_scr_load_anim(..., LV_SCR_LOAD_ANIM_NONE, 0, 0, false)`，
  不要用 `lv_scr_load()`（不稳定）。

## 输入与手势

- 触摸手势用坐标手动检测（`lv_indev_get_state/point` + 位移阈值 +
  超时），不要依赖 LVGL 的手势事件系统（不可靠）。
- 回调传参用 `lv_obj_set_user_data/get_user_data`，不要用
  `lv_event_get_param()`。
- KEY2：active high，GPIO1 pin43（PA43），`/dev/buttons` +
  `BTNIOC_SUPPORTED`，10ms 轮询、按下即触发、持续释放 80ms 后重新武装
  （长按只触发一次，连点每次有效）。

## 页面与应用生命周期

- 固定页（home/app_list/settings）+ 页栈（push/pop，anim 带 slug）；
  应用页 `app_page_screen` 由 pop 路径释放，**destroy 回调里不要删 screen**。
- 每个应用的 lv_timer 随页面生灭：create 建、destroy 删，destroy 里同时
  关 fd / join 线程 / memset 应用状态。
- 应用注册表 `g_app_desc_s`：名字 + 5 档图标 + create/destroy + available；
  `ai_watch_open_app` 有 index==3 为 Settings 的特判，重排注册表时注意。
- 蜂窝菜单 1+6+12=19 图标上限；拖拽用变换矩阵 + 吸附动画。

## 图标（性能与管线陷阱）

- **`lv_img_set_zoom` 在 128×128 ARGB8888 上极慢**（每帧双线性重算）：
  必须预缩放。管线：`icons/convert_icons.py`（PIL）从 `icons/src/*.png`
  生成 5 档（t0 160px … t4 25px）ARGB8888 C 数组。
- **陷阱**：脚本在 `icons/` 子目录运行、生成物落在 `icons/` 下；而编译实际
  引用的是**应用根目录的 ai_watch_icons.c/h 副本**——生成后必须
  `cp icons/ai_watch_icons.{c,h} .` 同步，否则报 undeclared。
- 图标数据 ~693KB（5 档 × N 图标），进 XIP flash。

## 中文字体

- 通知/提醒含中文：自生成 CJK 字体 `fonts/gen_cjk_font.sh`（lv_font_conv，
  GB2312 一级 3755 字 + 全角标点 + ASCII，16px/2bpp，Droid Sans Fallback），
  字符集存 `fonts/cjk_charset.txt`。代价：固件 +0.8MB（XIP，SRAM 不变）。
- 字体选择：`ai_watch_text_is_ascii()` 纯 ASCII 用 Montserrat，含多字节用
  CJK 字体；两套字体覆盖所有文本出口（条幅标题、列表条目）。
- 列表标题出方框 = 字形缺字，回到字符集核对再重新生成。

## 条幅与主题

- 到达通知类横幅：`lv_layer_top()` 上的 panel（类型行 + 标题行）+ 单次
  hide timer；点击横幅即隐藏；环形队列排队（深度 4），逐条展示。
- 主题切换必须**遍历刷新所有控件**（背景/文字/强调色三件套），只改部分
  控件会在浅色主题下出现不可读文字。主题色统一走 `ai_watch_theme_*` 助手。
