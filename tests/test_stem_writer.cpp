#include "test_main.h"

#include <cstdio>
#include <filesystem>

#include "cliplite/audio/stem_writer.h"

using namespace cliplite::audio;

namespace {
std::string tmp_wav(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
#pragma pack(push, 1)
struct WavHead {
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    char data[4];
    uint32_t data_size;
};
#pragma pack(pop)
}  // namespace

TEST(stem_writer_roundtrip) {
    const std::string path = tmp_wav("cliplite_stem_test.wav");
    std::remove(path.c_str());
    StemSegmentWriter w;
    CHECK(w.open(path));
    CHECK(w.is_open());
    float pcm[8] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};  // 4 stereo frames
    CHECK(w.append(pcm, 4));
    CHECK_EQ(w.frames_written(), 4u);
    CHECK(w.close());
    CHECK(!w.is_open());
    CHECK(w.close());  // idempotent

    FILE* f = nullptr;
    CHECK(fopen_s(&f, path.c_str(), "rb") == 0 && f != nullptr);
    WavHead h{};
    CHECK(fread(&h, sizeof(h), 1, f) == 1);
    CHECK(std::string(h.riff, 4) == "RIFF");
    CHECK(std::string(h.wave, 4) == "WAVE");
    CHECK_EQ(h.audio_format, 3u);  // IEEE float
    CHECK_EQ(h.channels, 2u);
    CHECK_EQ(h.sample_rate, 48000u);
    CHECK_EQ(h.data_size, 4u * 2u * 4u);
    CHECK_EQ(h.chunk_size, 36u + h.data_size);
    float back[8] = {0};
    CHECK(fread(back, sizeof(back), 1, f) == 1);
    for (int i = 0; i < 8; ++i) CHECK(back[i] == pcm[i]);  // bit-exact
    fclose(f);
    std::remove(path.c_str());
}

TEST(stem_writer_rejects_bad_use) {
    StemSegmentWriter w;
    float pcm[2] = {0, 0};
    CHECK(!w.append(pcm, 1));          // never opened
    CHECK(!w.open("", 48000, 2));      // empty path
    CHECK(!w.open(tmp_wav("x.wav"), 0, 2));
    CHECK(!w.open(tmp_wav("x.wav"), 48000, 0));
    // Unwritable directory.
    CHECK(!w.open("Z:\\definitely\\not\\here\\f.wav"));
    CHECK(!w.is_open());
}
