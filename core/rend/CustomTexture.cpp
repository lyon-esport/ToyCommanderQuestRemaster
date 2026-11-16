/*
	 Copyright 2018 flyinghead
 
	 This file is part of reicast.
 
	 reicast is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 2 of the License, or
	 (at your option) any later version.
 
	 reicast is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.
 
	 You should have received a copy of the GNU General Public License
	 along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "CustomTexture.h"
#include "TexCache.h"
#include "oslib/directory.h"
#include "oslib/storage.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "stdclass.h"
#include "util/worker_thread.h"
#include "xbrz/xbrz.h"
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <set>

#include <sstream>
#include <locale>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stbi.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

CustomTexture custom_texture;

class CustomTextureSource : public BaseCustomTextureSource
{
public:
	CustomTextureSource(const std::string game_id)
	{
		textures_path = hostfs::getTextureLoadPath(game_id);
		if (!textures_path.empty())
		{
			custom_textures_available = hostfs::storage().exists(textures_path);
			if (custom_textures_available)
				NOTICE_LOG(RENDERER, "Found custom textures directory: %s", textures_path.c_str());
		}
	}
	bool shouldReplace() const override { return config::CustomTextures && custom_textures_available; }
	bool shouldPreload() const override { return shouldReplace() && config::PreloadCustomTextures; }
	bool loadMap() override;
	size_t getTextureCount() const override { return texture_map.size(); }
	void preloadTextures(TextureCallback callback, std::atomic<bool>* stop_flag) override;
	u8* loadCustomTexture(u32 hash, int& width, int& height) override;
	bool isTextureReplaced(u32 hash) override final;
	
	

private:
	bool custom_textures_available = false;
	std::string textures_path;
	std::map<u32, std::string> texture_map;
};

// tcvr: the upscale cache (vr.UpscaleCache): textures upscaled with xBRZ from the player's own
// disc as the game first shows them, kept as PNG files in the app's storage
// (data/texcache/v2/<game id>/<hash>.png) and used in place of the originals from then on, also
// on later launches. Nothing of it leaves the device.
// v2: the first version (data/texcache/<game id>/) saved the packed mipmap chain of mipmapped
// textures instead of their full-size picture; its files are deleted once, then rebuilt.
class UpscaleCacheSource : public BaseCustomTextureSource
{
public:
	explicit UpscaleCacheSource(const std::string& gameId)
		: dir(get_writable_data_path("texcache/v2/") + gameId + "/"),
		  oldDir(get_writable_data_path("texcache/") + gameId + "/") {}
	bool shouldReplace() const override { return config::VrUpscaleCache > 1; }
	bool loadMap() override
	{
		deleteOldCache();
		if (!file_exists(dir))
			return false;
		std::lock_guard<std::mutex> _(mutex);
		map.clear();
		// tcvr: textures rejected in the review (tcvr/docs/TextureReview.md): one hex hash per line;
		// the game keeps their originals and never upscales them again
		rejected.clear();
		if (FILE *f = nowide::fopen((dir + "rejected.txt").c_str(), "r"))
		{
			char line[64];
			while (fgets(line, sizeof(line), f) != nullptr)
			{
				char *end;
				const u32 hash = (u32)strtoul(line, &end, 16);
				if (end != line && line[0] != '#')
					rejected.insert(hash);
			}
			fclose(f);
		}
		hostfs::DirectoryTree tree(dir);
		for (const hostfs::FileInfo& item : tree)
		{
			if (get_file_extension(item.name) != "png")
				continue;
			char *end;
			const u32 hash = (u32)strtoll(item.name.c_str(), &end, 16);
			if (*end == '.' && rejected.count(hash) == 0)
				map[hash] = item.path;
		}
		NOTICE_LOG(RENDERER, "tcvr: upscale cache: %zu textures in %s (%zu rejected in review)", map.size(), dir.c_str(), rejected.size());
		return !map.empty();
	}
	size_t getTextureCount() const override { std::lock_guard<std::mutex> _(mutex); return map.size(); }
	u8* loadCustomTexture(u32 hash, int& width, int& height) override
	{
		std::string path;
		{
			std::lock_guard<std::mutex> _(mutex);
			auto it = map.find(hash);
			if (it == map.end())
				return nullptr;
			path = it->second;
		}
		hostfs::File *file = hostfs::storage().openFile(path, "rb");
		if (file == nullptr)
			return nullptr;
		int n;
		stbi_set_flip_vertically_on_load_thread(1);
		u8 *data = stbi_load_from_file(file, &width, &height, &n, STBI_rgb_alpha);
		delete file;
		return data;
	}
	bool isTextureReplaced(u32 hash) override { std::lock_guard<std::mutex> _(mutex); return map.count(hash) != 0; }
	bool isRejected(u32 hash) const { std::lock_guard<std::mutex> _(mutex); return rejected.count(hash) != 0; }
	// a new file in the cache: used from the next time the game loads that texture
	void added(u32 hash, const std::string& path) { std::lock_guard<std::mutex> _(mutex); map[hash] = path; }
	const std::string dir;

private:
	// the v1 cache holds broken pictures (see above): delete its files, they're all regenerable
	void deleteOldCache()
	{
		if (!file_exists(oldDir))
			return;
		int count = 0;
		{
			hostfs::DirectoryTree tree(oldDir);
			for (const hostfs::FileInfo& item : tree)
			{
				const std::string ext = get_file_extension(item.name);
				if ((ext == "png" || ext == "tmp") && nowide::remove(item.path.c_str()) == 0)
					count++;
			}
		}
		NOTICE_LOG(RENDERER, "tcvr: upscale cache: deleted %d files of the old cache in %s", count, oldDir.c_str());
	}

	const std::string oldDir;
	mutable std::mutex mutex;
	std::map<u32, std::string> map;
	std::set<u32> rejected;
};

static UpscaleCacheSource *upscaleCache;		// owned by custom_texture's sources
static std::unique_ptr<WorkerThread> upscaleThread;
static std::mutex upscaleMutex;
static std::set<u32> upscaleSeen;			// queued or done this run
static std::atomic<int> upscaleDone, upscaleQueued;

bool CustomTextureSource::loadMap()
{
	texture_map.clear();
	hostfs::DirectoryTree tree(textures_path);
	for (const hostfs::FileInfo& item : tree)
	{
		std::string extension = get_file_extension(item.name);
		if (extension != "jpg" && extension != "jpeg" && extension != "png")
			continue;
		std::string::size_type dotpos = item.name.find_last_of('.');
		std::string basename = item.name.substr(0, dotpos);
		char *endptr;
		u32 hash = (u32)strtoll(basename.c_str(), &endptr, 16);
		if (endptr - basename.c_str() < (ptrdiff_t)basename.length())
		{
			INFO_LOG(RENDERER, "Invalid hash %s", basename.c_str());
			continue;
		}
		texture_map[hash] = item.path;
	}
	return !texture_map.empty();
}

void CustomTextureSource::preloadTextures(TextureCallback callback, std::atomic<bool>* stop_flag)
{
	for (auto const& [hash, path] : texture_map)
	{
		if (stop_flag != nullptr && *stop_flag)
			return;
		int w, h;
		u8* data = loadCustomTexture(hash, w, h);
		if (data != nullptr)
		{
			size_t size = (size_t)w * h * 4;
			TextureData tex;
			tex.w = w;
			tex.h = h;
			tex.data.resize(size);
			memcpy(tex.data.data(), data, size);
			stbi_image_free(data);
			callback(hash, std::move(tex));
		}
	}
}

u8* CustomTextureSource::loadCustomTexture(u32 hash, int& width, int& height)
{
	auto it = texture_map.find(hash);
	if (it == texture_map.end())
		return nullptr;

	hostfs::File *file = hostfs::storage().openFile(it->second, "rb");
	if (file == nullptr)
		return nullptr;
	int n;
	stbi_set_flip_vertically_on_load_thread(1);
	u8 *imgData = stbi_load_from_file(file, &width, &height, &n, STBI_rgb_alpha);
	delete file;
	return imgData;
}

bool CustomTextureSource::isTextureReplaced(u32 hash)
{
	return texture_map.count(hash);
}

void CustomTexture::loadTexture(BaseTextureCacheData *texture)
{
	if (texture->custom_image_data != nullptr) {
		free(texture->custom_image_data);
		texture->custom_image_data = nullptr;
	}
	if (!texture->dirty)
	{
		int width, height;
		u8 *image_data = loadTexture(texture->texture_hash, width, height);
		if (image_data == nullptr && texture->old_vqtexture_hash != 0)
			image_data = loadTexture(texture->old_vqtexture_hash, width, height);
		if (image_data == nullptr)
			image_data = loadTexture(texture->old_texture_hash, width, height);
		if (image_data != nullptr)
		{
			texture->custom_width = width;
			texture->custom_height = height;
			texture->custom_image_data = image_data;
		}
	}
	texture->custom_load_in_progress--;
}

std::string CustomTexture::getGameId()
{
   std::string game_id(settings.content.gameId);
   const size_t str_end = game_id.find_last_not_of(' ');
   if (str_end == std::string::npos)
	  return "";
   game_id = game_id.substr(0, str_end + 1);
   std::replace(game_id.begin(), game_id.end(), ' ', '_');

   return game_id;
}

bool CustomTexture::init()
{
	if (!initialized)
	{
		stop_preload = false;
		resetPreloadProgress();
		pending_preloads = 0;
		initialized = true;

		std::string game_id = getGameId();
		if (game_id.length() > 0)
		{
			// The first source added has highest priority.
			// Add your source after the default `CustomTextureSource`(data/textures/<game id> folder), so end-users can override your textures.
			addSource(std::make_unique<CustomTextureSource>(game_id));
			// tcvr: after it, so a texture pack the player adds wins over the upscale cache
			auto cache = std::make_unique<UpscaleCacheSource>(game_id);
			upscaleCache = cache.get();
			addSource(std::move(cache));
		}
	}

	return loaderThread != nullptr;
}

bool CustomTexture::enabled() {
	return loaderThread != nullptr;
}

bool CustomTexture::preloaded() {
	return preload_total > 0;
}

bool CustomTexture::isPreloading() {
	if (pending_preloads > 0)
		return true;

	int texLoaded = 0;
	int texTotal = 0;
	size_t loaded_size_b = 0;
	getPreloadProgress(texLoaded, texTotal, loaded_size_b);
	
	return (texTotal > 0 && texLoaded < texTotal);
}

void CustomTexture::addSource(std::unique_ptr<BaseCustomTextureSource> source)
{
	BaseCustomTextureSource* ptr = source.get();
	sources.emplace_back(std::move(source));
	
	if (initialized)
	{
		if (!loaderThread && ptr->shouldReplace())
		{
			loaderThread = std::make_unique<WorkerThread>("CustomTexLoader");
		}
		if (loaderThread)
		{
			if (ptr->shouldPreload())
				pending_preloads++;
			loaderThread->run([this, ptr]() {
				prepareSource(ptr);
			});
		}
	}
}

CustomTexture::~CustomTexture() {
	terminate();
}

void CustomTexture::terminate()
{
	stop_preload = true;
	if (loaderThread)
		loaderThread->stop();
	loaderThread.reset();
	for (auto& source : sources)
		source->terminate();
	sources.clear();
	preloaded_textures.clear();
	resetPreloadProgress();
	initialized = false;
}

u8* CustomTexture::loadTexture(u32 hash, int& width, int& height)
{
	auto it = preloaded_textures.find(hash);
	if (it != preloaded_textures.end())
	{
		width = it->second.w;
		height = it->second.h;
		size_t size = (size_t)width * height * 4;
		u8* buffer = (u8*)malloc(size);
		if (buffer == nullptr)
			return nullptr;
		memcpy(buffer, it->second.data.data(), size);
		return buffer;
	}

	for (auto it = sources.begin(); it != sources.end(); ++it)
	{
		auto& source = *it;
		if (source->shouldReplace())
		{
			u8* data = source->loadCustomTexture(hash, width, height);
			if (data != nullptr)
				return data;
		}
	}
	return nullptr;
}

bool CustomTexture::isTextureReplaced(BaseTextureCacheData* texture)
{
	if (preloaded_textures.count(texture->texture_hash))
		return true;
	if (texture->old_vqtexture_hash != 0 && preloaded_textures.count(texture->old_vqtexture_hash))
		return true;
	if (texture->old_texture_hash != 0 && preloaded_textures.count(texture->old_texture_hash))
		return true;

	for (auto it = sources.begin(); it != sources.end(); ++it)
	{
		auto& source = *it;
		if (source->shouldReplace())
		{
			if (source->isTextureReplaced(texture->texture_hash))
				return true;
			if (texture->old_vqtexture_hash != 0 && source->isTextureReplaced(texture->old_vqtexture_hash))
				return true;
			if (texture->old_texture_hash != 0 && source->isTextureReplaced(texture->old_texture_hash))
				return true;
		}
	}
	return false;
}

void CustomTexture::loadCustomTextureAsync(BaseTextureCacheData *texture_data)
{
	if (!init())
		return;

	texture_data->custom_load_in_progress++;
	loaderThread->run([this, texture_data]() {
		loadTexture(texture_data);
	});
}

// A decoded texture (as the renderer uploads it) as 8-bit RGBA, malloc'd, or nullptr
static u8 *textureToRGBA(BaseTextureCacheData* texture, int w, int h, void *src_buffer)
{
	u16 *src = (u16 *)src_buffer;
	u8 *dst_buffer = (u8 *)malloc(w * h * 4);	// 32-bit per pixel
	if (dst_buffer == nullptr)
	{
		ERROR_LOG(RENDERER, "Dump texture: out of memory");
		return nullptr;
	}
	u8 *dst = dst_buffer;

	for (int y = 0; y < h; y++)
	{
		if (!isDirectX(config::RendererType))
		{
			switch (texture->tex_type)
			{
			case TextureType::_4444:
				for (int x = 0; x < w; x++)
				{
					*dst++ = (((*src >> 12) & 0xF) << 4) | ((*src >> 12) & 0xF);
					*dst++ = (((*src >> 8) & 0xF) << 4) | ((*src >> 8) & 0xF);
					*dst++ = (((*src >> 4) & 0xF) << 4) | ((*src >> 4) & 0xF);
					*dst++ = ((*src & 0xF) << 4) | (*src & 0xF);
					src++;
				}
				break;
			case TextureType::_565:
				for (int x = 0; x < w; x++)
				{
					*(u32 *)dst = Unpacker565_32<RGBAPacker>::unpack(*src);
					dst += 4;
					src++;
				}
				break;
			case TextureType::_5551:
				for (int x = 0; x < w; x++)
				{
					*dst++ = (((*src >> 11) & 0x1F) << 3) | ((*src >> 13) & 7);
					*dst++ = (((*src >> 6) & 0x1F) << 3) | ((*src >> 8) & 7);
					*dst++ = (((*src >> 1) & 0x1F) << 3) | ((*src >> 3) & 7);
					*dst++ = (*src & 1) ? 255 : 0;
					src++;
				}
				break;
			case TextureType::_8888:
				memcpy(dst, src, w * 4);
				dst += w * 4;
				src += w * 2;
				break;
			default:
				WARN_LOG(RENDERER, "dumpTexture: unsupported picture format %x", (u32)texture->tex_type);
				free(dst_buffer);
				return nullptr;
			}
		}
		else
		{
			switch (texture->tex_type)
			{
			case TextureType::_4444:
				for (int x = 0; x < w; x++)
				{
					*(u32 *)dst = Unpacker4444_32<RGBAPacker>::unpack(*src);
					dst += 4;
					src++;
				}
				break;
			case TextureType::_565:
				for (int x = 0; x < w; x++)
				{
					*(u32 *)dst = Unpacker565_32<RGBAPacker>::unpack(*src);
					dst += 4;
					src++;
				}
				break;
			case TextureType::_5551:
				for (int x = 0; x < w; x++)
				{
					*(u32 *)dst = Unpacker1555_32<RGBAPacker>::unpack(*src);
					dst += 4;
					src++;
				}
				break;
			case TextureType::_8888:
				for (int x = 0; x < w; x++)
				{
					*(u32 *)dst = Unpacker8888<RGBAPacker>::unpack(*(u32 *)src);
					dst += 4;
					src += 2;
				}
				break;
			default:
				WARN_LOG(RENDERER, "dumpTexture: unsupported picture format %x", (u32)texture->tex_type);
				free(dst_buffer);
				return nullptr;
			}
		}
	}

	return dst_buffer;
}

void CustomTexture::dumpTexture(BaseTextureCacheData* texture, int w, int h, void *src_buffer)
{
	if (!config::DumpTextures)
		return;

	if (config::DumpUniqueTextures && (texture->Updates > 1 || texture->tcw.PixelFmt == PixelYUV))
		return;

	if (!config::DumpReplacedTextures.get() && isTextureReplaced(texture))
		return;

	std::string base_dump_dir = hostfs::getTextureDumpPath();
	if (!file_exists(base_dump_dir))
		make_directory(base_dump_dir);
	std::string game_id = getGameId();
	if (game_id.length() == 0)
		return;

	base_dump_dir += game_id + "/";
	if (!file_exists(base_dump_dir))
		make_directory(base_dump_dir);

	std::ostringstream path;
	path.imbue(std::locale::classic());
	path << base_dump_dir << std::hex << texture->texture_hash << ".png";

	u8 *dst_buffer = textureToRGBA(texture, w, h, src_buffer);
	if (dst_buffer == nullptr)
		return;

	stbi_flip_vertically_on_write(1);
	const auto& savefunc = [](void *context, void *data, int size) {
		FILE *f = nowide::fopen((const char *)context, "wb");
		if (f == nullptr)
		{
			WARN_LOG(RENDERER, "Dump texture: can't save to file %s: error %d", (const char *)context, errno);
		}
		else
		{
			fwrite(data, 1, size, f);
			fclose(f);
		}
	};
	stbi_write_png_to_func(savefunc, (void *)path.str().c_str(), w, h, STBI_rgb_alpha, dst_buffer, 0);

	free(dst_buffer);
}

void CustomTexture::prepareSource(BaseCustomTextureSource* source)
{
	bool should_preload = source->shouldPreload();

	if (!stop_preload && source->loadMap())
	{
		if (should_preload)
		{
			int count = static_cast<int>(source->getTextureCount());
			if (count > 0)
			{
				preload_total += count;
				auto callback = [this](u32 hash, TextureData&& data) {
					size_t size = data.data.size();
					preloaded_textures[hash] = std::move(data);
					preload_loaded++;
					preload_loaded_size += size;
				};
				source->preloadTextures(callback, &stop_preload);
			}
		}
	}

	if (should_preload)
		pending_preloads--;
}

void CustomTexture::getPreloadProgress(int& completed, int& total, size_t& loaded_size) const
{
	total = preload_total;
	if (total == 0 && pending_preloads > 0)
		total = -1; // Prints Preparing... in UI
	completed = preload_loaded;
	loaded_size = preload_loaded_size;
}

void CustomTexture::resetPreloadProgress()
{
	preload_total = 0;
	preload_loaded = 0;
	preload_loaded_size = 0;
}

// tcvr: see UpscaleCacheSource. Called on the render thread as a texture is uploaded; the
// upscaling and the file run on a worker thread of their own.
void CustomTexture::cacheUpscaled(BaseTextureCacheData* texture, int w, int h, void *src_buffer)
{
	const int factor = std::clamp((int)config::VrUpscaleCache, 0, 4);
	if (factor < 2 || upscaleCache == nullptr || config::TextureUpscale > 1)
		return;
	// small, still pictures only: no video (YUV), nothing the game redraws (render to texture,
	// animated), nothing big (memory and time)
	if (w * h > 256 * 256 || w < 8 || h < 8 || texture->tcw.PixelFmt == PixelYUV || texture->Updates > 1)
		return;
	// the caller passes the full-size picture only (never a mipmap chain or a cropped texture)
	if ((u32)w != texture->width || (u32)h != texture->height)
	{
		static bool logged;
		if (!logged)
			WARN_LOG(RENDERER, "tcvr: upscale cache: size mismatch %dx%d vs %dx%d, skipped", w, h, texture->width, texture->height);
		logged = true;
		return;
	}
	if (isTextureReplaced(texture) || upscaleCache->isRejected(texture->texture_hash))
		return;
	{
		std::lock_guard<std::mutex> _(upscaleMutex);
		if (!upscaleSeen.insert(texture->texture_hash).second)
			return;
	}
	u8 *rgba = textureToRGBA(texture, w, h, src_buffer);
	if (rgba == nullptr)
		return;
	if (!upscaleThread)
		upscaleThread = std::make_unique<WorkerThread>("UpscaleCache");
	upscaleQueued++;
	const u32 hash = texture->texture_hash;
	const bool alpha = texture->tcw.PixelFmt != Pixel565;
	UpscaleCacheSource *cache = upscaleCache;
	upscaleThread->run([=]() {
		// RGBA bytes -> xBRZ's ARGB words and back
		std::vector<u32> src((size_t)w * h), dst((size_t)w * h * factor * factor);
		for (size_t i = 0; i < src.size(); i++)
		{
			const u8 *p = &rgba[i * 4];
			src[i] = ((u32)p[3] << 24) | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
		}
		free(rgba);
		xbrz::scale(factor, src.data(), dst.data(), w, h, alpha ? xbrz::ColorFormat::ARGB : xbrz::ColorFormat::RGB, xbrz::ScalerCfg());
		std::vector<u8> out(dst.size() * 4);
		for (size_t i = 0; i < dst.size(); i++)
		{
			const u32 c = dst[i];
			out[i * 4] = (c >> 16) & 0xff;
			out[i * 4 + 1] = (c >> 8) & 0xff;
			out[i * 4 + 2] = c & 0xff;
			out[i * 4 + 3] = alpha ? (c >> 24) & 0xff : 0xff;
		}
		if (!file_exists(cache->dir))
		{
			make_directory(get_writable_data_path("texcache/"));
			make_directory(get_writable_data_path("texcache/v2/"));
			make_directory(cache->dir);
		}
		char name[16];
		snprintf(name, sizeof(name), "%08x.png", hash);
		const std::string path = cache->dir + name;
		const std::string tmp = path + ".tmp";
		bool written = false;
		if (FILE *f = nowide::fopen(tmp.c_str(), "wb"))
		{
			stbi_flip_vertically_on_write(1);
			written = stbi_write_png_to_func([](void *context, void *data, int size) {
					fwrite(data, 1, size, (FILE *)context);
				}, f, w * factor, h * factor, STBI_rgb_alpha, out.data(), 0) != 0;
			written = fclose(f) == 0 && written;
		}
		if (written && std::rename(tmp.c_str(), path.c_str()) == 0)
		{
			cache->added(hash, path);
			const int done = ++upscaleDone;
			if (done == 1 || done % 50 == 0)
				NOTICE_LOG(RENDERER, "tcvr: upscale cache: %d textures upscaled %dx this run (%d queued)", done, factor, (int)upscaleQueued);
		}
		else
			WARN_LOG(RENDERER, "tcvr: upscale cache: can't write %s", path.c_str());
	});
}

void CustomTexture::getUpscaleCacheProgress(int& done, int& queued, int& cached) const
{
	done = upscaleDone;
	queued = upscaleQueued;
	cached = upscaleCache != nullptr ? (int)upscaleCache->getTextureCount() : 0;
}
