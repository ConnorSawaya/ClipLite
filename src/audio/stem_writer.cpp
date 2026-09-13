#include "cliplite/audio/stem_writer.h"

namespace cliplite::audio {

namespace {
// All multi-byte fields are little-endian (true on every supported target).
void put_u16(std::ofstream& f, uint16_t v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}
void put_u32(std::ofstream& f, uint32_t v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}
void put_tag(std::ofstream& f, const char tag[4]) {
    f.write(tag, 4);
}
}  // namespace

StemSegmentWriter::~StemSegmentWriter() {
    close();
}

bool StemSegmentWriter::open(const std::string& utf8_path, uint32_t sample_rate,
                             uint16_t channels) {
    if (open_ || utf8_path.empty() || sample_rate == 0 || channels == 0) return false;
    out_.open(utf8_path, std::ios::binary | std::ios::trunc);
    if (!out_) return false;
    sample_rate_ = sample_rate;
    channels_ = channels;
    frames_ = 0;
    // Placeholder header (44 bytes); data size patched in close().
    put_tag(out_, "RIFF");
    put_u32(out_, 0);  // ChunkSize = 36 + data_bytes
    put_tag(out_, "WAVE");
    put_tag(out_, "fmt ");
    put_u32(out_, 16);         // Subchunk1Size (PCM)
    put_u16(out_, 3);          // AudioFormat = IEEE float
    put_u16(out_, channels_);
    put_u32(out_, sample_rate_);
    put_u32(out_, sample_rate_ * channels_ * 4);  // ByteRate
    put_u16(out_, static_cast<uint16_t>(channels_ * 4));  // BlockAlign
    put_u16(out_, 32);                                    // BitsPerSample
    put_tag(out_, "data");
    put_u32(out_, 0);  // Subchunk2Size = data_bytes
    if (!out_) {
        out_.close();
        return false;
    }
    open_ = true;
    return true;
}

bool StemSegmentWriter::append(const float* pcm, uint32_t frames) {
    if (!open_ || !pcm || frames == 0) return false;
    const uint32_t bytes = frames * channels_ * 4;
    out_.write(reinterpret_cast<const char*>(pcm), bytes);
    if (!out_) return false;
    frames_ += frames;
    return true;
}

bool StemSegmentWriter::close() {
    if (!open_) return true;
    open_ = false;
    const uint32_t data_bytes = frames_ * channels_ * 4;
    // Patch ChunkSize at offset 4 and Subchunk2Size at offset 40.
    out_.seekp(4, std::ios::beg);
    put_u32(out_, 36 + data_bytes);
    out_.seekp(40, std::ios::beg);
    put_u32(out_, data_bytes);
    out_.flush();
    const bool ok = static_cast<bool>(out_);
    out_.close();
    return ok;
}

}  // namespace cliplite::audio
