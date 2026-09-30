#pragma once

//
// helpers used only by wcap-cli: frame readback & comparison (duplicate detection, flicker metric),
// PNG output, mp4 decoding (diff, sheet), JSON text building
//

#include "wcap.h"

#include <d3d11.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#if defined(_M_X64)
#include <emmintrin.h>
#endif

//
// memory & text
//

// CLI build has no CRT initialization, so use process heap directly
static void* CliAlloc(SIZE_T Size)
{
	return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size ? Size : 1);
}

static void* CliRealloc(void* Ptr, SIZE_T Size)
{
	return Ptr ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Ptr, Size) : CliAlloc(Size);
}

static void CliFree(void* Ptr)
{
	if (Ptr)
	{
		HeapFree(GetProcessHeap(), 0, Ptr);
	}
}

typedef struct
{
	LPWSTR Data;
	size_t Length;
	size_t Capacity;
}
CliText;

static void CliText_Reserve(CliText* Text, size_t Extra)
{
	if (Text->Length + Extra + 1 > Text->Capacity)
	{
		size_t Capacity = Text->Capacity * 2;
		if (Capacity < Text->Length + Extra + 1) Capacity = Text->Length + Extra + 1;
		if (Capacity < 1024) Capacity = 1024;
		Text->Data = CliRealloc(Text->Data, Capacity * sizeof(WCHAR));
		Text->Capacity = Capacity;
	}
}

static void CliText_Char(CliText* Text, WCHAR Char)
{
	CliText_Reserve(Text, 1);
	Text->Data[Text->Length++] = Char;
	Text->Data[Text->Length] = 0;
}

static void CliText_Printf(CliText* Text, LPCWSTR Format, ...)
{
	WCHAR Buffer[1024];
	va_list Args;
	va_start(Args, Format);
	_vsnwprintf(Buffer, _countof(Buffer), Format, Args);
	va_end(Args);
	Buffer[_countof(Buffer) - 1] = 0;

	size_t Length = lstrlenW(Buffer);
	CliText_Reserve(Text, Length);
	CopyMemory(Text->Data + Text->Length, Buffer, (Length + 1) * sizeof(WCHAR));
	Text->Length += Length;
}

// appends JSON string literal with quotes
static void CliText_Json(CliText* Text, LPCWSTR String)
{
	CliText_Char(Text, L'"');
	for (; *String; String++)
	{
		WCHAR Char = *String;
		if (Char == L'"')       { CliText_Char(Text, L'\\'); CliText_Char(Text, L'"'); }
		else if (Char == L'\\') { CliText_Char(Text, L'\\'); CliText_Char(Text, L'\\'); }
		else if (Char == L'\n') { CliText_Char(Text, L'\\'); CliText_Char(Text, L'n'); }
		else if (Char == L'\r') { CliText_Char(Text, L'\\'); CliText_Char(Text, L'r'); }
		else if (Char == L'\t') { CliText_Char(Text, L'\\'); CliText_Char(Text, L't'); }
		else if (Char < 0x20)   { CliText_Printf(Text, L"\\u%04x", (unsigned)Char); }
		else                    { CliText_Char(Text, Char); }
	}
	CliText_Char(Text, L'"');
}

static void CliText_Free(CliText* Text)
{
	CliFree(Text->Data);
	*Text = (CliText){ 0 };
}

static void CliText_Print(DWORD StdHandle, const CliText* Text)
{
	if (Text->Data)
	{
		WriteText(StdHandle, Text->Data);
	}
}

static BOOL CliWriteFileUtf8(LPCWSTR Path, LPCWSTR Text)
{
	int Length = WideCharToMultiByte(CP_UTF8, 0, Text, -1, NULL, 0, NULL, NULL);
	if (Length <= 1)
	{
		return FALSE;
	}
	char* Buffer = CliAlloc(Length);
	WideCharToMultiByte(CP_UTF8, 0, Text, -1, Buffer, Length, NULL, NULL);

	BOOL Result = FALSE;
	HANDLE File = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (File != INVALID_HANDLE_VALUE)
	{
		DWORD Written;
		Result = WriteFile(File, Buffer, Length - 1, &Written, NULL) && Written == (DWORD)(Length - 1);
		CloseHandle(File);
	}
	CliFree(Buffer);
	return Result;
}

// no CRT locale dependency, accepts 12, 0.5, .5
static BOOL CliParseDouble(LPCWSTR Text, double* Value, LPCWSTR* End)
{
	double Result = 0;
	BOOL Digits = FALSE;
	while (*Text >= L'0' && *Text <= L'9')
	{
		Result = Result * 10 + (*Text++ - L'0');
		Digits = TRUE;
	}
	if (*Text == L'.')
	{
		Text++;
		double Scale = 0.1;
		while (*Text >= L'0' && *Text <= L'9')
		{
			Result += (*Text++ - L'0') * Scale;
			Scale /= 10;
			Digits = TRUE;
		}
	}
	if (!Digits)
	{
		return FALSE;
	}
	*Value = Result;
	if (End)
	{
		*End = Text;
	}
	return TRUE;
}

//
// double arrays & stats
//

typedef struct
{
	double* Data;
	size_t Count;
	size_t Capacity;
}
CliDoubles;

static void CliDoubles_Add(CliDoubles* Array, double Value)
{
	if (Array->Count == Array->Capacity)
	{
		Array->Capacity = Array->Capacity ? Array->Capacity * 2 : 1024;
		Array->Data = CliRealloc(Array->Data, Array->Capacity * sizeof(double));
	}
	Array->Data[Array->Count++] = Value;
}

static void CliDoubles_Free(CliDoubles* Array)
{
	CliFree(Array->Data);
	*Array = (CliDoubles){ 0 };
}

static void CliSortDoubles(double* Data, size_t Count)
{
	// shell sort, good enough for a few thousand values
	for (size_t Gap = Count / 2; Gap > 0; Gap /= 2)
	{
		for (size_t i = Gap; i < Count; i++)
		{
			double Value = Data[i];
			size_t j = i;
			for (; j >= Gap && Data[j - Gap] > Value; j -= Gap)
			{
				Data[j] = Data[j - Gap];
			}
			Data[j] = Value;
		}
	}
}

