#include "wcap.h"
#include "wcap_config.h"
#include "wcap_audio_capture.h"
#include "wcap_screen_capture.h"
#include "wcap_encoder.h"

#include <dxgi1_6.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <windowsx.h>

#if defined(WCAP_CLI)
#include "wcap_cli_tools.h"
#endif

#pragma comment (lib, "ntdll")
#pragma comment (lib, "kernel32")
#pragma comment (lib, "user32")
#pragma comment (lib, "gdi32")
#pragma comment (lib, "msimg32")
#pragma comment (lib, "dxgi")
#pragma comment (lib, "d3dcompiler")
#pragma comment (lib, "d3d11")
#pragma comment (lib, "dwmapi")
#pragma comment (lib, "shell32")
#pragma comment (lib, "shlwapi")
#pragma comment (lib, "mfplat")
#pragma comment (lib, "mfuuid")
#pragma comment (lib, "mfreadwrite")
#pragma comment (lib, "evr")
#pragma comment (lib, "strmiids")
#pragma comment (lib, "ksuser")
#pragma comment (lib, "mmdevapi")
#pragma comment (lib, "ole32")
#pragma comment (lib, "wmcodecdspuuid")
#pragma comment (lib, "avrt")
#pragma comment (lib, "uxtheme")
#pragma comment (lib, "OneCore")
#pragma comment (lib, "CoreMessaging")

#if defined(_M_AMD64)
// this is needed to be able to use Nvidia Media Foundation encoders on Optimus systems
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

#define WM_WCAP_ALREADY_RUNNING (WM_USER+1)
#define WM_WCAP_STOP_CAPTURE    (WM_USER+2)
#define WM_WCAP_TRAY_TITLE      (WM_USER+3)
#define WM_WCAP_COMMAND         (WM_USER+4)

#define WCAP_AUDIO_CAPTURE_TIMER    1
#define WCAP_AUDIO_CAPTURE_INTERVAL 100 // msec

#define WCAP_VIDEO_UPDATE_TIMER     2
#define WCAP_VIDEO_UPDATE_INTERVAL  100 // msec

#define WCAP_COUNTDOWN_TIMER        3
#define WCAP_COUNTDOWN_INTERVAL     1000 // msec

#define WCAP_CLI_DURATION_TIMER     4

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE      0x11
#endif

#define CMD_WCAP     1
#define CMD_QUIT     2
#define CMD_SETTINGS 3

#define HOT_RECORD_WINDOW  1
#define HOT_RECORD_MONITOR 2
#define HOT_RECORD_REGION  3

#define WCAP_RESIZE_NONE 0
#define WCAP_RESIZE_TL   1
#define WCAP_RESIZE_T    2
#define WCAP_RESIZE_TR   3
#define WCAP_RESIZE_L    4
#define WCAP_RESIZE_M    5
#define WCAP_RESIZE_R    6
#define WCAP_RESIZE_BL   7
#define WCAP_RESIZE_B    8
#define WCAP_RESIZE_BR   9

#define WCAP_UI_FONT      L"Segoe UI"
#define WCAP_UI_FONT_SIZE 16

#define WCAP_RECT_BORDER 2

#define WCAP_COUNTDOWN_SIZE      220
#define WCAP_COUNTDOWN_FONT_SIZE 160

// constants
static WCHAR gConfigPath[MAX_PATH];
static LARGE_INTEGER gTickFreq;
static HICON gIcon1;
static HICON gIcon2;
static UINT WM_TASKBARCREATED;
static HCURSOR gCursorArrow;
static HCURSOR gCursorClick;
static HCURSOR gCursorResize[10];
static HFONT gFont;
static HFONT gFontBold;

// recording state
static BOOL gRecordingStarted;
static BOOL gRecording;
static DWORD gRecordingLimitFramerate;
static DWORD gRecordingDroppedFrames;
static UINT64 gRecordingLastFrame;
static UINT64 gRecordingNextEncode;
static UINT64 gRecordingNextTooltip;
static EXECUTION_STATE gRecordingState;
static WCHAR gRecordingPath[MAX_PATH];

// when selecting rectangle to record
static HMONITOR gRectMonitor;
static HDC gRectContext;
static HDC gRectDarkContext;
static HBITMAP gRectBitmap;
static HBITMAP gRectDarkBitmap;
static DWORD gRectWidth;
static DWORD gRectHeight;
static BOOL gRectSelected;
static POINT gRectSelection[2];
static POINT gRectMousePos;
static int gRectResize;
static int gRectSetSize[2];
static BOOL gRectSetSizeClick;

// countdown before recording starts
static HWND gCountdownWindow;
static HFONT gFontCountdown;
static DWORD gCountdownValue; // 0 when countdown is not active
static WPARAM gCountdownAction; // HOT_RECORD_xyz
static HWND gCountdownTargetWindow;
static HMONITOR gCountdownTargetMonitor;

// globals
static HWND gWindow;
static Config gConfig;
static AudioCapture gAudio;
static ScreenCapture gCapture;
static Encoder gEncoder;

#if defined(WCAP_CLI)
static WCHAR gCliOutputPath[MAX_PATH];
static int gCliExitCode = 1;
static HANDLE gCliStopEvent;  // set by 'wcap-cli stop'
static HANDLE gCliAbortEvent; // set by Ctrl+C, interrupts waiting for start signal
static void CliReportFinished(void);
static BOOL CliWaitStart(void);
#endif

static void ShowNotification(LPCWSTR Message, LPCWSTR Title, DWORD Flags)
{
#if defined(WCAP_CLI)
	WCHAR Text[512];
	StrFormat(Text, L"%ls: %ls\n", Title ? Title : WCAP_TITLE, Message);
	WriteText(STD_ERROR_HANDLE, Text);
	return;
#endif
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_INFO | NIF_TIP,
		.dwInfoFlags = Flags, // NIIF_INFO, NIIF_WARNING, NIIF_ERROR
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	StrCpyNW(Data.szInfo, Message, _countof(Data.szInfo));
	StrCpyNW(Data.szInfoTitle, Title ? Title : WCAP_TITLE, _countof(Data.szInfoTitle));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayTitle(LPCWSTR Title)
{
#if defined(WCAP_CLI)
	return;
#endif
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_TIP,
	};
	StrCpyNW(Data.szTip, Title, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void UpdateTrayIcon(HICON Icon)
{
#if defined(WCAP_CLI)
	return;
#endif
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = gWindow,
		.uFlags = NIF_ICON,
		.hIcon = Icon,
	};
	Shell_NotifyIconW(NIM_MODIFY, &Data);
}

static void AddTrayIcon(HWND Window)
{
#if defined(WCAP_CLI)
	return;
#endif
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
		.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
		.uCallbackMessage = WM_WCAP_COMMAND,
		.hIcon = gIcon1,
	};
	StrCpyNW(Data.szTip, WCAP_TITLE, _countof(Data.szTip));
	Shell_NotifyIconW(NIM_ADD, &Data);
}

static void RemoveTrayIcon(HWND Window)
{
#if defined(WCAP_CLI)
	return;
#endif
	NOTIFYICONDATAW Data =
	{
		.cbSize = sizeof(Data),
		.hWnd = Window,
	};
	Shell_NotifyIconW(NIM_DELETE, &Data);
}

static void ShowFileInFolder(LPCWSTR Filename)
{
	SFGAOF Flags;
	PIDLIST_ABSOLUTE List;
	if (Filename[0] && SUCCEEDED(SHParseDisplayName(Filename, NULL, &List, 0, &Flags)))
	{
		HR(SHOpenFolderAndSelectItems(List, 0, NULL, 0));
		CoTaskMemFree(List);
	}
}

static void StartRecording(ID3D11Device* Device, HWND Window)
{
#if defined(WCAP_CLI)
	if (gCapture.Rect.right <= gCapture.Rect.left || gCapture.Rect.bottom <= gCapture.Rect.top)
	{
		ErrorMessage(L"Capture area is empty (window minimized, or crop is outside of captured area)");
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}
	if (gCapture.HasCrop && (gCapture.Rect.right - gCapture.Rect.left != gCapture.Crop.right - gCapture.Crop.left ||
		gCapture.Rect.bottom - gCapture.Rect.top != gCapture.Crop.bottom - gCapture.Crop.top))
	{
		CliWarn(L"crop does not fit into captured area, using %dx%d", gCapture.Rect.right - gCapture.Rect.left, gCapture.Rect.bottom - gCapture.Rect.top);
	}
#endif

	SYSTEMTIME Time;
	GetLocalTime(&Time);

	int Error = SHCreateDirectoryExW(NULL, gConfig.OutputFolder, NULL);
	if (Error != ERROR_SUCCESS && Error != ERROR_FILE_EXISTS && Error != ERROR_ALREADY_EXISTS)
	{
		ShowNotification(L"Cannot create output folder!", L"Cannot Start Recording", NIIF_WARNING);
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

	WCHAR Filename[256];
	StrFormat(Filename, L"%04u%02u%02u_%02u%02u%02u.mp4", Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);

	StrCpyW(gRecordingPath, gConfig.OutputFolder);
	PathAppendW(gRecordingPath, Filename);

#if defined(WCAP_CLI)
	if (gCliOutputPath[0])
	{
		StrCpyW(gRecordingPath, gCliOutputPath);
	}
#endif

	DWM_TIMING_INFO Info = { .cbSize = sizeof(Info) };
	HR(DwmGetCompositionTimingInfo(NULL, &Info));

	DWORD FramerateNum = Info.rateCompose.uiNumerator;
	DWORD FramerateDen = Info.rateCompose.uiDenominator;
	if (gConfig.VideoMaxFramerate > 0 && gConfig.VideoMaxFramerate * FramerateDen < FramerateNum)
	{
		// limit rate only if max framerate is specified and it is lower than compositor framerate
		gRecordingLimitFramerate = gConfig.VideoMaxFramerate;
		FramerateNum = gConfig.VideoMaxFramerate;
		FramerateDen = 1;
	}
	else
	{
		gRecordingLimitFramerate = 0;
	}

	EncoderConfig EncConfig =
	{
		.Width = gCapture.Rect.right - gCapture.Rect.left,
		.Height = gCapture.Rect.bottom - gCapture.Rect.top,
		.FramerateNum = FramerateNum,
		.FramerateDen = FramerateDen,
		.Config = &gConfig,
	};

#if defined(WCAP_CLI)
	if (gCli.FramesOnly)
	{
		// png sequence, no mp4 and no audio
		gConfig.CaptureAudio = FALSE;
		SHCreateDirectoryExW(NULL, gCli.FramesDir, NULL);
		StrCpyW(gRecordingPath, gCli.FramesDir);
	}
#endif

	if (gConfig.CaptureAudio)
	{
		HWND ApplicationWindow = gConfig.ApplicationLocalAudio && AudioCapture_CanCaptureApplicationLocal() ? Window : NULL;
		if (!AudioCapture_Start(&gAudio, ApplicationWindow))
		{
			ShowNotification(L"Cannot capture audio!", L"Cannot Start Recording", NIIF_WARNING);
			ScreenCapture_Stop(&gCapture);
			ID3D11Device_Release(Device);
			return;
		}
		EncConfig.AudioFormat = gAudio.Format;
	}

#if defined(WCAP_CLI)
	BOOL UseEncoder = !gCli.FramesOnly;
#else
	BOOL UseEncoder = TRUE;
#endif

	if (UseEncoder && !Encoder_Start(&gEncoder, Device, gRecordingPath, &EncConfig))
	{
		if (gConfig.CaptureAudio)
		{
			AudioCapture_Stop(&gAudio);
		}
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}

#if defined(WCAP_CLI)
	if (gCli.FramesOnly)
	{
		CliPngQueue_Start(&gCli.Png, EncConfig.Width, EncConfig.Height);
	}

	// everything is ready, wait for start signal so recording begins right when it arrives
	if (!CliWaitStart())
	{
		if (gConfig.CaptureAudio)
		{
			AudioCapture_Stop(&gAudio);
		}
		if (UseEncoder)
		{
			Encoder_Stop(&gEncoder);
			DeleteFileW(gRecordingPath);
		}
		CliPngQueue_Finish(&gCli.Png);
		ScreenCapture_Stop(&gCapture);
		ID3D11Device_Release(Device);
		return;
	}
#endif

	gRecordingNextTooltip = 0;
	gRecordingNextEncode = 0;
	gRecordingLastFrame = 0;
	gRecordingDroppedFrames = 0;
	ScreenCapture_Start(&gCapture, gConfig.MouseCursor, gConfig.ShowRecordingBorder, gConfig.IncludeSecondaryWindows);

	if (gConfig.CaptureAudio)
	{
		SetTimer(gWindow, WCAP_AUDIO_CAPTURE_TIMER, WCAP_AUDIO_CAPTURE_INTERVAL, NULL);
	}
	SetTimer(gWindow, WCAP_VIDEO_UPDATE_TIMER, WCAP_VIDEO_UPDATE_INTERVAL, NULL);

	UpdateTrayIcon(gIcon2);
	gRecordingState = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
	gRecording = TRUE;

	ID3D11Device_Release(Device);
}

static void EncodeCapturedAudio(void)
{
	if (gEncoder.StartTime == 0)
	{
		// we don't know when first video frame starts yet
		return;
	}

	AudioCaptureData Data;
	while (AudioCapture_GetData(&gAudio, &Data, gEncoder.StartTime))
	{
		UINT32 FramesToEncode = (UINT32)Data.Count;
		if (Data.Time < gEncoder.StartTime)
		{
			const UINT32 SampleRate = gAudio.Format->nSamplesPerSec;
			const UINT32 BytesPerFrame = gAudio.Format->nBlockAlign;

			// figure out how much time (100nsec units) and frame count to skip from current buffer
			UINT64 TimeToSkip = gEncoder.StartTime - Data.Time;
			UINT32 FramesToSkip = (UINT32)((TimeToSkip * SampleRate - 1) / MF_UNITS_PER_SECOND + 1);
			if (FramesToSkip < FramesToEncode)
			{
				// need to skip part of captured data
				Data.Time += FramesToSkip * MF_UNITS_PER_SECOND / SampleRate;
				FramesToEncode -= FramesToSkip;
				if (Data.Samples)
				{
					Data.Samples = (BYTE*)Data.Samples + FramesToSkip * BytesPerFrame;
				}
			}
			else
			{
				// need to skip all of captured data
				FramesToEncode = 0;
			}
		}
		if (FramesToEncode != 0)
		{
			Assert(Data.Time >= gEncoder.StartTime);
			Encoder_NewSamples(&gEncoder, Data.Samples, FramesToEncode, Data.Time, gTickFreq.QuadPart);
		}
		AudioCapture_ReleaseData(&gAudio, &Data);
	}
}

static void StopRecording(void)
{
	gRecording = FALSE;
	SetThreadExecutionState(gRecordingState);

	if (gConfig.CaptureAudio)
	{
		KillTimer(gWindow, WCAP_AUDIO_CAPTURE_TIMER);
		AudioCapture_Flush(&gAudio);
		EncodeCapturedAudio();
		AudioCapture_Stop(&gAudio);
	}
	KillTimer(gWindow, WCAP_VIDEO_UPDATE_TIMER);

	ScreenCapture_Stop(&gCapture);
#if defined(WCAP_CLI)
	if (!gCli.FramesOnly)
#endif
	Encoder_Stop(&gEncoder);
	if (gConfig.OpenFolder)
	{
		ShowFileInFolder(gRecordingPath);
	}

	SetWindowPos(gWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE);
	SetWindowLongW(gWindow, GWL_EXSTYLE, 0);

	UpdateTrayIcon(gIcon1);
	UpdateTrayTitle(WCAP_TITLE);

#if defined(WCAP_CLI)
	KillTimer(gWindow, WCAP_CLI_DURATION_TIMER);
	CliReportFinished();

	gCliExitCode = 0;
	PostQuitMessage(0);
#endif
}

static ID3D11Device* CreateDevice(void)
{
	IDXGIAdapter* Adapter = NULL;

	if (gConfig.HardwareEncoder)
	{
		IDXGIFactory* Factory;
		if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void**)&Factory)))
		{
			IDXGIFactory6* Factory6;
			if (SUCCEEDED(IDXGIFactory_QueryInterface(Factory, &IID_IDXGIFactory6, (void**)&Factory6)))
			{
				DXGI_GPU_PREFERENCE Preference = gConfig.HardwarePreferIntegrated ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
				if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(Factory6, 0, Preference, &IID_IDXGIAdapter, &Adapter)))
				{
					// just to be safe
					Adapter = NULL;
				}
				IDXGIFactory6_Release(Factory6);
			}
			IDXGIFactory_Release(Factory);
		}
	}

	ID3D11Device* Device;

	UINT flags = 0;
