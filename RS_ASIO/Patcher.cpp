#include "stdafx.h"
#include "dllmain.h"
#include "crc32.h"

typedef ULONG NTSTATUS;

typedef NTSTATUS(WINAPI* Type_NtProtectVirtualMemory)(HANDLE /*ProcessHandle*/, LPVOID* /*BaseAddress*/, SIZE_T* /*NumberOfBytesToProtect*/, ULONG /*NewAccessProtection*/, PULONG /*OldAccessProtection*/);

static Type_NtProtectVirtualMemory pfnNtProtectVirtualMemory = nullptr;

EXTERN_C ULONG NtProtectVirtualMemory(
	IN HANDLE ProcessHandle,
	IN OUT PVOID* BaseAddress,
	IN OUT PSIZE_T RegionSize,
	IN ULONG NewProtect,
	OUT PULONG OldProtect
);

static bool RedirectedVirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect)
{
	if (pfnNtProtectVirtualMemory != nullptr)
	{
		const HANDLE ProcessHandle = GetCurrentProcess();
		SIZE_T NumberOfBytesToProtect = dwSize;
		return (pfnNtProtectVirtualMemory(ProcessHandle, &lpAddress, &NumberOfBytesToProtect, flNewProtect, lpflOldProtect) == 0);
	}

	return NtProtectVirtualMemory(GetCurrentProcess(), &lpAddress, &dwSize, flNewProtect, lpflOldProtect) == 0;
}

DWORD GetImageCrc32()
{
	char exePath[MAX_PATH]{};
	DWORD exePathSize = GetModuleFileNameA(NULL, exePath, MAX_PATH);

	DWORD crc = 0;
	bool success = crc32file(exePath, crc);
	if (!success)
	{
		rslog::error_ts() << "Could not get the executable crc32" << std::endl;
		return 0;
	}

	return crc;
}

void PatchOriginalCode_e0f686e0();

void Patch_ReplaceWithNops(void* offset, size_t numBytes)
{
	DWORD oldProtectFlags = 0;
	if (!RedirectedVirtualProtect(offset, numBytes, PAGE_WRITECOPY, &oldProtectFlags))
	{
		rslog::error_ts() << "Failed to change memory protection" << std::endl;
	}
	else
	{
		BYTE* byte = (BYTE*)offset;
		for (size_t i = 0; i < numBytes; ++i)
		{
			byte[i] = 0x90; // nop
		}

		FlushInstructionCache(GetCurrentProcess(), offset, numBytes);
		if (!RedirectedVirtualProtect(offset, numBytes, oldProtectFlags, &oldProtectFlags))
		{
			rslog::error_ts() << "Failed to restore memory protection" << std::endl;
		}
	}
}

void Patch_ReplaceWithBytes(void* offset, size_t numBytes, const BYTE* replaceBytes)
{
	DWORD oldProtectFlags = 0;
	if (!RedirectedVirtualProtect(offset, numBytes, PAGE_WRITECOPY, &oldProtectFlags))
	{
		rslog::error_ts() << "Failed to change memory protection" << std::endl;
	}
	else
	{
		BYTE* byte = (BYTE*)offset;
		for (size_t i = 0; i < numBytes; ++i)
		{
			byte[i] = replaceBytes[i];
		}

		FlushInstructionCache(GetCurrentProcess(), offset, numBytes);
		if (!RedirectedVirtualProtect(offset, numBytes, oldProtectFlags, &oldProtectFlags))
		{
			rslog::error_ts() << "Failed to restore memory protection" << std::endl;
		}
	}
}

void InitPatcher()
{
	HMODULE ntdllMod = GetModuleHandleA("ntdll.dll");
	if (!ntdllMod)
	{
		rslog::error_ts() << "Failed get handle for ntdll.dll" << std::endl;
		return;
	}

	pfnNtProtectVirtualMemory = (Type_NtProtectVirtualMemory)GetProcAddress(ntdllMod, "NtProtectVirtualMemory");
	if (!pfnNtProtectVirtualMemory)
	{
		rslog::error_ts() << "Failed get proc address for NtProtectVirtualMemory in ntdll.dll" << std::endl;
		return;
	}
}

void DeinitPatcher()
{
}

void PatchOriginalCode()
{
	rslog::info_ts() << __FUNCTION__ << std::endl;

	const DWORD image_crc32 = GetImageCrc32();

	char image_crc32_str[16] = { 0 };
	snprintf(image_crc32_str, 15, "0x%08x", image_crc32);

	rslog::info_ts() << "image crc32: " << image_crc32_str << std::endl;

	switch (image_crc32)
	{
		case 0xe0f686e0:
			PatchOriginalCode_e0f686e0();
			break;
		default:
			rslog::error_ts() << "Unknown game version" << std::endl;
			break;
	}
}