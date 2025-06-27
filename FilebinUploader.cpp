#include <windows.h>
#include <commdlg.h>
#include <wininet.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <ctime>
#include <cstdlib>
#include "resource.h"

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

#define IDC_UPLOAD 101
#define IDC_URL    103
#define IDC_COPY   104
#define IDC_PROGRESS_BASE 200
#define IDC_LABEL_BASE    300
#define IDC_FILENAME_BASE 400

#define WM_UPLOAD_PROGRESS (WM_USER + 1)
#define WM_UPLOAD_DONE     (WM_USER + 2)

// ----- Control Sizing -----
#define DIALOG_WIDTH 760
#define BTN_W 80
#define BTN_H 30
#define COPY_BTN_W 80
#define TEXTBOX_H 25
#define LEFT_MARGIN 10
#define RIGHT_MARGIN 10
#define CTRL_SPACING 10
#define MIN_TEXTBOX_W 50
#define CONTROL_ROW_HEIGHT 40 // Space reserved at top for controls

struct UploadProgressMsg {
    size_t fileIndex;
    DWORDLONG sent;
    DWORDLONG totalSize;
    double speedBps;
};

std::wstring Utf8ToWide(const std::string& str) {
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], sz);
    wstr.resize(sz - 1);
    return wstr;
}
std::string WideToUtf8(const std::wstring& wstr) {
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, 0, 0);
    std::string str(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], sz, 0, 0);
    str.resize(sz - 1);
    return str;
}
std::vector<char> ReadFileBinary(const std::wstring& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}
std::wstring GetFileName(const std::wstring& path) {
    wchar_t fname[_MAX_FNAME], ext[_MAX_EXT];
    _wsplitpath_s(path.c_str(), NULL, 0, NULL, 0, fname, _MAX_FNAME, ext, _MAX_EXT);
    std::wstring n(fname);
    n += ext;
    return n;
}
std::wstring FormatSize(DWORDLONG bytes) {
    wchar_t buf[64];
    if (bytes > 1024 * 1024 * 10)
        swprintf_s(buf, L"%.1f MB", bytes / 1048576.0);
    else if (bytes > 1024 * 10)
        swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
    else
        swprintf_s(buf, L"%llu bytes", bytes);
    return buf;
}
std::wstring FormatSpeed(double bps) {
    wchar_t buf[64];
    if (bps > 1024 * 1024)
        swprintf_s(buf, L"%.1f MB/s", bps / 1048576.0);
    else if (bps > 1024)
        swprintf_s(buf, L"%.1f KB/s", bps / 1024.0);
    else
        swprintf_s(buf, L"%.0f B/s", bps);
    return buf;
}

std::string GenerateBin() {
    static const char* chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string bin;
    srand((unsigned int)time(NULL) ^ (unsigned int)GetCurrentProcessId());
    for (int i = 0; i < 16; ++i)
        bin += chars[rand() % 62];
    return bin;
}

struct FileUpload {
    std::wstring path;
    std::wstring name;
    DWORDLONG size;
    DWORDLONG sent;
    HWND hProgress;
    HWND hLabel;
    HWND hFileName;
};

std::vector<FileUpload> g_files;
HWND g_hwnd = NULL;
HWND g_hUrl = NULL;
HWND g_hUpload = NULL;
HWND g_hCopy = NULL;
std::vector<HWND> g_hProgressBars, g_hLabels, g_hFileNames;
std::wstring g_binId;
bool g_uploading = false;
bool g_storage_full = false;

bool IsFilebinStorageFull() {
    HINTERNET hSession = InternetOpenW(L"FilebinUploader/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hSession) return false;
    HINTERNET hConnect = InternetConnectW(hSession, L"filebin.net", INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) { InternetCloseHandle(hSession); return false; }
    HINTERNET hRequest = HttpOpenRequestW(hConnect, L"GET", L"/", NULL, NULL, NULL,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
    if (!hRequest) { InternetCloseHandle(hConnect); InternetCloseHandle(hSession); return false; }

    BOOL bSent = HttpSendRequestW(hRequest, NULL, 0, NULL, 0);
    if (!bSent) {
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hSession);
        return false;
    }

    std::string html;
    char buf[4096];
    DWORD read = 0;
    while (InternetReadFile(hRequest, buf, sizeof(buf) - 1, &read) && read) {
        buf[read] = 0;
        html += buf;
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hSession);

    if (html.find("The storage capacity is reached and new file uploads will be rejected. Please come back later.") != std::string::npos) {
        return true;
    }
    return false;
}

const wchar_t* STORAGE_FULL_MSG = L"Storage capacity has been reached. Please try again later.";

void UpdateStorageStatus(HWND hwnd) {
    g_storage_full = IsFilebinStorageFull();
    HWND hStaticMsg = GetDlgItem(hwnd, 1200);
    if (g_storage_full) {
        EnableWindow(g_hUpload, FALSE);
        if (!hStaticMsg) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int msgY = CONTROL_ROW_HEIGHT + 5;
            CreateWindowW(L"STATIC", STORAGE_FULL_MSG, WS_CHILD | WS_VISIBLE | SS_CENTER,
                LEFT_MARGIN, msgY, rc.right - rc.left - 2 * LEFT_MARGIN, 25,
                hwnd, (HMENU)1200, GetModuleHandle(NULL), NULL);
        }
        else {
            SetWindowTextW(hStaticMsg, STORAGE_FULL_MSG);
            ShowWindow(hStaticMsg, SW_SHOW);
        }
    }
    else {
        EnableWindow(g_hUpload, TRUE);
        if (hStaticMsg) ShowWindow(hStaticMsg, SW_HIDE);
    }
}

