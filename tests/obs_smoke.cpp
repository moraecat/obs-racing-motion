// Real libobs/D3D11 integration test. No frontend, devices, outputs or user config.
#include <winsock2.h>
#include <obs.h>
#include <graphics/vec4.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t W = 128, H = 96;
void require(bool value, const std::string &message)
{
    if (!value) throw std::runtime_error(message);
}

struct Watchdog {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    std::thread thread;
    explicit Watchdog(int seconds=45) : thread([this,seconds] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(seconds), [this] { return done; })) {
            std::fprintf(stderr, "FAIL: native OBS test exceeded watchdog deadline\n");
            std::fflush(stderr);
            std::_Exit(124);
        }
    }) {}
    ~Watchdog()
    {
        { std::lock_guard<std::mutex> lock(mutex); done = true; }
        condition.notify_one();
        thread.join();
    }
};

struct Pattern { gs_texture_t *texture = nullptr; };
void *pattern_create(obs_data_t *, obs_source_t *)
{
    auto *pattern = new Pattern;
    std::vector<uint8_t> pixels(W * H * 4, 0);
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
        auto *pixel = &pixels[(y * W + x) * 4];
        if (x >= 28 && x < 100 && y >= 20 && y < 76) {
            pixel[0] = 220; pixel[1] = 52; pixel[2] = 34; pixel[3] = 255;
            if (x >= 40 && x < 56 && y >= 30 && y < 62) {
                pixel[0] = 30; pixel[1] = 100; pixel[2] = 245;
            }
        }
        // Opaque pixels touch the right source boundary: a clamped sampler
        // incorrectly repeats this stripe throughout the added transparent pad.
        if (x >= W - 3 && y >= 40 && y < 56) {
            pixel[0] = 32; pixel[1] = 230; pixel[2] = 75; pixel[3] = 255;
        }
    }
    const uint8_t *data = pixels.data();
    obs_enter_graphics();
    pattern->texture = gs_texture_create(W, H, GS_RGBA, 1, &data, 0);
    obs_leave_graphics();
    if (!pattern->texture) { delete pattern; return nullptr; }
    return pattern;
}
void pattern_destroy(void *data)
{
    auto *pattern = static_cast<Pattern *>(data);
    obs_enter_graphics();
    gs_texture_destroy(pattern->texture);
    obs_leave_graphics();
    delete pattern;
}
void pattern_render(void *data, gs_effect_t *)
{
    auto *pattern = static_cast<Pattern *>(data);
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), pattern->texture);
    while (gs_effect_loop(effect, "Draw")) gs_draw_sprite(pattern->texture, 0, W, H);
}

struct Frame {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
    const uint8_t *at(uint32_t x, uint32_t y) const { return &pixels[(y * width + x) * 4]; }
};
struct Capture { obs_source_t *source; Frame frame; std::string error; };
void capture_task(void *data)
{
    auto &capture = *static_cast<Capture *>(data);
    auto &frame = capture.frame;
    obs_enter_graphics();
    frame.width = obs_source_get_width(capture.source);
    frame.height = obs_source_get_height(capture.source);
    gs_texrender_t *target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    gs_stagesurf_t *stage = gs_stagesurface_create(frame.width, frame.height, GS_RGBA);
    if (!target || !stage || !gs_texrender_begin(target, frame.width, frame.height)) {
        capture.error = "offscreen render target creation failed";
    } else {
        vec4 transparent{};
        gs_clear(GS_CLEAR_COLOR, &transparent, 0.0f, 0);
        gs_ortho(0, float(frame.width), 0, float(frame.height), -100, 100);
        gs_matrix_push();
        gs_matrix_identity();
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        obs_source_video_render(capture.source);
        gs_blend_state_pop();
        gs_matrix_pop();
        gs_texrender_end(target);
        gs_stage_texture(stage, gs_texrender_get_texture(target));
        uint8_t *mapped = nullptr;
        uint32_t stride = 0;
        if (gs_stagesurface_map(stage, &mapped, &stride)) {
            frame.pixels.resize(frame.width * frame.height * 4);
            for (uint32_t y = 0; y < frame.height; ++y)
                std::memcpy(&frame.pixels[y * frame.width * 4], mapped + y * stride, frame.width * 4);
            gs_stagesurface_unmap(stage);
        } else capture.error = "D3D11 staging texture readback failed";
    }
    gs_stagesurface_destroy(stage);
    gs_texrender_destroy(target);
    obs_leave_graphics();
}
Frame capture(obs_source_t *source)
{
    Capture request{source, {}, {}};
    obs_queue_task(OBS_TASK_GRAPHICS, capture_task, &request, true);
    require(request.error.empty(), request.error);
    require(!request.frame.pixels.empty(), "render returned no pixels");
    return std::move(request.frame);
}