typedef struct
{
	UINT64 Pairs;
	UINT64 NonZeroPairs;
	double Mean;        // over all pairs (duplicates count as zero)
	double MeanNonZero; // over pairs where frames differ
	double P95;
	double Max;
}
CliDiffStats;

static CliDiffStats CliDiffStats_Compute(const CliDoubles* Diffs)
{
	CliDiffStats Stats = { .Pairs = Diffs->Count };
	if (Diffs->Count == 0)
	{
		return Stats;
	}

	double Sum = 0;
	double* Copy = CliAlloc(Diffs->Count * sizeof(double));
	for (size_t i = 0; i < Diffs->Count; i++)
	{
		double Value = Diffs->Data[i];
		Copy[i] = Value;
		Sum += Value;
		if (Value > 0)
		{
			Stats.NonZeroPairs++;
		}
		if (Value > Stats.Max)
		{
			Stats.Max = Value;
		}
	}
	Stats.Mean = Sum / (double)Diffs->Count;
	Stats.MeanNonZero = Stats.NonZeroPairs ? Sum / (double)Stats.NonZeroPairs : 0;

	CliSortDoubles(Copy, Diffs->Count);
	size_t Index = (size_t)(0.95 * (double)Diffs->Count);
	if (Index >= Diffs->Count) Index = Diffs->Count - 1;
	Stats.P95 = Copy[Index];
	CliFree(Copy);
	return Stats;
}

static void CliText_Stats(CliText* Text, const CliDiffStats* Stats)
{
	CliText_Printf(Text, L"\"pairs\":%I64u,\"nonzero_pairs\":%I64u,\"mean\":%.5f,\"mean_nonzero\":%.5f,\"p95\":%.5f,\"max\":%.5f",
		Stats->Pairs, Stats->NonZeroPairs, Stats->Mean, Stats->MeanNonZero, Stats->P95, Stats->Max);
}

//
// pixel comparison, BGRA images
//

// sum of absolute differences of B,G,R channels (alpha is ignored) inside the rectangle
static UINT64 CliSad(const BYTE* A, size_t PitchA, const BYTE* B, size_t PitchB, int X, int Y, int Width, int Height)
{
	UINT64 Sum = 0;
	for (int y = 0; y < Height; y++)
	{
		const BYTE* PA = A + (size_t)(Y + y) * PitchA + (size_t)X * 4;
		const BYTE* PB = B + (size_t)(Y + y) * PitchB + (size_t)X * 4;
		int Count = Width;

#if defined(_M_X64)
		__m128i Mask = _mm_set1_epi32(0x00FFFFFF);
		__m128i Acc = _mm_setzero_si128();
		while (Count >= 4)
		{
			__m128i VA = _mm_and_si128(_mm_loadu_si128((const __m128i*)PA), Mask);
			__m128i VB = _mm_and_si128(_mm_loadu_si128((const __m128i*)PB), Mask);
			Acc = _mm_add_epi64(Acc, _mm_sad_epu8(VA, VB));
			PA += 16;
			PB += 16;
			Count -= 4;
		}
		Sum += (UINT64)_mm_cvtsi128_si64(Acc) + (UINT64)_mm_cvtsi128_si64(_mm_srli_si128(Acc, 8));
#endif
		for (; Count > 0; Count--, PA += 4, PB += 4)
		{
			for (int c = 0; c < 3; c++)
			{
				int D = (int)PA[c] - (int)PB[c];
				Sum += D < 0 ? -D : D;
			}
		}
	}
	return Sum;
}

// mean absolute difference per channel, 0..255
static double CliMeanDiff(UINT64 Sad, int Width, int Height)
{
	return Width > 0 && Height > 0 ? (double)Sad / ((double)Width * (double)Height * 3.0) : 0;
}

// rectangle given as x,y,w,h clipped to image size, returns false if empty
static BOOL CliClipRegion(const int Region[4], int Width, int Height, int Out[4])
{
	int X0 = Region[0] < 0 ? 0 : Region[0];
	int Y0 = Region[1] < 0 ? 0 : Region[1];
	int X1 = Region[0] + Region[2];
	int Y1 = Region[1] + Region[3];
	if (X1 > Width) X1 = Width;
	if (Y1 > Height) Y1 = Height;
	if (X1 <= X0 || Y1 <= Y0)
	{
		return FALSE;
	}
	Out[0] = X0;
	Out[1] = Y0;
	Out[2] = X1 - X0;
	Out[3] = Y1 - Y0;
	return TRUE;
}

//
// temporal difference tracker: feed frames one by one, get whole frame + region stats
//

#define CLI_MAX_REGIONS 8

typedef struct
{
	BOOL Keep;                  // keep per pair values for stats
	int Regions[CLI_MAX_REGIONS][4];
	int RegionCount;
	CliDoubles Whole;
	CliDoubles PerRegion[CLI_MAX_REGIONS];

	BYTE* Prev;                 // tight BGRA copy of previous frame
	UINT PrevWidth;
	UINT PrevHeight;
	UINT64 Frames;
	UINT64 Identical;           // frames identical to previous one
}
CliTemporal;

// returns whether frame differs from previous one, Diff receives mean difference of whole frame
static BOOL CliTemporal_Add(CliTemporal* T, const BYTE* Data, size_t Pitch, UINT Width, UINT Height, double* Diff)
{
	BOOL Unique = TRUE;
	*Diff = 0;

	T->Frames++;
	if (T->Prev && T->PrevWidth == Width && T->PrevHeight == Height)
	{
		UINT64 Sad = CliSad(T->Prev, (size_t)Width * 4, Data, Pitch, 0, 0, (int)Width, (int)Height);
		*Diff = CliMeanDiff(Sad, (int)Width, (int)Height);
		Unique = Sad != 0;
		if (!Unique)
		{
			T->Identical++;
		}

		if (T->Keep)
		{
			CliDoubles_Add(&T->Whole, *Diff);
			for (int i = 0; i < T->RegionCount; i++)
			{
				int Rect[4];
				if (CliClipRegion(T->Regions[i], (int)Width, (int)Height, Rect))
				{
					UINT64 RegionSad = CliSad(T->Prev, (size_t)Width * 4, Data, Pitch, Rect[0], Rect[1], Rect[2], Rect[3]);
					CliDoubles_Add(&T->PerRegion[i], CliMeanDiff(RegionSad, Rect[2], Rect[3]));
				}
			}
		}
	}
	else
	{
		CliFree(T->Prev);
		T->Prev = CliAlloc((size_t)Width * Height * 4);
		T->PrevWidth = Width;
		T->PrevHeight = Height;
	}

	for (UINT y = 0; y < Height; y++)
	{
		CopyMemory(T->Prev + (size_t)y * Width * 4, Data + (size_t)y * Pitch, (size_t)Width * 4);
	}
	return Unique;
}

