// Plain GameCube GCM/ISO implementation of the nod v2.0.0-alpha.10 C API (nod_gcm/nod.h).
//
// Used instead of the Rust nod library where Rust cannot be linked (Emscripten). Only
// uncompressed GameCube images are supported; compressed containers and Wii discs are
// rejected with NOD_RESULT_ERR_FORMAT. Observable behaviour (read chunking, seek rules,
// FST iteration, error codes) follows nod's ISO + GameCube partition readers.

#include "nod_gcm.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif

namespace {

constexpr uint32_t kHandleMagic = 0x4E4F4448; // "NODH"
constexpr size_t kBootSize = 0x440;
constexpr size_t kBi2Size = 0x2000;
constexpr size_t kDiscHeaderSize = 0x400;
constexpr uint64_t kBb2Offset = 0x420;
constexpr size_t kApploaderHeaderSize = 0x20;
constexpr size_t kDolHeaderSize = 0x100;
constexpr size_t kFstNodeSize = 12;
// nod reads disc data in sector groups of 64 * 0x8000 bytes; a single read never crosses one.
constexpr uint64_t kSectorGroupSize = 64 * 0x8000;

static_assert(sizeof(NodDiscHeader) == kDiscHeaderSize);

thread_local std::string t_lastError;
thread_local bool t_hasError = false;

void clear_last_error() { t_hasError = false; }

void set_last_error(std::string msg) {
  t_lastError = std::move(msg);
  t_hasError = true;
}

uint32_t be32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

uint64_t saturating_add_signed(uint64_t base, int64_t delta) {
  if (delta >= 0) {
    const uint64_t d = static_cast<uint64_t>(delta);
    return base > UINT64_MAX - d ? UINT64_MAX : base + d;
  }
  const uint64_t d = 0 - static_cast<uint64_t>(delta);
  return base < d ? 0 : base - d;
}

enum class IoStatus { Ok, Error, Eof };

// Shared callback stream; `close` runs when the last handle referencing it is freed.
struct SharedStream {
  NodDiscStream callbacks;
  std::mutex mutex;
  uint64_t size = 0;

  explicit SharedStream(const NodDiscStream& stream) : callbacks(stream) {}
  ~SharedStream() { callbacks.close(callbacks.user_data); }
  SharedStream(const SharedStream&) = delete;
  SharedStream& operator=(const SharedStream&) = delete;

  IoStatus read_exact_at(uint64_t offset, uint8_t* out, size_t len) {
    size_t total = 0;
    while (total < len) {
      int64_t result;
      {
        std::lock_guard lk{mutex};
        result = callbacks.read_at(callbacks.user_data, offset + total, out + total, len - total);
      }
      if (result < 0) {
        set_last_error("NodDiscStream.read_at callback failed");
        return IoStatus::Error;
      }
      if (result == 0) {
        set_last_error("failed to fill whole buffer");
        return IoStatus::Eof;
      }
      if (static_cast<uint64_t>(result) > len - total) {
        set_last_error("NodDiscStream.read_at callback returned more bytes than requested");
        return IoStatus::Error;
      }
      total += static_cast<size_t>(result);
    }
    return IoStatus::Ok;
  }
};

enum class HandleKind { Disc, Partition, File };

} // namespace

struct NodHandle {
  uint32_t magic = kHandleMagic;
  HandleKind kind;
  std::shared_ptr<SharedStream> stream;
  uint64_t discSize = 0;
  // Absolute disc position (all handle kinds).
  uint64_t pos = 0;
  // File window [begin, end) in absolute disc offsets.
  uint64_t begin = 0;
  uint64_t end = 0;

  // Disc: boot.bin. Partition: partition metadata blobs.
  std::vector<uint8_t> rawBoot;
  std::vector<uint8_t> rawBi2;
  std::vector<uint8_t> rawApploader;
  std::vector<uint8_t> rawDol;
  std::vector<uint8_t> rawFst;

  // nod_buf_read cache: bytes at absolute offset bufStart.
  std::vector<uint8_t> buf;
  uint64_t bufStart = 0;

