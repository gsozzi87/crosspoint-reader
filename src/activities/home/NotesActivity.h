#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Notes dictated through Talk, from the hub cache. La primera fila es "dictar
// una nota nueva" (el aparato no tiene teclado: se agrega hablando), OK abre la
// nota en el visor paginado y Atrás mantenido ofrece borrarla (local + POST
// /api/hub/edit, encolado si no hay WiFi). La línea de abajo lo dice: antes
// borrar era un atajo que no aparecía en ningún lado.
class NotesActivity final : public Activity {
 public:
  explicit NotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Notes", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int index = 0;  // 0 = fila "dictar una nota"; 1.. = notas
  int perPage = 1;
  int partialCount = 0;  // parciales desde el último refresco limpio
  OptionPopup confirm;
  bool confirming = false;
  std::vector<std::string> confirmOptions;

  int rowCount() const;
  // Índice de la nota seleccionada, o -1 cuando el foco está en "dictar".
  int noteIndex() const;
  void addByVoice();
  void openCurrent();
  void deleteCurrent();
};
