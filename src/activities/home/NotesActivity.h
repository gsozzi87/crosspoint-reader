#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Notes dictated through Talk, from the hub cache. OK opens one in the paged
// viewer, Back held offers to delete it (local + POST /api/hub/edit, queued
// when offline).
class NotesActivity final : public Activity {
 public:
  explicit NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Notes", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int index = 0;
  int perPage = 1;
  OptionPopup confirm;
  bool confirming = false;
  std::vector<std::string> confirmOptions;

  void openCurrent();
  void deleteCurrent();
};