static void CliTemporal_Free(CliTemporal* T)
{
	CliFree(T->Prev);
	CliDoubles_Free(&T->Whole);
	for (int i = 0; i < CLI_MAX_REGIONS; i++)
	{
		CliDoubles_Free(&T->PerRegion[i]);
	}
	*T = (CliTemporal){ 0 };
}

// {"width":..,"height":..,"whole":{...},"regions":[{"rect":[x,y,w,h],...}]}
static void CliText_Temporal(CliText* Text, const CliTemporal* T)
{
	CliDiffStats Whole = CliDiffStats_Compute(&T->Whole);
	CliText_Printf(Text, L"{\"frames\":%I64u,\"identical_frames\":%I64u,\"whole\":{", T->Frames, T->Identical);
	CliText_Stats(Text, &Whole);
	CliText_Printf(Text, L"},\"regions\":[");
	for (int i = 0; i < T->RegionCount; i++)
	{
		CliDiffStats Stats = CliDiffStats_Compute(&T->PerRegion[i]);
		CliText_Printf(Text, L"%ls{\"rect\":[%d,%d,%d,%d],", i ? L"," : L"", T->Regions[i][0], T->Regions[i][1], T->Regions[i][2], T->Regions[i][3]);
		CliText_Stats(Text, &Stats);
		CliText_Char(Text, L'}');
	}
	CliText_Printf(Text, L"]}");
}

static void CliText_StatsLine(CliText* Text, LPCWSTR Label, const CliDoubles* Diffs)
{
	CliDiffStats S = CliDiffStats_Compute(Diffs);
	CliText_Printf(Text, L"  %ls mean=%.4f mean_nonzero=%.4f p95=%.4f max=%.4f pairs=%I64u nonzero=%I64u\n",
		Label, S.Mean, S.MeanNonZero, S.P95, S.Max, S.Pairs, S.NonZeroPairs);
}

static void CliText_TemporalHuman(CliText* Text, const CliTemporal* T)
{
	CliText_Printf(Text, L"frames: %I64u (identical to previous: %I64u)\n", T->Frames, T->Identical);
	CliText_StatsLine(Text, L"whole      ", &T->Whole);
	for (int i = 0; i < T->RegionCount; i++)
	{
		WCHAR Label[64];
		StrFormat(Label, L"region %d [%d,%d,%d,%d]", i, T->Regions[i][0], T->Regions[i][1], T->Regions[i][2], T->Regions[i][3]);
		CliText_StatsLine(Text, Label, &T->PerRegion[i]);
	}
}

//
// GPU texture readback
//

typedef struct
{
	ID3D11Device* Device;
	ID3D11DeviceContext* Context;
	ID3D11Multithread* Multithread;
	ID3D11Texture2D* Staging;
	UINT Width;
	UINT Height;
}
CliReadback;

static void CliReadback_Release(CliReadback* R)
{
	if (R->Staging)     ID3D11Texture2D_Release(R->Staging);
	if (R->Multithread) ID3D11Multithread_Release(R->Multithread);
	if (R->Context)     ID3D11DeviceContext_Release(R->Context);
	if (R->Device)      ID3D11Device_Release(R->Device);
	*R = (CliReadback){ 0 };
}

// copies Rect of texture to CPU memory, data stays valid until Unmap
static BOOL CliReadback_Map(CliReadback* R, ID3D11Texture2D* Texture, RECT Rect, const BYTE** Data, UINT* Pitch, UINT* Width, UINT* Height)
{
	UINT W = Rect.right - Rect.left;
	UINT H = Rect.bottom - Rect.top;
	if (Rect.right <= Rect.left || Rect.bottom <= Rect.top)
	{
		return FALSE;
	}

	ID3D11Device* Device;
	ID3D11Texture2D_GetDevice(Texture, &Device);
	if (R->Device != Device)
	{
		CliReadback_Release(R);
		R->Device = Device;
		ID3D11Device_GetImmediateContext(Device, &R->Context);
		ID3D11DeviceContext_QueryInterface(R->Context, &IID_ID3D11Multithread, (void**)&R->Multithread);
	}
	else
	{
		ID3D11Device_Release(Device);
	}

	if (!R->Staging || R->Width != W || R->Height != H)
	{
		if (R->Staging)
		{
			ID3D11Texture2D_Release(R->Staging);
			R->Staging = NULL;
		}

		D3D11_TEXTURE2D_DESC Desc;
		ID3D11Texture2D_GetDesc(Texture, &Desc);
		Desc.Width = W;
		Desc.Height = H;
		Desc.MipLevels = 1;
		Desc.ArraySize = 1;
		Desc.SampleDesc.Count = 1;
		Desc.SampleDesc.Quality = 0;
		Desc.Usage = D3D11_USAGE_STAGING;
		Desc.BindFlags = 0;
		Desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		Desc.MiscFlags = 0;
		if (FAILED(ID3D11Device_CreateTexture2D(R->Device, &Desc, NULL, &R->Staging)))
		{
			return FALSE;
		}
		R->Width = W;
		R->Height = H;
	}

	D3D11_BOX Box = { Rect.left, Rect.top, 0, Rect.right, Rect.bottom, 1 };
	D3D11_MAPPED_SUBRESOURCE Mapped;

	if (R->Multithread) ID3D11Multithread_Enter(R->Multithread);
	ID3D11DeviceContext_CopySubresourceRegion(R->Context, (ID3D11Resource*)R->Staging, 0, 0, 0, 0, (ID3D11Resource*)Texture, 0, &Box);
	HRESULT hr = ID3D11DeviceContext_Map(R->Context, (ID3D11Resource*)R->Staging, 0, D3D11_MAP_READ, 0, &Mapped);
	if (R->Multithread) ID3D11Multithread_Leave(R->Multithread);

	if (FAILED(hr))
	{
		return FALSE;
	}

	*Data = Mapped.pData;
	*Pitch = Mapped.RowPitch;
	*Width = W;
	*Height = H;
	return TRUE;
}

