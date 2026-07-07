#include "bambu_networking.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace Slic3r {
class BBLModelTask;
using OnGetSubTaskFn = std::function<void(BBLModelTask*)>;
}

namespace {
constexpr int kUnsupported = BAMBU_NETWORK_ERR_CONNECT_FAILED;
constexpr int kInvalid = BAMBU_NETWORK_ERR_INVALID_HANDLE;

std::mutex g_log_mutex;

struct Agent {
    ~Agent();

    std::string log_dir;
    std::string config_dir;
    std::string country_code;
    BBL::OnMsgArrivedFn on_ssdp;
    BBL::OnUserLoginFn on_user_login;
    BBL::OnPrinterConnectedFn on_printer_connected;
    BBL::OnServerConnectedFn on_server_connected;
    BBL::OnHttpErrorFn on_http_error;
    BBL::GetCountryCodeFn get_country_code;
    BBL::GetSubscribeFailureFn on_subscribe_failure;
    BBL::OnMessageFn on_message;
    BBL::OnMessageFn on_user_message;
    BBL::OnLocalConnectedFn on_local_connect;
    BBL::OnMessageFn on_local_message;
    BBL::QueueOnMainFn queue_on_main;
    BBL::OnServerErrFn on_server_error;
    bool multi_machine = false;
    bool tracking = false;
    std::atomic<bool> discovery_running{false};
    std::thread discovery_thread;
    std::mutex discovery_mutex;
    std::map<std::string, std::string> discovered_devices;
    std::mutex connection_mutex;
    std::string connected_dev_id;
    std::string connected_dev_ip;
    int connected_port = 0;
    bool connected_use_ssl = false;
    std::mutex cert_mutex;
    std::set<std::string> cert_installed_devices;
    int mqtt_fd = -1;
    SSL_CTX *ssl_ctx = nullptr;
    SSL *ssl = nullptr;
    std::atomic<bool> mqtt_running{false};
    std::thread mqtt_thread;
    std::mutex mqtt_mutex;
    uint16_t mqtt_packet_id = 1;
};

std::string now_string()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::string log_path(const Agent *agent = nullptr)
{
    if (agent && !agent->log_dir.empty()) return agent->log_dir + "/arm64_network_stub.log";
    return "/tmp/bambu-network-arm64-stub.log";
}

std::string trust_store_path(const Agent *agent)
{
    if (agent && !agent->config_dir.empty()) return agent->config_dir + "/arm64_trusted_tls_pins.txt";
    const char *home = std::getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_trusted_tls_pins.txt";
    return "/tmp/arm64_trusted_tls_pins.txt";
}

void log_line(const Agent *agent, const std::string &line)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream out(log_path(agent), std::ios::app);
    if (!out) return;
    out << now_string() << " " << line << "\n";
}

void log_call(const char *name)
{
    log_line(nullptr, std::string(name));
}

void log_call(const Agent *agent, const char *name)
{
    log_line(agent, std::string(name));
}

std::string masked_len(const std::string &value)
{
    if (value.empty()) return "(empty)";
    return "len=" + std::to_string(value.size());
}

std::string to_hex(const unsigned char *data, unsigned int size)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (unsigned int i = 0; i < size; ++i) {
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0x0f]);
    }
    return out;
}

std::string base64_encode(const unsigned char *data, int size)
{
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(4 * ((size + 2) / 3)), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&out[0]), data, size);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string cert_fingerprint_hex(X509 *cert)
{
    if (!cert) return {};
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_len = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1) return {};
    return to_hex(digest, digest_len);
}

std::string cert_spki_pin(X509 *cert)
{
    if (!cert) return {};
    X509_PUBKEY *pubkey = X509_get_X509_PUBKEY(cert);
    if (!pubkey) return {};
    int der_len = i2d_X509_PUBKEY(pubkey, nullptr);
    if (der_len <= 0) return {};
    std::vector<unsigned char> der(static_cast<size_t>(der_len));
    unsigned char *cursor = der.data();
    if (i2d_X509_PUBKEY(pubkey, &cursor) != der_len) return {};
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_len = 0;
    if (EVP_Digest(der.data(), der.size(), digest, &digest_len, EVP_sha256(), nullptr) != 1) return {};
    return "sha256//" + base64_encode(digest, static_cast<int>(digest_len));
}

bool check_or_store_pin_file(const std::string &path, const std::string &key,
                             const std::string &fingerprint, std::string &error)
{
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream parts(line);
            std::string stored_key;
            std::string stored_fingerprint;
            if (parts >> stored_key >> stored_fingerprint) {
                if (stored_key == key) {
                    if (stored_fingerprint == fingerprint) return true;
                    error = "peer certificate changed";
                    return false;
                }
            }
        }
    }

    std::ofstream out(path, std::ios::app);
    if (!out) {
        error = "cannot store peer certificate pin";
        return false;
    }
    out << key << " " << fingerprint << "\n";
    return true;
}

bool write_cert_pem(const std::string &path, X509 *cert, std::string &error)
{
    FILE *file = std::fopen(path.c_str(), "w");
    if (!file) {
        error = "cannot write peer certificate";
        return false;
    }
    const bool ok = PEM_write_X509(file, cert) == 1;
    std::fclose(file);
    if (!ok) {
        error = "cannot encode peer certificate";
        return false;
    }
    return true;
}

bool verify_or_trust_peer(Agent *agent, SSL *ssl, const std::string &host, int port, std::string &error)
{
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
        error = "missing peer certificate";
        return false;
    }
    std::string fingerprint = cert_fingerprint_hex(cert);
    X509_free(cert);
    if (fingerprint.empty()) {
        error = "cannot fingerprint peer certificate";
        return false;
    }
    const std::string key = host + ":" + std::to_string(port);
    if (!check_or_store_pin_file(trust_store_path(agent), key, fingerprint, error)) return false;
    return true;
}

bool fetch_tls_pin(Agent *agent, const std::string &host, int port,
                   std::string &spki_pin, std::string &ca_cert_path, std::string &error)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "socket failed";
        return false;
    }
    timeval tv{};
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        error = "pin preflight connect failed";
        return false;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = ctx ? SSL_new(ctx) : nullptr;
    if (!ctx || !ssl) {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        close(fd);
        error = "pin preflight SSL init failed";
        return false;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        error = "pin preflight SSL_connect failed";
        return false;
    }
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        error = "pin preflight missing certificate";
        return false;
    }
    const std::string fingerprint = cert_fingerprint_hex(cert);
    spki_pin = cert_spki_pin(cert);
    if (fingerprint.empty() || spki_pin.empty()) {
        X509_free(cert);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        error = "pin preflight fingerprint failed";
        return false;
    }
    const std::string key = host + ":" + std::to_string(port);
    const bool trusted = check_or_store_pin_file(trust_store_path(agent), key, fingerprint, error);
    ca_cert_path = trust_store_path(agent) + "." + fingerprint + ".pem";
    const bool wrote_cert = trusted && write_cert_pem(ca_cert_path, cert, error);
    X509_free(cert);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return trusted && wrote_cert;
}

template <class Fn>
void dispatch_callback(Agent *agent, Fn fn)
{
    if (agent && agent->queue_on_main) {
        agent->queue_on_main(std::function<void()>(std::move(fn)));
        return;
    }
    std::thread([fn = std::move(fn)]() mutable { fn(); }).detach();
}

template <class T>
int set_cb(void *agent, T Agent::*slot, T fn, const char *name)
{
    log_call(static_cast<Agent *>(agent), name);
    if (!agent) return kInvalid;
    (static_cast<Agent *>(agent)->*slot) = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

void set_http(unsigned int *code, std::string *body, unsigned int value = 501, const char *msg = "ARM64 stub plugin: unsupported")
{
    if (code) *code = value;
    if (body) *body = msg;
}

struct FT_TunnelHandle {
    std::atomic<int> refs{1};
};

struct FT_JobHandle {
    std::atomic<int> refs{1};
    std::string result_json{"{\"error\":\"ARM64 stub plugin: unsupported\"}"};
};

std::string trim(std::string s)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), is_space));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), is_space).base(), s.end());
    return s;
}

std::string lower_copy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::map<std::string, std::string> parse_headers(const std::string &packet)
{
    std::map<std::string, std::string> headers;
    std::istringstream in(packet);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        headers[lower_copy(trim(line.substr(0, pos)))] = trim(line.substr(pos + 1));
    }
    return headers;
}

std::string header_any(const std::map<std::string, std::string> &headers, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        auto it = headers.find(lower_copy(key));
        if (it != headers.end() && !it->second.empty()) return it->second;
    }
    return {};
}

