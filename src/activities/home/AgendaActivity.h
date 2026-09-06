#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Reminders and task lists from the hub cache (HubStore): a section list
// (Reminders, then each list with its pending count), and inside a section
// the items. OK ticks an item: it goes locally and POST /api/hub/done is sent
// or queued for the next sync, so it works without WiFi. Back goes up a level.
class AgendaActivity final : public Activity {
 public:
  explicit AgendaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Agenda", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Level { SECTIONS, ITEMS };
  Level level = SECTIONS;
  ButtonNavigator buttonNavigator;
  int sectionIndex = 0;  // 0 = reminders, 1.. = lists
  int itemIndex = 0;
  int itemsPerPage = 1;
  OptionPopup menu;                   // Move / Date / Delete, then the sub-choice
  enum MenuStep { NONE, MAIN, MOVE, DATE };
  MenuStep menuStep = NONE;
  std::vector<std::string> menuOptions;
  int menuItemId = 0;

  int sectionCount() const;
  int itemCount() const;
  std::string sectionTitle(int index) const;
  std::string itemText(int index, std::string& detail) const;
  void tickCurrent();
  void openItemMenu();
  void onMenuPick(int index);
  void sendEdit(const char* action, const char* list, const char* dueDate);
};
