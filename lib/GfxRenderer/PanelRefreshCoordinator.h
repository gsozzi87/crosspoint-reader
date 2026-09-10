#pragma once

#include <HalDisplay.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <cstdint>

// Single owner of the FAST / HALF / FULL refresh policy for the whole
// firmware. GfxRenderer routes every panel refresh (displayBuffer,
// displayBufferAsync, displayGrayscaleBase) through plan() + commit(), so the
// panel rule "one clean refresh every 10-15 partials" holds at device level
// instead of per screen: the ~27 per-Activity partial counters keep working
// (an explicit HALF/FULL they ask for is simply honored and counted here), but
// they are no longer the thing that keeps the glass clean.
//
// Policy (see docs/ws397/REDISENO-BRIEF.md D1 and the adversarial review):
//   - counts EFFECTIVE FAST refreshes of UI screens; after FAST_BEFORE_CLEAN
//     the next UI FAST is promoted to HALF (0xD7: one flash, single cycle);
//   - page turns (Hint::PageTurn) keep the reader's own cadence and only get
//     a PAGE_TURN_CEILING safety net;
//   - every CLEANS_BEFORE_FULL cleans, one FULL (0xF7): on this panel 0xD7
//     flashes but does not restore contrast (folloup's measurement), and
//     nothing else in the firmware ever asks for 0xF7 after boot;
//   - the first paint after begin() is promoted to HALF (the driver would do it
//     too; keeping it here keeps the counters honest);
//   - a grayscale pass is neutral for the counter but leaves gray pixels on
//     the glass that a differential FAST cannot re-drive (RED holds the BW
//     plane), so the next NON-identical UI FAST is promoted to HALF;
//   - a 48 KB shadow of the last committed frame (PSRAM) lets a UI FAST whose
//     frame is byte-identical to what the panel shows skip the SPI + waveform
//     entirely. Output polarity is applied by the driver on the way out, so the
//     shadow also remembers the polarity it was shown with. HALF/FULL, async
//     and page-turn refreshes are never skipped.
//
// Thread safety: GfxRenderer holds the coordinator's own mutex around
// plan + panel write + commit, so callers on the loop task (boot, sleep,
// screenshots) and the render task cannot interleave the memcmp/memcpy.
//
// The coordinator is inert (plain pass-through, no shadow) on boards other than
// the WS397 so upstream boards keep their behaviour untouched.
class PanelRefreshCoordinator {
 public:
  enum class Hint : uint8_t { Ui, PageTurn };

  struct Plan {
    HalDisplay::RefreshMode mode;
    bool skip;
  };

  static constexpr int FAST_BEFORE_CLEAN = 12;  // effective UI FASTs before a HALF (0xD7)
  static constexpr int PAGE_TURN_CEILING = 24;  // safety ceiling for page turns (the reader cleans earlier)
  static constexpr int CLEANS_BEFORE_FULL = 2;  // every N cleans one is a FULL (0xF7); 0 = never

  ~PanelRefreshCoordinator();

  // Allocates the shadow (PSRAM) and resets the counters. Idempotent: a second
  // begin() (display re-init) keeps the allocation and just invalidates.
  void begin(uint32_t bufferSize, bool enabled);

  // Decide the effective mode for a refresh the caller wants in `requested`.
  // `allowSkip=false` forces the panel write even for an identical UI frame
  // (grayscale bases: the planes that follow need the base on the glass).
  Plan plan(const uint8_t* fb, HalDisplay::RefreshMode requested, Hint hint, bool inverted, bool async,
            bool allowSkip = true);

  // Record a refresh that actually reached the panel with `effective` mode.
  void commit(const uint8_t* fb, HalDisplay::RefreshMode effective, Hint hint, bool inverted, bool async);

  // Record a refresh skipped because the frame was identical to the shadow.
  void commitSkip(HalDisplay::RefreshMode requested, Hint hint);

  // Mientras el micrófono está abierto, una limpieza (HALF/FULL) son cientos
  // de ms de SPI con el DMA de RX de 90 ms: se pierden muestras y se come
  // media palabra. Con el hold puesto, la cadencia NO promueve un FAST; el
  // contador sigue corriendo y la limpieza sale apenas se suelta.
  void setCleanHold(bool hold) { cleanHold_ = hold; }
  bool cleanHold() const { return cleanHold_; }

  // A 4-gray pass was written on top of the current base. Neutral for the
  // counter; arms the "gray on glass" promotion for the next UI FAST.
  void noteGrayPass();

  // The framebuffer now holds exactly the BW base that is on the glass
  // (restoreBwBuffer(true), cleanupGrayscaleWithFrameBuffer): make the shadow
  // match it again.
  void resyncShadow(const uint8_t* fb, bool inverted);

  // Forget what the panel shows (display re-init, anything that bypassed us).
  void invalidate();

  bool enabled() const { return enabled_; }
  uint32_t skippedIdenticalFrames() const { return skipped_; }
  int fastSinceClean() const { return fastSinceClean_; }
  int cleansSinceFull() const { return cleansSinceFull_; }
  bool grayOnGlass() const { return grayOnGlass_; }

  // Own lock, taken by GfxRenderer around plan + panel write + commit. No-op
  // until begin() created the mutex.
  void lock() const;
  void unlock() const;
  class Guard {
   public:
    explicit Guard(const PanelRefreshCoordinator& c) : c_(c) { c_.lock(); }
    ~Guard() { c_.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

   private:
    const PanelRefreshCoordinator& c_;
  };

  static const char* modeName(HalDisplay::RefreshMode mode);
  static const char* hintName(Hint hint);

 private:
  bool shadowMatches(const uint8_t* fb, bool inverted) const;

  bool enabled_ = false;
  uint8_t* shadow_ = nullptr;
  uint32_t size_ = 0;
  bool shadowValid_ = false;
  bool shadowInverted_ = false;
  bool grayOnGlass_ = false;
  bool cleanHold_ = false;
  bool firstPaint_ = true;
  int fastSinceClean_ = 0;
  int cleansSinceFull_ = 0;
  uint32_t skipped_ = 0;
  uint32_t commits_ = 0;
  SemaphoreHandle_t mutex_ = nullptr;
};
