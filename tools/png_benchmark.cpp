#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <png.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

struct PngApi
{
    HMODULE module;
#define PNG_SYMBOL(name) decltype(&::name) name = reinterpret_cast<decltype(&::name)>(GetProcAddress(module, #name));
    PNG_SYMBOL(png_create_write_struct)
    PNG_SYMBOL(png_create_info_struct)
    PNG_SYMBOL(png_destroy_write_struct)
    PNG_SYMBOL(png_set_longjmp_fn)
    PNG_SYMBOL(png_set_write_fn)
    PNG_SYMBOL(png_set_compression_level)
    PNG_SYMBOL(png_set_IHDR)
    PNG_SYMBOL(png_write_info)
    PNG_SYMBOL(png_write_row)
    PNG_SYMBOL(png_write_end)
    PNG_SYMBOL(png_get_io_ptr)
    PNG_SYMBOL(png_image_begin_read_from_file)
    PNG_SYMBOL(png_image_begin_read_from_memory)
    PNG_SYMBOL(png_image_finish_read)
    PNG_SYMBOL(png_image_free)
    PNG_SYMBOL(png_get_libpng_ver)
#undef PNG_SYMBOL
};

static PngApi* api;

struct Image
{
    unsigned width;
    unsigned height;
    std::vector<unsigned char> pixels;
};

static Image Decode(const char* path, const std::vector<unsigned char>* encoded = nullptr)
{
    png_image info = {};
    info.version = PNG_IMAGE_VERSION;
    const bool started = encoded ? api->png_image_begin_read_from_memory(&info, encoded->data(), encoded->size()) :
        api->png_image_begin_read_from_file(&info, path);
    if (!started)
        throw std::runtime_error(info.message);
    info.format = PNG_FORMAT_RGBA;
    Image image{info.width, info.height, std::vector<unsigned char>(PNG_IMAGE_SIZE(info))};
    const bool success = api->png_image_finish_read(&info, nullptr, image.pixels.data(), 0, nullptr);
    const std::string message(info.message);
    api->png_image_free(&info);
    if (!success)
        throw std::runtime_error(message);
    return image;
}

static Image Resize(const Image& source, unsigned width, unsigned height)
{
    if (width == source.width && height == source.height)
        return source;
    Image image{width, height, std::vector<unsigned char>(static_cast<size_t>(width) * height * 4)};
    for (unsigned y = 0; y < height; y++)
    {
        const double sy = std::clamp((y + 0.5) * source.height / height - 0.5, 0.0, static_cast<double>(source.height - 1));
        const unsigned y0 = static_cast<unsigned>(sy), y1 = std::min(y0 + 1, source.height - 1);
        for (unsigned x = 0; x < width; x++)
        {
            const double sx = std::clamp((x + 0.5) * source.width / width - 0.5, 0.0, static_cast<double>(source.width - 1));
            const unsigned x0 = static_cast<unsigned>(sx), x1 = std::min(x0 + 1, source.width - 1);
            for (unsigned c = 0; c < 4; c++)
            {
                auto at = [&](unsigned px, unsigned py) { return source.pixels[(static_cast<size_t>(py) * source.width + px) * 4 + c]; };
                const double top = at(x0, y0) * (1.0 - (sx - x0)) + at(x1, y0) * (sx - x0);
                const double bottom = at(x0, y1) * (1.0 - (sx - x0)) + at(x1, y1) * (sx - x0);
                image.pixels[(static_cast<size_t>(y) * width + x) * 4 + c] =
                    static_cast<unsigned char>(std::lround(top * (1.0 - (sy - y0)) + bottom * (sy - y0)));
            }
        }
    }
    return image;
}

