#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <future>

namespace pix {
// A window-owning caller cannot block on future.get(): window creation and COM
// on the JUCE thread can synchronously send messages back to the caller.
template<class T> T waitForWindowFuture(std::future<T>& future,bool modal=false) {
    bool quit=false;int quitCode=0;
    while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
        MsgWaitForMultipleObjectsEx(0,nullptr,10,modal?QS_ALLINPUT:QS_SENDMESSAGE,MWMO_INPUTAVAILABLE);
        MSG msg{};
        if(modal) {
            // Bound each batch so a busy queue cannot starve completion.
            for(int i=0;i<64 && PeekMessageW(&msg,nullptr,0,0,PM_REMOVE);++i) {
                if(msg.message==WM_QUIT){quit=true;quitCode=int(msg.wParam);continue;}
                TranslateMessage(&msg);DispatchMessageW(&msg);
            }
        } else {
            // PeekMessage delivers sent messages internally. Leave queued host
            // input/timers alone during ordinary synchronous render operations.
            PeekMessageW(&msg,nullptr,0,0,PM_NOREMOVE|PM_QS_SENDMESSAGE);
        }
    }
    if(quit)PostQuitMessage(quitCode);
    return future.get();
}
class ModalOwner {
    HWND window=nullptr;
    bool restore=false;
public:
    explicit ModalOwner(void* owner):window(static_cast<HWND>(owner)) {
        if(window && IsWindow(window) && GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId()) {
            restore=IsWindowEnabled(window)!=FALSE;
            if(restore)EnableWindow(window,FALSE);
        }
    }
    ~ModalOwner(){if(restore && IsWindow(window)){EnableWindow(window,TRUE);SetActiveWindow(window);}}
};
}
