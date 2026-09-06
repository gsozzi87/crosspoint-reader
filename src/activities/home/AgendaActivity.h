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
  enum Kind { MESSAGES, REMINDERS, LIST };
  struct Section {
    Kind kind;
    int listIndex;  // into HUB_STORE.lists when kind == LIST
  };
  std::vector<Section> sections;
  ButtonNavigator buttonNavigator;
  int sectionIndex = 0;
  int itemIndex = 0;
  int itemsPerPage = 1;
  OptionPopup menu;                   // Move / Date / Delete, then the sub-choice
  enum MenuStep { NONE, MAIN, MOVE, DATE };
  MenuStep menuStep = NONE;
  std::vector<std::string> menuOptions;
  int menuItemId = 0;

  void rebuildSections();
  int sectionCount() const { return static_cast<int>(sections.size()); }
  int itemCount() const;
  const Section& current() const { return sections[sectionIndex]; }
  std::string sectionTitle(int index) const;
  std::string itemText(int index, std::string& detail) const;
  void tickCurrent();
  void openItemMenu();
  void onMenuPick(int index);
  void sendEdit(const char* action, const char* list, const char* dueDate);
};
