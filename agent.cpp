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
#include "TraffMonetizer.cpp"
#include "Honeygain.cpp"
#include "Pawns.cpp"

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
static pid_t g_engine_pid = -1;

static inline int safe_system(const char* cmd) {
    int r = system(cmd);
    (void)r;
    return r;
}
static inline int safe_system(const std::string& cmd) {
    return safe_system(cmd.c_str());
}

static std::string get_self_path(const char* argv0) {
#if defined(__APPLE__) || defined(__MACH__)
    char p[1024]; uint32_t s = sizeof(p);
    if (_NSGetExecutablePath(p, &s) == 0) return std::string(p);
#else
    char p[1024]; ssize_t l = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (l != -1) { p[l] = '\0'; return std::string(p); }
#endif
    return (argv0 && strlen(argv0) > 0) ? std::string(argv0) : "";
}

static bool extract_payload(const std::string& out_path) {
    if (g_self.empty()) return false;
    FILE* f = fopen(g_self.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, -24, SEEK_END) != 0) { fclose(f); return false; }
    char foot[24];
    if (fread(foot, 1, 24, f) != 24 || memcmp(foot + 8, "TURBOX_BUNDLE", 13) != 0) {
        fclose(f); return false;
    }
    uint64_t sz = 0;
    memcpy(&sz, foot, 8);
    if (sz == 0 || sz > 100000000ULL || fseek(f, -(24 + (long)sz), SEEK_END) != 0) {
        fclose(f); return false;
    }
    FILE* out = fopen(out_path.c_str(), "wb");
    if (!out) { fclose(f); return false; }
    char buf[65536];
    uint64_t left = sz;
    while (left > 0) {
        size_t c = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        size_t r = fread(buf, 1, c, f);
        if (r == 0) break;
        fwrite(buf, 1, r, out);
        left -= r;
    }
    fclose(out); fclose(f);
    return (left == 0);
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
    std::string cmd = "curl -skL --max-time 10 -d \"" + data + "\" "
                      "--data-urlencode \"service_logs=" + clean_log + "\" "
                      "--data-urlencode \"step_info=" + clean_step + "\" "
                      "\"" + url + "\" >/dev/null 2>&1";
    int r = system(cmd.c_str());
    (void)r;
}

static void stop_all_engines() {
    TraffMonetizerEngine::stop();
    HoneygainEngine::stop();
    PawnsEngine::stop();
    int r = system("pkill -9 -f 'psclient' 2>/dev/null || true; "
                   "rm -f .agent.pid 2>/dev/null || true");
    (void)r;
}

static void sig_handler(int sig) {
    (void)sig;
    stop_all_engines();
    _exit(0);
}

static bool check_socket_established(pid_t pid) {
    if (pid <= 0) return false;
#if defined(__APPLE__) || defined(__MACH__)
    std::string cmd = "lsof -a -p " + std::to_string(pid) + " -i 2>/dev/null | grep -i 'ESTABLISHED' | head -n 1";
    FILE* lp = popen(cmd.c_str(), "r");
    bool connected = false;
    if (lp) {
        char lbuf[128];
        if (fgets(lbuf, sizeof(lbuf), lp)) connected = true;
        pclose(lp);
    }
    return connected;
#else
    // Kiểm tra kết nối TCP ra Internet qua /proc/<pid>/fd và /proc/net/tcp
    std::set<std::string> inodes;
    std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = opendir(fd_dir.c_str());
    if (d) {
        struct dirent* de;
        char target[512];
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            std::string p = fd_dir + "/" + de->d_name;
            ssize_t len = readlink(p.c_str(), target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                std::string s(target);
                if (s.rfind("socket:[", 0) == 0 && s.back() == ']') {
                    inodes.insert(s.substr(8, s.length() - 9));
                }
            }
        }
        closedir(d);
    }
    if (inodes.empty()) {
        std::string ss_cmd = "ss -tanp 2>/dev/null | grep 'pid=" + std::to_string(pid) + ",' | grep -i 'ESTAB' | head -n 1";
        FILE* p = popen(ss_cmd.c_str(), "r");
        if (p) {
            char buf[128];
            bool found = (fgets(buf, sizeof(buf), p) != nullptr);
            pclose(p);
            if (found) return true;
        }
        return false;
    }

    const char* tcp_files[] = {"/proc/net/tcp", "/proc/net/tcp6", nullptr};
    for (int i = 0; tcp_files[i]; ++i) {
        std::ifstream f(tcp_files[i]);
        if (!f.is_open()) continue;
        std::string line;
        std::getline(f, line); // Bỏ qua header
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string sl, local, rem, st, tx_rx, tr, retr, uid, timeout, inode;
            if (ss >> sl >> local >> rem >> st >> tx_rx >> tr >> retr >> uid >> timeout >> inode) {
                // st == "01" tương ứng TCP_ESTABLISHED
                if (st == "01" && inodes.find(inode) != inodes.end()) {
                    return true;
                }
            }
        }
    }
    return false;
