#include <obs-module.h>
#include "motion.hpp"
#include "receiver.hpp"
#include "telemetry_monitor.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-racing-motion", "en-US")
MODULE_EXPORT const char *obs_module_description(void) {return "Direct racing telemetry with per-source 2D motion";}

namespace {
using namespace racing;
struct Filter {
    obs_source_t *source=nullptr;
    gs_effect_t *effect=nullptr;
    gs_eparam_t *input_size=nullptr,*output_size=nullptr,*offset=nullptr,*rotation_cs=nullptr,*border=nullptr,*scale=nullptr,*pivot_delta=nullptr;
    std::mutex mutex;
    std::shared_ptr<Receiver> receiver;
    MotionSettings settings;
    Motion motion;
    Pose pose;
    bool preview=false;
    unsigned padding=0;
    float pivot_x=0.5f,pivot_y=0.5f;
    double elapsed=0;
    std::shared_ptr<MonitorState> monitor_state=std::make_shared<MonitorState>();
    std::unique_ptr<TelemetryMonitor> monitor=std::make_unique<TelemetryMonitor>(monitor_state);
};
float number(obs_data_t *s,const char *key,float lo,float hi) {
    const double value=obs_data_get_double(s,key);
    return std::isfinite(value)?float(std::clamp(value,double(lo),double(hi))):lo;
}
const char *text(const char *key) {return obs_module_text(key);}

void update(void *data,obs_data_t *s) {
    auto *f=static_cast<Filter*>(data);
    MotionSettings settings;
    settings.intensity=number(s,"intensity",0,200)/100;
    settings.input_gain=number(s,"input_gain",1,20);
    settings.micro_sensitivity=number(s,"micro_sensitivity",0,100)/100;
    settings.x_limit=number(s,"x_limit",0,500);
    settings.y_limit=number(s,"y_limit",0,500);
    settings.rotation_limit=number(s,"rotation_limit",0,45);
    settings.surge_limit=number(s,"surge_limit",0,50)/100;
    settings.surge_y_limit=number(s,"surge_y_limit",0,500);
    settings.smoothing_ms=number(s,"smoothing_ms",0,1000);
    settings.return_ms=number(s,"return_ms",0,2000);
    settings.invert_x=obs_data_get_bool(s,"invert_x");
    settings.invert_y=obs_data_get_bool(s,"invert_y");
    settings.invert_rotation=obs_data_get_bool(s,"invert_rotation");
    settings.invert_surge=obs_data_get_bool(s,"invert_surge");
    InputConfig input;
    input.game=static_cast<Game>(std::clamp<int64_t>(obs_data_get_int(s,"game"),0,6));
    input.port=static_cast<uint16_t>(std::clamp<int64_t>(obs_data_get_int(s,"udp_port"),1,65535));
    input.lan=obs_data_get_bool(s,"lan");
    auto receiver=Receiver::acquire(input);
    std::shared_ptr<Receiver> previous;
    {
        std::lock_guard<std::mutex> guard(f->mutex);
        if(f->receiver!=receiver) f->motion=Motion{};
        previous=std::move(f->receiver);
        f->receiver=std::move(receiver);
        f->settings=settings;
        f->preview=obs_data_get_bool(s,"preview");
        f->padding=static_cast<unsigned>(std::clamp<int64_t>(obs_data_get_int(s,"padding"),0,500));
        f->pivot_x=number(s,"pivot_x",0,100)/100;
        f->pivot_y=number(s,"pivot_y",0,100)/100;
    }
    // Last-subscriber shutdown must not block the video tick's settings mutex.
    previous.reset();
}

void defaults(obs_data_t *s) {
    obs_data_set_default_int(s,"game",static_cast<int>(Game::ETS2));
    obs_data_set_default_int(s,"udp_port",5300);
    obs_data_set_default_double(s,"intensity",100);
    obs_data_set_default_double(s,"input_gain",1);
    obs_data_set_default_double(s,"micro_sensitivity",0);
    obs_data_set_default_double(s,"x_limit",24);
    obs_data_set_default_double(s,"y_limit",16);
    obs_data_set_default_double(s,"rotation_limit",3);
    obs_data_set_default_double(s,"pivot_x",50);
    obs_data_set_default_double(s,"pivot_y",50);
    obs_data_set_default_double(s,"surge_limit",5);
    obs_data_set_default_double(s,"surge_y_limit",12);
    obs_data_set_default_double(s,"smoothing_ms",100);
    obs_data_set_default_double(s,"return_ms",250);
    obs_data_set_default_int(s,"padding",0);
    obs_data_set_default_bool(s,"preview",false);
    obs_data_set_default_bool(s,"lan",false);
}

std::string status(Filter *f) {
    if(!f) return text("Waiting");
    std::shared_ptr<Receiver> receiver;
    bool preview;
    {
        std::lock_guard<std::mutex> guard(f->mutex);
        receiver=f->receiver;preview=f->preview;
    }
    auto s=receiver?receiver->snapshot():Snapshot{};
    char line[512];
    std::snprintf(line,sizeof(line),"%s | %s\nSurge %.2f G / Sway %.2f G / Heave %.2f G / Roll %.2f deg\n%.0f km/h / %.0f RPM / Gear %d | %llu packets",
        preview?text("PreviewActual"):(s.active?text("Receiving"):text("Waiting")),s.status.c_str(),
        s.values.surge,s.values.sway,s.values.heave,s.values.roll*57.29578f,s.values.speed,s.values.rpm,s.values.gear,
        static_cast<unsigned long long>(s.packets));
    return line;
}
bool refresh_status(obs_properties_t *props,obs_property_t *,void *data) {
    auto value=status(static_cast<Filter*>(data));
    obs_property_set_description(obs_properties_get(props,"receiver_status"),value.c_str());
    return true;
}
bool open_monitor(obs_properties_t *,obs_property_t *,void *data) {
    auto *f=static_cast<Filter*>(data);
    if(!f)return false;
    try {
        MonitorLabels labels{text("Receiving"),text("Waiting"),text("PreviewActive"),text("FilterDisabled"),
                             text("ActualInput"),text("SimulatedInput"),text("AppliedOutput")};
        f->monitor->open(std::string(text("MonitorTitle"))+" — "+obs_source_get_name(f->source),std::move(labels));
    } catch(const std::exception &error) {blog(LOG_ERROR,"[racing-motion] Telemetry monitor: %s",error.what());}
    return false; // A modeless monitor does not rebuild the property controls.
}
bool game_changed(obs_properties_t *props,obs_property_t *,obs_data_t *s) {
    const auto game=static_cast<Game>(obs_data_get_int(s,"game"));
    bool udp=game==Game::Forza||game==Game::FH6||game==Game::F1;
    obs_property_set_visible(obs_properties_get(props,"udp_port"),udp);
    obs_property_set_visible(obs_properties_get(props,"lan"),udp);
    obs_property_set_visible(obs_properties_get(props,"truck_help"),game==Game::ETS2||game==Game::ATS);
    return true;
}
obs_properties_t *properties(void *data) {
    auto *props=obs_properties_create();
    auto *games=obs_properties_add_list(props,"game",text("Game"),OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    for(const auto &item : {std::pair<const char*,Game>{"Euro Truck Simulator 2",Game::ETS2},
        {"American Truck Simulator",Game::ATS},{"Assetto Corsa",Game::AC},{"Assetto Corsa Competizione",Game::ACC},
        {"Forza Horizon / Motorsport (Sled / Dash)",Game::Forza},{"Forza Horizon 6 (existing bridge format)",Game::FH6},
        {"EA Sports F1 23 / 24 / 25",Game::F1}})
        obs_property_list_add_int(games,item.first,static_cast<int>(item.second));
    obs_property_set_modified_callback(games,game_changed);
    obs_properties_add_int(props,"udp_port",text("Port"),1,65535,1);
    obs_properties_add_bool(props,"lan",text("LAN"));
    obs_properties_add_text(props,"input_help",text("InputHelp"),OBS_TEXT_INFO);
    obs_properties_add_text(props,"truck_help",text("TruckHelp"),OBS_TEXT_INFO);
    auto current=status(static_cast<Filter*>(data));
    obs_properties_add_text(props,"receiver_status",current.c_str(),OBS_TEXT_INFO);
    obs_properties_add_button(props,"refresh_status",text("Refresh"),refresh_status);
    obs_properties_add_button(props,"live_monitor",text("LiveMonitor"),open_monitor);
    obs_properties_add_text(props,"live_help",text("LiveHelp"),OBS_TEXT_INFO);
    obs_properties_add_bool(props,"preview",text("Preview"));
    obs_properties_add_float_slider(props,"intensity",text("Intensity"),0,200,1);
    obs_properties_add_float_slider(props,"input_gain",text("InputGain"),1,20,0.1);
    obs_properties_add_text(props,"gain_help",text("InputGainHelp"),OBS_TEXT_INFO);
    obs_properties_add_float_slider(props,"micro_sensitivity",text("MicroSensitivity"),0,100,1);
    obs_properties_add_text(props,"micro_help",text("MicroHelp"),OBS_TEXT_INFO);
    obs_properties_add_float_slider(props,"x_limit",text("Horizontal"),0,500,1);
    obs_properties_add_float_slider(props,"y_limit",text("Vertical"),0,500,1);
    obs_properties_add_float_slider(props,"rotation_limit",text("Rotation"),0,45,0.1);
    obs_properties_add_float_slider(props,"pivot_x",text("PivotX"),0,100,0.5);
    obs_properties_add_float_slider(props,"pivot_y",text("PivotY"),0,100,0.5);
    obs_properties_add_text(props,"pivot_help",text("PivotHelp"),OBS_TEXT_INFO);
    obs_properties_add_float_slider(props,"surge_limit",text("SurgeScale"),0,50,0.5);
    obs_properties_add_float_slider(props,"surge_y_limit",text("SurgeVertical"),0,500,1);
    obs_properties_add_bool(props,"invert_surge",text("InvertSurge"));
    obs_properties_add_text(props,"surge_help",text("SurgeHelp"),OBS_TEXT_INFO);
    obs_properties_add_bool(props,"invert_x",text("InvertX"));
    obs_properties_add_bool(props,"invert_y",text("InvertY"));
    obs_properties_add_bool(props,"invert_rotation",text("InvertRotation"));
    obs_properties_add_float_slider(props,"smoothing_ms",text("Smoothing"),0,1000,5);
    obs_properties_add_float_slider(props,"return_ms",text("Return"),0,2000,10);
    obs_properties_add_int_slider(props,"padding",text("Padding"),0,500,1);
    obs_properties_add_text(props,"padding_help",text("PaddingHelp"),OBS_TEXT_INFO);
    return props;
}

void tick(void *data,float seconds) {
    auto *f=static_cast<Filter*>(data);
    std::lock_guard<std::mutex> guard(f->mutex);
    auto sample=f->receiver?f->receiver->snapshot():Snapshot{};
    const auto received=sample;
    f->elapsed+=std::clamp(seconds,0.f,0.25f);
    if(f->preview) {
        sample.values={};
        sample.values.surge=std::sin(f->elapsed*2.1)*0.5;
        sample.values.sway=std::sin(f->elapsed*2.8)*1.2;
        sample.values.heave=1+std::sin(f->elapsed*5.3)*0.6;
        sample.values.roll=std::sin(f->elapsed*1.7)*0.15;
        sample.active=true;
    }
    const bool enabled=obs_source_enabled(f->source);
    if(!enabled) {f->motion=Motion{};f->pose={};}
    else f->pose=f->motion.step(sample.values,sample.active && (f->preview||sample.age_seconds<0.5),seconds,f->settings);
    {
        std::lock_guard<std::mutex> monitor_guard(f->monitor_state->mutex);
        auto &frame=f->monitor_state->frame;
        frame.received=received;frame.preview_input=sample.values;frame.pose=f->pose;
        frame.preview=f->preview;frame.enabled=enabled;
    }
}
struct Dimensions {uint32_t w=0,h=0,pad=0;};
Dimensions dimensions(Filter *f) {
    auto *target=obs_filter_get_target(f->source);
    if(!target) return {};
    Dimensions d{obs_source_get_base_width(target),obs_source_get_base_height(target),0};
    if(!d.w||!d.h) return {};
    std::lock_guard<std::mutex> guard(f->mutex);
    const auto largest=std::max(d.w,d.h);
    d.pad=largest<16384?std::min(f->padding,(16384-largest)/2):0;
    return d;
}
uint32_t width(void *data) {auto d=dimensions(static_cast<Filter*>(data));return d.w+2*d.pad;}
uint32_t height(void *data) {auto d=dimensions(static_cast<Filter*>(data));return d.h+2*d.pad;}
void render(void *data,gs_effect_t *) {
    auto *f=static_cast<Filter*>(data);
    auto d=dimensions(f);
    if(!d.w||!d.h) {obs_source_skip_video_filter(f->source);return;}
    Pose pose;
    float pivot_x,pivot_y;
    {std::lock_guard<std::mutex> guard(f->mutex);pose=f->pose;pivot_x=f->pivot_x;pivot_y=f->pivot_y;}
    if(!obs_source_process_filter_begin(f->source,GS_RGBA,OBS_NO_DIRECT_RENDERING)) return;
    vec2 input,output,offset,rotation,pivot_delta;
    vec2_set(&input,float(d.w),float(d.h));
    vec2_set(&output,float(d.w+2*d.pad),float(d.h+2*d.pad));
    vec2_set(&offset,pose.x,pose.y);
    const float angle=pose.rotation*0.01745329252f;
    vec2_set(&rotation,std::cos(angle),std::sin(angle));
    vec2_set(&pivot_delta,(pivot_x-0.5f)*float(d.w),(pivot_y-0.5f)*float(d.h));
    gs_effect_set_vec2(f->input_size,&input);
    gs_effect_set_vec2(f->output_size,&output);
    gs_effect_set_vec2(f->offset,&offset);
    gs_effect_set_vec2(f->rotation_cs,&rotation);
    gs_effect_set_float(f->border,float(d.pad));
    gs_effect_set_float(f->scale,pose.scale);
    gs_effect_set_vec2(f->pivot_delta,&pivot_delta);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE,GS_BLEND_INVSRCALPHA);
    obs_source_process_filter_end(f->source,f->effect,d.w+2*d.pad,d.h+2*d.pad);
    gs_blend_state_pop();
}
void destroy(void *data) {
    auto *f=static_cast<Filter*>(data);
    f->monitor.reset();
    f->receiver.reset();
    obs_enter_graphics();
    gs_effect_destroy(f->effect);
    obs_leave_graphics();
    delete f;
}
void *create(obs_data_t *s,obs_source_t *source) {
    auto f=std::make_unique<Filter>();
    f->source=source;
    char *path=obs_module_file("motion.effect");
    if(!path) {blog(LOG_ERROR,"[racing-motion] Missing motion.effect");return nullptr;}
    obs_enter_graphics();
    f->effect=gs_effect_create_from_file(path,nullptr);
    obs_leave_graphics();
    bfree(path);
    if(!f->effect) {blog(LOG_ERROR,"[racing-motion] Shader compilation failed");return nullptr;}
    f->input_size=gs_effect_get_param_by_name(f->effect,"input_size");
    f->output_size=gs_effect_get_param_by_name(f->effect,"output_size");
    f->offset=gs_effect_get_param_by_name(f->effect,"offset");
    f->rotation_cs=gs_effect_get_param_by_name(f->effect,"rotation_cs");
    f->border=gs_effect_get_param_by_name(f->effect,"padding");
    f->scale=gs_effect_get_param_by_name(f->effect,"motion_scale");
    f->pivot_delta=gs_effect_get_param_by_name(f->effect,"pivot_delta");
    update(f.get(),s);
    return f.release();
}
const char *name(void *) {return text("FilterName");}
}

bool obs_module_load(void) {
    obs_source_info info{};
    info.id="racing_motion_filter";
    info.type=OBS_SOURCE_TYPE_FILTER;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_SRGB;
    info.get_name=name;info.create=create;info.destroy=destroy;
    info.get_defaults=defaults;info.get_properties=properties;info.update=update;
    info.video_tick=tick;info.video_render=render;info.get_width=width;info.get_height=height;
    obs_register_source(&info);
    blog(LOG_INFO,"[racing-motion] Direct game telemetry filter loaded (0.6.0)");
    return true;
}
