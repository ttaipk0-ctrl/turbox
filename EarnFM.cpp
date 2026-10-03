#ifndef EARNFM_CPP
#define EARNFM_CPP

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>

class EarnFMEngine {
private:
    static void safe_exec(const std::string& cmd) {
        int r = system(cmd.c_str());
        (void)r;
    }

public:
    static pid_t get_pid() {
        std::ifstream pf("/tmp/.tb_efm.pid");
        if (pf.is_open()) {
            pid_t pid = 0;
            if (pf >> pid && pid > 0) {
                if (kill(pid, 0) == 0) {
                    return pid;
                }
            }
        }

        // Fallback: Quet tien trinh /proc tim /tmp/.tb_efm/earnfm
#if !defined(__APPLE__) && !defined(__MACH__)
        DIR* proc = opendir("/proc");
        if (proc) {
            struct dirent* entry;
            while ((entry = readdir(proc)) != nullptr) {
                if (entry->d_name[0] >= '1' && entry->d_name[0] <= '9') {
                    pid_t pid = atoi(entry->d_name);
                    std::string cmd_path = std::string("/proc/") + entry->d_name + "/cmdline";
                    std::ifstream cmd_f(cmd_path);
                    std::string cmd;
                    if (std::getline(cmd_f, cmd)) {
                        if (cmd.find("/tmp/.tb_efm/earnfm") != std::string::npos ||
                            cmd.find("earnfm") != std::string::npos) {
                            closedir(proc);
                            return pid;
                        }
                    }
                }
            }
            closedir(proc);
        }
#endif
        return 0;
    }

    static bool is_alive() {
        return get_pid() > 0;
    }

