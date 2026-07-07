#include <dlfcn.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using Bambu_Tunnel = void *;

struct BambuVideoFormat {
    int width;
    int height;
    int frame_rate;
};

struct BambuStreamFormat {
    BambuVideoFormat video;
};

struct Bambu_StreamInfo {
    int type;
    int sub_type;
    BambuStreamFormat format;
    int format_type;
    int max_frame_size;
    char reserved[128];
};

struct Bambu_Sample {
    int itrack;
    int size;
    int flags;
    int reserved;
    unsigned char *buffer;
    unsigned long decode_time;
};

using InitFn = int (*)();
using DeinitFn = int (*)();
using CreateFn = int (*)(Bambu_Tunnel *, const char *);
using OpenFn = int (*)(Bambu_Tunnel);
using StartStreamFn = int (*)(Bambu_Tunnel, bool);
using StartStreamExFn = int (*)(Bambu_Tunnel, int);
using StreamCountFn = int (*)(Bambu_Tunnel);
using StreamInfoFn = int (*)(Bambu_Tunnel, int, Bambu_StreamInfo *);
using ReadSampleFn = int (*)(Bambu_Tunnel, Bambu_Sample *);
using LastErrorFn = const char *(*)();
using CloseFn = void (*)(Bambu_Tunnel);
using DestroyFn = void (*)(Bambu_Tunnel);

template <typename T>
T load_symbol(void *lib, const char *name)
{
    auto *symbol = dlsym(lib, name);
    if (!symbol)
        std::cerr << "missing symbol: " << name << "\n";
    return reinterpret_cast<T>(symbol);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        std::cerr << "usage: official-live-probe <libBambuSource.so> <host> <access-code> [authkey]\n";
        return 2;
    }

    const char *lib_path = argv[1];
    const char *host = argv[2];
    const char *access = argv[3];
    const char *authkey = argc > 4 ? argv[4] : "";

    void *lib = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::cerr << "dlopen failed: " << dlerror() << "\n";
        return 1;
    }

    auto create = load_symbol<CreateFn>(lib, "Bambu_Create");
    auto init = load_symbol<InitFn>(lib, "Bambu_Init");
    auto deinit = load_symbol<DeinitFn>(lib, "Bambu_Deinit");
    auto open = load_symbol<OpenFn>(lib, "Bambu_Open");
    auto start_stream = load_symbol<StartStreamFn>(lib, "Bambu_StartStream");
    auto start_stream_ex = load_symbol<StartStreamExFn>(lib, "Bambu_StartStreamEx");
    auto stream_count = load_symbol<StreamCountFn>(lib, "Bambu_GetStreamCount");
    auto stream_info = load_symbol<StreamInfoFn>(lib, "Bambu_GetStreamInfo");
    auto read_sample = load_symbol<ReadSampleFn>(lib, "Bambu_ReadSample");
    auto last_error = load_symbol<LastErrorFn>(lib, "Bambu_GetLastErrorMsg");
    auto close = load_symbol<CloseFn>(lib, "Bambu_Close");
    auto destroy = load_symbol<DestroyFn>(lib, "Bambu_Destroy");
    if (!init || !deinit || !create || !open || !start_stream || !start_stream_ex || !stream_count || !stream_info ||
        !read_sample || !last_error || !close || !destroy) {
        return 1;
    }

    int init_rc = init();
    std::cout << "init_rc=" << init_rc << "\n";

    std::string url = std::string("bambu:///local/") + host +
                      ".?port=6000&user=bblp&passwd=" + access;
    if (*authkey)
        url += std::string("&authkey=") + authkey;

    Bambu_Tunnel tunnel = nullptr;
    int rc = create(&tunnel, url.c_str());
    std::cout << "create_rc=" << rc << " tunnel=" << (tunnel ? "yes" : "no")
              << " last_error=" << last_error() << "\n";
    if (rc != 0 || !tunnel) {
        deinit();
        return 1;
    }

    rc = open(tunnel);
    std::cout << "open_rc=" << rc << " last_error=" << last_error() << "\n";
    if (rc != 0) {
        destroy(tunnel);
        deinit();
        return 1;
    }

    bool started = false;
    for (int i = 0; i < 60; ++i) {
        rc = start_stream(tunnel, true);
        std::cout << "start_stream[" << i << "] rc=" << rc
                  << " last_error=" << last_error() << "\n";
        if (rc == 0) {
            started = true;
            break;
        }
        if (i >= 7) {
            Bambu_Sample sample{};
            std::vector<unsigned char> buffer(256 * 1024);
            sample.buffer = buffer.data();
            sample.size = static_cast<int>(buffer.size());
            int read_rc = read_sample(tunnel, &sample);
            std::cout << "prestart_read_sample[" << i << "] rc=" << read_rc
                      << " size=" << sample.size << " flags=" << sample.flags
                      << " decode_time=" << sample.decode_time
                      << " last_error=" << last_error() << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "started=" << (started ? "yes" : "no") << "\n";

    int count = stream_count(tunnel);
    std::cout << "stream_count=" << count << "\n";

    Bambu_StreamInfo info{};
    rc = stream_info(tunnel, 0, &info);
    std::cout << "stream_info_rc=" << rc << " type=" << info.type
              << " sub_type=" << info.sub_type << " width=" << info.format.video.width
              << " height=" << info.format.video.height
              << " fps=" << info.format.video.frame_rate
              << " format_type=" << info.format_type
              << " max_frame_size=" << info.max_frame_size
              << " last_error=" << last_error() << "\n";

    for (int i = 0; i < 80; ++i) {
        Bambu_Sample sample{};
        std::vector<unsigned char> buffer(256 * 1024);
        sample.buffer = buffer.data();
        sample.size = static_cast<int>(buffer.size());
        rc = read_sample(tunnel, &sample);
        std::cout << "read_sample[" << i << "] rc=" << rc
                  << " size=" << sample.size << " flags=" << sample.flags
                  << " decode_time=" << sample.decode_time
                  << " last_error=" << last_error() << "\n";
        if (rc == 0 && sample.size > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    close(tunnel);
    destroy(tunnel);
    deinit();
    return 0;
}
