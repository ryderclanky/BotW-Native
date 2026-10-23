#pragma once

// From Cemu a53cf0e src/util/DXGIWrapper/DXGIWrapper.h.
// BotW-Native: that header uses WRL ComPtr (wrl/client.h), which MinGW UCRT64 does not ship.
// The query is the same: match the Vulkan device LUID to an IDXGIAdapter3 and read local VRAM.

#undef GetHwnd
#include <dxgi1_4.h>

#include <cstring>
#include <stdexcept>

class DXGIWrapper
{
public:
	DXGIWrapper()
		: DXGIWrapper(nullptr)
	{}

	DXGIWrapper(uint8* deviceLUID)
	{
		m_moduleHandle = LoadLibraryA("dxgi.dll");
		if (!m_moduleHandle)
			throw std::runtime_error("can't load dxgi module");

		using CreateDXGIFactory1_t = HRESULT (WINAPI*)(REFIID, void**);
		FARPROC createRaw = GetProcAddress(m_moduleHandle, "CreateDXGIFactory1");
		CreateDXGIFactory1_t pCreateDXGIFactory1 = nullptr;
		static_assert(sizeof(createRaw) == sizeof(pCreateDXGIFactory1));
		memcpy(&pCreateDXGIFactory1, &createRaw, sizeof(createRaw));
		if (!pCreateDXGIFactory1)
		{
			FreeLibrary(m_moduleHandle);
			m_moduleHandle = nullptr;
			throw std::runtime_error("can't find CreateDXGIFactory1 in dxgi module");
		}

		IDXGIFactory1* dxgiFactory = nullptr;
		if (FAILED(pCreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))) || !dxgiFactory)
		{
			Cleanup();
			throw std::runtime_error("can't create dxgi factory");
		}

		UINT adapterIndex = 0;
		for (;;)
		{
			IDXGIAdapter1* dxgiAdapter = nullptr;
			if (dxgiFactory->EnumAdapters1(adapterIndex, &dxgiAdapter) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 desc{};
			dxgiAdapter->GetDesc1(&desc);
			const bool match = deviceLUID == nullptr || memcmp(&desc.AdapterLuid, deviceLUID, sizeof(LUID)) == 0;
			if (match)
			{
				HRESULT qi = dxgiAdapter->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&m_dxgiAdapter);
				dxgiAdapter->Release();
				if (FAILED(qi))
				{
					dxgiFactory->Release();
					Cleanup();
					throw std::runtime_error("can't create dxgi adapter");
				}
				break;
			}
			dxgiAdapter->Release();
			++adapterIndex;
		}
		dxgiFactory->Release();
	}

	~DXGIWrapper()
	{
		Cleanup();
	}

	bool QueryVideoMemoryInfo(DXGI_QUERY_VIDEO_MEMORY_INFO& info) const
	{
		if (!m_dxgiAdapter)
			return false;
		return m_dxgiAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info) == S_OK;
	}

private:
	HMODULE m_moduleHandle = nullptr;
	IDXGIAdapter3* m_dxgiAdapter = nullptr;

	void Cleanup()
	{
		if (m_dxgiAdapter)
		{
			m_dxgiAdapter->Release();
			m_dxgiAdapter = nullptr;
		}
		if (m_moduleHandle)
		{
			FreeLibrary(m_moduleHandle);
			m_moduleHandle = nullptr;
		}
	}
};
