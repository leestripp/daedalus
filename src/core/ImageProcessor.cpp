#include "core/ImageProcessor.h"
#include "core/utils.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <set>
#include <algorithm>
#include <cstdio>
#include <csetjmp>

// libpng for image loading and encoding
#include <png.h>

// Maximum dimension after scaling (longest edge = 512)
static const int MAX_SCALED_DIMENSION = 512;

std::string ImageProcessor::detect_format(const unsigned char* data, size_t size) {
    if (size < 4) return "application/octet-stream";
    
    // Check PNG
    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && 
        data[2] == 0x4E && data[3] == 0x47) {
        return "image/png";
    }
    
    // Check JPEG
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        return "image/jpeg";
    }
    
    // Check GIF
    if (size >= 3 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
        return "image/gif";
    }
    
    // Check BMP
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
        return "image/bmp";
    }
    
    return "application/octet-stream";
}

std::string ImageProcessor::get_mime_from_extension(const std::string& extension) {
    std::string ext = extension;
    // Remove leading dot if present
    if (!ext.empty() && ext[0] == '.') {
        ext = ext.substr(1);
    }
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "bmp") return "image/bmp";
    
    return "application/octet-stream";
}

bool ImageProcessor::is_supported_image(const std::string& path) {
    std::string ext = get_image_extension(path);
    static const std::set<std::string> supported = {"jpg", "jpeg", "png", "gif", "bmp"};
    return supported.count(ext) > 0;
}

/**
 * @brief Calculate new dimensions maintaining aspect ratio
 */
static void calculate_scaled_dimensions(int width, int height, int max_dim, int& new_width, int& new_height) {
    if (width > height) {
        // Width is the longest edge
        new_width = max_dim;
        new_height = static_cast<int>((static_cast<float>(height) / width) * max_dim);
    } else {
        // Height is the longest edge (or equal)
        new_height = max_dim;
        new_width = static_cast<int>((static_cast<float>(width) / height) * max_dim);
    }
    
    // Ensure minimum dimensions of at least 1x1
    if (new_width < 1) new_width = 1;
    if (new_height < 1) new_height = 1;
}

/**
 * @brief Simple bilinear interpolation for image scaling
 */
static void scale_row(const png_bytep src_row, png_bytep dst_row, 
                      int src_width, int dst_width, int channels) {
    float ratio = static_cast<float>(src_width - 1) / dst_width;
    
    for (int x = 0; x < dst_width; x++) {
        float src_x = x * ratio;
        int x0 = static_cast<int>(src_x);
        int x1 = (x0 < src_width - 1) ? x0 + 1 : x0;
        float weight = src_x - x0;
        
        for (int c = 0; c < channels; c++) {
            int src_idx0 = x0 * channels + c;
            int src_idx1 = x1 * channels + c;
            
            float value = (1.0f - weight) * src_row[src_idx0] + weight * src_row[src_idx1];
            dst_row[x * channels + c] = static_cast<png_byte>(value + 0.5f);
        }
    }
}

static void scale_image_bilinear(const png_bytep src, int src_width, int src_height,
                                  png_bytep dst, int dst_width, int dst_height, int channels) {
    float ratio_y = static_cast<float>(src_height - 1) / dst_height;
    
    // Temporary row for vertical interpolation
    std::vector<png_byte> row0(src_width * channels);
    std::vector<png_byte> row1(src_width * channels);
    
    for (int y = 0; y < dst_height; y++) {
        float src_y = y * ratio_y;
        int y0 = static_cast<int>(src_y);
        int y1 = (y0 < src_height - 1) ? y0 + 1 : y0;
        float weight_y = src_y - y0;
        
        // Get source rows
        const png_bytep src_row0 = src + y0 * src_width * channels;
        const png_bytep src_row1 = src + y1 * src_width * channels;
        
        // Interpolate vertically first
        for (int x = 0; x < src_width; x++) {
            for (int c = 0; c < channels; c++) {
                float v0 = src_row0[x * channels + c];
                float v1 = src_row1[x * channels + c];
                float value = (1.0f - weight_y) * v0 + weight_y * v1;
                row0[x * channels + c] = static_cast<png_byte>(value + 0.5f);
            }
        }
        
        // Then interpolate horizontally
        png_bytep dst_row = dst + y * dst_width * channels;
        scale_row(row0.data(), dst_row, src_width, dst_width, channels);
    }
}

/**
 * @brief Load image using libpng, scale it, and encode back to PNG
 */
