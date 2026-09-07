#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// News: the RSS/Atom feeds loaded from the board page, headlines per feed,
// and the article cleaned to plain text by the server, read in the paged
// viewer. Headlines are cached on the SD (/.crosspoint/rss/feeds.json) and
// each article read is kept (last ones) so they open without WiFi. Back
// held refreshes the headlines.
class NewsActivity final : public Activity {
 public:
  explicit NewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("News", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == LOADING; }

 private:
  enum State { FEEDS, ITEMS, READING, CONNECTING, LOADING, FAILED };
  enum Pending { NONE, REFRESH, ARTICLE };
  State state = FEEDS;
  Pending pending = NONE;

  struct Item {
    int id = 0;
    std::string title;
    std::string when;
  };
  struct Feed {
    int id = 0;
    std::string name;
    std::vector<Item> items;
  };
  std::vector<Feed> feeds;
  int feedIndex = 0;
  int itemIndex = 0;
  int itemsPerPage = 1;
  ButtonNavigator buttonNavigator;
  bool wifiActivated = false;
  StrId failureId = StrId::STR_ASK_FAILED;
  std::string failureDetail;

  bool loadCache();
  bool fetchFeeds();
  std::string articlePath(int feed, int item) const;
  bool readArticle(const std::string& path, std::string& title, std::string& text);
  bool fetchArticle(int feed, int item, std::string& title, std::string& text);
  void openArticle();
  void showArticle(const std::string& title, const std::string& text);
  void ensureConnected();
  void onWifiSelectionComplete(bool connected);
  void fail(StrId why, std::string detail = "");
};