static void CliReadback_Unmap(CliReadback* R)
{
	if (R->Multithread) ID3D11Multithread_Enter(R->Multithread);
	ID3D11DeviceContext_Unmap(R->Context, (ID3D11Resource*)R->Staging, 0);
	if (R->Multithread) ID3D11Multithread_Leave(R->Multithread);
}

//
// PNG writing with WIC, BGRA input, alpha is dropped
//

DEFINE_GUID(IID_IWICImagingFactory, 0xec5ec8a9, 0xc395, 0x4314, 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70);

static IWICImagingFactory* CliPng_CreateFactory(void)
{
	IWICImagingFactory* Factory = NULL;
	if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (LPVOID*)&Factory)))
	{
		return NULL;
	}
	return Factory;
}

static BOOL CliPng_Write(IWICImagingFactory* Factory, LPCWSTR Path, UINT Width, UINT Height, const BYTE* Pixels, size_t Pitch)
{
	BOOL Result = FALSE;
	IWICStream* Stream = NULL;
	IWICBitmapEncoder* Encoder = NULL;
	IWICBitmapFrameEncode* Frame = NULL;
	IPropertyBag2* Props = NULL;

	size_t Stride = ((size_t)Width * 3 + 3) & ~(size_t)3;
	BYTE* Bgr = CliAlloc(Stride * Height);
	if (!Bgr)
	{
		return FALSE;
	}
	for (UINT y = 0; y < Height; y++)
	{
		const BYTE* Src = Pixels + (size_t)y * Pitch;
		BYTE* Dst = Bgr + (size_t)y * Stride;
		for (UINT x = 0; x < Width; x++)
		{
			Dst[0] = Src[0];
			Dst[1] = Src[1];
			Dst[2] = Src[2];
			Src += 4;
			Dst += 3;
		}
	}

	WICPixelFormatGUID Format = GUID_WICPixelFormat24bppBGR;

	if (FAILED(IWICImagingFactory_CreateStream(Factory, &Stream))) goto done;
	if (FAILED(IWICStream_InitializeFromFilename(Stream, Path, GENERIC_WRITE))) goto done;
	if (FAILED(IWICImagingFactory_CreateEncoder(Factory, &GUID_ContainerFormatPng, NULL, &Encoder))) goto done;
	if (FAILED(IWICBitmapEncoder_Initialize(Encoder, (IStream*)Stream, WICBitmapEncoderNoCache))) goto done;
	if (FAILED(IWICBitmapEncoder_CreateNewFrame(Encoder, &Frame, &Props))) goto done;
	if (FAILED(IWICBitmapFrameEncode_Initialize(Frame, Props))) goto done;
	if (FAILED(IWICBitmapFrameEncode_SetSize(Frame, Width, Height))) goto done;
	if (FAILED(IWICBitmapFrameEncode_SetPixelFormat(Frame, &Format))) goto done;
	if (FAILED(IWICBitmapFrameEncode_WritePixels(Frame, Height, (UINT)Stride, (UINT)(Stride * Height), Bgr))) goto done;
	if (FAILED(IWICBitmapFrameEncode_Commit(Frame))) goto done;
	if (FAILED(IWICBitmapEncoder_Commit(Encoder))) goto done;
	Result = TRUE;

done:
	if (Props)   IPropertyBag2_Release(Props);
	if (Frame)   IWICBitmapFrameEncode_Release(Frame);
	if (Encoder) IWICBitmapEncoder_Release(Encoder);
	if (Stream)  IWICStream_Release(Stream);
	CliFree(Bgr);
	return Result;
}

static BOOL CliPng_WriteSync(LPCWSTR Path, UINT Width, UINT Height, const BYTE* Pixels, size_t Pitch)
{
	IWICImagingFactory* Factory = CliPng_CreateFactory();
	if (!Factory)
	{
		return FALSE;
	}
	BOOL Result = CliPng_Write(Factory, Path, Width, Height, Pixels, Pitch);
	IWICImagingFactory_Release(Factory);
	return Result;
}

//
// background PNG writer, used for --frames-dir so that capture callback is not blocked by compression
//

#define CLI_PNG_THREADS 4

typedef struct CliPngJob
{
	struct CliPngJob* Next;
	UINT Width;
	UINT Height;
	WCHAR Path[MAX_PATH];
	BYTE* Pixels;
}
CliPngJob;

typedef struct
{
	SRWLOCK Lock;
	CONDITION_VARIABLE Wake;
	CONDITION_VARIABLE Idle;
	CliPngJob* Head;
	CliPngJob* Tail;
	int Pending;
	int Active;
	int MaxPending;
	BOOL Quit;
	BOOL Started;
	HANDLE Threads[CLI_PNG_THREADS];
	UINT64 Written;
	UINT64 Failed;
	UINT64 Dropped;
	UINT64 Bytes;
}
CliPngQueue;

static DWORD WINAPI CliPngQueue__Thread(LPVOID Param)
{
	CliPngQueue* Queue = Param;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IWICImagingFactory* Factory = CliPng_CreateFactory();

	AcquireSRWLockExclusive(&Queue->Lock);
	for (;;)
	{
		while (!Queue->Head && !Queue->Quit)
		{
			SleepConditionVariableSRW(&Queue->Wake, &Queue->Lock, INFINITE, 0);
		}
		if (!Queue->Head)
		{
			break;
		}

		CliPngJob* Job = Queue->Head;
		Queue->Head = Job->Next;
		if (!Queue->Head)
		{
			Queue->Tail = NULL;
		}
		Queue->Pending--;
		Queue->Active++;
		ReleaseSRWLockExclusive(&Queue->Lock);

		BOOL Ok = Factory && CliPng_Write(Factory, Job->Path, Job->Width, Job->Height, Job->Pixels, (size_t)Job->Width * 4);
		UINT64 Size = 0;
		if (Ok)
		{
			WIN32_FILE_ATTRIBUTE_DATA Attributes;
			if (GetFileAttributesExW(Job->Path, GetFileExInfoStandard, &Attributes))
			{
				Size = ((UINT64)Attributes.nFileSizeHigh << 32) | Attributes.nFileSizeLow;
			}
		}
		CliFree(Job->Pixels);
		CliFree(Job);

		AcquireSRWLockExclusive(&Queue->Lock);
		Queue->Active--;
		if (Ok)
		{
			Queue->Written++;
			Queue->Bytes += Size;
		}
		else
		{
			Queue->Failed++;
		}
		WakeAllConditionVariable(&Queue->Idle);
	}
	ReleaseSRWLockExclusive(&Queue->Lock);

	if (Factory)
	{
		IWICImagingFactory_Release(Factory);
	}
	CoUninitialize();
	return 0;
}

