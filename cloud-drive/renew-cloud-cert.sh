#!/bin/sh
# renew-cloud-cert.sh — acme.sh 续期 + 自动部署到板子 lighttpd
#
# 由 Windows 计划任务每天 03:30 与开机时执行（SYSTEM 账户）。
# acme.sh 只在 ARI 续期窗口内才会真正换证书；fullchain.cer 当天更新过才部署，
# 平时只是跑一遍 cron 检查，不重启 lighttpd。
#
# 日志：C:\Users\Administrator\.acme.sh\cron.log

exec >> /c/Users/Administrator/.acme.sh/cron.log 2>&1

# SYSTEM 计划任务环境没有用户代理变量；走本机 7890 代理访问 Let's Encrypt
export http_proxy=http://127.0.0.1:7890
export https_proxy=http://127.0.0.1:7890

ACME_HOME="/c/Users/Administrator/.acme.sh"
DOMAIN="cy.gcaiyy.xyz"
KEY_FILE="$ACME_HOME/${DOMAIN}_ecc/${DOMAIN}.key"
FULLCHAIN="$ACME_HOME/${DOMAIN}_ecc/fullchain.cer"
PEM=/tmp/server.pem
BOARD="192.168.100.1"
SSH_KEY="/c/Users/Administrator/.ssh/id_rsa"
KNOWN_HOSTS="/c/Users/Administrator/.ssh/known_hosts"
SSH_OPTS="-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 -i $SSH_KEY -o UserKnownHostsFile=$KNOWN_HOSTS -o StrictHostKeyChecking=accept-new"

echo "=== $(date '+%F %T') ==="
sh "$ACME_HOME/acme.sh" --cron --home "$ACME_HOME" || echo "acme.sh cron failed rc=$?"

TODAY=$(date +%Y%m%d)
MTIME=$(date -r "$FULLCHAIN" +%Y%m%d 2>/dev/null)
echo "TODAY=$TODAY MTIME=$MTIME"
if [ "$TODAY" = "$MTIME" ]; then
  echo "certificate updated today, deploying..."
  cat "$KEY_FILE" "$FULLCHAIN" > "$PEM"
  if ! cat "$PEM" | ssh $SSH_OPTS root@"$BOARD" "cat > /etc/lighttpd/server.pem"; then
    echo "push to board failed"
    exit 1
  fi
  if ! ssh $SSH_OPTS root@"$BOARD" "/etc/init.d/lighttpd restart"; then
    echo "lighttpd restart failed"
    exit 1
  fi
  echo "deploy OK"
else
  echo "no renewal today, skip deploy"
fi