void CopyTextToClipboard(HWND hWnd, const std::wstring& text) {
    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        size_t len = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
        if (hMem) {
            memcpy(GlobalLock(hMem), text.c_str(), len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }
}

std::wstring UploadOneFileToFilebinRaw(const std::wstring& binid, FileUpload& file, size_t fileIndex, HWND hwnd) {
    const size_t chunk_size = 64 * 1024;
    std::ifstream ifs(file.path, std::ios::binary | std::ios::ate);
    if (!ifs) return L"Failed to read file";

    DWORDLONG totalSize = ifs.tellg();
    ifs.seekg(0);

    HINTERNET hSession = InternetOpenW(L"FilebinUploader/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hSession) return L"Failed to open WinInet session";
    HINTERNET hConnect = InternetConnectW(hSession, L"filebin.net", INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) { InternetCloseHandle(hSession); return L"Failed to connect"; }

    std::wstring path = L"/" + binid + L"/" + file.name;
    HINTERNET hRequest = HttpOpenRequestW(hConnect, L"POST", path.c_str(), NULL, NULL, NULL,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);

    std::wstring wctype = L"Content-Type: application/octet-stream";
    HttpAddRequestHeadersW(hRequest, wctype.c_str(), (DWORD)-1, HTTP_ADDREQ_FLAG_ADD);

    INTERNET_BUFFERS ib = { 0 };
    ib.dwStructSize = sizeof(ib);
    ib.dwBufferTotal = (DWORD)totalSize;
    BOOL bSent = HttpSendRequestExW(hRequest, &ib, NULL, 0, 0);
    if (!bSent) {
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hSession);
        return L"Failed to send request";
    }

    DWORDLONG sent = 0;
    auto start = std::chrono::steady_clock::now();

    std::vector<char> buffer(chunk_size);
    while (ifs.read(buffer.data(), chunk_size) || ifs.gcount() > 0) {
        DWORD towrite = (DWORD)ifs.gcount();
        DWORD wrote = 0;

        if (!InternetWriteFile(hRequest, buffer.data(), towrite, &wrote)) {
            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConnect);
            InternetCloseHandle(hSession);
            return L"Failed to write file data";
        }
        sent += wrote;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        double speed = elapsed > 0 ? (sent / elapsed) : 0;
        UploadProgressMsg* msg = new UploadProgressMsg{ fileIndex, sent, totalSize, speed };
        PostMessage(hwnd, WM_UPLOAD_PROGRESS, (WPARAM)msg, 0);

        Sleep(10);
    }

    if (!HttpEndRequestW(hRequest, NULL, 0, 0)) {
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hSession);
        return L"Failed to finish request";
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hSession);

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start).count();
    double speed = elapsed > 0 ? (sent / elapsed) : 0;
    UploadProgressMsg* msg = new UploadProgressMsg{ fileIndex, sent, totalSize, speed };
    PostMessage(hwnd, WM_UPLOAD_PROGRESS, (WPARAM)msg, 0);

    return L"ok";
}

void PickFiles(HWND hwnd) {
    OPENFILENAME ofn = { 0 };
    wchar_t szFile[4096] = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = ARRAYSIZE(szFile);
    ofn.lpstrTitle = L"Select file(s) to upload";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
    ofn.lpstrFilter = L"All Files\0*.*\0";
    if (GetOpenFileName(&ofn)) {
        g_files.clear();
        wchar_t* p = szFile;
        std::wstring folder = p;
        p += folder.size() + 1;
        if (*p == 0) {
            FileUpload fu;
            fu.path = folder;
            fu.name = GetFileName(folder);
            std::ifstream ifs(folder, std::ios::binary | std::ios::ate);
            fu.size = ifs ? (DWORDLONG)ifs.tellg() : 0;
            fu.sent = 0;
            g_files.push_back(fu);
        }
        else {
            while (*p) {
                FileUpload fu;
                fu.path = folder + L"\\" + p;
                fu.name = p;
                std::ifstream ifs(fu.path, std::ios::binary | std::ios::ate);
                fu.size = ifs ? (DWORDLONG)ifs.tellg() : 0;
                fu.sent = 0;
                g_files.push_back(fu);
                p += wcslen(p) + 1;
            }
        }
    }
}

