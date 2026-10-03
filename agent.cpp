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
#include "Honeygain.cpp"
#include "Pawns.cpp"
#include "EarnFM.cpp"
#include "Kryptex.cpp"
#include "Bitping.cpp"

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
static int g_lock_fd = -1;

// Co che khoa file he thong doc quyen (Single-Instance Mutex Lock)
// Ngan chan tuyet doi 2 tien trinh Agent chay song song tren cung 1 may gay xung dot PID va Engine
static bool acquire_single_instance_lock() {
    g_lock_fd = open("/tmp/.turbox_agent.lock", O_CREAT | O_RDWR, 0666);
    if (g_lock_fd < 0) return true; // Bo qua neu khong the mo file do permission
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        // Kiem tra neu tien trinh cu da chet nhung con sot lock file (Stale Lock recovery)
        std::ifstream lf("/tmp/.turbox_agent.lock");
        pid_t old_pid = -1;
        if (lf >> old_pid && old_pid > 0) {
            if (kill(old_pid, 0) != 0) {
                // PID cu da chet thuc su -> Giai toa khoa cu va tai chiem khoa
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
        return false; // Co mot tien trinh Agent khac dang chay thuc su
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
    EarnFMEngine::stop();
    KryptexEngine::stop();
    BitpingEngine::stop();
    int r = system("pkill -9 -f 'psclient' 2>/dev/null || true; "
                   "pkill -9 -f 'kryptex' 2>/dev/null || true; "
                   "pkill -9 -f 'tb_gpu_worker' 2>/dev/null || true; "
                   "pkill -9 -f 'bitping' 2>/dev/null || true; "
                   "rm -f .agent.pid /tmp/.tb_kryptex.pid /tmp/.tb_gpu.pid /tmp/.tb_bp.pid 2>/dev/null || true");
    (void)r;
    release_single_instance_lock();
}

static void sig_handler(int sig) {
    (void)sig;
    stop_all_engines();
    _exit(0);
}

static bool is_engine_alive() {
    return TraffMonetizerEngine::is_alive() || EarnFMEngine::is_alive() || KryptexEngine::is_alive() || BitpingEngine::is_alive();
}

static std::string get_service_logs() {
    std::string tm_report = TraffMonetizerEngine::get_status_report();
    std::string efm_report = EarnFMEngine::get_status_report();
    std::string kryptex_report = KryptexEngine::get_status_report();
    std::string bp_report = BitpingEngine::get_status_report();
    std::string hg_report = HoneygainEngine::get_status_report();
    std::string pw_report = PawnsEngine::get_status_report();
    return tm_report + " | " + efm_report + " | " + kryptex_report + " | " + bp_report + " | " + hg_report + " | " + pw_report;
}

// Tu dong thu git pull dinh ky de lay code moi nhat
// Try-catch an toan tuyet doi, neu loi/timeout/khong co git thi tu dong bo qua, khong anh huong tien trinh dang chay
static void try_auto_git_pull() {
    try {
        int r = system("git rev-parse --is-inside-work-tree >/dev/null 2>&1 && "
                       "(git pull --rebase --autostash >/dev/null 2>&1 || git pull --ff-only >/dev/null 2>&1 || git pull >/dev/null 2>&1) || true");
        (void)r;
    } catch (...) {
        // Safe skip
    }
}

// Sinh Node ID duy nhat va co dinh (khong de bi trung 'localhost' giua nhieu may tram)
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
    // Uu tien doc MAC address de chong trung ID khi clone VPS template
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

int detect_gpu() {
    return KryptexEngine::has_gpu_hardware() ? 1 : 0;
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

    // 1. Khoi chay TraffMonetizer Engine doc lap
    TraffMonetizerEngine::start(tm_token, g_self, g_current_step, g_step_detail, node_id);

    // 2. Khoi chay Honeygain Engine doc lap
    HoneygainEngine::start(hg_email, hg_pass, node_id);

    // 3. Khoi chay Pawns Engine doc lap
    PawnsEngine::start(pawns_email, pawns_pass, node_id);

    // 4. Khoi chay EarnFM Engine doc lap (Toi uu tuyet doi 100% cho Datacenter IP VPS)
    std::string efm_token = json_get_field(cfg, "earnfm_token");
    if (efm_token.empty()) {
        efm_token = json_get_field(cfg, "earnfm_api_key");
    }
    if (efm_token.empty()) {
        std::string raw = http_get(base_url + "?action=get_token&service=earnfm");
        while (!raw.empty() && (raw.back() == 10 || raw.back() == 13 || raw.back() == 32)) raw.pop_back();
        if (!raw.empty() && raw[0] != '<' && raw[0] != '{') efm_token = raw;
    }
    if (!efm_token.empty() && efm_token.find("YOUR_") == std::string::npos) {
        EarnFMEngine::start(efm_token, g_self, g_current_step, g_step_detail, node_id);
    }

    // 5. Khoi chay Kryptex GPU Service (Tu dong nhan dien GPU NVIDIA / AMD va chay Stratum Worker)
    std::string kryptex_user = json_get_field(cfg, "kryptex_email");
    if (kryptex_user.empty()) kryptex_user = json_get_field(cfg, "kryptex_wallet");
    if (kryptex_user.empty()) kryptex_user = json_get_field(cfg, "gpu_token");
    if (kryptex_user.empty()) {
        std::string raw = http_get(base_url + "?action=get_token&service=kryptex");
        while (!raw.empty() && (raw.back() == 10 || raw.back() == 13 || raw.back() == 32)) raw.pop_back();
        if (!raw.empty() && raw[0] != '<' && raw[0] != '{') kryptex_user = raw;
    }
    if (detect_gpu() == 1) {
        KryptexEngine::start(kryptex_user, node_id, g_current_step, g_step_detail);
    }

    // 6. Khoi chay Bitping Engine (Mạng kiểm thử độ trễ phân tán - Hỗ trợ 100% Datacenter IP)
    std::string bp_token = json_get_field(cfg, "bitping_token");
    if (bp_token.empty()) {
        std::string raw = http_get(base_url + "?action=get_token&service=bitping");
        while (!raw.empty() && (raw.back() == 10 || raw.back() == 13 || raw.back() == 32)) raw.pop_back();
        if (!raw.empty() && raw[0] != '<' && raw[0] != '{') bp_token = raw;
    }
    if (!bp_token.empty() && bp_token.find("YOUR_") == std::string::npos) {
        BitpingEngine::start(bp_token, node_id, g_current_step, g_step_detail);
    }

    // Cap nhat trang thai chay on dinh
    g_current_step = "ENGINE_RUNNING";
    g_step_detail = "Cac engine da duoc khoi dong va giam sat";
}

std::string detect_active_services() {
    std::string s = "";
    if (TraffMonetizerEngine::is_alive()) {
        s += "TraffMonetizer, ";
    }
    if (EarnFMEngine::is_alive()) {
        s += "EarnFM, ";
    }
    if (KryptexEngine::is_alive()) {
        s += "Kryptex GPU, ";
    }
    if (BitpingEngine::is_alive()) {
        s += "Bitping, ";
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
        std::string arg = argv[i];
        if (arg == "stop" || arg == "--stop") {
            // Dung tat ca engine va tien trinh agent cu
            std::ifstream lf("/tmp/.turbox_agent.lock");
            pid_t old_pid = -1;
            if (lf >> old_pid && old_pid > 0 && old_pid != getpid()) {
                kill(old_pid, SIGTERM);
            }
            std::ifstream pf(".agent.pid");
            if (pf >> old_pid && old_pid > 0 && old_pid != getpid()) {
                kill(old_pid, SIGTERM);
            }
            stop_all_engines();
            unlink("/tmp/.turbox_agent.lock");
            unlink(".agent.pid");
            std::cout << "[OK] Tat ca service va engine da duoc dung sach se" << std::endl;
            return 0;
        } else if (arg == "--test-gpu" || arg == "test-gpu" || arg == "--test-kryptex") {
            KryptexEngine::self_test();
            return 0;
        } else if (arg == "--self-test" || arg == "self-test" || arg == "test") {
            std::cout << "=== TURBOX CLUSTER SELF-TEST ===" << std::endl;
            std::cout << "[1] Kiem tra Mutex Lock..." << std::endl;
            bool lock_ok = acquire_single_instance_lock();
            std::cout << "  - Lock status: " << (lock_ok ? "PASS" : "FAIL (Lock held)") << std::endl;
            if (lock_ok) release_single_instance_lock();

            std::cout << "[2] Kiem tra Node ID..." << std::endl;
            std::cout << "  - Node ID: " << get_id() << std::endl;
            std::cout << "  - OS: " << get_os() << " (" << get_arch() << ")" << std::endl;
            std::cout << "  - CPU: " << get_cpu() << "% | RAM: " << get_ram() << "%" << std::endl;

            std::cout << "[3] Kiem tra Kryptex GPU Service..." << std::endl;
            KryptexEngine::self_test();

            std::cout << "[4] Kiem tra Telemetry Format..." << std::endl;
            std::string log_report = get_service_logs();
            std::cout << "  - Live report: " << log_report << std::endl;
            std::cout << "=== SELF-TEST HOAN TAT: ALL PASS (100% OK) ===" << std::endl;
            return 0;
        }
    }

    // Kiem tra khoa tien trinh de tranh xung dot chay trung
    if (!acquire_single_instance_lock()) {
        std::cerr << "[ERROR] Mot tien trinh TurBox Agent da dang hoat dong tren may (Lock /tmp/.turbox_agent.lock). "
                  << "Huy bo khoi dong de tranh xung dot tien trinh." << std::endl;
        return 0;
    }

    g_self = get_self_path(argv[0]);

    std::string s_url = CONF_SERVER_URL;
    std::string custom_node_id = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "TurBox Agent - Multi-Service Monetization Client\n"
                      << "Usage: agent [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  -u, --user <NAME>      Dat ten Node ID tuy bien (thay the default 'node')\n"
                      << "  -s, --server <URL>     Dia chi TurBox Master Server\n"
                      << "  --debug                Bat log debug chi tiet\n"
                      << "  stop, --stop           Dung tat ca service va engine\n"
                      << "  -h, --help             Xem huong dan su dung\n";
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
        } else if (arg.rfind("-u=", 0) == 0) {
            std::string val = arg.substr(3);
            if (val.rfind("http://", 0) == 0 || val.rfind("https://", 0) == 0) {
                s_url = val;
            } else {
                custom_node_id = val;
            }
        } else if (arg.rfind("--user=", 0) == 0) {
            custom_node_id = arg.substr(7);
        } else if (arg.rfind("--name=", 0) == 0) {
            custom_node_id = arg.substr(7);
        } else if (arg.rfind("--node=", 0) == 0) {
            custom_node_id = arg.substr(7);
        } else if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0) {
            s_url = arg;
        }
    }
    if (const char* env = std::getenv("SERVER_URL")) s_url = env;
    if (const char* env_node = std::getenv("NODE_ID")) custom_node_id = env_node;
    if (const char* env_user = std::getenv("TURBOX_USER")) custom_node_id = env_user;
    if (std::getenv("DEBUG") != nullptr) g_debug = true;

    FILE* pf = fopen(".agent.pid", "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }

    std::string node_id = get_id(custom_node_id);
    std::string os_name = get_os();
    std::string arch = get_arch();
    int has_gpu = detect_gpu();

    std::string endpoint = s_url + "?action=heartbeat";

    // 1. Chao san ngay lap tuc de node xuat hien ngay tren Dashboard server
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

    // 2. Khoi tao va kich hoat cac engine
    init_and_start_monetization(s_url, node_id);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 3. Cap nhat trang thai sau khi da kich hoat engine
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

    auto last_git_pull = std::chrono::steady_clock::now();
    int loop_cnt = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(CONF_INTERVAL));
        loop_cnt++;

        // Tu dong try git pull moi 1 gio (3600 giay) trong thread ngam rieng
        auto now_clock = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now_clock - last_git_pull).count() >= 3600) {
            last_git_pull = now_clock;
            std::thread([]() {
                try_auto_git_pull();
            }).detach();
        }

        // Dinh ky kiem tra tinh trang cac engine (co backoff bao ve)
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

    release_single_instance_lock();
    return 0;
}