#ifndef NDEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	// if adapter is selected then driver type must be unknown
	D3D_DRIVER_TYPE Driver = Adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
	if (FAILED(D3D11CreateDevice(Adapter, Driver, NULL, flags, (D3D_FEATURE_LEVEL[]) { D3D_FEATURE_LEVEL_11_0 }, 1, D3D11_SDK_VERSION, &Device, NULL, NULL)))
	{
		ShowNotification(L"Cannot to create D3D11 device!", L"Error", NIIF_ERROR);
		Device = NULL;
	}
	if (Adapter)
	{
		IDXGIAdapter_Release(Adapter);
	}

	if (flags & D3D11_CREATE_DEVICE_DEBUG)
	{
		ID3D11InfoQueue* Info;
		if (SUCCEEDED(ID3D11Device_QueryInterface(Device, &IID_ID3D11InfoQueue, &Info)))
		{
			ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
			ID3D11InfoQueue_Release(Info);
		}
	}

	return Device;
}

// returns top-level window that can be captured, or NULL
static HWND GetCaptureWindow(HWND Window)
{
	if (Window == NULL)
	{
		ShowNotification(L"No window is selected!", L"Cannot Start Recording", NIIF_WARNING);
		return NULL;
	}

	// figure out who is owner of child window if somehow child window is selected (happens for fancy winamp skins)
	HWND Parent = GetParent(Window);
	while (Parent != NULL)
	{
		Window = Parent;
		Parent = GetParent(Window);
	}

	DWORD Affinity;
	BOOL Success = GetWindowDisplayAffinity(Window, &Affinity);
	Assert(Success);

	if (Affinity != WDA_NONE)
	{
		ShowNotification(L"Window is excluded from capture!", L"Cannot Start Recording", NIIF_WARNING);
		return NULL;
	}

	LONG ExStyle = GetWindowLongW(Window, GWL_EXSTYLE);
	if (ExStyle & WS_EX_TOOLWINDOW)
	{
		ShowNotification(L"Cannot capture toolbar window!", L"Cannot Start Recording", NIIF_WARNING);
		return NULL;
	}

	return Window;
}

static void CaptureWindow(HWND Window)
{
	if (!IsWindow(Window))
	{
		ShowNotification(L"Window does not exist anymore!", L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		return;
	}

	if (!ScreenCapture_CreateForWindow(&gCapture, Device, Window, gConfig.OnlyClientArea, !gConfig.KeepRoundedWindowCorners))
	{
		ID3D11Device_Release(Device);
		ShowNotification(L"Cannot record selected window!", L"Error", NIIF_WARNING);
		return;
	}

	StartRecording(Device, Window);
}

// returns monitor where mouse cursor is, or NULL
static HMONITOR GetCaptureMonitor(void)
{
	POINT Mouse;
	GetCursorPos(&Mouse);

	HMONITOR Monitor = MonitorFromPoint(Mouse, MONITOR_DEFAULTTONULL);
	if (Monitor == NULL)
	{
		ShowNotification(L"Unknown monitor!", L"Cannot Start Recording", NIIF_WARNING);
	}
	return Monitor;
}

// Rect is optional, in monitor relative coordinates
static void CaptureMonitor(HMONITOR Monitor, const RECT* Rect)
{
	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		return;
	}

	if (!ScreenCapture_CreateForMonitor(&gCapture, Device, Monitor, Rect))
	{
		ID3D11Device_Release(Device);
		ShowNotification(L"Cannot record selected monitor!", L"Error", NIIF_WARNING);
		return;
	}

	StartRecording(Device, NULL);
}

static void CaptureRegionInit(void)
{
	POINT Mouse;
	GetCursorPos(&Mouse);

	HMONITOR Monitor = MonitorFromPoint(Mouse, MONITOR_DEFAULTTONULL);
	if (Monitor == NULL)
	{
		ShowNotification(L"Unknown monitor!", L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	MONITORINFOEXW Info = { .cbSize = sizeof(Info) };
	GetMonitorInfoW(Monitor, (LPMONITORINFO)&Info);

	HDC DeviceContext = CreateDCW(L"DISPLAY", Info.szDevice, NULL, NULL);
	if (DeviceContext == NULL)
	{
		ShowNotification(L"Error getting HDC of monitor!", L"Cannot Start Recording", NIIF_WARNING);
		return;
	}

	DWORD Width = Info.rcMonitor.right - Info.rcMonitor.left;
	DWORD Height = Info.rcMonitor.bottom - Info.rcMonitor.top;

	// capture image from desktop

	HDC MemoryContext = CreateCompatibleDC(DeviceContext);
	Assert(MemoryContext);

	HBITMAP MemoryBitmap = CreateCompatibleBitmap(DeviceContext, Width, Height);
	Assert(MemoryBitmap);

	SelectObject(MemoryContext, MemoryBitmap);
	BitBlt(MemoryContext, 0, 0, Width, Height, DeviceContext, 0, 0, SRCCOPY);

	// prepare darkened image by doing alpha blend

	HDC MemoryDarkContext = CreateCompatibleDC(DeviceContext);
	Assert(MemoryDarkContext);

	HBITMAP MemoryDarkBitmap = CreateCompatibleBitmap(DeviceContext, Width, Height);
	Assert(MemoryDarkBitmap);

	BLENDFUNCTION Blend =
	{
		.BlendOp = AC_SRC_OVER,
		.SourceConstantAlpha = 0x40,
	};

	SelectObject(MemoryDarkContext, MemoryDarkBitmap);
	AlphaBlend(MemoryDarkContext, 0, 0, Width, Height, MemoryContext, 0, 0, Width, Height, Blend);

	// done

	DeleteDC(DeviceContext);

	gRectMonitor = Monitor;
	gRectContext = MemoryContext;
	gRectDarkContext = MemoryDarkContext;
	gRectBitmap = MemoryBitmap;
	gRectDarkBitmap = MemoryDarkBitmap;
	gRectWidth = Width;
	gRectHeight = Height;
	gRectSelected = FALSE;
	gRectResize = WCAP_RESIZE_NONE;
	gRectSetSize[0] = gRectSetSize[1] = 0;
	gRectSetSizeClick = FALSE;

	SetCursor(gCursorResize[WCAP_RESIZE_NONE]);
	SetWindowPos(gWindow, HWND_TOPMOST, Info.rcMonitor.left, Info.rcMonitor.top, Width, Height, SWP_SHOWWINDOW);
	SetForegroundWindow(gWindow);
	InvalidateRect(gWindow, NULL, FALSE);
}

static void CaptureRegionRelease(void)
{
	SetWindowPos(gWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE);
	SetWindowLongW(gWindow, GWL_EXSTYLE, 0);

	if (gRectContext)
	{
		DeleteDC(gRectContext);
		gRectContext = NULL;

		DeleteObject(gRectBitmap);
		gRectBitmap = NULL;

		DeleteDC(gRectDarkContext);
		gRectDarkContext = NULL;

		DeleteObject(gRectDarkBitmap);
		gRectDarkBitmap = NULL;
	}
}

static void CaptureRegionDone(void)
{
	SetCursor(gCursorArrow);
	ReleaseCapture();

	CaptureRegionRelease();
}

static void CaptureRegion(void)
{
	CaptureRegionDone();

	MONITORINFO Info = { .cbSize = sizeof(Info) };
	GetMonitorInfoW(gRectMonitor, &Info);

	RECT Rect =
	{
		.left   = gRectSelection[0].x,
		.right  = gRectSelection[1].x,
		.top    = gRectSelection[0].y,
		.bottom = gRectSelection[1].y,
	};

	LONG ExStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT;
	SetWindowLongW(gWindow, GWL_EXSTYLE, ExStyle);
	SetLayeredWindowAttributes(gWindow, RGB(255, 0, 255), 0, LWA_COLORKEY);

	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		CaptureRegionRelease();
		return;
	}

	if (!ScreenCapture_CreateForMonitor(&gCapture, Device, gRectMonitor, &Rect))
	{
		ShowNotification(L"Cannot record monitor!", L"Error", NIIF_WARNING);
		CaptureRegionRelease();
		return;
	}

	StartRecording(Device, NULL);

	if (gRecording)
	{
		int X = Info.rcMonitor.left + Rect.left - (WCAP_RECT_BORDER + 1);
		int Y = Info.rcMonitor.top + Rect.top - (WCAP_RECT_BORDER + 1);
		int W = Rect.right - Rect.left + 2 * (WCAP_RECT_BORDER + 1);
		int H = Rect.bottom - Rect.top + 2 * (WCAP_RECT_BORDER + 1);
		SetWindowPos(gWindow, HWND_TOPMOST, X, Y, W, H, SWP_SHOWWINDOW);
		InvalidateRect(gWindow, NULL, FALSE);
	}
	else
	{
		CaptureRegionRelease();
	}
}

static void CountdownStop(void)
{
	KillTimer(gWindow, WCAP_COUNTDOWN_TIMER);
	if (gCountdownWindow)
	{
		ShowWindow(gCountdownWindow, SW_HIDE);
	}
	gCountdownValue = 0;
}

static void CountdownFinish(void)
{
	CountdownStop();

	// make sure compositor has removed countdown window from screen before capture starts
	DwmFlush();

	gRecordingStarted = TRUE;
	if (gCountdownAction == HOT_RECORD_WINDOW)
	{
		CaptureWindow(gCountdownTargetWindow);
	}
	else if (gCountdownAction == HOT_RECORD_MONITOR)
	{
		CaptureMonitor(gCountdownTargetMonitor, NULL);
	}
	else if (gCountdownAction == HOT_RECORD_REGION)
	{
		CaptureRegion();
	}
	gRecordingStarted = FALSE;
}

// shows countdown centered on Area (screen coordinates), recording starts when it reaches zero
static void CountdownStart(WPARAM Action, const RECT* Area)
{
	gCountdownAction = Action;

	if (gConfig.Countdown == 0 || gCountdownWindow == NULL)
	{
		CountdownFinish();
		return;
	}

	gCountdownValue = gConfig.Countdown;

	int X = (Area->left + Area->right - WCAP_COUNTDOWN_SIZE) / 2;
	int Y = (Area->top + Area->bottom - WCAP_COUNTDOWN_SIZE) / 2;
	SetWindowPos(gCountdownWindow, HWND_TOPMOST, X, Y, WCAP_COUNTDOWN_SIZE, WCAP_COUNTDOWN_SIZE, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(gCountdownWindow, NULL, FALSE);

	SetTimer(gWindow, WCAP_COUNTDOWN_TIMER, WCAP_COUNTDOWN_INTERVAL, NULL);
}

static LRESULT CALLBACK CountdownWindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_NCHITTEST)
	{
		return HTTRANSPARENT;
	}
	else if (Message == WM_ERASEBKGND)
	{
		return 1;
	}
	else if (Message == WM_PAINT)
	{
		PAINTSTRUCT Paint;
		HDC PaintContext = BeginPaint(Window, &Paint);

		HDC Context;
		HPAINTBUFFER BufferedPaint = BeginBufferedPaint(PaintContext, &Paint.rcPaint, BPBF_COMPATIBLEBITMAP, NULL, &Context);
		if (BufferedPaint)
		{
			RECT Rect;
			GetClientRect(Window, &Rect);

			HBRUSH Brush = CreateSolidBrush(RGB(24, 24, 24));
			Assert(Brush);
			FillRect(Context, &Rect, Brush);
			DeleteObject(Brush);

			WCHAR Text[16];
			int TextLength = StrFormat(Text, L"%u", gCountdownValue);

			SelectObject(Context, gFontCountdown);
			SetTextColor(Context, RGB(255, 255, 255));
			SetBkMode(Context, TRANSPARENT);
			DrawTextW(Context, Text, TextLength, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			EndBufferedPaint(BufferedPaint, TRUE);
		}

		EndPaint(Window, &Paint);
		return 0;
	}

	return DefWindowProcW(Window, Message, WParam, LParam);
}

static int GetPointResize(int X, int Y)
{
	int BorderX = GetSystemMetrics(SM_CXSIZEFRAME);
	int BorderY = GetSystemMetrics(SM_CYSIZEFRAME);

	int X0 = min(gRectSelection[0].x, gRectSelection[1].x);
	int Y0 = min(gRectSelection[0].y, gRectSelection[1].y);
	int X1 = max(gRectSelection[0].x, gRectSelection[1].x);
	int Y1 = max(gRectSelection[0].y, gRectSelection[1].y);

	POINT P = { X, Y };

	RECT TL = { X0 - BorderX, Y0 - BorderY, X0 + BorderX, Y0 + BorderY };
	if (PtInRect(&TL, P)) return WCAP_RESIZE_TL;

	RECT TR = { X1 - BorderX, Y0 - BorderY, X1 + BorderX, Y0 + BorderY };
	if (PtInRect(&TR, P)) return WCAP_RESIZE_TR;

	RECT BL = { X0 - BorderX, Y1 - BorderY, X0 + BorderX, Y1 + BorderY };
	if (PtInRect(&BL, P)) return WCAP_RESIZE_BL;

	RECT BR = { X1 - BorderX, Y1 - BorderY, X1 + BorderX, Y1 + BorderY };
	if (PtInRect(&BR, P)) return WCAP_RESIZE_BR;

	RECT T = { X0, Y0 - BorderY, X1, Y0 + BorderY };
	if (PtInRect(&T, P)) return WCAP_RESIZE_T;

	RECT B = { X0, Y1 - BorderY, X1, Y1 + BorderY };
	if (PtInRect(&B, P)) return WCAP_RESIZE_B;

	RECT L = { X0 - BorderX, Y0, X0 + BorderX, Y1 };
	if (PtInRect(&L, P)) return WCAP_RESIZE_L;

	RECT R = { X1 - BorderX, Y0, X1 + BorderX, Y1 };
	if (PtInRect(&R, P)) return WCAP_RESIZE_R;

	RECT M = { X0, Y0, X1, Y1 };
	if (PtInRect(&M, P)) return WCAP_RESIZE_M;

	return WCAP_RESIZE_NONE;
}

void DisableHotKeys(void)
{
	UnregisterHotKey(gWindow, HOT_RECORD_MONITOR);
	UnregisterHotKey(gWindow, HOT_RECORD_WINDOW);
	UnregisterHotKey(gWindow, HOT_RECORD_REGION);
}

BOOL EnableHotKeys(void)
{
	BOOL Success = TRUE;
	if (gConfig.ShortcutMonitor)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_MONITOR, HOT_GET_MOD(gConfig.ShortcutMonitor), HOT_GET_KEY(gConfig.ShortcutMonitor));
	}
	if (gConfig.ShortcutWindow)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_WINDOW, HOT_GET_MOD(gConfig.ShortcutWindow), HOT_GET_KEY(gConfig.ShortcutWindow));
	}
	if (gConfig.ShortcutRegion)
	{
		Success = Success && RegisterHotKey(gWindow, HOT_RECORD_REGION, HOT_GET_MOD(gConfig.ShortcutRegion), HOT_GET_KEY(gConfig.ShortcutRegion));
	}
	return Success;
}

