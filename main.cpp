#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cwchar>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "dxva2.lib")

static constexpr UINT WM_TRAY = WM_APP + 1;
static constexpr UINT ID_SETTINGS = 1001, ID_EXIT = 1002, ID_ENABLE = 1003;
static constexpr int IDC_DELAY = 2001, IDC_BRIGHT = 2002, IDC_VOLUME = 2003, IDC_ENABLED = 2004, IDC_STARTUP = 2005;
struct Config { int delayMinutes=5; bool brightness=true, volume=true, enabled=true, startup=true; } cfg;
struct DisplayState { HANDLE handle=nullptr; DWORD min=0, cur=0, max=0; bool valid=false; };
std::vector<DisplayState> savedDisplays;
float savedVolume=1.0f; bool savedMute=false, savedAudioValid=false, sleeping=false;
int cursorHideCalls=0;
HWND mainWnd=nullptr, settingsWnd=nullptr; std::vector<HWND> overlays;
NOTIFYICONDATAW tray{}; HICON blackIcon=nullptr;

std::wstring configPath(){ wchar_t p[MAX_PATH]{}; GetEnvironmentVariableW(L"APPDATA",p,MAX_PATH); return std::wstring(p)+L"\\PixelShield.ini"; }
void loadConfig(){ std::wifstream f{std::filesystem::path(configPath())}; std::wstring k; while(f>>k){ if(k==L"delay"){f>>cfg.delayMinutes;} else if(k==L"brightness"){int v;f>>v;cfg.brightness=v!=0;} else if(k==L"volume"){int v;f>>v;cfg.volume=v!=0;} else if(k==L"enabled"){int v;f>>v;cfg.enabled=v!=0;} else if(k==L"startup"){int v;f>>v;cfg.startup=v!=0;} } cfg.delayMinutes=std::clamp(cfg.delayMinutes,5,10); }
void saveConfig(){ std::wofstream f(std::filesystem::path(configPath()),std::ios::trunc); f<<L"delay "<<cfg.delayMinutes<<L"\nbrightness "<<cfg.brightness<<L"\nvolume "<<cfg.volume<<L"\nenabled "<<cfg.enabled<<L"\nstartup "<<cfg.startup<<L"\n"; }

bool setStartup(bool enable){
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    bool ok=false;
    if(enable){
        wchar_t exe[MAX_PATH]{};
        DWORD n=GetModuleFileNameW(nullptr,exe,MAX_PATH);
        if(n>0 && n<MAX_PATH){
            std::wstring command=L"\""+std::wstring(exe)+L"\" --startup";
            ok=RegSetValueExW(key,L"PixelShield",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
        }
    }else{
        LONG result=RegDeleteValueW(key,L"PixelShield");
        ok=result==ERROR_SUCCESS || result==ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return ok;
}

BOOL CALLBACK enumMon(HMONITOR mon,HDC,LPRECT,LPARAM){ DWORD n=0; if(!GetNumberOfPhysicalMonitorsFromHMONITOR(mon,&n)||!n)return TRUE; std::vector<PHYSICAL_MONITOR> p(n); if(GetPhysicalMonitorsFromHMONITOR(mon,n,p.data())){ for(auto &x:p){ DisplayState d; d.handle=x.hPhysicalMonitor; d.valid=GetMonitorBrightness(d.handle,&d.min,&d.cur,&d.max)!=FALSE; if(d.valid)savedDisplays.push_back(d); else DestroyPhysicalMonitor(d.handle); } } return TRUE; }
void restoreBrightness(){ for(auto &d:savedDisplays){ if(d.valid)SetMonitorBrightness(d.handle,d.cur); DestroyPhysicalMonitor(d.handle); } savedDisplays.clear(); }
void setBrightnessZero(){ savedDisplays.clear(); EnumDisplayMonitors(nullptr,nullptr,enumMon,0); for(auto &d:savedDisplays) if(d.valid)SetMonitorBrightness(d.handle,d.min); }

bool getAudio(IAudioEndpointVolume** out){ IMMDeviceEnumerator* en=nullptr; IMMDevice* dev=nullptr; HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),(void**)&en); if(FAILED(hr))return false; hr=en->GetDefaultAudioEndpoint(eRender,eMultimedia,&dev); en->Release(); if(FAILED(hr))return false; hr=dev->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,(void**)out); dev->Release(); return SUCCEEDED(hr); }
void muteVolume(){ IAudioEndpointVolume* a=nullptr; if(!getAudio(&a))return; a->GetMasterVolumeLevelScalar(&savedVolume); BOOL mute=FALSE; a->GetMute(&mute); savedMute=(mute!=FALSE); savedAudioValid=true; a->SetMasterVolumeLevelScalar(0.0f,nullptr); a->SetMute(TRUE,nullptr); a->Release(); }
void restoreVolume(){ if(!savedAudioValid)return; IAudioEndpointVolume* a=nullptr; if(getAudio(&a)){ a->SetMasterVolumeLevelScalar(savedVolume,nullptr); a->SetMute(savedMute?TRUE:FALSE,nullptr); a->Release(); } savedAudioValid=false; }

