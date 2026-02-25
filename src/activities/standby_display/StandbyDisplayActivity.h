#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "activities/ActivityWithSubactivity.h"

class StandbyDisplayActivity : public ActivityWithSubactivity {
  enum State {
    WIFI_SELECTION,
    LOADING,
    STANDBY,
    CLOSING
  };

  enum WeatherState {
    CLEAR_SKY,
    CLOUDY,
    DRIZZLE,
    RAIN,
    SNOW,
    THUNDER
  };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  bool updateRequired = false;
  const std::function<void()> goBack;
  State state = WIFI_SELECTION;
  const std::string url = "https://api.open-meteo.com/v1/forecast?latitude=45.320835605886046&longitude=-75.66507323659283&daily=sunrise,sunset&current=temperature_2m,weather_code,apparent_temperature&timezone=America%2FNew_York";
  void onWifiSelectionComplete(bool success);
  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void updateWeather();
  void render();

  std::string time;
  std::string date;
  std::string tempUnit;
  std::string temp;
  std::string apparentTemp;
  bool isHeavy = false;
  WeatherState weatherState;

  const uint64_t UPDATE_INTERVAL = 30 * 60 * 1000; // 30 mins
  uint64_t lastUpdate;

 public:
  explicit StandbyDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& goBack)
      : ActivityWithSubactivity("StandbyDisplay", renderer, mappedInput), goBack(goBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return state == STANDBY; }
};