  NodHandle(HandleKind k, std::shared_ptr<SharedStream> s, uint64_t size)
  : kind(k), stream(std::move(s)), discSize(size) {}
  ~NodHandle() { magic = 0; }

  bool is_valid() const { return magic == kHandleMagic; }

  // Bytes available to a single read at the current position (nod's fill_buf length).
  uint64_t available() const {
    uint64_t limit = UINT64_MAX;
    if (kind == HandleKind::File) {
      if (pos >= end) {
        return 0;
      }
      limit = end - pos;
    }
    if (pos >= discSize) {
      return 0;
    }
    const uint64_t groupStart = pos - pos % kSectorGroupSize;
    const uint64_t groupEnd = std::min(groupStart + kSectorGroupSize, discSize);
    return std::min(limit, groupEnd - pos);
  }
};

namespace {

NodHandle* check_handle(const NodHandle* handle) {
  if (handle == nullptr) {
    set_last_error("null handle");
    return nullptr;
  }
  if (!handle->is_valid()) {
    set_last_error("use after free");
    return nullptr;
  }
  return const_cast<NodHandle*>(handle);
}

// read_exact over a GameCube partition reader: data past the disc end is unreadable.
NodResult read_partition_exact(NodHandle& part, uint64_t offset, uint8_t* out, size_t len, const char* context) {
  if (len == 0) {
    return NOD_RESULT_OK;
  }
  if (offset >= part.discSize || len > part.discSize - offset) {
    set_last_error(std::string(context) + ": failed to fill whole buffer");
    return NOD_RESULT_ERR_IO;
  }
  if (part.stream->read_exact_at(offset, out, len) != IoStatus::Ok) {
    set_last_error(std::string(context) + ": " + t_lastError);
    return NOD_RESULT_ERR_IO;
  }
  return NOD_RESULT_OK;
}

NodResult load_partition_meta(NodHandle& part) {
  part.rawBoot.resize(kBootSize);
  if (const auto r = read_partition_exact(part, 0, part.rawBoot.data(), kBootSize, "Reading boot.bin");
      r != NOD_RESULT_OK) {
    return r;
  }
  part.rawBi2.resize(kBi2Size);
  if (const auto r = read_partition_exact(part, kBootSize, part.rawBi2.data(), kBi2Size, "Reading bi2.bin");
      r != NOD_RESULT_OK) {
    return r;
  }

  // apploader.bin: 0x20-byte header + size + trailer_size.
  constexpr uint64_t apploaderOffset = kBootSize + kBi2Size;
  uint8_t apploaderHeader[kApploaderHeaderSize];
  if (const auto r = read_partition_exact(part, apploaderOffset, apploaderHeader, kApploaderHeaderSize,
                                          "Reading apploader header");
      r != NOD_RESULT_OK) {
    return r;
  }
  const size_t apploaderSize =
      kApploaderHeaderSize + size_t{be32(apploaderHeader + 0x14)} + size_t{be32(apploaderHeader + 0x18)};
  part.rawApploader.resize(apploaderSize);
  std::memcpy(part.rawApploader.data(), apploaderHeader, kApploaderHeaderSize);
  if (const auto r = read_partition_exact(part, apploaderOffset + kApploaderHeaderSize,
                                          part.rawApploader.data() + kApploaderHeaderSize,
                                          apploaderSize - kApploaderHeaderSize, "Reading apploader");
      r != NOD_RESULT_OK) {
    return r;
  }

  const uint8_t* bb2 = part.rawBoot.data() + kBb2Offset;
  const uint64_t dolOffset = be32(bb2 + 0);
  const uint64_t fstOffset = be32(bb2 + 4);
  const uint32_t fstSize = be32(bb2 + 8);

  // fst.bin
  part.rawFst.resize(fstSize);
  if (const auto r = read_partition_exact(part, fstOffset, part.rawFst.data(), fstSize, "Reading partition FST");
      r != NOD_RESULT_OK) {
    return r;
  }

  // main.dol: size = max(offset + size) over the 7 text and 11 data sections.
  uint8_t dolHeader[kDolHeaderSize];
  if (const auto r = read_partition_exact(part, dolOffset, dolHeader, kDolHeaderSize, "Reading DOL header");
      r != NOD_RESULT_OK) {
    return r;
  }
  uint32_t dolSize = 0;
  for (size_t i = 0; i < 18; ++i) {
    const uint32_t offs = be32(dolHeader + 0x00 + i * 4);
    const uint32_t size = be32(dolHeader + 0x90 + i * 4);
    dolSize = std::max(dolSize, offs + size);
  }
  if (dolSize < kDolHeaderSize) {
    set_last_error("disc format error: DOL size is smaller than the DOL header");
    return NOD_RESULT_ERR_FORMAT;
  }
  part.rawDol.resize(dolSize);
  std::memcpy(part.rawDol.data(), dolHeader, kDolHeaderSize);
  return read_partition_exact(part, dolOffset + kDolHeaderSize, part.rawDol.data() + kDolHeaderSize,
                              dolSize - kDolHeaderSize, "Reading DOL");
}

// View of a raw FST (nod::disc::fst::Fst).
struct FstView {
  const uint8_t* nodes = nullptr;
  size_t count = 0;
  const uint8_t* strings = nullptr;
  size_t stringsSize = 0;