LRESULT CALLBACK overlayProc(HWND h,UINT m,WPARAM w,LPARAM l){ if(m==WM_ERASEBKGND){ RECT r;GetClientRect(h,&r);FillRect((HDC)w,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));return 1;} if(m==WM_PAINT){ PAINTSTRUCT ps{}; HDC dc=BeginPaint(h,&ps); RECT r{};GetClientRect(h,&r);FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));EndPaint(h,&ps);return 0;} return DefWindowProcW(h,m,w,l); }
void hideCursor(){ cursorHideCalls=0; int count; do{ count=ShowCursor(FALSE); ++cursorHideCalls; }while(count>=0 && cursorHideCalls<32); }
void restoreCursor(){ while(cursorHideCalls>0){ ShowCursor(TRUE); --cursorHideCalls; } }
void showBlack(){ if(sleeping)return; sleeping=true; hideCursor(); if(cfg.brightness)setBrightnessZero(); if(cfg.volume)muteVolume(); overlays.clear(); EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR m,HDC,LPRECT r,LPARAM){ MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(m,&mi); HWND h=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"PixelShieldOverlay",L"",WS_POPUP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr); if(h){ShowWindow(h,SW_SHOWNOACTIVATE);UpdateWindow(h);overlays.push_back(h);}return TRUE;},0); }
void wake(){ if(!sleeping)return; for(HWND h:overlays)DestroyWindow(h); overlays.clear(); if(cfg.brightness)restoreBrightness(); if(cfg.volume)restoreVolume(); restoreCursor(); sleeping=false; }