std::string json_string_value(const std::string &text, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    auto pos = text.find(needle);
    if (pos == std::string::npos) return {};
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    std::string out;
    bool escaped = false;
    for (++pos; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (escaped) {
            out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') break;
        out.push_back(c);
    }
    return out;
}

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

size_t find_json_object_end(const std::string &text, size_t object_start)
{
    if (object_start == std::string::npos || object_start >= text.size() || text[object_start] != '{')
        return std::string::npos;

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = object_start; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string) {
            if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

std::string ensure_local_liveview_advertised(const std::string &msg, bool *changed = nullptr)
{
    if (changed)
        *changed = false;
    const size_t ipcam_key = msg.find("\"ipcam\"");
    if (ipcam_key == std::string::npos)
        return msg;
    const size_t object_start = msg.find('{', ipcam_key);
    const size_t object_end = find_json_object_end(msg, object_start);
    if (object_end == std::string::npos)
        return msg;

    const std::string ipcam = msg.substr(object_start, object_end - object_start + 1);
    if (ipcam.find("\"liveview\"") != std::string::npos || ipcam.find("\"rtsp_url\"") != std::string::npos)
        return msg;
    if (ipcam.find("\"ipcam_dev\":\"1\"") == std::string::npos)
        return msg;

    std::string out = msg;
    out.insert(object_end, ",\"liveview\":{\"local\":\"local\",\"remote\":\"none\"}");
    if (changed)
        *changed = true;
    return out;
}

std::string path_basename(const std::string &path);

std::string strip_extension(std::string name)
{
    const auto slash = name.find_last_of("/\\");
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) name.resize(dot);
    return name;
}

bool file_contains_text(const std::string &path, const std::string &needle)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return data.find(needle) != std::string::npos;
}

std::string embedded_gcode_param(const std::string &path)
{
    for (int plate = 1; plate <= 32; ++plate) {
        const std::string candidate = "Metadata/plate_" + std::to_string(plate) + ".gcode";
        if (file_contains_text(path, candidate)) return candidate;
    }
    return "Metadata/plate_1.gcode";
}

std::string next_sequence_id()
{
    static std::atomic<unsigned long long> seq{
        static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())};
    return std::to_string(seq.fetch_add(1));
}

std::string project_file_payload(const BBL::PrintParams &params, const std::string &remote_path)
{
    const std::string base = path_basename(params.filename);
    const std::string subtask = !params.task_name.empty() ? params.task_name :
                                !params.project_name.empty() ? params.project_name :
                                strip_extension(base);

    std::ostringstream out;
    out << "{\"print\":{"
        << "\"command\":\"project_file\","
        << "\"sequence_id\":\"" << next_sequence_id() << "\","
        << "\"url\":\"file:///sdcard/" << json_escape(remote_path) << "\","
        << "\"param\":\"" << json_escape(embedded_gcode_param(params.filename)) << "\","
        << "\"subtask_name\":\"" << json_escape(subtask) << "\","
        << "\"bed_leveling\":" << (params.task_bed_leveling ? "true" : "false") << ","
        << "\"flow_cali\":" << (params.task_flow_cali ? "true" : "false") << ","
        << "\"vibration_cali\":" << (params.task_vibration_cali ? "true" : "false") << ","
        << "\"layer_inspect\":" << (params.task_layer_inspect ? "true" : "false") << ","
        << "\"timelapse\":" << (params.task_record_timelapse ? "true" : "false") << ","
        << "\"use_ams\":" << (params.task_use_ams ? "true" : "false");
    if (!params.task_bed_type.empty()) out << ",\"bed_type\":\"" << json_escape(params.task_bed_type) << "\"";
    if (!params.ams_mapping.empty() && params.ams_mapping.front() == '[') {
        out << ",\"ams_mapping\":" << params.ams_mapping;
    } else {
        out << ",\"ams_mapping\":[]";
    }
    out << "}}";
    return out.str();
}

void fill_detect_result_from_json(BBL::detectResult &detect, const std::string &json)
{
    detect.dev_id = json_string_value(json, "dev_id");
    detect.dev_name = json_string_value(json, "dev_name");
    detect.model_id = json_string_value(json, "dev_type");
    detect.version = json_string_value(json, "ssdp_version");
    detect.bind_state = json_string_value(json, "bind_state");
    detect.connect_type = json_string_value(json, "connect_type");
    if (detect.dev_name.empty()) detect.dev_name = detect.dev_id;
    if (detect.model_id.empty()) detect.model_id = "N2S";
    if (detect.bind_state.empty()) detect.bind_state = "free";
    if (detect.connect_type.empty()) detect.connect_type = "lan";
}

std::string ip_from_sockaddr(const sockaddr_in &addr)
{
    char buf[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) return {};
    return buf;
}

std::string dev_id_from_usn(std::string usn)
{
    if (usn.empty()) return {};
    auto pos = usn.find("uuid:");
    if (pos != std::string::npos) usn = usn.substr(pos + 5);
    pos = usn.find("::");
    if (pos != std::string::npos) usn = usn.substr(0, pos);
    pos = usn.find(':');
    if (pos != std::string::npos) usn = usn.substr(pos + 1);
    return trim(usn);
}

void stop_mqtt(Agent *agent);

std::string path_basename(const std::string &path)
{
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

std::string path_extension(const std::string &path)
{
    const std::string base = path_basename(path);
    const auto dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == base.size()) return {};
    return base.substr(dot);
}

std::string sanitize_remote_stem(const std::string &name)
{
    std::string out;
    bool last_sep = false;
    for (unsigned char ch : name) {
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(ch));
            last_sep = false;
        } else if (ch == '-' || ch == '_') {
            out.push_back(static_cast<char>(ch));
            last_sep = false;
        } else if (!last_sep) {
            out.push_back('_');
            last_sep = true;
        }
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '_' || out.front() == '-' || std::isspace(static_cast<unsigned char>(out.front())))) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '_' || out.back() == '-' || std::isspace(static_cast<unsigned char>(out.back())))) {
        out.pop_back();
    }
    return out;
}

std::string remote_upload_filename(const BBL::PrintParams &params, const std::string &local_path)
{
    std::string ext = path_extension(local_path);
    if (ext.empty()) ext = ".3mf";
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    const std::string local_base = path_basename(local_path);
    std::string stem = !params.project_name.empty() ? params.project_name :
                       !params.task_name.empty() ? params.task_name :
                       strip_extension(local_base);
    stem = sanitize_remote_stem(stem);
    if (stem.empty()) stem = "upload";

    std::string safe_ext;
    for (unsigned char ch : ext) {
        if (std::isalnum(ch) || ch == '.') safe_ext.push_back(static_cast<char>(ch));
    }
    if (safe_ext.empty() || safe_ext == ".") safe_ext = ".3mf";
    if (safe_ext.front() != '.') safe_ext.insert(safe_ext.begin(), '.');

    return stem + safe_ext;
}

std::string normalize_remote_folder(std::string folder)
{
    folder = trim(folder);
    while (!folder.empty() && folder.front() == '/') folder.erase(folder.begin());
    if (folder.empty() || folder == "sdcard" || folder == "sdcard/") folder = "cache";
    if (!folder.empty() && folder.back() != '/') folder.push_back('/');
    return folder;
}

bool ftps_upload_file(Agent *agent, const BBL::PrintParams &params, const std::string &local_path, const std::string &remote_folder, std::string &remote_path, std::string &error)
{
    if (params.dev_ip.empty()) {
        error = "missing printer ip";
        return false;
    }
    if (params.password.empty()) {
        error = "missing access code";
        return false;
    }
    const std::string remote_name = remote_upload_filename(params, local_path);
    if (remote_name.empty()) {
        error = "missing local filename";
        return false;
    }

    FILE *file = std::fopen(local_path.c_str(), "rb");
    if (!file) {
        error = "open failed errno=" + std::to_string(errno);
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const auto file_size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(file);
        error = "curl init failed";
        return false;
    }

    char *escaped = curl_easy_escape(curl, remote_name.c_str(), static_cast<int>(remote_name.size()));
    if (!escaped) {
        curl_easy_cleanup(curl);
        std::fclose(file);
        error = "curl escape failed";
        return false;
    }

    const std::string folder = normalize_remote_folder(remote_folder);
    remote_path = folder + escaped;
    curl_free(escaped);
    const std::string url = "ftps://" + params.dev_ip + ":990/" + remote_path;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, params.username.empty() ? "bblp" : params.username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, params.password.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, file);
    if (file_size >= 0) curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(file_size));
    std::string spki_pin;
    std::string ca_cert_path;
    if (!fetch_tls_pin(agent, params.dev_ip, 990, spki_pin, ca_cert_path, error)) {
        curl_easy_cleanup(curl);
        std::fclose(file);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl, CURLOPT_FTPSSLAUTH, CURLFTPAUTH_TLS);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_cert_path.c_str());
    curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, spki_pin.c_str());
    // Printer LAN certificates are self-signed; verify against the first-use
    // pinned certificate and SPKI pin instead of a public CA.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, CURLFTP_CREATE_DIR_RETRY);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    log_line(agent, "ftps upload start dev_id=" + masked_len(params.dev_id) +
                    " remote=" + path_basename(remote_path) +
                    " bytes=" + std::to_string(std::max<long>(file_size, 0)));
    const CURLcode rc = curl_easy_perform(curl);
    long response = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    if (rc != CURLE_OK) {
        error = std::string("curl ") + curl_easy_strerror(rc) + " response=" + std::to_string(response);
        log_line(agent, "ftps upload failed dev_id=" + masked_len(params.dev_id) +
                        " remote=" + path_basename(remote_path) + " error=" + error);
        curl_easy_cleanup(curl);
        std::fclose(file);
        return false;
    }

    curl_easy_cleanup(curl);
    std::fclose(file);
    log_line(agent, "ftps upload ok dev_id=" + masked_len(params.dev_id) +
                    " remote=" + path_basename(remote_path));
    return true;
}