  bool is_file(size_t idx) const { return nodes[idx * kFstNodeSize] == 0; }
  bool is_dir(size_t idx) const { return nodes[idx * kFstNodeSize] == 1; }
  // nod maps NodeKind::Invalid to NOD_NODE_KIND_FILE.
  NodNodeKind kind(size_t idx) const { return is_dir(idx) ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE; }
  uint32_t name_offset(size_t idx) const {
    const uint8_t* n = nodes + idx * kFstNodeSize;
    return (uint32_t{n[1]} << 16) | (uint32_t{n[2]} << 8) | uint32_t{n[3]};
  }
  uint32_t offset(size_t idx) const { return be32(nodes + idx * kFstNodeSize + 4); }
  uint32_t length(size_t idx) const { return be32(nodes + idx * kFstNodeSize + 8); }
  bool name(size_t idx, std::string& out) const;
};

bool parse_fst(const std::vector<uint8_t>& raw, FstView& out) {
  if (raw.size() < kFstNodeSize) {
    set_last_error("FST root node not found");
    return false;
  }
  // Root node length = total node count; the string table follows the last node.
  const uint32_t stringBase = be32(raw.data() + 8) * static_cast<uint32_t>(kFstNodeSize);
  if (stringBase > raw.size()) {
    set_last_error("FST string table out of bounds");
    return false;
  }
  out.nodes = raw.data();
  out.count = stringBase / kFstNodeSize;
  out.strings = raw.data() + stringBase;
  out.stringsSize = raw.size() - stringBase;
  return true;
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// WHATWG Shift_JIS decoder (as encoding_rs::SHIFT_JIS, which nod uses) without the
// JIS X 0208 index: ASCII, U+0080, half-width katakana and the user-defined area decode
// exactly; other double-byte characters decode to U+FFFD.
std::string decode_shift_jis(const uint8_t* s, size_t len) {
  std::string out;
  out.reserve(len);
  size_t i = 0;
  while (i < len) {
    const uint8_t b = s[i++];
    if (b <= 0x80) {
      append_utf8(out, b);
    } else if (b >= 0xA1 && b <= 0xDF) {
      append_utf8(out, 0xFF61 + (b - 0xA1));
    } else if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
      if (i >= len) {
        append_utf8(out, 0xFFFD);
        break;
      }
      const uint8_t t = s[i];
      const bool trailValid = (t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC);
      if (trailValid) {
        const uint32_t leadOffset = b < 0xA0 ? 0x81 : 0xC1;
        const uint32_t trailOffset = t < 0x7F ? 0x40 : 0x41;
        const uint32_t pointer = (b - leadOffset) * 188 + (t - trailOffset);
        ++i;
        append_utf8(out, pointer >= 8836 && pointer <= 10715 ? 0xE000 + pointer - 8836 : 0xFFFD);
      } else {
        // Invalid trail: an ASCII trail byte is re-processed on its own.
        append_utf8(out, 0xFFFD);
        if (t >= 0x80) {
          ++i;
        }
      }
    } else {
      append_utf8(out, 0xFFFD);
    }
  }
  return out;
}

bool FstView::name(size_t idx, std::string& out) const {
  const uint32_t off = name_offset(idx);
  if (off > stringsSize) {
    return false;
  }
  const auto* start = strings + off;
  const auto* nul = static_cast<const uint8_t*>(std::memchr(start, 0, stringsSize - off));
  if (nul == nullptr) {
    return false;
  }
  out = decode_shift_jis(start, static_cast<size_t>(nul - start));
  return true;
}

bool ascii_iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = static_cast<char>(x - 'A' + 'a');
    }
    if (y >= 'A' && y <= 'Z') {
      y = static_cast<char>(y - 'A' + 'a');
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

bool is_valid_utf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<uint8_t>(s[i]);
    size_t n;
    uint32_t cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if (c >= 0xC2 && c <= 0xDF) {
      n = 1;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      n = 2;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      n = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + n >= s.size()) {
      return false;
    }
    for (size_t k = 1; k <= n; ++k) {
      const auto cc = static_cast<uint8_t>(s[i + k]);
      if ((cc & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF))) {
      return false;
    }
    i += n + 1;
  }
  return true;
}

