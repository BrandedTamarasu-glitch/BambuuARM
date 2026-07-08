#include "Printer/BambuTunnel.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct StubTunnel {
    std::string path;
    std::string host;
    std::string user;
    std::string passwd;
    std::string authkey;
    std::string cli_id;
    std::string cli_ver;
    int port = 6000;
    Logger logger = nullptr;
    void *logger_context = nullptr;
    bool open = false;
    bool local = false;
    bool live_control_sent = false;
    bool first_sample_ready = false;
    bool stream_info_known = false;
    bool stream_is_mjpeg = true;
    int read_wait_count = 0;
    int fd = -1;
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    uint32_t sequence = 0;
    std::vector<uint8_t> first_sample;
    uint64_t first_decode_time = 0;
    uint32_t max_frame_size = 1024 * 1024;
    uint64_t sample_count = 0;
};

std::mutex g_error_mutex;
char g_last_error[512] = "ARM64 BambuSource: stream backend is not implemented";

std::string log_path()
{
    const char *home = std::getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.var/app/com.bambulab.BambuStudio/config/BambuStudio/arm64_bambu_source.log";
    return "/tmp/arm64_bambu_source.log";
}

std::string config_dir()
{
    const char *home = std::getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.var/app/com.bambulab.BambuStudio/config/BambuStudio";
    return "/tmp";
}

void log_line(const std::string &message)
{
    std::ofstream log(log_path(), std::ios::app);
    if (!log)
        return;
    std::time_t now = std::time(nullptr);
    char ts[32] = {};
    std::strftime(ts, sizeof(ts), "%F %T", std::localtime(&now));
    log << ts << " " << message << "\n";
}

bool verbose_logging()
{
    const char *value = std::getenv("BAMBU_ARM_VERBOSE_LOG");
    if (!value)
        return false;
    return std::strcmp(value, "1") == 0 ||
           std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 ||
           std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "YES") == 0 ||
           std::strcmp(value, "on") == 0 ||
           std::strcmp(value, "ON") == 0;
}

void log_debug(const std::string &message)
{
    if (verbose_logging())
        log_line(message);
}

std::string masked(const std::string &value)
{
    if (value.empty())
        return "(empty)";
    return "len=" + std::to_string(value.size());
}

std::string masked_host(const std::string &value)
{
    if (value.empty())
        return "(empty)";
    return "host_len=" + std::to_string(value.size());
}

