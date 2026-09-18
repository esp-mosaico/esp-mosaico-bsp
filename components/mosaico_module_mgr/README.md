# Mosaico 模块管理器

`mosaico_module_mgr` 负责左右扩展槽的模块发现、EEPROM V1 描述校验、状态通知，以及驱动对槽位的独占 claim/release。

## 硬件约束

两个槽共享子板 I2C 总线。标准模块用槽位控制 GPIO 驱动 AT24C02 A0：

| 槽位 | 控制 GPIO | 电平 | EEPROM 地址 |
|---|---:|---:|---:|
| 左槽 | GPIO14 | 0 | `0x50` |
| 右槽 | GPIO39 | 1 | `0x51` |

相机是例外：相机 EEPROM 内部固定为 `0x50`，没有地址线，只支持左槽；GPIO14 在相机工作时是 DVP D4。相机驱动 claim 时声明 `USE_SLOT_CONTROL_GPIO | PROBE_WHILE_CLAIMED`，因此管理器不会把相机类型硬编码进扫描逻辑，且相机工作期间仍可探测固定地址 `0x50`。

## 状态模型

每个槽位公开三个互不覆盖的状态：

- `presence`：`UNKNOWN / ABSENT / PRESENT`，只表示 EEPROM 地址探测结果。
- `descriptor_state`：`UNKNOWN / VALID / INVALID`，只表示 EEPROM 描述是否可用。
- `owner_state`：`FREE / CLAIMED / RESTORING`，只表示驱动占用关系。

`last_error` 保存当前 probe、描述读取/校验或 GPIO 恢复错误。`generation` 在公开快照发生变化时递增。只有 `descriptor_state == VALID` 时，`eeprom` 才表示当前有效描述；模块拔出时会保留最后一次有效 EEPROM 内容，便于移除事件识别原模块，但描述状态会改成 `UNKNOWN`。

## 基本使用

具体模块驱动会自动调用 `mosaico_module_mgr_init(NULL)`。只有需要状态通知或直接查询槽位时，应用才需要显式初始化：

```c
static void on_module_event(const mosaico_module_mgr_event_t *event, void *user_data)
{
    (void)user_data;
    // Keep callbacks short; query or hand off work as needed.
}

const mosaico_module_mgr_config_t config = MOSAICO_MODULE_MGR_DEFAULT_CONFIG();
mosaico_module_subscription_t subscription = {0};

ESP_ERROR_CHECK(mosaico_module_mgr_init(&config));
ESP_ERROR_CHECK(mosaico_module_mgr_subscribe(on_module_event, NULL, &subscription));
```

订阅不保证补发历史或初始状态，但可能收到注册时尚未派发的 pending change。需要完整初始视图时，应在订阅成功后调用 `mosaico_module_mgr_get_info()` 读取两个槽；通过 `generation` 或幂等处理容忍订阅与查询之间的重复通知。

## Claim 和 release

驱动用一次原子 claim 完成“等待匹配模块并占用槽位”：

```c
const mosaico_module_mgr_claim_config_t claim = {
    .expected_type = MOSAICO_BOARD_TYPE_INTERACT,
    .slot = MOSAICO_MODULE_MGR_SLOT_AUTO,
    .timeout_ms = 3000,
};
mosaico_module_lease_t lease = {0};

ESP_ERROR_CHECK(mosaico_module_mgr_claim(&claim, &lease));
// Stop and release peripheral resources before returning the slot.
ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
```

正常 claim 只接受 `PRESENT + VALID + 类型匹配 + FREE`。`ALLOW_INVALID_DESCRIPTOR` 仅允许显式槽位上的 `PRESENT + INVALID`，不能 claim 空槽。release 必须携带匹配的非零 lease ID；若恢复槽位 GPIO 失败，lease 保持有效，可用同一个 lease 重试。

## 并发和事件

- 一个 scan task 执行全部 EEPROM probe/read；I2C 操作不持状态锁。
- 一个 event task 串行执行 callback；callback 不在状态锁内运行。
- 每个槽只保存变化位和最新快照，事件表示最终状态通知，不是无损审计日志。
- 最多支持四个并发 claim waiter 和四个订阅者。
- 外部任务的 `unsubscribe` 会等待正在运行的 callback 完成；callback 可以取消自身。
- callback 中不能调用 `deinit`，且应避免长时间阻塞 event task。

完整实现说明见 [`docs/mosaico_module_mgr_workflow.md`](../../docs/mosaico_module_mgr_workflow.md)。