static void CliPngQueue_Start(CliPngQueue* Queue, UINT FrameWidth, UINT FrameHeight)
{
	InitializeSRWLock(&Queue->Lock);
	InitializeConditionVariable(&Queue->Wake);
	InitializeConditionVariable(&Queue->Idle);

	// keep at most ~1GB of pending frames in memory
	UINT64 FrameBytes = (UINT64)FrameWidth * FrameHeight * 4;
	UINT64 Max = FrameBytes ? (1ULL << 30) / FrameBytes : 64;
	if (Max < 4) Max = 4;
	if (Max > 256) Max = 256;
	Queue->MaxPending = (int)Max;

	for (int i = 0; i < CLI_PNG_THREADS; i++)
	{
		Queue->Threads[i] = CreateThread(NULL, 0, &CliPngQueue__Thread, Queue, 0, NULL);
	}
	Queue->Started = TRUE;
}

// copies pixels, returns false if queue is full (frame is dropped)
static BOOL CliPngQueue_Add(CliPngQueue* Queue, LPCWSTR Path, const BYTE* Data, size_t Pitch, UINT Width, UINT Height)
{
	AcquireSRWLockShared(&Queue->Lock);
	BOOL Full = Queue->Pending >= Queue->MaxPending;
	ReleaseSRWLockShared(&Queue->Lock);
	if (Full)
	{
		Queue->Dropped++;
		return FALSE;
	}

	CliPngJob* Job = CliAlloc(sizeof(*Job));
	Job->Pixels = CliAlloc((size_t)Width * Height * 4);
	if (!Job->Pixels)
	{
		CliFree(Job);
		Queue->Dropped++;
		return FALSE;
	}
	Job->Width = Width;
	Job->Height = Height;
	StrCpyNW(Job->Path, Path, _countof(Job->Path));
	for (UINT y = 0; y < Height; y++)
	{
		CopyMemory(Job->Pixels + (size_t)y * Width * 4, Data + (size_t)y * Pitch, (size_t)Width * 4);
	}

	AcquireSRWLockExclusive(&Queue->Lock);
	if (Queue->Tail)
	{
		Queue->Tail->Next = Job;
	}
	else
	{
		Queue->Head = Job;
	}
	Queue->Tail = Job;
	Queue->Pending++;
	ReleaseSRWLockExclusive(&Queue->Lock);
	WakeConditionVariable(&Queue->Wake);
	return TRUE;
}

// waits for all queued frames to be written and stops threads
static void CliPngQueue_Finish(CliPngQueue* Queue)
{
	if (!Queue->Started)
	{
		return;
	}

	AcquireSRWLockExclusive(&Queue->Lock);
	while (Queue->Head || Queue->Active)
	{
		SleepConditionVariableSRW(&Queue->Idle, &Queue->Lock, INFINITE, 0);
	}
	Queue->Quit = TRUE;
	ReleaseSRWLockExclusive(&Queue->Lock);
	WakeAllConditionVariable(&Queue->Wake);

	WaitForMultipleObjects(CLI_PNG_THREADS, Queue->Threads, TRUE, INFINITE);
	for (int i = 0; i < CLI_PNG_THREADS; i++)
	{
		CloseHandle(Queue->Threads[i]);
	}
	Queue->Started = FALSE;
}

//
// mp4 decoding with Media Foundation source reader, frames are converted to BGRA
//

typedef struct
{
	IMFSourceReader* Reader;
	UINT Width;
	UINT Height;
	LONGLONG DurationHns;
	size_t Capacity;    // size of Pixels in bytes
	BOOL BottomUp;      // decoded RGB32 rows are stored bottom row first
	BYTE* Pixels;       // tight top-down BGRA of last frame that was read
	LONGLONG TimeHns;   // timestamp of Pixels
	BOOL HasFrame;
	BOOL Ended;
}
CliClip;

// reads size and orientation of decoded frames, reallocates Pixels when size changes
static BOOL CliClip__UpdateType(CliClip* Clip)
{
	IMFMediaType* Current;
	if (FAILED(IMFSourceReader_GetCurrentMediaType(Clip->Reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &Current)))
	{
		return FALSE;
	}
	UINT64 Size = 0;
	IMFMediaType_GetUINT64(Current, &MF_MT_FRAME_SIZE, &Size);
	UINT32 Stride = 0;
	BOOL BottomUp = FALSE;
	if (SUCCEEDED(IMFMediaType_GetUINT32(Current, &MF_MT_DEFAULT_STRIDE, &Stride)))
	{
		// RGB32 is bottom-up when stride is negative
		BottomUp = (INT32)Stride < 0;
	}
	IMFMediaType_Release(Current);

	UINT Width = (UINT)(Size >> 32);
	UINT Height = (UINT)(Size & 0xFFFFFFFF);
	if (Width == 0 || Height == 0 || Width > 16384 || Height > 16384)
	{
		return FALSE;
	}

	size_t Needed = (size_t)Width * Height * 4;
	if (Needed > Clip->Capacity)
	{
		CliFree(Clip->Pixels);
		Clip->Pixels = CliAlloc(Needed);
		Clip->Capacity = Needed;
		if (!Clip->Pixels)
		{
			return FALSE;
		}
	}
	Clip->Width = Width;
	Clip->Height = Height;
	Clip->BottomUp = BottomUp;
	return TRUE;
}

