#include "ServerTestActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ServerCredentialStore.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include "SilentRestart.h"

void ServerTestActivity::onEnter() {
  Activity::onEnter();
  state = CONNECTING;
  baseUrl = SERVER_STORE.getBaseUrl();

  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ServerTestActivity::onExit() {
  Activity::onExit();
  // Same as the other WiFi activities: drop the radio and silent-restart to
  // clear the LWIP/TLS heap fragmentation.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void ServerTestActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_ERR("SERVER_TEST", "WiFi connection failed");
    state = WIFI_FAILED;
    requestUpdate();
    return;
  }
  // The checks run from loop() so the "checking" screen paints first.
  state = CHECKING;
  requestUpdate();
}

void ServerTestActivity::runChecks() {
  ServerClient::Response resp;

  // 1. Public endpoint: is the server there at all?
  reach = SERVER_CLIENT.get("/firmware/latest", resp, /*auth=*/false);
  reachStatus = resp.status;

  // 2. Token: the server must answer /api/ping with 200 for a valid Bearer.
  auth = SERVER_CLIENT.get("/api/ping", resp, /*auth=*/true);
  authStatus = resp.status;

  // 3. Whatever was queued while offline goes out now.
  queued = SERVER_CLIENT.queueSize();
  flushed = 0;
  if (queued > 0 && auth == ServerClient::Result::Ok) {
    flushed = SERVER_CLIENT.flushQueue();
    queued = SERVER_CLIENT.queueSize();
  }

  LOG_INF("SERVER_TEST", "%s: reach=%s(%d) auth=%s(%d) queued=%u flushed=%d", baseUrl.c_str(),
          ServerClient::resultName(reach), reachStatus, ServerClient::resultName(auth), authStatus, (unsigned)queued,
          flushed);
  state = DONE;
  requestUpdate();
}

void ServerTestActivity::loop() {
  if (state == CHECKING) {
    runChecks();
    return;
  }
  if (state == DONE || state == WIFI_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

void ServerTestActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int mid = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SERVER_TEST));

  char line[128];
  switch (state) {
    case CONNECTING:
      // The WiFi sub-activity owns the screen until it returns.
      break;
    case WIFI_FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 10, tr(STR_SERVER_WIFI_FAILED), true, EpdFontFamily::BOLD);
      break;
    case CHECKING:
      renderer.drawCenteredText(UI_10_FONT_ID, mid - 40, baseUrl.empty() ? tr(STR_SERVER_NOT_CONFIGURED) : baseUrl.c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, mid, tr(STR_SERVER_CHECKING), true, EpdFontFamily::BOLD);
      break;
    case DONE: {
      int y = mid - 80;
      renderer.drawCenteredText(UI_10_FONT_ID, y, baseUrl.empty() ? tr(STR_SERVER_NOT_CONFIGURED) : baseUrl.c_str());
      y += 40;

      if (reach == ServerClient::Result::Ok) {
        renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_SERVER_REACHABLE), true, EpdFontFamily::BOLD);
      } else {
        snprintf(line, sizeof(line), "%s (%s %d)", tr(STR_SERVER_UNREACHABLE), ServerClient::resultName(reach),
                 reachStatus);
        renderer.drawCenteredText(UI_10_FONT_ID, y, line, true, EpdFontFamily::BOLD);
      }
      y += 30;

      switch (auth) {
        case ServerClient::Result::Ok: snprintf(line, sizeof(line), "%s", tr(STR_SERVER_AUTH_OK)); break;
        case ServerClient::Result::NoToken: snprintf(line, sizeof(line), "%s", tr(STR_SERVER_NO_TOKEN)); break;
        case ServerClient::Result::Unauthorized:
          snprintf(line, sizeof(line), "%s (%d)", tr(STR_SERVER_AUTH_FAILED), authStatus);
          break;
        default:
          snprintf(line, sizeof(line), "%s: %s %d", tr(STR_SERVER_PING_FAILED), ServerClient::resultName(auth),
                   authStatus);
          break;
      }
      renderer.drawCenteredText(UI_10_FONT_ID, y, line);
      y += 30;

      snprintf(line, sizeof(line), tr(STR_SERVER_QUEUE_FORMAT), (int)queued);
      renderer.drawCenteredText(UI_10_FONT_ID, y, line);
      if (flushed > 0) {
        y += 30;
        snprintf(line, sizeof(line), tr(STR_SERVER_QUEUE_SENT_FORMAT), flushed);
        renderer.drawCenteredText(UI_10_FONT_ID, y, line);
      }
      break;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