static void AdjustRectSizeMultipleOf2(int Adjust, int Ref)
{
	int W = gRectSelection[Ref].x - gRectSelection[Adjust].x;
	W = (W + (W > 0)) & ~1;
	gRectSelection[Adjust].x = gRectSelection[Ref].x - W;

	int H = gRectSelection[Ref].y - gRectSelection[Adjust].y;
	H = (H + (H > 0)) & ~1;
	gRectSelection[Adjust].y = gRectSelection[Ref].y - H;
}

static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if (Message == WM_CREATE)
	{
		HR(BufferedPaintInit());
		AddTrayIcon(Window);
		return 0;
	}
	else if (Message == WM_DESTROY)
	{
		CountdownStop();
		if (gRecording)
		{
			StopRecording();
		}
		RemoveTrayIcon(Window);
		PostQuitMessage(0);
		return 0;
	}
	else if (Message == WM_CLOSE)
	{
		if (gRectContext)
		{
			CaptureRegionDone();
		}
		return 0;
	}
	else if (Message == WM_ACTIVATEAPP)
	{
		if (gRectContext)
		{
			if (WParam == FALSE)
			{
				CaptureRegionDone();
				return 0;
			}
		}
	}
	else if (Message == WM_KEYDOWN)
	{
		if (gRectContext)
		{
			if (WParam == VK_ESCAPE)
			{
				CaptureRegionDone();
				return 0;
			}
			else if (WParam == VK_RETURN)
			{
				if (gRectSelected)
				{
					MONITORINFO Info = { .cbSize = sizeof(Info) };
					GetMonitorInfoW(gRectMonitor, &Info);

					RECT Area =
					{
						.left   = Info.rcMonitor.left + min(gRectSelection[0].x, gRectSelection[1].x),
						.top    = Info.rcMonitor.top  + min(gRectSelection[0].y, gRectSelection[1].y),
						.right  = Info.rcMonitor.left + max(gRectSelection[0].x, gRectSelection[1].x),
						.bottom = Info.rcMonitor.top  + max(gRectSelection[0].y, gRectSelection[1].y),
					};

					// hide selection overlay, selected rectangle stays in gRectSelection for CaptureRegion
					CaptureRegionDone();
					CountdownStart(HOT_RECORD_REGION, &Area);
				}
				return 0;
			}
		}
	}
	else if (Message == WM_LBUTTONDOWN)
	{
		if (gRectContext)
		{
			if (gRectSetSize[0])
			{
				gRectSetSizeClick = TRUE;
				gRectSelection[1].x = gRectSelection[0].x + gRectSetSize[0];
				gRectSelection[1].y = gRectSelection[0].y + gRectSetSize[1];
				InvalidateRect(Window, NULL, FALSE);
			}
			else
			{
				int X = GET_X_LPARAM(LParam);
				int Y = GET_Y_LPARAM(LParam);

				int Resize = gRectSelected ? GetPointResize(X, Y) : WCAP_RESIZE_NONE;
				if (Resize == WCAP_RESIZE_NONE)
				{
					// inital rectangle will be empty
					gRectSelection[0].x = gRectSelection[1].x = X;
					gRectSelection[0].y = gRectSelection[1].y = Y;
					gRectSelected = FALSE;

					InvalidateRect(Window, NULL, FALSE);
				}
				else
				{
					// resizing direction
					gRectMousePos = (POINT){ X, Y };
				}

				gRectResize = Resize;
				SetCapture(Window);
			}
			return 0;
		}
	}
	else if (Message == WM_LBUTTONUP)
	{
		if (gRectContext)
		{
			if (gRectSetSizeClick)
			{
				gRectSetSizeClick = FALSE;
			}
			else
			{
				if (gRectSelected)
				{
					// fix the selected rectangle coordinates, so next resizing starts on the correct side
					int X0 = min(gRectSelection[0].x, gRectSelection[1].x);
					int Y0 = min(gRectSelection[0].y, gRectSelection[1].y);
					int X1 = max(gRectSelection[0].x, gRectSelection[1].x);
					int Y1 = max(gRectSelection[0].y, gRectSelection[1].y);
					gRectSelection[0] = (POINT){ X0, Y0 };
					gRectSelection[1] = (POINT){ X1, Y1 };
				}
				ReleaseCapture();
			}
			return 0;
		}
	}
	else if (Message == WM_MOUSEMOVE)
	{
		if (gRectContext)
		{
			int X = GET_X_LPARAM(LParam);
			int Y = GET_Y_LPARAM(LParam);

			if (gRectSetSize[0])
			{
				SetCursor(gCursorClick);
				InvalidateRect(Window, NULL, FALSE);
			}
			else if (gRectSetSizeClick)
			{
				InvalidateRect(Window, NULL, FALSE);
			}
			else if (WParam & MK_LBUTTON)
			{
				BOOL Update = FALSE;

				if (gRectResize == WCAP_RESIZE_TL || gRectResize == WCAP_RESIZE_L || gRectResize == WCAP_RESIZE_BL)
				{
					// left moved
					gRectSelection[0].x = X;
					AdjustRectSizeMultipleOf2(0, 1);
					Update = TRUE;
				}
				else if (gRectResize == WCAP_RESIZE_TR || gRectResize == WCAP_RESIZE_R || gRectResize == WCAP_RESIZE_BR)
				{
					// right moved
					gRectSelection[1].x = X;
					AdjustRectSizeMultipleOf2(1, 0);
					Update = TRUE;
				}

				if (gRectResize == WCAP_RESIZE_TL || gRectResize == WCAP_RESIZE_T || gRectResize == WCAP_RESIZE_TR)
				{
					// top moved
					gRectSelection[0].y = Y;
					AdjustRectSizeMultipleOf2(0, 1);
					Update = TRUE;
				}
				else if (gRectResize == WCAP_RESIZE_BL || gRectResize == WCAP_RESIZE_B || gRectResize == WCAP_RESIZE_BR)
				{
					// bottom moved
					gRectSelection[1].y = Y;
					AdjustRectSizeMultipleOf2(1, 0);
					Update = TRUE;
				}

				if (gRectResize == WCAP_RESIZE_M)
				{
					// if moving whole rectangle update both
					int DX = X - gRectMousePos.x;
					int DY = Y - gRectMousePos.y;
					gRectMousePos = (POINT){ X, Y };

					gRectSelection[0].x += DX;
					gRectSelection[0].y += DY;
					gRectSelection[1].x += DX;
					gRectSelection[1].y += DY;

					Update = TRUE;
				}
				else if (gRectResize == WCAP_RESIZE_NONE)
				{
					// no resize means we're selecting initial rectangle
					gRectSelection[1].x = X;
					gRectSelection[1].y = Y;
					AdjustRectSizeMultipleOf2(1, 0);
					if (gRectSelection[0].x != gRectSelection[1].x && gRectSelection[0].y != gRectSelection[1].y)
					{
						// when we have non-zero size rectangle, we're good with initial stage
						gRectSelected = TRUE;
						Update = TRUE;
					}
				}

				if (Update)
				{
					InvalidateRect(Window, NULL, FALSE);
				}
			}
			else
			{
				int Resize = gRectSelected ? GetPointResize(X, Y) : WCAP_RESIZE_NONE;
				SetCursor(gCursorResize[Resize]);

				if (Resize == WCAP_RESIZE_NONE)
				{
					// in case hovering over resize text
					InvalidateRect(Window, NULL, FALSE);
				}
			}

			return 0;
		}
	}
	else if (Message == WM_TIMER)
	{
		if (WParam == WCAP_COUNTDOWN_TIMER)
		{
			if (gCountdownValue > 1)
			{
				gCountdownValue--;
				InvalidateRect(gCountdownWindow, NULL, FALSE);
			}
			else
			{
				CountdownFinish();
			}
			return 0;
		}
#if defined(WCAP_CLI)
		if (WParam == WCAP_CLI_DURATION_TIMER)
		{
			if (gRecording)
			{
				StopRecording();
			}
			return 0;
		}
#endif
		if (gRecording)
		{
			if (WParam == WCAP_AUDIO_CAPTURE_TIMER)
			{
				EncodeCapturedAudio();
				return 0;
			}
			else if (WParam == WCAP_VIDEO_UPDATE_TIMER)
			{
				LARGE_INTEGER Time;
				QueryPerformanceCounter(&Time);
#if defined(WCAP_CLI)
				if (!gCli.FramesOnly)
#endif
				Encoder_Update(&gEncoder, Time.QuadPart, gTickFreq.QuadPart);
				return 0;
			}
		}
	}
	else if (Message == WM_POWERBROADCAST)
	{
		if (WParam == PBT_APMQUERYSUSPEND)
		{
			if (gRecording)
			{
				if (LParam & 1)
				{
					// reject request to suspend when recording
					return BROADCAST_QUERY_DENY;
				}
				else
				{
					// if cannot prevent suspend, need to stop recording
					StopRecording();
				}
			}
			else
			{
				// allow to suspend when not recording
			}
		}
		return TRUE;
	}
	else if (Message == WM_WCAP_COMMAND)
	{
		if (LOWORD(LParam) == WM_RBUTTONUP)
		{
			HMENU Menu = CreatePopupMenu();
			Assert(Menu);

			AppendMenuW(Menu, MF_STRING, CMD_WCAP, WCAP_TITLE);
			AppendMenuW(Menu, MF_SEPARATOR, 0, NULL);
			AppendMenuW(Menu, MF_STRING | (gRecording ? MF_DISABLED : 0), CMD_SETTINGS, L"Settings");
			AppendMenuW(Menu, MF_STRING, CMD_QUIT, L"Exit");

			POINT Mouse;
			GetCursorPos(&Mouse);

			SetForegroundWindow(Window);
			int Command = TrackPopupMenu(Menu, TPM_RETURNCMD | TPM_NONOTIFY, Mouse.x, Mouse.y, 0, Window, NULL);
			if (Command == CMD_WCAP)
			{
				ShellExecuteW(NULL, L"open", WCAP_URL, NULL, NULL, SW_SHOWNORMAL);
			}
			else if (Command == CMD_QUIT)
			{
				DestroyWindow(Window);
			}
			else if (Command == CMD_SETTINGS)
			{
				if (Config_ShowDialog(&gConfig))
				{
					Config_Save(&gConfig, gConfigPath);
					DisableHotKeys();
					EnableHotKeys();
				}
			}

			DestroyMenu(Menu);
		}
		else if (LOWORD(LParam) == WM_LBUTTONDBLCLK)
		{
			if (!gRecording)
			{
				if (Config_ShowDialog(&gConfig))
				{
					Config_Save(&gConfig, gConfigPath);
					DisableHotKeys();
					EnableHotKeys();
				}
			}
		}
		else if (LOWORD(LParam) == NIN_BALLOONUSERCLICK)
		{
			// TODO: no idea how to prevent this happening for right-click on tray icon...
			ShowFileInFolder(gRecordingPath);
		}
		return 0;
	}
	else if (Message == WM_HOTKEY)
	{
		if (gRecording)
		{
			StopRecording();
		}
		else if (gCountdownValue)
		{
			// any shortcut during countdown cancels it
			CountdownStop();
		}
		else if (!gRecordingStarted)
		{
			if (gRectContext == NULL)
			{
				if (WParam == HOT_RECORD_WINDOW)
				{
					HWND Target = GetCaptureWindow(GetForegroundWindow());
					if (Target)
					{
						RECT Area;
						GetWindowRect(Target, &Area);
						gCountdownTargetWindow = Target;
						CountdownStart(HOT_RECORD_WINDOW, &Area);
					}
				}
				else if (WParam == HOT_RECORD_MONITOR)
				{
					HMONITOR Target = GetCaptureMonitor();
					if (Target)
					{
						MONITORINFO Info = { .cbSize = sizeof(Info) };
						GetMonitorInfoW(Target, &Info);
						gCountdownTargetMonitor = Target;
						CountdownStart(HOT_RECORD_MONITOR, &Info.rcMonitor);
					}
				}
				else if (WParam == HOT_RECORD_REGION)
				{
					gRecordingStarted = TRUE;
					CaptureRegionInit();
					gRecordingStarted = FALSE;
				}
			}
		}
		return 0;
	}
	else if (Message == WM_WCAP_TRAY_TITLE)
	{
#if defined(WCAP_CLI)
		// no tray icon in cli, also there is no encoder when recording png sequence
		if (FALSE)
#else
		if (gRecording)
#endif
		{
			UINT64 FileSize;
			DWORD Bitrate, LengthMsec;
			Encoder_GetStats(&gEncoder, &Bitrate, &LengthMsec, &FileSize);

			WCHAR LengthText[128];
			StrFromTimeIntervalW(LengthText, _countof(LengthText), LengthMsec, 6);

			WCHAR SizeText[128];
			StrFormatByteSizeW(FileSize, SizeText, _countof(SizeText));

			WCHAR Text[1024];
			StrFormat(Text, L"Recording: %dx%d @ %.2f\nLength: %ls\nBitrate: %u kbit/s\nSize: %ls\nFramedrop: %u",
				gEncoder.OutputWidth, gEncoder.OutputHeight,
				(float)gEncoder.FramerateNum / (float)gEncoder.FramerateDen,
				LengthText,
				Bitrate,
				SizeText,
				gRecordingDroppedFrames);

			UpdateTrayTitle(Text);
		}
		return 0;
	}
	else if (Message == WM_WCAP_STOP_CAPTURE)
	{
		if (gRecording)
		{
			StopRecording();
		}
		return 0;
	}
	else if (Message == WM_WCAP_ALREADY_RUNNING)
	{
		ShowNotification(L"wcap is already running!", NULL, NIIF_INFO);
		return 0;
	}
	else if (Message == WM_TASKBARCREATED)
	{
		// in case taskbar was re-created (explorer.exe crashed) add our icon back
		AddTrayIcon(Window);
		return 0;
	}
	else if (Message == WM_ERASEBKGND)
	{
		return 1;
	}
	else if (Message == WM_PAINT)
	{
		PAINTSTRUCT Paint;
		HDC PaintContext = BeginPaint(Window, &Paint);

		HDC Context;
		HPAINTBUFFER BufferedPaint = BeginBufferedPaint(PaintContext, &Paint.rcPaint, BPBF_COMPATIBLEBITMAP, NULL, &Context);
		if (BufferedPaint)
		{
			if (gRectContext)
			{
				{
					int X = Paint.rcPaint.left;
					int Y = Paint.rcPaint.top;
					int W = Paint.rcPaint.right - Paint.rcPaint.left;
					int H = Paint.rcPaint.bottom - Paint.rcPaint.top;

					// draw darkened screenshot
					BitBlt(Context, X, Y, W, H, gRectDarkContext, X, Y, SRCCOPY);
				}

				if (gRectSelected)
				{
					// draw selected rectangle
					int X0 = min(gRectSelection[0].x, gRectSelection[1].x);
					int Y0 = min(gRectSelection[0].y, gRectSelection[1].y);
					int X1 = max(gRectSelection[0].x, gRectSelection[1].x);
					int Y1 = max(gRectSelection[0].y, gRectSelection[1].y);
					BitBlt(Context, X0, Y0, X1 - X0, Y1 - Y0, gRectContext, X0, Y0, SRCCOPY);

					RECT Rect = { X0 - 1, Y0 - 1, X1 + 1, Y1 + 1 };
					FrameRect(Context, &Rect, GetStockObject(WHITE_BRUSH));

					WCHAR Text[128];
					int TextLength = StrFormat(Text, L"%d x %d", X1 - X0, Y1 - Y0);

					SelectObject(Context, gFontBold);
					SetTextAlign(Context, TA_TOP | TA_RIGHT);
					SetTextColor(Context, RGB(255, 255, 255));
					SetBkMode(Context, TRANSPARENT);
					ExtTextOutW(Context, X1, Y1, 0, NULL, Text, TextLength, NULL);

					SelectObject(Context, gFontBold);
					SetTextAlign(Context, TA_BOTTOM | TA_LEFT);
					SetTextColor(Context, RGB(255, 255, 255));

					const WCHAR TextResize[] = L"Resize:  ";

					SIZE Size;
					GetTextExtentPoint32W(Context, TextResize, _countof(TextResize) - 1, &Size);
					ExtTextOutW(Context, X0, Y0, 0, NULL, TextResize, _countof(TextResize) - 1, NULL);

					int X = X0;
					SelectObject(Context, gFont);

					POINT CursorPos;
					GetCursorPos(&CursorPos);
					ScreenToClient(Window, &CursorPos);

					gRectSetSize[0] = gRectSetSize[1] = 0;

					int Sizes[][2] = { { 800, 600 }, { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 } };
					for (int i=0; i<_countof(Sizes); i++)
					{
						X += Size.cx;

						TextLength = StrFormat(Text, L"%dx%d  ", Sizes[i][0], Sizes[i][1]);
						GetTextExtentPoint32W(Context, Text, TextLength, &Size);

						RECT Rect = { X, Y0 - Size.cy, X + Size.cx, Y0 };
						BOOL Hovering = PtInRect(&Rect, CursorPos);
						SetTextColor(Context, Hovering ? RGB(255, 255, 255) : RGB(192, 192, 192));
						ExtTextOutW(Context, X, Y0, 0, NULL, Text, TextLength, NULL);

						if (Hovering)
						{
							gRectSetSize[0] = Sizes[i][0];
							gRectSetSize[1] = Sizes[i][1];
							SetCursor(gCursorClick);
						}
					}
				}
				else
				{
					// draw initial message when no rectangle is selected
					SelectObject(Context, gFont);
					SelectObject(Context, GetStockObject(DC_PEN));
					SelectObject(Context, GetStockObject(DC_BRUSH));

					const WCHAR Line1[] = L"Select region with the mouse and press ENTER to start capture.";
					const WCHAR Line2[] = L"Press ESC to cancel.";

					const WCHAR* Lines[] = { Line1, Line2 };
					const int LineLengths[] = { _countof(Line1) - 1, _countof(Line2) - 1 };
					int Widths[_countof(Lines)];
					int Height;

					int TotalWidth = 0;
					int TotalHeight = 0;
					for (int i = 0; i < _countof(Lines); i++)
					{
						SIZE Size;
						GetTextExtentPoint32W(Context, Lines[i], LineLengths[i], &Size);
						Widths[i] = Size.cx;
						Height = Size.cy;
						TotalWidth = max(TotalWidth, Size.cx);
						TotalHeight += Size.cy;
					}
					TotalWidth += 2 * Height;
					TotalHeight += Height;

					int MsgX = (gRectWidth - TotalWidth) / 2;
					int MsgY = (gRectHeight - TotalHeight) / 2;

					SetDCPenColor(Context, RGB(255, 255, 255));
					SetDCBrushColor(Context, RGB(0, 0, 128));
					Rectangle(Context, MsgX, MsgY, MsgX + TotalWidth, MsgY + TotalHeight);

					SetTextAlign(Context, TA_TOP | TA_CENTER);
					SetTextColor(Context, RGB(255, 255, 0));
					SetBkMode(Context, TRANSPARENT);
					int Y = MsgY + Height / 2;
					int X = gRectWidth / 2;
					for (int i = 0; i < _countof(Lines); i++)
					{
						ExtTextOutW(Context, X, Y, 0, NULL, Lines[i], LineLengths[i], NULL);
						Y += Height;
					}
				}
			}
			else
			{
				RECT Rect;
				GetClientRect(Window, &Rect);

				HBRUSH BorderBrush = CreateSolidBrush(RGB(255, 255, 0));
				Assert(BorderBrush);
				FillRect(Context, &Rect, BorderBrush);
				DeleteObject(BorderBrush);

				Rect.left += WCAP_RECT_BORDER;
				Rect.top += WCAP_RECT_BORDER;
				Rect.right -= WCAP_RECT_BORDER;
				Rect.bottom -= WCAP_RECT_BORDER;

				HBRUSH ColorKeyBrush = CreateSolidBrush(RGB(255, 0, 255));
				Assert(ColorKeyBrush);
				FillRect(Context, &Rect, ColorKeyBrush);
				DeleteObject(ColorKeyBrush);

				FrameRect(Context, &Rect, GetStockObject(BLACK_BRUSH));
			}

			EndBufferedPaint(BufferedPaint, TRUE);
		}

		EndPaint(Window, &Paint);
		return 0;
	}

	return DefWindowProcW(Window, Message, WParam, LParam);
}

