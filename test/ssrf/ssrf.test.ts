import { expect, test } from "bun:test";
import { checkUrl, selectResolvedAddress } from "../../server/src/net";

const bloquea = (u: string) => expect(checkUrl(u, { allowHttp: true }).ok).toBe(false);
const pasa = (u: string) => expect(checkUrl(u, { allowHttp: true }).ok).toBe(true);

test("IPv4 mapeado en IPv6 no pasa", () => {
  bloquea("http://[::ffff:127.0.0.1]/");
  bloquea("http://[::ffff:7f00:1]/");
  bloquea("http://[::ffff:169.254.169.254]/");
  bloquea("http://[::ffff:10.0.0.1]/");
});
test("los rangos que faltaban", () => {
  bloquea("http://100.64.0.1/");
  bloquea("http://198.18.0.1/");
  bloquea("http://192.0.0.1/");
  bloquea("http://239.1.2.3/");
  bloquea("http://[64:ff9b::7f00:1]/");
});
test("lo de siempre sigue bloqueado", () => {
  bloquea("http://127.0.0.1/");
  bloquea("http://169.254.169.254/latest/meta-data/");
  bloquea("http://10.1.2.3/");
  bloquea("http://172.20.0.1/");
  bloquea("http://localhost/");
  bloquea("http://[::1]/");
});
test("las públicas siguen pasando", () => {
  pasa("https://www.lanacion.com.ar/rss");
  pasa("https://api.groq.com/openai/v1");
  pasa("http://8.8.8.8/");
  pasa("https://paper-esp32.up.railway.app/");
});

test("Railway puede usar su salida CGNAT sin aceptar una URL CGNAT literal", () => {
  bloquea("https://100.64.0.2/feed.xml");
  expect(selectResolvedAddress([{ address: "100.64.0.2", family: 4 }], true)).toEqual({
    address: "100.64.0.2",
    family: 4,
  });
  expect(selectResolvedAddress([{ address: "100.64.0.2", family: 4 }], false)).toBeNull();
});

test("una dirección privada no invalida otra respuesta pública", () => {
  expect(
    selectResolvedAddress(
      [
        { address: "10.0.0.5", family: 4 },
        { address: "23.45.67.89", family: 4 },
      ],
      false,
    ),
  ).toEqual({ address: "23.45.67.89", family: 4 });
});
