#include "stdafx.h"
#include "dllmain.h"
#include "Patcher.h"
#include <setupapi.h>
#pragma comment(lib, "setupapi.lib")

// Patch code for Rocksmith (2011), CRC32 0xe0f686e0
//
// The Rocksmith 2011 executable encrypts its .text section on disk using a custom
// packer stub (PSFD00 section), so call-site byte patterns cannot be found statically.
// Instead, we patch the Import Address Table (IAT) directly.
//
// The IAT is in .rdata (unencrypted) and is filled by the loader before our DLL runs.
// We overwrite the function pointer slots for the COM functions that PortAudio/WASAPI
// uses to enumerate audio devices, replacing them with our own implementations.
//
// IAT entry RVAs (relative to image base 0x00400000, verified from the PE headers):
//
//   CoCreateInstance                      RVA 0x0088d47c  (ole32.dll)
//   CoMarshalInterThreadInterfaceInStream  RVA 0x0088d490  (ole32.dll)
//   CoGetInterfaceAndReleaseStream         RVA 0x0088d494  (ole32.dll)
//
//   SetupDiGetClassDevsW                  RVA 0x0088d2bc  (setupapi.dll)
//   SetupDiOpenDeviceInterfaceRegKey      RVA 0x0088d2c0  (setupapi.dll)
//   SetupDiGetDeviceRegistryPropertyW     RVA 0x0088d2c4  (setupapi.dll)
//   SetupDiGetDeviceInterfaceDetailW      RVA 0x0088d2c8  (setupapi.dll)
//   SetupDiDestroyDeviceInfoList          RVA 0x0088d2cc  (setupapi.dll)
//   SetupDiGetDeviceInterfaceAlias        RVA 0x0088d2d0  (setupapi.dll)
//   SetupDiGetClassDevsA                  RVA 0x0088d2d4  (setupapi.dll)
//   SetupDiEnumDeviceInterfaces           RVA 0x0088d2d8  (setupapi.dll)
//
//   RegQueryValueExW                      RVA 0x0088d000  (advapi32.dll)
//   RegCloseKey                           RVA 0x0088d004  (advapi32.dll)

// Known IAT RVAs - these are fixed offsets from the module base and do not change
// because the executable does not use ASLR (ImageBase is fixed at 0x00400000).
static const DWORD IAT_RVA_CoCreateInstance                     = 0x0088d47c;
static const DWORD IAT_RVA_CoMarshalInterThreadInterfaceInStream = 0x0088d490;
static const DWORD IAT_RVA_CoGetInterfaceAndReleaseStream        = 0x0088d494;

static const DWORD IAT_RVA_SetupDiGetClassDevsW             = 0x0088d2bc;
static const DWORD IAT_RVA_SetupDiOpenDeviceInterfaceRegKey = 0x0088d2c0;
static const DWORD IAT_RVA_SetupDiGetDeviceRegistryPropertyW= 0x0088d2c4;
static const DWORD IAT_RVA_SetupDiGetDeviceInterfaceDetailW = 0x0088d2c8;
static const DWORD IAT_RVA_SetupDiDestroyDeviceInfoList     = 0x0088d2cc;
static const DWORD IAT_RVA_SetupDiGetDeviceInterfaceAlias   = 0x0088d2d0;
static const DWORD IAT_RVA_SetupDiGetClassDevsA             = 0x0088d2d4;
static const DWORD IAT_RVA_SetupDiEnumDeviceInterfaces      = 0x0088d2d8;

static const DWORD IAT_RVA_RegQueryValueExW = 0x0088d000;
static const DWORD IAT_RVA_RegCloseKey      = 0x0088d004;

// Sentinel value used as an HDEVINFO handle for our fake cable device set.
// Must not collide with real heap pointers; RS2011 is 32-bit and heap starts well
// above 0x10000, so a small constant is safe.
static const HDEVINFO FAKE_DEVINFO = reinterpret_cast<HDEVINFO>(static_cast<ULONG_PTR>(0xC0FFEE01));

// Sentinel HKEY for our fake cable's device interface registry key.
static const HKEY FAKE_CABLE_HKEY = reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0xC0FFEE02));

// The WASAPI endpoint ID for the fake cable device - written under every plausible
// value name so whatever key name the game reads, it will get the right ID.
static const wchar_t FAKE_WASAPI_ENDPOINT_ID[] =
    L"{0.0.1.00000000}.{21D5646C-D708-4E90-A57A-E1956015D4F3}";

