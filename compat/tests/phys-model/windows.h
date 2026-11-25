#pragma once
// Minimal declarations for the Linux state-machine test of phys.h.
// Not a Windows ABI emulator; the PE probe uses real Windows headers/APIs.
#include <cstddef>
#include <cstdint>
#define WINAPI
using PVOID=void*;using HANDLE=void*;using HMODULE=void*;
using DWORD=uint32_t;using ULONG=uint32_t;using ULONG64=uint64_t;
using SIZE_T=size_t;using BOOL=int;
#define MEM_RESERVE 0x2000
#define MEM_COMMIT 0x1000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 1
#define PAGE_READWRITE 4
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
DWORD GetEnvironmentVariableA(const char*,char*,DWORD);
HMODULE LoadLibraryW(const wchar_t*);
void* GetProcAddress(HMODULE,const char*);
HANDLE CreateFileMappingW(HANDLE,void*,DWORD,DWORD,DWORD,const wchar_t*);
DWORD GetLastError();
BOOL VirtualFree(PVOID,SIZE_T,DWORD);
HANDLE GetCurrentProcess();