// Splits a path the way Rust's `str::split('/')` + nod's `next_non_empty` do.
struct PathSegments {
  std::string_view rest;
  bool done = false;

  std::string_view next_non_empty() {
    while (!done) {
      std::string_view seg;
      if (const size_t slash = rest.find('/'); slash == std::string_view::npos) {
        seg = rest;
        done = true;
      } else {
        seg = rest.substr(0, slash);
        rest.remove_prefix(slash + 1);
      }
      if (!seg.empty()) {
        return seg;
      }
    }
    return {};
  }
};

NodResult open_partition(NodHandle* disc, bool ok, const char* notFoundMessage, NodHandle** out) {
  if (out == nullptr) {
    set_last_error("null pointer argument");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  NodHandle* d = check_handle(disc);
  if (d == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (d->kind != HandleKind::Disc) {
    set_last_error("handle is not a disc");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (!ok) {
    set_last_error(notFoundMessage);
    return NOD_RESULT_ERR_FORMAT;
  }
  auto part = std::make_unique<NodHandle>(HandleKind::Partition, d->stream, d->discSize);
  if (const NodResult r = load_partition_meta(*part); r != NOD_RESULT_OK) {
    return r;
  }
  part->pos = 0;
  *out = part.release();
  return NOD_RESULT_OK;
}

int64_t seek_handle(NodHandle& h, int64_t offset, int32_t whence) {
  if (whence < 0 || whence > 2) {
    set_last_error("invalid whence value: " + std::to_string(whence));
    return -1;
  }
  switch (h.kind) {
  case HandleKind::Disc:
    if (whence == 2) {
      set_last_error("BlockIOReader: SeekFrom::End is not supported");
      return -1;
    }
    h.pos = whence == 0 ? static_cast<uint64_t>(offset) : saturating_add_signed(h.pos, offset);
    return static_cast<int64_t>(h.pos);
  case HandleKind::Partition:
    if (whence == 0) {
      h.pos = static_cast<uint64_t>(offset);
    } else {
      h.pos = saturating_add_signed(whence == 1 ? h.pos : h.discSize, offset);
    }
    return static_cast<int64_t>(h.pos);
  case HandleKind::File: {
    uint64_t pos;
    if (whence == 0) {
      pos = h.begin + static_cast<uint64_t>(offset); // wrapping, as nod's release build
    } else {
      pos = saturating_add_signed(whence == 1 ? h.pos : h.end, offset);
    }
    pos = std::clamp(pos, h.begin, h.end);
    h.pos = pos;
    return static_cast<int64_t>(pos - h.begin);
  }
  }
  return -1;
}

#ifndef __EMSCRIPTEN__
int64_t file_stream_read_at(void* userData, uint64_t offset, void* out, size_t len) {
  auto* f = static_cast<FILE*>(userData);
  if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) {
    return -1;
  }
  const size_t n = fread(out, 1, len, f);
  if (n == 0 && ferror(f)) {
    return -1;
  }
  return static_cast<int64_t>(n);
}

int64_t file_stream_len(void* userData) {
  auto* f = static_cast<FILE*>(userData);
  if (fseeko(f, 0, SEEK_END) != 0) {
    return -1;
  }
  return static_cast<int64_t>(ftello(f));
}

void file_stream_close(void* userData) { fclose(static_cast<FILE*>(userData)); }
#endif

} // namespace

#ifdef __EMSCRIPTEN__
// Browser build: the OPFS sync access handle is owned by a dedicated JS "disc server" worker (the page's
// disc-server.worker.ts), so any wasm thread — the DVD worker, ARQ/DVD callbacks, the main thread — can read
// without depending on another wasm thread's event loop. A request is published in this shared slot; the
// server performs the read straight into wasm memory and answers through `response`. Layout is a contract
// with the server (byte offsets in comments).
struct AuroraDiscServerBlock {
  uint32_t magic;    // 0: kDiscServerMagic
  uint32_t request;  // 4: sequence number of the published request (server waits on this word)
  uint32_t response; // 8: sequence number of the last completed request (requester waits on this word)
  uint32_t dest;     // 12: wasm address to read into
  uint32_t length;   // 16: bytes to read
  uint32_t ready;    // 20: 1 once the server has opened the disc and stored `size`
  double offset;     // 24: disc offset (exceeds 2^31)
  double result;     // 32: bytes read, or -1 on error
  double size;       // 40: disc size in bytes
};
static_assert(sizeof(AuroraDiscServerBlock) == 48);
static_assert(offsetof(AuroraDiscServerBlock, offset) == 24 && offsetof(AuroraDiscServerBlock, size) == 40);
constexpr uint32_t kDiscServerMagic = 0x43534944; // "DISC"

namespace {
AuroraDiscServerBlock g_discServer{.magic = kDiscServerMagic};
std::mutex g_discServerMutex;

int64_t em_stream_read_at(void*, uint64_t offset, void* out, size_t len) {
  if (__atomic_load_n(&g_discServer.ready, __ATOMIC_ACQUIRE) == 0) {
    return -1;
  }
  std::lock_guard lock{g_discServerMutex};
  g_discServer.dest = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(out));
  g_discServer.length = static_cast<uint32_t>(len);
  g_discServer.offset = static_cast<double>(offset);
  g_discServer.result = -1;
  const uint32_t sequence = g_discServer.request + 1;
  __atomic_store_n(&g_discServer.request, sequence, __ATOMIC_SEQ_CST);
  emscripten_futex_wake(&g_discServer.request, 1);
  for (;;) {
    const uint32_t done = __atomic_load_n(&g_discServer.response, __ATOMIC_SEQ_CST);
    if (done == sequence) {
      break;
    }
    emscripten_futex_wait(&g_discServer.response, done, INFINITY);
  }
  return static_cast<int64_t>(g_discServer.result);
}

int64_t em_stream_len(void*) {
  if (__atomic_load_n(&g_discServer.ready, __ATOMIC_ACQUIRE) == 0) {
    return -1;
  }
  return static_cast<int64_t>(g_discServer.size);
}

void em_stream_close(void*) {}

constexpr NodDiscStream kEmscriptenStream{
    .user_data = nullptr,
    .read_at = em_stream_read_at,
    .stream_len = em_stream_len,
    .close = em_stream_close,
};
} // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE AuroraDiscServerBlock* aurora_disc_server_block(void) { return &g_discServer; }
const NodDiscStream* nod_gcm_emscripten_stream(void) { return &kEmscriptenStream; }
}
#endif