static bool OnCaptureFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame)
{
	if (Frame == NULL)
	{
		PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
		return true;
	}

	BOOL DoEncode = TRUE;
	DWORD LimitFramerate = gRecordingLimitFramerate;
	if (LimitFramerate != 0)
	{
		if (Frame->Time * LimitFramerate < gRecordingNextEncode)
		{
			DoEncode = FALSE;
#if defined(WCAP_CLI)
			gCli.LimitedByFps++;
#endif
		}
		else
		{
			if (gRecordingNextEncode == 0)
			{
				gRecordingNextEncode = Frame->Time * LimitFramerate;
			}
			gRecordingNextEncode += gTickFreq.QuadPart;
		}
	}

	// ignore frames if it comes from the past
	if (Frame->Time <= gRecordingLastFrame)
	{
		DoEncode = FALSE;
	}
	gRecordingLastFrame = Frame->Time;

#if defined(WCAP_CLI)
	if (DoEncode)
	{
		// duplicate detection, png output, frame log
		DoEncode = CliOnFrame(Frame);
	}
#endif

	if (DoEncode)
	{
		if (!Encoder_NewFrame(&gEncoder, Frame->Texture, Frame->Rect, Frame->Time, gTickFreq.QuadPart))
		{
			// TODO: maybe highlight tray icon when droppped frames are increasing too much?
			gRecordingDroppedFrames++;
#if defined(WCAP_CLI)
			CliFrameDropped();
#endif
		}
#if defined(WCAP_CLI)
		else
		{
			CliFrameEncoded();
		}
#endif
	}

	if (gConfig.EnableLimitLength || gConfig.EnableLimitSize)
	{
		BOOL Stop = FALSE;

		if (gConfig.EnableLimitLength)
		{
#if defined(WCAP_CLI)
			UINT64 StartTime = gCli.FramesOnly ? gCli.FirstQpc : gEncoder.StartTime;
#else
			UINT64 StartTime = gEncoder.StartTime;
#endif
			if (Frame->Time - StartTime >= (UINT64)(gConfig.LimitLength * gTickFreq.QuadPart))
			{
				Stop = TRUE;
			}
		}
		if (gConfig.EnableLimitSize && !Stop)
		{
			UINT64 FileSize;
			DWORD Bitrate, LengthMsec;
			Encoder_GetStats(&gEncoder, &Bitrate, &LengthMsec, &FileSize);

			// reserve 0.5% for mp4 format overhead (probably an overestimate)
			if (1000 * FileSize >= (995ULL * gConfig.LimitSize) << 20)
			{
				Stop = TRUE;
			}
		}

		if (Stop)
		{
			PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
			return true;
		}
	}

	// update tray title with stats once every second
	if (gRecordingNextTooltip == 0)
	{
		gRecordingNextTooltip = Frame->Time + gTickFreq.QuadPart;
	}
	else if (Frame->Time >= gRecordingNextTooltip)
	{
		gRecordingNextTooltip += gTickFreq.QuadPart;

		// do the update, but not from frame callback to minimize time when texture is used
		PostMessageW(gWindow, WM_WCAP_TRAY_TITLE, 0, 0);
	}

	return true;
}

#if !defined(WCAP_CLI)

#ifndef NDEBUG
int WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdline, int cmdshow)
#else
void WinMainCRTStartup()
#endif
{
	WNDCLASSEXW WindowClass =
	{
		.cbSize = sizeof(WindowClass),
		.lpfnWndProc = WindowProc,
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = L"wcap_window_class",
	};

	HWND Existing = FindWindowW(WindowClass.lpszClassName, NULL);
	if (Existing)
	{
		PostMessageW(Existing, WM_WCAP_ALREADY_RUNNING, 0, 0);
		ExitProcess(0);
	}

	if (!ScreenCapture_IsSupported())
	{
		MessageBoxW(NULL, L"Windows 10 Version 1903, May 2019 Update (19H1) or newer is required!", WCAP_TITLE, MB_ICONEXCLAMATION);
		ExitProcess(0);
	}

	GetModuleFileNameW(NULL, gConfigPath, _countof(gConfigPath));
	PathRenameExtensionW(gConfigPath, L".ini");

	HR(CoInitializeEx(0, COINIT_APARTMENTTHREADED));

	Config_Defaults(&gConfig);
	Config_Load(&gConfig, gConfigPath);
	ScreenCapture_Create(&gCapture, &OnCaptureFrame, false);
	Encoder_Init(&gEncoder);

	QueryPerformanceFrequency(&gTickFreq);

	gCursorArrow = LoadCursor(NULL, IDC_ARROW);
	gCursorClick = LoadCursor(NULL, IDC_HAND);
	gCursorResize[WCAP_RESIZE_NONE] = LoadCursor(NULL, IDC_CROSS);
	gCursorResize[WCAP_RESIZE_M]    = LoadCursor(NULL, IDC_SIZEALL);
	gCursorResize[WCAP_RESIZE_T]    = gCursorResize[WCAP_RESIZE_B]  = LoadCursor(NULL, IDC_SIZENS);
	gCursorResize[WCAP_RESIZE_L]    = gCursorResize[WCAP_RESIZE_R]  = LoadCursor(NULL, IDC_SIZEWE);
	gCursorResize[WCAP_RESIZE_TL]   = gCursorResize[WCAP_RESIZE_BR] = LoadCursor(NULL, IDC_SIZENWSE);
	gCursorResize[WCAP_RESIZE_TR]   = gCursorResize[WCAP_RESIZE_BL] = LoadCursor(NULL, IDC_SIZENESW);

	gFont = CreateFontW(-WCAP_UI_FONT_SIZE, 0, 0, 0, FW_NORMAL,
		FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, WCAP_UI_FONT);
	Assert(gFont);

	gFontBold = CreateFontW(-WCAP_UI_FONT_SIZE, 0, 0, 0, FW_BOLD,
		FALSE, FALSE, FALSE,DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, WCAP_UI_FONT);
	Assert(gFontBold);

	gFontCountdown = CreateFontW(-WCAP_COUNTDOWN_FONT_SIZE, 0, 0, 0, FW_BOLD,
		FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH, WCAP_UI_FONT);
	Assert(gFontCountdown);

	gIcon1 = LoadIconW(WindowClass.hInstance, MAKEINTRESOURCEW(1));
	gIcon2 = LoadIconW(WindowClass.hInstance, MAKEINTRESOURCEW(2));
	Assert(gIcon1 && gIcon2);

	WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
	Assert(WM_TASKBARCREATED);

	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	gWindow = CreateWindowExW(
		0, WindowClass.lpszClassName, WCAP_TITLE, WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, NULL, WindowClass.hInstance, NULL);
	if (!gWindow)
	{
		ExitProcess(0);
	}

	WNDCLASSEXW CountdownClass =
	{
		.cbSize = sizeof(CountdownClass),
		.lpfnWndProc = CountdownWindowProc,
		.hInstance = WindowClass.hInstance,
		.lpszClassName = L"wcap_countdown_class",
	};
	if (RegisterClassExW(&CountdownClass))
	{
		gCountdownWindow = CreateWindowExW(
			WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			CountdownClass.lpszClassName, WCAP_TITLE, WS_POPUP,
			0, 0, WCAP_COUNTDOWN_SIZE, WCAP_COUNTDOWN_SIZE,
			NULL, NULL, WindowClass.hInstance, NULL);
		if (gCountdownWindow)
		{
			SetLayeredWindowAttributes(gCountdownWindow, 0, 224, LWA_ALPHA);
			SetWindowRgn(gCountdownWindow, CreateRoundRectRgn(0, 0, WCAP_COUNTDOWN_SIZE + 1, WCAP_COUNTDOWN_SIZE + 1, 48, 48), FALSE);
			// never let countdown itself end up in recording
			SetWindowDisplayAffinity(gCountdownWindow, WDA_EXCLUDEFROMCAPTURE);
		}
	}

	if (!EnableHotKeys())
	{
		MessageBoxW(NULL,
			L"Cannot register wcap keyboard shortcuts.\nSome other application might already use shorcuts.\nPlease check & adjust the settings!",
			WCAP_TITLE, MB_ICONEXCLAMATION);
	}

	for (;;)
	{
		MSG Message;
		BOOL Result = GetMessageW(&Message, NULL, 0, 0);
		if (Result == 0)
		{
			ExitProcess(0);
		}
		Assert(Result > 0);

		TranslateMessage(&Message);
		DispatchMessageW(&Message);
	}
}

#else // WCAP_CLI

//
// command line interface, records immediately (no countdown), no tray icon or hotkeys
//

#define WCAP_CLI_STOP_EVENT  L"Local\\wcap-cli-stop"
#define WCAP_CLI_START_EVENT L"Local\\wcap-cli-start-"

static HANDLE gCliDoneEvent;

static void CliPrint(DWORD StdHandle, LPCWSTR Format, ...)
{
	WCHAR Text[8192];
	va_list Args;
	va_start(Args, Format);
	_vsnwprintf(Text, _countof(Text), Format, Args);
	va_end(Args);
	Text[_countof(Text) - 1] = 0;
	WriteText(StdHandle, Text);
}

static void CliUsage(void)
{
	CliPrint(STD_OUTPUT_HANDLE,
		L"wcap-cli - screen recording from command line (records immediately, no countdown)\n"
		L"\n"
		L"usage:\n"
		L"  wcap-cli list [--json] [--filter TEXT]   list monitors and capturable windows\n"
		L"  wcap-cli record [target] [options]       record until --duration expires, Ctrl+C or 'wcap-cli stop'\n"
		L"  wcap-cli stop                            stop all running 'wcap-cli record' processes\n"
		L"  wcap-cli signal NAME                     start a recording that waits with '--start-on NAME'\n"
		L"  wcap-cli snapshot [target] -o FILE.png   save one frame as png\n"
		L"  wcap-cli diff A.mp4 [B.mp4] [--region X,Y,W,H]...   frame to frame difference of clips, in numbers\n"
		L"  wcap-cli sheet CLIP.mp4... [--at T,T,...]           contact sheet png with frames at given times\n"
		L"\n"
		L"target (default is primary monitor):\n"
		L"  --monitor N          monitor index from 'list'\n"
		L"  --window W           window handle (0x...) or case-insensitive title substring\n"
		L"  --region X,Y,W,H     rectangle in virtual screen coordinates (must be on one monitor)\n"
		L"  --crop X,Y,W,H       crop of captured area, relative to top-left of window (or of monitor/region)\n"
		L"\n"
		L"record options (defaults come from wcap-cli .ini file next to exe):\n"
		L"  -o, --output FILE    output .mp4 path (default: <OutputFolder>\\<timestamp>.mp4)\n"
		L"  -d, --duration SEC   stop after SEC seconds\n"
		L"  --fps N              max framerate (0 = monitor refresh rate), 'match' = only new frames, no limit\n"
		L"  --vfr                variable framerate: do not write frames identical to the previous one\n"
		L"  --bitrate KBPS       video bitrate in kbit/s\n"
		L"  --max-width N        max video width, downscale if larger (0 = no limit)\n"
		L"  --max-height N       max video height, downscale if larger (0 = no limit)\n"
		L"  --audio / --no-audio enable or disable audio capture\n"
		L"  --no-cursor          do not capture mouse cursor\n"
		L"  --no-border          do not show yellow recording border (Windows 11)\n"
		L"  --fragmented         fragmented mp4, stays playable if process is killed (H264 only)\n"
		L"  --lossless           no mp4, write lossless png sequence to <output without .mp4>_frames folder\n"
		L"  --frames-dir DIR     like --lossless but to given folder (frame_000000.png, ...)\n"
		L"  --start-on SPEC      create everything, then wait to begin capture: 'event:NAME' (see 'signal') or\n"
		L"                       'file:PATH' (starts when file exists). Without prefix: file if it looks like a path\n"
		L"  --start-timeout SEC  give up waiting for --start-on after SEC seconds (default: wait forever)\n"
		L"  --timestamps         write <clip>.frames.json with capture time of every frame\n"
		L"  --measure-flicker    mean difference between consecutive captured frames (uncompressed), whole frame\n"
		L"  --measure-region R   also measure region X,Y,W,H of the captured frame (before --max-width scaling), can be repeated\n"
		L"  --json               print result as one JSON object on stdout\n"
		L"\n"
		L"other commands: -o FILE, --json for snapshot/diff/sheet\n"
		L"  sheet: --at T,T,..  --every SEC  --count N  --grid  --cols N  --width PX  --region X,Y,W,H  --no-labels\n"
		L"\n"
		L"output (stdout): 'recording: PATH' when started, 'saved: PATH' when finished\n"
		L"exit code: 0 on success, 1 on error\n");
}

