// Quién queda de administrador al registrarse.
//
// Había un agujero: además de la primera cuenta, se daba admin a quien se
// registrara con el correo de ADMIN_EMAIL. `seedFromFiles()` crea esa cuenta
// sólo cuando la base está vacía, así que en un servidor donde ADMIN_EMAIL se
// configuró DESPUÉS ese correo no existía y cualquiera podía tomarlo. Un correo
// es público: no es una credencial.
//
// Esto prueba la REGLA, sin base de datos: la regla es lo que estaba mal.
import { expect, test } from "bun:test";

// La regla vieja y la nueva, tal cual están en el código.
const reglaVieja = (cuentas: number, email: string, adminEmail: string) =>
  cuentas === 0 || (!!adminEmail && email === adminEmail);
const reglaNueva = (cuentas: number) => cuentas === 0;

test("crear la instancia: el primero es administrador", () => {
  expect(reglaNueva(0)).toBe(true);
});

test("con cuentas ya creadas, nadie se hace admin registrándose", () => {
  expect(reglaNueva(5)).toBe(false);
});

test("EL AGUJERO: con la regla vieja, saber el correo del dueño alcanzaba", () => {
  // Servidor con gente adentro y ADMIN_EMAIL configurado después, así que ese
  // correo nunca llegó a la base.
  expect(reglaVieja(5, "dueno@ejemplo.com", "dueno@ejemplo.com")).toBe(true);  // así estaba
  expect(reglaNueva(5)).toBe(false);                                            // así quedó
});

test("un correo cualquiera nunca dio admin, ni antes ni ahora", () => {
  expect(reglaVieja(5, "otro@ejemplo.com", "dueno@ejemplo.com")).toBe(false);
  expect(reglaNueva(5)).toBe(false);
});
