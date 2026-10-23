#!/bin/sh
# deploy.sh —— 在板子上部署语音助手
#
# 用法（在板子上执行）：
#   sh deploy.sh /tmp/voice /tmp/voice.conf /tmp/voicectl /tmp/voice-init

set -e

VOICE_SRC="${1:-/tmp/voice}"
CONF_SRC="${2:-/tmp/voice.conf}"
CTL_SRC="${3:-/tmp/voicectl}"
INIT_SRC="${4:-/tmp/voice-init}"

echo "=== 1. 安装二进制 ==="
if [ ! -f "$VOICE_SRC" ]; then
    echo "错误: $VOICE_SRC 不存在"
    exit 1
fi
cp "$VOICE_SRC" /usr/bin/voice
chmod 755 /usr/bin/voice
echo "  已安装 /usr/bin/voice ($(ls -l /usr/bin/voice | awk '{print $5}') 字节)"

echo ""
echo "=== 2. 安装配置文件 ==="
# 如果已存在配置，先备份
if [ -f /etc/voice.conf ]; then
    cp /etc/voice.conf /etc/voice.conf.bak.$(date +%s)
    echo "  已备份旧配置"
fi
cp "$CONF_SRC" /etc/voice.conf
chmod 600 /etc/voice.conf
chown root:root /etc/voice.conf
echo "  已安装 /etc/voice.conf (权限 $(ls -l /etc/voice.conf | cut -c1-10))"

echo ""
echo "=== 3. 安装包装脚本 ==="
cp "$CTL_SRC" /usr/bin/voicectl
chmod 755 /usr/bin/voicectl
echo "  已安装 /usr/bin/voicectl"

echo ""
echo "=== 4. 安装 init 脚本 ==="
cp "$INIT_SRC" /etc/init.d/voice-init
chmod 755 /etc/init.d/voice-init
/etc/init.d/voice-init enable
echo "  已安装并启用 /etc/init.d/voice-init"

echo ""
echo "=== 5. 验证 ==="
echo "--- 文件检查 ---"
ls -l /usr/bin/voice /usr/bin/voicectl /etc/voice.conf /etc/init.d/voice-init

echo ""
echo "--- 配置权限（应为 -rw-------）---"
ls -l /etc/voice.conf | cut -c1-10

echo ""
echo "--- 启动检查 ---"
/etc/init.d/voice-init start

echo ""
echo "--- 功能测试 ---"
/usr/bin/voicectl info 2>&1 | tail -6

echo ""
echo "=== 部署完成 ==="
echo ""
echo "常用命令："
echo "  voicectl ask 5        一轮对话"
echo "  voicectl chat         多轮对话"
echo "  voicectl e2e          全自动测试（不用麦克风）"
echo "  voicectl llmtest '你好'  纯文本测大模型"
echo "  voicectl info         查看配置"