static BOOL CliClip_Open(CliClip* Clip, LPCWSTR Path)
{
	*Clip = (CliClip){ 0 };

	IMFAttributes* Attributes;
	if (FAILED(MFCreateAttributes(&Attributes, 1)))
	{
		return FALSE;
	}
	IMFAttributes_SetUINT32(Attributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
	HRESULT hr = MFCreateSourceReaderFromURL(Path, Attributes, &Clip->Reader);
	IMFAttributes_Release(Attributes);
	if (FAILED(hr))
	{
		return FALSE;
	}

	IMFSourceReader_SetStreamSelection(Clip->Reader, MF_SOURCE_READER_ALL_STREAMS, FALSE);
	IMFSourceReader_SetStreamSelection(Clip->Reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

	IMFMediaType* Type;
	MFCreateMediaType(&Type);
	IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
	IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
	hr = IMFSourceReader_SetCurrentMediaType(Clip->Reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, Type);
	IMFMediaType_Release(Type);
	if (FAILED(hr))
	{
		IMFSourceReader_Release(Clip->Reader);
		Clip->Reader = NULL;
		return FALSE;
	}

	if (!CliClip__UpdateType(Clip))
	{
		IMFSourceReader_Release(Clip->Reader);
		Clip->Reader = NULL;
		return FALSE;
	}

	PROPVARIANT Duration;
	PropVariantInit(&Duration);
	if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(Clip->Reader, MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &Duration)))
	{
		Clip->DurationHns = (LONGLONG)Duration.uhVal.QuadPart;
	}
	PropVariantClear(&Duration);

	return TRUE;
}

static void CliClip_Close(CliClip* Clip)
{
	if (Clip->Reader)
	{
		IMFSourceReader_Release(Clip->Reader);
	}
	CliFree(Clip->Pixels);
	*Clip = (CliClip){ 0 };
}

// reads next frame into Pixels, returns false at end of stream
static BOOL CliClip_Next(CliClip* Clip)
{
	while (!Clip->Ended)
	{
		DWORD Flags = 0;
		LONGLONG Time = 0;
		IMFSample* Sample = NULL;
		HRESULT hr = IMFSourceReader_ReadSample(Clip->Reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &Flags, &Time, &Sample);
		if (FAILED(hr) || (Flags & MF_SOURCE_READERF_ENDOFSTREAM) || (Flags & MF_SOURCE_READERF_ERROR))
		{
			if (Sample)
			{
				IMFSample_Release(Sample);
			}
			Clip->Ended = TRUE;
			return FALSE;
		}
		if (Flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
		{
			if (!CliClip__UpdateType(Clip))
			{
				if (Sample)
				{
					IMFSample_Release(Sample);
				}
				Clip->Ended = TRUE;
				return FALSE;
			}
		}
		if (!Sample)
		{
			continue;
		}

		BOOL Copied = FALSE;
		IMFMediaBuffer* Buffer;
		DWORD BufferLength = 0;
		if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(Sample, &Buffer)) && SUCCEEDED(IMFMediaBuffer_GetCurrentLength(Buffer, &BufferLength)))
		{
			// decoded RGB32 buffer is always at least Width*Height*4 bytes, do not trust it otherwise
			BOOL Big = BufferLength >= (DWORD)(Clip->Width * Clip->Height * 4);
			IMF2DBuffer* Buffer2D;
			if (!Big)
			{
				// skip frame
			}
			else if (SUCCEEDED(IMFMediaBuffer_QueryInterface(Buffer, &IID_IMF2DBuffer, (void**)&Buffer2D)))
			{
				BYTE* Scan0;
				LONG Pitch;
				if (SUCCEEDED(IMF2DBuffer_Lock2D(Buffer2D, &Scan0, &Pitch)))
				{
					// Scan0 with negative pitch already points to top row, otherwise memory order is bottom row first for bottom-up
					BOOL Flip = Clip->BottomUp && Pitch > 0;
					for (UINT y = 0; y < Clip->Height; y++)
					{
						UINT Row = Flip ? Clip->Height - 1 - y : y;
						CopyMemory(Clip->Pixels + (size_t)y * Clip->Width * 4, Scan0 + (ptrdiff_t)Row * Pitch, (size_t)Clip->Width * 4);
					}
					IMF2DBuffer_Unlock2D(Buffer2D);
					Copied = TRUE;
				}
				IMF2DBuffer_Release(Buffer2D);
			}
			else
			{
				BYTE* Data;
				DWORD Length;
				if (SUCCEEDED(IMFMediaBuffer_Lock(Buffer, &Data, NULL, &Length)) && Length >= (DWORD)(Clip->Width * Clip->Height * 4))
				{
					for (UINT y = 0; y < Clip->Height; y++)
					{
						UINT Row = Clip->BottomUp ? Clip->Height - 1 - y : y;
						CopyMemory(Clip->Pixels + (size_t)y * Clip->Width * 4, Data + (size_t)Row * Clip->Width * 4, (size_t)Clip->Width * 4);
					}
					Copied = TRUE;
				}
				IMFMediaBuffer_Unlock(Buffer);
			}
			IMFMediaBuffer_Release(Buffer);
		}
		IMFSample_Release(Sample);

		if (Copied)
		{
			Clip->TimeHns = Time;
			Clip->HasFrame = TRUE;
			return TRUE;
		}
	}
	return FALSE;
}

// positions on first frame with timestamp at or after Hns (or last frame of the clip if there is none)
static BOOL CliClip_SeekFrame(CliClip* Clip, LONGLONG Hns)
{
	PROPVARIANT Position;
	PropVariantInit(&Position);
	Position.vt = VT_I8;
	Position.hVal.QuadPart = Hns;
	if (FAILED(IMFSourceReader_SetCurrentPosition(Clip->Reader, &GUID_NULL, &Position)))
	{
		return FALSE;
	}
	Clip->Ended = FALSE;
	Clip->HasFrame = FALSE;

	// timestamps can be a bit off after seek, allow half a millisecond
	while (CliClip_Next(Clip))
	{
		if (Clip->TimeHns + 5000 >= Hns)
		{
			return TRUE;
		}
	}
	return Clip->HasFrame;
}

//
// contact sheet
//

typedef struct
{
	LPCWSTR Path;
	CliClip Clip;
}
CliSheetInput;

// cells are ordered row by row, each cell has a frame from clip Input[Index] at time Times[Cell]
typedef struct
{
	const CliSheetInput* Inputs;
	int InputCount;
	const double* Times;
	int TimeCount;
	int Columns;        // 0 = automatic
	BOOL Grid;          // wrap single clip into square-ish grid
	int CellWidth;      // 0 = native size of region
	int Region[4];      // x,y,w,h crop, all zero = full frame
	BOOL Labels;
}
CliSheetOptions;