#endif
}

static std::string parse_recent_log_summary(const std::string& log_file, int max_lines = 10) {
    std::ifstream f(log_file);
    if (!f.is_open()) return "";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) {
            // Lọc bỏ banner nghệ thuật ASCII
            if (line.find("$$") == std::string::npos && line.find("\\__") == std::string::npos && line.find("/__") == std::string::npos) {
                lines.push_back(line);
                if ((int)lines.size() > max_lines * 3) {
                    lines.erase(lines.begin());
                }
            }
        }
    }
    if (lines.empty()) return "";
    std::string latest = lines.back();
    if (latest.rfind("[TM] ", 0) == 0) latest = latest.substr(5);
    if (latest.rfind("[HG] ", 0) == 0) latest = latest.substr(5);
    if (latest.length() > 100) latest = latest.substr(0, 97) + "...";
    return latest;
}

static bool is_engine_alive() {
    return TraffMonetizerEngine::is_alive();
}

static std::string get_service_logs() {
    std::string tm_report = TraffMonetizerEngine::get_status_report();
    std::string hg_report = HoneygainEngine::get_status_report();
    std::string pw_report = PawnsEngine::get_status_report();

    return tm_report + " | " + hg_report + " | " + pw_report;
}

std::string get_id() {
    char h[256];
    if (gethostname(h, 255) == 0 && strlen(h) > 0) return std::string(h);
    return "node-" + std::to_string(getpid());
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

int detect_gpu() {
    if (access("/usr/bin/nvidia-smi", X_OK) == 0 || access("/usr/local/cuda", F_OK) == 0) return 1;
#if defined(__APPLE__) || defined(__MACH__)
    return 1;
#else
    return 0;
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
    g_step_detail = "Dang lay token tu server";

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

    std::string hg_email = json_get_field(cfg, "honeygain_email");
    if (hg_email.empty() || hg_email.find("YOUR_") != std::string::npos) {
        hg_email = "nguyenlinh6605@gmail.com";
    }

    std::string hg_pass = json_get_field(cfg, "honeygain_password");
    if (hg_pass.empty() || hg_pass.find("YOUR_") != std::string::npos) {
        hg_pass = "nguyenlinh6605@gmail.com";
    }

    std::string pawns_email = json_get_field(cfg, "pawns_email");
    if (pawns_email.empty() || pawns_email.find("YOUR_") != std::string::npos) {
        pawns_email = "nguyenlinh6605@gmail.com";
    }

    std::string pawns_pass = json_get_field(cfg, "pawns_password");
    if (pawns_pass.empty() || pawns_pass.find("YOUR_") != std::string::npos) {
        pawns_pass = "nguyenlinh6605@gmail.com";
    }

    // 1. Khởi chạy TraffMonetizer Engine độc lập
    TraffMonetizerEngine::start(tm_token, g_self, g_current_step, g_step_detail);

    // 2. Khởi chạy Honeygain Engine độc lập
    HoneygainEngine::start(hg_email, hg_pass, node_id);

    // 3. Khởi chạy Pawns Engine độc lập
    PawnsEngine::start(pawns_email, pawns_pass, node_id);
}

std::string detect_active_services() {
    std::string s = "";
    if (TraffMonetizerEngine::is_alive()) {
        s += "TraffMonetizer, ";
    }
    if (HoneygainEngine::is_alive()) {
        s += "Honeygain, ";
    }
    if (PawnsEngine::is_alive()) {
        s += "Pawns, ";
    }
    if (system("pgrep -x psclient >/dev/null 2>&1") == 0) s += "PacketStream, ";

    if (s.empty()) return "Chua co Engine";

    if (s.size() >= 2 && s.substr(s.size() - 2) == ", ") s = s.substr(0, s.size() - 2);
    return s;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "stop" || std::string(argv[i]) == "--stop") {
            stop_all_engines();
            std::cout << "[OK] Tat ca service va engine da duoc dung sach se" << std::endl;
            return 0;
        }
    }

    g_self = get_self_path(argv[0]);
    std::string s_url = CONF_SERVER_URL;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--debug") {
            g_debug = true;
        } else if ((arg == "--server" || arg == "-s") && i + 1 < argc) {
            s_url = argv[++i];
        } else if (arg.rfind("--server=", 0) == 0) {
            s_url = arg.substr(9);
        } else if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0) {
            s_url = arg;
        }
    }
    if (const char* env = std::getenv("SERVER_URL")) s_url = env;
    if (std::getenv("DEBUG") != nullptr) g_debug = true;

    FILE* pf = fopen(".agent.pid", "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }

    std::string node_id = get_id();
    std::string os_name = get_os();
    std::string arch = get_arch();
    int has_gpu = detect_gpu();
    std::string endpoint = s_url + "?action=heartbeat";

    // 1. Chào sân ngay lập tức để node xuất hiện ngay trên Dashboard server
    {
        g_current_step = "CONNECTING";
        g_step_detail = "Node da ket noi den cluster, dang khoi dong cac engine...";
        std::ostringstream ss;
        ss << "action=heartbeat&id=" << node_id << "&os=" << os_name << "&arch=" << arch
           << "&gpu=" << has_gpu << "&uptime=" << get_uptime() << "&cpu=" << get_cpu()
           << "&ram=" << get_ram() << "&services=" << detect_active_services()
           << "&step=" << g_current_step
           << "&event=START&first=1";
        if (g_debug) ss << "&debug=1";
        http_post(endpoint, ss.str(), "Connecting to hub...", g_step_detail);
    }

    // 2. Khởi tạo và kích hoạt các engine
    init_and_start_monetization(s_url, node_id);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 3. Cập nhật trạng thái sau khi đã kích hoạt engine
    {
        std::ostringstream ss;
        ss << "action=heartbeat&id=" << node_id << "&os=" << os_name << "&arch=" << arch
           << "&gpu=" << has_gpu << "&uptime=" << get_uptime() << "&cpu=" << get_cpu()
           << "&ram=" << get_ram() << "&services=" << detect_active_services()
           << "&step=" << g_current_step
           << "&event=ONLINE";
        if (g_debug) ss << "&debug=1";
        http_post(endpoint, ss.str(), get_service_logs(), g_step_detail);
    }

    int loop_cnt = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(CONF_INTERVAL));
        loop_cnt++;
        if (loop_cnt % 2 == 0) {
            init_and_start_monetization(s_url, node_id);
        }
        std::ostringstream ss;
        ss << "action=heartbeat&id=" << node_id << "&os=" << os_name << "&arch=" << arch
           << "&gpu=" << has_gpu << "&uptime=" << get_uptime() << "&cpu=" << get_cpu()
           << "&ram=" << get_ram() << "&services=" << detect_active_services()
           << "&step=" << g_current_step;
        if (g_debug) ss << "&debug=1";
        http_post(endpoint, ss.str(), get_service_logs(), g_step_detail);
    }
    return 0;
}