std::string discovery_json_from_packet(const std::string &packet, const std::string &sender_ip)
{
    const std::string lowered = lower_copy(packet);
    const bool looks_bambu = lowered.find("bambu") != std::string::npos ||
                             lowered.find("bbl") != std::string::npos ||
                             lowered.find("3dprinter") != std::string::npos;
    if (!looks_bambu) return {};

    const auto headers = parse_headers(packet);
    std::string dev_id = json_string_value(packet, "dev_id");
    if (dev_id.empty()) dev_id = header_any(headers, {"dev_id", "x-bbl-dev-id", "x-bambu-dev-id", "x-bbl-device-id"});
    if (dev_id.empty()) dev_id = dev_id_from_usn(header_any(headers, {"usn", "uuid"}));
    if (dev_id.empty()) return {};

    std::string dev_name = json_string_value(packet, "dev_name");
    if (dev_name.empty()) dev_name = header_any(headers, {"dev_name", "devname.bambu.com", "x-bbl-dev-name", "x-bambu-dev-name", "x-bbl-device-name"});
    if (dev_name.empty()) dev_name = "Bambu Lab A1";

    std::string dev_ip = json_string_value(packet, "dev_ip");
    if (dev_ip.empty()) dev_ip = header_any(headers, {"dev_ip", "location", "x-bbl-dev-ip", "x-bambu-dev-ip"});
    if (dev_ip.empty()) dev_ip = sender_ip;

    std::string dev_type = json_string_value(packet, "dev_type");
    if (dev_type.empty()) dev_type = json_string_value(packet, "model_id");
    if (dev_type.empty()) dev_type = header_any(headers, {"dev_type", "devmodel.bambu.com", "x-bbl-dev-type", "x-bambu-dev-type", "x-bbl-model-id"});
    if (dev_type.empty()) dev_type = "N2S";

    std::string signal = json_string_value(packet, "dev_signal");
    if (signal.empty()) signal = header_any(headers, {"dev_signal", "devsignal.bambu.com", "x-bbl-dev-signal", "x-bambu-dev-signal"});
    if (signal.empty()) signal = "-50";

    std::string connect_type = json_string_value(packet, "connect_type");
    if (connect_type.empty()) connect_type = header_any(headers, {"connect_type", "devconnect.bambu.com", "x-bbl-connect-type"});
    if (connect_type.empty()) connect_type = "lan";

    std::string bind_state = json_string_value(packet, "bind_state");
    if (bind_state.empty()) bind_state = header_any(headers, {"bind_state", "devbind.bambu.com", "x-bbl-bind-state"});
    if (bind_state.empty()) bind_state = "free";

    std::string sec_link = json_string_value(packet, "sec_link");
    if (sec_link.empty()) sec_link = header_any(headers, {"sec_link", "devseclink.bambu.com", "x-bbl-sec-link"});
    if (sec_link.empty()) sec_link = "secure";

    std::string ssdp_version = json_string_value(packet, "ssdp_version");
    if (ssdp_version.empty()) ssdp_version = header_any(headers, {"ssdp_version", "devversion.bambu.com", "x-bbl-ssdp-version"});

    std::ostringstream out;
    out << "{"
        << "\"dev_name\":\"" << json_escape(dev_name) << "\","
        << "\"dev_id\":\"" << json_escape(dev_id) << "\","
        << "\"dev_ip\":\"" << json_escape(dev_ip) << "\","
        << "\"dev_type\":\"" << json_escape(dev_type) << "\","
        << "\"dev_signal\":\"" << json_escape(signal) << "\","
        << "\"connect_type\":\"" << json_escape(connect_type) << "\","
        << "\"bind_state\":\"" << json_escape(bind_state) << "\","
        << "\"sec_link\":\"" << json_escape(sec_link) << "\"";
    if (!ssdp_version.empty()) out << ",\"ssdp_version\":\"" << json_escape(ssdp_version) << "\"";
    out << "}";
    return out.str();
}

std::string seed_discovery_for_ip(Agent *agent, const std::string &dev_ip)
{
    if (!agent || agent->config_dir.empty()) return {};
    std::ifstream in(agent->config_dir + "/arm64_discovery_devices.jsonl");
    if (!in) return {};
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (dev_ip.empty() || json_string_value(line, "dev_ip") == dev_ip) return line;
    }
    return {};
}

void emit_discovery(Agent *agent, const std::string &json)
{
    if (!agent || !agent->on_ssdp || json.empty()) return;
    const std::string dev_id = json_string_value(json, "dev_id");
    {
        std::lock_guard<std::mutex> lock(agent->discovery_mutex);
        auto &last_json = agent->discovered_devices[dev_id];
        if (!dev_id.empty() && last_json == json) {
            return;
        }
        last_json = json;
    }
    log_line(agent, "discovery emit dev_id=" + masked_len(json_string_value(json, "dev_id")) +
                    " dev_ip=" + masked_len(json_string_value(json, "dev_ip")));
    dispatch_callback(agent, [fn = agent->on_ssdp, json]() { fn(json); });
}

void emit_seed_devices(Agent *agent)
{
    if (!agent || agent->config_dir.empty()) return;
    const std::string path = agent->config_dir + "/arm64_discovery_devices.jsonl";
    std::ifstream in(path);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() != '{' || line.back() != '}') {
            log_line(agent, "seed discovery skipped invalid line");
            continue;
        }
        log_line(agent, "seed discovery line loaded");
        emit_discovery(agent, line);
    }
}

void send_msearch(int fd, uint16_t port, const char *st)
{
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, "239.255.255.250", &dest.sin_addr);
    std::ostringstream msg;
    msg << "M-SEARCH * HTTP/1.1\r\n"
        << "HOST: 239.255.255.250:" << port << "\r\n"
        << "MAN: \"ssdp:discover\"\r\n"
        << "MX: 1\r\n"
        << "ST: " << st << "\r\n\r\n";
    const auto s = msg.str();
    sendto(fd, s.data(), s.size(), 0, reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
}

int create_udp_socket(Agent *agent, uint16_t port, bool multicast)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_line(agent, std::string("discovery socket failed port=") + std::to_string(port) + " errno=" + std::to_string(errno));
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) != 0) {
        log_line(agent, std::string("discovery bind failed port=") + std::to_string(port) + " errno=" + std::to_string(errno));
        close(fd);
        return -1;
    }

    if (multicast) {
        ip_mreq mreq{};
        inet_pton(AF_INET, "239.255.255.250", &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            log_line(agent, std::string("discovery multicast join failed port=") + std::to_string(port) + " errno=" + std::to_string(errno));
        }
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    log_line(agent, std::string("discovery socket ready port=") + std::to_string(port) + (multicast ? " multicast=true" : " multicast=false"));
    return fd;
}

