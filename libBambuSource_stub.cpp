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
#include <openssl/ssl.h>
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
    int read_wait_count = 0;
    int fd = -1;
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    uint32_t sequence = 0;
    std::vector<uint8_t> first_sample;
    uint64_t first_decode_time = 0;
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

bool ssl_read_all(SSL *ssl, uint8_t *data, size_t size)
{
    size_t off = 0;
    while (off < size) {
        int n = SSL_read(ssl, data + off, static_cast<int>(size - off));
        if (n <= 0)
            return false;
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
    // x86 BambuTunnelLocal stores passwd first, then authkey, before sending
    // the 0x3000 liveview control frame. LAN URLs commonly omit authkey.
    std::memcpy(payload.data(), tunnel->passwd.c_str(), std::min<size_t>(32, tunnel->passwd.size()));
    std::memcpy(payload.data() + 32, tunnel->authkey.c_str(), std::min<size_t>(32, tunnel->authkey.size()));

    return ssl_write_all(tunnel->ssl, reinterpret_cast<const uint8_t *>(&header), sizeof(header)) &&
           ssl_write_all(tunnel->ssl, payload.data(), payload.size());
}

bool read_frame(StubTunnel *tunnel, FrameHeader &header, std::vector<uint8_t> &payload)
{
    if (!ssl_read_all(tunnel->ssl, reinterpret_cast<uint8_t *>(&header), sizeof(header))) {
        int ssl_error = SSL_get_error(tunnel->ssl, -1);
        unsigned long err = ERR_get_error();
        if (err)
            log_line("read_frame header failed ssl_error=" + std::to_string(ssl_error) +
                     " err=" + std::to_string(err));
        return false;
    }
    if (header.size > 1024 * 1024) {
        log_line("read_frame oversized size=" + std::to_string(header.size));
        return false;
    }
    payload.assign(header.size, 0);
    if (!payload.empty() && !ssl_read_all(tunnel->ssl, payload.data(), payload.size())) {
        int ssl_error = SSL_get_error(tunnel->ssl, -1);
        unsigned long err = ERR_get_error();
        if (err)
            log_line("read_frame payload failed size=" + std::to_string(header.size) +
                     " ssl_error=" + std::to_string(ssl_error) +
                     " err=" + std::to_string(err));
        return false;
    }
    return true;
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
    SSL_CTX_set_verify(tunnel->ctx, SSL_VERIFY_NONE, nullptr);
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
        "ARM64 BambuSource shim loaded; camera/video streaming is not implemented yet";
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
    log_line(std::string("Bambu_Create path=") + (path ? path : "(null)"));
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
        log_line("Bambu_Open local host=" + stub->host + " port=" + std::to_string(stub->port) +
                 " user=" + stub->user + " cli_id=" + stub->cli_id);
        int rc = connect_local(stub);
        log_line("Bambu_Open local rc=" + std::to_string(rc));
        return rc;
    }

    stub->open = true;
    log_line("Bambu_Open path=" + stub->path);
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
    std::memcpy(auth.data(), stub->authkey.c_str(), std::min<size_t>(8, stub->authkey.size()));
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

extern "C" int Bambu_GetStreamInfo(Bambu_Tunnel /*tunnel*/, int index, Bambu_StreamInfo *info)
{
    if (index != 0) {
        set_error("ARM64 BambuSource stream index out of range");
        return Bambu_stream_end;
    }
    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->type = VIDE;
        info->sub_type = AVC1;
        info->format.video.width = 1920;
        info->format.video.height = 1080;
        info->format.video.frame_rate = 15;
        info->format_type = video_avc_byte_stream;
        info->max_frame_size = 0xbb80;
    }
    log_line("Bambu_GetStreamInfo");
    set_error("ARM64 BambuSource returning provisional H.264 stream info");
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
        log_line("Bambu_ReadSample returning first size=" + std::to_string(sample->size));
        return Bambu_success;
    }

    FrameHeader header{};
    std::vector<uint8_t> payload;
    if (!read_frame(stub, header, payload)) {
        stub->read_wait_count++;
        if (stub->read_wait_count == 1 || stub->read_wait_count % 10 == 0)
            log_line("Bambu_ReadSample waiting count=" + std::to_string(stub->read_wait_count));
        set_error("ARM64 BambuSource local stream waiting for sample");
        return Bambu_would_block;
    }
    stub->read_wait_count = 0;
    stub->first_sample = std::move(payload);
    sample->itrack = 0;
    sample->size = static_cast<int>(stub->first_sample.size());
    sample->flags = 0;
    sample->buffer = stub->first_sample.data();
    sample->decode_time = header.sequence;
    log_line("Bambu_ReadSample returning size=" + std::to_string(sample->size) +
             " kind=" + std::to_string(header.kind));
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
        stub->read_wait_count = 0;
        stub->first_sample.clear();
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