extern "C" {

const char* nod_error_message(void) { return t_hasError ? t_lastError.c_str() : nullptr; }

NodResult nod_disc_open(const char* path, const NodDiscOptions* options, NodHandle** out) {
  clear_last_error();
  if (path == nullptr || out == nullptr) {
    set_last_error("null pointer argument");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
#ifdef __EMSCRIPTEN__
  // The only disc on web is the OPFS-backed module stream; `path` is informational.
  return nod_disc_open_stream(nod_gcm_emscripten_stream(), options, out);
#else
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    set_last_error(std::string("Failed to open ") + path + ": " + std::strerror(errno));
    return NOD_RESULT_ERR_IO;
  }
  const NodDiscStream stream{
      .user_data = f,
      .read_at = file_stream_read_at,
      .stream_len = file_stream_len,
      .close = file_stream_close,
  };
  return nod_disc_open_stream(&stream, options, out);
#endif
}

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out) {
  (void)options; // Partition encryption and preloading do not apply to plain GameCube images.
  clear_last_error();
  if (stream == nullptr || out == nullptr) {
    set_last_error("null pointer argument");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  // From here on the stream is owned: `close` runs on failure or when the last handle is freed.
  auto shared = std::make_shared<SharedStream>(*stream);

  // Format detection (first 0x20 bytes).
  uint8_t magic[0x20];
  switch (shared->read_exact_at(0, magic, sizeof(magic))) {
  case IoStatus::Ok:
    break;
  case IoStatus::Eof:
    set_last_error("disc format error: Unknown disc format");
    return NOD_RESULT_ERR_FORMAT;
  case IoStatus::Error:
    set_last_error("Detecting file type: " + t_lastError);
    return NOD_RESULT_ERR_IO;
  }
  static constexpr uint8_t kGcnMagic[4] = {0xC2, 0x33, 0x9F, 0x3D};
  static constexpr uint8_t kWiiMagic[4] = {0x5D, 0x1C, 0x9E, 0xA3};
  const bool gcn = std::memcmp(magic + 0x1C, kGcnMagic, 4) == 0;
  const bool wii = std::memcmp(magic + 0x18, kWiiMagic, 4) == 0;
  if (!gcn && !wii) {
    set_last_error("disc format error: Unknown disc format or unsupported compressed container (nod_gcm reads plain "
                   "GameCube ISO/GCM only)");
    return NOD_RESULT_ERR_FORMAT;
  }
  if (wii) {
    set_last_error("disc format error: Wii discs are not supported by nod_gcm");
    return NOD_RESULT_ERR_FORMAT;
  }

  int64_t len;
  {
    std::lock_guard lk{shared->mutex};
    len = shared->callbacks.stream_len(shared->callbacks.user_data);
  }
  if (len < 0) {
    set_last_error("Determining stream length: NodDiscStream.stream_len callback failed");
    return NOD_RESULT_ERR_IO;
  }
  shared->size = static_cast<uint64_t>(len);

  auto disc = std::make_unique<NodHandle>(HandleKind::Disc, shared, shared->size);
  // boot.bin; a partial final sector reads as zeroes.
  disc->rawBoot.assign(kBootSize, 0);
  const size_t bootRead = static_cast<size_t>(std::min<uint64_t>(kBootSize, shared->size));
  if (shared->read_exact_at(0, disc->rawBoot.data(), bootRead) != IoStatus::Ok) {
    set_last_error("Reading disc headers: " + t_lastError);
    return NOD_RESULT_ERR_IO;
  }
  *out = disc.release();
  return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition(NodHandle* disc, uint32_t index, const NodPartitionOptions* options,
                                  NodHandle** out) {
  (void)options;
  clear_last_error();
  return open_partition(disc, index == 0, "disc format error: GameCube discs only have one partition", out);
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions* options,
                                       NodHandle** out) {
  (void)options;
  clear_last_error();
  return open_partition(disc, kind == NOD_PARTITION_KIND_DATA,
                        "disc format error: GameCube discs only have a data partition", out);
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out) {
  clear_last_error();
  if (out == nullptr) {
    set_last_error("null pointer argument");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  NodHandle* part = check_handle(partition);
  if (part == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (part->kind != HandleKind::Partition) {
    set_last_error("handle is not a partition");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  FstView fst;
  if (!parse_fst(part->rawFst, fst)) {
    return NOD_RESULT_ERR_FORMAT;
  }
  if (fst_index >= fst.count) {
    set_last_error("FST index " + std::to_string(fst_index) + " out of range (total: " + std::to_string(fst.count) +
                   ")");
    return NOD_RESULT_ERR_NOT_FOUND;
  }
  if (!fst.is_file(fst_index)) {
    set_last_error("node is not a file");
    return NOD_RESULT_ERR_FORMAT;
  }
  auto file = std::make_unique<NodHandle>(HandleKind::File, part->stream, part->discSize);
  file->begin = fst.offset(fst_index);
  file->end = file->begin + fst.length(fst_index);
  file->pos = file->begin;
  *out = file.release();
  return NOD_RESULT_OK;
}

void nod_free(NodHandle* handle) {
  if (handle != nullptr && handle->is_valid()) {
    delete handle;
  }
}

int64_t nod_read(NodHandle* handle, uint8_t* buf, size_t len) {
  clear_last_error();
  if (buf == nullptr) {
    set_last_error("null pointer argument");
    return -1;
  }
  NodHandle* h = check_handle(handle);
  if (h == nullptr) {
    return -1;
  }
  const size_t n = static_cast<size_t>(std::min<uint64_t>(len, h->available()));
  if (n == 0) {
    return 0;
  }
  if (h->stream->read_exact_at(h->pos, buf, n) != IoStatus::Ok) {
    return -1;
  }
  h->pos += n;
  return static_cast<int64_t>(n);
}

int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence) {
  clear_last_error();
  NodHandle* h = check_handle(handle);
  if (h == nullptr) {
    return -1;
  }
  return seek_handle(*h, offset, whence);
}

const void* nod_buf_read(NodHandle* handle, size_t* out_len) {
  clear_last_error();
  if (out_len == nullptr) {
    set_last_error("null pointer argument");
    return nullptr;
  }
  NodHandle* h = check_handle(handle);
  if (h == nullptr) {
    return nullptr;
  }
  const size_t n = static_cast<size_t>(h->available());
  *out_len = n;
  if (n == 0) {
    return nullptr;
  }
  const bool cached = h->pos >= h->bufStart && h->pos - h->bufStart <= h->buf.size() &&
                      h->buf.size() - (h->pos - h->bufStart) >= n;
  if (!cached) {
    h->buf.resize(n);
    if (h->stream->read_exact_at(h->pos, h->buf.data(), n) != IoStatus::Ok) {
      h->buf.clear();
      *out_len = 0;
      return nullptr;
    }
    h->bufStart = h->pos;
  }
  return h->buf.data() + (h->pos - h->bufStart);
}

void nod_buf_consume(NodHandle* handle, size_t n) {
  if (handle == nullptr) {
    return;
  }
  if (!handle->is_valid()) {
    set_last_error("use after free");
    return;
  }
  handle->pos += n;
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
  clear_last_error();
  if (out == nullptr) {
    set_last_error("null pointer argument");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  const NodHandle* d = check_handle(disc);
  if (d == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (d->kind != HandleKind::Disc) {
    set_last_error("handle is not a disc");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  std::memcpy(out, d->rawBoot.data(), sizeof(NodDiscHeader));
  return NOD_RESULT_OK;
}

NodResult nod_disc_meta(const NodHandle* disc, NodDiscMeta* out) {
  clear_last_error();
  if (out == nullptr) {
    set_last_error("null pointer argument");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  const NodHandle* d = check_handle(disc);
  if (d == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (d->kind != HandleKind::Disc) {
    set_last_error("handle is not a disc");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  NodDiscMeta meta{};
  meta.format = NOD_FORMAT_ISO;
  meta.compression = NodCompression{NOD_COMPRESSION_KIND_NONE, 0};
  meta.lossless = true;
  meta.disc_size = d->discSize;
  *out = meta;
  return NOD_RESULT_OK;
}

uint64_t nod_disc_size(const NodHandle* disc) {
  const NodHandle* d = check_handle(disc);
  if (d == nullptr || d->kind != HandleKind::Disc) {
    return 0;
  }
  return d->discSize;
}

size_t nod_disc_partitions(const NodHandle* disc, NodPartitionInfo* out, size_t cap) {
  // GameCube discs have no partition table; nod reports zero partitions for them.
  (void)out;
  (void)cap;
  (void)check_handle(disc);
  return 0;
}

bool nod_partition_is_wii(const NodHandle* partition) {
  (void)check_handle(partition);
  return false;
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out) {
  clear_last_error();
  if (out == nullptr) {
    set_last_error("null pointer argument");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  const NodHandle* p = check_handle(partition);
  if (p == nullptr) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (p->kind != HandleKind::Partition) {
    set_last_error("handle is not a partition");
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  NodPartitionMeta meta{};
  meta.raw_boot = NodBlob{p->rawBoot.data(), p->rawBoot.size()};
  meta.raw_bi2 = NodBlob{p->rawBi2.data(), p->rawBi2.size()};
  meta.raw_apploader = NodBlob{p->rawApploader.data(), p->rawApploader.size()};
  meta.raw_dol = NodBlob{p->rawDol.data(), p->rawDol.size()};
  meta.raw_fst = NodBlob{p->rawFst.data(), p->rawFst.size()};
  *out = meta;
  return NOD_RESULT_OK;
}

uint32_t nod_partition_find_file(const NodHandle* partition, const char* path, NodNodeKind* out_kind,
                                 uint32_t* out_length) {
  clear_last_error();
  if (path == nullptr) {
    set_last_error("null pointer argument");
    return NOD_FST_STOP;
  }
  const NodHandle* p = check_handle(partition);
  if (p == nullptr) {
    return NOD_FST_STOP;
  }
  if (p->kind != HandleKind::Partition) {
    set_last_error("handle is not a partition");
    return NOD_FST_STOP;
  }
  FstView fst;
  if (!parse_fst(p->rawFst, fst)) {
    return NOD_FST_STOP;
  }
  std::string_view pathView{path};
  if (!is_valid_utf8(pathView)) {
    set_last_error("invalid UTF-8 in path");
    return NOD_FST_STOP;
  }
  const auto found = [&](size_t idx) {
    if (out_kind != nullptr) {
      *out_kind = fst.kind(idx);
    }
    if (out_length != nullptr) {
      *out_length = fst.length(idx);
    }
    return static_cast<uint32_t>(idx);
  };

  // Port of nod::disc::fst::Fst::find.
  while (!pathView.empty() && pathView.front() == '/') {
    pathView.remove_prefix(1);
  }
  while (!pathView.empty() && pathView.back() == '/') {
    pathView.remove_suffix(1);
  }
  PathSegments split{pathView};
  std::string_view current = split.next_non_empty();
  if (current.empty()) {
    return fst.count > 0 ? found(0) : NOD_FST_STOP;
  }
  size_t idx = 1;
  bool hasStop = false;
  size_t stopAt = 0;
  std::string name;
  while (idx < fst.count) {
    if (fst.name(idx, name) && ascii_iequals(name, current)) {
      current = split.next_non_empty();
      if (current.empty()) {
        return found(idx);
      }
      // Descend into directory
      idx += 1;
      hasStop = true;
      stopAt = size_t{fst.length(idx - 1)} + idx;
    } else if (fst.is_dir(idx)) {
      idx = fst.length(idx);
    } else {
      idx += 1;
    }
    if (hasStop && idx >= stopAt) {
      break;
    }
  }
  return NOD_FST_STOP;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data) {
  clear_last_error();
  const NodHandle* p = check_handle(partition);
  if (p == nullptr) {
    return;
  }
  if (p->kind != HandleKind::Partition) {
    set_last_error("handle is not a partition");
    return;
  }
  FstView fst;
  if (!parse_fst(p->rawFst, fst)) {
    return;
  }
  std::string name;
  size_t idx = 1; // skip root node
  while (idx < fst.count) {
    if (!fst.name(idx, name)) {
      name = "<invalid>";
    }
    idx = callback(static_cast<uint32_t>(idx), fst.kind(idx), name.c_str(), fst.length(idx), user_data);
  }
}

} // extern "C"
