// Conversor de imágenes para el panel del aparato: 480x800 y cuatro grises.
//
// Vivía en `photos.ts`. Las fotos salieron del producto, pero esto NO es de las
// fotos: lo usan los adjuntos de los viajes (un PDF que se rasteriza para que el
// aparato lo pueda mirar) y su vista previa en la web.

import sharp from "sharp";

// La pantalla del aparato: 480x800 y cuatro grises (0, 85, 170, 255).
const SCREEN_W = 480;
const SCREEN_H = 800;
const LEVELS = [0, 85, 170, 255];

// Foto de teléfono -> BMP de 2 bpp listo para el panel. Lo hace el servidor (antes
// lo hacía el navegador con un canvas): acá se puede rotar por EXIF, escalar bien
// y difuminar con Floyd-Steinberg sin depender del teléfono que suba la foto.
export async function toDeviceBmp(input: Uint8Array): Promise<Uint8Array> {
  const { data, info } = await sharp(input)
    .rotate()  // respeta la orientación EXIF: si no, las verticales salen acostadas
    .resize(SCREEN_W, SCREEN_H, { fit: "contain", background: { r: 255, g: 255, b: 255 } })
    .greyscale()
    .raw()
    .toBuffer({ resolveWithObject: true });
  const w = info.width;
  const h = info.height;

  // Difuminado a 4 niveles (el error se reparte a los vecinos: sin esto una foto
  // queda en cuatro manchas planas).
  const gray = new Float32Array(w * h);
  for (let i = 0; i < w * h; i++) gray[i] = data[i];
  const idx = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const p = y * w + x;
      const old = gray[p];
      const q = Math.max(0, Math.min(3, Math.round(old / 85)));
      idx[p] = q;
      const err = old - LEVELS[q];
      if (x + 1 < w) gray[p + 1] += (err * 7) / 16;
      if (y + 1 < h) {
        if (x > 0) gray[p + w - 1] += (err * 3) / 16;
        gray[p + w] += (err * 5) / 16;
        if (x + 1 < w) gray[p + w + 1] += (err * 1) / 16;
      }
    }
  }

  const rowBytes = Math.ceil((w * 2) / 32) * 4;  // 2 bpp, filas alineadas a 4
  const off = 14 + 40 + 4 * 4;                   // cabeceras + paleta de 4 colores
  const buf = new Uint8Array(off + rowBytes * h);
  const dv = new DataView(buf.buffer);
  buf[0] = 0x42;
  buf[1] = 0x4d;
  dv.setUint32(2, buf.length, true);
  dv.setUint32(10, off, true);
  dv.setUint32(14, 40, true);
  dv.setInt32(18, w, true);
  dv.setInt32(22, h, true);
  dv.setUint16(26, 1, true);
  dv.setUint16(28, 2, true);
  dv.setUint32(34, rowBytes * h, true);
  dv.setUint32(46, 4, true);
  dv.setUint32(50, 4, true);
  for (let i = 0; i < 4; i++) {
    const o = 54 + i * 4;
    buf[o] = LEVELS[i];
    buf[o + 1] = LEVELS[i];
    buf[o + 2] = LEVELS[i];
    buf[o + 3] = 0;
  }
  for (let y = 0; y < h; y++) {
    const row = off + (h - 1 - y) * rowBytes;  // BMP: de abajo hacia arriba
    for (let x = 0; x < w; x++) buf[row + (x >> 2)] |= idx[y * w + x] << (6 - 2 * (x % 4));
  }
  return buf;
}

// El BMP de 2 bpp del aparato -> PNG para verlo en el teléfono. Es el inverso
// exacto de toDeviceBmp: paleta de 4 grises, filas de abajo hacia arriba,
// alineadas a 4 bytes. Sirve también para las páginas de los adjuntos, que
// tienen el mismo formato.
export async function bmpToPng(bmp: Uint8Array): Promise<Buffer> {
  if (bmp.length < 70 || bmp[0] !== 0x42 || bmp[1] !== 0x4d) throw new Error("no es un BMP");
  const dv = new DataView(bmp.buffer, bmp.byteOffset, bmp.byteLength);
  const off = dv.getUint32(10, true);
  const w = dv.getInt32(18, true);
  const hRaw = dv.getInt32(22, true);
  const bpp = dv.getUint16(28, true);
  const h = Math.abs(hRaw);
  if (bpp !== 2 || w <= 0 || w > 4096 || h <= 0 || h > 4096) throw new Error("BMP raro");
  const palette: number[] = [];
  for (let i = 0; i < 4; i++) palette.push(bmp[54 + i * 4 + 1] ?? LEVELS[i]);  // el verde, da igual
  const rowBytes = Math.ceil((w * 2) / 32) * 4;
  const gray = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) {
    const row = off + (hRaw > 0 ? h - 1 - y : y) * rowBytes;
    for (let x = 0; x < w; x++) {
      const byte = bmp[row + (x >> 2)] ?? 0;
      gray[y * w + x] = palette[(byte >> (6 - 2 * (x % 4))) & 3];
    }
  }
  return sharp(Buffer.from(gray), { raw: { width: w, height: h, channels: 1 } }).png().toBuffer();
}