static BOOL CliParseNumber(LPCWSTR Text, int* Value)
{
	LONGLONG Result;
	if (!StrToInt64ExW(Text, STIF_SUPPORT_HEX, &Result))
	{
		return FALSE;
	}
	*Value = (int)Result;
	return TRUE;
}

static BOOL CliParseRect(LPCWSTR Text, int Values[4])
{
	for (int i = 0; i < 4; i++)
	{
		int Sign = 1;
		if (*Text == L'-')
		{
			Sign = -1;
			Text++;
		}
		if (*Text < L'0' || *Text > L'9')
		{
			return FALSE;
		}
		int Value = 0;
		while (*Text >= L'0' && *Text <= L'9')
		{
			Value = Value * 10 + (*Text++ - L'0');
		}
		Values[i] = Sign * Value;
		if (i < 3)
		{
			if (*Text != L',')
			{
				return FALSE;
			}
			Text++;
		}
	}
	return *Text == 0;
}

static BOOL CliIsHandle(LPCWSTR Text)
{
	return Text[0] == L'0' && (Text[1] == L'x' || Text[1] == L'X');
}

typedef struct
{
	int Index;
	int Wanted; // -1 to list all
	HMONITOR Result;
	CliText* Json; // when not NULL, listing is done as JSON
}
CliMonitorEnum;

static BOOL CALLBACK CliMonitorProc(HMONITOR Monitor, HDC Context, LPRECT Rect, LPARAM Param)
{
	CliMonitorEnum* Enum = (CliMonitorEnum*)Param;
	if (Enum->Wanted < 0)
	{
		MONITORINFOEXW Info = { .cbSize = sizeof(Info) };
		GetMonitorInfoW(Monitor, (LPMONITORINFO)&Info);
		BOOL Primary = (Info.dwFlags & MONITORINFOF_PRIMARY) != 0;
		if (Enum->Json)
		{
			CliText_Printf(Enum->Json, L"%ls{\"index\":%d,\"width\":%d,\"height\":%d,\"x\":%d,\"y\":%d,\"device\":",
				Enum->Index ? L"," : L"", Enum->Index,
				Info.rcMonitor.right - Info.rcMonitor.left, Info.rcMonitor.bottom - Info.rcMonitor.top,
				Info.rcMonitor.left, Info.rcMonitor.top);
			CliText_Json(Enum->Json, Info.szDevice);
			CliText_Printf(Enum->Json, L",\"primary\":%ls}", Primary ? L"true" : L"false");
		}
		else
		{
			CliPrint(STD_OUTPUT_HANDLE, L"monitor %d: %dx%d at %d,%d %ls%ls\n",
				Enum->Index,
				Info.rcMonitor.right - Info.rcMonitor.left,
				Info.rcMonitor.bottom - Info.rcMonitor.top,
				Info.rcMonitor.left, Info.rcMonitor.top,
				Info.szDevice,
				Primary ? L" (primary)" : L"");
		}
	}
	else if (Enum->Index == Enum->Wanted)
	{
		Enum->Result = Monitor;
		return FALSE;
	}
	Enum->Index++;
	return TRUE;
}

static BOOL CliIsCapturableWindow(HWND Window)
{
	if (!IsWindowVisible(Window) || GetWindowTextLengthW(Window) == 0)
	{
		return FALSE;
	}
	if (GetWindowLongW(Window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
	{
		return FALSE;
	}
	BOOL Cloaked = FALSE;
	if (SUCCEEDED(DwmGetWindowAttribute(Window, DWMWA_CLOAKED, &Cloaked, sizeof(Cloaked))) && Cloaked)
	{
		return FALSE;
	}
	return TRUE;
}

typedef struct
{
	LPCWSTR Title;  // NULL to list all
	LPCWSTR Filter; // when listing, only windows with this text in title
	CliText* Json;  // when not NULL, listing is done as JSON
	HWND Result;
	int Count;
}
CliWindowEnum;

static BOOL CALLBACK CliWindowProc(HWND Window, LPARAM Param)
{
	CliWindowEnum* Enum = (CliWindowEnum*)Param;
	if (!CliIsCapturableWindow(Window))
	{
		return TRUE;
	}

	WCHAR Title[256];
	GetWindowTextW(Window, Title, _countof(Title));

	if (Enum->Title == NULL)
	{
		if (Enum->Filter && !StrStrIW(Title, Enum->Filter))
		{
			return TRUE;
		}

		RECT Rect;
		GetWindowRect(Window, &Rect);
		DWORD ProcessId;
		GetWindowThreadProcessId(Window, &ProcessId);
		if (Enum->Json)
		{
			CliText_Printf(Enum->Json, L"%ls{\"handle\":\"0x%llx\",\"width\":%d,\"height\":%d,\"x\":%d,\"y\":%d,\"pid\":%u,\"minimized\":%ls,\"title\":",
				Enum->Count ? L"," : L"", (UINT64)(ULONG_PTR)Window,
				Rect.right - Rect.left, Rect.bottom - Rect.top, Rect.left, Rect.top, ProcessId,
				IsIconic(Window) ? L"true" : L"false");
			CliText_Json(Enum->Json, Title);
			CliText_Char(Enum->Json, L'}');
		}
		else
		{
			CliPrint(STD_OUTPUT_HANDLE, L"window 0x%llx: %dx%d pid=%u \"%ls\"\n",
				(UINT64)(ULONG_PTR)Window, Rect.right - Rect.left, Rect.bottom - Rect.top, ProcessId, Title);
		}
		Enum->Count++;
	}
	else if (StrStrIW(Title, Enum->Title))
	{
		if (Enum->Count++ == 0)
		{
			Enum->Result = Window;
		}
	}
	return TRUE;
}

static BOOL WINAPI CliCtrlHandler(DWORD Type)
{
	if (gCliAbortEvent)
	{
		SetEvent(gCliAbortEvent);
	}
	PostMessageW(gWindow, WM_WCAP_STOP_CAPTURE, 0, 0);
	if (Type == CTRL_CLOSE_EVENT || Type == CTRL_LOGOFF_EVENT || Type == CTRL_SHUTDOWN_EVENT)
	{
		// process is terminated when handler returns, give encoder time to finalize mp4 file
		WaitForSingleObject(gCliDoneEvent, 5000);
	}
	return TRUE;
}

static int CliList(int ArgCount, LPWSTR* Args)
{
	BOOL Json = FALSE;
	LPCWSTR Filter = NULL;
	for (int i = 0; i < ArgCount; i++)
	{
		if (StrCmpW(Args[i], L"--json") == 0)
		{
			Json = TRUE;
		}
		else if (StrCmpW(Args[i], L"--filter") == 0 && i + 1 < ArgCount)
		{
			Filter = Args[++i];
		}
		else
		{
			CliPrint(STD_ERROR_HANDLE, L"error: unknown argument: %ls\n", Args[i]);
			return 1;
		}
	}

	CliText Monitors = { 0 };
	CliText Windows = { 0 };

	CliMonitorEnum MonitorEnum = { .Wanted = -1, .Json = Json ? &Monitors : NULL };
	EnumDisplayMonitors(NULL, NULL, &CliMonitorProc, (LPARAM)&MonitorEnum);

	CliWindowEnum WindowEnum = { .Filter = Filter, .Json = Json ? &Windows : NULL };
	EnumWindows(&CliWindowProc, (LPARAM)&WindowEnum);

	if (Json)
	{
		CliText Out = { 0 };
		CliText_Printf(&Out, L"{\"monitors\":[");
		// texts can be longer than printf buffer, append them directly
		for (LPCWSTR p = Monitors.Data ? Monitors.Data : L""; *p; p++)
		{
			CliText_Char(&Out, *p);
		}
		CliText_Printf(&Out, L"],\"windows\":[");
		for (LPCWSTR p = Windows.Data ? Windows.Data : L""; *p; p++)
		{
			CliText_Char(&Out, *p);
		}
		CliText_Printf(&Out, L"]}\n");
		CliText_Print(STD_OUTPUT_HANDLE, &Out);
	}
	return 0;
}

static int CliStop(void)
{
	HANDLE Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, WCAP_CLI_STOP_EVENT);
	if (!Event)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: no recording in progress\n");
		return 1;
	}
	// event is manual reset so every recording (also ones still waiting for start signal) sees it,
	// give them a moment to react and then reset it so that recordings started later are not affected
	SetEvent(Event);
	Sleep(300);
	ResetEvent(Event);
	CloseHandle(Event);
	CliPrint(STD_OUTPUT_HANDLE, L"stop requested\n");
	return 0;
}

static void CliStartEventName(LPCWSTR Name, WCHAR* Full, size_t Count)
{
	if (StrCmpNIW(Name, L"Local\\", 6) == 0 || StrCmpNIW(Name, L"Global\\", 7) == 0)
	{
		StrCpyNW(Full, Name, (int)Count);
	}
	else
	{
		StrCpyNW(Full, WCAP_CLI_START_EVENT, (int)Count);
		StrCatBuffW(Full, Name, (int)Count);
	}
}

static int CliSignal(int ArgCount, LPWSTR* Args)
{
	if (ArgCount != 1)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: usage: wcap-cli signal NAME\n");
		return 1;
	}

	WCHAR Full[300];
	CliStartEventName(Args[0], Full, _countof(Full));
	HANDLE Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, Full);
	if (!Event)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: no recording is waiting for '%ls'\n", Args[0]);
		return 1;
	}
	SetEvent(Event);
	CloseHandle(Event);
	CliPrint(STD_OUTPUT_HANDLE, L"signaled: %ls\n", Args[0]);
	return 0;
}

//
// capture target shared by record & snapshot
//

typedef struct
{
	int MonitorIndex; // -1 when not used
	LPCWSTR WindowArg;
	BOOL HasRegion;
	int Region[4];
	BOOL HasCrop;
	int Crop[4];
	LPCWSTR Output;
	BOOL Json;
	BOOL NoCursor;
	BOOL NoBorder;
}
CliTarget;

typedef struct
{
	HWND Window;
	HMONITOR Monitor;
	RECT Rect;
	BOOL UseRect;
}
CliResolved;

#define CLI_NEED_VALUE() do { if (!Next) { CliPrint(STD_ERROR_HANDLE, L"error: %ls requires a value\n", Arg); return -1; } (*Index)++; } while (0)

// returns 1 if argument was handled, 0 if it is not a target option, -1 on error
static int CliParseTargetOption(CliTarget* Target, int* Index, int ArgCount, LPWSTR* Args)
{
	LPCWSTR Arg = Args[*Index];
	LPCWSTR Next = *Index + 1 < ArgCount ? Args[*Index + 1] : NULL;

	if (StrCmpW(Arg, L"--monitor") == 0)
	{
		CLI_NEED_VALUE();
		if (!CliParseNumber(Next, &Target->MonitorIndex) || Target->MonitorIndex < 0)
		{
			CliPrint(STD_ERROR_HANDLE, L"error: invalid value for %ls: %ls\n", Arg, Next);
			return -1;
		}
	}
	else if (StrCmpW(Arg, L"--window") == 0)
	{
		CLI_NEED_VALUE();
		Target->WindowArg = Next;
	}
	else if (StrCmpW(Arg, L"--region") == 0)
	{
		CLI_NEED_VALUE();
		if (!CliParseRect(Next, Target->Region) || Target->Region[2] <= 0 || Target->Region[3] <= 0)
		{
			CliPrint(STD_ERROR_HANDLE, L"error: --region expects X,Y,W,H with positive W and H\n");
			return -1;
		}
		Target->HasRegion = TRUE;
	}
	else if (StrCmpW(Arg, L"--crop") == 0)
	{
		CLI_NEED_VALUE();
		if (!CliParseRect(Next, Target->Crop) || Target->Crop[0] < 0 || Target->Crop[1] < 0 || Target->Crop[2] <= 0 || Target->Crop[3] <= 0)
		{
			CliPrint(STD_ERROR_HANDLE, L"error: --crop expects X,Y,W,H with non-negative X,Y and positive W,H\n");
			return -1;
		}
		Target->HasCrop = TRUE;
	}
	else if (StrCmpW(Arg, L"-o") == 0 || StrCmpW(Arg, L"--output") == 0)
	{
		CLI_NEED_VALUE();
		Target->Output = Next;
	}
	else if (StrCmpW(Arg, L"--json") == 0)      Target->Json = TRUE;
	else if (StrCmpW(Arg, L"--no-cursor") == 0) Target->NoCursor = TRUE;
	else if (StrCmpW(Arg, L"--no-border") == 0) Target->NoBorder = TRUE;
	else
	{
		return 0;
	}
	return 1;
}

// warn about window states where the recording would be empty or not what user expects
static void CliCheckWindowState(HWND Window)
{
	if (IsIconic(Window))
	{
		CliWarn(L"window is minimized, the recording will be empty or frozen (restore the window first)");
		return;
	}

	BOOL Cloaked = FALSE;
	if (SUCCEEDED(DwmGetWindowAttribute(Window, DWMWA_CLOAKED, &Cloaked, sizeof(Cloaked))) && Cloaked)
	{
		CliWarn(L"window is cloaked (hidden, or on another virtual desktop)");
		return;
	}

	RECT Rect;
	if (FAILED(DwmGetWindowAttribute(Window, DWMWA_EXTENDED_FRAME_BOUNDS, &Rect, sizeof(Rect))))
	{
		return;
	}

	LONG W = Rect.right - Rect.left;
	LONG H = Rect.bottom - Rect.top;
	POINT Points[] =
	{
		{ Rect.left + W / 2, Rect.top + H / 2 },
		{ Rect.left + W / 4, Rect.top + H / 4 },
		{ Rect.left + 3 * W / 4, Rect.top + H / 4 },
		{ Rect.left + W / 4, Rect.top + 3 * H / 4 },
		{ Rect.left + 3 * W / 4, Rect.top + 3 * H / 4 },
	};

	int Covered = 0;
	for (int i = 0; i < _countof(Points); i++)
	{
		HWND Hit = WindowFromPoint(Points[i]);
		if (Hit && GetAncestor(Hit, GA_ROOTOWNER) != GetAncestor(Window, GA_ROOTOWNER))
		{
			Covered++;
		}
	}
	if (Covered == _countof(Points))
	{
		CliWarn(L"window is fully covered by other windows");
	}
	else if (Covered > 0)
	{
		CliWarn(L"window is partially covered by other windows");
	}
}

