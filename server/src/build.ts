// QUÉ VERSIÓN DEL SERVIDOR ESTÁ CORRIENDO, Y DESDE CUÁNDO.
//
// Esto existe por una pregunta que el dueño hizo y que no se podía contestar
// mirando: *"no aparece el servidor desplegado, ni se redeployó, que onda?"*.
// El servidor estaba desplegado — se comprobó bajando `/board/app.js` y
// comparando el sha contra el archivo local —, pero eso es un truco de
// consola, no algo que él pueda ver. Un despliegue que no se puede confirmar
// de un vistazo es indistinguible de uno que no pasó.
//
// Railway inyecta estas variables en cada build; fuera de Railway quedan
// vacías y se dice "local", que también es una respuesta.
const START = Date.now();

export type BuildInfo = {
  commit: string;   // sha corto del commit desplegado ("" fuera de Railway)
  branch: string;
  startedAt: number;  // epoch ms del arranque de ESTE proceso
  uptimeS: number;
};

export function buildInfo(): BuildInfo {
  const sha = process.env.RAILWAY_GIT_COMMIT_SHA ?? "";
  return {
    commit: sha.slice(0, 7),
    branch: process.env.RAILWAY_GIT_BRANCH ?? "",
    startedAt: START,
    uptimeS: Math.round((Date.now() - START) / 1000),
  };
}
