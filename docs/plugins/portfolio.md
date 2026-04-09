# Portfolio

**插件名称**：`Portfolio`  
**类型**：Module  
**入口类**：`PortfolioModule`

| name | type | required | default | range | effect | hot_path | example |
|---|---|---|---|---|---|---|---|
| `default_account` | `string` | no | `default` | `account id` | `生成 `OrderReq` 时使用的默认账户` | `no` | `"sim"` |
| `signal_scale` | `float` | no | `1.0` | `real number` | `将聚合信号缩放为目标手数` | `yes` | `10` |
| `min_signal_threshold` | `float` | no | `0.0` | `>=0` | `低于阈值的聚合信号直接忽略，不下单` | `yes` | `0.2` |
| `max_abs_pos` | `int` | no | `int_max` | `>=1` | `限制净仓绝对值，防止越仓` | `yes` | `50` |
| `max_order_size` | `int` | no | `int_max` | `>=1` | `限制单笔下单手数` | `yes` | `5` |
| `max_notional` | `float` | no | `0.0` | `>=0` | `按最新价和合约乘数约束单笔名义金额；0 表示关闭` | `yes` | `200000` |
| `prefer_close_first` | `bool` | no | `true` | `true/false` | `卖出方向优先走平仓；若无可平仓位会被裁剪` | `yes` | `true` |
| `signal_ttl_ms` | `int` | no | `2000` | `>=0` | `信号缓存有效期，过期信号会在聚合前清理` | `yes` | `2000` |
| `margin_rate` | `float` | no | `1.0` | `>0` | `结合账户可用资金估算最大可开仓手数` | `yes` | `0.1` |
| `debug` | `bool` | no | `false` | `true/false` | `打印组合层下单明细，便于联调` | `no` | `false` |