static LPCWSTR CliFileName(LPCWSTR Path)
{
	LPCWSTR Name = Path;
	for (LPCWSTR p = Path; *p; p++)
	{
		if (*p == L'\\' || *p == L'/')
		{
			Name = p + 1;
		}
	}
	return Name;
}

// renders to png, returns 0 on success, otherwise error text
static LPCWSTR CliSheet_Render(const CliSheetOptions* Options, LPCWSTR OutputPath, UINT* OutWidth, UINT* OutHeight)
{
	const int Gutter = 6;

	int Cells = Options->InputCount * Options->TimeCount;
	int Columns, Rows;
	if (Options->InputCount > 1)
	{
		Columns = Options->TimeCount;
		Rows = Options->InputCount;
	}
	else
	{
		Columns = Options->TimeCount;
		if (Options->Columns > 0)
		{
			Columns = Options->Columns;
		}
		else if (Options->Grid)
		{
			Columns = 1;
			while (Columns * Columns < Cells)
			{
				Columns++;
			}
		}
		if (Columns > Cells) Columns = Cells;
		Rows = (Cells + Columns - 1) / Columns;
	}

	// all frames get scaled to the size of the first clip region
	const CliClip* First = &Options->Inputs[0].Clip;
	int Source[4] = { 0, 0, (int)First->Width, (int)First->Height };
	if (Options->Region[2] > 0 && Options->Region[3] > 0)
	{
		if (!CliClipRegion(Options->Region, (int)First->Width, (int)First->Height, Source))
		{
			return L"region is outside of the video";
		}
	}

	int CellW = Options->CellWidth > 0 ? Options->CellWidth : Source[2];
	int CellH = (int)((INT64)Source[3] * CellW / Source[2]);
	if (CellH < 1) CellH = 1;

	UINT SheetW = Columns * CellW + (Columns + 1) * Gutter;
	UINT SheetH = Rows * CellH + (Rows + 1) * Gutter;
	if ((UINT64)SheetW * SheetH > 200000000ULL)
	{
		return L"sheet is too large, use --width to make cells smaller";
	}

	BITMAPINFO SheetInfo =
	{
		.bmiHeader =
		{
			.biSize = sizeof(BITMAPINFOHEADER),
			.biWidth = (LONG)SheetW,
			.biHeight = -(LONG)SheetH,
			.biPlanes = 1,
			.biBitCount = 32,
			.biCompression = BI_RGB,
		},
	};

	void* Bits;
	HDC Context = CreateCompatibleDC(NULL);
	HBITMAP Bitmap = CreateDIBSection(Context, &SheetInfo, DIB_RGB_COLORS, &Bits, NULL, 0);
	if (!Bitmap)
	{
		DeleteDC(Context);
		return L"cannot allocate sheet image";
	}
	HGDIOBJ OldBitmap = SelectObject(Context, Bitmap);

	RECT All = { 0, 0, (LONG)SheetW, (LONG)SheetH };
	HBRUSH Background = CreateSolidBrush(RGB(32, 32, 32));
	FillRect(Context, &All, Background);
	DeleteObject(Background);

	SetStretchBltMode(Context, HALFTONE);
	SetBrushOrgEx(Context, 0, 0, NULL);
	SetBkMode(Context, OPAQUE);
	SetBkColor(Context, RGB(0, 0, 0));
	SetTextColor(Context, RGB(255, 255, 255));
	HGDIOBJ OldFont = SelectObject(Context, GetStockObject(DEFAULT_GUI_FONT));

	LPCWSTR Error = NULL;
	for (int Cell = 0; Cell < Cells && !Error; Cell++)
	{
		int InputIndex = Options->InputCount > 1 ? Cell / Options->TimeCount : 0;
		int TimeIndex = Options->InputCount > 1 ? Cell % Options->TimeCount : Cell;
		CliSheetInput* Input = (CliSheetInput*)&Options->Inputs[InputIndex];
		double Time = Options->Times[TimeIndex];

		if (!CliClip_SeekFrame(&Input->Clip, (LONGLONG)(Time * 10000000.0)))
		{
			Error = L"cannot read frame from video";
			break;
		}

		int Region[4];
		int Full[4] = { 0, 0, (int)Input->Clip.Width, (int)Input->Clip.Height };
		if (Options->Region[2] > 0 && Options->Region[3] > 0)
		{
			if (!CliClipRegion(Options->Region, (int)Input->Clip.Width, (int)Input->Clip.Height, Region))
			{
				Error = L"region is outside of the video";
				break;
			}
		}
		else
		{
			CopyMemory(Region, Full, sizeof(Region));
		}

		// source is sub image that starts at region top row, this way y source is always 0 regardless of DIB orientation
		BITMAPINFO FrameInfo =
		{
			.bmiHeader =
			{
				.biSize = sizeof(BITMAPINFOHEADER),
				.biWidth = (LONG)Input->Clip.Width,
				.biHeight = -(LONG)Region[3],
				.biPlanes = 1,
				.biBitCount = 32,
				.biCompression = BI_RGB,
			},
		};
		const BYTE* SubImage = Input->Clip.Pixels + (size_t)Region[1] * Input->Clip.Width * 4;

		int Column = Cell % Columns;
		int Row = Cell / Columns;
		int X = Gutter + Column * (CellW + Gutter);
		int Y = Gutter + Row * (CellH + Gutter);

		StretchDIBits(Context, X, Y, CellW, CellH, Region[0], 0, Region[2], Region[3],
			SubImage, &FrameInfo, DIB_RGB_COLORS, SRCCOPY);

		if (Options->Labels)
		{
			WCHAR Label[MAX_PATH + 32];
			if (Options->InputCount > 1)
			{
				StrFormat(Label, L" %ls  %.2fs ", CliFileName(Input->Path), (double)Input->Clip.TimeHns / 10000000.0);
			}
			else
			{
				StrFormat(Label, L" %.2fs ", (double)Input->Clip.TimeHns / 10000000.0);
			}
			Label[_countof(Label) - 1] = 0;
			TextOutW(Context, X + 2, Y + 2, Label, lstrlenW(Label));
		}
	}

	if (!Error)
	{
		GdiFlush();
		if (!CliPng_WriteSync(OutputPath, SheetW, SheetH, Bits, (size_t)SheetW * 4))
		{
			Error = L"cannot write output png";
		}
	}

	SelectObject(Context, OldFont);
	SelectObject(Context, OldBitmap);
	DeleteObject(Bitmap);
	DeleteDC(Context);

	*OutWidth = SheetW;
	*OutHeight = SheetH;
	return Error;
}