std::string mask_url_secrets(std::string value)
{
    constexpr const char *local_prefix = "bambu:///local/";
    if (value.rfind(local_prefix, 0) == 0) {
        size_t host_start = std::strlen(local_prefix);
        size_t host_end = value.find('?', host_start);
        if (host_end == std::string::npos)
            host_end = value.size();
        value.replace(host_start, host_end - host_start, "<redacted-host>");
    }
    for (const char *key : {"passwd=", "authkey=", "cli_id="}) {
        size_t pos = 0;
        while ((pos = value.find(key, pos)) != std::string::npos) {
            pos += std::strlen(key);
            size_t end = value.find('&', pos);
            value.replace(pos, end == std::string::npos ? std::string::npos : end - pos, "<redacted>");
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
    }
    return value;
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

std::string peer_cert_fingerprint(SSL *ssl)
{
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert)
        return {};
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_len = 0;
    const bool ok = X509_digest(cert, EVP_sha256(), digest, &digest_len) == 1;
    X509_free(cert);
    return ok ? to_hex(digest, digest_len) : std::string();
}

bool verify_or_trust_peer(SSL *ssl, const std::string &host, int port, std::string &error)
{
    const std::string fingerprint = peer_cert_fingerprint(ssl);
    if (fingerprint.empty()) {
        error = "missing peer certificate";
        return false;
    }

    const std::string key = host + ":" + std::to_string(port);
    const std::string path = config_dir() + "/arm64_trusted_tls_pins.txt";
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream parts(line);
            std::string stored_key;
            std::string stored_fingerprint;
            if (parts >> stored_key >> stored_fingerprint) {
                if (stored_key == key) {
                    if (stored_fingerprint == fingerprint)
                        return true;
                    error = "peer certificate changed for " + masked_host(host);
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
    log_line("trusted first peer certificate for " + masked_host(host) +
             " port=" + std::to_string(port));
    return true;
}

void set_error(const char *message)
{
    std::lock_guard<std::mutex> lock(g_error_mutex);
    std::snprintf(g_last_error, sizeof(g_last_error), "%s",
                  message ? message : "ARM64 BambuSource: unknown error");
}

void tunnel_log(StubTunnel *tunnel, int level, const char *message)
{
    if (tunnel && tunnel->logger)
        tunnel->logger(tunnel->logger_context, level, message);
}

struct FrameHeader {
    uint32_t size;
    uint8_t marker;
    uint8_t channel;
    uint8_t kind;
    uint8_t version;
    uint32_t sequence;
    uint32_t reserved;
};
static_assert(sizeof(FrameHeader) == 16);

std::string query_value(const std::string &query, const std::string &key)
{
    size_t pos = 0;
    while (pos < query.size()) {
        size_t next = query.find('&', pos);
        std::string part = query.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key)
            return part.substr(eq + 1);
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return {};
}

bool parse_local_url(StubTunnel *tunnel)
{
    constexpr const char *prefix = "bambu:///local/";
    if (tunnel->path.rfind(prefix, 0) != 0)
        return false;

    size_t start = std::strlen(prefix);
    size_t q = tunnel->path.find('?', start);
    std::string host_part = tunnel->path.substr(start, q == std::string::npos ? std::string::npos : q - start);
    if (!host_part.empty() && host_part.back() == '.')
        host_part.pop_back();
    tunnel->host = host_part;
    tunnel->local = !tunnel->host.empty();

    std::string query = q == std::string::npos ? std::string() : tunnel->path.substr(q + 1);
    std::string port = query_value(query, "port");
    if (!port.empty())
        tunnel->port = std::atoi(port.c_str());
    tunnel->user = query_value(query, "user");
    tunnel->passwd = query_value(query, "passwd");
    tunnel->authkey = query_value(query, "authkey");
    tunnel->cli_id = query_value(query, "cli_id");
    tunnel->cli_ver = query_value(query, "cli_ver");
    return tunnel->local;
}

bool ssl_write_all(SSL *ssl, const uint8_t *data, size_t size)
{
    size_t off = 0;
    while (off < size) {
        int n = SSL_write(ssl, data + off, static_cast<int>(size - off));
        if (n <= 0)
            return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool ssl_read_all(SSL *ssl, uint8_t *data, size_t size, int *last_result = nullptr)
{
    size_t off = 0;
    while (off < size) {
        int n = SSL_read(ssl, data + off, static_cast<int>(size - off));
        if (n <= 0) {
            if (last_result)
                *last_result = n;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool write_frame(StubTunnel *tunnel, uint8_t kind, const std::vector<uint8_t> &payload)
{
    FrameHeader header{};
    header.size = static_cast<uint32_t>(payload.size());
    header.marker = 0x3f;
    header.channel = 0x01;
    header.kind = kind;
    header.version = 0x01;
    header.sequence = tunnel->sequence++;
    return ssl_write_all(tunnel->ssl, reinterpret_cast<const uint8_t *>(&header), sizeof(header)) &&
           (payload.empty() || ssl_write_all(tunnel->ssl, payload.data(), payload.size()));
}

bool write_control3000(StubTunnel *tunnel)
{
    FrameHeader header{};
    header.size = 64;
    header.marker = 0x00;
    header.channel = 0x30;
    header.kind = 0x00;
    header.version = 0x00;

    std::vector<uint8_t> payload(64, 0);
    // x86 BambuTunnelLocal copies the URL user into the first 32-byte slot,
    // then passwd into the second, before sending the 0x3000 liveview control.
    std::memcpy(payload.data(), tunnel->user.c_str(), std::min<size_t>(32, tunnel->user.size()));
    std::memcpy(payload.data() + 32, tunnel->passwd.c_str(), std::min<size_t>(32, tunnel->passwd.size()));

    log_line("write_control3000 user=" + masked(tunnel->user) +
             " passwd=" + masked(tunnel->passwd));
    std::vector<uint8_t> frame(sizeof(header) + payload.size());
    std::memcpy(frame.data(), &header, sizeof(header));
    std::memcpy(frame.data() + sizeof(header), payload.data(), payload.size());
    return ssl_write_all(tunnel->ssl, frame.data(), frame.size());
}

enum class ReadFrameResult {
    ok,
    wait,
    closed,
    error,
};

ReadFrameResult read_frame(StubTunnel *tunnel, FrameHeader &header, std::vector<uint8_t> &payload)
{
    int last_result = 0;
    if (!ssl_read_all(tunnel->ssl, reinterpret_cast<uint8_t *>(&header), sizeof(header), &last_result)) {
        int ssl_error = SSL_get_error(tunnel->ssl, last_result);
        unsigned long err = ERR_get_error();
        log_line("read_frame header failed ssl_result=" + std::to_string(last_result) +
                 " ssl_error=" + std::to_string(ssl_error) +
                 " err=" + std::to_string(err));
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
            return ReadFrameResult::wait;
        if (ssl_error == SSL_ERROR_ZERO_RETURN)
            return ReadFrameResult::closed;
        return ReadFrameResult::error;
    }
    if (header.size > 1024 * 1024) {
        log_line("read_frame oversized size=" + std::to_string(header.size));
        return ReadFrameResult::error;
    }
    payload.assign(header.size, 0);
    if (!payload.empty() && !ssl_read_all(tunnel->ssl, payload.data(), payload.size(), &last_result)) {
        int ssl_error = SSL_get_error(tunnel->ssl, last_result);
        unsigned long err = ERR_get_error();
        log_line("read_frame payload failed size=" + std::to_string(header.size) +
                 " ssl_result=" + std::to_string(last_result) +
                 " ssl_error=" + std::to_string(ssl_error) +
                 " err=" + std::to_string(err));
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
            return ReadFrameResult::wait;
        if (ssl_error == SSL_ERROR_ZERO_RETURN)
            return ReadFrameResult::closed;
        return ReadFrameResult::error;
    }
    return ReadFrameResult::ok;
}

bool looks_like_jpeg(const std::vector<uint8_t> &payload)
{
    return payload.size() >= 2 && payload[0] == 0xff && payload[1] == 0xd8;
}

bool looks_like_h264_annexb(const std::vector<uint8_t> &payload)
{
    return (payload.size() >= 4 &&
            payload[0] == 0x00 && payload[1] == 0x00 &&
            ((payload[2] == 0x01) || (payload[2] == 0x00 && payload[3] == 0x01)));
}

const char *stream_codec_name(const StubTunnel *tunnel)
{
    return tunnel && tunnel->stream_is_mjpeg ? "mjpeg" : "h264";
}

void fill_stream_info(StubTunnel *tunnel, Bambu_StreamInfo *info)
{
    if (!info)
        return;
    std::memset(info, 0, sizeof(*info));
    info->type = VIDE;
    info->sub_type = tunnel && !tunnel->stream_is_mjpeg ? AVC1 : MJPG;
    info->format.video.width = 1920;
    info->format.video.height = 1080;
    info->format.video.frame_rate = 15;
    info->format_type = tunnel && !tunnel->stream_is_mjpeg ? video_avc_byte_stream : video_jpeg;
    info->max_frame_size = static_cast<int>(tunnel ? tunnel->max_frame_size : 1024 * 1024);
}

ReadFrameResult prefetch_stream_info_sample(StubTunnel *tunnel)
{
    if (!tunnel || tunnel->first_sample_ready)
        return ReadFrameResult::ok;

    FrameHeader header{};
    std::vector<uint8_t> payload;
    ReadFrameResult result = read_frame(tunnel, header, payload);
    if (result != ReadFrameResult::ok)
        return result;

    tunnel->first_sample = std::move(payload);
    tunnel->first_decode_time = header.sequence;
    tunnel->first_sample_ready = true;
    tunnel->read_wait_count = 0;
    tunnel->stream_is_mjpeg = looks_like_jpeg(tunnel->first_sample) ||
                              !looks_like_h264_annexb(tunnel->first_sample);
    tunnel->stream_info_known = true;
    tunnel->max_frame_size = std::max<uint32_t>(
        1024 * 1024,
        static_cast<uint32_t>(tunnel->first_sample.size()));
    log_line("prefetch_stream_info_sample codec=" + std::string(stream_codec_name(tunnel)) +
             " size=" + std::to_string(tunnel->first_sample.size()) +
             " kind=" + std::to_string(header.kind) +
             " sequence=" + std::to_string(header.sequence));
    return ReadFrameResult::ok;
}

void close_local(StubTunnel *tunnel)
{
    if (!tunnel)
        return;
    if (tunnel->ssl) {
        SSL_shutdown(tunnel->ssl);
        SSL_free(tunnel->ssl);
        tunnel->ssl = nullptr;
    }
    if (tunnel->ctx) {
        SSL_CTX_free(tunnel->ctx);
        tunnel->ctx = nullptr;
    }
    if (tunnel->fd >= 0) {
        close(tunnel->fd);
        tunnel->fd = -1;
    }
    tunnel->open = false;
}

int connect_local(StubTunnel *tunnel)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(tunnel->port));
    if (inet_pton(AF_INET, tunnel->host.c_str(), &addr.sin_addr) != 1) {
        set_error("ARM64 BambuSource local URL has invalid host");
        return Bambu_stream_end;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_error("ARM64 BambuSource socket failed");
        return Bambu_stream_end;
    }
    timeval tv{};
    tv.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        set_error("ARM64 BambuSource TCP connect failed");
        return Bambu_stream_end;
    }

    SSL_library_init();
    SSL_load_error_strings();
    tunnel->ctx = SSL_CTX_new(TLS_client_method());
    if (!tunnel->ctx) {
        close(fd);
        set_error("ARM64 BambuSource SSL_CTX_new failed");
        return Bambu_stream_end;
    }
    // Printer LAN certificates are self-signed; authenticate with a persisted
    // first-use certificate pin instead of public CA validation.
    SSL_CTX_set_cipher_list(tunnel->ctx, "HIGH:MEDIA:LOW:!DH");
    tunnel->ssl = SSL_new(tunnel->ctx);
    tunnel->fd = fd;
    SSL_set_fd(tunnel->ssl, fd);
    if (SSL_connect(tunnel->ssl) != 1) {
        unsigned long err = ERR_get_error();
        log_line("Bambu_Open SSL_connect failed err=" + std::to_string(err));
        close_local(tunnel);
        set_error("ARM64 BambuSource SSL connect failed");
        return Bambu_stream_end;
    }
    std::string pin_error;
    if (!verify_or_trust_peer(tunnel->ssl, tunnel->host, tunnel->port, pin_error)) {
        log_line("Bambu_Open TLS pin failed " + pin_error);
        close_local(tunnel);
        set_error("ARM64 BambuSource TLS peer check failed");
        return Bambu_stream_end;
    }

    tunnel->sequence = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    tunnel->open = true;
    return Bambu_success;
}

StubTunnel *as_tunnel(Bambu_Tunnel tunnel)
{
    return static_cast<StubTunnel *>(tunnel);
}

int unavailable(StubTunnel *tunnel)
{
    constexpr const char *message =
        "ARM64 BambuSource shim loaded; requested non-local media path is unsupported";
    set_error(message);
    tunnel_log(tunnel, 2, message);
    return Bambu_buffer_limit;
}

int start_local_live(StubTunnel *stub, int type)
{
    if (!stub || !stub->local || !stub->ssl)
        return unavailable(stub);

    if (type != 0x3000)
        log_line("start_local_live unexpected type=" + std::to_string(type));

    if (!stub->live_control_sent) {
        if (!write_control3000(stub)) {
            set_error("ARM64 BambuSource local live-control write failed");
            log_line("start_local_live ctrl3000 write failed");
            return Bambu_stream_end;
        }
        stub->live_control_sent = true;
        stub->read_wait_count = 0;
        set_error("ARM64 BambuSource local live-control sent");
        log_line("start_local_live ctrl3000 sent");
    }

    return Bambu_success;
}

}

extern "C" int Bambu_Create(Bambu_Tunnel *tunnel, char const *path)
{
    if (!tunnel) {
        set_error("Bambu_Create called with null tunnel output");
        return Bambu_stream_end;
    }

    auto *created = new StubTunnel();
    if (path)
        created->path = path;
    parse_local_url(created);
    *tunnel = created;
    log_line(std::string("Bambu_Create path=") + (path ? mask_url_secrets(path) : "(null)"));
    set_error("ARM64 BambuSource tunnel created");
    return Bambu_success;
}

extern "C" void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void *context)
{
    auto *stub = as_tunnel(tunnel);
    if (!stub)
        return;
    stub->logger = logger;
    stub->logger_context = context;
    log_line("Bambu_SetLogger");
}

extern "C" int Bambu_Open(Bambu_Tunnel tunnel)
{
    auto *stub = as_tunnel(tunnel);
    if (!stub) {
        set_error("Bambu_Open called with null tunnel");
        return Bambu_stream_end;
    }

    if (parse_local_url(stub)) {
        log_line("Bambu_Open local " + masked_host(stub->host) + " port=" + std::to_string(stub->port) +
                 " user=" + stub->user + " passwd=" + masked(stub->passwd) +
                 " authkey=" + masked(stub->authkey) +
                 " cli_id=" + masked(stub->cli_id) + " cli_ver=" + stub->cli_ver);
        int rc = connect_local(stub);
        log_line("Bambu_Open local rc=" + std::to_string(rc));
        return rc;
    }

    stub->open = true;
    log_line("Bambu_Open path=" + mask_url_secrets(stub->path));
    return unavailable(stub);
}

extern "C" int Bambu_StartStream(Bambu_Tunnel tunnel, bool video)
{
    auto *stub = as_tunnel(tunnel);
    log_line(std::string("Bambu_StartStream video=") + (video ? "true" : "false"));
    if (!stub || !stub->local || !stub->ssl)
        return unavailable(stub);

    if (video)
        return start_local_live(stub, 0x3000);

    std::vector<uint8_t> auth(16, 0);
    std::memcpy(auth.data(), stub->user.c_str(), std::min<size_t>(8, stub->user.size()));
    std::memcpy(auth.data() + 8, stub->passwd.c_str(), std::min<size_t>(8, stub->passwd.size()));
    if (!write_frame(stub, 0x01, auth)) {
        set_error("ARM64 BambuSource local auth write failed");
        log_line("Bambu_StartStream auth write failed");
        return Bambu_stream_end;
    }
    return Bambu_would_block;
}

extern "C" int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type)
{
    log_line("Bambu_StartStreamEx type=" + std::to_string(type));
    return start_local_live(as_tunnel(tunnel), type);
}

extern "C" int Bambu_GetStreamCount(Bambu_Tunnel tunnel)
{
    log_line("Bambu_GetStreamCount");
    auto *stub = as_tunnel(tunnel);
    return stub && stub->local ? 1 : 0;
}

extern "C" int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo *info)
{
    if (index != 0) {
        set_error("ARM64 BambuSource stream index out of range");
        return Bambu_stream_end;
    }

    auto *stub = as_tunnel(tunnel);
    if (!stub || !stub->local || !stub->ssl) {
        fill_stream_info(stub, info);
        log_line("Bambu_GetStreamInfo fallback codec=" + std::string(stream_codec_name(stub)));
        set_error("ARM64 BambuSource returning fallback stream info");
        return Bambu_success;
    }

    if (!stub->stream_info_known && stub->live_control_sent) {
        ReadFrameResult result = ReadFrameResult::wait;
        for (int attempt = 0; attempt < 3 && result == ReadFrameResult::wait; ++attempt)
            result = prefetch_stream_info_sample(stub);
        if (result == ReadFrameResult::closed) {
            set_error("ARM64 BambuSource local stream closed before stream info");
            return Bambu_stream_end;
        }
        if (result == ReadFrameResult::error) {
            set_error("ARM64 BambuSource local stream info read failed");
            return Bambu_stream_end;
        }
        if (result == ReadFrameResult::wait) {
            stub->stream_is_mjpeg = true;
            stub->stream_info_known = true;
            log_line("Bambu_GetStreamInfo no prefetch sample, defaulting codec=mjpeg");
        }
    }

    fill_stream_info(stub, info);
    log_line("Bambu_GetStreamInfo codec=" + std::string(stream_codec_name(stub)) +
             " max_frame_size=" + std::to_string(stub->max_frame_size));
    set_error("ARM64 BambuSource returning local stream info");
    return Bambu_success;
}

extern "C" unsigned long Bambu_GetDuration(Bambu_Tunnel /*tunnel*/)
{
    return 0;
}

extern "C" int Bambu_Seek(Bambu_Tunnel tunnel, unsigned long /*time*/)
{
    return unavailable(as_tunnel(tunnel));
}

extern "C" int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample *sample)
{
    auto *stub = as_tunnel(tunnel);
    if (sample)
        std::memset(sample, 0, sizeof(*sample));
    if (!stub || !sample || !stub->local || !stub->ssl)
        return Bambu_stream_end;

    if (stub->first_sample_ready) {
        sample->itrack = 0;
        sample->size = static_cast<int>(stub->first_sample.size());
        sample->flags = f_sync;
        sample->buffer = stub->first_sample.data();
        sample->decode_time = stub->first_decode_time;
        stub->first_sample_ready = false;
        ++stub->sample_count;
        log_debug("Bambu_ReadSample returning first size=" + std::to_string(sample->size));
        return Bambu_success;
    }

    FrameHeader header{};
    std::vector<uint8_t> payload;
    ReadFrameResult read_result = read_frame(stub, header, payload);
    if (read_result != ReadFrameResult::ok) {
        if (read_result == ReadFrameResult::closed) {
            set_error("ARM64 BambuSource local stream closed before first frame");
            return Bambu_stream_end;
        }
        if (read_result == ReadFrameResult::error) {
            set_error("ARM64 BambuSource local stream read failed");
            return Bambu_stream_end;
        }
        stub->read_wait_count++;
        if (verbose_logging() && (stub->read_wait_count == 1 || stub->read_wait_count % 30 == 0))
            log_line("Bambu_ReadSample waiting count=" + std::to_string(stub->read_wait_count));
        set_error("ARM64 BambuSource local stream waiting for sample");
        return Bambu_would_block;
    }
    stub->read_wait_count = 0;
    stub->first_sample = std::move(payload);
    sample->itrack = 0;
    sample->size = static_cast<int>(stub->first_sample.size());
    sample->flags = stub->stream_is_mjpeg ? f_sync : 0;
    sample->buffer = stub->first_sample.data();
    sample->decode_time = header.sequence;
    ++stub->sample_count;
    if (verbose_logging() && (stub->sample_count <= 3 || stub->sample_count % 150 == 0))
        log_line("Bambu_ReadSample returning size=" + std::to_string(sample->size) +
                 " kind=" + std::to_string(header.kind) +
                 " count=" + std::to_string(stub->sample_count));
    return Bambu_success;
}