void discovery_loop(Agent *agent)
{
    log_line(agent, "discovery worker start");
    const int probe_fd = create_udp_socket(agent, 0, false);
    if (probe_fd < 0) {
        return;
    }
    std::vector<int> fds{probe_fd};
    const int ssdp_fd = create_udp_socket(agent, 1900, true);
    if (ssdp_fd >= 0) fds.push_back(ssdp_fd);
    const int bambu_fd = create_udp_socket(agent, 1990, true);
    if (bambu_fd >= 0) fds.push_back(bambu_fd);

    auto last_probe = std::chrono::steady_clock::time_point{};
    while (agent->discovery_running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_probe > std::chrono::seconds(5)) {
            send_msearch(probe_fd, 1900, "ssdp:all");
            send_msearch(probe_fd, 1900, "urn:bambulab-com:device:3dprinter:1");
            send_msearch(probe_fd, 1990, "ssdp:all");
            send_msearch(probe_fd, 1990, "urn:bambulab-com:device:3dprinter:1");
            log_line(agent, "discovery probe sent");
            last_probe = now;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = -1;
        for (int fd : fds) {
            FD_SET(fd, &readfds);
            max_fd = std::max(max_fd, fd);
        }
        timeval tv{};
        tv.tv_sec = 1;
        const int ready = select(max_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (ready <= 0) continue;

        for (int fd : fds) {
            if (!FD_ISSET(fd, &readfds)) continue;
            while (true) {
                char buf[8192];
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                const ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
                if (n <= 0) break;
                buf[n] = '\0';
                const std::string packet(buf, static_cast<size_t>(n));
                const std::string sender_ip = ip_from_sockaddr(from);
                const std::string lowered = lower_copy(packet);
                if (lowered.find("bambu") != std::string::npos || lowered.find("bbl") != std::string::npos || lowered.find("3dprinter") != std::string::npos) {
                    log_line(agent, "discovery candidate sender=" + masked_len(sender_ip) +
                                    " bytes=" + std::to_string(n));
                }
                emit_discovery(agent, discovery_json_from_packet(packet, sender_ip));
            }
        }
    }

    for (int fd : fds) close(fd);
    log_line(agent, "discovery worker stop");
}

void stop_discovery(Agent *agent)
{
    if (!agent) return;
    const bool was_running = agent->discovery_running.exchange(false);
    if (was_running && agent->discovery_thread.joinable()) {
        agent->discovery_thread.join();
    }
}

Agent::~Agent()
{
    stop_discovery(this);
    stop_mqtt(this);
}

bool tcp_probe(const std::string &host, int port, int timeout_ms, std::string &error)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    const std::string port_s = std::to_string(port);
    const int gai = getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
    if (gai != 0) {
        error = std::string("dns:") + gai_strerror(gai);
        return false;
    }

    bool ok = false;
    for (addrinfo *rp = res; rp; rp = rp->ai_next) {
        const int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            error = std::string("socket errno=") + std::to_string(errno);
            continue;
        }

        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        const int ret = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (ret == 0) {
            ok = true;
            close(fd);
            break;
        }
        if (errno != EINPROGRESS) {
            error = std::string("connect errno=") + std::to_string(errno);
            close(fd);
            continue;
        }

        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int sel = select(fd + 1, nullptr, &writefds, nullptr, &tv);
        if (sel > 0 && FD_ISSET(fd, &writefds)) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error == 0) {
                ok = true;
                close(fd);
                break;
            }
            error = std::string("so_error=") + std::to_string(so_error);
        } else if (sel == 0) {
            error = "timeout";
        } else {
            error = std::string("select errno=") + std::to_string(errno);
        }
        close(fd);
    }

    freeaddrinfo(res);
    return ok;
}

