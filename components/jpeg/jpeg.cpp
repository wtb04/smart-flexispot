#include "jpeg.h"

#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jpeglib.h"
#include "units.h"

#include <algorithm>
#include <csetjmp>
#include <cstring>

namespace jpeg {
namespace {
constexpr char TAG[] = "jpeg";

constexpr int ENGINE_TIMEOUT_MS = 2 * units::kMsPerSecond;

constexpr std::size_t OUT_BUFFER_SIZE = kMaxSide * kMaxSide * sizeof(std::uint16_t);

// An MCU is 16 pixels wide where the chroma is halved across, 8 where it is not.
constexpr int MCU_WIDTH_SUBSAMPLED = 16;
constexpr int MCU_WIDTH_FULL       = 8;

// The engine only takes pictures whose pixel count is a multiple of this.
constexpr unsigned ENGINE_PIXEL_MULTIPLE = 8;

// libjpeg scales down by 1/2, 1/4 or 1/8 and no further.
constexpr unsigned MAX_SCALE_DENOM = 8;

constexpr std::uint8_t MARKER_PREFIX = 0xff;
constexpr std::uint8_t SOF0          = 0xc0;  // baseline
constexpr std::uint8_t SOF1          = 0xc1;  // extended sequential, Huffman
constexpr std::uint8_t SOF2          = 0xc2;
constexpr std::uint8_t SOF15         = 0xcf;
constexpr std::uint8_t DHT           = 0xc4;  // these three share the SOFn range
constexpr std::uint8_t JPG           = 0xc8;
constexpr std::uint8_t DAC           = 0xcc;
constexpr std::uint8_t SOS           = 0xda;

constexpr std::size_t SOI_LENGTH     = 2;
constexpr std::size_t MARKER_LENGTH  = 2;
constexpr std::size_t LENGTH_FIELD   = 2;
constexpr std::size_t SEGMENT_HEADER = MARKER_LENGTH + LENGTH_FIELD;
constexpr int         BITS_PER_BYTE  = 8;

jpeg_decoder_handle_t s_engine = nullptr;
std::uint8_t         *s_in     = nullptr;  // where the engine reads from
std::uint8_t         *s_out    = nullptr;  // and writes to
SemaphoreHandle_t     s_lock   = nullptr;
StaticSemaphore_t     s_lock_ctrl;

// The engine writes whole MCUs, so a picture whose width is not a multiple of
// the MCU width comes back padded on the end of every row. Reading it at the
// picture width shears the image into diagonal streaks.
int decoded_stride(const jpeg_decode_picture_info_t &info)
{
    const int mcu_w = info.sample_method == JPEG_DOWN_SAMPLING_YUV422 ||
                              info.sample_method == JPEG_DOWN_SAMPLING_YUV420
                          ? MCU_WIDTH_SUBSAMPLED
                          : MCU_WIDTH_FULL;
    return (static_cast<int>(info.width) + mcu_w - 1) / mcu_w * mcu_w;
}

bool starts_other_frame(std::uint8_t marker)
{
    return marker >= SOF2 && marker <= SOF15 && marker != DHT && marker != JPG && marker != DAC;
}

std::size_t segment_length(const std::uint8_t *length)
{
    return (static_cast<std::size_t>(length[0]) << BITS_PER_BYTE) + length[1];
}

bool baseline(const std::uint8_t *data, std::size_t length)
{
    std::size_t at = SOI_LENGTH;
    while (at + SEGMENT_HEADER <= length && data[at] == MARKER_PREFIX) {
        const std::uint8_t marker = data[at + 1];
        if (marker == SOF0 || marker == SOF1) {
            return true;
        }
        if (marker == SOS || starts_other_frame(marker)) {
            return false;
        }
        at += MARKER_LENGTH + segment_length(data + at + MARKER_LENGTH);
    }
    return false;
}

struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf        escape;
};

void on_jpeg_error(j_common_ptr info)
{
    std::longjmp(reinterpret_cast<JpegError *>(info->err)->escape, 1);
}

bool decode_soft(const void *data, std::size_t length, std::uint16_t *out, int max_w, int max_h,
                 int &out_w, int &out_h)
{
    jpeg_decompress_struct info{};
    JpegError              err{};

    info.err           = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_jpeg_error;
    if (setjmp(err.escape) != 0) {
        jpeg_destroy_decompress(&info);
        return false;
    }

    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, static_cast<const unsigned char *>(data), length);
    jpeg_read_header(&info, TRUE);

    // Despite the name this picks the byte order of the RGB565 word, not the
    // channel order: _RGB writes it big-endian and LVGL reads a native
    // little-endian uint16, which mangles red and blue into each other.
    info.out_color_space = JCS_RGB565;
    info.scale_num       = 1;
    info.scale_denom     = 1;
    jpeg_calc_output_dimensions(&info);
    while ((static_cast<int>(info.output_width) > max_w ||
            static_cast<int>(info.output_height) > max_h) &&
           info.scale_denom < MAX_SCALE_DENOM) {
        info.scale_denom *= 2;
        jpeg_calc_output_dimensions(&info);
    }
    if (static_cast<int>(info.output_width) > max_w ||
        static_cast<int>(info.output_height) > max_h) {
        jpeg_destroy_decompress(&info);
        return false;
    }