static std::string load_and_scale_image(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        return "";
    }
    
    // Create PNG structure
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        fclose(fp);
        return "";
    }
    
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(fp);
        return "";
    }
    
    // Set up error handling
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        return "";
    }
    
    // Initialize I/O
    png_init_io(png, fp);
    
    // Read image info
    png_read_info(png, info);
    
    int width = png_get_image_width(png, info);
    int height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    int bit_depth = png_get_bit_depth(png, info);
    
    // Convert to RGBA if needed
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (bit_depth == 16)
        png_set_strip_16(png);
    
    // Add alpha channel if not present
    if (!(color_type & PNG_COLOR_MASK_ALPHA))
        png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
    
    // Update info after transformations
    png_read_update_info(png, info);
    
    // Calculate new dimensions for scaling (longest edge = MAX_SCALED_DIMENSION)
    // If image is smaller than target, keep original size; otherwise scale
    // Calculate new dimensions for scaling (longest edge = MAX_SCALED_DIMENSION)
    // Note: marked volatile to suppress GCC warning about potential clobbering by setjmp/longjmp in libpng
    volatile int new_width = width;
    volatile int new_height = height;
    if (width > MAX_SCALED_DIMENSION || height > MAX_SCALED_DIMENSION) {
        int scaled_w, scaled_h;
        calculate_scaled_dimensions(width, height, MAX_SCALED_DIMENSION, scaled_w, scaled_h);
        new_width = scaled_w;
        new_height = scaled_h;
    }
    
    // Allocate buffers
    int channels = 4; // RGBA
    std::vector<png_byte> src_data(height * width * channels);
    std::vector<png_bytep> row_pointers(height);
    
    for (int y = 0; y < height; y++) {
        row_pointers[y] = src_data.data() + y * width * channels;
    }
    
    // Read image data
    png_read_image(png, row_pointers.data());
    fclose(fp);
    
    // Scale the image if needed
    std::vector<png_byte> scaled_data;
    std::vector<png_bytep> scaled_row_pointers;
    
    if (new_width != width || new_height != height) {
        scaled_data.resize(new_width * new_height * channels);
        scaled_row_pointers.resize(new_height);
        
        for (int y = 0; y < new_height; y++) {
            scaled_row_pointers[y] = scaled_data.data() + y * new_width * channels;
        }
        
        // Scale using bilinear interpolation
        scale_image_bilinear(src_data.data(), width, height,
                            scaled_data.data(), new_width, new_height, channels);
    } else {
        scaled_data = std::move(src_data);
        scaled_row_pointers = std::move(row_pointers);
    }
    
    // Clean up read structures
    png_destroy_read_struct(&png, &info, nullptr);
    
    // Now encode the scaled image to PNG
    png_structp write_png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!write_png) {
        return "";
    }
    
    png_infop write_info = png_create_info_struct(write_png);
    if (!write_info) {
        png_destroy_write_struct(&write_png, nullptr);
        return "";
    }
    
    // Set up error handling for writing
    if (setjmp(png_jmpbuf(write_png))) {
        png_destroy_write_struct(&write_png, &write_info);
        return "";
    }
    
    // Create memory buffer for writing (reserved for future use)
    std::vector<unsigned char> png_output_buffer;
    png_output_buffer.reserve(new_width * new_height); // Rough estimate
    
    // Write to a temp file
    std::string temp_path = std::filesystem::temp_directory_path().string() + "/daedalus_scaled_" + 
                           std::to_string(std::time(nullptr)) + ".png";
    
    FILE* out_fp = fopen(temp_path.c_str(), "wb");
    if (!out_fp) {
        png_destroy_write_struct(&write_png, &write_info);
        return "";
    }
    
    png_init_io(write_png, out_fp);
    
    // Set compression
    png_set_compression_level(write_png, 6);
    
    // Set image attributes
    png_set_IHDR(write_png, write_info, new_width, new_height, 
                 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    
    // Write image
    png_write_info(write_png, write_info);
    png_write_image(write_png, scaled_row_pointers.data());
    png_write_end(write_png, nullptr);
    
    fclose(out_fp);
    png_destroy_write_struct(&write_png, &write_info);
    
    // Read the PNG file back and encode to base64
    std::ifstream png_file(temp_path, std::ios::binary);
    std::vector<unsigned char> png_buffer((std::istreambuf_iterator<char>(png_file)),
                                          std::istreambuf_iterator<char>());
    png_file.close();
    
    // Clean up temp file
    std::filesystem::remove(temp_path);
    
    if (png_buffer.empty()) {
        return "";
    }
    
    // Encode to base64
    std::string base64 = base64_encode(png_buffer.data(), png_buffer.size());
    
    // Return as data URI (PNG format)
    return "data:image/png;base64," + base64;
}

std::string ImageProcessor::read_file_as_base64(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    
    // Read file into memory
    std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
    file.close();
    
    if (buffer.empty()) {
        return "";
    }
    
    // Detect format from magic bytes
    std::string mime_type = detect_format(buffer.data(), buffer.size());
    
    // If detection failed, try extension
    if (mime_type == "application/octet-stream") {
        mime_type = get_mime_from_extension(path);
    }
    
    // Encode to base64
    std::string base64 = base64_encode(buffer.data(), buffer.size());
    
    // Return as data URI
    return "data:" + mime_type + ";base64," + base64;
}

std::string ImageProcessor::load_local_image_absolute(const std::string& absolute_path) {
    return load_and_scale_image(absolute_path);
}

std::string ImageProcessor::load_local_image(const std::string& path, const std::string& project_root) {
    // Resolve the path
    std::filesystem::path file_path;
    
    // Check if path is absolute
    if (std::filesystem::path(path).is_absolute()) {
        file_path = path;
    } else {
        // Make relative to project root
        file_path = std::filesystem::path(project_root) / path;
    }
    
    // Check if file exists
    if (!std::filesystem::exists(file_path)) {
        return "";
    }
    
    // Check if it's a regular file
    if (!std::filesystem::is_regular_file(file_path)) {
        return "";
    }
    
    // Check file size (max 10MB)
    size_t file_size = std::filesystem::file_size(file_path);
    const size_t MAX_FILE_SIZE = 10 * 1024 * 1024; // 10MB
    if (file_size > MAX_FILE_SIZE) {
        return "";
    }
    
    // Load, scale to 512px longest edge, and encode to base64 PNG
    return load_and_scale_image(file_path.string());
}
