#pragma once

#define UNICODE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_DEPRECATE

#include <initguid.h>
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <intrin.h>
#include <stdatomic.h>

#define WCAP_TITLE L"wcap"
#define WCAP_URL   L"https://github.com/mmozeiko/wcap"

#if defined(WCAP_GIT_INFO)
#	define WCAP_CONFIG_TITLE "wcap, " __DATE__ " [" WCAP_GIT_INFO "]"
#else
#	define WCAP_CONFIG_TITLE "wcap, " __DATE__
#endif

#ifdef _DEBUG
#define Assert(Cond) do { if (!(Cond)) __debugbreak(); } while (0)
#else
#define Assert(Cond) (void)(Cond)
#endif
#define HR(hr) do { HRESULT _hr = (hr); Assert(SUCCEEDED(_hr)); } while (0)

// calculates ceil(X * Num / Den)
#define MUL_DIV_ROUND_UP(X, Num, Den) (((X) * (Num) - 1) / (Den) + 1)

// caclulates ceil(X / Y)
#define DIV_ROUND_UP(X, Y) ( ((X) + (Y) - 1) / (Y) )

// MF works with 100nsec units
#define MF_UNITS_PER_SECOND 10000000ULL

#include <stdio.h>
#define StrFormat(Buffer, ...) _snwprintf(Buffer, _countof(Buffer), __VA_ARGS__)

// writes UTF-8 text to console/pipe handle (STD_OUTPUT_HANDLE or STD_ERROR_HANDLE)
static void WriteText(DWORD StdHandle, LPCWSTR Text)
{
	int Length = WideCharToMultiByte(CP_UTF8, 0, Text, -1, NULL, 0, NULL, NULL);
	if (Length > 1)
	{
		char* Buffer = HeapAlloc(GetProcessHeap(), 0, Length);
		if (Buffer)
		{
			WideCharToMultiByte(CP_UTF8, 0, Text, -1, Buffer, Length, NULL, NULL);
			DWORD Written;
			WriteFile(GetStdHandle(StdHandle), Buffer, Length - 1, &Written, NULL);
			HeapFree(GetProcessHeap(), 0, Buffer);
		}
	}
}

// in CLI build errors go to stderr instead of blocking message box
static void ErrorMessage(LPCWSTR Text)
{
#if defined(WCAP_CLI)
	WriteText(STD_ERROR_HANDLE, L"error: ");
	WriteText(STD_ERROR_HANDLE, Text);
	WriteText(STD_ERROR_HANDLE, L"\n");
#else
	MessageBoxW(NULL, Text, WCAP_TITLE, MB_ICONERROR);
#endif
}
