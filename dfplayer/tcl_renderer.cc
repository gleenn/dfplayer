// Copyright 2014, Igor Chernyshev.
// Licensed under The MIT License
//
// TCL controller access module.

#include "tcl_renderer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <opencv2/opencv.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FRAME_DATA_LEN  (STRAND_LENGTH * 8 * 3)

static const int kMgsStartDelayUs = 500;
static const int kMgsDataDelayUs = 1500;
static const int kFrameSendDurationUs =
    kMgsStartDelayUs + kMgsDataDelayUs * (FRAME_DATA_LEN / 1024);

TclRenderer* TclRenderer::instance_ = new TclRenderer();

///////////////////////////////////////////////////////////////////////////////
// TclController
///////////////////////////////////////////////////////////////////////////////

struct LayoutMap {
  LayoutMap() {
    memset(lengths_, 0, sizeof(lengths_));
  }

  void AddCoord(int strand_id, int led_id, int x, int y) {
    coords_[strand_id][led_id].push_back(Coord(x, y));
  }

  void AddHdrSibling(int strand_id, int led_id, int strand_id2, int led_id2) {
    hdr_siblings_[strand_id][led_id].push_back(
        HdrSibling(strand_id2, led_id2));
  }

  struct HdrSibling {
    HdrSibling(int strand_id, int led_id)
        : strand_id_(strand_id), led_id_(led_id) {}

    int strand_id_;
    int led_id_;
  };

  std::vector<Coord> coords_[STRAND_COUNT][STRAND_LENGTH];
  std::vector<HdrSibling> hdr_siblings_[STRAND_COUNT][STRAND_LENGTH];
  int lengths_[STRAND_COUNT];
};

struct Strands {
  Strands() {
    for (int i = 0; i < STRAND_COUNT; i++) {
      colors[i] = new uint8_t[STRAND_LENGTH * 4];
      lengths[i] = 0;
    }
  }

  ~Strands() {
    for (int i = 0; i < STRAND_COUNT; i++) {
      delete[] colors[i];
    }
  }

  uint8_t* colors[STRAND_COUNT];
  int lengths[STRAND_COUNT];
  RgbaImage led_image;

 private:
  Strands(const Strands& src);
  Strands& operator=(const Strands& rhs);
};

class TclController {
 public:
  TclController(
      int id, int width, int height,
      const Layout& layout, double gamma);
  ~TclController();

  int GetId() const { return id_; }
  int GetWidth() const { return width_; }
  int GetHeight() const { return height_; }

  bool BuildImage(Bytes* bytes, int w, int h, EffectMode mode, RgbaImage* dst);

  Bytes* GetAndClearLastImage();
  Bytes* GetAndClearLastLedImage();
  int GetLastImageId() const { return last_image_id_; }

  void SetEffectImage(Bytes* bytes, int w, int h, EffectMode mode);

  void SetGammaRanges(
      int r_min, int r_max, double r_gamma,
      int g_min, int g_max, double g_gamma,
      int b_min, int b_max, double b_gamma);

  // Socket communication funtions are invoked from worker thread only.
  void UpdateAutoReset(uint64_t auto_reset_after_no_data_ms);
  void ScheduleReset();
  bool InitController();
  bool SendFrame(uint8_t* frame_data);

  uint8_t* BuildFrameDataForImage(RgbaImage* img, int id);

  void ApplyEffect(RgbaImage* image);
  Strands* ConvertImageToStrands(const RgbaImage& image);

  void SetHdrMode(HdrMode mode);

  static uint8_t* ConvertStrandsToFrame(Strands* strands);
  static int BuildFrameColorSeq(
      Strands* strands, int led_id, int color_component, uint8_t* dst);

 private:
  TclController(const TclController& src);
  TclController& operator=(const TclController& rhs);

  void PopulateLayoutMap(const Layout& layout);

  bool PopulateStrandsColors(
      Strands* strands, const RgbaImage& image);
  void SaveLedImageForStrands(Strands* strands);
  void ConvertStrandsHls(Strands* strands, bool to_hls);
  void PerformHdr(Strands* strands);
  void ApplyStrandsGamma(Strands* strands);

  bool Connect();
  void CloseSocket();
  bool SendPacket(const void* data, int size);
  void ConsumeReplyData();
  void SetLastReplyTime();

  int id_;
  int width_;
  int height_;
  bool init_sent_;
  RgbGamma gamma_;
  LayoutMap layout_;
  int socket_;
  bool require_reset_;
  uint64_t last_reply_time_;
  RgbaImage last_image_;
  RgbaImage last_led_image_;
  int last_image_id_;
  RgbaImage effect_image_;
  int frames_sent_after_reply_;
  HdrMode hdr_mode_;
};

TclController::TclController(
    int id, int width, int height,
    const Layout& layout, double gamma)
    : id_(id), width_(width), height_(height), init_sent_(false),
      socket_(-1), require_reset_(true), last_reply_time_(0),
      last_image_id_(0), frames_sent_after_reply_(0),
      hdr_mode_(HDR_NONE) {
  SetGammaRanges(0, 255, gamma, 0, 255, gamma, 0, 255, gamma);
  PopulateLayoutMap(layout);
}