extern "C" int Bambu_SendMessage(Bambu_Tunnel tunnel, int /*ctrl*/, char const * /*data*/, int /*len*/)
{
    return unavailable(as_tunnel(tunnel));
}

extern "C" int Bambu_RecvMessage(Bambu_Tunnel /*tunnel*/, int *ctrl, char *data, int *len)
{
    if (ctrl)
        *ctrl = 0;
    if (data && len && *len > 0)
        data[0] = '\0';
    if (len)
        *len = 0;
    set_error("ARM64 BambuSource has no message available");
    return Bambu_stream_end;
}

extern "C" void Bambu_Close(Bambu_Tunnel tunnel)
{
    auto *stub = as_tunnel(tunnel);
    if (stub) {
        close_local(stub);
        stub->live_control_sent = false;
        stub->first_sample_ready = false;
        stub->stream_info_known = false;
        stub->stream_is_mjpeg = true;
        stub->read_wait_count = 0;
        stub->first_sample.clear();
        stub->max_frame_size = 1024 * 1024;
        stub->sample_count = 0;
    }
    log_line("Bambu_Close");
}

extern "C" void Bambu_Destroy(Bambu_Tunnel tunnel)
{
    log_line("Bambu_Destroy");
    delete as_tunnel(tunnel);
}

extern "C" int Bambu_Init()
{
    set_error("ARM64 BambuSource initialized");
    return Bambu_success;
}

extern "C" void Bambu_Deinit()
{
}

extern "C" char const *Bambu_GetLastErrorMsg()
{
    return g_last_error;
}

extern "C" void Bambu_FreeLogMsg(tchar const * /*msg*/)
{
}
