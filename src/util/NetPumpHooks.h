#pragma once

// El bombeo de verdad que cuelga de include/NetPump.h. Vive en src/ porque
// necesita POWER_KEY y BoardConfig, que lib/ServerClient no puede ver.
//
// begin() se llama UNA vez desde setup(), después de POWER_KEY.begin() y de
// que los botones estén configurados: anota la tarea del loop (lo que el
// bombeo toca —I2C del PMIC, GPIO de los botones— sólo vale desde ahí) e
// instala el enganche.
namespace netpumphooks {
void begin();
}  // namespace netpumphooks