//
// recording session state for wcap-cli record
//

typedef struct
{
	UINT64 Qpc;
	float Diff;
	BYTE Unique;
	int Out;        // index of frame in output (mp4 frame or png file), -1 if frame was not written
}
CliFrameLog;

typedef struct
{
	BOOL FramesOnly;        // png sequence instead of mp4
	BOOL Analyze;           // compare frames to find duplicates
	BOOL Vfr;               // do not write frames identical to previous one
	BOOL Json;
	BOOL Timestamps;
	BOOL MeasureFlicker;
	BOOL ForceWrite;        // last frame that should have been written was dropped, so next one is written even if identical
	WCHAR FramesDir[MAX_PATH];
	WCHAR TimestampsPath[MAX_PATH];
	WCHAR StartOn[MAX_PATH + 16];
	DWORD StartTimeoutMs;

	CliReadback Readback;
	CliTemporal Temporal;
	CliPngQueue Png;

	CliFrameLog* Log;
	size_t LogCount;
	size_t LogCapacity;

	UINT64 Considered;      // frames that were considered for encoding
	UINT64 Duplicated;      // considered frames identical to previous considered frame
	UINT64 LimitedByFps;    // frames skipped because of --fps limit
	UINT64 OutCount;        // frames written
	UINT64 FirstQpc;
	UINT64 LastQpc;
	UINT Width;
	UINT Height;

	WCHAR Warnings[8][256];
	int WarningCount;
}
CliSession;

static CliSession gCli;

// called for every frame that is going to be encoded, returns true if frame should go to mp4 encoder
static BOOL CliOnFrame(const ScreenCaptureFrame* Frame)
{
	CliSession* S = &gCli;

	UINT W = Frame->Rect.right - Frame->Rect.left;
	UINT H = Frame->Rect.bottom - Frame->Rect.top;
	if (S->Considered == 0)
	{
		S->FirstQpc = Frame->Time;
		S->Width = W;
		S->Height = H;
	}
	S->LastQpc = Frame->Time;
	S->Considered++;

	BOOL Unique = TRUE;
	double Diff = 0;
	int Out = -1;
	BOOL Skip = FALSE; // identical frame that is not written in --vfr mode

	if (S->Analyze || S->FramesOnly)
	{
		const BYTE* Data;
		UINT Pitch, ReadWidth, ReadHeight;
		if (CliReadback_Map(&S->Readback, Frame->Texture, Frame->Rect, &Data, &Pitch, &ReadWidth, &ReadHeight))
		{
			if (S->Analyze)
			{
				Unique = CliTemporal_Add(&S->Temporal, Data, Pitch, ReadWidth, ReadHeight, &Diff);
			}
			Skip = S->Vfr && !Unique && !S->ForceWrite;
			if (S->FramesOnly && !Skip)
			{
				WCHAR Path[MAX_PATH];
				StrFormat(Path, L"%ls\\frame_%06I64u.png", S->FramesDir, S->OutCount);
				Path[_countof(Path) - 1] = 0;
				if (CliPngQueue_Add(&S->Png, Path, Data, Pitch, ReadWidth, ReadHeight))
				{
					Out = (int)S->OutCount++;
					S->ForceWrite = FALSE;
				}
				else
				{
					S->ForceWrite = TRUE;
				}
			}
			CliReadback_Unmap(&S->Readback);
		}
		else if (S->FramesOnly)
		{
			// cannot read frame back, count as dropped and make sure next frame is written
			S->Png.Dropped++;
			S->ForceWrite = TRUE;
		}
	}
	if (!Unique)
	{
		S->Duplicated++;
	}

		if (S->Timestamps)
	{
		if (S->LogCount == S->LogCapacity)
		{
			S->LogCapacity = S->LogCapacity ? S->LogCapacity * 2 : 4096;
			S->Log = CliRealloc(S->Log, S->LogCapacity * sizeof(CliFrameLog));
		}
		S->Log[S->LogCount++] = (CliFrameLog){ .Qpc = Frame->Time, .Diff = (float)Diff, .Unique = Unique, .Out = Out };
	}

	return !S->FramesOnly && !Skip;
}

// called when mp4 encoder could not take the frame from CliOnFrame
static void CliFrameDropped(void)
{
	// content of dropped frame is lost, so next frame must be written even if it is identical to it
	gCli.ForceWrite = TRUE;
}

// called after frame from CliOnFrame was successfully sent to mp4 encoder
static void CliFrameEncoded(void)
{
	CliSession* S = &gCli;
	gCli.ForceWrite = FALSE;
	int Out = (int)S->OutCount++;
	if (S->Timestamps && S->LogCount)
	{
		S->Log[S->LogCount - 1].Out = Out;
	}
}

static void CliWarn(LPCWSTR Format, ...)
{
	WCHAR Text[256];
	va_list Args;
	va_start(Args, Format);
	_vsnwprintf(Text, _countof(Text), Format, Args);
	va_end(Args);
	Text[_countof(Text) - 1] = 0;

	WriteText(STD_ERROR_HANDLE, L"warning: ");
	WriteText(STD_ERROR_HANDLE, Text);
	WriteText(STD_ERROR_HANDLE, L"\n");

	if (gCli.WarningCount < _countof(gCli.Warnings))
	{
		StrCpyNW(gCli.Warnings[gCli.WarningCount++], Text, _countof(gCli.Warnings[0]));
	}
}

static void CliText_Warnings(CliText* Text)
{
	CliText_Char(Text, L'[');
	for (int i = 0; i < gCli.WarningCount; i++)
	{
		if (i)
		{
			CliText_Char(Text, L',');
		}
		CliText_Json(Text, gCli.Warnings[i]);
	}
	CliText_Char(Text, L']');
}