TclController::~TclController() {
  CloseSocket();
}

void TclController::UpdateAutoReset(uint64_t auto_reset_after_no_data_ms) {
  if (auto_reset_after_no_data_ms <= 0 || require_reset_ ||
      frames_sent_after_reply_ <= 2) {
    return;
  }

  uint64_t reply_delay = GetCurrentMillis() - last_reply_time_;
  if (reply_delay > auto_reset_after_no_data_ms) {
    fprintf(stderr, "No reply in %lld ms and %d frames, RESETTING !!!\n",
            (long long) reply_delay, frames_sent_after_reply_);
    require_reset_ = true;
  }
}

void TclController::ScheduleReset() {
  require_reset_ = true;
}

Bytes* TclController::GetAndClearLastImage() {
  if (last_image_.IsEmpty())
    return NULL;
  Bytes* result = new Bytes(last_image_.GetData(), last_image_.GetDataLen());
  last_image_.Clear();
  return result;
}

Bytes* TclController::GetAndClearLastLedImage() {
  if (last_led_image_.IsEmpty())
    return NULL;
  Bytes* result = new Bytes(
      last_led_image_.GetData(), last_led_image_.GetDataLen());
  last_led_image_.Clear();
  return result;
}

void TclController::SetGammaRanges(
    int r_min, int r_max, double r_gamma,
    int g_min, int g_max, double g_gamma,
    int b_min, int b_max, double b_gamma) {
  gamma_.SetGammaRanges(
      r_min, r_max, r_gamma, g_min, g_max, g_gamma, b_min, b_max, b_gamma);
}

void TclController::SetHdrMode(HdrMode mode) {
  hdr_mode_ = mode;
}

struct PixelUsage {
  bool in_use;
  bool is_primary;
  int strand_id;
  int led_id;
};

static void AddCoord(
    LayoutMap* layout, PixelUsage* usage, int strand_id, int led_id,
    int x, int y, int w, int h) {
  if (x < 0 || x >= w || y < 0 || y >= h || usage[y * w + x].in_use)
    return;
  layout->AddCoord(strand_id, led_id, x, y);
  int pos = y * w + x;
  usage[pos].strand_id = strand_id;
  usage[pos].led_id = led_id;
  usage[pos].in_use = true;
  usage[pos].is_primary = true;
}

static void CopyCoord(
    LayoutMap* layout, PixelUsage* usage, int x, int y, int w, int h,
    int x_src, int y_src) {
  int pos_dst = y * w + x;
  if (x_src < 0 || x_src >= w || y_src < 0 || y_src >= h ||
      !usage[y_src * w + x_src].is_primary || usage[pos_dst].in_use) {
    return;
  }
  (void) layout;
  /*int pos_src = y_src * w + x_src;
  layout->AddCoord(usage[pos_src].strand_id, usage[pos_src].led_id, x, y);
  usage[pos_dst].strand_id = usage[pos_src].strand_id;
  usage[pos_dst].led_id = usage[pos_src].led_id;
  usage[pos_dst].in_use = true;*/
}

void TclController::PopulateLayoutMap(const Layout& layout) {
  PixelUsage* usage = new PixelUsage[width_ * height_];
  memset(usage, 0, width_ * height_ * sizeof(PixelUsage));
  for (int strand_id = 0; strand_id < STRAND_COUNT; ++strand_id) {
    int len = layout.lengths_[strand_id];
    layout_.lengths_[strand_id] = len;
    for (int led_id = 0; led_id < len; ++led_id) {
      int x = layout.x_[strand_id][led_id];
      int y = layout.y_[strand_id][led_id];
      AddCoord(&layout_, usage, strand_id, led_id, x, y, width_, height_);
      AddCoord(&layout_, usage, strand_id, led_id, x-1, y-1, width_, height_);
      AddCoord(&layout_, usage, strand_id, led_id, x-1, y,   width_, height_);
      AddCoord(&layout_, usage, strand_id, led_id, x-1, y+1, width_, height_);
      AddCoord(&layout_, usage, strand_id, led_id, x+1, y-1, width_, height_);
      AddCoord(&layout_, usage, strand_id, led_id, x+1, y,   width_, height_);
      AddCoord(&layout_, usage, strand_id, led_id, x+1, y+1, width_, height_);
      AddCoord(&layout_, usage, strand_id, led_id, x,   y-1, width_, height_);
      AddCoord(&layout_, usage, strand_id, led_id, x,   y+1, width_, height_);
    }
  }

  // TODO(igorc): Fill more of the pixel area.
  // Find matches for all points that were not filled. Do only one iteration
  // as the majority of the relevant pixels have already been mapped.
  for (int x = 0; x < width_; ++x) {
    for (int y = 0; y < height_; ++y) {
      CopyCoord(&layout_, usage, x, y, width_, height_, x-1, y-1);
      CopyCoord(&layout_, usage, x, y, width_, height_, x-1, y  );
      CopyCoord(&layout_, usage, x, y, width_, height_, x-1, y+1);
      CopyCoord(&layout_, usage, x, y, width_, height_, x+1, y-1);
      CopyCoord(&layout_, usage, x, y, width_, height_, x+1, y  );
      CopyCoord(&layout_, usage, x, y, width_, height_, x+1, y+1);
      CopyCoord(&layout_, usage, x, y, width_, height_, x,   y-1);
      CopyCoord(&layout_, usage, x, y, width_, height_, x,   y+1);
    }
  }

  // Find HDR siblings.
  // TODO(igorc): Compute max distance instead of hard-coding.
  static const int kHdrSiblingsDistance = 13;
  int max_distance2 = kHdrSiblingsDistance * kHdrSiblingsDistance;
  for (int strand_id1 = 0; strand_id1 < STRAND_COUNT; ++strand_id1) {
    for (int led_id1 = 0; led_id1 < layout.lengths_[strand_id1]; ++led_id1) {
      int x1 = layout.x_[strand_id1][led_id1];
      int y1 = layout.y_[strand_id1][led_id1];
      for (int strand_id2 = 0; strand_id2 < STRAND_COUNT; ++strand_id2) {
        for (int led_id2 = 0; led_id2 < layout.lengths_[strand_id2]; ++led_id2) {
          int x_d = layout.x_[strand_id2][led_id2] - x1;
          int y_d = layout.y_[strand_id2][led_id2] - y1;
          int distance2 = x_d * x_d + y_d * y_d;
          if (distance2 < max_distance2)
            layout_.AddHdrSibling(strand_id1, led_id1, strand_id2, led_id2);
        }
      }
    }
  }

  /*for (int strand_id  = 0; strand_id < STRAND_COUNT; ++strand_id) {
    for (int led_id = 0; led_id < layout_.lengths_[strand_id]; ++led_id) {
      fprintf(stderr, "HDR siblings: strand=%d, led=%d, count=%ld\n",
              strand_id, led_id,
              layout_.hdr_siblings_[strand_id][led_id].size());
    }
  }*/
}

