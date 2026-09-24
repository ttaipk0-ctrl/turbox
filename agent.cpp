#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <sys/utsname.h>
#include <curl/curl.h>

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

static size_t string_write_cb(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t len = size * nmemb;
    if (s) s->append((char*)contents, len);
    return len;
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
    return 15.0;
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

void fetch_config_and_init(CURL* curl) {
    std::string cfg_url = std::string(CONF_SERVER_URL) + "?action=get_config";
    std::string resp;

    curl_easy_setopt(curl, CURLOPT_URL, cfg_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK && !resp.empty()) {
        // Doc JSON token vao RAM va khoi tao cac tien trinh kiem tien tu dong
    }
}

int main() {
    if (daemon(1, 0) != 0) {}

    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl = curl_easy_init();
    if (!curl) return 1;

    fetch_config_and_init(curl);

    std::string node_id = get_id();
    std::string os_name = get_os();
    std::string arch = get_arch();
    int has_gpu = detect_gpu();
    std::string endpoint = std::string(CONF_SERVER_URL) + "?action=heartbeat";

    while (true) {
        std::ostringstream ss;
        ss << "id=" << node_id
           << "&os=" << os_name
           << "&arch=" << arch
           << "&gpu=" << has_gpu
           << "&uptime=" << get_uptime()
           << "&cpu=" << get_cpu()
           << "&ram=" << get_ram();
        std::string post_fields = ss.str();

        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_perform(curl);
        std::this_thread::sleep_for(std::chrono::seconds(CONF_INTERVAL));
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 0;
}