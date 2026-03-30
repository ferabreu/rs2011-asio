// dllmain.cpp : Defines the entry point for the DLL application.
#include "stdafx.h"
#include "Patcher.h"
#include "DebugDeviceEnum.h"
#include "RSAggregatorDeviceEnum.h"
#include "Configurator.h"
#include "MyUnknown.h"



extern "C" {

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);

		rslog::InitLog();
		rslog::info_ts() << " - Wrapper DLL loaded (v0.7.4)" << std::endl;
		InitPatcher();
		PatchOriginalCode();
		break;
	case DLL_PROCESS_DETACH:
		DeinitPatcher();
		rslog::info_ts() << " - Wrapper DLL unloaded" << std::endl;
		rslog::CleanupLog();
		break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
		break;
    }
    return TRUE;
}

HRESULT STDAPICALLTYPE Patched_CoCreateInstance(REFCLSID rclsid, IUnknown *pUnkOuter, DWORD dwClsContext, REFIID riid, void **ppOut)
{
	rslog::info_ts() << "Patched_CoCreateInstance called: " << riid << std::endl;

	if (!ppOut)
		return E_POINTER;

	if (riid == __uuidof(IMMDeviceEnumerator))
	{
		RSAggregatorDeviceEnum* aggregatorEnum = new RSAggregatorDeviceEnum();
		SetupDeviceEnumerator(*aggregatorEnum);

		DebugDeviceEnum* newEnum = new DebugDeviceEnum(aggregatorEnum);
		aggregatorEnum->Release();

		*ppOut = newEnum;
		return S_OK;
	}

	return CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppOut);
}



struct PaWasapiSubStream
{
	IUnknown* clientParent;    // IAudioClient
	IUnknown* clientStream;    // IStream
	IUnknown* clientProc;      // IAudioClient
	char dummy[0xdc];
};

struct PaWasapiStream
{
	char dummy[0x108];

	PaWasapiSubStream in;
	IUnknown* captureClientParent; // IAudioCaptureClient
	IUnknown* captureClientStream; // IStream
	IUnknown* captureClient;       // IAudioCaptureClient
	IUnknown* inVol;               // IAudioEndpointVolume

	PaWasapiSubStream out;
	IUnknown* renderClientParent; // IAudioRenderClient
	IUnknown* renderClientStream; // IStream
	IUnknown* renderClient;       // IAudioRenderClient
	IUnknown* outVol;             // IAudioEndpointVolume
};

static void MarshalSubStreamComPointers(PaWasapiSubStream *substream)
{
	substream->clientStream = nullptr;

	if (substream->clientParent)
	{
		substream->clientStream = substream->clientParent;
		substream->clientStream->AddRef();
	}
	else
	{
		if (substream->clientProc)
		{
			substream->clientProc->Release();
			substream->clientProc = nullptr;
		}
	}
}

HRESULT Patched_PortAudio_MarshalStreamComPointers(void* s)
{
	rslog::info_ts() << __FUNCTION__ << std::endl;

	PaWasapiStream* stream = reinterpret_cast<PaWasapiStream*>(s);

	if (stream->in.clientParent)
	{
		MarshalSubStreamComPointers(&stream->in);
		stream->captureClientStream = stream->captureClientParent;
		stream->captureClientStream->AddRef();
	}

	if (stream->out.clientParent)
	{
		MarshalSubStreamComPointers(&stream->out);
		stream->renderClientStream = stream->renderClientParent;
		stream->renderClientStream->AddRef();
	}

	return S_OK;
}

static void UnmarshalSubStreamComPointers(PaWasapiSubStream *substream)
{
	if (substream->clientStream)
	{
		substream->clientProc = substream->clientStream;
		substream->clientStream = nullptr;
	}
}

HRESULT Patched_PortAudio_UnmarshalStreamComPointers(void* s)
{
	rslog::info_ts() << __FUNCTION__ << std::endl;

	PaWasapiStream* stream = reinterpret_cast<PaWasapiStream*>(s);

	if (stream->in.clientParent)
	{
		UnmarshalSubStreamComPointers(&stream->in);
		stream->captureClient = stream->captureClientStream;
		stream->captureClientStream = nullptr;

		// HACK: this works around the game not calling release on this. could be a bug?
		// this avoids a crash in asio4all, but introduces other possible random crashes
		if (stream->in.clientProc)
		{
			MyUnknown* myUnknown = nullptr;
			stream->in.clientProc->QueryInterface(IID_IMyUnknown, (void**)&myUnknown);
			if (myUnknown)
			{
				if (myUnknown->RefCountHackEnabled)
				{
					rslog::info_ts() << "  using ref count hack" << std::endl;
					stream->in.clientProc->Release();
				}
				myUnknown->Release();
			}
		}
	}

	if (stream->out.clientParent)
	{
		UnmarshalSubStreamComPointers(&stream->out);
		stream->renderClient = stream->renderClientStream;
		stream->renderClientStream = nullptr;

		// HACK: this works around the game not calling release on this. could be a bug?
		// this avoids a crash in asio4all, but introduces other possible random crashes
		if (stream->out.clientProc)
		{
			MyUnknown* myUnknown = nullptr;
			stream->out.clientProc->QueryInterface(IID_IMyUnknown, (void**)&myUnknown);
			if (myUnknown)
			{
				if (myUnknown->RefCountHackEnabled)
				{
					rslog::info_ts() << "  using ref count hack" << std::endl;
					stream->out.clientProc->Release();
				}
				myUnknown->Release();
			}
		}
	}

	return S_OK;
}

} // Extern "C"