bool TclController::BuildImage(
    Bytes* bytes, int w, int h, EffectMode mode, RgbaImage* dst) {
  if (!bytes || !bytes->GetLen())
    return false;

  // We expect all incoming images to use linearized RGB gamma.
  uint8_t* img_data;
  if (mode == EFFECT_OVERLAY) {
    img_data = ResizeImage(bytes->GetData(), w, h, width_, height_);
  } else if (mode == EFFECT_DUPLICATE) {
    img_data = new uint8_t[RGBA_LEN(width_, height_)];
    uint8_t* img1 = ResizeImage(bytes->GetData(), w, h, width_ / 2, height_);
    PasteSubImage(img1, width_ / 2, height_,
        img_data, 0, 0, width_, height_, false);
    PasteSubImage(img1, width_ / 2, height_,
        img_data, width_ / 2, 0, width_, height_, false);
    delete[] img1;
  } else {  // EFFECT_MIRROR
    img_data = new uint8_t[RGBA_LEN(width_, height_)];
    uint8_t* img1 = ResizeImage(bytes->GetData(), w, h, width_ / 2, height_);
    uint8_t* img2 = FlipImage(img1, width_ / 2, height_, true);
    PasteSubImage(img1, width_ / 2, height_,
        img_data, 0, 0, width_, height_, false);
    PasteSubImage(img2, width_ / 2, height_,
        img_data, width_ / 2, 0, width_, height_, false);
    delete[] img1;
    delete[] img2;
  }

  dst->Set(img_data, width_, height_);
  delete[] img_data;
  return true;
}

void TclController::SetEffectImage(
    Bytes* bytes, int w, int h, EffectMode mode) {
  effect_image_.Clear();
  BuildImage(bytes, w, h, mode, &effect_image_);
}

void TclController::ApplyEffect(RgbaImage* image) {
  if (effect_image_.IsEmpty())
    return;

  PasteSubImage(effect_image_.GetData(), width_, height_,
      image->GetData(), 0, 0, width_, height_, true);
}

uint8_t* TclController::BuildFrameDataForImage(RgbaImage* img, int id) {
  ApplyEffect(img);
  Strands* strands = ConvertImageToStrands(*img);
  if (!strands)
    return NULL;

  uint8_t* frame_data = ConvertStrandsToFrame(strands);
  last_image_ = *img;
  last_image_id_ = id;
  last_led_image_ = strands->led_image;
  delete strands;
  return frame_data;
}

Strands* TclController::ConvertImageToStrands(
    const RgbaImage& image) {
  Strands* strands = new Strands();
  if (!PopulateStrandsColors(strands, image)) {
    delete strands;
    return NULL;
  }

  ConvertStrandsHls(strands, true);
  PerformHdr(strands);

  // TODO(igorc): Adjust S and L through a user-controlled gamma curve.

  // TODO(igorc): Fill the darkness.

  ConvertStrandsHls(strands, false);

  // Apply gamma at the end to preserve linear RGB-HSL conversions.
  ApplyStrandsGamma(strands);

  SaveLedImageForStrands(strands);
  return strands;
}