size_t changed_pixels(const Frame &a, const Frame &b, int tolerance = 0)
{
    require(a.width == b.width && a.height == b.height, "pixel comparison dimensions differ");
    size_t count = 0;
    for (size_t i = 0; i < a.pixels.size(); i += 4) {
        bool different = false;
        for (size_t c = 0; c < 4; ++c)
            different |= std::abs(int(a.pixels[i+c]) - int(b.pixels[i+c])) > tolerance;
        count += different;
    }
    return count;
}
double red_center_x(const Frame &frame)
{
    double weighted = 0, weight = 0;
    for (uint32_t y = 0; y < frame.height; ++y) for (uint32_t x = 0; x < frame.width; ++x) {
        const auto *p = frame.at(x,y);
        if (p[0] > p[1] * 2 && p[0] > p[2] * 2 && p[3] > 200) { weighted += x; ++weight; }
    }
    require(weight > 100, "test pattern red rectangle is missing");
    return weighted / weight;
}
int red_width(const Frame &frame)
{
    int left=int(frame.width),right=-1;
    for(uint32_t y=0;y<frame.height;++y) for(uint32_t x=0;x<frame.width;++x) {
        const auto *p=frame.at(x,y);
        if(p[0]>p[1]*2 && p[0]>p[2]*2 && p[3]>200) {
            left=std::min(left,int(x));right=std::max(right,int(x));
        }
    }
    require(right>=left,"scaled test pattern missing");
    require(std::abs((left+right)*0.5-63.5)<1,"surge scaling must keep source center fixed");
    return right-left+1;
}
double translation_center_x(const Frame &frame) {
    // This row crosses only the opaque rectangle. Include fractional edge
    // alpha so subpixel shifts are not rounded by a binary color threshold.
    double sum=0,weight=0;
    for(uint32_t x=0;x<frame.width;++x) {
        const double alpha=frame.at(x,24)[3];sum+=x*alpha;weight+=alpha;
    }
    require(weight>100,"translation measurement row is empty");
    return sum/weight;
}
double red_center_y(const Frame &frame)
{
    double weighted=0,weight=0;
    for(uint32_t y=0;y<frame.height;++y) for(uint32_t x=0;x<frame.width;++x) {
        const auto *p=frame.at(x,y);
        if(p[0]>p[1]*2 && p[0]>p[2]*2 && p[3]>200) {weighted+=y;++weight;}
    }
    require(weight>100,"vertical surge pattern missing");
    return weighted/weight;
}

