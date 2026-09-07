// La "app del teléfono": una página web del Hono para hablarle al aparato
// desde cualquier navegador. Pide el token del aparato una vez (queda en
// localStorage) y usa la misma API que el aparato. Sirve para dejar mensajes
// en la pizarra del hub, crear recordatorios con fecha y repetición, agregar
// ítems a las listas, escribir notas, y tildar o borrar lo que sea.
//
//   GET  /board                      página (sin token; el JS lo pide)
//   POST /api/board/message  {from, text}
//   POST /api/board/reminder {title, dueAt: "YYYY-MM-DDTHH:MM"|null, repeat}
//   POST /api/board/item     {list, text}
//   POST /api/board/note     {text}
//   (leer, tildar y borrar: GET /api/hub, POST /api/hub/done, POST /api/hub/edit)
import { Hono } from "hono";
import { load, save, nextId, resolveList } from "./store";
import { savePhoto, MAX_PHOTO_BYTES } from "./photos";

export const boardApi = new Hono();

boardApi.post("/message", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const text = (b.text ?? "").toString().trim().slice(0, 300);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  store.messages.push({ id: nextId(store), from: (b.from ?? "").toString().trim().slice(0, 40) || "web", text, createdAt: new Date().toISOString(), read: false });
  await save(store);
  return c.json({ ok: true });
});

boardApi.post("/reminder", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const title = (b.title ?? "").toString().trim().slice(0, 200);
  if (!title) return c.json({ ok: false, error: "title required" }, 400);
  const dueAt = typeof b.dueAt === "string" && /^\d{4}-\d{2}-\d{2}(T\d{2}:\d{2})?$/.test(b.dueAt) ? b.dueAt : null;
  const repeat = ["none", "daily", "weekly", "monthly"].includes(b.repeat) ? b.repeat : "none";
  const store = await load();
  store.reminders.push({ id: nextId(store), title, dueAt, repeat, done: false, createdAt: new Date().toISOString() });
  await save(store);
  return c.json({ ok: true });
});

boardApi.post("/item", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const text = (b.text ?? "").toString().trim().slice(0, 200);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  const list = resolveList(store, b.list, true);
  store.lists[list].push({ id: nextId(store), text, done: false, dueDate: null, createdAt: new Date().toISOString() });
  await save(store);
  return c.json({ ok: true, list });
});

boardApi.post("/feed", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const url = (b.url ?? "").toString().trim().slice(0, 500);
  if (!/^https?:\/\//.test(url)) return c.json({ ok: false, error: "url required" }, 400);
  const store = await load();
  store.feeds ??= [];
  const name = (b.name ?? "").toString().trim().slice(0, 40) || new URL(url).hostname.replace(/^www\./, "");
  store.feeds.push({ id: nextId(store), name, url });
  await save(store);
  return c.json({ ok: true });
});

// El BMP ya viene convertido por el navegador (ver la página): 2 bpp, 4 grises.
boardApi.post("/photo", async (c) => {
  const name = (c.req.query("name") ?? "foto").toString().slice(0, 80);
  const bytes = new Uint8Array(await c.req.arrayBuffer());
  if (bytes.byteLength < 100 || bytes.byteLength > MAX_PHOTO_BYTES) return c.json({ ok: false, error: "bad size" }, 400);
  if (bytes[0] !== 0x42 || bytes[1] !== 0x4d) return c.json({ ok: false, error: "not a BMP" }, 400);
  const id = await savePhoto(name, bytes);
  console.log(`photo ${id}: ${name} (${bytes.byteLength} bytes)`);
  return c.json({ ok: true, id });
});

boardApi.get("/extra", async (c) => {
  const store = await load();
  return c.json({ ok: true, feeds: store.feeds ?? [], memories: store.memories ?? [] });
});

boardApi.post("/note", async (c) => {
  const b = await c.req.json().catch(() => ({}));
  const text = (b.text ?? "").toString().trim().slice(0, 2000);
  if (!text) return c.json({ ok: false, error: "text required" }, 400);
  const store = await load();
  store.notes.push({ id: nextId(store), text, createdAt: new Date().toISOString() });
  await save(store);
  return c.json({ ok: true });
});