bool TclController::PopulateStrandsColors(
    Strands* strands, const RgbaImage& image) {
  const uint8_t* image_data = image.GetData();
  int image_len = RGBA_LEN(width_, height_);
  for (int strand_id  = 0; strand_id < STRAND_COUNT; ++strand_id) {
    int strand_len = layout_.lengths_[strand_id];
    uint8_t* dst_colors = strands->colors[strand_id];
    for (int led_id = 0; led_id < strand_len; ++led_id) {
      uint32_t r = 0, g = 0, b = 0;
      const std::vector<Coord>& coords = layout_.coords_[strand_id][led_id];
      int coord_count = coords.size();
      for (int c_id = 0; c_id < coord_count; ++c_id) {
        int x = coords[c_id].x_;
        int y = coords[c_id].y_;
        int color_idx = (y * width_ + x) * 4;
        if (color_idx < 0 || (color_idx + 4) > image_len) {
          fprintf(stderr,
                  "Not enough data in image. Accessing %d, len=%d, strand=%d, "
                  "led=%d, x=%d, y=%d\n",
                  color_idx, image_len, strand_id, led_id, x, y);
          return false;
        }
        r += image_data[color_idx];
        g += image_data[color_idx + 1];
        b += image_data[color_idx + 2];
      }

      r = (r / coord_count) & 0xFF;
      g = (g / coord_count) & 0xFF;
      b = (b / coord_count) & 0xFF;
      uint32_t color = PACK_COLOR32(r, g, b, 255);

      int dst_idx = led_id * 4;
      *((uint32_t*) (dst_colors + dst_idx)) = color;
    }
    strands->lengths[strand_id] = strand_len;
  }
  return true;
}

void TclController::ApplyStrandsGamma(Strands* strands) {
  for (int strand_id  = 0; strand_id < STRAND_COUNT; ++strand_id) {
    int strand_len = layout_.lengths_[strand_id];
    uint8_t* colors = strands->colors[strand_id];
    for (int led_id = 0; led_id < strand_len; ++led_id) {
      uint32_t* color = (uint32_t*) (colors + led_id * 4);
      *color = gamma_.Apply(*color);
    }
  }
}

void TclController::SaveLedImageForStrands(Strands* strands) {
  uint8_t* led_image_data = new uint8_t[RGBA_LEN(width_, height_)];
  memset(led_image_data, 0, RGBA_LEN(width_, height_));
  for (int strand_id  = 0; strand_id < STRAND_COUNT; ++strand_id) {
    int strand_len = layout_.lengths_[strand_id];
    uint8_t* colors = strands->colors[strand_id];
    for (int led_id = 0; led_id < strand_len; ++led_id) {
      const std::vector<Coord>& coords = layout_.coords_[strand_id][led_id];
      uint32_t color = *((uint32_t*) (colors + led_id * 4));
      for (size_t c_id = 0; c_id < coords.size(); ++c_id) {
        int x = coords[c_id].x_;
        int y = coords[c_id].y_;
        int color_idx = (y * width_ + x) * 4;
        *((uint32_t*) (led_image_data + color_idx)) = color;
      }
    }
  }
  strands->led_image.Set(led_image_data, width_, height_);
  delete[] led_image_data;
}

void TclController::ConvertStrandsHls(Strands* strands, bool to_hls) {
  uint32_t tmp = 0;
  cv::Mat hls(1, 1, CV_8UC3, &tmp);
  cv::Mat rgb(1, 1, CV_8UC3, &tmp);
  for (int strand_id  = 0; strand_id < STRAND_COUNT; ++strand_id) {
    int strand_len = layout_.lengths_[strand_id];
    uint8_t* colors = strands->colors[strand_id];
    for (int led_id = 0; led_id < strand_len; ++led_id) {
      uint8_t* color_ptr = colors + led_id * 4;
      if (to_hls) {
        memcpy(rgb.data, color_ptr, 3);
        cv::cvtColor(rgb, hls, CV_RGB2HLS);
        memcpy(color_ptr, hls.data, 3);
      } else {
        memcpy(hls.data, color_ptr, 3);
        cv::cvtColor(hls, rgb, CV_HLS2RGB);
        memcpy(color_ptr, rgb.data, 3);
      }
    }
  }
}

// Extrapolates value to a range from 0 to 255.
#define EXTEND256(value, min, max)   \
  ((max) == (min) ? (max) :          \
      ((255 * ((value) - (min)) / ((max) - (min))) & 0xFF))

