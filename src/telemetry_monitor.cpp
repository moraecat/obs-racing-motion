#include "telemetry_monitor.hpp"
#include <windows.h>
#include <obs-module.h>
#include <atomic>
#include <thread>
#include <utility>

namespace racing {
namespace {
std::wstring wide(const std::string &value) {
    const int size=MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),nullptr,0);
    std::wstring result(size,L'\0');
    if(size) MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),result.data(),size);
    return result;
}
struct WindowData {
    std::shared_ptr<MonitorState> state;
    MonitorLabels labels;
    std::atomic<HWND> *handle;
    HWND body=nullptr;
    HFONT font=nullptr;
    std::string displayed;
};
void refresh(HWND window,WindowData &data) {
    const auto frame=data.state->snapshot();
    if(frame.closed) {DestroyWindow(window);return;}
    const auto display=format_monitor(frame,data.labels);
    if(display!=data.displayed) {
        data.displayed=display;
        SetWindowTextW(data.body,wide(display).c_str());
    }
}
void layout(HWND window,WindowData &data) {
    const UINT dpi=GetDpiForWindow(window);
    const int margin=MulDiv(16,int(dpi),96);
    RECT area{};GetClientRect(window,&area);
    MoveWindow(data.body,margin,margin,area.right-margin*2,area.bottom-margin*2,TRUE);
    HFONT font=CreateFontW(-MulDiv(15,int(dpi),96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
                          DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(font) {SendMessageW(data.body,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);if(data.font)DeleteObject(data.font);data.font=font;}
}
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto *data=reinterpret_cast<WindowData*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        data=static_cast<WindowData*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(data));
    }
    if(!data)return DefWindowProcW(window,message,wparam,lparam);
    switch(message) {
    case WM_CREATE:
        data->body=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,0,0,0,0,window,nullptr,nullptr,nullptr);
        if(!data->body)return -1;
        layout(window,*data);
        if(!SetTimer(window,1,200,nullptr))return -1;
        return 0;
    case WM_TIMER: refresh(window,*data);return 0;
    case WM_SIZE: if(data->body)layout(window,*data);return 0;
    case WM_DPICHANGED: {
        const auto *rect=reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(window);return 0;
    case WM_DESTROY:
        KillTimer(window,1);data->handle->store(nullptr);
        if(data->font){DeleteObject(data->font);data->font=nullptr;}
        PostQuitMessage(0);return 0;
    default:return DefWindowProcW(window,message,wparam,lparam);
    }
}
}
struct TelemetryMonitor::Impl {
    std::shared_ptr<MonitorState> state;
    std::atomic<HWND> handle{nullptr};
    std::atomic<bool> running{false};
    std::thread worker;
    explicit Impl(std::shared_ptr<MonitorState> s):state(std::move(s)){}
    ~Impl() {
        {std::lock_guard<std::mutex> guard(state->mutex);state->frame.closed=true;}
        if(HWND window=handle.load())PostMessageW(window,WM_CLOSE,0,0);
        if(worker.joinable())worker.join();
    }
    void run(std::string title,MonitorLabels labels) {
        HINSTANCE instance=nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&window_proc),&instance);
        const std::wstring class_name=L"OBSRacingMotionTelemetry_"+std::to_wstring(reinterpret_cast<uintptr_t>(this));
        WNDCLASSW type{};type.lpfnWndProc=window_proc;type.hInstance=instance;
        type.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));type.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);type.lpszClassName=class_name.c_str();
        WindowData data{state,std::move(labels),&handle,nullptr,nullptr,{}};
        if(RegisterClassW(&type)) {
            HWND window=CreateWindowExW(WS_EX_APPWINDOW,class_name.c_str(),wide(title).c_str(),WS_OVERLAPPEDWINDOW,
                                        CW_USEDEFAULT,CW_USEDEFAULT,650,480,nullptr,nullptr,instance,&data);
            if(window) {
                blog(LOG_INFO,"[racing-motion] Live telemetry window created");
                handle.store(window);
                if(state->snapshot().closed)DestroyWindow(window);
                else {
                    refresh(window,data);
                    ShowWindow(window,SW_SHOWNORMAL);
                    MSG message{};
                    while(GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
                }
            } else blog(LOG_ERROR,"[racing-motion] Live telemetry window creation failed: %lu",GetLastError());
            UnregisterClassW(class_name.c_str(),instance);
        } else blog(LOG_ERROR,"[racing-motion] Live telemetry window class registration failed: %lu",GetLastError());
        handle.store(nullptr);running=false;
    }
};
TelemetryMonitor::TelemetryMonitor(std::shared_ptr<MonitorState> state):impl_(std::make_unique<Impl>(std::move(state))){}
TelemetryMonitor::~TelemetryMonitor()=default;
void TelemetryMonitor::open(std::string title,MonitorLabels labels) {
    if(impl_->running) {
        if(HWND window=impl_->handle.load()){ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);}
        return;
    }
    if(impl_->worker.joinable())impl_->worker.join();
    impl_->running=true;
    try {impl_->worker=std::thread([impl=impl_.get(),title=std::move(title),labels=std::move(labels)]()mutable{impl->run(std::move(title),std::move(labels));});}
    catch(...) {impl_->running=false;throw;}
}
}