void append_u16(std::vector<uint8_t> &out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_mqtt_string(std::vector<uint8_t> &out, const std::string &value)
{
    append_u16(out, static_cast<uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<uint8_t> mqtt_packet(uint8_t header, const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> out;
    out.push_back(header);
    size_t remaining = payload.size();
    do {
        uint8_t encoded = static_cast<uint8_t>(remaining % 128);
        remaining /= 128;
        if (remaining > 0) encoded |= 0x80;
        out.push_back(encoded);
    } while (remaining > 0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool ssl_write_all(Agent *agent, const std::vector<uint8_t> &packet)
{
    if (!agent || !agent->ssl) return false;
    std::lock_guard<std::mutex> lock(agent->mqtt_mutex);
    size_t offset = 0;
    while (offset < packet.size()) {
        const int written = SSL_write(agent->ssl, packet.data() + offset, static_cast<int>(packet.size() - offset));
        if (written <= 0) {
            log_line(agent, "mqtt SSL_write failed err=" + std::to_string(SSL_get_error(agent->ssl, written)));
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool ssl_read_exact_locked(Agent *agent, uint8_t *buf, size_t size)
{
    size_t offset = 0;
    while (offset < size && agent->mqtt_running.load()) {
        const int got = SSL_read(agent->ssl, buf + offset, static_cast<int>(size - offset));
        if (got > 0) {
            offset += static_cast<size_t>(got);
            continue;
        }
        const int err = SSL_get_error(agent->ssl, got);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
        if (agent->mqtt_running.load()) log_line(agent, "mqtt SSL_read failed err=" + std::to_string(err));
        return false;
    }
    return offset == size;
}

bool mqtt_read_packet(Agent *agent, uint8_t &type, std::vector<uint8_t> &payload)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(agent->mqtt_fd, &readfds);
    timeval tv{};
    tv.tv_sec = 1;
    const int ready = select(agent->mqtt_fd + 1, &readfds, nullptr, nullptr, &tv);
    if (ready == 0) {
        type = 0;
        payload.clear();
        return true;
    }
    if (ready < 0) return false;

    uint8_t header = 0;
    if (!ssl_read_exact_locked(agent, &header, 1)) return false;
    type = header;

    size_t multiplier = 1;
    size_t remaining = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t encoded = 0;
        if (!ssl_read_exact_locked(agent, &encoded, 1)) return false;
        remaining += (encoded & 127) * multiplier;
        if ((encoded & 128) == 0) break;
        multiplier *= 128;
        if (i == 3) return false;
    }

    payload.assign(remaining, 0);
    if (remaining == 0) return true;
    return ssl_read_exact_locked(agent, payload.data(), payload.size());
}

bool mqtt_send_connect(Agent *agent, const std::string &dev_id, const std::string &username, const std::string &password)
{
    std::vector<uint8_t> payload;
    append_mqtt_string(payload, "MQTT");
    payload.push_back(4);
    uint8_t flags = 0x02;
    if (!username.empty()) flags |= 0x80;
    if (!password.empty()) flags |= 0x40;
    payload.push_back(flags);
    append_u16(payload, 60);
    append_mqtt_string(payload, "bambu_studio_arm64_" + dev_id.substr(dev_id.size() > 8 ? dev_id.size() - 8 : 0));
    if (!username.empty()) append_mqtt_string(payload, username);
    if (!password.empty()) append_mqtt_string(payload, password);
    return ssl_write_all(agent, mqtt_packet(0x10, payload));
}

uint16_t next_packet_id(Agent *agent)
{
    const uint16_t id = agent->mqtt_packet_id++;
    if (agent->mqtt_packet_id == 0) agent->mqtt_packet_id = 1;
    return id == 0 ? 1 : id;
}

bool mqtt_send_subscribe(Agent *agent, const std::string &dev_id)
{
    std::vector<uint8_t> payload;
    append_u16(payload, next_packet_id(agent));
    append_mqtt_string(payload, "device/" + dev_id + "/report");
    payload.push_back(0);
    return ssl_write_all(agent, mqtt_packet(0x82, payload));
}

bool mqtt_send_publish(Agent *agent, const std::string &dev_id, const std::string &json, int qos)
{
    std::vector<uint8_t> payload;
    append_mqtt_string(payload, "device/" + dev_id + "/request");
    const int mqtt_qos = qos > 0 ? 1 : 0;
    if (mqtt_qos) append_u16(payload, next_packet_id(agent));
    payload.insert(payload.end(), json.begin(), json.end());
    return ssl_write_all(agent, mqtt_packet(static_cast<uint8_t>(0x30 | (mqtt_qos << 1)), payload));
}

void mqtt_send_puback(Agent *agent, uint16_t packet_id)
{
    std::vector<uint8_t> payload;
    append_u16(payload, packet_id);
    ssl_write_all(agent, mqtt_packet(0x40, payload));
}

void mqtt_reader_loop(Agent *agent, std::string dev_id)
{
    log_line(agent, "mqtt reader start dev_id=" + masked_len(dev_id));
    auto last_ping = std::chrono::steady_clock::now();
    while (agent->mqtt_running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ping > std::chrono::seconds(25)) {
            ssl_write_all(agent, mqtt_packet(0xc0, {}));
            last_ping = now;
        }

        uint8_t type = 0;
        std::vector<uint8_t> payload;
        if (!mqtt_read_packet(agent, type, payload)) break;
        if (type == 0) continue;
        const uint8_t packet_type = type & 0xf0;
        if (packet_type == 0x30) {
            if (payload.size() < 2) continue;
            const size_t topic_len = (static_cast<size_t>(payload[0]) << 8) | payload[1];
            if (payload.size() < 2 + topic_len) continue;
            size_t offset = 2 + topic_len;
            uint16_t packet_id = 0;
            const int qos = (type >> 1) & 0x03;
            if (qos > 0 && payload.size() >= offset + 2) {
                packet_id = static_cast<uint16_t>((payload[offset] << 8) | payload[offset + 1]);
                offset += 2;
            }
            std::string msg(reinterpret_cast<const char *>(payload.data() + offset), payload.size() - offset);
            bool liveview_injected = false;
            msg = ensure_local_liveview_advertised(msg, &liveview_injected);
            log_line(agent, "mqtt publish received dev_id=" + masked_len(dev_id) +
                                    " bytes=" + std::to_string(msg.size()));
            if (liveview_injected)
                log_line(agent, "mqtt status injected ipcam.liveview local protocol");
            auto fn = agent->on_local_message ? agent->on_local_message : agent->on_message;
            if (fn) dispatch_callback(agent, [fn, dev_id, msg]() { fn(dev_id, msg); });
            if (qos == 1 && packet_id != 0) mqtt_send_puback(agent, packet_id);
        } else if (packet_type == 0x90) {
            log_line(agent, "mqtt suback bytes=" + std::to_string(payload.size()));
        } else if (packet_type == 0xd0) {
            log_line(agent, "mqtt pingresp");
        } else if (packet_type == 0x40) {
            log_line(agent, "mqtt puback");
        } else {
            log_line(agent, "mqtt packet type=0x" + std::to_string(packet_type) + " bytes=" + std::to_string(payload.size()));
        }
    }
    agent->mqtt_running.store(false);
    log_line(agent, "mqtt reader stop dev_id=" + masked_len(dev_id));
}

void stop_mqtt(Agent *agent)
{
    if (!agent) return;
    agent->mqtt_running.store(false);
    if (agent->mqtt_fd >= 0) shutdown(agent->mqtt_fd, SHUT_RDWR);
    if (agent->mqtt_thread.joinable()) agent->mqtt_thread.join();
    if (agent->ssl) {
        SSL_free(agent->ssl);
        agent->ssl = nullptr;
    }
    if (agent->ssl_ctx) {
        SSL_CTX_free(agent->ssl_ctx);
        agent->ssl_ctx = nullptr;
    }
    if (agent->mqtt_fd >= 0) {
        close(agent->mqtt_fd);
        agent->mqtt_fd = -1;
    }
}

bool start_mqtt(Agent *agent, const std::string &dev_id, const std::string &host, int port, const std::string &username, const std::string &password, std::string &error)
{
    stop_mqtt(agent);
    SSL_library_init();
    SSL_load_error_strings();

    agent->mqtt_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (agent->mqtt_fd < 0) {
        error = "socket errno=" + std::to_string(errno);
        return false;
    }

    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(agent->mqtt_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(agent->mqtt_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        error = "invalid ip";
        stop_mqtt(agent);
        return false;
    }
    if (connect(agent->mqtt_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        error = "connect errno=" + std::to_string(errno);
        stop_mqtt(agent);
        return false;
    }

    agent->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!agent->ssl_ctx) {
        error = "SSL_CTX_new failed";
        stop_mqtt(agent);
        return false;
    }
    // Printer LAN certificates are self-signed; authenticate with a persisted
    // first-use certificate pin instead of public CA validation.
    agent->ssl = SSL_new(agent->ssl_ctx);
    if (!agent->ssl) {
        error = "SSL_new failed";
        stop_mqtt(agent);
        return false;
    }
    SSL_set_fd(agent->ssl, agent->mqtt_fd);
    SSL_set_tlsext_host_name(agent->ssl, host.c_str());
    if (SSL_connect(agent->ssl) != 1) {
        error = "SSL_connect failed err=" + std::to_string(ERR_get_error());
        stop_mqtt(agent);
        return false;
    }
    if (!verify_or_trust_peer(agent, agent->ssl, host, port, error)) {
        stop_mqtt(agent);
        return false;
    }

    agent->mqtt_running.store(true);
    if (!mqtt_send_connect(agent, dev_id, username, password)) {
        error = "mqtt connect write failed";
        stop_mqtt(agent);
        return false;
    }

    uint8_t type = 0;
    std::vector<uint8_t> payload;
    if (!mqtt_read_packet(agent, type, payload) || (type & 0xf0) != 0x20 || payload.size() < 2) {
        error = "mqtt connack read failed";
        stop_mqtt(agent);
        return false;
    }
    const int rc = payload[1];
    log_line(agent, "mqtt connected connack rc=" + std::to_string(rc));
    if (rc != 0) {
        error = "mqtt connack rc=" + std::to_string(rc);
        stop_mqtt(agent);
        return false;
    }

    if (!mqtt_send_subscribe(agent, dev_id)) {
        error = "mqtt subscribe write failed";
        stop_mqtt(agent);
        return false;
    }
    bool subscribed = false;
    const auto subscribe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < subscribe_deadline) {
        uint8_t sub_type = 0;
        std::vector<uint8_t> sub_payload;
        if (!mqtt_read_packet(agent, sub_type, sub_payload)) {
            error = "mqtt suback read failed";
            stop_mqtt(agent);
            return false;
        }
        if (sub_type == 0) continue;
        if ((sub_type & 0xf0) == 0x90) {
            log_line(agent, "mqtt suback bytes=" + std::to_string(sub_payload.size()));
            subscribed = true;
            break;
        }
        log_line(agent, "mqtt pre-reader packet type=0x" + std::to_string(sub_type & 0xf0) + " bytes=" + std::to_string(sub_payload.size()));
    }
    if (!subscribed) {
        error = "mqtt suback timeout";
        stop_mqtt(agent);
        return false;
    }
    agent->mqtt_thread = std::thread([agent, dev_id]() { mqtt_reader_loop(agent, dev_id); });
    return true;
}

int local_upload_and_start_print(Agent *agent, BBL::PrintParams params, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn cancel)
{
    if (!agent) return kInvalid;
    if (cancel && cancel()) return BAMBU_NETWORK_ERR_CANCELED;
    if (params.filename.empty()) {
        if (update) dispatch_callback(agent, [update]() { update(BBL::PrintingStageERROR, BAMBU_NETWORK_ERR_FILE_NOT_EXIST, "missing filename"); });
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }

    if (update) dispatch_callback(agent, [update]() { update(BBL::PrintingStageUpload, 0, "Uploading file over LAN"); });
    std::string remote_path;
    std::string error;
    if (!ftps_upload_file(agent, params, params.filename, params.ftp_folder, remote_path, error)) {
        if (update) dispatch_callback(agent, [update, error]() { update(BBL::PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, error); });
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }

    if (cancel && cancel()) return BAMBU_NETWORK_ERR_CANCELED;
    if (!agent->mqtt_running.load() || !agent->ssl) {
        error = "mqtt not connected";
        if (update) dispatch_callback(agent, [update, error]() { update(BBL::PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, error); });
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    const std::string payload = project_file_payload(params, remote_path);
    log_line(agent, "local print publish dev_id=" + masked_len(params.dev_id) +
                    " remote=" + path_basename(remote_path) +
                    " payload_bytes=" + std::to_string(payload.size()));
    if (update) dispatch_callback(agent, [update]() { update(BBL::PrintingStageSending, 0, "Starting print over LAN"); });
    if (!mqtt_send_publish(agent, params.dev_id, payload, 1)) {
        error = "mqtt publish failed";
        if (update) dispatch_callback(agent, [update, error]() { update(BBL::PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, error); });
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }

    if (update) dispatch_callback(agent, [update, remote_path]() { update(BBL::PrintingStageWaiting, BAMBU_NETWORK_SUCCESS, remote_path); });
    return BAMBU_NETWORK_SUCCESS;
}

void set_connected(Agent *agent, const std::string &dev_id, const std::string &dev_ip, int port, bool use_ssl)
{
    std::lock_guard<std::mutex> lock(agent->connection_mutex);
    agent->connected_dev_id = dev_id;
    agent->connected_dev_ip = dev_ip;
    agent->connected_port = port;
    agent->connected_use_ssl = use_ssl;
}

void clear_connected(Agent *agent)
{
    std::lock_guard<std::mutex> lock(agent->connection_mutex);
    agent->connected_dev_id.clear();
    agent->connected_dev_ip.clear();
    agent->connected_port = 0;
    agent->connected_use_ssl = false;
}
}

extern "C" {

bool bambu_network_check_debug_consistent(bool) { return true; }
std::string bambu_network_get_version()
{
    log_call(__func__);
    return "02.07.01.62";
}

void *bambu_network_create_agent(std::string log_dir)
{
    auto *agent = new Agent;
    agent->log_dir = std::move(log_dir);
    log_line(agent, "bambu_network_create_agent log_dir=<set>");
    return agent;
}

int bambu_network_destroy_agent(void *agent)
{
    log_call(static_cast<Agent *>(agent), __func__);
    delete static_cast<Agent *>(agent);
    return BAMBU_NETWORK_SUCCESS;
}

int bambu_network_init_log(void *agent)
{
    log_call(static_cast<Agent *>(agent), __func__);
    return agent ? BAMBU_NETWORK_SUCCESS : kInvalid;
}

int bambu_network_set_config_dir(void *agent, std::string config_dir)
{
    log_call(static_cast<Agent *>(agent), __func__);
    if (!agent) return kInvalid;
    static_cast<Agent *>(agent)->config_dir = std::move(config_dir);
    return BAMBU_NETWORK_SUCCESS;
}

int bambu_network_set_cert_file(void *agent, std::string, std::string)
{
    log_call(static_cast<Agent *>(agent), __func__);
    return agent ? BAMBU_NETWORK_SUCCESS : kInvalid;
}

int bambu_network_set_country_code(void *agent, std::string country_code)
{
    log_call(static_cast<Agent *>(agent), __func__);
    if (!agent) return kInvalid;
    static_cast<Agent *>(agent)->country_code = std::move(country_code);
    return BAMBU_NETWORK_SUCCESS;
}

int bambu_network_start(void *agent)
{
    log_call(static_cast<Agent *>(agent), __func__);
    return agent ? BAMBU_NETWORK_SUCCESS : kInvalid;
}

int bambu_network_set_on_ssdp_msg_fn(void *agent, BBL::OnMsgArrivedFn fn) { return set_cb(agent, &Agent::on_ssdp, std::move(fn), __func__); }
int bambu_network_set_on_user_login_fn(void *agent, BBL::OnUserLoginFn fn) { return set_cb(agent, &Agent::on_user_login, std::move(fn), __func__); }
int bambu_network_set_on_printer_connected_fn(void *agent, BBL::OnPrinterConnectedFn fn) { return set_cb(agent, &Agent::on_printer_connected, std::move(fn), __func__); }
int bambu_network_set_on_server_connected_fn(void *agent, BBL::OnServerConnectedFn fn) { return set_cb(agent, &Agent::on_server_connected, std::move(fn), __func__); }
int bambu_network_set_on_http_error_fn(void *agent, BBL::OnHttpErrorFn fn) { return set_cb(agent, &Agent::on_http_error, std::move(fn), __func__); }
int bambu_network_set_get_country_code_fn(void *agent, BBL::GetCountryCodeFn fn) { return set_cb(agent, &Agent::get_country_code, std::move(fn), __func__); }
int bambu_network_set_on_subscribe_failure_fn(void *agent, BBL::GetSubscribeFailureFn fn) { return set_cb(agent, &Agent::on_subscribe_failure, std::move(fn), __func__); }
int bambu_network_set_on_message_fn(void *agent, BBL::OnMessageFn fn) { return set_cb(agent, &Agent::on_message, std::move(fn), __func__); }
int bambu_network_set_on_user_message_fn(void *agent, BBL::OnMessageFn fn) { return set_cb(agent, &Agent::on_user_message, std::move(fn), __func__); }
int bambu_network_set_on_local_connect_fn(void *agent, BBL::OnLocalConnectedFn fn) { return set_cb(agent, &Agent::on_local_connect, std::move(fn), __func__); }
int bambu_network_set_on_local_message_fn(void *agent, BBL::OnMessageFn fn) { return set_cb(agent, &Agent::on_local_message, std::move(fn), __func__); }
int bambu_network_set_queue_on_main_fn(void *agent, BBL::QueueOnMainFn fn) { return set_cb(agent, &Agent::queue_on_main, std::move(fn), __func__); }
int bambu_network_set_server_callback(void *agent, BBL::OnServerErrFn fn) { return set_cb(agent, &Agent::on_server_error, std::move(fn), __func__); }

int bambu_network_connect_server(void *agent) { log_call(static_cast<Agent *>(agent), __func__); return kUnsupported; }
bool bambu_network_is_server_connected(void *agent)
{
    auto *a = static_cast<Agent *>(agent);
    log_call(a, __func__);
    if (!a) return false;
    std::lock_guard<std::mutex> lock(a->connection_mutex);
    return !a->connected_dev_id.empty();
}
int bambu_network_refresh_connection(void *agent) { log_call(static_cast<Agent *>(agent), __func__); return kUnsupported; }
int bambu_network_start_subscribe(void *agent, std::string) { log_call(static_cast<Agent *>(agent), __func__); return kUnsupported; }
int bambu_network_stop_subscribe(void *agent, std::string) { log_call(static_cast<Agent *>(agent), __func__); return BAMBU_NETWORK_SUCCESS; }
int bambu_network_add_subscribe(void *agent, std::vector<std::string>) { log_call(static_cast<Agent *>(agent), __func__); return kUnsupported; }
int bambu_network_del_subscribe(void *agent, std::vector<std::string>) { log_call(static_cast<Agent *>(agent), __func__); return BAMBU_NETWORK_SUCCESS; }

void bambu_network_enable_multi_machine(void *agent, bool enable)
{
    log_call(static_cast<Agent *>(agent), __func__);
    if (agent) static_cast<Agent *>(agent)->multi_machine = enable;
}

int bambu_network_send_message(void *agent, std::string, std::string, int, int)
{
    log_call(static_cast<Agent *>(agent), __func__);
    return kUnsupported;
}
int bambu_network_connect_printer(void *agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    auto *a = static_cast<Agent *>(agent);
    log_line(a, std::string(__func__) + " dev_id=" + masked_len(dev_id) +
                    " dev_ip=" + masked_len(dev_ip) +
                    " username=" + masked_len(username) + " password_set=" + (!password.empty() ? "true" : "false") +
                    " use_ssl=" + (use_ssl ? "true" : "false"));
    if (!agent) return kInvalid;

    std::thread([a, dev_id = std::move(dev_id), dev_ip = std::move(dev_ip), username = std::move(username), password = std::move(password), use_ssl]() {
        const int first_port = use_ssl ? 8883 : 1883;
        const int second_port = use_ssl ? 1883 : 8883;
        std::string error;
        int connected_port = 0;

        if (tcp_probe(dev_ip, first_port, 1800, error)) {
            connected_port = first_port;
        } else {
            log_line(a, "connect probe failed dev_ip=" + masked_len(dev_ip) +
                        " port=" + std::to_string(first_port) + " error=" + error);
            error.clear();
            if (tcp_probe(dev_ip, second_port, 1800, error)) {
                connected_port = second_port;
            }
        }

        if (connected_port != 0) {
            if (connected_port != 8883) {
                log_line(a, "connect rejected non-tls-mqtt port=" + std::to_string(connected_port));
                if (a->on_local_connect) {
                    dispatch_callback(a, [fn = a->on_local_connect, dev_id]() {
                        fn(BBL::ConnectStatusFailed, dev_id, "secure_mqtt_unavailable");
                    });
                }
                return;
            }
            std::string mqtt_error;
            if (!start_mqtt(a, dev_id, dev_ip, connected_port, username.empty() ? "bblp" : username, password, mqtt_error)) {
                clear_connected(a);
                log_line(a, "mqtt start failed dev_id=" + masked_len(dev_id) +
                            " dev_ip=" + masked_len(dev_ip) + " error=" + mqtt_error);
                if (a->on_local_connect) {
                    dispatch_callback(a, [fn = a->on_local_connect, dev_id, mqtt_error]() {
                        fn(BBL::ConnectStatusFailed, dev_id, mqtt_error.empty() ? "mqtt_failed" : mqtt_error);
                    });
                }
                return;
            }
            set_connected(a, dev_id, dev_ip, connected_port, connected_port == 8883);
            log_line(a, "connect ok dev_id=" + masked_len(dev_id) +
                        " dev_ip=" + masked_len(dev_ip) +
                        " port=" + std::to_string(connected_port));
            if (a->on_local_connect) {
                dispatch_callback(a, [fn = a->on_local_connect, dev_id]() {
                    fn(BBL::ConnectStatusOk, dev_id, "mqtt_ok");
                });
            }
            return;
        }

        clear_connected(a);
        log_line(a, "connect probe failed dev_id=" + masked_len(dev_id) +
                    " dev_ip=" + masked_len(dev_ip) + " error=" + error);
        if (a->on_local_connect) {
            dispatch_callback(a, [fn = a->on_local_connect, dev_id, error]() {
                fn(BBL::ConnectStatusFailed, dev_id, error.empty() ? "tcp_probe_failed" : error);
            });
        }
    }).detach();

    return BAMBU_NETWORK_SUCCESS;
}
int bambu_network_disconnect_printer(void *agent)
{
    auto *a = static_cast<Agent *>(agent);
    log_call(a, __func__);
    if (!a) return kInvalid;
    std::string dev_id;
    {
        std::lock_guard<std::mutex> lock(a->connection_mutex);
        dev_id = a->connected_dev_id;
    }
    clear_connected(a);
    stop_mqtt(a);
    if (!dev_id.empty() && a->on_local_connect) {
        dispatch_callback(a, [fn = a->on_local_connect, dev_id]() {
            fn(BBL::ConnectStatusLost, dev_id, "disconnect");
        });
    }
    return BAMBU_NETWORK_SUCCESS;
}
int bambu_network_send_message_to_printer(void *agent, std::string dev_id, std::string json_str, int qos, int flag)
{
    auto *a = static_cast<Agent *>(agent);
    log_line(a, std::string(__func__) + " dev_id=" + masked_len(dev_id) + " qos=" + std::to_string(qos) +
                    " flag=" + std::to_string(flag) + " bytes=" + std::to_string(json_str.size()));
    if (!a) return kInvalid;
    if (!a->mqtt_running.load() || !a->ssl) {
        log_line(a, "mqtt publish skipped not connected");
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
    if (!mqtt_send_publish(a, dev_id, json_str, qos)) return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    log_line(a, "mqtt publish sent dev_id=" + masked_len(dev_id) + " bytes=" + std::to_string(json_str.size()));
    return BAMBU_NETWORK_SUCCESS;
}
int bambu_network_update_cert(void *agent) { log_call(static_cast<Agent *>(agent), __func__); return agent ? BAMBU_NETWORK_SUCCESS : kInvalid; }
void bambu_network_install_device_cert(void *agent, std::string dev_id, bool lan_only)
{
    auto *a = static_cast<Agent *>(agent);
    if (!a) return;
    if (dev_id.empty()) {
        log_line(a, std::string(__func__) + " skipped empty dev_id lan_only=" + (lan_only ? "true" : "false"));
        return;
    }
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(a->cert_mutex);
        first = a->cert_installed_devices.insert(dev_id).second;
    }
    if (!first) return;
    log_line(a, std::string(__func__) + " dev_id=" + masked_len(dev_id) +
                    " lan_only=" + (lan_only ? "true" : "false") + " status=device_cert_installed");
    auto fn = a->on_local_message ? a->on_local_message : a->on_message;
    if (fn) dispatch_callback(a, [fn, dev_id]() { fn(dev_id, "device_cert_installed"); });
}
bool bambu_network_start_discovery(void *agent, bool start, bool sending)
{
    auto *a = static_cast<Agent *>(agent);
    log_line(a, std::string(__func__) + " start=" + (start ? "true" : "false") + " sending=" + (sending ? "true" : "false"));
    if (!a) return false;

    if (!start) {
        stop_discovery(a);
        return true;
    }

    bool expected = false;
    if (a->discovery_running.compare_exchange_strong(expected, true)) {
        a->discovery_thread = std::thread([a]() { discovery_loop(a); });
    }
    emit_seed_devices(a);
    return true;
}

int bambu_network_change_user(void *agent, std::string) { log_call(static_cast<Agent *>(agent), __func__); return BAMBU_NETWORK_SUCCESS; }
bool bambu_network_is_user_login(void *agent) { log_call(static_cast<Agent *>(agent), __func__); return false; }
int bambu_network_user_logout(void *agent, bool) { log_call(static_cast<Agent *>(agent), __func__); return BAMBU_NETWORK_SUCCESS; }
std::string bambu_network_get_user_id(void *) { return {}; }
std::string bambu_network_get_user_name(void *) { return {}; }
std::string bambu_network_get_user_avatar(void *) { return {}; }
std::string bambu_network_get_user_nickanme(void *) { return {}; }
std::string bambu_network_build_login_cmd(void *) { return {}; }
std::string bambu_network_build_logout_cmd(void *) { return {}; }
std::string bambu_network_build_login_info(void *) { return {}; }
int bambu_network_ping_bind(void *agent, std::string) { log_call(static_cast<Agent *>(agent), __func__); return kUnsupported; }
int bambu_network_bind_detect(void *agent, std::string dev_ip, std::string, BBL::detectResult &detect)
{
    log_call(static_cast<Agent *>(agent), __func__);
    auto *a = static_cast<Agent *>(agent);
    if (!a) return kInvalid;
    if (dev_ip.empty()) {
        std::lock_guard<std::mutex> lock(a->connection_mutex);
        dev_ip = a->connected_dev_ip;
    }
    std::string matched_json;
    {
        std::lock_guard<std::mutex> lock(a->discovery_mutex);
        for (const auto &entry : a->discovered_devices) {
            if (dev_ip.empty() || json_string_value(entry.second, "dev_ip") == dev_ip) {
                matched_json = entry.second;
                break;
            }
        }
    }
    if (!matched_json.empty()) {
        fill_detect_result_from_json(detect, matched_json);
    } else if (!(matched_json = seed_discovery_for_ip(a, dev_ip)).empty()) {
        fill_detect_result_from_json(detect, matched_json);
    } else {
        detect.model_id = "N2S";
        detect.bind_state = "free";
        detect.connect_type = "lan";
    }
    detect.result_msg = "ok";
    log_line(a, std::string(__func__) + " ok dev_id=" + masked_len(detect.dev_id) + " model_id=" + detect.model_id);
    return BAMBU_NETWORK_SUCCESS;
}
int bambu_network_report_consent(void *agent, std::string) { log_call(static_cast<Agent *>(agent), __func__); return BAMBU_NETWORK_SUCCESS; }
int bambu_network_bind(void *agent, std::string, std::string, std::string, std::string, bool, BBL::OnUpdateStatusFn) { log_call(static_cast<Agent *>(agent), __func__); return kUnsupported; }
int bambu_network_unbind(void *agent, std::string) { log_call(static_cast<Agent *>(agent), __func__); return kUnsupported; }
std::string bambu_network_get_bambulab_host(void *) { return {}; }
std::string bambu_network_get_user_selected_machine(void *) { return {}; }
int bambu_network_set_user_selected_machine(void *, std::string) { return BAMBU_NETWORK_SUCCESS; }

int bambu_network_start_print(void *agent, BBL::PrintParams, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn, BBL::OnWaitFn)
{
    log_call(static_cast<Agent *>(agent), __func__);
    if (update) dispatch_callback(static_cast<Agent *>(agent), [update]() { update(BBL::PrintingStageERROR, kUnsupported, "ARM64 stub plugin: cloud print unsupported"); });
    return kUnsupported;
}
int bambu_network_start_local_print_with_record(void *agent, BBL::PrintParams params, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn cancel, BBL::OnWaitFn)
{
    auto *a = static_cast<Agent *>(agent);
    log_line(a, std::string(__func__) + " dev_id=" + masked_len(params.dev_id) + " dev_ip=" + masked_len(params.dev_ip) +
                    " filename=" + path_basename(params.filename) + " folder=" + params.ftp_folder);
    return local_upload_and_start_print(a, std::move(params), std::move(update), std::move(cancel));
}
int bambu_network_start_send_gcode_to_sdcard(void *agent, BBL::PrintParams params, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn cancel, BBL::OnWaitFn)
{
    auto *a = static_cast<Agent *>(agent);
    log_line(a, std::string(__func__) + " dev_id=" + masked_len(params.dev_id) + " dev_ip=" + masked_len(params.dev_ip) +
                    " filename=" + path_basename(params.filename) + " folder=" + params.ftp_folder);
    if (!a) return kInvalid;
    if (cancel && cancel()) return BAMBU_NETWORK_ERR_CANCELED;
    if (params.filename.empty()) {
        if (update) dispatch_callback(a, [update]() { update(BBL::PrintingStageERROR, BAMBU_NETWORK_ERR_FILE_NOT_EXIST, "missing filename"); });
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    }
    if (update) dispatch_callback(a, [update]() { update(BBL::PrintingStageUpload, 0, "Uploading file over LAN"); });
    std::string remote_path;
    std::string error;
    if (!ftps_upload_file(a, params, params.filename, params.ftp_folder, remote_path, error)) {
        if (update) dispatch_callback(a, [update, error]() { update(BBL::PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED, error); });
        return BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED;
    }
    if (update) dispatch_callback(a, [update, remote_path]() { update(BBL::PrintingStageFinished, BAMBU_NETWORK_SUCCESS, remote_path); });
    return BAMBU_NETWORK_SUCCESS;
}
int bambu_network_start_local_print(void *agent, BBL::PrintParams params, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn cancel)
{
    auto *a = static_cast<Agent *>(agent);
    log_line(a, std::string(__func__) + " dev_id=" + masked_len(params.dev_id) + " dev_ip=" + masked_len(params.dev_ip) +
                    " filename=" + path_basename(params.filename) + " folder=" + params.ftp_folder);
    return local_upload_and_start_print(a, std::move(params), std::move(update), std::move(cancel));
}
int bambu_network_start_sdcard_print(void *agent, BBL::PrintParams, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn)
{
    log_call(static_cast<Agent *>(agent), __func__);
    if (update) dispatch_callback(static_cast<Agent *>(agent), [update]() { update(BBL::PrintingStageERROR, kUnsupported, "ARM64 stub plugin: sdcard print unsupported"); });
    return kUnsupported;
}

int bambu_network_get_user_presets(void *, std::map<std::string, std::map<std::string, std::string>> *presets)
{
    if (presets) presets->clear();
    return BAMBU_NETWORK_SUCCESS;
}
std::string bambu_network_request_setting_id(void *, std::string, std::map<std::string, std::string> *, unsigned int *http_code)
{
    if (http_code) *http_code = 501;
    return {};
}
int bambu_network_put_setting(void *, std::string, std::string, std::map<std::string, std::string> *, unsigned int *http_code)
{
    if (http_code) *http_code = 501;
    return kUnsupported;
}
int bambu_network_get_setting_list(void *, std::string, BBL::ProgressFn, BBL::WasCancelledFn) { return kUnsupported; }
int bambu_network_get_setting_list2(void *, std::string, BBL::CheckFn, BBL::ProgressFn, BBL::WasCancelledFn) { return kUnsupported; }
int bambu_network_delete_setting(void *, std::string) { return kUnsupported; }
std::string bambu_network_get_studio_info_url(void *) { return {}; }
int bambu_network_set_extra_http_header(void *, std::map<std::string, std::string>) { return BAMBU_NETWORK_SUCCESS; }
int bambu_network_get_my_message(void *, int, int, int, unsigned int *code, std::string *body) { set_http(code, body); return kUnsupported; }
int bambu_network_check_user_task_report(void *, int *, bool *printable) { if (printable) *printable = false; return kUnsupported; }
int bambu_network_get_user_print_info(void *, unsigned int *code, std::string *body) { set_http(code, body); return kUnsupported; }
int bambu_network_get_user_tasks(void *, BBL::TaskQueryParams, std::string *body) { if (body) body->clear(); return kUnsupported; }
int bambu_network_get_filament_spools(void *, BBL::FilamentQueryParams, std::string *body) { if (body) body->clear(); return kUnsupported; }
int bambu_network_create_filament_spool(void *, std::string, std::string *body) { if (body) body->clear(); return kUnsupported; }
int bambu_network_update_filament_spool(void *, std::string, std::string, std::string *body) { if (body) body->clear(); return kUnsupported; }
int bambu_network_delete_filament_spools(void *, BBL::FilamentDeleteParams, std::string *body) { if (body) body->clear(); return kUnsupported; }
int bambu_network_get_filament_config(void *, std::string *body) { if (body) body->clear(); return kUnsupported; }
int bambu_network_get_printer_firmware(void *, std::string, unsigned *code, std::string *body) { if (code) *code = 501; if (body) body->clear(); return kUnsupported; }
int bambu_network_get_task_plate_index(void *, std::string, int *plate_index) { if (plate_index) *plate_index = 0; return kUnsupported; }
int bambu_network_get_user_info(void *, int *identifier) { if (identifier) *identifier = 0; return kUnsupported; }
int bambu_network_request_bind_ticket(void *, std::string *ticket) { if (ticket) ticket->clear(); return kUnsupported; }
int bambu_network_get_subtask_info(void *, std::string, std::string *task_json, unsigned int *code, std::string *body)
{
    if (task_json) task_json->clear();
    set_http(code, body);
    return kUnsupported;
}
int bambu_network_get_slice_info(void *, std::string, std::string, int, std::string *slice_json) { if (slice_json) slice_json->clear(); return kUnsupported; }
int bambu_network_query_bind_status(void *, std::vector<std::string>, unsigned int *code, std::string *body) { set_http(code, body); return kUnsupported; }
int bambu_network_modify_printer_name(void *, std::string, std::string) { return kUnsupported; }
int bambu_network_get_camera_url(void *, std::string, std::function<void(std::string)> cb) { if (cb) cb({}); return kUnsupported; }
int bambu_network_get_camera_url_for_golive(void *, std::string, std::string, std::function<void(std::string)> cb) { if (cb) cb({}); return kUnsupported; }
int bambu_network_get_design_staffpick(void *, int, int, std::function<void(std::string)> cb) { if (cb) cb("[]"); return kUnsupported; }
int bambu_network_start_publish(void *, BBL::PublishParams, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn, std::string *out)
{
    if (out) out->clear();
    if (update) update(BBL::PublishingJumpUrl, kUnsupported, "ARM64 stub plugin: publish unsupported");
    return kUnsupported;
}
int bambu_network_get_model_publish_url(void *, std::string *url) { if (url) url->clear(); return kUnsupported; }
int bambu_network_get_subtask(void *, Slic3r::BBLModelTask *, Slic3r::OnGetSubTaskFn) { return kUnsupported; }
int bambu_network_get_model_mall_home_url(void *, std::string *url) { if (url) url->clear(); return kUnsupported; }
int bambu_network_get_model_mall_detail_url(void *, std::string *url, std::string) { if (url) url->clear(); return kUnsupported; }
int bambu_network_get_my_profile(void *, std::string, unsigned int *code, std::string *body) { set_http(code, body); return kUnsupported; }
int bambu_network_get_my_token(void *, std::string, unsigned int *code, std::string *body) { set_http(code, body); return kUnsupported; }
int bambu_network_track_enable(void *agent, bool enable) { if (agent) static_cast<Agent *>(agent)->tracking = enable; return BAMBU_NETWORK_SUCCESS; }
int bambu_network_track_remove_files(void *) { return BAMBU_NETWORK_SUCCESS; }
int bambu_network_track_event(void *, std::string, std::string) { return BAMBU_NETWORK_SUCCESS; }
int bambu_network_track_header(void *, std::string) { return BAMBU_NETWORK_SUCCESS; }
int bambu_network_track_update_property(void *, std::string, std::string, std::string) { return BAMBU_NETWORK_SUCCESS; }
int bambu_network_track_get_property(void *, std::string, std::string &value, std::string) { value.clear(); return BAMBU_NETWORK_SUCCESS; }
int bambu_network_put_model_mall_rating(void *, int, int, std::string, std::vector<std::string>, unsigned int &code, std::string &err) { code = 501; err = "unsupported"; return kUnsupported; }
int bambu_network_get_oss_config(void *, std::string &config, std::string, unsigned int &code, std::string &err) { config.clear(); code = 501; err = "unsupported"; return kUnsupported; }
int bambu_network_put_rating_picture_oss(void *, std::string &, std::string &, std::string, int, unsigned int &code, std::string &err) { code = 501; err = "unsupported"; return kUnsupported; }
int bambu_network_get_model_mall_rating(void *, int, std::string &result, unsigned int &code, std::string &err) { result.clear(); code = 501; err = "unsupported"; return kUnsupported; }
int bambu_network_get_mw_user_preference(void *, std::function<void(std::string)> cb) { if (cb) cb("{}"); return kUnsupported; }
int bambu_network_get_mw_user_4ulist(void *, int, int, std::function<void(std::string)> cb) { if (cb) cb("[]"); return kUnsupported; }
int bambu_network_get_hms_snapshot(void *, std::string &, std::string &, std::function<void(std::string, int)> cb) { if (cb) cb({}, kUnsupported); return kUnsupported; }

struct ft_job_result {
    int ec;
    int resp_ec;
    const char *json;
    const void *bin;
    uint32_t bin_size;
};

struct ft_job_msg {
    int kind;
    const char *json;
};

int ft_abi_version() { return 1; }
void ft_free(void *p) { std::free(p); }
void ft_job_result_destroy(ft_job_result *) {}
void ft_job_msg_destroy(ft_job_msg *) {}

int ft_tunnel_create(const char *, FT_TunnelHandle **out)
{
    if (!out) return -1;
    *out = new FT_TunnelHandle;
    return 0;
}
void ft_tunnel_retain(FT_TunnelHandle *h) { if (h) ++h->refs; }
void ft_tunnel_release(FT_TunnelHandle *h) { if (h && --h->refs == 0) delete h; }
int ft_tunnel_start_connect(FT_TunnelHandle *, void (*cb)(void *, int, int, const char *), void *user)
{
    if (cb) cb(user, 0, -3, "ARM64 stub plugin: file-transfer tunnel unsupported");
    return -3;
}
int ft_tunnel_sync_connect(FT_TunnelHandle *) { return -3; }
int ft_tunnel_set_status_cb(FT_TunnelHandle *, void (*)(void *, int, int, int, const char *), void *) { return 0; }
int ft_tunnel_shutdown(FT_TunnelHandle *) { return 0; }

int ft_job_create(const char *, FT_JobHandle **out)
{
    if (!out) return -1;
    *out = new FT_JobHandle;
    return 0;
}
void ft_job_retain(FT_JobHandle *h) { if (h) ++h->refs; }
void ft_job_release(FT_JobHandle *h) { if (h && --h->refs == 0) delete h; }
int ft_job_set_result_cb(FT_JobHandle *h, void (*cb)(void *, ft_job_result), void *user)
{
    if (cb) {
        ft_job_result r{-3, -3, h ? h->result_json.c_str() : "unsupported", nullptr, 0};
        cb(user, r);
    }
    return 0;
}
int ft_job_get_result(FT_JobHandle *h, uint32_t, ft_job_result *out)
{
    if (!out) return -1;
    *out = {-3, -3, h ? h->result_json.c_str() : "unsupported", nullptr, 0};
    return -3;
}
int ft_tunnel_start_job(FT_TunnelHandle *, FT_JobHandle *) { return -3; }
int ft_job_cancel(FT_JobHandle *) { return -5; }
int ft_job_set_msg_cb(FT_JobHandle *, void (*)(void *, ft_job_msg), void *) { return 0; }
int ft_job_try_get_msg(FT_JobHandle *, ft_job_msg *out)
{
    if (out) *out = {0, nullptr};
    return -4;
}
int ft_job_get_msg(FT_JobHandle *, uint32_t, ft_job_msg *out)
{
    if (out) *out = {0, nullptr};
    return -4;
}

}