static BOOL CliResolveTarget(const CliTarget* Target, BOOL RoundEven, CliResolved* Result)
{
	*Result = (CliResolved){ 0 };

	if ((Target->MonitorIndex >= 0) + (Target->WindowArg != NULL) + (Target->HasRegion != FALSE) > 1)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: use only one of --monitor, --window or --region\n");
		return FALSE;
	}

	if (Target->WindowArg)
	{
		LPCWSTR WindowArg = Target->WindowArg;
		HWND Window;
		int Handle;
		if (CliIsHandle(WindowArg) && CliParseNumber(WindowArg, &Handle))
		{
			Window = (HWND)(ULONG_PTR)(UINT)Handle;
			if (!IsWindow(Window))
			{
				CliPrint(STD_ERROR_HANDLE, L"error: window %ls does not exist\n", WindowArg);
				return FALSE;
			}
		}
		else
		{
			CliWindowEnum Enum = { .Title = WindowArg };
			EnumWindows(&CliWindowProc, (LPARAM)&Enum);
			if (Enum.Count == 0)
			{
				CliPrint(STD_ERROR_HANDLE, L"error: no window title contains \"%ls\"\n", WindowArg);
				return FALSE;
			}
			if (Enum.Count > 1)
			{
				CliWarn(L"%d windows match \"%ls\", using first one", Enum.Count, WindowArg);
			}
			Window = Enum.Result;
		}

		Window = GetCaptureWindow(Window);
		if (!Window)
		{
			return FALSE;
		}
		CliCheckWindowState(Window);
		Result->Window = Window;
	}
	else if (Target->HasRegion)
	{
		RECT Rect = { Target->Region[0], Target->Region[1], Target->Region[0] + Target->Region[2], Target->Region[1] + Target->Region[3] };
		Result->Monitor = MonitorFromRect(&Rect, MONITOR_DEFAULTTONULL);
		if (!Result->Monitor)
		{
			CliPrint(STD_ERROR_HANDLE, L"error: region is not on any monitor\n");
			return FALSE;
		}

		MONITORINFO Info = { .cbSize = sizeof(Info) };
		GetMonitorInfoW(Result->Monitor, &Info);

		IntersectRect(&Rect, &Rect, &Info.rcMonitor);
		OffsetRect(&Rect, -Info.rcMonitor.left, -Info.rcMonitor.top);

		if (RoundEven)
		{
			// video encoder needs even sizes
			Rect.right = Rect.left + ((Rect.right - Rect.left) & ~1);
			Rect.bottom = Rect.top + ((Rect.bottom - Rect.top) & ~1);
		}
		if (IsRectEmpty(&Rect))
		{
			CliPrint(STD_ERROR_HANDLE, L"error: region is too small\n");
			return FALSE;
		}

		Result->Rect = Rect;
		Result->UseRect = TRUE;
	}
	else if (Target->MonitorIndex >= 0)
	{
		CliMonitorEnum Enum = { .Wanted = Target->MonitorIndex };
		EnumDisplayMonitors(NULL, NULL, &CliMonitorProc, (LPARAM)&Enum);
		if (!Enum.Result)
		{
			CliPrint(STD_ERROR_HANDLE, L"error: monitor %d does not exist, see 'wcap-cli list'\n", Target->MonitorIndex);
			return FALSE;
		}
		Result->Monitor = Enum.Result;
	}
	else
	{
		Result->Monitor = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	}
	return TRUE;
}

static void CliSetCrop(const CliTarget* Target, BOOL RoundEven)
{
	if (Target->HasCrop)
	{
		int W = Target->Crop[2];
		int H = Target->Crop[3];
		if (RoundEven)
		{
			W = max(2, W & ~1);
			H = max(2, H & ~1);
		}
		gCapture.HasCrop = true;
		gCapture.Crop = (RECT){ Target->Crop[0], Target->Crop[1], Target->Crop[0] + W, Target->Crop[1] + H };
	}
}

static BOOL CliInitConfig(void)
{
	if (!ScreenCapture_IsSupported())
	{
		CliPrint(STD_ERROR_HANDLE, L"error: Windows 10 Version 1903 or newer is required\n");
		return FALSE;
	}

	GetModuleFileNameW(NULL, gConfigPath, _countof(gConfigPath));
	PathRenameExtensionW(gConfigPath, L".ini");

	HR(CoInitializeEx(0, COINIT_APARTMENTTHREADED));

	Config_Defaults(&gConfig);
	Config_Load(&gConfig, gConfigPath);
	gConfig.OpenFolder = FALSE;
	return TRUE;
}

//
// waiting for start signal
//

static BOOL CliWaitStart(void)
{
	if (!gCli.StartOn[0])
	{
		return TRUE;
	}

	LPCWSTR Spec = gCli.StartOn;
	LPCWSTR Name;
	BOOL IsFile;
	if (StrCmpNIW(Spec, L"file:", 5) == 0)
	{
		IsFile = TRUE;
		Name = Spec + 5;
	}
	else if (StrCmpNIW(Spec, L"event:", 6) == 0)
	{
		IsFile = FALSE;
		Name = Spec + 6;
	}
	else
	{
		IsFile = StrPBrkW(Spec, L"\\/.:") != NULL;
		Name = Spec;
	}

	HANDLE Event = NULL;
	if (!IsFile)
	{
		WCHAR Full[300];
		CliStartEventName(Name, Full, _countof(Full));
		Event = CreateEventW(NULL, TRUE, FALSE, Full);
		if (!Event)
		{
			ErrorMessage(L"Cannot create start event!");
			return FALSE;
		}
	}

	CliPrint(gCli.Json ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, L"waiting: %ls %ls\n", IsFile ? L"file" : L"event", Name);

	ULONGLONG Deadline = gCli.StartTimeoutMs ? GetTickCount64() + gCli.StartTimeoutMs : 0;
	BOOL Started = FALSE;
	LPCWSTR Failure = NULL;
	for (;;)
	{
		if (IsFile && GetFileAttributesW(Name) != INVALID_FILE_ATTRIBUTES)
		{
			Started = TRUE;
			break;
		}

		DWORD Timeout = INFINITE;
		if (IsFile)
		{
			Timeout = 2;
		}
		else if (Deadline)
		{
			ULONGLONG Now = GetTickCount64();
			Timeout = Now >= Deadline ? 0 : (DWORD)(Deadline - Now);
		}

		HANDLE Handles[3] = { gCliAbortEvent, gCliStopEvent, Event };
		DWORD Wait = WaitForMultipleObjects(Event ? 3 : 2, Handles, FALSE, Timeout);
		if (Wait == WAIT_OBJECT_0 || Wait == WAIT_OBJECT_0 + 1)
		{
			Failure = L"Stopped while waiting for start signal!";
			break;
		}
		if (Wait == WAIT_OBJECT_0 + 2)
		{
			Started = TRUE;
			break;
		}
		if (Deadline && GetTickCount64() >= Deadline)
		{
			Failure = L"Timed out waiting for start signal!";
			break;
		}
	}

	if (Event)
	{
		CloseHandle(Event);
	}
	if (!Started)
	{
		ErrorMessage(Failure);
	}
	else if (gConfig.CaptureAudio)
	{
		// audio was captured while waiting, throw it away so recording starts with current audio
		atomic_store_explicit(&gAudio.BufferRead, atomic_load_explicit(&gAudio.BufferWrite, memory_order_acquire), memory_order_release);
	}
	return Started;
}

//
// result of recording
//

static void CliWriteTimestamps(LPCWSTR Path, UINT64 Freq)
{
	CliSession* S = &gCli;

	// figure out wall clock time of first frame from current time
	LARGE_INTEGER NowQpc;
	QueryPerformanceCounter(&NowQpc);
	FILETIME NowFile;
	GetSystemTimePreciseAsFileTime(&NowFile);
	UINT64 NowTicks = ((UINT64)NowFile.dwHighDateTime << 32) | NowFile.dwLowDateTime;
	UINT64 StartTicks = NowTicks - (UINT64)MFllMulDiv((LONGLONG)(NowQpc.QuadPart - S->FirstQpc), 10000000, (LONGLONG)Freq, 0);

	FILETIME StartFile = { (DWORD)StartTicks, (DWORD)(StartTicks >> 32) };
	SYSTEMTIME StartTime;
	FileTimeToSystemTime(&StartFile, &StartTime);

	CliText Text = { 0 };
	CliText_Printf(&Text, L"{\"qpc_freq\":%I64u,\"start_qpc\":%I64u,\"start_utc\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\",\"start_unix\":%.6f,",
		Freq, S->FirstQpc, StartTime.wYear, StartTime.wMonth, StartTime.wDay, StartTime.wHour, StartTime.wMinute, StartTime.wSecond, StartTime.wMilliseconds,
		(double)(StartTicks - 116444736000000000ULL) / 10000000.0);
	CliText_Printf(&Text, L"\"width\":%u,\"height\":%u,\"output\":", S->Width, S->Height);
	CliText_Json(&Text, gRecordingPath);
	CliText_Printf(&Text, L",\"frames\":[\n");
	for (size_t i = 0; i < S->LogCount; i++)
	{
		const CliFrameLog* F = &S->Log[i];
		CliText_Printf(&Text, L"%ls{\"i\":%I64u,\"t\":%.6f,\"qpc\":%I64u,\"unique\":%ls,\"diff\":%.4f,\"out\":%d}",
			i ? L",\n" : L"", (UINT64)i, (double)(F->Qpc - S->FirstQpc) / (double)Freq, F->Qpc,
			F->Unique ? L"true" : L"false", F->Diff, F->Out);
	}
	CliText_Printf(&Text, L"\n]}\n");
	if (!CliWriteFileUtf8(Path, Text.Data))
	{
		CliWarn(L"cannot write %ls", Path);
	}
	CliText_Free(&Text);
}

static void CliReportFinished(void)
{
	CliSession* S = &gCli;
	UINT64 Freq = gTickFreq.QuadPart;

	if (S->FramesOnly)
	{
		CliPngQueue_Finish(&S->Png);
	}

	UINT64 FileSize = 0;
	if (S->FramesOnly)
	{
		FileSize = S->Png.Bytes;
	}
	else
	{
		WIN32_FILE_ATTRIBUTE_DATA Attributes;
		if (GetFileAttributesExW(gRecordingPath, GetFileExInfoStandard, &Attributes))
		{
			FileSize = ((UINT64)Attributes.nFileSizeHigh << 32) | Attributes.nFileSizeLow;
		}
	}

	UINT64 Dropped = gRecordingDroppedFrames + S->Png.Dropped + S->Png.Failed;
	UINT64 Unique = S->Considered - S->Duplicated;
	double Duration = S->Considered > 1 ? (double)(S->LastQpc - S->FirstQpc) / (double)Freq : 0;
	double EffectiveFps = Duration > 0 ? (double)Unique / Duration : 0;

	WCHAR TimestampsPath[MAX_PATH] = L"";
	if (S->Timestamps)
	{
		StrCpyW(TimestampsPath, gRecordingPath);
		if (S->FramesOnly)
		{
			PathAppendW(TimestampsPath, L"frames.json");
		}
		else
		{
			PathRenameExtensionW(TimestampsPath, L".frames.json");
		}
		CliWriteTimestamps(TimestampsPath, Freq);
	}

	CliText Out = { 0 };
	if (S->Json)
	{
		CliText_Printf(&Out, L"{\"saved\":");
		CliText_Json(&Out, gRecordingPath);
		CliText_Printf(&Out, L",\"size_bytes\":%I64u,\"width\":%u,\"height\":%u,\"frames\":%I64u,\"unique_frames\":%I64u,\"duplicated\":%I64u,"
			L"\"written_frames\":%I64u,\"skipped_fps_limit\":%I64u,\"dropped\":%I64u,\"duration\":%.3f,\"effective_fps\":%.2f",
			FileSize, S->Width, S->Height, S->Considered, Unique, S->Duplicated,
			S->OutCount, S->LimitedByFps, Dropped, Duration, EffectiveFps);
		if (S->FramesOnly)
		{
			CliText_Printf(&Out, L",\"frames_dir\":");
			CliText_Json(&Out, S->FramesDir);
		}
		else
		{
			// width & height above are size of captured area, video can be smaller with --max-width/--max-height
			CliText_Printf(&Out, L",\"video_width\":%u,\"video_height\":%u", gEncoder.OutputWidth, gEncoder.OutputHeight);
		}
		if (S->Timestamps)
		{
			CliText_Printf(&Out, L",\"timestamps\":");
			CliText_Json(&Out, TimestampsPath);
		}
		if (S->MeasureFlicker)
		{
			CliText_Printf(&Out, L",\"flicker\":");
			CliText_Temporal(&Out, &S->Temporal);
		}
		CliText_Printf(&Out, L",\"warnings\":");
		CliText_Warnings(&Out);
		CliText_Printf(&Out, L"}\n");
	}
	else
	{
		CliText_Printf(&Out, L"saved: %ls\nsize: %I64u bytes\ndropped_frames: %I64u\n", gRecordingPath, FileSize, Dropped);
		if (S->Analyze)
		{
			CliText_Printf(&Out, L"frames: %I64u\nunique_frames: %I64u\nduplicated_frames: %I64u\nwritten_frames: %I64u\neffective_fps: %.2f\n",
				S->Considered, Unique, S->Duplicated, S->OutCount, EffectiveFps);
		}
		else if (S->FramesOnly)
		{
			CliText_Printf(&Out, L"written_frames: %I64u\n", S->OutCount);
		}
		if (S->Timestamps)
		{
			CliText_Printf(&Out, L"timestamps: %ls\n", TimestampsPath);
		}
		if (S->MeasureFlicker)
		{
			CliText_Printf(&Out, L"flicker (mean absolute difference between consecutive frames, 0..255):\n");
			CliText_TemporalHuman(&Out, &S->Temporal);
		}
	}
	CliText_Print(STD_OUTPUT_HANDLE, &Out);
	CliText_Free(&Out);
}

//
// record
//