void LayoutProgressBars(HWND hwnd, const std::vector<FileUpload>& files,
    std::vector<HWND>& outProgress,
    std::vector<HWND>& outLabels,
    std::vector<HWND>& outFilenames) {
    for (HWND h : outProgress) DestroyWindow(h);
    for (HWND h : outLabels) DestroyWindow(h);
    for (HWND h : outFilenames) DestroyWindow(h);
    outProgress.clear();
    outLabels.clear();
    outFilenames.clear();

    RECT rc;
    GetClientRect(hwnd, &rc);
    int y = CONTROL_ROW_HEIGHT + 10;
    int n = (int)files.size();

    int ctrl_width = (std::max)((int)rc.right - (int)rc.left - 2 * LEFT_MARGIN, 240);

    for (int i = 0; i < n; ++i) {
        HWND hProg = CreateWindowEx(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE,
            LEFT_MARGIN, y, ctrl_width, 20, hwnd, (HMENU)(IDC_PROGRESS_BASE + i), GetModuleHandle(NULL), NULL);
        SendMessage(hProg, PBM_SETRANGE32, 0, (LPARAM)files[i].size);
        SendMessage(hProg, PBM_SETPOS, 0, 0);
        HWND hLabel = CreateWindowEx(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            LEFT_MARGIN, y + 20, ctrl_width, 20, hwnd, (HMENU)(IDC_LABEL_BASE + i), GetModuleHandle(NULL), NULL);
        HWND hFileName = CreateWindowEx(0, L"STATIC", files[i].name.c_str(), WS_CHILD | WS_VISIBLE,
            LEFT_MARGIN, y + 40, ctrl_width, 16, hwnd, (HMENU)(IDC_FILENAME_BASE + i), GetModuleHandle(NULL), NULL);
        outProgress.push_back(hProg);
        outLabels.push_back(hLabel);
        outFilenames.push_back(hFileName);
        y += 60;
    }
}

