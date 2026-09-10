#include "PanelRefreshCoordinator.h"

#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstdlib>
#include <cstring>

namespace {
constexpr const char* TAG = "GFX";
}

PanelRefreshCoordinator::~PanelRefreshCoordinator() {
  if (shadow_) {
    free(shadow_);
    shadow_ = nullptr;
  }
  if (mutex_) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
}

void PanelRefreshCoordinator::begin(const uint32_t bufferSize, const bool enabled) {
  if (!mutex_) {
    mutex_ = xSemaphoreCreateRecursiveMutex();
  }
  Guard g(*this);
  enabled_ = enabled;
  shadowValid_ = false;
  grayOnGlass_ = false;
  firstPaint_ = true;
  fastSinceClean_ = 0;
  cleansSinceFull_ = 0;
  if (!enabled_) {
    LOG_DBG(TAG, "refresh coordinator off (not WS397): pass-through");
    return;
  }
  if (shadow_ && size_ != bufferSize) {
    free(shadow_);
    shadow_ = nullptr;
  }
  size_ = bufferSize;
  const char* where = "psram";
  if (!shadow_ && size_ > 0) {
    // The framebuffer itself already lives in PSRAM (malloc >= 4 KB goes to
    // SPIRAM on this build); ask for it explicitly so a fragmented internal
    // heap can never end up holding a 48 KB shadow.
    shadow_ = static_cast<uint8_t*>(heap_caps_malloc(size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!shadow_) {
      shadow_ = static_cast<uint8_t*>(malloc(size_));
      where = "heap";
    }
  }
  if (!shadow_) {
    LOG_ERR(TAG, "refresh coordinator: no memory for the %lu B shadow; identical-frame skip disabled",
            static_cast<unsigned long>(size_));
    where = "none";
  }
  LOG_INF(TAG, "refresh coordinator on: shadow %lu B (%s), fastBeforeClean=%d pageTurnCeiling=%d cleansBeforeFull=%d",
          static_cast<unsigned long>(size_), where, FAST_BEFORE_CLEAN, PAGE_TURN_CEILING, CLEANS_BEFORE_FULL);
}

void PanelRefreshCoordinator::lock() const {
  if (mutex_) xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
}

void PanelRefreshCoordinator::unlock() const {
  if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

const char* PanelRefreshCoordinator::modeName(const HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "FULL";
    case HalDisplay::HALF_REFRESH:
      return "HALF";
    case HalDisplay::FAST_REFRESH:
      return "FAST";
  }
  return "?";
}

const char* PanelRefreshCoordinator::hintName(const Hint hint) { return hint == Hint::PageTurn ? "page" : "ui"; }

bool PanelRefreshCoordinator::shadowMatches(const uint8_t* fb, const bool inverted) const {
  if (!shadow_ || !shadowValid_ || !fb) return false;
  if (shadowInverted_ != inverted) return false;  // same bytes, other polarity: the panel must repaint
  return memcmp(fb, shadow_, size_) == 0;
}

PanelRefreshCoordinator::Plan PanelRefreshCoordinator::plan(const uint8_t* fb, const HalDisplay::RefreshMode requested,
                                                            const Hint hint, const bool inverted, const bool async,
                                                            const bool allowSkip) {
  Plan p{requested, false};
  if (!enabled_) return p;

  HalDisplay::RefreshMode mode = requested;
  if (mode == HalDisplay::FAST_REFRESH) {
    if (firstPaint_) {
      // First paint after begin(): the controller's differential baseline was
      // just destroyed by the init sequence, so it has to be an absolute clean.
      mode = HalDisplay::HALF_REFRESH;
    } else if (hint == Hint::Ui && grayOnGlass_) {
      mode = HalDisplay::HALF_REFRESH;
    } else if (hint == Hint::Ui && fastSinceClean_ >= FAST_BEFORE_CLEAN) {
      mode = HalDisplay::HALF_REFRESH;
    } else if (hint == Hint::PageTurn && fastSinceClean_ >= PAGE_TURN_CEILING) {
      mode = HalDisplay::HALF_REFRESH;
    }
  }
  if (mode == HalDisplay::HALF_REFRESH && CLEANS_BEFORE_FULL > 0 && cleansSinceFull_ + 1 >= CLEANS_BEFORE_FULL) {
    mode = HalDisplay::FULL_REFRESH;
  }

  p.mode = mode;
  // Only a plain UI FAST may be skipped: HALF/FULL were asked for (or promoted)
  // to clean the glass, async refreshes are followed by gray planes written on
  // top of them, and page turns keep the reader's own bookkeeping honest.
  p.skip = mode == HalDisplay::FAST_REFRESH && hint == Hint::Ui && !async && allowSkip && shadowMatches(fb, inverted);
  return p;
}

void PanelRefreshCoordinator::commit(const uint8_t* fb, const HalDisplay::RefreshMode effective, const Hint hint,
                                     const bool inverted, const bool async) {
  if (!enabled_) return;
  firstPaint_ = false;
  ++commits_;
  switch (effective) {
    case HalDisplay::FAST_REFRESH:
      ++fastSinceClean_;
      break;
    case HalDisplay::HALF_REFRESH:
      fastSinceClean_ = 0;
      ++cleansSinceFull_;
      grayOnGlass_ = false;
      break;
    case HalDisplay::FULL_REFRESH:
      fastSinceClean_ = 0;
      cleansSinceFull_ = 0;
      grayOnGlass_ = false;
      break;
  }
  if (async || !shadow_ || !fb) {
    // Async: the frame is a base for gray planes and the caller resyncs us
    // through cleanupGrayscaleWithFrameBuffer() once the planes are in.
    shadowValid_ = false;
  } else {
    memcpy(shadow_, fb, size_);
    shadowValid_ = true;
    shadowInverted_ = inverted;
  }
  LOG_DBG(TAG, "refresh %s hint=%s%s fast=%d cleans=%d gray=%d skipped=%lu n=%lu", modeName(effective),
          hintName(hint), async ? " async" : "", fastSinceClean_, cleansSinceFull_, grayOnGlass_ ? 1 : 0,
          static_cast<unsigned long>(skipped_), static_cast<unsigned long>(commits_));
}

void PanelRefreshCoordinator::commitSkip(const HalDisplay::RefreshMode requested, const Hint hint) {
  if (!enabled_) return;
  ++skipped_;
  LOG_DBG(TAG, "refresh skip identical (req=%s hint=%s) fast=%d cleans=%d gray=%d skipped=%lu", modeName(requested),
          hintName(hint), fastSinceClean_, cleansSinceFull_, grayOnGlass_ ? 1 : 0,
          static_cast<unsigned long>(skipped_));
}

void PanelRefreshCoordinator::noteGrayPass() {
  if (!enabled_) return;
  Guard g(*this);
  grayOnGlass_ = true;
  LOG_DBG(TAG, "refresh gray pass on glass (fast=%d cleans=%d)", fastSinceClean_, cleansSinceFull_);
}

void PanelRefreshCoordinator::resyncShadow(const uint8_t* fb, const bool inverted) {
  if (!enabled_) return;
  Guard g(*this);
  if (!shadow_ || !fb) {
    shadowValid_ = false;
    return;
  }
  memcpy(shadow_, fb, size_);
  shadowValid_ = true;
  shadowInverted_ = inverted;
}

void PanelRefreshCoordinator::invalidate() {
  Guard g(*this);
  shadowValid_ = false;
}