static int CliRecord(int ArgCount, LPWSTR* Args)
{
	CliTarget Target = { .MonitorIndex = -1 };
	int Duration = 0;

	// parse arguments first, config values get overridden after loading .ini
	int Fps = -1, Bitrate = -1, MaxWidth = -1, MaxHeight = -1, Audio = -1;
	BOOL Fragmented = FALSE, Lossless = FALSE;
	LPCWSTR FramesDir = NULL;
	LPCWSTR StartOn = NULL;
	int StartTimeout = 0;

	for (int i = 0; i < ArgCount; i++)
	{
		LPCWSTR Arg = Args[i];
		LPCWSTR Next = i + 1 < ArgCount ? Args[i + 1] : NULL;

		int Handled = CliParseTargetOption(&Target, &i, ArgCount, Args);
		if (Handled < 0)
		{
			return 1;
		}
		if (Handled > 0)
		{
			continue;
		}

		#define CLI_RECORD_VALUE() do { if (!Next) { CliPrint(STD_ERROR_HANDLE, L"error: %ls requires a value\n", Arg); return 1; } i++; } while (0)
		#define CLI_RECORD_NUMBER(Var) do { CLI_RECORD_VALUE(); if (!CliParseNumber(Next, &(Var)) || (Var) < 0) { CliPrint(STD_ERROR_HANDLE, L"error: invalid value for %ls: %ls\n", Arg, Next); return 1; } } while (0)

		if (StrCmpW(Arg, L"-d") == 0 || StrCmpW(Arg, L"--duration") == 0)
		{
			CLI_RECORD_NUMBER(Duration);
		}
		else if (StrCmpW(Arg, L"--fps") == 0)
		{
			if (Next && StrCmpIW(Next, L"match") == 0)
			{
				// only new frames, no framerate limit
				i++;
				Fps = 0;
				gCli.Vfr = TRUE;
			}
			else
			{
				CLI_RECORD_NUMBER(Fps);
			}
		}
		else if (StrCmpW(Arg, L"--bitrate") == 0)    CLI_RECORD_NUMBER(Bitrate);
		else if (StrCmpW(Arg, L"--max-width") == 0)  CLI_RECORD_NUMBER(MaxWidth);
		else if (StrCmpW(Arg, L"--max-height") == 0) CLI_RECORD_NUMBER(MaxHeight);
		else if (StrCmpW(Arg, L"--audio") == 0)      Audio = 1;
		else if (StrCmpW(Arg, L"--no-audio") == 0)   Audio = 0;
		else if (StrCmpW(Arg, L"--fragmented") == 0) Fragmented = TRUE;
		else if (StrCmpW(Arg, L"--vfr") == 0)        gCli.Vfr = TRUE;
		else if (StrCmpW(Arg, L"--lossless") == 0)   Lossless = TRUE;
		else if (StrCmpW(Arg, L"--timestamps") == 0) gCli.Timestamps = TRUE;
		else if (StrCmpW(Arg, L"--measure-flicker") == 0) gCli.MeasureFlicker = TRUE;
		else if (StrCmpW(Arg, L"--frames-dir") == 0)
		{
			CLI_RECORD_VALUE();
			FramesDir = Next;
		}
		else if (StrCmpW(Arg, L"--start-on") == 0)
		{
			CLI_RECORD_VALUE();
			StartOn = Next;
		}
		else if (StrCmpW(Arg, L"--start-timeout") == 0)
		{
			CLI_RECORD_NUMBER(StartTimeout);
		}
		else if (StrCmpW(Arg, L"--measure-region") == 0)
		{
			CLI_RECORD_VALUE();
			int* Region = gCli.Temporal.Regions[gCli.Temporal.RegionCount < CLI_MAX_REGIONS ? gCli.Temporal.RegionCount : 0];
			if (gCli.Temporal.RegionCount >= CLI_MAX_REGIONS || !CliParseRect(Next, Region) || Region[2] <= 0 || Region[3] <= 0)
			{
				CliPrint(STD_ERROR_HANDLE, L"error: --measure-region expects X,Y,W,H with positive W and H (at most %d)\n", CLI_MAX_REGIONS);
				return 1;
			}
			gCli.Temporal.RegionCount++;
			gCli.MeasureFlicker = TRUE;
		}
		else
		{
			CliPrint(STD_ERROR_HANDLE, L"error: unknown argument: %ls\n", Arg);
			return 1;
		}

		#undef CLI_RECORD_NUMBER
		#undef CLI_RECORD_VALUE
	}

	if (!CliInitConfig())
	{
		return 1;
	}

	gCli.Json = Target.Json;
	gCli.Analyze = gCli.Vfr || gCli.Json || gCli.Timestamps || gCli.MeasureFlicker;
	gCli.Temporal.Keep = gCli.MeasureFlicker;
	if (StartOn)
	{
		StrCpyNW(gCli.StartOn, StartOn, _countof(gCli.StartOn));
		gCli.StartTimeoutMs = (DWORD)StartTimeout * 1000;
	}

	if (Fps >= 0)             gConfig.VideoMaxFramerate = Fps;
	if (Bitrate > 0)          gConfig.VideoBitrate = Bitrate;
	if (MaxWidth >= 0)        gConfig.VideoMaxWidth = MaxWidth;
	if (MaxHeight >= 0)       gConfig.VideoMaxHeight = MaxHeight;
	if (Audio >= 0)           gConfig.CaptureAudio = Audio;
	if (Target.NoCursor)      gConfig.MouseCursor = FALSE;
	if (Target.NoBorder)      gConfig.ShowRecordingBorder = FALSE;
	if (Fragmented)           gConfig.FragmentedOutput = TRUE;
	if (Duration > 0)
	{
		gConfig.EnableLimitLength = TRUE;
		gConfig.LimitLength = Duration;
	}
	else
	{
		gConfig.EnableLimitLength = FALSE;
	}

	if (Target.Output)
	{
		if (!GetFullPathNameW(Target.Output, _countof(gCliOutputPath), gCliOutputPath, NULL))
		{
			CliPrint(STD_ERROR_HANDLE, L"error: invalid output path: %ls\n", Target.Output);
			return 1;
		}
		// output folder gets created by StartRecording
		StrCpyW(gConfig.OutputFolder, gCliOutputPath);
		PathRemoveFileSpecW(gConfig.OutputFolder);
	}

	if (Lossless || FramesDir)
	{
		gCli.FramesOnly = TRUE;
		gConfig.CaptureAudio = FALSE;
		gConfig.EnableLimitSize = FALSE;

		if (FramesDir)
		{
			if (!GetFullPathNameW(FramesDir, _countof(gCli.FramesDir), gCli.FramesDir, NULL))
			{
				CliPrint(STD_ERROR_HANDLE, L"error: invalid frames folder: %ls\n", FramesDir);
				return 1;
			}
		}
		else
		{
			if (Target.Output)
			{
				StrCpyW(gCli.FramesDir, gCliOutputPath);
			}
			else
			{
				SYSTEMTIME Time;
				GetLocalTime(&Time);
				StrFormat(gCli.FramesDir, L"%ls\\%04u%02u%02u_%02u%02u%02u", gConfig.OutputFolder, Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);
			}
			PathRemoveExtensionW(gCli.FramesDir);
			StrCatBuffW(gCli.FramesDir, L"_frames", (int)_countof(gCli.FramesDir));
		}
	}

	CliResolved Resolved;
	if (!CliResolveTarget(&Target, TRUE, &Resolved))
	{
		return 1;
	}

	// setup same state as GUI, but with message-only window

	ScreenCapture_Create(&gCapture, &OnCaptureFrame, false);
	Encoder_Init(&gEncoder);
	QueryPerformanceFrequency(&gTickFreq);
	CliSetCrop(&Target, TRUE);

	WNDCLASSEXW WindowClass =
	{
		.cbSize = sizeof(WindowClass),
		.lpfnWndProc = WindowProc,
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = L"wcap_cli_window_class",
	};
	ATOM Atom = RegisterClassExW(&WindowClass);
	Assert(Atom);

	gWindow = CreateWindowExW(0, WindowClass.lpszClassName, WCAP_TITLE, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, WindowClass.hInstance, NULL);
	if (!gWindow)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: cannot create window\n");
		return 1;
	}

	gCliStopEvent = CreateEventW(NULL, TRUE, FALSE, WCAP_CLI_STOP_EVENT);
	gCliAbortEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	gCliDoneEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	SetConsoleCtrlHandler(&CliCtrlHandler, TRUE);

	// start recording right away

	if (Resolved.Window)
	{
		CaptureWindow(Resolved.Window);
	}
	else
	{
		CaptureMonitor(Resolved.Monitor, Resolved.UseRect ? &Resolved.Rect : NULL);
	}

	if (!gRecording)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: cannot start recording\n");
		return 1;
	}

	if (gCli.FramesOnly)
	{
		CliPrint(gCli.Json ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, L"recording: %ls\nframes: %ux%u png sequence\n",
			gRecordingPath, gCapture.Rect.right - gCapture.Rect.left, gCapture.Rect.bottom - gCapture.Rect.top);
	}
	else
	{
		CliPrint(gCli.Json ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, L"recording: %ls\nvideo: %ux%u @ %.2f fps\n",
			gRecordingPath, gEncoder.OutputWidth, gEncoder.OutputHeight,
			(float)gEncoder.FramerateNum / (float)gEncoder.FramerateDen);
	}

	if (Duration > 0)
	{
		// in case no new frames arrive (static screen) stop anyway, give frame based limit a bit of time first
		SetTimer(gWindow, WCAP_CLI_DURATION_TIMER, Duration * 1000 + 500, NULL);
	}

	gCliExitCode = 1;
	for (;;)
	{
		DWORD Wait = MsgWaitForMultipleObjects(1, &gCliStopEvent, FALSE, INFINITE, QS_ALLINPUT);
		if (Wait == WAIT_OBJECT_0 && gRecording)
		{
			StopRecording();
		}

		MSG Message;
		while (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
		{
			if (Message.message == WM_QUIT)
			{
				SetEvent(gCliDoneEvent);
				return gCliExitCode;
			}
			TranslateMessage(&Message);
			DispatchMessageW(&Message);
		}
	}
}

//
// snapshot
//

static struct
{
	WCHAR Path[MAX_PATH];
	CliReadback Readback;
	BOOL Done;
	BOOL Ok;
	UINT Width;
	UINT Height;
}
gSnap;

static bool CliSnapshotFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame)
{
	if (Frame == NULL)
	{
		// captured item was closed
		gSnap.Done = TRUE;
		PostQuitMessage(0);
		return true;
	}
	if (gSnap.Done)
	{
		return true;
	}

	const BYTE* Data;
	UINT Pitch, W, H;
	if (CliReadback_Map(&gSnap.Readback, Frame->Texture, Frame->Rect, &Data, &Pitch, &W, &H))
	{
		gSnap.Ok = CliPng_WriteSync(gSnap.Path, W, H, Data, Pitch);
		gSnap.Width = W;
		gSnap.Height = H;
		CliReadback_Unmap(&gSnap.Readback);
		gSnap.Done = TRUE;
		PostQuitMessage(0);
	}
	return true;
}

static int CliSnapshot(int ArgCount, LPWSTR* Args)
{
	CliTarget Target = { .MonitorIndex = -1 };
	for (int i = 0; i < ArgCount; i++)
	{
		int Handled = CliParseTargetOption(&Target, &i, ArgCount, Args);
		if (Handled < 0)
		{
			return 1;
		}
		if (Handled == 0)
		{
			CliPrint(STD_ERROR_HANDLE, L"error: unknown argument: %ls\n", Args[i]);
			return 1;
		}
	}

	if (!CliInitConfig())
	{
		return 1;
	}
	if (Target.NoCursor) gConfig.MouseCursor = FALSE;
	if (Target.NoBorder) gConfig.ShowRecordingBorder = FALSE;

	if (Target.Output)
	{
		if (!GetFullPathNameW(Target.Output, _countof(gSnap.Path), gSnap.Path, NULL))
		{
			CliPrint(STD_ERROR_HANDLE, L"error: invalid output path: %ls\n", Target.Output);
			return 1;
		}
	}
	else
	{
		SYSTEMTIME Time;
		GetLocalTime(&Time);
		StrFormat(gSnap.Path, L"%ls\\%04u%02u%02u_%02u%02u%02u.png", gConfig.OutputFolder, Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond);
	}
	WCHAR Folder[MAX_PATH];
	StrCpyW(Folder, gSnap.Path);
	PathRemoveFileSpecW(Folder);
	SHCreateDirectoryExW(NULL, Folder, NULL);

	CliResolved Resolved;
	if (!CliResolveTarget(&Target, FALSE, &Resolved))
	{
		return 1;
	}

	ScreenCapture_Create(&gCapture, &CliSnapshotFrame, false);
	CliSetCrop(&Target, FALSE);

	ID3D11Device* Device = CreateDevice();
	if (!Device)
	{
		return 1;
	}

	bool Created;
	if (Resolved.Window)
	{
		Created = ScreenCapture_CreateForWindow(&gCapture, Device, Resolved.Window, gConfig.OnlyClientArea, !gConfig.KeepRoundedWindowCorners);
	}
	else
	{
		Created = ScreenCapture_CreateForMonitor(&gCapture, Device, Resolved.Monitor, Resolved.UseRect ? &Resolved.Rect : NULL);
	}
	if (!Created)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: cannot capture selected target\n");
		return 1;
	}
	if (IsRectEmpty(&gCapture.Rect))
	{
		CliPrint(STD_ERROR_HANDLE, L"error: capture area is empty (window minimized, or crop is outside of captured area)\n");
		ScreenCapture_Stop(&gCapture);
		return 1;
	}

	ScreenCapture_Start(&gCapture, gConfig.MouseCursor, gConfig.ShowRecordingBorder, gConfig.IncludeSecondaryWindows);

	// wait for first frame
	ULONGLONG Deadline = GetTickCount64() + 5000;
	while (!gSnap.Done)
	{
		ULONGLONG Now = GetTickCount64();
		if (Now >= Deadline)
		{
			break;
		}
		MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)(Deadline - Now), QS_ALLINPUT);

		MSG Message;
		while (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
		{
			if (Message.message == WM_QUIT)
			{
				break;
			}
			TranslateMessage(&Message);
			DispatchMessageW(&Message);
		}
	}

	ScreenCapture_Stop(&gCapture);
	CliReadback_Release(&gSnap.Readback);
	ID3D11Device_Release(Device);

	if (!gSnap.Done || !gSnap.Ok)
	{
		CliPrint(STD_ERROR_HANDLE, gSnap.Done ? L"error: cannot write %ls\n" : L"error: no frame was captured within 5 seconds\n", gSnap.Path);
		return 1;
	}

	UINT64 FileSize = 0;
	WIN32_FILE_ATTRIBUTE_DATA Attributes;
	if (GetFileAttributesExW(gSnap.Path, GetFileExInfoStandard, &Attributes))
	{
		FileSize = ((UINT64)Attributes.nFileSizeHigh << 32) | Attributes.nFileSizeLow;
	}

	if (Target.Json)
	{
		CliText Out = { 0 };
		CliText_Printf(&Out, L"{\"saved\":");
		CliText_Json(&Out, gSnap.Path);
		CliText_Printf(&Out, L",\"width\":%u,\"height\":%u,\"size_bytes\":%I64u,\"warnings\":", gSnap.Width, gSnap.Height, FileSize);
		CliText_Warnings(&Out);
		CliText_Printf(&Out, L"}\n");
		CliText_Print(STD_OUTPUT_HANDLE, &Out);
	}
	else
	{
		CliPrint(STD_OUTPUT_HANDLE, L"saved: %ls\nimage: %ux%u\nsize: %I64u bytes\n", gSnap.Path, gSnap.Width, gSnap.Height, FileSize);
	}
	return 0;
}

//
// diff & sheet work on mp4 files
//

static BOOL CliStartMediaFoundation(void)
{
	HR(CoInitializeEx(0, COINIT_APARTMENTTHREADED));
	if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
	{
		CliPrint(STD_ERROR_HANDLE, L"error: cannot initialize Media Foundation\n");
		return FALSE;
	}
	return TRUE;
}

static void CliFullPath(LPCWSTR Path, WCHAR* Full, DWORD Count)
{
	if (!GetFullPathNameW(Path, Count, Full, NULL))
	{
		StrCpyNW(Full, Path, Count);
	}
}

static void CliText_RegionStats(CliText* Out, const int Regions[][4], const CliDoubles* Values, int Count)
{
	for (int r = 0; r < Count; r++)
	{
		CliDiffStats Stats = CliDiffStats_Compute(&Values[r]);
		CliText_Printf(Out, L"%ls{\"rect\":[%d,%d,%d,%d],", r ? L"," : L"", Regions[r][0], Regions[r][1], Regions[r][2], Regions[r][3]);
		CliText_Stats(Out, &Stats);
		CliText_Char(Out, L'}');
	}
}

