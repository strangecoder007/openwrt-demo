# wol-proxy

把 WireGuard 隧道里收到的 Wake-on-LAN 魔术包重新广播到局域网接口，
解决“手机经 wg0 发到 `192.168.100.255:9` 的广播包不会由路由器转发到
LAN 口”的问题。

原理：路由器内核会把到达 wg0 的子网定向广播判定为本地投递，但不会把它
当转发包转发到 eth0/br-lan。wol-proxy 直接监听 UDP 9 端口，收到魔术包
后解析出目标 MAC，重新构造标准魔术包（`FF FF FF FF FF FF` + MAC × 16），
再通过 LAN 接口以广播方式发出去，目标电脑的网卡就能收到。

## 编译

```sh
make menuconfig        # Network -> wol-proxy，勾选
make package/network/services/wol-proxy/compile V=s
```

生成的 ipk 位于 `bin/` 下，也可直接 `opkg install` 到开发板。

## 配置

`/etc/config/wol-proxy`：

```uci
config wol-proxy 'config'
	option enabled '1'
	option bind_addr '0.0.0.0'
	option port '9'
	option ifname 'eth0'
	option broadcast ''
	option allow '0.0.0.0/0'
```

- `ifname`：实际 LAN 接口名。如果是网桥（OpenWrt 常见为 `br-lan`），
  必须改成 `br-lan`。
- `broadcast`：留空时程序自动使用 LAN 接口的广播地址；收到包时若目标
  地址落在 LAN 子网内，也会优先使用包里的广播地址（如 `192.168.100.255`）。
  需要手工指定时填，例如 `192.168.100.255`。
- `allow`：只转发来自该网段的请求，建议填 WireGuard 网段如
  `10.10.10.0/24`，避免局域网内重复转发。

## 防火墙

虽然包最终是“本地投递”给 wol-proxy，但仍要经过路由器的 INPUT 链。
如果 wg0 所在防火墙区域的输入策略是拒绝，需要放行 UDP 9：

```uci
config rule
	option name 'Allow-WoL'
	option src 'wg'
	option proto 'udp'
	option dest_port '9'
	option target 'ACCEPT'
```

`src` 换成 wg0 实际所属的防火墙区域名。

## 使用

```sh
/etc/init.d/wol-proxy enable
/etc/init.d/wol-proxy start
logread -f | grep wol-proxy   # 每次转发会打日志
```

手机 App 里仍按局域网方式填目标广播地址（`192.168.100.255`）和目标
MAC 即可，App 需要走 WireGuard 隧道访问该网段。

## 排查

- 收不到包：`tcpdump -i wg0 udp port 9` 确认包到达 wg0；再看防火墙
  INPUT 链是否放行；`logread` 看 wol-proxy 是否在运行。
- 能收到但没转发：确认 `ifname` 是实际 LAN 接口（`ip addr` 查看），
  以及该接口有 IPv4 地址（用于计算广播地址）。
- 目标电脑网卡需要开启 Wake on LAN（魔术包）功能。