void TclController::PerformHdr(Strands* strands) {
  if (hdr_mode_ == HDR_NONE)
    return;

  uint8_t* res_colors[STRAND_COUNT];
  for (int i = 0; i < STRAND_COUNT; ++i)
    res_colors[i] = new uint8_t[STRAND_LENGTH * 4];

  // Populate resulting colors with HDR image.
  for (int strand_id  = 0; strand_id < STRAND_COUNT; ++strand_id) {
    int strand_len = layout_.lengths_[strand_id];
    for (int led_id = 0; led_id < strand_len; ++led_id) {
      uint32_t l_min = 255, l_max = 0, s_min = 255, s_max = 0;
      const std::vector<LayoutMap::HdrSibling>& siblings =
          layout_.hdr_siblings_[strand_id][led_id];
      uint8_t* src_color = strands->colors[strand_id] + led_id * 4;
      uint8_t* res_color = res_colors[strand_id] + led_id * 4;
      for (size_t i = 0; i < siblings.size(); ++i) {
        uint8_t* hls = strands->colors[siblings[i].strand_id_] +
            siblings[i].led_id_ * 4;
        uint8_t l = hls[1];
        uint8_t s = hls[2];
        if (l < l_min) {
          l_min = l;
        }
        if (l > l_max) {
          l_max = l;
        }
        if (s < s_min) {
          s_min = s;
        }
        if (s > s_max) {
          s_max = s;
        }
      }
      // Always preserve the hue and alpha.
      res_color[0] = src_color[0];
      if (hdr_mode_ == HDR_LSAT || hdr_mode_ == HDR_LUMINANCE) {
        res_color[1] = EXTEND256(src_color[1], l_min, l_max);
      } else {
        res_color[1] = src_color[1];
      }
      if (hdr_mode_ == HDR_LSAT || hdr_mode_ == HDR_SATURATION) {
        res_color[2] = EXTEND256(src_color[2], s_min, s_max);
      } else {
        res_color[2] = src_color[2];
      }
      res_color[3] = src_color[3];
    }
  }

  // Copy resulting colors back to the strand.
  for (int strand_id  = 0; strand_id < STRAND_COUNT; ++strand_id) {
    int strand_len = layout_.lengths_[strand_id];
    memcpy(strands->colors[strand_id], res_colors[strand_id],
           strand_len * 4);
  }

  for (int i = 0; i < STRAND_COUNT; ++i)
    delete[] res_colors[i];
}

// static
uint8_t* TclController::ConvertStrandsToFrame(Strands* strands) {
  uint8_t* result = new uint8_t[FRAME_DATA_LEN];
  int pos = 0;
  for (int led_id = 0; led_id < STRAND_LENGTH; led_id++) {
    pos += BuildFrameColorSeq(strands, led_id, 2, result + pos);
    pos += BuildFrameColorSeq(strands, led_id, 1, result + pos);
    pos += BuildFrameColorSeq(strands, led_id, 0, result + pos);
  }
  CHECK(pos == FRAME_DATA_LEN);
  for (int i = 0; i < FRAME_DATA_LEN; i++) {
    // Black color is offset by 0x2C.
    result[i] = (result[i] + 0x2C) & 0xFF;
  }
  return result;
}

// static
int TclController::BuildFrameColorSeq(
    Strands* strands, int led_id,
    int color_component, uint8_t* dst) {
  int pos = 0;
  int color_bit_mask = 0x80;
  while (color_bit_mask > 0) {
    uint8_t dst_byte = 0;
    for (int strand_id  = 0; strand_id < STRAND_COUNT; strand_id++) {
      if (led_id >= strands->lengths[strand_id])
        continue;
      uint8_t* colors = strands->colors[strand_id];
      uint8_t color = colors[led_id * 4 + color_component];
      if ((color & color_bit_mask) != 0)
        dst_byte |= 1 << strand_id;
    }
    color_bit_mask >>= 1;
    dst[pos++] = dst_byte;
  }
  CHECK(pos == 8);
  return pos;
}

void TclController::CloseSocket() {
  if (socket_ != -1) {
    close(socket_);
    socket_ = -1;
  }
}

bool TclController::Connect() {
  if (socket_ != -1)
    return true;

  socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_ == -1) {
    REPORT_ERRNO("socket");
    return false;
  }

  /*if (fcntl(socket_, F_SETFL, O_NONBLOCK, 1) == -1) {
    REPORT_ERRNO("fcntl");
    CloseSocket();
    return false;
  }*/

  struct sockaddr_in si_local;
  memset(&si_local, 0, sizeof(si_local));
  si_local.sin_family = AF_INET;
  si_local.sin_port = htons(0);
  si_local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(socket_, (struct sockaddr*) &si_local, sizeof(si_local)) == -1) {
    REPORT_ERRNO("bind");
    CloseSocket();
    return false;
  }

  char addr_str[65];
  snprintf(addr_str, sizeof(addr_str), "192.168.60.%d", (49 + id_));

  struct sockaddr_in si_remote;
  memset(&si_remote, 0, sizeof(si_remote));
  si_remote.sin_family = AF_INET;
  si_remote.sin_port = htons(5000);
  if (inet_aton(addr_str, &si_remote.sin_addr) == 0) {
    fprintf(stderr, "inet_aton() failed\n");
    CloseSocket();
    return false;
  }

  if (TEMP_FAILURE_RETRY(connect(
          socket_, (struct sockaddr*) &si_remote, sizeof(si_remote))) == -1) {
    REPORT_ERRNO("connect");
    CloseSocket();
    return false;
  }

  return true;
}

bool TclController::SendPacket(const void* data, int size) {
  int sent = TEMP_FAILURE_RETRY(send(socket_, data, size, 0));
  if (sent == -1) {
    REPORT_ERRNO("send");
    return false;
  }
  if (sent != size) {
    fprintf(stderr, "Not all data was sent in UDP packet\n");
    require_reset_ = true;
    return false;
  }
  return true;
}

