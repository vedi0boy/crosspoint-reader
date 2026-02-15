#include "StandbyDisplayActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include <StreamString.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <ctime>
#include <chrono>
#include <cstdint>

void StandbyDisplayActivity::taskTrampoline(void* param) {
  auto* self = static_cast<StandbyDisplayActivity*>(param);
  self->displayTaskLoop();
}

void StandbyDisplayActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    goBack();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = LOADING;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
  vTaskDelay(10 / portTICK_PERIOD_MS);

  updateWeather();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = STANDBY;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
}

void StandbyDisplayActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  xTaskCreate(&StandbyDisplayActivity::taskTrampoline, "StandbyDisplayActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void StandbyDisplayActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off wifi
  WiFi.disconnect(false);  // false = don't erase credentials, send disconnect frame
  delay(100);              // Allow disconnect frame to be sent
  WiFi.mode(WIFI_OFF);
  delay(100);  // Allow WiFi hardware to fully power down

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

// TODO: What is this?
void StandbyDisplayActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void StandbyDisplayActivity::render() {
  if (subActivity) {
    // Subactivity handles its own rendering
    return;
  }

  if (state == LOADING) {
    LOG_DBG("Standby", "Loading weather information");
  }

  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  if (state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, "Getting weather info...", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == STANDBY) {
    renderer.drawCenteredText(BOOKERLY_14_FONT_ID, 120, date.c_str(), true, EpdFontFamily::BOLD);

    int tempTextWidth = renderer.getTextWidth(BOOKERLY_18_FONT_ID, (temp + tempUnit).c_str(), EpdFontFamily::BOLD);
    int slashTextWidth = renderer.getTextWidth(BOOKERLY_18_FONT_ID, "  /  ", EpdFontFamily::REGULAR);
    int apparentTextWidth = renderer.getTextWidth(BOOKERLY_14_FONT_ID, (apparentTemp + tempUnit).c_str(), EpdFontFamily::REGULAR);

    int start = (renderer.getScreenWidth() - tempTextWidth - slashTextWidth - apparentTextWidth) / 2;

    renderer.drawText(BOOKERLY_18_FONT_ID, start, 500, (temp + tempUnit).c_str(), true, EpdFontFamily::BOLD);
    renderer.drawText(BOOKERLY_18_FONT_ID, start + tempTextWidth, 500, "  /  ", true, EpdFontFamily::REGULAR);
    renderer.drawText(BOOKERLY_14_FONT_ID, start + tempTextWidth + slashTextWidth, 506, (apparentTemp + tempUnit).c_str(), true, EpdFontFamily::REGULAR);

    int updatedTextWidth = renderer.getTextWidth(BOOKERLY_12_FONT_ID, ("Updated: " + time).c_str(), EpdFontFamily::REGULAR);
    renderer.drawText(BOOKERLY_12_FONT_ID, renderer.getScreenWidth() - updatedTextWidth - 10, renderer.getScreenHeight() - renderer.getLineHeight(BOOKERLY_12_FONT_ID) - 10, ("Updated: " + time).c_str(), true, EpdFontFamily::REGULAR);

    renderer.displayBuffer();
    return;
  }
}

void StandbyDisplayActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == STANDBY) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
      return; 
    }

    using namespace std::chrono;
    system_clock::time_point timeNow = system_clock::now();
    uint64_t millisecondsSinceEpoch = duration_cast<milliseconds>(timeNow.time_since_epoch()).count();
    if (millisecondsSinceEpoch - lastUpdate >= UPDATE_INTERVAL) {
      lastUpdate = millisecondsSinceEpoch;
      updateWeather();
      updateRequired = true;
    }

    return;
  }
}

void StandbyDisplayActivity::updateWeather() {
  StreamString stream;
  if (!HttpDownloader::fetchUrl(url, stream)) {
    goBack();
    return;
  }
  const char* outContent = stream.c_str();

  JsonDocument filter;
  JsonDocument doc;

  filter["current_units"]["temperature_2m"] = true;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["apparent_temperature"] = true;
  filter["current"]["weather_code"] = true;
  filter["current"]["time"] = true;
  filter["daily"]["sunrise"][0] = true;
  filter["daily"]["sunrise"][1] = true;
  filter["daily"]["sunset"][0] = true;
  filter["daily"]["sunset"][1] = true;
  
  const DeserializationError error = deserializeJson(doc, outContent, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("OTA", "JSON parse failed: %s", error.c_str());
    goBack();
    return;
  }

  tempUnit = doc["current_units"]["temperature_2m"].as<std::string>();
  float rawTemp = doc["current"]["temperature_2m"].as<float>();
  float rawApparentTemp = doc["current"]["apparent_temperature"].as<float>();

  std::stringstream ss;
  ss << std::fixed;
  ss << std::setprecision(1);
  
  ss << rawTemp;
  temp = ss.str();
  ss.str("");
  ss.clear();
  ss << rawApparentTemp;
  apparentTemp = ss.str();

  int weatherCode = doc["current"]["weather_code"].as<size_t>();
  if (weatherCode <= 19) {
    weatherState = CLEAR_SKY;
  } else if (weatherCode <= 49) {
    weatherState = CLOUDY;
  } else if (weatherCode <= 59) {
    weatherState = DRIZZLE;
  } else if (weatherCode <= 69) {
    weatherState = RAIN;
  } else if (weatherCode <= 79) {
    weatherState = SNOW;
  } else if (weatherCode <= 82) {
    weatherState = RAIN;
    isHeavy = true;
  } else if (weatherCode <= 90) {
    weatherState = SNOW;
    isHeavy = true;
  } else {
    weatherState = THUNDER;
  }

  std::string iso_time_str = doc["current"]["time"].as<std::string>();
  std::tm t = {};
  std::istringstream iss(iso_time_str);

  // Parse: %Y-%m-%dT%H:%M:%S
  iss >> std::get_time(&t, "%Y-%m-%dT%H:%M:%S");

  // 2. Format the time_point back into an ISO 8601 string (UTC format with 'Z')
  // std::format is available since C++20
  std::ostringstream oss;
  oss << std::put_time(&t, "%I:%M%p");
  time = oss.str();
  oss.str("");
  oss.clear();
  oss << std::put_time(&t, "%A, %B %e %Y");
  date = oss.str();
}