// Device interface path that the game will parse for VID_12BA&PID_00FF.
// The GUID suffix must match KSCATEGORY_CAPTURE = {65E8773D-8F56-11D0-A3B9-00A0C9223196}.
static const wchar_t FAKE_DEVICE_PATH[] =
    L"\\\\?\\USB#VID_12BA&PID_00FF#0001#{65e8773d-8f56-11d0-a3b9-00a0c9223196}";

// KSCATEGORY_CAPTURE GUID, used to filter which SetupDiGetClassDevs calls we intercept.
static const GUID GUID_KSCATEGORY_CAPTURE =
    { 0x65E8773D, 0x8F56, 0x11D0, { 0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };

// Patch a single IAT slot to point to replacementFn.
// Patch_ReplaceWithBytes (from Patcher.h) handles page protection internally
// via NtProtectVirtualMemory, bypassing any hook on VirtualProtect.
static bool PatchIATEntry(DWORD rva, void* replacementFn, const char* fnName)
{
	const HMODULE hBase = GetModuleHandle(NULL);
	if (!hBase)
	{
		rslog::error_ts() << "PatchIATEntry: GetModuleHandle failed for " << fnName << std::endl;
		return false;
	}

	void** iatSlot = reinterpret_cast<void**>(reinterpret_cast<BYTE*>(hBase) + rva);

	rslog::info_ts() << "PatchIATEntry: patching " << fnName
	                 << " at " << iatSlot
	                 << "  old=" << *iatSlot
	                 << "  new=" << replacementFn << std::endl;

	Patch_ReplaceWithBytes(iatSlot, sizeof(void*), reinterpret_cast<const BYTE*>(&replacementFn));
	return true;
}

// Thin wrappers that match the calling convention of the ole32 functions we are replacing.
// CoMarshalInterThreadInterfaceInStream / CoGetInterfaceAndReleaseStream are patched to
// no-op / identity replacements so that PortAudio's cross-thread COM marshaling does not
// interfere with our fake WASAPI device objects.
//
// NOTE: These are only used if RS2011's embedded PortAudio performs cross-apartment
// marshaling the same way RS2014's does. The game may work without them; if it crashes
// after CoCreateInstance is redirected successfully, these patches are the next step.

static HRESULT STDAPICALLTYPE Patched_CoMarshalInterThreadInterfaceInStream(
    REFIID riid, IUnknown* pUnk, IStream** ppStm)
{
	rslog::info_ts() << "Patched_CoMarshalInterThreadInterfaceInStream called - riid: " << riid << " pUnk: " << pUnk << std::endl;
	// Return the interface directly as the "stream" — PortAudio will read it back
	// via Patched_CoGetInterfaceAndReleaseStream below.
	if (!ppStm)
		return E_POINTER;
	*ppStm = reinterpret_cast<IStream*>(pUnk);
	if (pUnk)
		pUnk->AddRef();
	return S_OK;
}

static HRESULT STDAPICALLTYPE Patched_CoGetInterfaceAndReleaseStream(
    IStream* pStm, REFIID riid, void** ppv)
{
	rslog::info_ts() << "Patched_CoGetInterfaceAndReleaseStream called - riid: " << riid << " pStm: " << pStm << std::endl;
	if (!ppv)
		return E_POINTER;
	// The "stream" is actually the interface pointer stored by Patched_CoMarshalInterThreadInterfaceInStream.
	// The real CoGetInterfaceAndReleaseStream deserializes and returns the pointer that was originally
	// marshaled — it does NOT call QueryInterface on the result (the stream data already encodes the
	// correct interface type). We replicate that: just hand back the stored pointer directly.
	// The AddRef performed during marshal transfers to the caller; no additional AddRef or Release needed.
	IUnknown* pUnk = reinterpret_cast<IUnknown*>(pStm);
	if (!pUnk)
	{
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	*ppv = pUnk;
	return S_OK;
}

// ---- SetupDi fakes --------------------------------------------------------
// RS2011 calls SetupDiGetClassDevsW(KSCATEGORY_CAPTURE, ...) to verify the Real Tone
// Cable is physically connected before activating the WASAPI capture path. We return a
// fake device info set that contains one device with the expected VID_12BA&PID_00FF path
// so the game proceeds to WASAPI regardless of cable presence.

static HDEVINFO WINAPI Patched_SetupDiGetClassDevsW(
    const GUID* ClassGuid, PCWSTR Enumerator, HWND hwndParent, DWORD Flags)
{
	if (ClassGuid)
		rslog::info_ts() << "Patched_SetupDiGetClassDevsW - ClassGuid: " << *ClassGuid << std::endl;
	else
		rslog::info_ts() << "Patched_SetupDiGetClassDevsW - ClassGuid: (null)" << std::endl;
	// Always return our fake handle so the game believes the cable is present.
	// For classes other than audio capture this is safe: the game has no other SetupDi
	// usage that would be confused by a fake cable device in the enumeration result.
	return FAKE_DEVINFO;
}

static HDEVINFO WINAPI Patched_SetupDiGetClassDevsA(
    const GUID* ClassGuid, PCSTR Enumerator, HWND hwndParent, DWORD Flags)
{
	if (ClassGuid)
		rslog::info_ts() << "Patched_SetupDiGetClassDevsA - ClassGuid: " << *ClassGuid << std::endl;
	else
		rslog::info_ts() << "Patched_SetupDiGetClassDevsA - ClassGuid: (null)" << std::endl;
	return FAKE_DEVINFO;
}

static BOOL WINAPI Patched_SetupDiEnumDeviceInterfaces(
    HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData,
    const GUID* InterfaceClassGuid, DWORD MemberIndex,
    PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData)
{
	if (DeviceInfoSet != FAKE_DEVINFO)
		return SetupDiEnumDeviceInterfaces(DeviceInfoSet, DeviceInfoData,
		                                   InterfaceClassGuid, MemberIndex, DeviceInterfaceData);

	rslog::info_ts() << "Patched_SetupDiEnumDeviceInterfaces - MemberIndex: " << MemberIndex << std::endl;
	if (MemberIndex == 0 && DeviceInterfaceData)
	{
		DeviceInterfaceData->cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
		DeviceInterfaceData->InterfaceClassGuid = InterfaceClassGuid ? *InterfaceClassGuid : GUID_KSCATEGORY_CAPTURE;
		DeviceInterfaceData->Flags = SPINT_ACTIVE;
		DeviceInterfaceData->Reserved = 0;
		return TRUE;
	}
	SetLastError(ERROR_NO_MORE_ITEMS);
	return FALSE;
}

static BOOL WINAPI Patched_SetupDiGetDeviceInterfaceDetailW(
    HDEVINFO DeviceInfoSet, PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W DeviceInterfaceDetailData,
    DWORD DeviceInterfaceDetailDataSize, PDWORD RequiredSize,
    PSP_DEVINFO_DATA DeviceInfoData)
{
	if (DeviceInfoSet != FAKE_DEVINFO)
		return SetupDiGetDeviceInterfaceDetailW(DeviceInfoSet, DeviceInterfaceData,
		    DeviceInterfaceDetailData, DeviceInterfaceDetailDataSize, RequiredSize, DeviceInfoData);

	rslog::info_ts() << "Patched_SetupDiGetDeviceInterfaceDetailW called" << std::endl;

	// Size needed: struct header (cbSize DWORD) + wide string including null terminator.
	// SP_DEVICE_INTERFACE_DETAIL_DATA_W has cbSize(4) + DevicePath[1](2) but we need
	// room for the full path, so required = offsetof(...,DevicePath) + (len+1)*sizeof(WCHAR).
	DWORD pathBytes = static_cast<DWORD>((wcslen(FAKE_DEVICE_PATH) + 1) * sizeof(WCHAR));
	DWORD needed = FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + pathBytes;

	if (RequiredSize)
		*RequiredSize = needed;

	if (!DeviceInterfaceDetailData)
	{
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (DeviceInterfaceDetailDataSize < needed)
	{
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	DeviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
	wcscpy(DeviceInterfaceDetailData->DevicePath, FAKE_DEVICE_PATH);

	if (DeviceInfoData)
		ZeroMemory(DeviceInfoData, sizeof(SP_DEVINFO_DATA));

	return TRUE;
}

static BOOL WINAPI Patched_SetupDiDestroyDeviceInfoList(HDEVINFO DeviceInfoSet)
{
	if (DeviceInfoSet == FAKE_DEVINFO)
		return TRUE;
	return SetupDiDestroyDeviceInfoList(DeviceInfoSet);
}

static HKEY WINAPI Patched_SetupDiOpenDeviceInterfaceRegKey(
    HDEVINFO DeviceInfoSet, PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
    DWORD Reserved, REGSAM samDesired)
{
	rslog::info_ts() << "Patched_SetupDiOpenDeviceInterfaceRegKey called" << std::endl;
	if (DeviceInfoSet == FAKE_DEVINFO)
		return FAKE_CABLE_HKEY;
	return SetupDiOpenDeviceInterfaceRegKey(DeviceInfoSet, DeviceInterfaceData, Reserved, samDesired);
}

static LONG WINAPI Patched_RegQueryValueExW(
    HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
    LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
	if (hKey != FAKE_CABLE_HKEY)
		return RegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);

	rslog::info_ts() << "Patched_RegQueryValueExW (fake cable key) - ValueName: "
	                 << (lpValueName ? lpValueName : L"(null)") << std::endl;

	// The game reads "FriendlyName" from the device interface registry key to match
	// the USB device against the WASAPI endpoint found during enumeration.
	// Return the same friendly name that the fake WASAPI device reports so the match succeeds.
	const wchar_t* value = FAKE_WASAPI_ENDPOINT_ID;
	if (lpValueName && _wcsicmp(lpValueName, L"FriendlyName") == 0)
		value = L"Rocksmith Guitar Adapter Mono";

	DWORD needed = static_cast<DWORD>((wcslen(value) + 1) * sizeof(WCHAR));
	if (lpType) *lpType = REG_SZ;
	if (lpcbData)
	{
		DWORD available = *lpcbData;
		*lpcbData = needed;
		if (!lpData)
			return ERROR_SUCCESS;
		if (available < needed)
			return ERROR_MORE_DATA;
	}
	if (lpData)
		memcpy(lpData, value, needed);
	return ERROR_SUCCESS;
}

static LONG WINAPI Patched_RegCloseKey(HKEY hKey)
{
	if (hKey == FAKE_CABLE_HKEY)
		return ERROR_SUCCESS;
	return RegCloseKey(hKey);
}

static BOOL WINAPI Patched_SetupDiGetDeviceRegistryPropertyW(
    HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, DWORD Property,
    PDWORD PropertyRegDataType, PBYTE PropertyBuffer, DWORD PropertyBufferSize,
    PDWORD RequiredSize)
{
	rslog::info_ts() << "Patched_SetupDiGetDeviceRegistryPropertyW - Property: " << Property << std::endl;
	if (DeviceInfoSet == FAKE_DEVINFO)
	{
		// Return a hardware ID multi-string so VID/PID is discoverable via this path too.
		if (Property == SPDRP_HARDWAREID)
		{
			static const wchar_t hwid[] = L"USB\\VID_12BA&PID_00FF&REV_0103\0USB\\VID_12BA&PID_00FF\0";
			DWORD needed = sizeof(hwid);
			if (RequiredSize) *RequiredSize = needed;
			if (PropertyRegDataType) *PropertyRegDataType = REG_MULTI_SZ;
			if (PropertyBuffer && PropertyBufferSize >= needed)
			{
				memcpy(PropertyBuffer, hwid, needed);
				return TRUE;
			}
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	return SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, DeviceInfoData, Property,
	    PropertyRegDataType, PropertyBuffer, PropertyBufferSize, RequiredSize);
}

static BOOL WINAPI Patched_SetupDiGetDeviceInterfaceAlias(
    HDEVINFO DeviceInfoSet, PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
    const GUID* AliasInterfaceClassGuid, PSP_DEVICE_INTERFACE_DATA AliasDeviceInterfaceData)
{
	if (AliasInterfaceClassGuid)
		rslog::info_ts() << "Patched_SetupDiGetDeviceInterfaceAlias - AliasGuid: " << *AliasInterfaceClassGuid << std::endl;
	else
		rslog::info_ts() << "Patched_SetupDiGetDeviceInterfaceAlias - AliasGuid: (null)" << std::endl;

	if (DeviceInfoSet != FAKE_DEVINFO)
		return SetupDiGetDeviceInterfaceAlias(DeviceInfoSet, DeviceInterfaceData,
		    AliasInterfaceClassGuid, AliasDeviceInterfaceData);

	// Report that the alias exists and is active so the game proceeds to GetDeviceInterfaceDetailW.
	if (AliasDeviceInterfaceData)
	{
		AliasDeviceInterfaceData->cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
		AliasDeviceInterfaceData->InterfaceClassGuid = AliasInterfaceClassGuid ? *AliasInterfaceClassGuid : GUID_KSCATEGORY_CAPTURE;
		AliasDeviceInterfaceData->Flags = SPINT_ACTIVE;
		AliasDeviceInterfaceData->Reserved = 0;
	}
	return TRUE;
}

void PatchOriginalCode_e0f686e0()
{
	rslog::info_ts() << __FUNCTION__ << " - patching Rocksmith 2011 via IAT" << std::endl;

	bool ok = true;

	ok &= PatchIATEntry(IAT_RVA_CoCreateInstance,
	                    reinterpret_cast<void*>(&Patched_CoCreateInstance),
	                    "CoCreateInstance");

	ok &= PatchIATEntry(IAT_RVA_CoMarshalInterThreadInterfaceInStream,
	                    reinterpret_cast<void*>(&Patched_CoMarshalInterThreadInterfaceInStream),
	                    "CoMarshalInterThreadInterfaceInStream");

	ok &= PatchIATEntry(IAT_RVA_CoGetInterfaceAndReleaseStream,
	                    reinterpret_cast<void*>(&Patched_CoGetInterfaceAndReleaseStream),
	                    "CoGetInterfaceAndReleaseStream");

	ok &= PatchIATEntry(IAT_RVA_SetupDiGetClassDevsW,
	                    reinterpret_cast<void*>(&Patched_SetupDiGetClassDevsW),
	                    "SetupDiGetClassDevsW");
	ok &= PatchIATEntry(IAT_RVA_SetupDiOpenDeviceInterfaceRegKey,
	                    reinterpret_cast<void*>(&Patched_SetupDiOpenDeviceInterfaceRegKey),
	                    "SetupDiOpenDeviceInterfaceRegKey");
	ok &= PatchIATEntry(IAT_RVA_SetupDiGetDeviceRegistryPropertyW,
	                    reinterpret_cast<void*>(&Patched_SetupDiGetDeviceRegistryPropertyW),
	                    "SetupDiGetDeviceRegistryPropertyW");
	ok &= PatchIATEntry(IAT_RVA_SetupDiGetDeviceInterfaceDetailW,
	                    reinterpret_cast<void*>(&Patched_SetupDiGetDeviceInterfaceDetailW),
	                    "SetupDiGetDeviceInterfaceDetailW");
	ok &= PatchIATEntry(IAT_RVA_SetupDiDestroyDeviceInfoList,
	                    reinterpret_cast<void*>(&Patched_SetupDiDestroyDeviceInfoList),
	                    "SetupDiDestroyDeviceInfoList");
	ok &= PatchIATEntry(IAT_RVA_SetupDiGetDeviceInterfaceAlias,
	                    reinterpret_cast<void*>(&Patched_SetupDiGetDeviceInterfaceAlias),
	                    "SetupDiGetDeviceInterfaceAlias");
	ok &= PatchIATEntry(IAT_RVA_SetupDiGetClassDevsA,
	                    reinterpret_cast<void*>(&Patched_SetupDiGetClassDevsA),
	                    "SetupDiGetClassDevsA");
	ok &= PatchIATEntry(IAT_RVA_SetupDiEnumDeviceInterfaces,
	                    reinterpret_cast<void*>(&Patched_SetupDiEnumDeviceInterfaces),
	                    "SetupDiEnumDeviceInterfaces");

	ok &= PatchIATEntry(IAT_RVA_RegQueryValueExW,
	                    reinterpret_cast<void*>(&Patched_RegQueryValueExW),
	                    "RegQueryValueExW");
	ok &= PatchIATEntry(IAT_RVA_RegCloseKey,
	                    reinterpret_cast<void*>(&Patched_RegCloseKey),
	                    "RegCloseKey");

	if (!ok)
	{
		rslog::error_ts() << __FUNCTION__ << " - one or more IAT patches failed" << std::endl;
	}
	else
	{
		rslog::info_ts() << __FUNCTION__ << " - all IAT patches applied successfully" << std::endl;
	}
}