    jpeg_start_decompress(&info);
    out_w = static_cast<int>(info.output_width);
    out_h = static_cast<int>(info.output_height);
    while (info.output_scanline < info.output_height) {
        auto *row = reinterpret_cast<JSAMPROW>(
            out + static_cast<std::size_t>(info.output_scanline) * out_w);
        jpeg_read_scanlines(&info, &row, 1);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    return true;
}

bool decode_locked(std::size_t length, int max_w, int max_h, Picture &picture)
{
    max_w = std::min(max_w, kMaxSide);
    max_h = std::min(max_h, kMaxSide);

    // The engine takes baseline pictures whose pixel count divides by eight;
    // anything else, or anything too big for the box, goes through software.
    // Baseline is checked first: the engine cannot read a progressive header,
    // and says so on the console at error level when asked to.
    jpeg_decode_picture_info_t info{};
    const bool engine = baseline(s_in, length) &&
                        jpeg_decoder_get_info(s_in, length, &info) == ESP_OK &&
                        static_cast<int>(info.width) <= max_w &&
                        static_cast<int>(info.height) <= max_h &&
                        (info.width * info.height) % ENGINE_PIXEL_MULTIPLE == 0;
    if (engine) {
        const bool grey = info.sample_method == JPEG_DOWN_SAMPLING_GRAY;

        jpeg_decode_cfg_t cfg{};
        cfg.output_format = grey ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565;
        cfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

        std::uint32_t produced = 0;
        const esp_err_t err = jpeg_decoder_process(s_engine, &cfg, s_in, length, s_out,
                                                   OUT_BUFFER_SIZE, &produced);
        if (err == ESP_OK) {
            picture = {s_out, static_cast<int>(info.width), static_cast<int>(info.height),
                       decoded_stride(info), grey, true};
            return true;
        }
        ESP_LOGW(TAG, "engine refused a %ux%u picture: %s", info.width, info.height,
                 esp_err_to_name(err));
    }

    int width  = 0;
    int height = 0;
    if (!decode_soft(s_in, length, reinterpret_cast<std::uint16_t *>(s_out), max_w, max_h, width,
                     height)) {
        return false;
    }
    picture = {s_out, width, height, width, false, false};
    return true;
}

struct CopyOut {
    std::uint16_t *out;
    int           *out_w;
    int           *out_h;
};

void copy_out(const Picture &picture, void *context)
{
    const auto &into = *static_cast<CopyOut *>(context);
    *into.out_w      = picture.width;
    *into.out_h      = picture.height;
    for (int y = 0; y < picture.height; ++y) {
        std::uint16_t *row = into.out + static_cast<std::size_t>(y) * picture.width;
        if (picture.grey) {
            const auto *src = static_cast<const std::uint8_t *>(picture.pixels) +
                              static_cast<std::size_t>(y) * picture.stride;
            for (int x = 0; x < picture.width; ++x) {
                row[x] = grey_to_rgb565(src[x]);
            }
        } else {
            const auto *src = static_cast<const std::uint16_t *>(picture.pixels) +
                              static_cast<std::size_t>(y) * picture.stride;
            std::memcpy(row, src, static_cast<std::size_t>(picture.width) * sizeof(std::uint16_t));
        }
    }
}

}  // namespace

esp_err_t start()
{
    ESP_RETURN_ON_FALSE(s_lock == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, TAG, "lock");

    const jpeg_decode_engine_cfg_t engine{.intr_priority = 0, .timeout_ms = ENGINE_TIMEOUT_MS};
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine, &s_engine), TAG, "engine");

    jpeg_decode_memory_alloc_cfg_t in{.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t out{.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    std::size_t got = 0;
    s_in  = static_cast<std::uint8_t *>(jpeg_alloc_decoder_mem(kMaxInput, &in, &got));
    s_out = static_cast<std::uint8_t *>(jpeg_alloc_decoder_mem(OUT_BUFFER_SIZE, &out, &got));
    ESP_RETURN_ON_FALSE(s_in != nullptr && s_out != nullptr, ESP_ERR_NO_MEM, TAG, "buffers");
    return ESP_OK;
}

bool decode(const void *data, std::size_t length, int max_w, int max_h, Use use, void *context)
{
    if (data == nullptr || length == 0 || length > kMaxInput || s_engine == nullptr ||
        use == nullptr) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::memcpy(s_in, data, length);
    Picture   picture{};
    const bool ok = decode_locked(length, max_w, max_h, picture);
    if (ok) {
        use(picture, context);
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool decode_into(const void *data, std::size_t length, std::uint16_t *out, int max_w, int max_h,
                 int &out_w, int &out_h)
{
    if (out == nullptr) {
        return false;
    }
    CopyOut into{out, &out_w, &out_h};
    return decode(data, length, max_w, max_h, copy_out, &into);
}

}  // namespace jpeg
