#pragma once

#include <string>

#include "HubSyncActivity.h"  // FriendlyWifi: conexión sin la pantalla técnica
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "voice/VoiceRecorder.h"

// El conversor de unidades: lo que reemplaza al papelito pegado en la puerta de
// la alacena ("una taza de harina son 120 g").
//
// El aparato NO tiene teclado, así que la cantidad entra de dos maneras y las
// dos tienen que ser buenas:
//
//   - por VOZ, que es la rápida: "tres tazas de harina", "cinco millas",
//     "doce grados". Se graba, el servidor pasa el audio a texto y el número y
//     la unidad los saca el aparato mismo (`parseSpoken`), en los siete idiomas
//     del producto. Sin WiFi no hay dictado, pero el conversor sigue andando;
//   - con la PALANCA, un carrusel de dígitos de siete segmentos: arriba y abajo
//     cambian el dígito elegido, OK pasa al siguiente e inclinar el aparato a
//     los costados también lo mueve (MOTION).
//
// La pantalla es una sola lista de campos, como el reproductor: Decirlo, Tipo,
// (Ingrediente, sólo en Cocina), Unidad y los seis dígitos. OK avanza y da la
// vuelta, Atrás retrocede y en el primer campo sale. OK mantenido no existe en
// esta placa, así que no hay ningún gesto colgado de ahí.
//
// El resultado grande de arriba es la unidad "hermana" de la elegida (milla ->
// kilómetro, taza -> gramo, °C -> °F), y debajo van TODAS las demás de la misma
// familia: quien dice "cinco millas" ve kilómetros, metros y pies de una.
class UnitsActivity final : public Activity {
 public:
  // Cuatro dígitos enteros y dos decimales: 0,01 a 9999,99. Más que eso no se
  // arma cómodo con una palanca, y menos que eso deja afuera media receta.
  static constexpr int DIGITS = 6;
  static constexpr int INT_DIGITS = 4;

  explicit UnitsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Units", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return state == RECORDING; }
  bool preventAutoSleep() override { return state != EDIT; }

 private:
  enum State { EDIT, RECORDING, CONNECTING, TRANSCRIBING };

  // Los campos por los que pasa el cursor, en el mismo orden en el que se leen
  // en la pantalla. Ingredient sólo existe en la familia de cocina.
  enum class Field : uint8_t { Say, Kind, Ingredient, Unit, Sign, Digit };
  struct Slot {
    Field field = Field::Say;
    int8_t digit = -1;  // índice del dígito cuando field == Digit
  };

  State state = EDIT;

  int familyIdx = 0;
  int unitIdx = 0;
  int ingredientIdx = 0;
  uint8_t digits[DIGITS] = {0, 0, 0, 1, 0, 0};  // arranca en 1
  // El menos existe sólo en temperatura, que es la única familia donde un valor
  // bajo cero quiere decir algo (y en invierno es la mitad de las consultas).
  bool negative = false;
  Slot slots[5 + DIGITS];
  int slotCount = 0;
  int cursor = 0;

  ButtonNavigator navigator;

  // Dictado. Mismo esquema que Hablar y Lugar del clima: grabar, subir WiFi,
  // POST /api/transcribe y de vuelta a la pantalla con el número puesto.
  VoiceRecorder recorder{8};  // "tres tazas de harina" entra de sobra en 8 s
  FriendlyWifi wifi;
  bool wifiPicker = false;
  bool wifiActivated = false;
  bool requestPending = false;
  std::string spoken;  // lo que se entendió, tal cual, para mostrarlo
  std::string notice;  // renglón de ayuda: el dictado o el error de la última vez

  void rebuildSlots();
  const Slot& slot() const { return slots[cursor < slotCount ? cursor : 0]; }
  void moveCursor(int delta);
  void adjust(int delta);  // la palanca sobre el campo elegido

  double amount() const;
  void setAmount(double value);
  double density() const;  // g/ml del ingrediente, sólo en cocina
  double convertTo(int targetUnit) const;
  int heroUnit() const;  // la unidad "hermana", la del resultado grande

  void selectFamily(int newFamily, int newUnit);

  void startRecording();
  void stopRecording();
  void beginConnect();
  void pumpConnect();
  void onWifiSelectionComplete(bool connected);
  void performTranscribe();
  void applySpoken();
  void fail(StrId why, const std::string& detail = "");

  void loadState();
  void saveState() const;

  void renderInput();
  void renderVoice();
  int renderAmount(int x, int y, int w) const;
  int renderResult(int x, int y, int w) const;
  void renderEquivalences(int x, int y, int w, int bottom) const;
};
