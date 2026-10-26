/*
 * trigger.c —— 唤醒触发监听（UART + GPIO）
 *
 * 设计要点：
 *
 * 1. poll 事件循环，不阻塞 read
 *    阻塞 read 收不到 SIGTERM、没法同时监听多个源、也没法做超时检查。
 *
 * 2. 串口走行缓冲
 *    串口是字节流，read 可能只给你半个包（"WA" 而不是 "WAKE"）。
 *    累积到 \n 才算一条完整消息。
 *
 * 3. 状态机防止重叠触发
 *    一次对话要 8 秒。这期间再收到 WAKE 不能起第二个进程（会抢麦克风）。
 *    用 LISTENING 状态挡住。
 *
 * 4. 子进程异步 + WNOHANG 回收
 *    不能同步 system()，否则主循环卡 8 秒。fork 后立刻回主循环，
 *    每轮 poll 用 waitpid(WNOHANG) 检查子进程是否结束。
 *
 * 5. GPIO 走 sysfs
 *    或用户态直接读写 /sys/class/gpio/gpioN/value。
 *    不依赖 libgpiod（板子上没有）。
 */
#include "trigger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <poll.h>
#include <sys/wait.h>

#define LINE_BUF_SIZE   256
#define POLL_TIMEOUT_MS 500

static volatile sig_atomic_t g_quit = 0;

/* ---------------- 状态 ---------------- */
enum {
    ST_IDLE = 0,        /* 空闲，可以接受触发 */
    ST_LISTENING,       /* 正在跑一次对话 */
};

struct trig_state {
    /* 配置副本 */
    trigger_cfg_t cfg;

    /* UART */
    int    uart_fd;
    char   linebuf[LINE_BUF_SIZE];
    size_t linelen;

    /* GPIO */
    int    gpio_fd;
    int    gpio_last;        /* 上次读到的电平，用于边沿检测 */

    /* 运行 */
    int    state;
    pid_t  child;
    long long last_trigger_ms;
};

/* ---------------- 信号 ---------------- */
static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

void trigger_stop(void)
{
    g_quit = 1;
}

/* ---------------- 工具 ---------------- */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int baud_to_speed(int baud)
{
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return B115200;
    }
}