bool TclController::InitController() {
  static const uint8_t MSG_INIT[] = {0xC5, 0x77, 0x88, 0x00, 0x00};
  static const double MSG_INIT_DELAY = 0.1;
  static const uint8_t MSG_RESET[] = {0xC2, 0x77, 0x88, 0x00, 0x00};
  static const double MSG_RESET_DELAY = 5;

  if (!Connect())
    return false;
  if (init_sent_ && !require_reset_)
    return true;

  if (require_reset_) {
    if (init_sent_)
      fprintf(stderr, "Performing a requested reset\n");
    if (!SendPacket(MSG_RESET, sizeof(MSG_RESET)))
      return false;
    require_reset_ = false;
    Sleep(MSG_RESET_DELAY);
  }

  if (!SendPacket(MSG_INIT, sizeof(MSG_INIT)))
    return false;
  Sleep(MSG_INIT_DELAY);

  init_sent_ = true;
  SetLastReplyTime();
  return true;
}

bool TclController::SendFrame(uint8_t* frame_data) {
  static const uint8_t MSG_START_FRAME[] = {0xC5, 0x77, 0x88, 0x00, 0x00};
  static const uint8_t MSG_END_FRAME[] = {0xAA, 0x01, 0x8C, 0x01, 0x55};
  static const uint8_t FRAME_MSG_PREFIX[] = {
      0x88, 0x00, 0x68, 0x3F, 0x2B, 0xFD,
      0x60, 0x8B, 0x95, 0xEF, 0x04, 0x69};
  static const uint8_t FRAME_MSG_SUFFIX[] = {0x00, 0x00, 0x00, 0x00};

  ConsumeReplyData();
  if (!SendPacket(MSG_START_FRAME, sizeof(MSG_START_FRAME)))
    return false;
  Sleep(((double) kMgsStartDelayUs) / 1000000.0);

  uint8_t packet[sizeof(FRAME_MSG_PREFIX) + 1024 + sizeof(FRAME_MSG_SUFFIX)];
  memcpy(packet, FRAME_MSG_PREFIX, sizeof(FRAME_MSG_PREFIX));
  memcpy(packet + sizeof(FRAME_MSG_PREFIX) + 1024,
         FRAME_MSG_SUFFIX, sizeof(FRAME_MSG_SUFFIX));

  int message_idx = 0;
  int frame_data_pos = 0;
  while (frame_data_pos < FRAME_DATA_LEN) {
    packet[1] = message_idx++;
    memcpy(packet + sizeof(FRAME_MSG_PREFIX),
           frame_data + frame_data_pos, 1024);
    frame_data_pos += 1024;

    if (!SendPacket(packet, sizeof(packet)))
      return false;
    Sleep(((double) kMgsDataDelayUs) / 1000000.0);
  }
  CHECK(frame_data_pos == FRAME_DATA_LEN);
  CHECK(message_idx == 12);

  if (!SendPacket(MSG_END_FRAME, sizeof(MSG_END_FRAME)))
    return false;
  ConsumeReplyData();
  frames_sent_after_reply_++;
  return true;
}