void addTray(){ tray={}; tray.cbSize=sizeof(tray); tray.hWnd=mainWnd; tray.uID=1; tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP; tray.uCallbackMessage=WM_TRAY; tray.hIcon=blackIcon; lstrcpynW(tray.szTip,L"PixelShield",_countof(tray.szTip)); Shell_NotifyIconW(NIM_ADD,&tray); }
void showSettings();
void trayMenu(){ POINT p;GetCursorPos(&p); HMENU m=CreatePopupMenu(); AppendMenuW(m,MF_STRING,ID_SETTINGS,L"Settings"); AppendMenuW(m,MF_STRING,ID_ENABLE,cfg.enabled?L"Pause protection":L"Enable protection"); AppendMenuW(m,MF_SEPARATOR,0,nullptr); AppendMenuW(m,MF_STRING,ID_EXIT,L"Exit"); SetForegroundWindow(mainWnd); int c=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,mainWnd,nullptr); DestroyMenu(m); if(c==ID_SETTINGS)showSettings(); else if(c==ID_ENABLE){cfg.enabled=!cfg.enabled;if(!cfg.enabled)wake();saveConfig();} else if(c==ID_EXIT)DestroyWindow(mainWnd); }
LRESULT CALLBACK settingsProc(HWND h,UINT m,WPARAM w,LPARAM l){ switch(m){case WM_CREATE:{ CreateWindowW(L"STATIC",L"Idle timeout",WS_CHILD|WS_VISIBLE,18,18,100,22,h,nullptr,nullptr,nullptr); HWND cb=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,125,14,130,120,h,(HMENU)IDC_DELAY,nullptr,nullptr); SendMessageW(cb,CB_ADDSTRING,0,(LPARAM)L"5 minutes");SendMessageW(cb,CB_ADDSTRING,0,(LPARAM)L"6 minutes");SendMessageW(cb,CB_ADDSTRING,0,(LPARAM)L"7 minutes");SendMessageW(cb,CB_ADDSTRING,0,(LPARAM)L"8 minutes");SendMessageW(cb,CB_ADDSTRING,0,(LPARAM)L"9 minutes");SendMessageW(cb,CB_ADDSTRING,0,(LPARAM)L"10 minutes");SendMessageW(cb,CB_SETCURSEL,cfg.delayMinutes-5,0); CreateWindowW(L"BUTTON",L"Lower supported monitor brightness",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,18,55,260,24,h,(HMENU)IDC_BRIGHT,nullptr,nullptr);CreateWindowW(L"BUTTON",L"Mute system volume",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,18,85,220,24,h,(HMENU)IDC_VOLUME,nullptr,nullptr);CreateWindowW(L"BUTTON",L"Enable idle protection",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,18,115,220,24,h,(HMENU)IDC_ENABLED,nullptr,nullptr);CreateWindowW(L"BUTTON",L"Start with Windows",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,18,145,220,24,h,(HMENU)IDC_STARTUP,nullptr,nullptr);SendDlgItemMessageW(h,IDC_BRIGHT,BM_SETCHECK,cfg.brightness,0);SendDlgItemMessageW(h,IDC_VOLUME,BM_SETCHECK,cfg.volume,0);SendDlgItemMessageW(h,IDC_ENABLED,BM_SETCHECK,cfg.enabled,0);SendDlgItemMessageW(h,IDC_STARTUP,BM_SETCHECK,cfg.startup,0);CreateWindowW(L"BUTTON",L"Save",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,174,185,80,28,h,(HMENU)IDOK,nullptr,nullptr);return 0;} case WM_COMMAND:if(LOWORD(w)==IDOK){cfg.delayMinutes=5+(int)SendDlgItemMessageW(h,IDC_DELAY,CB_GETCURSEL,0,0);cfg.brightness=SendDlgItemMessageW(h,IDC_BRIGHT,BM_GETCHECK,0,0)==BST_CHECKED;cfg.volume=SendDlgItemMessageW(h,IDC_VOLUME,BM_GETCHECK,0,0)==BST_CHECKED;cfg.enabled=SendDlgItemMessageW(h,IDC_ENABLED,BM_GETCHECK,0,0)==BST_CHECKED;cfg.startup=SendDlgItemMessageW(h,IDC_STARTUP,BM_GETCHECK,0,0)==BST_CHECKED;setStartup(cfg.startup);if(!cfg.enabled)wake();saveConfig();DestroyWindow(h);return 0;}break;case WM_CLOSE:DestroyWindow(h);return 0;case WM_DESTROY:settingsWnd=nullptr;return 0;}return DefWindowProcW(h,m,w,l); }
void showSettings(){ if(settingsWnd){SetForegroundWindow(settingsWnd);return;} settingsWnd=CreateWindowExW(WS_EX_DLGMODALFRAME,L"PixelShieldSettings",L"PixelShield Settings",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, CW_USEDEFAULT,CW_USEDEFAULT,300,265,mainWnd,nullptr,GetModuleHandleW(nullptr),nullptr);ShowWindow(settingsWnd,SW_SHOW); }
LRESULT CALLBACK mainProc(HWND h,UINT m,WPARAM w,LPARAM l){ if(m==WM_TRAY){if(l==WM_RBUTTONUP||l==WM_CONTEXTMENU)trayMenu();else if(l==WM_LBUTTONDBLCLK)showSettings();return 0;} if(m==WM_TIMER){ if(!cfg.enabled){wake();return 0;} LASTINPUTINFO li{sizeof(li)}; if(GetLastInputInfo(&li)){DWORD idle=GetTickCount()-li.dwTime; if(sleeping){if(idle<1500)wake();}else if(idle>=(DWORD)cfg.delayMinutes*60000)showBlack();}return 0;} if(m==WM_DESTROY){wake();Shell_NotifyIconW(NIM_DELETE,&tray);PostQuitMessage(0);return 0;}return DefWindowProcW(h,m,w,l); }
HICON makeBlackIcon(){ HBITMAP color=CreateBitmap(32,32,1,32,nullptr); HBITMAP mask=CreateBitmap(32,32,1,1,nullptr); ICONINFO ii{};ii.fIcon=TRUE;ii.hbmColor=color;ii.hbmMask=mask;HICON i=CreateIconIndirect(&ii);DeleteObject(color);DeleteObject(mask);return i; }
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE, PWSTR cmdLine,int){ CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED); loadConfig(); setStartup(cfg.startup); WNDCLASSW ov{};ov.lpfnWndProc=overlayProc;ov.hInstance=inst;ov.lpszClassName=L"PixelShieldOverlay";ov.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);RegisterClassW(&ov); WNDCLASSW sw{};sw.lpfnWndProc=settingsProc;sw.hInstance=inst;sw.lpszClassName=L"PixelShieldSettings";sw.hCursor=LoadCursor(nullptr,IDC_ARROW);sw.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&sw); WNDCLASSW mw{};mw.lpfnWndProc=mainProc;mw.hInstance=inst;mw.lpszClassName=L"PixelShieldMain";RegisterClassW(&mw); mainWnd=CreateWindowW(L"PixelShieldMain",L"PixelShield",0,0,0,0,0,nullptr,nullptr,inst,nullptr);blackIcon=makeBlackIcon();addTray();SetTimer(mainWnd,1,500,nullptr);if(!cmdLine || !wcsstr(cmdLine,L"--startup"))showSettings(); MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);} if(blackIcon)DestroyIcon(blackIcon);CoUninitialize();return 0; }