/* ---------------- UART ---------------- */
static int uart_open(const char *dev, int baud, int verbose)
{
    int fd;
    struct termios tio;

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[trigger] 打开 %s 失败: %s\n", dev, strerror(errno));
        return -1;
    }

    if (tcgetattr(fd, &tio) < 0) {
        fprintf(stderr, "[trigger] tcgetattr 失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);                          /* 原始模式：不处理任何字节 */
    cfsetispeed(&tio, baud_to_speed(baud));
    cfsetospeed(&tio, baud_to_speed(baud));

    tio.c_cflag |= (CLOCAL | CREAD);          /* 忽略调制解调器线，使能接收 */
    tio.c_cflag &= ~CRTSCTS;                  /* 无硬件流控 */
    tio.c_cc[VMIN]  = 0;                      /* 非阻塞读 */
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        fprintf(stderr, "[trigger] tcsetattr 失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);

    if (verbose)
        fprintf(stderr, "[trigger] UART %s 已打开，%d bps\n", dev, baud);

    return fd;
}

/* ---------------- GPIO sysfs ---------------- */
static int gpio_open(int num, int verbose)
{
    char path[128];
    int fd;

    /* export（若未导出） */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", num);
    if (access(path, F_OK) != 0) {
        int ef = open("/sys/class/gpio/export", O_WRONLY);
        if (ef < 0) {
            fprintf(stderr, "[trigger] 无法 export GPIO%d: %s\n", num, strerror(errno));
            return -1;
        }
        {
            char nbuf[16];
            int n = snprintf(nbuf, sizeof(nbuf), "%d", num);
            if (write(ef, nbuf, (size_t)n) < 0) {
                fprintf(stderr, "[trigger] write export 失败: %s\n", strerror(errno));
            }
        }
        close(ef);
        usleep(100000);       /* 等 udev/内核建好节点 */
    }

    /* 设为输入 */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", num);
    {
        int df = open(path, O_WRONLY);
        if (df < 0) {
            fprintf(stderr, "[trigger] 设置 direction 失败: %s\n", strerror(errno));
            return -1;
        }
        if (write(df, "in", 2) < 0) {
            fprintf(stderr, "[trigger] write direction 失败: %s\n", strerror(errno));
        }
        close(df);
    }

    /* 打开 value 供 poll */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", num);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[trigger] 打开 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }

    if (verbose)
        fprintf(stderr, "[trigger] GPIO%d 已打开（%s 触发）\n",
                num, "高电平" /* 实际极性在运行时判断 */);

    return fd;
}

/* 读 GPIO 当前电平，返回 0/1，失败返回 -1 */
static int gpio_read(int fd)
{
    char c;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    if (read(fd, &c, 1) != 1) return -1;
    return (c == '1') ? 1 : 0;
}

/* ---------------- 触发动作 ---------------- */
/*
 * 启动一次对话（fork + exec，不等待）。
 * 调用前必须确认 state == ST_IDLE。
 */
static void fire(struct trig_state *st, const char *reason)
{
    pid_t pid;
    char secs[16];

    snprintf(secs, sizeof(secs), "%u", st->cfg.ask_secs ? st->cfg.ask_secs : 15);

    fprintf(stderr, "[trigger] === 触发（%s），启动对话 %s 秒 ===\n",
            reason, secs);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[trigger] fork 失败: %s\n", strerror(errno));
        return;
    }

    if (pid == 0) {
        /* 子进程：重置信号处理，exec voicectl */
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        setsid();    /* 脱离控制终端，避免父进程退出时被子进程拖住 */

        execl(st->cfg.voicectl ? st->cfg.voicectl : "/usr/bin/voicectl",
              "voicectl", "ask", secs, (char *)NULL);

        /* exec 失败 */
        fprintf(stderr, "[trigger] exec voicectl 失败: %s\n", strerror(errno));
        _exit(127);
    }

    st->child = pid;
    st->state = ST_LISTENING;
    st->last_trigger_ms = now_ms();
}

/* 检查子进程是否结束 */
static void reap_child(struct trig_state *st)
{
    int status;
    pid_t r;

    if (st->child <= 0) return;

    r = waitpid(st->child, &status, WNOHANG);
    if (r == st->child) {
        if (WIFEXITED(status))
            fprintf(stderr, "[trigger] 对话结束（rc=%d），回到待机\n",
                    WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "[trigger] 对话被信号 %d 终止\n", WTERMSIG(status));
        st->child = 0;
        st->state = ST_IDLE;
        /* 清掉串口里积压的旧数据，避免下次立即误触发 */
        if (st->uart_fd >= 0) tcflush(st->uart_fd, TCIFLUSH);
        st->linelen = 0;
    }
}

/* ---------------- 串口行处理 ---------------- */
static void handle_line(struct trig_state *st, const char *line)
{
    size_t len = strlen(line);
    size_t i;

    /* 去首尾空白 */
    while (*line == ' ' || *line == '\t') line++;
    len = strlen(line);
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t'))
        ((char *)line)[--len] = 0;

    if (len == 0) return;

    if (st->cfg.verbose)
        fprintf(stderr, "[trigger] UART 收到: \"%s\"\n", line);

    /* 关键字过滤 */
    if (st->cfg.uart_keyword && st->cfg.uart_keyword[0]) {
        if (!strstr(line, st->cfg.uart_keyword)) {
            if (st->cfg.verbose)
                fprintf(stderr, "[trigger]   不匹配关键字 \"%s\"，忽略\n",
                        st->cfg.uart_keyword);
            return;
        }
    }

    /* 防抖 */
    if (st->last_trigger_ms &&
        now_ms() - st->last_trigger_ms < (long long)st->cfg.debounce_ms) {
        fprintf(stderr, "[trigger] 防抖窗口内，忽略\n");
        return;
    }

    /* 状态检查 */
    if (st->state != ST_IDLE) {
        fprintf(stderr, "[trigger] 正在对话中，忽略本次触发\n");
        return;
    }

    fire(st, line);
    (void)i;
}

/* ---------------- 主循环 ---------------- */
int trigger_run(const trigger_cfg_t *cfg)
{
    struct trig_state st;
    struct sigaction sa;
    struct pollfd fds[2];
    int nfds = 0;
    int uart_idx = -1, gpio_idx = -1;

    if (!cfg) return -1;

    memset(&st, 0, sizeof(st));
    st.cfg = *cfg;
    if (st.cfg.uart_baud == 0)        st.cfg.uart_baud = 115200;
    if (st.cfg.debounce_ms == 0)      st.cfg.debounce_ms = 3000;
    if (st.cfg.ask_secs == 0)         st.cfg.ask_secs = 15;
    if (!st.cfg.voicectl)             st.cfg.voicectl = "/usr/bin/voicectl";
    st.uart_fd = -1;
    st.gpio_fd = -1;
    st.state = ST_IDLE;

    /* 信号处理：不用 signal()，它隐式带 SA_RESTART，会让 accept/read 自动重启 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags = 0;              /* ★ 关键：不要 SA_RESTART */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* 打开 UART */
    if (st.cfg.uart_dev && st.cfg.uart_dev[0]) {
        st.uart_fd = uart_open(st.cfg.uart_dev, st.cfg.uart_baud, st.cfg.verbose);
        if (st.uart_fd >= 0) {
            fds[nfds].fd = st.uart_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            uart_idx = nfds++;
        }
    }

    /* 打开 GPIO */
    if (st.cfg.gpio_num >= 0) {
        st.gpio_fd = gpio_open(st.cfg.gpio_num, st.cfg.verbose);
        if (st.gpio_fd >= 0) {
            fds[nfds].fd = st.gpio_fd;
            fds[nfds].events = POLLPRI;   /* sysfs GPIO 用 POLLPRI 才可靠 */
            fds[nfds].revents = 0;
            gpio_idx = nfds++;
            st.gpio_last = gpio_read(st.gpio_fd);
        }
    }

    if (nfds == 0) {
        fprintf(stderr, "[trigger] 没有可用的触发源，退出\n");
        if (st.uart_fd >= 0) close(st.uart_fd);
        if (st.gpio_fd >= 0) close(st.gpio_fd);
        return -1;
    }

    fprintf(stderr, "[trigger] 就绪（%d 个触发源），等待唤醒...\n", nfds);

    /* ---------------- 主循环 ---------------- */
    while (!g_quit) {
        int r, i;
        char tmp[128];
        ssize_t n;

        /* 先回收子进程（不阻塞） */
        reap_child(&st);

        r = poll(fds, (nfds_t)nfds, POLL_TIMEOUT_MS);

        if (r < 0) {
            if (errno == EINTR) continue;      /* 被信号打断，正常 */
            fprintf(stderr, "[trigger] poll 失败: %s\n", strerror(errno));
            break;
        }

        if (r == 0) {
            /* 超时：本轮没事件。可以用来做心跳/健康检查，这里空转 */
            continue;
        }

        /* ---- UART ---- */
        if (uart_idx >= 0 && (fds[uart_idx].revents & (POLLIN | POLLHUP))) {
            while ((n = read(st.uart_fd, tmp, sizeof(tmp))) > 0) {
                ssize_t k;
                for (k = 0; k < n; k++) {
                    char c = tmp[k];
                    if (c == '\n' || c == '\r') {
                        /* 换行：处理完整一行 */
                        if (st.linelen > 0) {
                            st.linebuf[st.linelen] = 0;
                            handle_line(&st, st.linebuf);
                            st.linelen = 0;
                        }
                    } else if (st.linelen < LINE_BUF_SIZE - 1) {
                        st.linebuf[st.linelen++] = c;
                        st.linebuf[st.linelen] = 0;

                        /*
                         * ★ 不依赖换行：缓冲区里一出现关键字就立即处理。
                         * ASRPRO 的固件可能发 "WAKE\n"（字面反斜杠 n）
                         * 也可能根本不发换行，所以不能只等 '\n'。
                         */
                        if (st.cfg.uart_keyword && st.cfg.uart_keyword[0] &&
                            strstr(st.linebuf, st.cfg.uart_keyword)) {
                            handle_line(&st, st.linebuf);
                            if (st.state == ST_LISTENING)
                                st.linelen = 0;     /* 已触发，清缓冲 */
                            else
                                st.linelen = 0;     /* 未触发（防抖/忙），也清 */
                        }
                    } else {
                        /* 超长，丢弃整行 */
                        st.linelen = 0;
                    }
                }
            }
        }

        /* ---- GPIO ---- */
        if (gpio_idx >= 0 && (fds[gpio_idx].revents & (POLLPRI | POLLERR))) {
            int v = gpio_read(st.gpio_fd);
            if (v >= 0 && v != st.gpio_last) {
                /* 边沿检测：只在电平变化时处理 */
                if (v == st.cfg.gpio_active) {
                    if (st.cfg.verbose)
                        fprintf(stderr, "[trigger] GPIO%d 变为 %d\n",
                                st.cfg.gpio_num, v);
                    if (st.last_trigger_ms &&
                        now_ms() - st.last_trigger_ms <
                            (long long)st.cfg.debounce_ms) {
                        fprintf(stderr, "[trigger] 防抖窗口内，忽略\n");
                    } else if (st.state != ST_IDLE) {
                        fprintf(stderr, "[trigger] 正在对话中，忽略\n");
                    } else {
                        fire(&st, "GPIO");
                    }
                }
                st.gpio_last = v;
            }
        }

        /* 检查所有 fd 的错误 */
        for (i = 0; i < nfds; i++) {
            if (fds[i].revents & POLLNVAL) {
                fprintf(stderr, "[trigger] fd[%d] 无效，退出\n", i);
                g_quit = 1;
            }
        }
    }

    fprintf(stderr, "[trigger] 收到退出信号，清理中...\n");

    /* 等子进程结束（最多 3 秒） */
    if (st.child > 0) {
        int waited = 0;
        while (waited < 3000) {
            if (waitpid(st.child, NULL, WNOHANG) == st.child) break;
            usleep(100000);
            waited += 100;
        }
        if (waited >= 3000) {
            kill(st.child, SIGTERM);
            fprintf(stderr, "[trigger] 子进程未退出，已发 SIGTERM\n");
        }
    }

    if (st.uart_fd >= 0) close(st.uart_fd);
    if (st.gpio_fd >= 0) close(st.gpio_fd);

    fprintf(stderr, "[trigger] 已退出\n");
    return 0;
}
