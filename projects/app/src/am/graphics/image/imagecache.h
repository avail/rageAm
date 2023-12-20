#pragma once

#include "image.h"

namespace rageam::graphics
{
	// #define IMAGE_CACHE_ENABLE_LOG

#ifdef IMAGE_CACHE_ENABLE_LOG
#include "common/logger.h"
#define IMAGE_CACHE_LOG(fmt, ...) AM_TRACEF(fmt, __VA_ARGS__)
#else
	inline void ImageCacheLogPlaceHolder(ConstString, ...) {}
#define IMAGE_CACHE_LOG(fmt, ...) ImageCacheLogPlaceHolder(fmt, __VA_ARGS__)
#endif

	struct CompressedImageInfo;

	struct ImageCacheState
	{
		u64 SizeRamUsed;
		u64 SizeRamBudget;
		u64 SizeFsUsed;
		u64 SizeFsBudget;
		u32 ImageCountRam;
		u32 ImageCountFs;
		u32	DX11ViewCount;
	};

	/**
	 * \brief Two level image cache - in memory and in file system.
	 */
	class ImageCache
	{
		static constexpr u32 DEFAULT_MEMORY_STORE_BUDGET = 2048u * 1024u * 1024u;		// 2GB
		static constexpr u32 DEFAULT_FILESYSTEM_STORE_BUDGET = 2048u * 1024u * 1024u;	// 2GB
		static constexpr u32 DEFAULT_TIME_TO_CACHE_THRESHOLD = 25;						// Milliseconds
		static constexpr u32 DEFAULT_DX11_VIEWS_MAX = 50;
		static constexpr u32 SETTINGS_VERSION = 0;
		static constexpr u32 CACHE_LIST_VERSION = 0;
		static constexpr ConstWString DEFAULT_CACHE_DIRECTORY_NAME = L"CompressorCache";
		static constexpr ConstWString CACHE_LIST_NAME = L"List.xml";

		static inline ImageCache* sm_Instance = nullptr;

		struct CacheEntry
		{
			ImagePtr			Image;
			u32					ImageSize;
			ImageFileKind		ImageKind;			// We store kind to get file extension when loading image from file system
			Vec2S				ImagePaddingUV2;	// In case if image was padded, contains adjusted UV2
			bool				StoreInFileSystem;
		};

		struct CacheEntryDX11
		{
			amComPtr<ID3D11ShaderResourceView>	View;
			amComPtr<ID3D11Texture2D>			Tex;
			Vec2S								PaddingUV2;
		};

		struct Settings
		{
			file::WPath			CacheDirectory;			// If not specified, default data folder is used instead
			u32					MemoryStoreBudget;		// How much RAM we allow to be taken by cached images
			u32					FileSystemStoreBudget;	// After image unloaded from RAM it goes to FS, oldest images are removed from cache once FS budget is reached
			u32					TimeToCacheThreshold;	// Processing in milliseconds required for image to be added to cache, to skip caching small images
			u32					MaxDX11Views;			// Maximum number of textures
		};

		Settings				m_Settings = {};
		file::WPath				m_CacheDirectory;
		HashSet<CacheEntry>		m_Entries;
		HashSet<CacheEntryDX11>	m_EntriesDX11;
		// Hashes of entries ordered from newest (first) to oldest (last)
		// Ordered this way because it's easier to remove entries from end than from beginning
		List<u32>				m_NewToOldEntriesRAM;
		List<u32>				m_NewToOldEntriesFS;
		List<u32>				m_NewToOldEntriesDX11;
		u64						m_SizeRam = 0;			// Bytes taken by images in memory
		u64						m_SizeFs = 0;			// Bytes taken by images in file system
		std::mutex				m_Mutex;

		u32 BytesToMb(u32 bytes) const { return bytes / (1024u * 1024u); }
		u32 MbToBytes(u32 mb) const { return mb * 1024u * 1024u; }

		void SaveSettings(const file::WPath& path, const Settings& settings) const;
		// Loads settings from app data and if config does not exist yet, creates new one
		void LoadSettings();

		bool GetInfoFromCachedImagePath(const file::WPath& path, u32& hash, u32& imageSize) const;
		// If image kind is none, no extension added
		file::WPath GetCachedImagePath(u32 hash, u32 imageSize, ImageFileKind kind) const;

		// Does not account stats
		void MoveImageToFileSystem(CacheEntry& entry, u32 hash) const;

		void CleanUpOldEntriesToFitBudget();

	public:
		ImageCache();
		~ImageCache();

		ImagePtr GetFromCache(u32 hash, Vec2S* outUV2 = nullptr);
		// Tex is optional
		bool GetFromCacheDX11(u32 hash, amComPtr<ID3D11ShaderResourceView>& outView, Vec2S* outUV2 = nullptr, amComPtr<ID3D11Texture2D>* tex = nullptr);
		// If store in file system is set to true, image will be moved to file on disk when ram budget is reached
		// Image size is used for accounting budget
		void Cache(const ImagePtr& image, u32 hash, u32 imageSize, bool storeInFileSystem, Vec2S uv2);
		void CacheDX11(u32 hash, const amComPtr<ID3D11ShaderResourceView>& view, const amComPtr<ID3D11Texture2D>& tex, Vec2S uv2);

		// Computes unique hash based on pixel data and compression options
		HashValue ComputeImageHash(ImagePixelData imageData, u32 imageDataSize, const CompressedImageInfo& compInfo) const;
		// Computes unique hash based on only pixel data
		HashValue ComputeImageHash(ImagePixelData	imageData, u32 imageDataSize) const;
		// Checks if given time is greater than threshold
		bool ShouldStore(u32 elapsedMilliseconds) const { return elapsedMilliseconds >= m_Settings.TimeToCacheThreshold; }

		void Clear();

		ImageCacheState GetState();

		static ImageCache* GetInstance() { return sm_Instance; }
		static void SetInstance(ImageCache* cache) { sm_Instance = cache; }
	};
}