// Store a checkerboard-composited BMP beside the smoke executable for visual QA.
void save_bmp(const Frame &frame, const std::filesystem::path &path)
{
    const uint32_t stride = (frame.width * 3 + 3) & ~3u;
    const uint32_t bytes = stride * frame.height;
    std::ofstream stream(path, std::ios::binary);
    auto u16 = [&](uint16_t v) { stream.put(char(v)); stream.put(char(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); };
    stream.write("BM", 2); u32(54 + bytes); u32(0); u32(54); u32(40);
    u32(frame.width); u32(frame.height); u16(1); u16(24); u32(0); u32(bytes);
    u32(2835); u32(2835); u32(0); u32(0);
    for (uint32_t row = frame.height; row > 0; --row) {
        const uint32_t y = row - 1;
        for (uint32_t x = 0; x < frame.width; ++x) {
            const auto *p = frame.at(x, y);
            const unsigned bg = ((x / 8 + y / 8) % 2) ? 60 : 95;
            for (int c = 2; c >= 0; --c)
                stream.put(char(std::min(255u, unsigned(p[c]) + bg * (255 - p[3]) / 255)));
        }
        for (uint32_t i = frame.width * 3; i < stride; ++i) stream.put(0);
    }
    require(bool(stream), "could not write visual QA image");
}

struct Session {
    obs_source_t *source = nullptr;
    obs_source_t *filter = nullptr;
    ~Session() { shutdown(); }
    void shutdown()
    {
        if (source && filter) obs_source_filter_remove(source, filter);
        if (filter) obs_source_release(filter);
        if (source) obs_source_release(source);
        source = filter = nullptr;
        if (obs_initialized()) { obs_wait_for_destroy_queue(); obs_shutdown(); }
    }
};
struct WorkingDirectory {
    std::filesystem::path original = std::filesystem::current_path();
    ~WorkingDirectory() { std::error_code ignored; std::filesystem::current_path(original, ignored); }
};
void update(obs_source_t *filter, bool preview, double intensity, double x, double y, double rotation, int padding, double surge=0, double surge_y=0)
{
    obs_data_t *settings = obs_data_create();
    obs_data_set_bool(settings, "preview", preview);
    obs_data_set_double(settings, "intensity", intensity);
    obs_data_set_double(settings, "x_limit", x);
    obs_data_set_double(settings, "y_limit", y);
    obs_data_set_double(settings, "rotation_limit", rotation);
    obs_data_set_double(settings, "surge_limit", surge);
    obs_data_set_double(settings, "surge_y_limit", surge_y);
    obs_data_set_int(settings, "padding", padding);
    obs_source_update(filter, settings);
    obs_data_release(settings);
}
void settle(int ms = 550) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A constant packet gives the real receiver/filter a stable 7-degree rotation.
// Bind an ephemeral loopback port first instead of using a user's game port.
struct ConstantTelemetry {
    SOCKET socket=INVALID_SOCKET;
    sockaddr_in destination{};
    std::atomic<bool> stopped{false};
    std::thread worker;
    ConstantTelemetry() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2,2),&data)==0,"test Winsock initialization failed");
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        require(socket!=INVALID_SOCKET,"test UDP socket creation failed");
        destination.sin_family=AF_INET;destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(socket,reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==0,"test UDP bind failed");
        int size=sizeof(destination);
        require(getsockname(socket,reinterpret_cast<sockaddr*>(&destination),&size)==0,"test UDP port discovery failed");
        closesocket(socket);
        socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        require(socket!=INVALID_SOCKET,"test UDP sender creation failed");
    }
    void start(float lateral_g=1.5f) {
        worker=std::thread([this,lateral_g] {
            char packet[232]{};
            int active=1;float lateral=lateral_g*9.81f;
            std::memcpy(packet,&active,sizeof(active));
            std::memcpy(packet+20,&lateral,sizeof(lateral));
            while(!stopped) {
                sendto(socket,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    ~ConstantTelemetry() {
        stopped=true;if(worker.joinable())worker.join();
        if(socket!=INVALID_SOCKET)closesocket(socket);WSACleanup();
    }
};
void pivot(obs_source_t *filter,double x,double y) {
    obs_data_t *settings=obs_data_create();
    obs_data_set_double(settings,"pivot_x",x);obs_data_set_double(settings,"pivot_y",y);
    obs_source_update(filter,settings);obs_data_release(settings);
    settle(100);
}
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "Usage: obs_smoke <plugin.dll> <plugin-data-directory> <OBS-bin-directory> [live-ets2-seconds]\n");
        return 2;
    }
    const int live_seconds=argc==5?std::clamp(std::atoi(argv[4]),1,600):0;
    const bool show_monitor=std::getenv("RACING_SMOKE_MONITOR")!=nullptr;
    Watchdog watchdog(live_seconds+45+(show_monitor?150:0));
    WorkingDirectory directory;
    Session session;
    try {
        const auto artifacts = std::filesystem::absolute(argv[0]).parent_path();
        const auto plugin = std::filesystem::absolute(argv[1]).string();
        const auto plugin_data = std::filesystem::absolute(argv[2]).string();
        const auto bin = std::filesystem::absolute(argv[3]);
        // Windows libobs locates built-in effects relative to its bin directory.
        // A process-only cwd change avoids the deprecated global data-path registry.
        std::filesystem::current_path(bin);
        const long initial_allocations = bnum_allocs();
        require(obs_startup(show_monitor?"ko-KR":"en-US", nullptr, nullptr), "obs_startup failed");
        const std::string graphics = (bin / "libobs-d3d11.dll").string();
        obs_video_info video{};
        video.graphics_module = graphics.c_str();
        video.fps_num = 60; video.fps_den = 1;
        video.base_width = video.output_width = 320;
        video.base_height = video.output_height = 240;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709;
        video.range = VIDEO_RANGE_FULL;
        video.scale_type = OBS_SCALE_BILINEAR;
        const int reset = obs_reset_video(&video);
        require(reset == OBS_VIDEO_SUCCESS, "D3D11 initialization failed: " + std::to_string(reset));
        obs_module_t *module = nullptr;
        const int opened = obs_open_module(&module, plugin.c_str(), plugin_data.c_str());
        require(opened == MODULE_SUCCESS, "plugin module could not be opened: " + std::to_string(opened));
        require(obs_init_module(module), "plugin module initialization failed");
        require(obs_source_get_display_name("racing_motion_filter") != nullptr, "racing_motion_filter is not registered");

        obs_source_info info{};
        info.id = "racing_motion_smoke_pattern";
        info.type = OBS_SOURCE_TYPE_INPUT;
        info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
        info.get_name = [](void *) { return "Smoke test RGBA pattern"; };
        info.create = pattern_create;
        info.destroy = pattern_destroy;
        info.get_width = [](void *) { return W; };
        info.get_height = [](void *) { return H; };
        info.video_render = pattern_render;
        obs_register_source(&info);
        session.source = obs_source_create_private(info.id, "Private smoke pattern", nullptr);
        require(session.source != nullptr, "pattern source creation failed");
        const Frame reference = capture(session.source);
        require(reference.at(30, 25)[3] == 255 && reference.at(0, 0)[3] == 0,
                "reference texture must contain opaque and transparent pixels");

        obs_data_t *settings = obs_get_source_defaults("racing_motion_filter");
        require(settings != nullptr, "plugin defaults unavailable");
        obs_data_set_int(settings, "game", live_seconds?5:0);
        obs_data_set_bool(settings, "preview", !live_seconds);
        obs_data_set_double(settings, "intensity", 0);
        obs_data_set_int(settings, "smoothing_ms", 100);
        obs_data_set_int(settings, "return_ms", 250);
        obs_data_set_int(settings, "padding", 0);
        session.filter = obs_source_create_private("racing_motion_filter", "Private motion filter", settings);
        obs_data_release(settings);
        require(session.filter != nullptr, "filter creation failed");
        require(obs_source_get_type(session.filter) == OBS_SOURCE_TYPE_FILTER, "registered source is not a filter");
        obs_source_filter_add(session.source, session.filter);
        settle();
        const Frame neutral = capture(session.source);
        require(neutral.width == W && neutral.height == H, "padding=0 changed source dimensions");
        require(changed_pixels(reference, neutral) == 0, "zero intensity is not pixel-exact neutral");
        std::puts("PASS: DLL load, registration, D3D11 RGBA render, zero-intensity pixel equivalence");

        if(live_seconds) {
            update(session.filter,false,100,24,16,3,0,5,12);
            size_t maximum_changes=0;double maximum_x=0;
            std::ofstream report(artifacts/"live-ets2-render.csv");
            report<<"seconds,changed_pixels,horizontal_displacement\n";
            for(int i=0;i<live_seconds*4;++i) {
                settle(250);
                Frame frame=capture(session.source);
                const size_t changes=changed_pixels(reference,frame,2);
                const double displacement=std::abs(red_center_x(frame)-red_center_x(reference));
                report<<i/4.0<<','<<changes<<','<<displacement<<'\n';report.flush();
                if(changes>maximum_changes) {
                    maximum_changes=changes;
                    save_bmp(frame,artifacts/"live-ets2-largest-motion.bmp");
                }
                maximum_x=std::max(maximum_x,displacement);
                if(i%4==0) {std::printf("LIVE %ds changed=%zu displacement=%.3fpx preview=OFF\n",i/4,changes,displacement);std::fflush(stdout);}
            }
            save_bmp(reference,artifacts/"live-ets2-neutral.bmp");
            std::printf("LIVE SUMMARY maximum_changed_pixels=%zu max_horizontal=%.3fpx\n",maximum_changes,maximum_x);
            session.shutdown();
            require(bnum_allocs()==initial_allocations,"live OBS test leaked allocations");
            require(maximum_changes>20,"no visible motion from real ETS2 telemetry; preview remained disabled");
            return 0;
        }

        update(session.filter, true, 100, 24, 16, 0, 0);
        Frame translated;
        double displacement = 0;
        for (int i = 0; i < 8 && displacement < 1.5; ++i) {
            settle(250);
            translated = capture(session.source);
            displacement = std::abs(red_center_x(translated) - red_center_x(reference));
        }
        require(displacement >= 1.5 && changed_pixels(reference, translated, 2) > 100,
                "preview does not translate the rendered pixels");
        std::printf("PASS: preview translation moves red centroid by %.2f px\n", displacement);

        // Zero intensity settles any previous translation before rotation-only.
        update(session.filter, true, 0, 0, 0, 3, 0);
        settle(900);
        update(session.filter, true, 100, 0, 0, 3, 0);
        Frame rotated;
        size_t rotation_changes = 0;
        for (int i = 0; i < 8 && rotation_changes < 70; ++i) {
            settle(250);
            rotated = capture(session.source);
            rotation_changes = changed_pixels(reference, rotated, 2);
        }
        require(rotation_changes >= 70, "rotation-only preview does not alter rendered pixels");
        std::printf("PASS: rotation-only preview changes %zu pixels\n", rotation_changes);

        update(session.filter,true,0,0,0,0,0,20);
        settle(300);
        update(session.filter,true,100,0,0,0,0,20);
        int smallest=72,largest=72;
        Frame scaled_small,scaled_large;
        for(int i=0;i<20;++i) {
            settle(200);
            const Frame scaled=capture(session.source);
            require(scaled.width==W && scaled.height==H,"surge must preserve source dimensions");
            const int width=red_width(scaled);
            if(width<smallest) {smallest=width;scaled_small=scaled;}
            if(width>largest) {largest=width;scaled_large=scaled;}
        }
        require(smallest<68 && largest>76,"surge-only preview must shrink and enlarge the source");
        save_bmp(scaled_small,artifacts/"obs-smoke-surge-away.bmp");
        save_bmp(scaled_large,artifacts/"obs-smoke-surge-toward.bmp");
        std::printf("PASS: surge-only rendered width %d..%d pixels, centered and fixed canvas\n",smallest,largest);
        update(session.filter,true,100,0,0,0,0,0);
        settle(300);
        require(changed_pixels(reference,capture(session.source))==0,"zero surge limit must disable depth effect");

        update(session.filter,true,100,0,0,0,0,0,12);
        double lowest=0,highest=0;
        for(int i=0;i<20;++i) {
            settle(200);
            const Frame shifted=capture(session.source);
            require(red_width(shifted)==72,"vertical-only surge must not change scale or horizontal position");
            const double dy=red_center_y(shifted)-red_center_y(reference);
            lowest=std::min(lowest,dy);highest=std::max(highest,dy);
        }
        require(lowest<-6 && highest>6,"surge-only vertical cue must move up and down");
        std::printf("PASS: surge-only vertical movement %.2f..%.2f px without zoom\n",lowest,highest);

        update(session.filter, true, 0, 24, 16, 3, 16);
        settle(1000);
        const Frame padded = capture(session.source);
        require(padded.width == W + 32 && padded.height == H + 32, "padding=16 must add 32 pixels to both dimensions");
        for (uint32_t y = 0; y < padded.height; ++y) for (uint32_t x = 0; x < padded.width; ++x) {
            if (x < 16 || x >= W + 16 || y < 16 || y >= H + 16)
                require(padded.at(x,y)[3] == 0, "padding has alpha streaks outside source bounds");
            else require(std::memcmp(padded.at(x,y), reference.at(x-16,y-16), 4) == 0,
                         "neutral padded pixels do not preserve centered source content");
        }
        std::puts("PASS: padded dimensions, centered content, transparent edges without clamped streaks");

        save_bmp(neutral, artifacts / "obs-smoke-neutral.bmp");
        save_bmp(translated, artifacts / "obs-smoke-translation.bmp");
        save_bmp(rotated, artifacts / "obs-smoke-rotation.bmp");
        save_bmp(padded, artifacts / "obs-smoke-padding.bmp");
        {
            ConstantTelemetry telemetry;
            update(session.filter,false,100,0,0,10,0);
            obs_data_t *input=obs_data_create();
            obs_data_set_int(input,"game",2);
            obs_data_set_int(input,"udp_port",ntohs(telemetry.destination.sin_port));
            obs_data_set_double(input,"smoothing_ms",0);
            obs_source_update(session.filter,input);obs_data_release(input);
            telemetry.start();
            pivot(session.filter,50,50);
            settle(300);
            const Frame centered=capture(session.source);
            require(changed_pixels(reference,centered,2)>70,"constant telemetry must produce rotation");
            pivot(session.filter,50,100);
            const Frame bottom=capture(session.source);
            // At 7 degrees, moving the pivot down by 48px shifts the image
            // by (48*sin(7deg),48*(1-cos(7deg))) = (5.85,0.36).
            require(std::abs(red_center_x(bottom)-red_center_x(centered)-5.85)<0.4 &&
                    std::abs(red_center_y(bottom)-red_center_y(centered)-0.36)<0.4,
                    "bottom pivot must rotate around lower source edge");
            pivot(session.filter,100,50);
            const Frame right=capture(session.source);
            require(std::abs(red_center_x(right)-red_center_x(centered)-0.48)<0.4 &&
                    std::abs(red_center_y(right)-red_center_y(centered)+7.80)<0.4,
                    "right pivot must rotate around right source edge");
            pivot(session.filter,12.3,87.6);
            update(session.filter,false,0,0,0,10,0);
            settle(150);
            require(changed_pixels(reference,capture(session.source))==0,"arbitrary pivot at zero intensity must preserve exact neutral pixels");
            save_bmp(bottom,artifacts/"obs-smoke-pivot-bottom.bmp");
            save_bmp(right,artifacts/"obs-smoke-pivot-right.bmp");
            std::puts("PASS: rotation pivot X/Y changes with constant telemetry and exact neutral at arbitrary pivot");
            if(show_monitor) {
                update(session.filter,true,100,24,16,3,0,5,12);
                auto *props=obs_source_properties(session.filter);
                auto *button=obs_properties_get(props,"live_monitor");
                require(button!=nullptr,"live telemetry button is missing");
                require(!obs_property_button_clicked(button,session.filter),"live monitor must not rebuild setting controls");
                obs_properties_destroy(props);
                std::puts("MONITOR: visible for 120 seconds with synthetic UDP input and labeled preview; filter destruction must close it");
                std::fflush(stdout);
                settle(120000);
            }
        }
        {
            ConstantTelemetry telemetry;
            update(session.filter,false,100,24,0,0,0);
            auto *input=obs_data_create();
            obs_data_set_int(input,"game",2);
            obs_data_set_int(input,"udp_port",ntohs(telemetry.destination.sin_port));
            obs_data_set_double(input,"input_gain",1);
            obs_source_update(session.filter,input);
            telemetry.start(0.03f);settle(300);
            const double normal_x=translation_center_x(capture(session.source))-translation_center_x(reference);
            obs_data_set_double(input,"input_gain",10);
            obs_source_update(session.filter,input);settle(150);
            const double boosted_x=translation_center_x(capture(session.source))-translation_center_x(reference);
            require(std::abs(normal_x-0.48)<0.35 && std::abs(boosted_x-4.8)<0.35,
                    "OBS input gain must amplify small real UDP input from 0.48px to 4.8px");
            obs_data_set_double(input,"input_gain",1);
            obs_data_set_double(input,"micro_sensitivity",100);
            obs_source_update(session.filter,input);settle(150);
            const double micro_x=translation_center_x(capture(session.source))-translation_center_x(reference);
            require(std::abs(micro_x-4.0678)<0.15,
                    "OBS micro sensitivity must emphasize small UDP input without raising limits");
            obs_data_set_double(input,"intensity",0);
            obs_source_update(session.filter,input);obs_data_release(input);settle(100);
            require(changed_pixels(reference,capture(session.source))==0,"boosted filter must preserve exact neutral at zero intensity");
            std::printf("PASS: 0.03 G real UDP input amplified from %.2f to %.2f px at 10x, with exact zero-intensity neutral\n",normal_x,boosted_x);
            std::printf("PASS: 0.03 G input at 1x with 100%% micro sensitivity moves %.2f px\n",micro_x);
        }
        session.shutdown();
        const long remaining = bnum_allocs() - initial_allocations;
        require(remaining == 0, "libobs allocations remain after shutdown: " + std::to_string(remaining));
        std::puts("PASS: clean source/filter destruction and zero remaining libobs allocations");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