const PAGE = `<!doctype html>
<html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pizarra</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#f4f4f4;color:#111}
header{background:#111;color:#fff;padding:12px 16px;display:flex;justify-content:space-between;align-items:center}
main{max-width:720px;margin:0 auto;padding:12px}
section{background:#fff;border-radius:12px;padding:12px 14px;margin:12px 0;box-shadow:0 1px 3px #0002}
h2{margin:0 0 8px;font-size:17px}
form{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px}
input,select,textarea,button{font:inherit;padding:8px 10px;border:1px solid #bbb;border-radius:8px}
input[type=text],textarea{flex:1;min-width:160px}
button{background:#111;color:#fff;border:none;cursor:pointer}
button.ghost{background:#eee;color:#111}
ul{list-style:none;margin:0;padding:0}
li{display:flex;gap:8px;align-items:center;padding:8px 0;border-top:1px solid #eee}
li span{flex:1}
li small{color:#666}
.muted{color:#666;font-size:14px}
</style></head><body>
<header><strong>Pizarra del aparato</strong><button class="ghost" onclick="logout()">Token</button></header>
<main>
<section><h2>Mensaje para el hub</h2>
<form onsubmit="return send(event,'/api/board/message',{from:f.from.value,text:f.text.value})" id="fm">
<input type="text" name="from" placeholder="De" style="max-width:120px"><input type="text" name="text" placeholder="Mensaje" required><button>Dejar</button></form>
<ul id="messages"></ul></section>
<section><h2>Recordatorio</h2>
<form onsubmit="return send(event,'/api/board/reminder',{title:f.title.value,dueAt:f.date.value?(f.date.value+(f.time.value?'T'+f.time.value:'')):null,repeat:f.repeat.value})">
<input type="text" name="title" placeholder="Qué" required><input type="date" name="date"><input type="time" name="time">
<select name="repeat"><option value="none">Una vez</option><option value="daily">Diario</option><option value="weekly">Semanal</option><option value="monthly">Mensual</option></select><button>Guardar</button></form>
<ul id="reminders"></ul></section>
<section><h2>Listas</h2>
<form onsubmit="return send(event,'/api/board/item',{list:f.list.value,text:f.text.value})"><select name="list" id="lists"></select><input type="text" name="text" placeholder="Ítem" required><button>Agregar</button></form>
<div id="listItems"></div></section>
<section><h2>Fotos</h2>
<form onsubmit="return false"><input type="file" name="photo" id="photoInput" accept="image/*"><button type="button" onclick="sendPhoto()">Subir</button></form>
<p class="muted" id="photoStatus"></p>
<ul id="photos"></ul></section>
<section><h2>Noticias (RSS)</h2>
<form onsubmit="return send(event,'/api/board/feed',{name:f.name.value,url:f.url.value})"><input type="text" name="name" placeholder="Nombre" style="max-width:140px"><input type="text" name="url" placeholder="https://.../rss" required><button>Agregar</button></form>
<ul id="feeds"></ul></section>
<section><h2>Memoria del asistente</h2><ul id="memories"></ul></section>
<section><h2>Notas</h2>
<form onsubmit="return send(event,'/api/board/note',{text:f.text.value})"><textarea name="text" rows="2" placeholder="Nota" required></textarea><button>Guardar</button></form>
<ul id="notes"></ul></section>
<p class="muted">Lo que cargues acá aparece en el aparato en la próxima sincronización (o mantené Atrás en el hub).</p>
</main>
<script>
let token=localStorage.getItem('deviceToken')||'';
function logout(){localStorage.removeItem('deviceToken');token='';boot();}
function h(){return {'Authorization':'Bearer '+token,'Content-Type':'application/json'};}
async function api(path,body){const r=await fetch(path,{method:body?'POST':'GET',headers:h(),body:body?JSON.stringify(body):undefined});if(r.status===401){logout();throw new Error('token');}return r.json();}
let f;
async function send(e,path,body){e.preventDefault();f=e.target;body=typeof body==='function'?body():body;try{await api(path,body);e.target.reset();await refresh();}catch(err){alert('No se pudo guardar');}return false;}
document.querySelectorAll('form').forEach(x=>x.addEventListener('submit',ev=>{f=ev.target;}));
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
async function done(kind,id){await api('/api/hub/done',{kind,id});refresh();}
async function edit(kind,id,action){await api('/api/hub/edit',{kind,id,action});refresh();}
async function refresh(){
  const d=await api('/api/hub?lang=es');
  document.getElementById('messages').innerHTML=d.messages.map(m=>'<li><span><b>'+esc(m.from)+'</b>: '+esc(m.text)+'</span><button class="ghost" onclick="done(\\'message\\','+m.id+')">Leído</button></li>').join('')||'<li class="muted">Sin mensajes</li>';
  document.getElementById('reminders').innerHTML=d.reminders.map(r=>'<li><span>'+esc(r.title)+' <small>'+esc(r.when||'')+'</small></span><button class="ghost" onclick="done(\\'reminder\\','+r.id+')">Hecho</button></li>').join('')||'<li class="muted">Sin recordatorios</li>';
  document.getElementById('lists').innerHTML=d.lists.map(l=>'<option>'+esc(l.name)+'</option>').join('');
  document.getElementById('listItems').innerHTML=d.lists.map(l=>'<h3 style="margin:10px 0 0;font-size:15px">'+esc(l.name)+' <small class="muted">'+l.items.length+'</small></h3><ul>'+(l.items.map(i=>'<li><span>'+esc(i.text)+'</span><button class="ghost" onclick="done(\\'item\\','+i.id+')">Hecho</button><button class="ghost" onclick="edit(\\'item\\','+i.id+',\\'delete\\')">Borrar</button></li>').join('')||'<li class="muted">Vacía</li>')+'</ul>').join('');
  const x=await api('/api/board/extra');
  const ph=await api('/api/photos');
  document.getElementById('photos').innerHTML=ph.photos.map(p=>'<li><span>'+esc(p.name)+' <small>'+Math.round(p.size/1024)+' KB</small></span><button class="ghost" onclick="delPhoto(\''+p.id+'\')">Borrar</button></li>').join('')||'<li class="muted">Sin fotos</li>';
  document.getElementById('feeds').innerHTML=x.feeds.map(f=>'<li><span>'+esc(f.name)+' <small>'+esc(f.url)+'</small></span><button class="ghost" onclick="edit(\'feed\','+f.id+',\'delete\')">Borrar</button></li>').join('')||'<li class="muted">Sin feeds</li>';
  document.getElementById('memories').innerHTML=x.memories.map(m=>'<li><span>'+esc(m.text)+'</span><button class="ghost" onclick="edit(\'memory\','+m.id+',\'delete\')">Borrar</button></li>').join('')||'<li class="muted">Nada guardado</li>';
  document.getElementById('notes').innerHTML=d.notes.map(n=>'<li><span>'+esc(n.text)+'</span><button class="ghost" onclick="edit(\\'note\\','+n.id+',\\'delete\\')">Borrar</button></li>').join('')||'<li class="muted">Sin notas</li>';
}
// El aparato muestra 800x480 en 4 grises: convertimos acá (canvas, difuminado
// Floyd-Steinberg) y mandamos un BMP de 2 bpp ya listo, sin trabajo en el servidor.
async function sendPhoto(){
  const input=document.getElementById('photoInput');
  const file=input.files&&input.files[0];
  if(!file){alert('Elegí una foto');return;}
  const st=document.getElementById('photoStatus');
  st.textContent='Convirtiendo...';
  const img=new Image();
  img.src=URL.createObjectURL(file);
  await img.decode();
  const W=800,H=480;
  const cv=document.createElement('canvas');cv.width=W;cv.height=H;
  const ctx=cv.getContext('2d');
  ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);
  const s=Math.min(W/img.width,H/img.height);
  const dw=Math.round(img.width*s),dh=Math.round(img.height*s);
  ctx.drawImage(img,Math.round((W-dw)/2),Math.round((H-dh)/2),dw,dh);
  const px=ctx.getImageData(0,0,W,H).data;
  const gray=new Float32Array(W*H);
  for(let i=0,j=0;i<px.length;i+=4,j++) gray[j]=0.299*px[i]+0.587*px[i+1]+0.114*px[i+2];
  const idx=new Uint8Array(W*H);
  for(let y=0;y<H;y++){
    for(let x=0;x<W;x++){
      const p=y*W+x;
      const old=gray[p];
      const q=Math.max(0,Math.min(3,Math.round(old/85)));
      idx[p]=q;
      const err=old-q*85;
      if(x+1<W)gray[p+1]+=err*7/16;
      if(y+1<H){
        if(x>0)gray[p+W-1]+=err*3/16;
        gray[p+W]+=err*5/16;
        if(x+1<W)gray[p+W+1]+=err*1/16;
      }
    }
  }
  const rowBytes=Math.ceil(W*2/32)*4;      // 2 bpp, filas alineadas a 4
  const pixels=rowBytes*H;
  const off=14+40+4*4;                      // cabeceras + paleta de 4 colores
  const buf=new Uint8Array(off+pixels);
  const dv=new DataView(buf.buffer);
  buf[0]=0x42;buf[1]=0x4d;
  dv.setUint32(2,buf.length,true);dv.setUint32(10,off,true);
  dv.setUint32(14,40,true);dv.setInt32(18,W,true);dv.setInt32(22,H,true);
  dv.setUint16(26,1,true);dv.setUint16(28,2,true);dv.setUint32(34,pixels,true);
  dv.setUint32(46,4,true);dv.setUint32(50,4,true);
  const levels=[0,85,170,255];
  for(let i=0;i<4;i++){const o=54+i*4;buf[o]=levels[i];buf[o+1]=levels[i];buf[o+2]=levels[i];buf[o+3]=0;}
  for(let y=0;y<H;y++){
    const row=off+(H-1-y)*rowBytes;         // BMP: de abajo hacia arriba
    for(let x=0;x<W;x++){
      const shift=6-2*(x%4);
      buf[row+(x>>2)]|=idx[y*W+x]<<shift;
    }
  }
  st.textContent='Subiendo '+Math.round(buf.length/1024)+' KB...';
  const r=await fetch('/api/board/photo?name='+encodeURIComponent(file.name),{method:'POST',headers:{'Authorization':'Bearer '+token,'Content-Type':'image/bmp'},body:buf});
  st.textContent=r.ok?'Foto subida':'No se pudo subir';
  input.value='';
  refresh();
}
async function delPhoto(id){await api('/api/photos/delete',{id});refresh();}
function boot(){if(!token){token=prompt('Token del aparato (web UI del aparato → Servidor)')||'';if(!token)return;localStorage.setItem('deviceToken',token);}refresh().catch(()=>{});}
boot();
</script></body></html>`;

export const board = new Hono();
board.get("/", (c) => c.html(PAGE));
