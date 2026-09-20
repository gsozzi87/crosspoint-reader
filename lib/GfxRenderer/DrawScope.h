#pragma once

// REV-056: QUIÉN está dibujando cuando un píxel cae fuera de la pantalla.
//
// El log del aparato repetía `[GFX] 80 pixeles fuera de pantalla (ultimo
// 480,447)` en cada cuadro y esa línea no alcanza para encontrar al culpable:
// `drawPixel` es el fondo de todo (texto, líneas, iconos, marcos, las
// primitivas de Lua, el chrome de cada Activity), así que la coordenada sola
// deja el mismo espacio de búsqueda que no tener nada. Ya se falló una vez
// arreglando por inferencia (REV-053, refutada: `fillRectImpl` ya recortaba).
//
// Esto es instrumentación: dos punteros a literales que dicen qué pantalla y
// qué operación estaban corriendo. Cuesta una escritura de puntero por
// operación de dibujo —NO por píxel— y nada en el caso normal.
namespace gfxscope {

// La pantalla de turno. Se COPIA a un buffer fijo: el nombre sale del
// `c_str()` de la Activity viva, y la instrumentación se lee después —desde
// `drawPixel`, o al cerrar la ventana de un segundo—, cuando esa Activity puede
// haber muerto ya. Guardar el puntero sería un uso después de liberar, en un
// camino que existe SÓLO para diagnosticar. La copia es una por pasada del
// loop, no una por píxel.
const char* activity();
void setActivity(const char* tag);

// La primitiva de turno. Acá sí es el puntero pelado: los llamadores pasan
// literales de la propia función ("cp.image", "drawText"), que viven siempre.
const char* op();
void setOp(const char* tag);

// RAII para la operación: deja lo que había al salir, así una primitiva que
// llama a otra no borra el nombre de la de afuera.
class Op {
 public:
  explicit Op(const char* tag) : previous_(op()) { setOp(tag); }
  ~Op() { setOp(previous_); }
  Op(const Op&) = delete;
  Op& operator=(const Op&) = delete;

 private:
  const char* previous_;
};

}  // namespace gfxscope