static std::vector<unsigned char> Encode(const Image& image, int level)
{
    std::vector<unsigned char> encoded;
    // Match PCSX2's libpng row encoding and memory writer; omit file I/O from the timing.
    encoded.reserve(static_cast<size_t>(image.width) * image.height * 2);
    png_structp writer = api->png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!writer)
        throw std::runtime_error("png_create_write_struct failed");
    png_infop info = api->png_create_info_struct(writer);
    if (!info || setjmp(*api->png_set_longjmp_fn(writer, longjmp, sizeof(jmp_buf))))
    {
        api->png_destroy_write_struct(&writer, info ? &info : nullptr);
        throw std::runtime_error("PNG encoding failed");
    }
    api->png_set_write_fn(writer, &encoded,
        [](png_structp ptr, png_bytep data, png_size_t length) {
            auto& output = *static_cast<std::vector<unsigned char>*>(api->png_get_io_ptr(ptr));
            const size_t old_size = output.size();
            output.resize(old_size + length);
            std::memcpy(output.data() + old_size, data, length);
        }, [](png_structp) {});
    api->png_set_compression_level(writer, level);
    api->png_set_IHDR(writer, info, image.width, image.height, 8, PNG_COLOR_TYPE_RGBA,
        PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    api->png_write_info(writer, info);
    for (unsigned y = 0; y < image.height; y++)
        api->png_write_row(writer, const_cast<png_bytep>(image.pixels.data() + static_cast<size_t>(y) * image.width * 4));
    api->png_write_end(writer, nullptr);
    api->png_destroy_write_struct(&writer, &info);
    return encoded;
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 9)
            throw std::runtime_error("usage: png-benchmark libpng.dll image width height repetitions level backend round");
        PngApi loaded{LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)};
        if (!loaded.module || !loaded.png_image_begin_read_from_file || !loaded.png_set_longjmp_fn)
            throw std::runtime_error("Could not load libpng or its API");
        api = &loaded;
        HMODULE zlib = GetModuleHandleW(L"z.dll");
        const auto version = reinterpret_cast<const char* (*)()>(GetProcAddress(zlib, "zlibVersion"));
        if (!version)
            throw std::runtime_error("Could not identify zlib backend");
        char zlib_path[MAX_PATH] = {};
        GetModuleFileNameA(zlib, zlib_path, MAX_PATH);
        const std::string png_path(argv[1]);
        const std::string expected_zlib = png_path.substr(0, png_path.find_last_of("\\/")) + "\\z.dll";
        if (_stricmp(expected_zlib.c_str(), zlib_path) != 0)
            throw std::runtime_error("Unexpected zlib DLL was loaded");
        Image image = Decode(argv[2]);
        const unsigned width = static_cast<unsigned>(std::stoul(argv[3]));
        const unsigned height = static_cast<unsigned>(std::stoul(argv[4]));
        if (width && height)
            image = Resize(image, width, height);
        const int repetitions = std::stoi(argv[5]), level = std::stoi(argv[6]);
        auto expected = Encode(image, level); // Warm-up, outside measured interval.
        const Image decoded = Decode(nullptr, &expected);
        if (decoded.width != image.width || decoded.height != image.height || decoded.pixels != image.pixels)
            throw std::runtime_error("Decoded PNG pixels do not match the input");
        std::vector<double> timings;
        for (int iteration = 0; iteration < repetitions; iteration++)
        {
            const auto start = std::chrono::steady_clock::now();
            auto output = Encode(image, level);
            const auto finish = std::chrono::steady_clock::now();
            timings.push_back(std::chrono::duration<double, std::milli>(finish - start).count());
            if (output != expected)
                throw std::runtime_error("PNG output changed across repetitions");
        }
        std::cout.precision(10);
        std::cout << "{\"backend\":\"" << argv[7] << "\",\"round\":" << argv[8]
            << ",\"libpng_version\":\"" << api->png_get_libpng_ver(nullptr)
            << "\",\"zlib_version\":\"" << version() << "\",\"width\":" << image.width
            << ",\"height\":" << image.height << ",\"level\":" << level
            << ",\"png_bytes\":" << expected.size() << ",\"pixels_equal\":true,\"milliseconds\":[";
        for (size_t i = 0; i < timings.size(); i++)
            std::cout << (i ? "," : "") << timings[i];
        std::cout << "]}\n";
        FreeLibrary(loaded.module);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