DWORD WINAPI UploadThreadProc(LPVOID param) {
    HWND hwnd = (HWND)param;
    std::wstring binid;
    {
        static CRITICAL_SECTION cs = { 0 };
        static bool cs_init = false;
        if (!cs_init) { InitializeCriticalSection(&cs); cs_init = true; }
        EnterCriticalSection(&cs);
        binid = Utf8ToWide(GenerateBin());
        g_binId = binid;
        LeaveCriticalSection(&cs);
    }
    std::wstring url = L"https://filebin.net/" + binid;
    SendMessage(g_hUrl, WM_SETTEXT, 0, (LPARAM)url.c_str());
    for (size_t i = 0; i < g_files.size(); ++i) {
        std::wstring result = UploadOneFileToFilebinRaw(binid, g_files[i], i, hwnd);
        if (result != L"ok") {
            std::wstring fail = L"Failed: " + g_files[i].name + L" (" + result + L")";
            SetWindowTextW(g_files[i].hLabel, fail.c_str());
        }
        else {
            SendMessage(g_files[i].hProgress, PBM_SETPOS, (WPARAM)g_files[i].size, 0);
            std::wstring done = L"100% - " + FormatSize(g_files[i].size);
            SetWindowTextW(g_files[i].hLabel, done.c_str());
        }
    }
    PostMessage(hwnd, WM_UPLOAD_DONE, 0, 0);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);
        int btn_w = BTN_W;
        int copy_btn_w = COPY_BTN_W;
        int textbox_w = DIALOG_WIDTH - (2 * LEFT_MARGIN + btn_w + copy_btn_w + 2 * CTRL_SPACING);
        if (textbox_w < MIN_TEXTBOX_W) textbox_w = MIN_TEXTBOX_W;
        int left_margin = LEFT_MARGIN;
        int spacing = CTRL_SPACING;
        int control_y = 10;
        g_hUpload = CreateWindowW(L"BUTTON", L"Upload", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            left_margin, control_y, btn_w, BTN_H, hwnd, (HMENU)IDC_UPLOAD, NULL, NULL);
        g_hUrl = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY,
            left_margin + btn_w + spacing, control_y, textbox_w, TEXTBOX_H, hwnd, (HMENU)IDC_URL, NULL, NULL);
        g_hCopy = CreateWindowW(L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
            left_margin + btn_w + spacing + textbox_w + spacing, control_y, copy_btn_w, TEXTBOX_H, hwnd, (HMENU)IDC_COPY, NULL, NULL);
        PostMessage(hwnd, WM_USER + 100, 0, 0);
        break;
    }
    case (WM_USER + 100): {
        UpdateStorageStatus(hwnd);
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDC_UPLOAD && !g_uploading && !g_storage_full) {
            PickFiles(hwnd);
            LayoutProgressBars(hwnd, g_files, g_hProgressBars, g_hLabels, g_hFileNames);
            for (size_t i = 0; i < g_files.size(); ++i) {
                g_files[i].hProgress = g_hProgressBars[i];
                g_files[i].hLabel = g_hLabels[i];
                g_files[i].hFileName = g_hFileNames[i];
                g_files[i].sent = 0;
            }
            SetWindowTextW(g_hUrl, L"Uploading...");
            EnableWindow(g_hCopy, FALSE);
            SetWindowTextW(g_hCopy, L"Copy");
            g_uploading = true;
            EnableWindow(g_hUpload, FALSE);
            CloseHandle(CreateThread(NULL, 0, UploadThreadProc, hwnd, 0, NULL));
        }
        if (LOWORD(wParam) == IDC_COPY) {
            int len = GetWindowTextLengthW(g_hUrl);
            if (len > 0) {
                std::wstring url(len, L'\0');
                GetWindowTextW(g_hUrl, &url[0], len + 1);
                CopyTextToClipboard(hwnd, url);
            }
            SetWindowTextW(g_hCopy, L"Copied");
            EnableWindow(g_hCopy, FALSE);
        }
        break;
    }
    case WM_UPLOAD_PROGRESS: {
        UploadProgressMsg* msg = (UploadProgressMsg*)wParam;
        if (msg && msg->fileIndex < g_files.size()) {
            SendMessage(g_files[msg->fileIndex].hProgress, PBM_SETRANGE32, 0, (LPARAM)msg->totalSize);
            SendMessage(g_files[msg->fileIndex].hProgress, PBM_SETPOS, (WPARAM)msg->sent, 0);
            double percent = msg->totalSize ? (double)msg->sent / msg->totalSize * 100.0 : 0;
            std::wstring text = std::to_wstring((int)percent) + L"% - " +
                FormatSize(msg->sent) + L" / " + FormatSize(msg->totalSize) +
                L" (" + FormatSpeed(msg->speedBps) + L")";
            SetWindowTextW(g_files[msg->fileIndex].hLabel, text.c_str());
        }
        delete msg;
        break;
    }
    case WM_UPLOAD_DONE: {
        g_uploading = false;
        PostMessage(hwnd, WM_USER + 100, 0, 0);
        std::wstring url = L"https://filebin.net/" + g_binId;
        SetWindowTextW(g_hUrl, url.c_str());
        EnableWindow(g_hCopy, TRUE);
        SetWindowTextW(g_hCopy, L"Copy");
        break;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int btn_w = BTN_W;
        int btn_h = BTN_H;
        int copy_btn_w = COPY_BTN_W;
        int textbox_h = TEXTBOX_H;
        int left_margin = LEFT_MARGIN;
        int spacing = CTRL_SPACING;

        int available_width = width - (btn_w + copy_btn_w + 2 * spacing + 2 * left_margin);
        int textbox_w = (std::max)(available_width, MIN_TEXTBOX_W);

        int x = left_margin;
        int control_y = 10;

        MoveWindow(g_hUpload, x, control_y, btn_w, btn_h, TRUE);
        MoveWindow(g_hUrl, x + btn_w + spacing, control_y, textbox_w, textbox_h, TRUE);
        MoveWindow(g_hCopy, x + btn_w + spacing + textbox_w + spacing, control_y, copy_btn_w, textbox_h, TRUE);

        LayoutProgressBars(hwnd, g_files, g_hProgressBars, g_hLabels, g_hFileNames);

        HWND hStaticMsg = GetDlgItem(hwnd, 1200);
        if (hStaticMsg) {
            int msgY = CONTROL_ROW_HEIGHT + 5;
            MoveWindow(hStaticMsg, LEFT_MARGIN, msgY, rc.right - rc.left - 2 * LEFT_MARGIN, 25, TRUE);
        }
        break;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const wchar_t CLASS_NAME[] = L"FilebinUploaderClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); // set before RegisterClass
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        L"Filebin Uploader",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, DIALOG_WIDTH, 400,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) return 0;
    g_hwnd = hwnd;

    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)));
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)));

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // --- Force cursor to arrow immediately! ---
    SendMessage(hwnd, WM_SETCURSOR, 0, 0);
    SetCursor(LoadCursor(NULL, IDC_ARROW));

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}