static int CliDiff(int ArgCount, LPWSTR* Args)
{
	LPCWSTR Clips[2] = { 0 };
	int ClipCount = 0;
	int Regions[CLI_MAX_REGIONS][4];
	int RegionCount = 0;
	BOOL Json = FALSE;

	for (int i = 0; i < ArgCount; i++)
	{
		LPCWSTR Arg = Args[i];
		if (StrCmpW(Arg, L"--json") == 0)
		{
			Json = TRUE;
		}
		else if (StrCmpW(Arg, L"--region") == 0)
		{
			if (i + 1 >= ArgCount || RegionCount >= CLI_MAX_REGIONS || !CliParseRect(Args[i + 1], Regions[RegionCount]) || Regions[RegionCount][2] <= 0 || Regions[RegionCount][3] <= 0)
			{
				CliPrint(STD_ERROR_HANDLE, L"error: --region expects X,Y,W,H with positive W and H (at most %d)\n", CLI_MAX_REGIONS);
				return 1;
			}
			RegionCount++;
			i++;
		}
		else if (Arg[0] == L'-' && Arg[1])
		{
			CliPrint(STD_ERROR_HANDLE, L"error: unknown argument: %ls\n", Arg);
			return 1;
		}
		else if (ClipCount < 2)
		{
			Clips[ClipCount++] = Arg;
		}
		else
		{
			CliPrint(STD_ERROR_HANDLE, L"error: diff takes at most two clips\n");
			return 1;
		}
	}
	if (ClipCount == 0)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: usage: wcap-cli diff A.mp4 [B.mp4] [--region X,Y,W,H]... [--json]\n");
		return 1;
	}

	if (!CliStartMediaFoundation())
	{
		return 1;
	}

	CliClip Clip[2] = { 0 };
	CliTemporal Temporal[2] = { 0 };
	WCHAR Paths[2][MAX_PATH];
	for (int c = 0; c < ClipCount; c++)
	{
		CliFullPath(Clips[c], Paths[c], MAX_PATH);
		if (!CliClip_Open(&Clip[c], Paths[c]))
		{
			CliPrint(STD_ERROR_HANDLE, L"error: cannot open video: %ls\n", Paths[c]);
			return 1;
		}
		Temporal[c].Keep = TRUE;
		Temporal[c].RegionCount = RegionCount;
		CopyMemory(Temporal[c].Regions, Regions, sizeof(Regions));
	}

	// difference between frame N of A and frame N of B, only when sizes match
	BOOL Spatial = ClipCount == 2 && Clip[0].Width == Clip[1].Width && Clip[0].Height == Clip[1].Height;
	CliDoubles SpatialWhole = { 0 };
	CliDoubles SpatialRegion[CLI_MAX_REGIONS] = { 0 };

	for (;;)
	{
		BOOL Have[2] = { 0 };
		for (int c = 0; c < ClipCount; c++)
		{
			Have[c] = CliClip_Next(&Clip[c]);
			if (Have[c])
			{
				double Diff;
				CliTemporal_Add(&Temporal[c], Clip[c].Pixels, (size_t)Clip[c].Width * 4, Clip[c].Width, Clip[c].Height, &Diff);
			}
		}
		if (!Have[0] && !Have[1])
		{
			break;
		}
		if (Spatial && Have[0] && Have[1])
		{
			UINT W = Clip[0].Width, H = Clip[0].Height;
			UINT64 Sad = CliSad(Clip[0].Pixels, (size_t)W * 4, Clip[1].Pixels, (size_t)W * 4, 0, 0, (int)W, (int)H);
			CliDoubles_Add(&SpatialWhole, CliMeanDiff(Sad, (int)W, (int)H));
			for (int r = 0; r < RegionCount; r++)
			{
				int Rect[4];
				if (CliClipRegion(Regions[r], (int)W, (int)H, Rect))
				{
					UINT64 RegionSad = CliSad(Clip[0].Pixels, (size_t)W * 4, Clip[1].Pixels, (size_t)W * 4, Rect[0], Rect[1], Rect[2], Rect[3]);
					CliDoubles_Add(&SpatialRegion[r], CliMeanDiff(RegionSad, Rect[2], Rect[3]));
				}
			}
		}
	}

	CliText Out = { 0 };
	if (Json)
	{
		CliText_Char(&Out, L'{');
		for (int c = 0; c < ClipCount; c++)
		{
			CliText_Printf(&Out, L"%ls\"%ls\":{\"path\":", c ? L"," : L"", c ? L"b" : L"a");
			CliText_Json(&Out, Paths[c]);
			CliText_Printf(&Out, L",\"width\":%u,\"height\":%u,\"duration\":%.3f,\"temporal\":", Clip[c].Width, Clip[c].Height, (double)Clip[c].DurationHns / 10000000.0);
			CliText_Temporal(&Out, &Temporal[c]);
			CliText_Char(&Out, L'}');
		}
		if (ClipCount == 2)
		{
			CliText_Printf(&Out, L",\"spatial\":");
			if (Spatial)
			{
				CliDiffStats Whole = CliDiffStats_Compute(&SpatialWhole);
				CliText_Printf(&Out, L"{\"frames\":%I64u,\"whole\":{", (UINT64)SpatialWhole.Count);
				CliText_Stats(&Out, &Whole);
				CliText_Printf(&Out, L"},\"regions\":[");
				CliText_RegionStats(&Out, Regions, SpatialRegion, RegionCount);
				CliText_Printf(&Out, L"]}");
			}
			else
			{
				CliText_Printf(&Out, L"null");
			}

			// temporal comparison of A and B, region -1 is whole frame
			CliText_Printf(&Out, L",\"comparison\":[");
			for (int r = 0; r <= RegionCount; r++)
			{
				CliDiffStats SA = CliDiffStats_Compute(r ? &Temporal[0].PerRegion[r - 1] : &Temporal[0].Whole);
				CliDiffStats SB = CliDiffStats_Compute(r ? &Temporal[1].PerRegion[r - 1] : &Temporal[1].Whole);
				CliText_Printf(&Out, L"%ls{\"region\":%d,\"mean_a\":%.5f,\"mean_b\":%.5f,\"mean_nonzero_a\":%.5f,\"mean_nonzero_b\":%.5f,\"ratio_mean\":",
					r ? L"," : L"", r - 1, SA.Mean, SB.Mean, SA.MeanNonZero, SB.MeanNonZero);
				if (SA.Mean > 0) CliText_Printf(&Out, L"%.4f", SB.Mean / SA.Mean); else CliText_Printf(&Out, L"null");
				CliText_Char(&Out, L'}');
			}
			CliText_Char(&Out, L']');
		}
		CliText_Printf(&Out, L"}\n");
	}
	else
	{
		for (int c = 0; c < ClipCount; c++)
		{
			CliText_Printf(&Out, L"%ls: %ls %ux%u %.3fs\n", c ? L"B" : L"A", Paths[c], Clip[c].Width, Clip[c].Height, (double)Clip[c].DurationHns / 10000000.0);
			CliText_Printf(&Out, L"temporal difference between consecutive frames (mean absolute difference per channel, 0..255)\n");
			CliText_TemporalHuman(&Out, &Temporal[c]);
		}
		if (ClipCount == 2)
		{
			if (Spatial)
			{
				CliText_Printf(&Out, L"A vs B, same frame index (%I64u frames):\n", (UINT64)SpatialWhole.Count);
				CliText_StatsLine(&Out, L"whole      ", &SpatialWhole);
				for (int r = 0; r < RegionCount; r++)
				{
					WCHAR Label[64];
					StrFormat(Label, L"region %d [%d,%d,%d,%d]", r, Regions[r][0], Regions[r][1], Regions[r][2], Regions[r][3]);
					CliText_StatsLine(&Out, Label, &SpatialRegion[r]);
				}
			}
			else
			{
				CliText_Printf(&Out, L"A vs B: skipped, videos have different sizes\n");
			}
			CliText_Printf(&Out, L"temporal mean B / A:\n");
			for (int r = 0; r <= RegionCount; r++)
			{
				CliDiffStats SA = CliDiffStats_Compute(r ? &Temporal[0].PerRegion[r - 1] : &Temporal[0].Whole);
				CliDiffStats SB = CliDiffStats_Compute(r ? &Temporal[1].PerRegion[r - 1] : &Temporal[1].Whole);
				CliText_Printf(&Out, L"  %ls %d: A=%.4f B=%.4f ratio=", r ? L"region" : L"whole", r ? r - 1 : 0, SA.Mean, SB.Mean);
				if (SA.Mean > 0) CliText_Printf(&Out, L"%.3f\n", SB.Mean / SA.Mean); else CliText_Printf(&Out, L"n/a\n");
			}
		}
	}
	CliText_Print(STD_OUTPUT_HANDLE, &Out);
	return 0;
}

static int CliSheet(int ArgCount, LPWSTR* Args)
{
	CliSheetInput Inputs[8] = { 0 };
	int InputCount = 0;
	double Times[64];
	int TimeCount = 0;
	double Every = 0;
	int Count = 0;
	CliSheetOptions Options = { .Labels = TRUE, .CellWidth = 640 };
	LPCWSTR Output = NULL;
	BOOL Json = FALSE;

	for (int i = 0; i < ArgCount; i++)
	{
		LPCWSTR Arg = Args[i];
		LPCWSTR Next = i + 1 < ArgCount ? Args[i + 1] : NULL;

		if (StrCmpW(Arg, L"--json") == 0)            Json = TRUE;
		else if (StrCmpW(Arg, L"--grid") == 0)       Options.Grid = TRUE;
		else if (StrCmpW(Arg, L"--no-labels") == 0)  Options.Labels = FALSE;
		else if (Arg[0] == L'-' && Arg[1] && !Next)
		{
			CliPrint(STD_ERROR_HANDLE, L"error: %ls requires a value\n", Arg);
			return 1;
		}
		else if (StrCmpW(Arg, L"-o") == 0 || StrCmpW(Arg, L"--output") == 0)
		{
			Output = Next;
			i++;
		}
		else if (StrCmpW(Arg, L"--at") == 0)
		{
			LPCWSTR p = Next;
			for (;;)
			{
				double Time;
				if (TimeCount >= (int)_countof(Times) || !CliParseDouble(p, &Time, &p))
				{
					CliPrint(STD_ERROR_HANDLE, L"error: --at expects comma separated times in seconds (at most %d)\n", (int)_countof(Times));
					return 1;
				}
				Times[TimeCount++] = Time;
				if (*p != L',')
				{
					break;
				}
				p++;
			}
			if (*p)
			{
				CliPrint(STD_ERROR_HANDLE, L"error: invalid --at value: %ls\n", Next);
				return 1;
			}
			i++;
		}
		else if (StrCmpW(Arg, L"--every") == 0)
		{
			LPCWSTR End;
			if (!CliParseDouble(Next, &Every, &End) || *End || Every <= 0)
			{
				CliPrint(STD_ERROR_HANDLE, L"error: invalid --every value: %ls\n", Next);
				return 1;
			}
			i++;
		}
		else if (StrCmpW(Arg, L"--count") == 0)
		{
			if (!CliParseNumber(Next, &Count) || Count <= 0 || Count > (int)_countof(Times))
			{
				CliPrint(STD_ERROR_HANDLE, L"error: invalid --count value: %ls\n", Next);
				return 1;
			}
			i++;
		}
		else if (StrCmpW(Arg, L"--cols") == 0)
		{
			if (!CliParseNumber(Next, &Options.Columns) || Options.Columns <= 0)
			{
				CliPrint(STD_ERROR_HANDLE, L"error: invalid --cols value: %ls\n", Next);
				return 1;
			}
			i++;
		}
		else if (StrCmpW(Arg, L"--width") == 0)
		{
			if (!CliParseNumber(Next, &Options.CellWidth) || Options.CellWidth < 0 || Options.CellWidth > 16384)
			{
				CliPrint(STD_ERROR_HANDLE, L"error: invalid --width value: %ls\n", Next);
				return 1;
			}
			i++;
		}
		else if (StrCmpW(Arg, L"--region") == 0)
		{
			if (!CliParseRect(Next, Options.Region) || Options.Region[2] <= 0 || Options.Region[3] <= 0)
			{
				CliPrint(STD_ERROR_HANDLE, L"error: --region expects X,Y,W,H with positive W and H\n");
				return 1;
			}
			i++;
		}
		else if (Arg[0] == L'-' && Arg[1])
		{
			CliPrint(STD_ERROR_HANDLE, L"error: unknown argument: %ls\n", Arg);
			return 1;
		}
		else if (InputCount < (int)_countof(Inputs))
		{
			Inputs[InputCount++].Path = Arg;
		}
		else
		{
			CliPrint(STD_ERROR_HANDLE, L"error: too many input videos\n");
			return 1;
		}
	}
	if (InputCount == 0)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: usage: wcap-cli sheet CLIP.mp4... [--at T,T,...] [-o sheet.png]\n");
		return 1;
	}

	if (!CliStartMediaFoundation())
	{
		return 1;
	}

	WCHAR Paths[8][MAX_PATH];
	for (int c = 0; c < InputCount; c++)
	{
		CliFullPath(Inputs[c].Path, Paths[c], MAX_PATH);
		Inputs[c].Path = Paths[c];
		if (!CliClip_Open(&Inputs[c].Clip, Paths[c]))
		{
			CliPrint(STD_ERROR_HANDLE, L"error: cannot open video: %ls\n", Paths[c]);
			return 1;
		}
	}

	// times relative to duration of first clip when not given explicitly
	if (TimeCount == 0)
	{
		double Duration = (double)Inputs[0].Clip.DurationHns / 10000000.0;
		if (Every > 0)
		{
			for (double t = 0; t < Duration && TimeCount < (int)_countof(Times); t += Every)
			{
				Times[TimeCount++] = t;
			}
		}
		else
		{
			int N = Count ? Count : 6;
			for (int i = 0; i < N; i++)
			{
				Times[TimeCount++] = Duration * (i + 0.5) / N;
			}
		}
	}
	if (TimeCount == 0)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: no frame times to extract\n");
		return 1;
	}
	if (InputCount * TimeCount > 64)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: at most 64 frames per sheet\n");
		return 1;
	}

	WCHAR OutputPath[MAX_PATH];
	if (Output)
	{
		CliFullPath(Output, OutputPath, MAX_PATH);
	}
	else
	{
		StrCpyW(OutputPath, Paths[0]);
		PathRenameExtensionW(OutputPath, L".sheet.png");
	}
	WCHAR Folder[MAX_PATH];
	StrCpyW(Folder, OutputPath);
	PathRemoveFileSpecW(Folder);
	SHCreateDirectoryExW(NULL, Folder, NULL);

	Options.Inputs = Inputs;
	Options.InputCount = InputCount;
	Options.Times = Times;
	Options.TimeCount = TimeCount;

	UINT Width, Height;
	LPCWSTR Error = CliSheet_Render(&Options, OutputPath, &Width, &Height);
	if (Error)
	{
		CliPrint(STD_ERROR_HANDLE, L"error: %ls\n", Error);
		return 1;
	}

	if (Json)
	{
		CliText Out = { 0 };
		CliText_Printf(&Out, L"{\"saved\":");
		CliText_Json(&Out, OutputPath);
		CliText_Printf(&Out, L",\"width\":%u,\"height\":%u,\"clips\":%d,\"times\":[", Width, Height, InputCount);
		for (int i = 0; i < TimeCount; i++)
		{
			CliText_Printf(&Out, L"%ls%.3f", i ? L"," : L"", Times[i]);
		}
		CliText_Printf(&Out, L"]}\n");
		CliText_Print(STD_OUTPUT_HANDLE, &Out);
	}
	else
	{
		CliPrint(STD_OUTPUT_HANDLE, L"saved: %ls\nimage: %ux%u\nframes: %d\n", OutputPath, Width, Height, InputCount * TimeCount);
	}
	return 0;
}

static int CliMain(int ArgCount, LPWSTR* Args)
{
	if (ArgCount < 2 || StrCmpW(Args[1], L"help") == 0 || StrCmpW(Args[1], L"--help") == 0 || StrCmpW(Args[1], L"-h") == 0)
	{
		CliUsage();
		return ArgCount < 2 ? 1 : 0;
	}
	if (StrCmpW(Args[1], L"list") == 0)
	{
		return CliList(ArgCount - 2, Args + 2);
	}
	if (StrCmpW(Args[1], L"stop") == 0)
	{
		return CliStop();
	}
	if (StrCmpW(Args[1], L"signal") == 0)
	{
		return CliSignal(ArgCount - 2, Args + 2);
	}
	if (StrCmpW(Args[1], L"record") == 0)
	{
		return CliRecord(ArgCount - 2, Args + 2);
	}
	if (StrCmpW(Args[1], L"snapshot") == 0)
	{
		return CliSnapshot(ArgCount - 2, Args + 2);
	}
	if (StrCmpW(Args[1], L"diff") == 0)
	{
		return CliDiff(ArgCount - 2, Args + 2);
	}
	if (StrCmpW(Args[1], L"sheet") == 0)
	{
		return CliSheet(ArgCount - 2, Args + 2);
	}

	CliPrint(STD_ERROR_HANDLE, L"error: unknown command: %ls, see 'wcap-cli help'\n", Args[1]);
	return 1;
}

#ifndef NDEBUG
int main(void)
#else
void mainCRTStartup(void)
#endif
{
	int ArgCount;
	LPWSTR* Args = CommandLineToArgvW(GetCommandLineW(), &ArgCount);
	ExitProcess(CliMain(ArgCount, Args));
}

#endif // WCAP_CLI
