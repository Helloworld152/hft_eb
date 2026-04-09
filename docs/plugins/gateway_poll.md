# GatewayPoll

**插件名称**：`GatewayPoll`  
**类型**：Module  
**入口类**：`GatewayPollModule`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `gateway_id` | `string` | yes | `none` | `gateway id` | `决定共享内存 ring 命名与路由标识，是桥接插件主键` | `no` | `"ctp_sim"` |
| `account_id` | `string` | no | `""` | `account id` | `仅处理匹配该账户的请求和回报；为空表示不过滤` | `no` | `"sim"` |
| `cmd_shm` | `string` | no | `"/cmd_ring_<gateway_id>"` | `shm name` | `主引擎写、gateway 读的命令 ring 名称` | `no` | `"/cmd_ring_ctp_sim"` |
| `rtn_shm` | `string` | no | `"/rtn_ring_<gateway_id>"` | `shm name` | `gateway 写、主引擎读的回报 ring 名称` | `no` | `"/rtn_ring_ctp_sim"` |
| `ring_capacity` | `int` | no | `1024` | `>=2` | `共享 ring 容量；过小会放大队列满和丢命令风险` | `yes` | `4096` |
| `poll_batch` | `int` | no | `256` | `>=1` | `每次 `EVENT_POLL_GATEWAY` 最多消费的回报条数` | `yes` | `512` |
| `create_rings` | `bool` | no | `false` | `true/false` | `启动时是否负责创建共享内存 ring` | `no` | `true` |
| `unlink_on_exit` | `bool` | no | `false` | `true/false` | `停止时是否 unlink ring；适合临时环境，不适合多进程共管场景` | `no` | `false` |
| `debug` | `bool` | no | `false` | `true/false` | `打印 gateway 错误等调试信息` | `no` | `true` |
| `node_id` | `int` | no | `none` | `uint32` | `设置本地订单 ID 生成器节点号，避免多实例冲突` | `no` | `3` |