void TclController::ConsumeReplyData() {
  // static const uint8_t MSG_REPLY[] = {0x55, 0x00, 0x00, 0x00, 0x00};
  uint8_t buf[65536];
  while (true) {
    ssize_t size = TEMP_FAILURE_RETRY(
        recv(socket_, buf, sizeof(buf), MSG_DONTWAIT));
    if (size != -1) {
      SetLastReplyTime();
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      REPORT_ERRNO("recv");
    break;
  }
}

void TclController::SetLastReplyTime() {
  last_reply_time_ = GetCurrentMillis();
  frames_sent_after_reply_ = 0;
}

///////////////////////////////////////////////////////////////////////////////
// TclRenderer
///////////////////////////////////////////////////////////////////////////////

TclRenderer::TclRenderer()
    : fps_(15), auto_reset_after_no_data_ms_(5000),
      is_shutting_down_(false), has_started_thread_(false),
      enable_net_(false), controllers_locked_(false),
      lock_(PTHREAD_MUTEX_INITIALIZER),
      cond_(PTHREAD_COND_INITIALIZER) {
  base_time_ = GetCurrentMillis();
}

TclRenderer::~TclRenderer() {
  if (has_started_thread_) {
    {
      Autolock l(lock_);
      is_shutting_down_ = true;
      pthread_cond_broadcast(&cond_);
    }

    pthread_join(thread_, NULL);
  }

  ResetImageQueue();

  for (std::vector<TclController*>::iterator it = controllers_.begin();
        it != controllers_.end(); ++it) {
    delete (*it);
  }

  pthread_cond_destroy(&cond_);
  pthread_mutex_destroy(&lock_);
}

void TclRenderer::AddController(
    int id, int width, int height,
    const Layout& layout, double gamma) {
  Autolock l(lock_);
  CHECK(!controllers_locked_);
  CHECK(FindControllerLocked(id) == NULL);
  TclController* controller = new TclController(
      id, width, height, layout, gamma);
  controllers_.push_back(controller);
}

TclController* TclRenderer::FindControllerLocked(int id) {
  for (std::vector<TclController*>::iterator it = controllers_.begin();
        it != controllers_.end(); ++it) {
    if ((*it)->GetId() == id)
      return *it;
  }
  return NULL;
}

void TclRenderer::LockControllers() {
  Autolock l(lock_);
  controllers_locked_ = true;
}

void TclRenderer::StartMessageLoop(int fps, bool enable_net) {
  Autolock l(lock_);
  CHECK(controllers_locked_);
  if (has_started_thread_)
    return;
  fps_ = fps;
  enable_net_ = enable_net;
  has_started_thread_ = true;
  int err = pthread_create(&thread_, NULL, &ThreadEntry, this);
  if (err != 0) {
    fprintf(stderr, "pthread_create failed with %d\n", err);
    CHECK(false);
  }
}

void TclRenderer::SetGamma(double gamma) {
  // 1.0 is uncorrected gamma, which is perceived as "too bright"
  // in the middle. 2.4 is a good starting point. Changing this value
  // affects mid-range pixels - higher values produce dimmer pixels.
  SetGammaRanges(0, 255, gamma, 0, 255, gamma, 0, 255, gamma);
}

void TclRenderer::SetGammaRanges(
    int r_min, int r_max, double r_gamma,
    int g_min, int g_max, double g_gamma,
    int b_min, int b_max, double b_gamma) {
  Autolock l(lock_);
  CHECK(controllers_locked_);
  for (std::vector<TclController*>::iterator it = controllers_.begin();
        it != controllers_.end(); ++it) {
    (*it)->SetGammaRanges(
        r_min, r_max, r_gamma, g_min, g_max, g_gamma, b_min, b_max, b_gamma);
  }
}

void TclRenderer::SetHdrMode(HdrMode mode) {
  Autolock l(lock_);
  CHECK(controllers_locked_);
  for (std::vector<TclController*>::iterator it = controllers_.begin();
        it != controllers_.end(); ++it) {
    (*it)->SetHdrMode(mode);
  }
}

void TclRenderer::SetAutoResetAfterNoDataMs(int value) {
  Autolock l(lock_);
  auto_reset_after_no_data_ms_ = value;
}

std::vector<int> TclRenderer::GetAndClearFrameDelays() {
  Autolock l(lock_);
  std::vector<int> result = frame_delays_;
  frame_delays_.clear();
  return result;
}

int TclRenderer::GetFrameSendDuration() {
  return kFrameSendDurationUs / 1000;
}

Bytes* TclRenderer::GetAndClearLastImage(int controller_id) {
  Autolock l(lock_);
  TclController* controller = FindControllerLocked(controller_id);
  return (controller ? controller->GetAndClearLastImage() : NULL);
}

Bytes* TclRenderer::GetAndClearLastLedImage(int controller_id) {
  Autolock l(lock_);
  TclController* controller = FindControllerLocked(controller_id);
  return (controller ? controller->GetAndClearLastLedImage() : NULL);
}

int TclRenderer::GetLastImageId(int controller_id) {
  Autolock l(lock_);
  TclController* controller = FindControllerLocked(controller_id);
  return (controller ? controller->GetLastImageId() : -1);
}

void TclRenderer::ScheduleImageAt(
    int controller_id, Bytes* bytes, int w, int h, EffectMode mode,
    int crop_x, int crop_y, int crop_w, int crop_h,
    int id, const AdjustableTime& time) {
  Autolock l(lock_);
  CHECK(has_started_thread_);
  if (is_shutting_down_)
    return;
  TclController* controller = FindControllerLocked(controller_id);
  if (!controller) {
    fprintf(stderr, "Ignoring TclRenderer::ScheduleImageAt on %d\n", controller_id);
    return;
  }
  if (bytes->GetLen() != RGBA_LEN(w, h)) {
    fprintf(stderr, "Unexpected image size in TCL renderer: %d\n",
            bytes->GetLen());
    return;
  }

  uint64_t time_abs = time.time_;
  if (time_abs > base_time_) {
    // Align with FPS.
    double frame_num = round(((double) (time_abs - base_time_)) / 1000.0 * fps_);
    time_abs = base_time_ + (uint64_t) (frame_num * 1000.0 / fps_);
    //fprintf(stderr, "Changed target time from %ld to %ld, f=%.2f\n",
    //        time.time_, time_abs, frame_num);
  }

  // TODO(igorc): Support crop_x.
  (void) crop_x;
  (void) crop_y;
  (void) crop_w;
  (void) crop_h;

  RgbaImage image;
  controller->BuildImage(bytes, w, h, mode, &image);
  queue_.push(WorkItem(false, controller, image, id, time_abs));

  int err = pthread_cond_broadcast(&cond_);
  if (err != 0) {
    fprintf(stderr, "Unable to signal condition: %d\n", err);
    CHECK(false);
  }
}

void TclRenderer::SetEffectImage(
    int controller_id, Bytes* bytes, int w, int h, EffectMode mode) {
  Autolock l(lock_);
  TclController* controller = FindControllerLocked(controller_id);
  if (controller)
    controller->SetEffectImage(bytes, w, h, mode);
}

void TclRenderer::ResetImageQueue() {
  Autolock l(lock_);
  while (!queue_.empty()) {
    WorkItem item = queue_.top();
    queue_.pop();
  }
}

std::vector<int> TclRenderer::GetFrameDataForTest(
    int controller_id, Bytes* bytes, int w, int h) {
  Autolock l(lock_);
  std::vector<int> result;
  TclController* controller = FindControllerLocked(controller_id);
  if (!bytes || !bytes->GetData() || !controller)
    return result;
  RgbaImage img(bytes->GetData(), w, h);
  Strands* strands = controller->ConvertImageToStrands(img);
  if (!strands)
    return result;
  uint8_t* frame_data = controller->ConvertStrandsToFrame(strands);
  for (int i = 0; i < FRAME_DATA_LEN; i++) {
    result.push_back(frame_data[i]);
  }
  delete[] frame_data;
  delete strands;
  return result;
}

int TclRenderer::GetQueueSize() {
  Autolock l(lock_);
  return queue_.size();
}

// static
void* TclRenderer::ThreadEntry(void* arg) {
  TclRenderer* self = reinterpret_cast<TclRenderer*>(arg);
  self->Run();
  return NULL;
}

void TclRenderer::Run() {
  while (true) {
    {
      Autolock l(lock_);
      if (is_shutting_down_)
        break;

      if (enable_net_) {
        for (std::vector<TclController*>::iterator it = controllers_.begin();
              it != controllers_.end(); ++it) {
          (*it)->UpdateAutoReset(auto_reset_after_no_data_ms_);
        }
      }
    }

    if (enable_net_) {
      bool failed_init = false;
      for (std::vector<TclController*>::iterator it = controllers_.begin();
            it != controllers_.end(); ++it) {
        failed_init |= !(*it)->InitController();
      }
      if (failed_init) {
        Sleep(1);
        continue;
      }
    }

    uint8_t* frame_data = NULL;
    WorkItem item(false, NULL, RgbaImage(), 0, 0);
    {
      Autolock l(lock_);
      while (!is_shutting_down_) {
        int64_t next_time;
        if (PopNextWorkItemLocked(&item, &next_time))
          break;
        WaitForQueueLocked(next_time);
      }
      if (is_shutting_down_)
        break;

      if (item.needs_reset_) {
        item.controller_->ScheduleReset();
        continue;
      }

      if (!item.img_.IsEmpty())
        frame_data = item.controller_->BuildFrameDataForImage(
            &item.img_, item.id_);
    }

    if (frame_data) {
      if (!enable_net_ || item.controller_->SendFrame(frame_data)) {
        Autolock l(lock_);
        frame_delays_.push_back(GetCurrentMillis() - item.time_);
        // fprintf(stderr, "Sent frame for %ld at %ld\n", item.time_, GetCurrentMillis());
      } else {
        fprintf(stderr, "Scheduling reset after failed frame\n");
        item.controller_->ScheduleReset();
      }
      delete[] frame_data;
    }
  }
}

bool TclRenderer::PopNextWorkItemLocked(
    TclRenderer::WorkItem* item, int64_t* next_time) {
  if (queue_.empty()) {
    *next_time = 0;
    return false;
  }
  uint64_t cur_time = GetCurrentMillis();
  while (true) {
    *item = queue_.top();
    if (item->needs_reset_) {
      // Ready to reset, and no future item time is known.
      *next_time = 0;
      while (!queue_.empty())
        queue_.pop();
      return true;
    }
    if (item->time_ > cur_time) {
      // No current item, report the time of the future item.
      *next_time = item->time_;
      return false;
    }
    queue_.pop();
    if (queue_.empty()) {
      // Return current item, and report no future time.
      *next_time = 0;
      return true;
    }
    if (queue_.top().time_ > cur_time) {
      // Return current item, and report future item's time.
      *next_time = queue_.top().time_;
      return true;
    }
    // The queue contains another item that is closer to current time.
    // Skip to that item.
  }
}

void TclRenderer::WaitForQueueLocked(int64_t next_time) {
  int err;
  if (next_time) {
    struct timespec timeout;
    if (clock_gettime(CLOCK_REALTIME, &timeout) == -1) {
      REPORT_ERRNO("clock_gettime(realtime)");
      CHECK(false);
    }
    AddTimeMillis(&timeout, next_time - GetCurrentMillis());
    err = pthread_cond_timedwait(&cond_, &lock_, &timeout);
  } else {
    err = pthread_cond_wait(&cond_, &lock_);
  }
  if (err != 0 && err != ETIMEDOUT) {
    fprintf(stderr, "Unable to wait on condition: %d\n", err);
    CHECK(false);
  }
}

///////////////////////////////////////////////////////////////////////////////
// Misc classes
///////////////////////////////////////////////////////////////////////////////

AdjustableTime::AdjustableTime() : time_(GetCurrentMillis()) {}

void AdjustableTime::AddMillis(int ms) {
  time_ += ms;
}

Layout::Layout() {
  memset(lengths_, 0, sizeof(lengths_));
}

void Layout::AddCoord(int strand_id, int x, int y) {
  CHECK(strand_id >= 0 && strand_id < STRAND_COUNT);
  if (lengths_[strand_id] == STRAND_LENGTH) {
    fprintf(stderr, "Cannot add more coords to strand %d\n", strand_id);
    return;
  }
  int pos = lengths_[strand_id];
  lengths_[strand_id] = pos + 1;
  x_[strand_id][pos] = x;
  y_[strand_id][pos] = y;
}

