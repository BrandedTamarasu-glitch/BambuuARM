#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

bool ssl_write_all(SSL *ssl, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        int n = SSL_write(ssl, data + offset, static_cast<int>(size - offset));
        if (n <= 0)
            return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool ssl_read_all(SSL *ssl, uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        int n = SSL_read(ssl, data + offset, static_cast<int>(size - offset));
        if (n <= 0)
            return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool write_frame(SSL *ssl, uint8_t kind, uint32_t sequence, const std::vector<uint8_t> &payload)
{
    FrameHeader header{};
    header.size = static_cast<uint32_t>(payload.size());
    header.marker = 0x3f;
    header.channel = 0x01;
    header.kind = kind;
    header.version = 0x01;
    header.sequence = sequence;

    return ssl_write_all(ssl, reinterpret_cast<const uint8_t *>(&header), sizeof(header)) &&
           (payload.empty() || ssl_write_all(ssl, payload.data(), payload.size()));
}

bool write_raw_header_frame(SSL *ssl, uint32_t size, uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7,
                            uint32_t sequence, const std::vector<uint8_t> &payload)
{
    FrameHeader header{};
    header.size = size;
    header.marker = b4;
    header.channel = b5;
    header.kind = b6;
    header.version = b7;
    header.sequence = sequence;

    return ssl_write_all(ssl, reinterpret_cast<const uint8_t *>(&header), sizeof(header)) &&
           (payload.empty() || ssl_write_all(ssl, payload.data(), payload.size()));
}

bool read_frame(SSL *ssl, FrameHeader &header, std::vector<uint8_t> &payload)
{
    if (!ssl_read_all(ssl, reinterpret_cast<uint8_t *>(&header), sizeof(header)))
        return false;
    if (header.size > 1024 * 1024) {
        std::cerr << "refusing oversized frame: " << header.size << "\n";
        return false;
    }
    payload.assign(header.size, 0);
    return payload.empty() || ssl_read_all(ssl, payload.data(), payload.size());
}

void dump_frame(const char *label, const FrameHeader &header, const std::vector<uint8_t> &payload)
{
    std::cout << label << " size=" << header.size
              << " marker=0x" << std::hex << static_cast<int>(header.marker)
              << " channel=0x" << static_cast<int>(header.channel)
              << " kind=0x" << static_cast<int>(header.kind)
              << " version=0x" << static_cast<int>(header.version)
              << std::dec << " sequence=" << header.sequence
              << " reserved=" << header.reserved << "\n";

    std::cout << "payload:";
    const size_t n = std::min<size_t>(payload.size(), 96);
    for (size_t i = 0; i < n; ++i)
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(payload[i]);
    std::cout << std::dec << std::setfill(' ') << "\n";

    bool printable = true;
    for (auto c : payload) {
        if (c == 0)
            continue;
        if (c < 0x20 || c > 0x7e) {
            printable = false;
            break;
        }
    }
    if (printable && !payload.empty())
        std::cout << "text=" << std::string(reinterpret_cast<const char *>(payload.data()), payload.size()) << "\n";
}

int connect_tcp(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    timeval tv{};
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: probe-local-tunnel <host> <access-code> [client-id] [client-version] [variant] [mode] [sequence]\n";
        return 2;
    }
    const char *host = argv[1];
    const char *access = argv[2];
    const char *client_id = argc > 3 ? argv[3] : "arm64probe";
    const char *client_ver = argc > 4 ? argv[4] : "02.07.01.62";
    const std::string variant = argc > 5 ? argv[5] : "user-pass";
    const std::string mode = argc > 6 ? argv[6] : "step";
    uint32_t sequence = argc > 7 ? static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 0))
                                 : static_cast<uint32_t>(
                                       std::chrono::steady_clock::now().time_since_epoch().count());

    SSL_library_init();
    SSL_load_error_strings();

    int fd = connect_tcp(host, 6000);
    if (fd < 0) {
        std::cerr << "tcp connect failed\n";
        return 1;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_cipher_list(ctx, "HIGH:MEDIA:LOW:!DH");
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (mode.find("no-sni") == std::string::npos)
        SSL_set_tlsext_host_name(ssl, host);
    if (SSL_connect(ssl) != 1) {
        std::cerr << "SSL_connect failed: " << ERR_get_error() << "\n";
        return 1;
    }
    std::cout << "tls=" << SSL_get_version(ssl) << " cipher=" << SSL_get_cipher(ssl) << "\n";

    std::vector<uint8_t> auth(16, 0);
    if (variant == "pass-user") {
        std::memcpy(auth.data(), access, std::min<size_t>(8, std::strlen(access)));
        std::memcpy(auth.data() + 8, "bblp", 4);
    } else if (variant == "pass-pass") {
        std::memcpy(auth.data(), access, std::min<size_t>(8, std::strlen(access)));
        std::memcpy(auth.data() + 8, access, std::min<size_t>(8, std::strlen(access)));
    } else if (variant == "user-user") {
        std::memcpy(auth.data(), "bblp", 4);
        std::memcpy(auth.data() + 8, "bblp", 4);
    } else if (variant == "x86-empty-pass") {
        std::memcpy(auth.data() + 8, access, std::min<size_t>(8, std::strlen(access)));
    } else if (variant == "x86-default-pass") {
        std::memcpy(auth.data(), "888888", 6);
        std::memcpy(auth.data() + 8, access, std::min<size_t>(8, std::strlen(access)));
    } else {
        std::memcpy(auth.data(), "bblp", 4);
        std::memcpy(auth.data() + 8, access, std::min<size_t>(8, std::strlen(access)));
    }
    std::cout << "variant=" << variant << " mode=" << mode << " sequence0=" << sequence << "\n";
    if (mode.find("ctrl3000") != std::string::npos) {
        std::vector<uint8_t> ctrl(64, 0);
        auto put32 = [&ctrl](size_t offset, const char *value) {
            std::memcpy(ctrl.data() + offset, value, std::min<size_t>(32, std::strlen(value)));
        };
        if (variant == "pass-user") {
            put32(0, access);
            put32(32, "bblp");
        } else if (variant == "pass-pass") {
            put32(0, access);
            put32(32, access);
        } else if (variant == "user-user") {
            put32(0, "bblp");
            put32(32, "bblp");
        } else if (variant == "empty-pass") {
            put32(32, access);
        } else if (variant == "authkey-pass") {
            put32(0, access);
            put32(32, "888888");
        } else if (variant == "default-pass") {
            put32(0, "888888");
            put32(32, access);
        } else if (variant == "default-default") {
            put32(0, "888888");
            put32(32, "888888");
        } else if (variant == "x86-pass-empty") {
            put32(0, access);
        } else if (variant == "x86-pass-authkey") {
            put32(0, access);
            put32(32, "888888");
        } else {
            put32(0, "bblp");
            put32(32, access);
        }
        if (!write_raw_header_frame(ssl, 64, 0x00, 0x30, 0x00, 0x00, 0, ctrl)) {
            std::cerr << "ctrl3000 frame write failed\n";
            return 1;
        }
        const int max_frames = mode.find("long") != std::string::npos ? 60 : 8;
        for (int i = 0; i < max_frames; ++i) {
            FrameHeader header{};
            std::vector<uint8_t> payload;
            if (!read_frame(ssl, header, payload)) {
                std::cerr << "frame read timed out at " << i << "\n";
                continue;
            }
            dump_frame("ctrl3000_frame", header, payload);
            if (payload.size() > 0)
                return 0;
        }
        return 1;
    }

    if (!write_frame(ssl, 0x01, sequence, auth)) {
        std::cerr << "auth frame write failed\n";
        return 1;
    }
    ++sequence;

    std::string request =
        std::string("{\"sequence\":0,\"mtype\":12291,\"req\":{\"t_av\":0,\"mtype\":1,\"peer_t\":3,\"pid\":\"") +
        client_id + "\",\"ver\":\"" + client_ver + "\"}}\n\n";
    std::vector<uint8_t> request_payload(request.begin(), request.end());

    if (mode == "send-both") {
        if (!write_frame(ssl, 0x02, sequence, request_payload)) {
            std::cerr << "start frame write failed\n";
            return 1;
        }
        ++sequence;
    }

    FrameHeader header{};
    std::vector<uint8_t> payload;
    if (!read_frame(ssl, header, payload)) {
        std::cerr << "auth response read failed\n";
        return 1;
    }
    dump_frame("auth_response", header, payload);

    if (mode != "send-both") {
        if (!write_frame(ssl, 0x02, sequence, request_payload)) {
            std::cerr << "start frame write failed\n";
            return 1;
        }
        ++sequence;
    }
    if (!read_frame(ssl, header, payload)) {
        std::cerr << "start response read failed\n";
        return 1;
    }
    dump_frame("start_response", header, payload);

    for (int i = 0; i < 5; ++i) {
        if (!read_frame(ssl, header, payload)) {
            std::cerr << "media frame read failed at " << i << "\n";
            break;
        }
        dump_frame("media_frame", header, payload);
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return 0;
}