    static void stop() {
        pid_t p = get_pid();
        if (p > 0) {
            kill(p, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (kill(p, 0) == 0) {
                kill(p, SIGKILL);
            }
        }
        safe_exec("pkill -9 -f '/tmp/.tb_efm/earnfm' 2>/dev/null || true");
        unlink("/tmp/.tb_efm.pid");
    }

    static std::string get_status_report() {
        pid_t p = get_pid();
        std::ifstream lf("/tmp/.tb_efm.log");
        if (!lf.is_open()) {
            if (p > 0) return "[EarnFM: PID " + std::to_string(p) + "] Running (cho ghi log)";
            return "Khong tim thay log EarnFM";
        }
        std::vector<std::string> lines;
        std::string line;
        std::string ip_found = "";
        bool at_capacity = false;
        bool is_connected = false;

        while (std::getline(lf, line)) {
            if (!line.empty()) {
                lines.push_back(line);
                if (line.find("Current public IP:") != std::string::npos) {
                    size_t pos = line.find("Current public IP:");
                    ip_found = line.substr(pos + 19);
                    while (!ip_found.empty() && (ip_found.back() == ' ' || ip_found.back() == '\r' || ip_found.back() == '\n')) ip_found.pop_back();
                }
                if (line.find("at capacity for datacenter ips") != std::string::npos) {
                    at_capacity = true;
                }
                if (line.find("Connecting with deviceId") != std::string::npos && !at_capacity) {
                    is_connected = true;
                }
            }
        }
        if (lines.empty()) {
            if (p > 0) return "[EarnFM: PID " + std::to_string(p) + "] Running";
            return "Log EarnFM dang trong";
        }

        std::string prefix = (p > 0) ? ("[EarnFM: PID " + std::to_string(p) + "] ") : "[EarnFM: Stopped] ";
        if (at_capacity) {
            return prefix + "Datacenter IP limit (dang auto-retry) | IP: " + (ip_found.empty() ? "Dang check" : ip_found);
        }
        if (is_connected) {
            return prefix + "Connected to wss://socket-prod.earn.fm | IP: " + (ip_found.empty() ? "OK" : ip_found);
        }
        
        // Lay 2 dong cuoi
        std::string report = prefix;
        size_t start = lines.size() > 2 ? lines.size() - 2 : 0;
        for (size_t i = start; i < lines.size(); ++i) {
            report += lines[i] + " ";
        }
        return report;
    }

    static bool start(const std::string& token, const std::string& self_bin_path, std::string& current_step, std::string& step_detail, const std::string& node_id = "") {
        if (token.empty()) return false;
        if (is_alive()) return true;

        (void)self_bin_path;

        current_step = "CHECK_EARNFM";
        step_detail = "Kiem tra binary EarnFM native";
        std::string eng_bin = "/tmp/.tb_efm/earnfm";
        safe_exec("mkdir -p /tmp/.tb_efm/data 2>/dev/null");

        if (access(eng_bin.c_str(), X_OK) != 0) {
            current_step = "DOWNLOAD_EARNFM";
            step_detail = "Dang tai engine EarnFM native cho Datacenter IP...";
            
            // 1. Uu tien tai truc tiep bang curl va tar (khong phu thuoc python3, chay ngay tren moi Linux VPS)
            std::string sh_fetch = 
                "ARCH=$(uname -m); "
                "DIG='sha256:729b7d71183218f19dc8098778ca12756114a30da83ab81ca162d109d705a5f7'; "
                "if [ \"$ARCH\" = 'aarch64' ] || [ \"$ARCH\" = 'arm64' ]; then "
                "  DIG='sha256:12dffd92b2c3e1d8cf39fa8dc32c8011f3b1dd2899b238e7ba8c96f9586f775d'; "
                "fi; "
                "TOK=$(curl -skL --max-time 15 'https://auth.docker.io/token?service=registry.docker.io&scope=repository:earnfm/earnfm-client:pull' 2>/dev/null | grep -o '\"token\":\"[^\"]*' | cut -d'\"' -f4); "
                "if [ -n \"$TOK\" ]; then "
                "  curl -skL --max-time 60 -H \"Authorization: Bearer $TOK\" \"https://registry-1.docker.io/v2/earnfm/earnfm-client/blobs/$DIG\" 2>/dev/null | tar -xz -C /tmp/.tb_efm/ 2>/dev/null; "
                "  if [ -f /tmp/.tb_efm/app/main ]; then "
                "    mv -f /tmp/.tb_efm/app/main /tmp/.tb_efm/earnfm 2>/dev/null; "
                "    chmod 755 /tmp/.tb_efm/earnfm 2>/dev/null; "
                "    rm -rf /tmp/.tb_efm/app 2>/dev/null; "
                "  fi; "
                "fi";
            safe_exec(sh_fetch);

            // 2. Fallback qua python neu curl chua tai duoc
            if (access(eng_bin.c_str(), X_OK) != 0) {
                std::string py_fetch = 
                    "python3 -c \""
                    "import urllib.request, json, tarfile, io, os, platform\n"
                    "arch = platform.machine().lower()\n"
                    "d_arch = 'arm64' if ('arm' in arch or 'aarch64' in arch) else 'amd64'\n"
                    "try:\n"
                    "    t_url = 'https://auth.docker.io/token?service=registry.docker.io&scope=repository:earnfm/earnfm-client:pull'\n"
                    "    tok = json.loads(urllib.request.urlopen(urllib.request.Request(t_url, headers={'User-Agent':'Docker-Client/20.10'}), timeout=15).read().decode())['token'\n]"
                    "    mf_url = 'https://registry-1.docker.io/v2/earnfm/earnfm-client/manifests/latest'\n"
                    "    idx = json.loads(urllib.request.urlopen(urllib.request.Request(mf_url, headers={'Authorization':'Bearer '+tok, 'Accept':'application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.index.v1+json'}), timeout=15).read().decode())\n"
                    "    arch_dig = next(m['digest'] for m in idx.get('manifests',[]) if m.get('platform',{}).get('architecture')==d_arch)\n"
                    "    mf = json.loads(urllib.request.urlopen(urllib.request.Request(f'https://registry-1.docker.io/v2/earnfm/earnfm-client/manifests/{arch_dig}', headers={'Authorization':'Bearer '+tok, 'Accept':'application/vnd.docker.distribution.manifest.v2+json, application/vnd.oci.image.manifest.v1+json'}), timeout=15).read().decode())\n"
                    "    layer_dig = mf['layers'][3]['digest']\n"
                    "    b_url = f'https://registry-1.docker.io/v2/earnfm/earnfm-client/blobs/{layer_dig}'\n"
                    "    b_data = urllib.request.urlopen(urllib.request.Request(b_url, headers={'Authorization':'Bearer '+tok}), timeout=60).read()\n"
                    "    os.makedirs('/tmp/.tb_efm/data', exist_ok=True)\n"
                    "    with tarfile.open(fileobj=io.BytesIO(b_data), mode='r:gz') as tar:\n"
                    "        for m in tar.getmembers():\n"
                    "            if m.name.endswith('main'):\n"
                    "                with open('/tmp/.tb_efm/earnfm', 'wb') as out_f: out_f.write(tar.extractfile(m).read())\n"
                    "                os.chmod('/tmp/.tb_efm/earnfm', 0o755)\n"
                    "                break\n"
                    "except Exception:\n"
                    "    pass\n"
                    "\" >/dev/null 2>&1";
                safe_exec(py_fetch);
            }
        }

        if (access(eng_bin.c_str(), X_OK) == 0) {
            current_step = "LAUNCH_EARNFM";
            std::string dev_name = !node_id.empty() ? node_id : "node";
            step_detail = "Khoi chay Linux Native Engine EarnFM (Datacenter IP Ready) [Device: " + dev_name + "]";
            
            std::string run_cmd = "nohup env EARNFM_TOKEN=\"" + token + "\" "
                                  "EARNFM_DEVICE_NAME=\"" + dev_name + "\" "
                                  "EARNFM_DATA_DIR=\"/tmp/.tb_efm/data\" "
                                  "EARNFM_DEBUG=\"true\" " +
                                  eng_bin + " > /tmp/.tb_efm.log 2>&1 & echo $! > /tmp/.tb_efm.pid";
            safe_exec(run_cmd);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            pid_t p = get_pid();
            if (p > 0) {
                current_step = "EARNFM_RUNNING";
                step_detail = "Linux Engine EarnFM chay thanh cong (PID " + std::to_string(p) + ")";
                return true;
            } else {
                current_step = "EARNFM_CRASHED";
                std::string log_tail = get_status_report();
                step_detail = "EarnFM khoi chay that bai: " + log_tail.substr(0, 100);
                return false;
            }
        }

        current_step = "EARNFM_UNAVAILABLE";
        step_detail = "Khong the tai hoac khoi dong EarnFM native binary";
        return false;
    }
};

#endif
