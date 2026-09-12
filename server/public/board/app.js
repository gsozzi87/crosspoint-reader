"use strict";
// La app del teléfono. Un solo archivo, sin framework: un estado (`S`, lo que
// devuelve GET /api/board/state), un enrutador por #hash y una función de
// pintado por pantalla. Después de CADA cambio se vuelve a pedir el estado
// entero y se repinta: así nunca queda una parte vieja y otra nueva en la
// misma pantalla, que era la queja de la página anterior.
//
// Pantallas (barra de abajo): Hoy · Agenda · Listas · Notas · Más.
// Debajo de Más: Fotos, Noticias, Viajes, Memoria, Ajustes, Aparatos, IA,
// Contenido, Log y Cuenta.
//
// Todo lo que se edita se edita en una "hoja" que sube desde abajo (sheet):
// tocar una fila la abre con todos los campos y el botón de borrar.

// ── Estado ──────────────────────────────────────────────────────────────────
let token = localStorage.getItem("deviceToken") || "";
let me = null;       // GET /auth/me: {ok, multi, email, isAdmin, devices}
let S = null;        // GET /api/board/state
let entered = false;
let cfg = null;      // GET /api/board/config (solo admin)
const CAL = {};      // caché del calendario por mes: "2026-09" -> {days, events}
const DAY = {};      // caché del día: "2026-09-12" -> items
let calMonth = "";   // mes visible "YYYY-MM"
let calSel = "";     // día elegido "YYYY-MM-DD"
let tripCache = {};  // id -> viaje entero
let rssCache = null;

const $ = (id) => document.getElementById(id);
const qs = (root, sel) => root.querySelector(sel);
const qsa = (root, sel) => Array.from(root.querySelectorAll(sel));

function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
function attr(s) { return esc(s).replace(/\n/g, "&#10;"); }

let toastTimer = 0;
function toast(msg, ms) {
  const t = $("toast");
  t.textContent = msg;
  t.classList.add("on");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove("on"), ms || 2200);
}

function busy(on) {
  let el = $("stripe");
  if (on && !el) { el = document.createElement("div"); el.id = "stripe"; el.className = "stripe"; document.body.appendChild(el); }
  if (!on && el) el.remove();
}

function multi() { return !!(me && me.multi); }
function isAdmin() { return !me || me.isAdmin !== false; }

// ── API ─────────────────────────────────────────────────────────────────────
function authHeaders() {
  const h = {};
  if (token) h["Authorization"] = "Bearer " + token;  // con sesión no hay token: va la cookie
  return h;
}

async function api(path, body, method) {
  const headers = authHeaders();
  if (body !== undefined && !(body instanceof FormData) && !(body instanceof Blob)) headers["Content-Type"] = "application/json";
  const r = await fetch(path, {
    method: method || (body !== undefined ? "POST" : "GET"),
    headers,
    credentials: "same-origin",
    body: body === undefined ? undefined : (body instanceof FormData || body instanceof Blob) ? body : JSON.stringify(body),
  });
  if (r.status === 401) {
    if (entered) {
      if (multi()) showLogin("Se cerró la sesión. Entra de nuevo.");
      else gate("El servidor rechazó el token.");
    }
    throw new Error("no autorizado");
  }
  let j = null;
  try { j = await r.json(); } catch (e) {}
  if (!r.ok || (j && j.ok === false)) throw new Error((j && j.error) || ("error " + r.status));
  return j;
}

async function apiText(path, method) {
  const r = await fetch(path, { method: method || "GET", headers: authHeaders(), credentials: "same-origin" });
  if (r.status === 401) throw new Error("no autorizado");
  return r.text();
}

async function loadState() {
  S = await api("/api/board/state");
  return S;
}

// Un cambio: se hace, se vuelve a pedir TODO el estado y se repinta.
async function change(fn, okMsg) {
  busy(true);
  try {
    const out = await fn();
    await loadState();
    render();
    if (okMsg) toast(okMsg);
    return out;
  } catch (e) {
    toast("No se pudo: " + e.message, 3500);
    throw e;
  } finally {
    busy(false);
  }
}

async function refresh() {
  busy(true);
  $("reloadBtn").classList.add("spin");
  try {
    await loadState();
    render();
  } catch (e) {
    toast("No se pudo actualizar: " + e.message, 3500);
  } finally {
    busy(false);
    $("reloadBtn").classList.remove("spin");
  }
}

// ── Fechas ──────────────────────────────────────────────────────────────────
const DOW = ["domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"];
const DOW3 = ["dom", "lun", "mar", "mié", "jue", "vie", "sáb"];
const MON = ["enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"];
const MON3 = ["ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"];

function pad2(n) { return (n < 10 ? "0" : "") + n; }
function dateOf(s) { const [y, m, d] = s.slice(0, 10).split("-").map(Number); return new Date(y, m - 1, d); }
function isoOf(d) { return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate()); }
function addDays(s, n) { const d = dateOf(s); d.setDate(d.getDate() + n); return isoOf(d); }
function todayIso() { return (S && S.today) || isoOf(new Date()); }
function fmtDay(s, withYear) {
  if (!s) return "";
  const d = dateOf(s);
  return DOW3[d.getDay()] + " " + d.getDate() + " " + MON3[d.getMonth()] + (withYear ? " " + d.getFullYear() : "");
}
function fmtDayLong(s) {
  const d = dateOf(s);
  return DOW[d.getDay()] + " " + d.getDate() + " de " + MON[d.getMonth()];
}
function fmtWhen(iso) {
  if (!iso) return "";
  const [date, time] = iso.split("T");
  const t = todayIso();
  const day = date === t ? "hoy" : date === addDays(t, 1) ? "mañana" : date === addDays(t, -1) ? "ayer" : fmtDay(date, date.slice(0, 4) !== t.slice(0, 4));
  return time ? day + " " + time : day;
}
function ago(ms) {
  if (!ms) return "";
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (s < 60) return "hace un momento";
  if (s < 3600) return "hace " + Math.round(s / 60) + " min";
  if (s < 86400) return "hace " + Math.round(s / 3600) + " h";
  const d = Math.round(s / 86400);
  return d === 1 ? "ayer" : "hace " + d + " días";
}
function fmtStamp(iso) {
  if (!iso) return "";
  const d = new Date(iso);
  if (isNaN(d.getTime())) return iso;
  return fmtDay(isoOf(d)) + " " + pad2(d.getHours()) + ":" + pad2(d.getMinutes());
}
function kb(b) { return b < 1024 ? b + " B" : b < 1048576 ? Math.round(b / 1024) + " KB" : (b / 1048576).toFixed(1) + " MB"; }

// ── Hoja (editor) ───────────────────────────────────────────────────────────
let sheetOpts = null;

function openSheet(title, html, opts) {
  sheetOpts = opts || {};
  const sh = $("sheet");
  const bg = $("sheetBg");
  sh.innerHTML =
    '<div class="grip"></div>' +
    '<h2><span class="grow">' + esc(title) + '</span><button class="iconbtn" data-act="sheet-close" aria-label="Cerrar">×</button></h2>' +
    '<div class="sheet-body">' + html + "</div>" +
    (sheetOpts.save || sheetOpts.del
      ? '<div class="actions">' +
        (sheetOpts.save ? '<button class="save" data-act="sheet-save">' + esc(sheetOpts.saveLabel || "Guardar") + "</button>" : "") +
        (sheetOpts.del ? '<button class="danger del" data-act="sheet-del">' + esc(sheetOpts.delLabel || "Borrar") + "</button>" : "") +
        "</div>"
      : "") +
    '<div class="status" id="sheetStatus"></div>';
  sh.hidden = false;
  bg.hidden = false;
  requestAnimationFrame(() => { sh.classList.add("on"); bg.classList.add("on"); });
  if (sheetOpts.onOpen) sheetOpts.onOpen(sh);
  const first = qs(sh, "[autofocus]");
  if (first) setTimeout(() => first.focus(), 250);
  return sh;
}

function closeSheet() {
  const sh = $("sheet");
  const bg = $("sheetBg");
  sh.classList.remove("on");
  bg.classList.remove("on");
  setTimeout(() => { sh.hidden = true; bg.hidden = true; sh.innerHTML = ""; }, 200);
  sheetOpts = null;
}

function sheetStatus(msg, cls) {
  const el = $("sheetStatus");
  if (!el) return;
  el.textContent = msg || "";
  el.className = "status " + (cls || "");
}

function val(root, name) {
  const el = qs(root, "[name='" + name + "']");
  if (!el) return "";
  if (el.type === "checkbox") return el.checked;
  return el.value;
}

function field(label, inner, rawLabel) { return '<label class="f"><span>' + (rawLabel ? label : esc(label)) + "</span>" + inner + "</label>"; }
function input(name, value, extra) { return '<input name="' + name + '" value="' + attr(value == null ? "" : value) + '" ' + (extra || "") + ">"; }
function select(name, options, value, extra) {
  return '<select name="' + name + '" ' + (extra || "") + ">" +
    options.map((o) => '<option value="' + attr(o[0]) + '"' + (String(o[0]) === String(value) ? " selected" : "") + ">" + esc(o[1]) + "</option>").join("") +
    "</select>";
}

// Confirmación de borrado con el mismo diálogo en todos lados.
function sure(msg) { return confirm(msg || "¿Borrar? No se puede deshacer."); }

// ── Repetición (recordatorios y eventos) ────────────────────────────────────
const REPEAT_KINDS = [["none", "No se repite"], ["daily", "Todos los días"], ["weekdays", "De lunes a viernes"], ["weekly", "Cada semana"], ["monthly", "Cada mes"], ["yearly", "Cada año"]];
const REPEAT_UNIT = { daily: "días", weekly: "semanas", monthly: "meses", yearly: "años" };

function untilIso(until) {
  if (!until) return "";
  const d = new Date(until * 1000);
  return isoOf(d);
}

function repeatHtml(rep) {
  rep = rep || { kind: "none" };
  const days = rep.days || [];
  return (
    '<div class="rep">' +
    field("Repetir", select("repKind", REPEAT_KINDS, rep.kind)) +
    '<div class="repDays chips" ' + (rep.kind === "weekly" ? "" : "hidden") + ">" +
    DOW3.map((d, i) => '<button type="button" class="chip' + (days.indexOf(i) >= 0 ? " on" : "") + '" data-day="' + i + '">' + d + "</button>").join("") +
    "</div>" +
    '<div class="two repMore" ' + (rep.kind === "none" || rep.kind === "weekdays" ? "hidden" : "") + ">" +
    field("Cada", '<div class="addbar" style="margin:0"><input name="repInterval" type="number" min="1" max="99" value="' + (rep.interval || 1) + '"><span class="muted repUnit" style="align-self:center">' + (REPEAT_UNIT[rep.kind] || "") + "</span></div>") +
    field("Hasta (opcional)", input("repUntil", untilIso(rep.until), 'type="date"')) +
    "</div>" +
    '<div class="muted repText" style="margin:-4px 0 10px 2px"></div>' +
    "</div>"
  );
}

function readRepeat(root) {
  const kind = val(root, "repKind");
  if (kind === "none") return { kind: "none" };
  const rep = { kind, interval: Math.max(1, Number(val(root, "repInterval")) || 1) };
  if (kind === "weekly") rep.days = qsa(root, ".repDays .chip.on").map((b) => Number(b.dataset.day));
  const until = val(root, "repUntil");
  if (until) rep.until = until;
  return rep;
}

// El texto ("los martes y jueves hasta el 12 dic") lo escribe el servidor: es
// exactamente lo que después ve el aparato.
function wireRepeat(root, dateName) {
  const box = qs(root, ".rep");
  if (!box) return;
  const update = async () => {
    const kind = val(root, "repKind");
    qs(box, ".repDays").hidden = kind !== "weekly";
    qs(box, ".repMore").hidden = kind === "none" || kind === "weekdays";
    qs(box, ".repUnit").textContent = REPEAT_UNIT[kind] || "";
    const out = qs(box, ".repText");
    if (kind === "none") { out.textContent = ""; return; }
    const rep = readRepeat(root);
    const date = val(root, dateName) || todayIso();
    const q = "kind=" + rep.kind + "&days=" + (rep.days || []).join(",") + "&interval=" + rep.interval + "&until=" + (rep.until || "") + "&date=" + date + "&lang=es";
    try {
      const r = await api("/api/calendar/repeat?" + q);
      out.textContent = "Va a sonar: " + r.text + (r.first && r.first !== date ? " · empieza el " + fmtDay(r.first) : "");
    } catch (e) { out.textContent = ""; }
  };
  box.addEventListener("change", update);
  box.addEventListener("click", (ev) => {
    const chip = ev.target.closest(".chip[data-day]");
    if (!chip) return;
    chip.classList.toggle("on");
    update();
  });
  const dateEl = qs(root, "[name='" + dateName + "']");
  if (dateEl) dateEl.addEventListener("change", update);
  update();
}

// ── Recordatorios ───────────────────────────────────────────────────────────
function reminderEditor(r, preset) {
  const isNew = !r;
  r = r || { title: preset || "", at: null, repeatSpec: { kind: "none" } };
  const [date, time] = (r.at || "T").split("T");
  openSheet(isNew ? "Recordatorio nuevo" : "Recordatorio",
    field("Qué", input("title", r.title, 'autofocus maxlength="200" placeholder="Sacar la basura"')) +
    '<div class="two">' + field("Día", input("date", date, 'type="date"')) + field("Hora", input("time", time || "", 'type="time"')) + "</div>" +
    '<p class="hint">Sin día queda como pendiente sin alarma. Sin hora suena a las 9:00.</p>' +
    repeatHtml(r.repeatSpec),
    {
      onOpen: (root) => wireRepeat(root, "date"),
      save: async (root) => {
        const title = val(root, "title").trim();
        if (!title) { sheetStatus("Ponle un título", "bad"); return false; }
        const date = val(root, "date");
        const time = val(root, "time");
        const body = { id: isNew ? null : r.id, title, dueAt: date ? date + (time ? "T" + time : "") : null, repeat: readRepeat(root) };
        const res = await change(() => api("/api/board/reminder", body));
        toast("Guardado · " + res.reminder.repeatText);
        return true;
      },
      del: isNew ? null : async () => {
        if (!sure("¿Borrar el recordatorio «" + r.title + "»?")) return false;
        await change(() => api("/api/hub/edit", { kind: "reminder", id: r.id, action: "delete" }), "Borrado");
        return true;
      },
    });
}

function reminderRow(r, opts) {
  const late = r.dueAt && r.dueAt < S.now;
  const sub = [r.at ? fmtWhen(r.at) : "sin fecha", r.repeatSpec && r.repeatSpec.kind !== "none" ? r.repeatText : ""].filter(Boolean).join(" · ");
  return '<li class="' + (late ? "late" : "") + (r.done ? " done" : "") + '">' +
    (opts && opts.noTick ? "" :
      '<button class="tick' + (r.done ? " on" : "") + '" data-act="rem-tick" data-id="' + r.id + '" data-at="' + (r.dueAt || 0) + '" aria-label="Hecho"><i></i></button>') +
    '<div class="body" data-act="rem-open" data-id="' + r.id + '"><span class="title">' + esc(r.title) + '</span><span class="sub">' + esc(sub) + "</span></div>" +
    (opts && opts.restore ? '<button class="ghost small" data-act="rem-restore" data-id="' + r.id + '">Reactivar</button>' : '<span class="chev">›</span>') +
    "</li>";
}

function findReminder(id) {
  return S.reminders.find((r) => r.id === id) || S.doneReminders.find((r) => r.id === id);
}

function remindersView() {
  const t = todayIso();
  const groups = [["Vencidos", []], ["Hoy", []], ["Mañana", []], ["Próximos", []], ["Sin fecha", []]];
  for (const r of S.reminders) {
    if (!r.at) groups[4][1].push(r);
    else if (r.dueAt < S.now) groups[0][1].push(r);
    else if (r.at.slice(0, 10) === t) groups[1][1].push(r);
    else if (r.at.slice(0, 10) === addDays(t, 1)) groups[2][1].push(r);
    else groups[3][1].push(r);
  }
  let html = '<div class="card"><h2><span class="grow">Recordatorios</span><span class="count">' + S.reminders.length + "</span></h2>";
  html += '<p class="hint">Toca uno para cambiarle la hora o la repetición. La casilla lo da por hecho (si se repite, pasa a la próxima vez).</p><ul class="rows">';
  if (!S.reminders.length) html += '<li class="empty">No hay recordatorios pendientes. Con el + de abajo agregas uno.</li>';
  for (const [name, list] of groups) {
    if (!list.length) continue;
    html += '<li class="head">' + name + "</li>" + list.map((r) => reminderRow(r)).join("");
  }
  html += "</ul></div>";
  if (S.doneReminders.length) {
    html += '<details class="card"><summary class="muted" style="cursor:pointer">Hechos (' + S.doneReminders.length + ')</summary><ul class="rows">' +
      S.doneReminders.map((r) => reminderRow(r, { noTick: true, restore: true })).join("") + "</ul></details>";
  }
  return html;
}

// ── Calendario ──────────────────────────────────────────────────────────────
async function loadMonth(ym) {
  if (CAL[ym]) return CAL[ym];
  const first = ym + "-01";
  const d = dateOf(first);
  // La grilla arranca el lunes anterior al 1 y termina el domingo después del último día.
  const startOffset = (d.getDay() + 6) % 7;
  const from = addDays(first, -startOffset);
  const lastDay = new Date(d.getFullYear(), d.getMonth() + 1, 0).getDate();
  const last = ym + "-" + pad2(lastDay);
  const endOffset = (7 - dateOf(last).getDay()) % 7;
  const to = addDays(last, endOffset);
  const r = await api("/api/calendar?from=" + from + "&to=" + to + "&lang=es");
  CAL[ym] = { from, to, days: r.days, events: r.events, today: r.today };
  if (r.today) S.today = r.today;
  return CAL[ym];
}

function clearCal() { for (const k of Object.keys(CAL)) delete CAL[k]; for (const k of Object.keys(DAY)) delete DAY[k]; }

function calendarView() {
  if (!calMonth) calMonth = todayIso().slice(0, 7);
  if (!calSel) calSel = todayIso();
  const m = CAL[calMonth];
  const d = dateOf(calMonth + "-01");
  let html = '<div class="card"><div class="calhead"><button class="iconbtn" data-act="cal-prev">‹</button><b>' + MON[d.getMonth()] + " " + d.getFullYear() +
    '</b><button class="ghost small" data-act="cal-today">Hoy</button><button class="iconbtn" data-act="cal-next">›</button></div>';
  html += '<div class="cal">' + DOW3.slice(1).concat(DOW3[0]).map((x) => '<div class="dow">' + x + "</div>").join("");
  if (!m) {
    html += "</div><p class='loading'>Cargando…</p></div>";
    loadMonth(calMonth).then(render).catch((e) => toast("No se pudo cargar el calendario: " + e.message));
    return html;
  }
  const counts = {};
  for (const x of m.days) counts[x.date] = x.count;
  let day = m.from;
  while (day <= m.to) {
    const cls = ["d"];
    if (day.slice(0, 7) !== calMonth) cls.push("other");
    if (day === todayIso()) cls.push("today");
    if (day === calSel) cls.push("sel");
    const n = counts[day] || 0;
    html += '<button class="' + cls.join(" ") + '" data-act="cal-sel" data-date="' + day + '">' + Number(day.slice(8, 10)) + (n ? '<span class="n">' + (n > 1 ? n : "•") + "</span>" : '<i class="none"></i>') + "</button>";
    day = addDays(day, 1);
  }
  html += "</div></div>";

  const items = m.events.filter((e) => e.date === calSel);
  html += '<div class="card"><h2><span class="grow">' + fmtDayLong(calSel) + '</span><button class="ghost small" data-act="ev-new" data-date="' + calSel + '">+ Evento</button></h2><ul class="rows">';
  if (!items.length) html += '<li class="empty">Nada agendado. Salen los eventos, los recordatorios con hora y los días de viaje.</li>';
  for (const o of items) html += occurrenceRow(o);
  html += "</ul></div>";
  return html;
}

function occurrenceRow(o) {
  const icon = o.kind === "reminder" ? "⏰" : o.kind === "trip" ? "✈️" : "📅";
  const when = o.allDay ? (o.days > 1 ? "día " + o.dayIndex + " de " + o.days : "todo el día") : o.time + (o.endTime && o.endTime !== o.time ? "–" + o.endTime : "");
  const sub = [when, o.place, o.repeat && o.repeat.kind !== "none" ? o.repeatText : ""].filter(Boolean).join(" · ");
  const act = o.kind === "reminder" ? "rem-open" : o.kind === "trip" ? "trip-go" : "ev-open";
  const extra = o.kind === "trip" ? ' data-trip="' + attr(o.tripId || "") + '"' : ' data-key="' + attr(o.key) + '"';
  return '<li><span class="kind">' + icon + '</span><div class="body" data-act="' + act + '" data-id="' + o.id + '"' + extra + '><span class="title">' + esc(o.title) + '</span><span class="sub">' + esc(sub) + "</span></div><span class=\"chev\">›</span></li>";
}

function eventEditor(o, presetDate, presetTitle) {
  const isNew = !o;
  const start = o ? o.startAt : (presetDate || calSel || todayIso());
  const end = o ? o.endAt : start;
  const [date, time] = start.split("T");
  const [endDate, endTime] = end.split("T");
  const allDay = o ? o.allDay : true;
  openSheet(isNew ? "Evento nuevo" : "Evento",
    field("Qué", input("title", o ? o.title : (presetTitle || ""), 'autofocus maxlength="200" placeholder="Dentista"')) +
    '<div class="two">' + field("Día", input("date", date, 'type="date"')) + field("Hasta", input("endDate", endDate || date, 'type="date"')) + "</div>" +
    '<label class="inline"><input type="checkbox" name="allDay" ' + (allDay ? "checked" : "") + "> Todo el día</label>" +
    '<div class="two timeRow" ' + (allDay ? "hidden" : "") + ">" + field("Desde", input("time", time || "", 'type="time"')) + field("Hasta", input("endTime", endTime || "", 'type="time"')) + "</div>" +
    field("Dónde", input("place", o ? o.place : "", 'maxlength="120"')) +
    field("Nota", '<textarea name="note" maxlength="1000" style="min-height:70px">' + esc(o ? o.note : "") + "</textarea>") +
    repeatHtml(o ? o.repeat : null),
    {
      onOpen: (root) => {
        wireRepeat(root, "date");
        qs(root, "[name=allDay]").addEventListener("change", (ev) => { qs(root, ".timeRow").hidden = ev.target.checked; });
      },
      save: async (root) => {
        const title = val(root, "title").trim();
        if (!title) { sheetStatus("Ponle un título", "bad"); return false; }
        const body = {
          id: isNew ? undefined : o.id, title,
          date: val(root, "date"), endDate: val(root, "endDate"), allDay: val(root, "allDay"),
          time: val(root, "time"), endTime: val(root, "endTime"),
          place: val(root, "place"), note: val(root, "note"), repeat: readRepeat(root),
        };
        if (!body.date) { sheetStatus("Falta el día", "bad"); return false; }
        clearCal();
        const r = await change(() => api("/api/calendar/event?lang=es", body));
        toast("Guardado · " + r.repeatText);
        return true;
      },
      del: isNew ? null : async () => {
        if (!sure("¿Borrar «" + o.title + "»" + (o.repeat && o.repeat.kind !== "none" ? " con todas sus repeticiones" : "") + "?")) return false;
        clearCal();
        await change(() => api("/api/calendar/event/delete", { id: o.id }), "Borrado");
        return true;
      },
    });
}

// ── Listas ──────────────────────────────────────────────────────────────────
function listSeg() { return localStorage.getItem("listSeg") || "Compras"; }

function itemEditor(item, listKey) {
  const other = S.lists.find((l) => l.key !== listKey);
  openSheet("Ítem",
    field("Texto", input("text", item.text, 'autofocus maxlength="200"')) +
    field("Para el día (opcional)", input("dueDate", item.dueDate || "", 'type="date"')) +
    field("Lista", select("list", S.lists.map((l) => [l.key, l.name]), listKey)) +
    '<label class="inline"><input type="checkbox" name="done" ' + (item.done ? "checked" : "") + "> Hecho</label>",
    {
      save: async (root) => {
        const text = val(root, "text").trim();
        if (!text) { sheetStatus("El texto no puede quedar vacío", "bad"); return false; }
        const dueDate = val(root, "dueDate") || null;
        const list = val(root, "list");
        const done = val(root, "done");
        await change(async () => {
          if (text !== item.text || done !== item.done) await api("/api/board/item", { id: item.id, text, done });
          if ((dueDate || null) !== (item.dueDate || null)) await api("/api/hub/edit", { kind: "item", id: item.id, action: "date", dueDate });
          if (list !== listKey) await api("/api/hub/edit", { kind: "item", id: item.id, action: "move", list });
        }, "Guardado");
        return true;
      },
      del: async () => {
        if (!sure("¿Borrar «" + item.text + "»?")) return false;
        await change(() => api("/api/hub/edit", { kind: "item", id: item.id, action: "delete" }), "Borrado");
        return true;
      },
    });
  void other;
}

function itemRow(it, listKey) {
  const sub = it.dueDate ? fmtWhen(it.dueDate) : "";
  return '<li class="' + (it.done ? "done" : "") + '"><button class="tick' + (it.done ? " on" : "") + '" data-act="item-tick" data-id="' + it.id + '" data-done="' + (it.done ? 1 : 0) + '" aria-label="Hecho"><i></i></button>' +
    '<div class="body" data-act="item-open" data-id="' + it.id + '" data-list="' + attr(listKey) + '"><span class="title">' + esc(it.text) + "</span>" + (sub ? '<span class="sub">' + esc(sub) + "</span>" : "") + "</div><span class=\"chev\">›</span></li>";
}

function listsView() {
  const key = listSeg();
  const list = S.lists.find((l) => l.key === key) || S.lists[0];
  const pending = list.items.filter((i) => !i.done);
  const done = list.items.filter((i) => i.done);
  let html = '<div class="seg">' + S.lists.map((l) => '<button class="' + (l.key === list.key ? "on" : "") + '" data-act="list-seg" data-key="' + attr(l.key) + '">' + esc(l.name) + " (" + l.items.filter((i) => !i.done).length + ")</button>").join("") + "</div>";
  html += '<div class="card"><form class="addbar" data-form="item-add" data-list="' + attr(list.key) + '"><input name="text" placeholder="' + (list.key === "Compras" ? "Leche, pan…" : "Llamar al banco…") + '" maxlength="200" autocomplete="off"><button>Agregar</button></form>';
  html += '<ul class="rows">';
  if (!pending.length) html += '<li class="empty">Nada pendiente en ' + esc(list.name.toLowerCase()) + ".</li>";
  html += pending.map((i) => itemRow(i, list.key)).join("");
  if (done.length) {
    html += '<li class="head">Hechos (' + done.length + ') <button class="link small" data-act="items-clear" data-key="' + attr(list.key) + '">limpiar</button></li>' + done.map((i) => itemRow(i, list.key)).join("");
  }
  html += "</ul></div>";
  return html;
}

// ── Notas ───────────────────────────────────────────────────────────────────
function noteTitle(text) {
  const first = (text || "").split("\n").find((l) => l.trim()) || "";
  return first.trim().slice(0, 80) || "(vacía)";
}

function noteEditor(n) {
  const isNew = !n;
  openSheet(isNew ? "Nota nueva" : "Nota",
    '<textarea name="text" autofocus maxlength="20000" style="min-height:40vh">' + esc(n ? n.text : "") + "</textarea>" +
    (n ? '<p class="hint" style="margin-top:6px">' + esc(fmtStamp(n.createdAt)) + "</p>" : ""),
    {
      save: async (root) => {
        const text = val(root, "text").trim();
        if (!text) { sheetStatus("La nota está vacía", "bad"); return false; }
        await change(() => api("/api/board/note", { id: isNew ? undefined : n.id, text }), "Guardada");
        return true;
      },
      del: isNew ? null : async () => {
        if (!sure("¿Borrar la nota «" + noteTitle(n.text) + "»?")) return false;
        await change(() => api("/api/hub/edit", { kind: "note", id: n.id, action: "delete" }), "Borrada");
        return true;
      },
    });
}

function notesView() {
  let html = '<div class="card"><h2><span class="grow">Notas</span><span class="count">' + S.notes.length + "</span></h2>";
  html += '<p class="hint">Lo que dictas al aparato como nota llega acá. Toca una para leerla entera o cambiarla.</p><ul class="rows">';
  if (!S.notes.length) html += '<li class="empty">Todavía no hay notas.</li>';
  for (const n of S.notes) {
    const rest = n.text.replace(/^[^\n]*\n?/, "").replace(/\s+/g, " ").trim().slice(0, 90);
    html += '<li><div class="body" data-act="note-open" data-id="' + n.id + '"><span class="title">' + esc(noteTitle(n.text)) + '</span><span class="sub">' + esc([fmtStamp(n.createdAt), rest].filter(Boolean).join(" · ")) + "</span></div><span class=\"chev\">›</span></li>";
  }
  html += "</ul></div>";
  return html;
}

// ── Hoy ─────────────────────────────────────────────────────────────────────
async function loadDay(date) {
  if (DAY[date]) return DAY[date];
  const r = await api("/api/calendar/day?date=" + date + "&lang=es");
  DAY[date] = r.items;
  if (r.date) S.today = r.date;
  return r.items;
}

function hoyView() {
  const t = todayIso();
  const dev = S.device || {};
  const w = S.weather || {};
  let html = "";

  // Clima
  html += '<div class="card">';
  if (S.place) {
    html += '<div style="display:flex;gap:12px;align-items:center"><div class="grow"><div class="bigtemp">' + esc(w.line || "—") + '</div><div class="muted">' + esc(w.detail || "") + "</div></div>" +
      '<div class="right muted" style="font-size:13px">' + esc(S.place.label || S.place.name) + "<br>" + fmtDayLong(t) + "</div></div>";
    if (w.error) html += '<p class="hint bad" style="margin-top:8px">' + esc(w.error) + "</p>";
  } else {
    html += '<h2>Clima</h2><p class="hint">Falta el lugar. <a href="#mas/ajustes">Cárgalo en Ajustes</a> y el aparato lo muestra en el hub.</p>';
  }
  html += "</div>";

  // Aparato
  const sync = dev.lastFetch ? ago(dev.lastFetch) : (dev.logAt ? "log subido " + ago(new Date(dev.logAt).getTime()) : "sin datos todavía");
  html += '<div class="card"><h2><span class="grow">Aparato</span><a class="muted" href="#mas/log" style="font-size:14px;font-weight:500">Log ›</a></h2><div class="status">' +
    '<div><div class="k">Última sincronización</div><div class="v">' + esc(sync) + "</div></div>" +
    '<div><div class="k">Firmware</div><div class="v">' + esc(dev.firmware || "—") + "</div></div>" +
    '<div><div class="k">Último arranque</div><div class="v">' + esc(dev.wake || "—") + "</div></div>" +
    '<div><div class="k">Idioma · voz</div><div class="v">' + esc((S.settings.lang || "es").toUpperCase() + " · " + ({ none: "muda", short: "corta", all: "habla" }[S.settings.speak] || "")) + "</div></div>" +
    "</div>";
  if (!dev.lastFetch) html += '<p class="hint" style="margin-top:8px">El aparato sincroniza solo al abrir el hub cada 6 h, o al mantener Atrás en el hub. Lo que cambies acá lo toma en la próxima.</p>';
  html += "</div>";

  // Recordatorios próximos
  const next = S.reminders.slice(0, 5);
  html += '<div class="card"><h2><span class="grow">Recordatorios</span><a class="muted" href="#agenda" style="font-size:14px;font-weight:500">Todos (' + S.reminders.length + ") ›</a></h2><ul class=\"rows\">";
  if (!next.length) html += '<li class="empty">Nada pendiente.</li>';
  html += next.map((r) => reminderRow(r)).join("") + "</ul></div>";

  // Agenda de hoy
  const items = DAY[t];
  html += '<div class="card"><h2><span class="grow">Hoy en la agenda</span><a class="muted" href="#agenda/cal" style="font-size:14px;font-weight:500">Calendario ›</a></h2><ul class="rows">';
  if (!items) { html += '<li class="empty">Cargando…</li>'; loadDay(t).then(() => { if (screenId() === "hoy") render(); }).catch(() => {}); }
  else if (!items.length) html += '<li class="empty">Nada agendado para hoy.</li>';
  else html += items.filter((o) => o.kind !== "reminder").map(occurrenceRow).join("") || '<li class="empty">Solo los recordatorios de arriba.</li>';
  html += "</ul></div>";

  // Resumen
  const shop = S.lists.find((l) => l.key === "Compras") || { items: [] };
  const tasks = S.lists.find((l) => l.key === "Tareas") || { items: [] };
  html += '<div class="tiles">' +
    '<a class="tile" href="#listas" data-act="seg-go" data-key="Compras"><div class="n">' + shop.items.filter((i) => !i.done).length + '</div><div class="t">compras pendientes</div></a>' +
    '<a class="tile" href="#listas" data-act="seg-go" data-key="Tareas"><div class="n">' + tasks.items.filter((i) => !i.done).length + '</div><div class="t">tareas pendientes</div></a>' +
    '<a class="tile" href="#notas"><div class="n">' + S.notes.length + '</div><div class="t">notas</div></a>' +
    '<a class="tile" href="#mas/memoria"><div class="n">' + S.memories.length + '</div><div class="t">cosas que recuerda de ti</div></a>' +
    "</div>";

  if (S.usage && S.usage.quotasOn) {
    const u = S.usage;
    html += '<div class="card" style="margin-top:12px"><h2>Uso del mes</h2><div class="status">' +
      '<div><div class="k">Consultas</div><div class="v">' + u.llmCalls + (u.limits.llmCalls ? " / " + u.limits.llmCalls : "") + "</div></div>" +
      '<div><div class="k">Audio transcrito</div><div class="v">' + Math.round(u.sttSeconds / 60) + " min" + (u.limits.sttSeconds ? " / " + Math.round(u.limits.sttSeconds / 60) : "") + "</div></div></div></div>";
  }
  return html;
}

// ── Más ─────────────────────────────────────────────────────────────────────
function masView() {
  const item = (href, ic, label, sub) => '<li><a href="' + href + '"><span class="ic">' + ic + '</span><span class="lbl">' + esc(label) + (sub ? "<small>" + esc(sub) + "</small>" : "") + '</span><span class="chev">›</span></a></li>';
  let html = '<ul class="menu">' +
    item("#mas/fotos", "🖼", "Fotos", "Las que ve el aparato en su álbum") +
    item("#mas/noticias", "📰", "Noticias", S.feeds.length ? S.feeds.length + " feed" + (S.feeds.length === 1 ? "" : "s") : "Ningún feed cargado") +
    item("#mas/viajes", "✈️", "Viajes", "Días, horarios y papeles") +
    item("#mas/memoria", "🧠", "Memoria del asistente", S.memories.length + " datos") +
    "</ul><ul class=\"menu\">" +
    item("#mas/ajustes", "⚙️", "Ajustes del aparato", "Idioma, voz, sonidos, volumen, clima") +
    (multi() ? item("#mas/aparatos", "📟", "Aparatos", (me.devices || []).length + " vinculado" + ((me.devices || []).length === 1 ? "" : "s")) : "") +
    (isAdmin() ? item("#mas/ia", "🤖", "Inteligencia artificial", "Proveedor, claves, costos") : "") +
    (isAdmin() ? item("#mas/contenido", "📦", "Paquete de contenido", "Lo que el aparato se baja a la tarjeta") : "") +
    item("#mas/log", "🧾", "Log del aparato", S.device.logAt ? "subido " + ago(new Date(S.device.logAt).getTime()) : "todavía no subió ninguno") +
    "</ul>";
  html += '<div class="card plain"><p class="muted">' + (multi() ? "Sesión: " + esc(me.email) : "Entraste con el token del aparato") + "</p>" +
    '<div class="btnrow">' + (multi() ? '<button class="ghost" data-act="logout">Cerrar sesión</button>' : '<button class="ghost" data-act="token-change">Cambiar el token</button>') + "</div></div>";
  return html;
}

// ── Fotos ───────────────────────────────────────────────────────────────────
let photos = null;

async function loadPhotos() {
  const r = await api("/api/photos");
  photos = r.photos;
  return photos;
}

function fotosView() {
  let html = '<div class="card"><p class="hint">Sube fotos tal como salen del teléfono: el servidor las pasa a 4 grises de 480x800 y el aparato las baja la próxima vez que entres a Fotos. Hasta 60; las más viejas se van solas.</p>' +
    '<input type="file" id="photoFiles" accept="image/*" multiple hidden><button class="wide" data-act="photo-pick">Subir fotos</button><p class="muted" id="photoStatus" style="margin-top:8px"></p></div>';
  if (!photos) { html += '<p class="loading">Cargando…</p>'; loadPhotos().then(render).catch((e) => toast(e.message)); return html; }
  if (!photos.length) return html + '<p class="loading">Todavía no hay fotos.</p>';
  html += '<div class="grid">' + photos.map((p) => '<button class="ph" data-act="photo-open" data-id="' + attr(p.id) + '"><img loading="lazy" src="' + photoSrc(p.id) + '" alt=""><span>' + esc(p.name) + "</span></button>").join("") + "</div>";
  return html;
}

// Las imágenes se piden con <img>, que no manda cabeceras: con token va en la
// URL... no: el token NO tiene que ir en URLs (quedan en el historial). Se bajan
// con fetch y se muestran como blob.
const blobCache = {};
function photoSrc(id) {
  const key = "p:" + id;
  if (blobCache[key]) return blobCache[key];
  loadBlob("/api/photos/preview?id=" + encodeURIComponent(id), key);
  return "data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==";
}
async function loadBlob(url, key) {
  if (blobCache[key] === "loading") return;
  blobCache[key] = "loading";
  try {
    const r = await fetch(url, { headers: authHeaders(), credentials: "same-origin" });
    if (!r.ok) throw new Error("http " + r.status);
    blobCache[key] = URL.createObjectURL(await r.blob());
    qsa(document, "img[data-blob='" + key + "']").forEach((img) => { img.src = blobCache[key]; });
    if (screenId() === "mas") render();
  } catch (e) { delete blobCache[key]; }
}

async function uploadPhotos(files) {
  const st = $("photoStatus");
  let n = 0;
  for (const f of files) {
    if (st) st.textContent = "Subiendo " + f.name + " (" + kb(f.size) + ")…";
    try {
      const r = await fetch("/api/board/photo?name=" + encodeURIComponent(f.name.replace(/\.[^.]+$/, "")), {
        method: "POST", headers: Object.assign({ "Content-Type": f.type || "application/octet-stream" }, authHeaders()), credentials: "same-origin", body: f,
      });
      const j = await r.json();
      if (!r.ok || !j.ok) throw new Error(j.error || "http " + r.status);
      n++;
    } catch (e) { toast("No se pudo subir " + f.name + ": " + e.message, 4000); }
  }
  if (st) st.textContent = n ? n + " foto" + (n === 1 ? "" : "s") + " lista" + (n === 1 ? "" : "s") + ". El aparato las baja al entrar a Fotos." : "";
  photos = null;
  render();
}

function photoSheet(p) {
  const key = "p:" + p.id;
  openSheet(p.name,
    '<img class="preview" data-blob="' + key + '" src="' + photoSrc(p.id) + '" alt="">' +
    '<p class="muted">' + esc(fmtStamp(p.at)) + " · " + kb(p.size) + " en el aparato</p>",
    {
      del: async () => {
        if (!sure("¿Borrar la foto «" + p.name + "»?")) return false;
        await api("/api/photos/delete", { id: p.id });
        photos = null;
        toast("Borrada");
        render();
        return true;
      },
    });
}

// ── Noticias ────────────────────────────────────────────────────────────────
function noticiasView() {
  let html = '<div class="card"><h2>Feeds</h2><p class="hint">Pega la dirección del diario o del feed (RSS o Atom). Se prueba antes de guardarlo. El aparato los lee en Noticias.</p>' +
    '<form data-form="feed-add"><div class="addbar"><input name="url" placeholder="https://diario.com/rss" autocomplete="off" inputmode="url"><button>Agregar</button></div><input name="name" placeholder="Nombre (opcional)" maxlength="40" style="margin-top:-2px"></form>' +
    '<ul class="rows" style="margin-top:8px">';
  if (!S.feeds.length) html += '<li class="empty">No hay feeds cargados.</li>';
  for (const f of S.feeds) html += '<li><span class="kind">📰</span><div class="body" data-act="feed-open" data-id="' + f.id + '"><span class="title">' + esc(f.name) + '</span><span class="sub ellip">' + esc(f.url) + "</span></div><span class=\"chev\">›</span></li>";
  html += "</ul></div>";
  if (S.feeds.length) {
    html += '<div class="card"><h2><span class="grow">Titulares</span>' + (rssCache ? '<button class="ghost small" data-act="rss-reload">Actualizar</button>' : '<button class="ghost small" data-act="rss-load">Ver</button>') + "</h2>";
    if (rssCache === "loading") html += '<p class="loading">Bajando los feeds…</p>';
    else if (rssCache) {
      for (const f of rssCache.feeds) {
        html += "<h3 style=\"margin-top:10px\">" + esc(f.name) + "</h3>";
        if (f.error) html += '<p class="hint bad">' + esc(f.error) + "</p>";
        html += '<ul class="rows">' + f.items.slice(0, 8).map((it) => '<li><div class="body"><a href="' + attr(it.link) + '" target="_blank" rel="noopener" style="text-decoration:none"><span class="title">' + esc(it.title) + '</span><span class="sub">' + esc(it.when || "") + "</span></a></div></li>").join("") + "</ul>";
      }
    } else html += '<p class="hint">Lo mismo que va a ver el aparato, para comprobar que los feeds traen algo.</p>';
    html += "</div>";
  }
  return html;
}

function feedSheet(f) {
  openSheet(f.name,
    field("Nombre", input("name", f.name, 'maxlength="40" autofocus')) +
    '<p class="muted mono" style="word-break:break-all;font-size:13px">' + esc(f.url) + "</p>" +
    '<button class="ghost wide" data-act="feed-test" data-id="' + f.id + '">Probar el feed</button>',
    {
      save: async (root) => {
        const name = val(root, "name").trim();
        if (!name) { sheetStatus("Ponle un nombre", "bad"); return false; }
        await change(() => api("/api/board/feed", { id: f.id, name }), "Guardado");
        return true;
      },
      del: async () => {
        if (!sure("¿Sacar el feed «" + f.name + "»?")) return false;
        rssCache = null;
        await change(() => api("/api/hub/edit", { kind: "feed", id: f.id, action: "delete" }), "Sacado");
        return true;
      },
    });
}

// ── Memoria ─────────────────────────────────────────────────────────────────
function memoriaView() {
  let html = '<div class="card"><h2>Memoria del asistente</h2><p class="hint">Lo que el aparato sabe de ti y usa para contestarte: «soy vegetariano», «mi hija se llama Ana». Entra en cada pregunta, en el hub y leyendo. Se carga acá o diciéndole «recuerda que…». Hasta 40 datos.</p>' +
    '<form class="addbar" data-form="memory-add"><input name="text" placeholder="Recuerda que…" maxlength="300" autocomplete="off"><button>Agregar</button></form><ul class="rows">';
  if (!S.memories.length) html += '<li class="empty">Todavía no recuerda nada.</li>';
  for (const m of S.memories.slice().reverse()) html += '<li><div class="body" data-act="memory-open" data-id="' + m.id + '"><span class="title">' + esc(m.text) + '</span><span class="sub">' + esc(fmtStamp(m.createdAt)) + "</span></div><span class=\"chev\">›</span></li>";
  return html + "</ul></div>";
}

function memorySheet(m) {
  openSheet("Dato",
    '<textarea name="text" autofocus maxlength="300" style="min-height:90px">' + esc(m.text) + "</textarea>",
    {
      save: async (root) => {
        const text = val(root, "text").trim();
        if (!text) { sheetStatus("No puede quedar vacío", "bad"); return false; }
        await change(() => api("/api/board/memory", { id: m.id, text }), "Guardado");
        return true;
      },
      del: async () => {
        if (!sure("¿Olvidar «" + m.text + "»?")) return false;
        await change(() => api("/api/hub/edit", { kind: "memory", id: m.id, action: "delete" }), "Olvidado");
        return true;
      },
    });
}

// ── Ajustes ─────────────────────────────────────────────────────────────────
const LANGS = [["es", "Español"], ["en", "English"], ["fr", "Français"], ["de", "Deutsch"], ["pt", "Português"], ["ru", "Русский"]];
let placeResults = null;

function ajustesView() {
  const s = S.settings;
  let html = '<div class="card"><h2>Aparato</h2><p class="hint">Lo toma en la próxima sincronización (al abrir el hub, o manteniendo Atrás en el hub). Lo que cambies en el propio aparato no se pisa.</p>' +
    '<form data-form="settings">' +
    field("Idioma del aparato", select("lang", LANGS, s.lang)) +
    field("Voz hablada", select("speak", [["none", "Nunca: solo escribe"], ["short", "Respuestas cortas"], ["all", "Siempre"]], s.speak)) +
    field("Sonidos de la interfaz", select("uiSound", [["off", "Apagados"], ["soft", "Suaves"], ["normal", "Normales"]], s.uiSound || "normal")) +
    field("Traductor: el otro idioma", select("translatorLang", LANGS, s.translatorLang)) +
    field("Volumen (voz, música y avisos): <b class=\"volOut\">" + s.musicVolume + " %</b>", '<input type="range" name="musicVolume" min="0" max="100" step="5" value="' + s.musicVolume + '">', true) +
    '<button class="wide">Guardar ajustes</button></form></div>';

  html += '<div class="card"><h2>Lugar del clima</h2>';
  html += '<p class="muted">' + (S.place ? "<b>" + esc(S.place.label || S.place.name) + "</b> · zona horaria " + esc(S.place.timezone) : "Sin lugar: el hub muestra el clima vacío y las horas salen en la zona del servidor.") + "</p>";
  html += '<form class="addbar" data-form="place-search"><input name="q" placeholder="Ciudad" autocomplete="off"><button>Buscar</button></form>';
  if (placeResults === "loading") html += '<p class="loading">Buscando…</p>';
  else if (placeResults) {
    html += '<ul class="rows">' + (placeResults.length ? placeResults.map((p, i) => '<li><div class="body" data-act="place-pick" data-i="' + i + '"><span class="title">' + esc(p.label) + '</span><span class="sub">' + esc(p.timezone) + "</span></div><span class=\"chev\">›</span></li>").join("") : '<li class="empty">No se encontró nada con ese nombre.</li>') + "</ul>";
  }
  html += "</div>";
  return html;
}

// ── Aparatos (multiusuario) ─────────────────────────────────────────────────
function aparatosView() {
  const devs = (me && me.devices) || [];
  let html = '<div class="card"><h2>Vincular un aparato</h2><p class="hint">En el aparato: <b>Ajustes → Sistema → Vincular con mi cuenta</b>. Muestra un código de 6 dígitos que dura 10 minutos. Escríbelo acá.</p>' +
    '<form data-form="pair"><div class="two">' + field("Código", input("code", "", 'inputmode="numeric" pattern="[0-9]*" maxlength="6" placeholder="123456"')) + field("Nombre (opcional)", input("name", "", 'maxlength="60" placeholder="El de la cocina"')) + "</div><button class=\"wide\">Vincular</button></form></div>";
  html += '<div class="card"><h2>Mis aparatos</h2><ul class="rows">';
  if (!devs.length) html += '<li class="empty">Todavía no hay ninguno vinculado.</li>';
  for (const d of devs) html += '<li><span class="kind">📟</span><div class="body" data-act="dev-open" data-id="' + attr(d.deviceId) + '"><span class="title">' + esc(d.name || d.deviceId) + '</span><span class="sub">' + esc(d.deviceId + " · " + (d.lastSeen ? "visto " + ago(new Date(d.lastSeen).getTime()) : "todavía no se conectó")) + "</span></div><span class=\"chev\">›</span></li>";
  html += "</ul></div>";
  html += '<div class="card"><h2>Contraseña</h2><form data-form="password">' + field("Actual", input("current", "", 'type="password" autocomplete="current-password"')) + field("Nueva (8 o más)", input("password", "", 'type="password" autocomplete="new-password" minlength="8"')) + '<button class="wide ghost">Cambiar la contraseña</button></form></div>';
  return html;
}

function deviceSheet(d) {
  openSheet(d.name || d.deviceId,
    field("Nombre", input("name", d.name || "", 'maxlength="60" autofocus')) + '<p class="muted mono">' + esc(d.deviceId) + "</p>",
    {
      save: async (root) => {
        await api("/api/account/device/rename", { deviceId: d.deviceId, name: val(root, "name").trim() });
        await loadMe();
        render();
        toast("Guardado");
        return true;
      },
      del: async () => {
        if (!sure("¿Desvincular este aparato? Va a dejar de sincronizar hasta que lo vincules de nuevo.")) return false;
        await api("/api/account/device/delete", { deviceId: d.deviceId });
        await loadMe();
        render();
        toast("Desvinculado");
        return true;
      },
      delLabel: "Desvincular",
    });
}

// ── IA (solo admin) ─────────────────────────────────────────────────────────
let costs = null;
let aiTest = "";

async function loadConfig() {
  const r = await api("/api/board/config");
  cfg = r.config;
  return cfg;
}

function presetOf(c) {
  const presets = c.presets || {};
  let current = "anthropic";
  for (const k in presets) {
    const p = presets[k];
    if (p.provider === c.llm.provider && (p.provider === "anthropic" || p.baseUrl === c.llm.baseUrl)) current = k;
  }
  return current;
}

function iaView() {
  if (!cfg) { loadConfig().then(render).catch((e) => toast(e.message)); return '<p class="loading">Cargando…</p>'; }
  const presets = cfg.presets || {};
  const cur = presetOf(cfg);
  const p = presets[cur] || { models: [] };
  const models = p.models.slice();
  if (cfg.llm.model && models.indexOf(cfg.llm.model) < 0) models.unshift(cfg.llm.model);
  const se = cfg.search || { enabled: false, provider: "free", maxUses: 3, hasKey: false };
  const keyState = (has, alt) => '<span class="' + (has ? "ok" : "muted") + '" style="font-size:13px"> · ' + (has ? "clave puesta" : alt || "sin clave") + "</span>";
  let html = '<form data-form="ai">';
  html += '<div class="card"><h2>Modelo de texto</h2><p class="hint">El que entiende lo que dices, responde y traduce. Se cambia sin tocar el aparato.</p>' +
    field("Proveedor", select("preset", Object.keys(presets).map((k) => [k, presets[k].label]), cur)) +
    '<div class="llmBase" ' + (p.provider === "anthropic" ? "hidden" : "") + ">" + field("URL", input("llmBase", cfg.llm.baseUrl || "", 'placeholder="https://api.groq.com/openai/v1" inputmode="url"')) + "</div>" +
    field("Modelo", select("llmModel", models.map((m) => [m, m]), cfg.llm.model)) +
    field("Clave" + keyState(cfg.llm.hasKey), input("llmKey", "", 'type="password" placeholder="vacía = no cambiarla" autocomplete="off"'), true) +
    "</div>";
  html += '<div class="card"><h2>Transcripción de voz</h2><p class="hint">Lo que pasa tu voz a texto. Groq (whisper-large-v3-turbo) es gratis y rápido.</p>' +
    field("URL", input("sttBase", cfg.stt.baseUrl, 'inputmode="url"')) + field("Modelo", input("sttModel", cfg.stt.model)) +
    field("Clave" + keyState(cfg.stt.hasKey), input("sttKey", "", 'type="password" placeholder="vacía = no cambiarla" autocomplete="off"'), true) +
    '<label class="inline"><input type="checkbox" name="sttFollow" checked> Seguir al proveedor de texto</label></div>';
  html += '<div class="card"><h2>Buscar en internet</h2><p class="hint">Para que conteste cosas de ahora. Con Claude busca el modelo (US$ 0,01 por búsqueda); con los demás busca este servidor: gratis con Google Noticias y DuckDuckGo, o con una clave de Tavily o Brave.</p>' +
    '<label class="inline"><input type="checkbox" name="searchOn" ' + (se.enabled ? "checked" : "") + "> Buscar cuando haga falta</label>" +
    field("Buscador", select("searchProvider", [["free", "Gratis (Google Noticias + DuckDuckGo)"], ["tavily", "Tavily (con clave)"], ["brave", "Brave (con clave)"]], se.provider)) +
    '<div class="two">' + field("Máximo por respuesta", input("searchMax", se.maxUses, 'type="number" min="1" max="10"')) + field("Clave" + keyState(se.hasKey, "sin clave"), input("searchKey", "", 'type="password" placeholder="vacía = no cambiarla" autocomplete="off"'), true) + "</div></div>";
  html += '<div class="btnrow"><button>Guardar</button><button type="button" class="ghost" data-act="ai-test">Probar</button></div>' +
    (aiTest ? '<pre class="muted" style="white-space:pre-wrap;font-size:13px">' + esc(aiTest) + "</pre>" : "") + "</form>";

  html += '<div class="card" style="margin-top:12px"><h2><span class="grow">Cuánto sale cada consulta</span>' + (costs ? "" : '<button class="ghost small" data-act="costs-load">Ver</button>') + "</h2>";
  if (costs === "loading") html += '<p class="loading">Cargando…</p>';
  else if (costs) {
    const usd = (v) => "US$ " + v.toFixed(4).replace(".", ",");
    html += '<p class="hint">Una consulta típica: ' + costs.shape.seconds + " s de audio, " + costs.shape.inTokens + " tokens de entrada y " + costs.shape.outTokens + " de salida. Incluye la transcripción con <b>" + esc(costs.stt) + "</b>.</p>" +
      '<div class="scroll"><table class="t"><tr><th>Modelo</th><th class="num">Modelo</th><th class="num">Voz</th><th class="num">Total</th></tr>' +
      costs.rows.map((x) => '<tr class="' + (x.current ? "on" : "") + '"><td>' + esc(x.model) + (x.current ? " ←" : "") + (x.note ? '<br><small class="muted">' + esc(x.note) + "</small>" : "") + '</td><td class="num">' + usd(x.llm) + '</td><td class="num">' + usd(x.stt) + '</td><td class="num">' + usd(x.total) + "</td></tr>").join("") + "</table></div>" +
      '<p class="hint" style="margin-top:8px">DeepSeek cobra el doble en hora pico: ahora está ' + (costs.deepSeekPeakNow ? "<b>en pico</b>" : "<b>fuera de pico</b>") + ". Una búsqueda con Claude suma US$ " + String(costs.searchAnthropic).replace(".", ",") + ".</p>";
  } else html += '<p class="hint">Precio por consulta de voz con cada modelo, con el precio de la transcripción incluido.</p>';
  html += "</div>";

  html += '<div class="card"><h2>Token del aparato</h2><p class="hint">' + (cfg.deviceTokenSet ? "Hay un token propio guardado además del del entorno." : "Se usa el token del entorno.") + " El del entorno vale siempre, así que no puedes quedarte afuera. El nuevo hay que ponerlo también en el aparato → Servidor.</p>" +
    '<form class="addbar" data-form="token-set"><input name="token" placeholder="token nuevo (8 o más)" autocomplete="off"><button class="danger">Cambiar</button></form></div>';
  return html;
}

async function saveAi(root) {
  const preset = (cfg.presets || {})[val(root, "preset")] || {};
  const body = {
    llm: { provider: preset.provider, baseUrl: val(root, "llmBase"), model: val(root, "llmModel"), key: val(root, "llmKey") },
    stt: { baseUrl: val(root, "sttBase"), model: val(root, "sttModel"), key: val(root, "sttKey") },
    search: { enabled: val(root, "searchOn"), provider: val(root, "searchProvider"), maxUses: Number(val(root, "searchMax")), key: val(root, "searchKey") },
  };
  busy(true);
  try {
    await api("/api/board/config", body);
    cfg = null; costs = null;
    await loadConfig();
    render();
    toast("Proveedor guardado");
  } catch (e) { toast("No se pudo: " + e.message, 4000); } finally { busy(false); }
}

// Cambiar de proveedor en el formulario: rellena modelo, URL y transcripción sin guardar nada.
function onPresetChange(root) {
  const key = val(root, "preset");
  const p = (cfg.presets || {})[key] || { models: [] };
  const sel = qs(root, "[name=llmModel]");
  sel.innerHTML = p.models.map((m) => "<option>" + esc(m) + "</option>").join("");
  qs(root, ".llmBase").hidden = p.provider === "anthropic";
  if (p.provider !== "anthropic" && p.baseUrl) qs(root, "[name=llmBase]").value = p.baseUrl;
  if (p.sttBaseUrl && val(root, "sttFollow")) {
    qs(root, "[name=sttBase]").value = p.sttBaseUrl;
    qs(root, "[name=sttModel]").value = (p.sttModels || [])[0] || val(root, "sttModel");
  }
}

// ── Paquete de contenido ────────────────────────────────────────────────────
let assets = null;

function contenidoView() {
  let html = '<div class="card"><h2>Paquete de contenido</h2><p class="hint">Lo pesado (la Biblia entera, dibujos, sonidos) se genera una sola vez en el servidor y el aparato se lo baja completo después de actualizar el firmware o al sincronizar si falta algo.</p>';
  if (assets === "loading" || !assets) { if (!assets) { assets = "loading"; api("/api/assets/status").then((r) => { assets = r; render(); }).catch((e) => { assets = { langs: [] }; toast(e.message); }); } html += '<p class="loading">Cargando…</p></div>'; return html; }
  const mb = (b) => (b / 1048576).toFixed(1) + " MB";
  if (!assets.langs.length) html += '<p class="muted">Todavía no se generó nada.</p>';
  else html += '<div class="scroll"><table class="t"><tr><th>Idioma</th><th>Versión</th><th class="num">Archivos</th><th class="num">Tamaño</th><th>Estado</th></tr>' +
    assets.langs.map((l) => "<tr><td>" + esc(l.lang) + "</td><td>" + esc(l.version) + '</td><td class="num">' + l.files + '</td><td class="num">' + mb(l.bytes) + "</td><td>" + (l.building ? "generando " + l.done + "/" + l.total : "listo") + (l.error ? ' <span class="bad">' + esc(l.error) + "</span>" : "") + "</td></tr>").join("") + "</table></div>";
  html += '<div class="btnrow"><button class="ghost" data-act="assets-reload">Actualizar</button><button class="ghost" data-act="assets-build">Generar lo que falte</button></div></div>';
  return html;
}

// ── Log ─────────────────────────────────────────────────────────────────────
let logText = null;

function logView() {
  const dev = S.device || {};
  let html = '<div class="card"><p class="hint">Lo que el aparato sube en cada sincronización (el final de su log de la tarjeta). Última subida: <b>' + esc(dev.logAt ? fmtStamp(dev.logAt) : "nunca") + "</b>" + (dev.firmware ? " · firmware " + esc(dev.firmware) : "") + "</p>" +
    '<div class="btnrow"><button class="ghost" data-act="log-reload">Actualizar</button><button class="ghost" data-act="log-copy">Copiar</button><button class="danger" data-act="log-clear">Vaciar</button></div></div>';
  if (logText === null) { logText = "loading"; apiText("/api/log").then((t) => { logText = t; render(); }).catch((e) => { logText = "(no se pudo leer: " + e.message + ")"; render(); }); }
  html += '<pre class="log" id="logBox">' + (logText === "loading" ? "Cargando…" : esc(logText)) + "</pre>";
  return html;
}

async function copyText(text) {
  try { await navigator.clipboard.writeText(text); }
  catch (e) {
    const a = document.createElement("textarea");
    a.value = text; document.body.appendChild(a); a.select();
    try { document.execCommand("copy"); } catch (e2) {}
    a.remove();
  }
  toast("Copiado");
}

// ── Viajes ──────────────────────────────────────────────────────────────────
const TRIP_KINDS = [["flight", "Vuelo", "✈️"], ["train", "Tren", "🚆"], ["hotel", "Hotel", "🏨"], ["ticket", "Entrada", "🎟"], ["meal", "Comida", "🍽"], ["visit", "Visita", "📍"], ["other", "Otro", "•"]];
function kindIcon(k) { const f = TRIP_KINDS.find((x) => x[0] === k); return f ? f[2] : "•"; }
let trips = null;

async function loadTrips() { trips = (await api("/api/trips?lang=es")).trips; return trips; }
async function loadTrip(id) { const r = await api("/api/trip?id=" + encodeURIComponent(id) + "&lang=es"); tripCache[id] = r.trip; return r.trip; }
function forgetTrips() { trips = null; tripCache = {}; clearCal(); }

function viajesView() {
  if (!trips) { loadTrips().then(render).catch((e) => toast(e.message)); return '<p class="loading">Cargando…</p>'; }
  let html = '<div class="card"><h2>Viajes</h2><p class="hint">Un viaje son sus días, lo que se hace cada día con su hora, y los papeles (el PDF del vuelo, la reserva). Todo aparece también en el calendario del aparato.</p><ul class="rows">';
  if (!trips.length) html += '<li class="empty">Todavía no hay viajes. Con el + de abajo creas uno.</li>';
  for (const t of trips) {
    const state = t.state === "now" ? "en curso" : t.state === "past" ? "pasado" : "";
    html += '<li><span class="kind">✈️</span><div class="body" data-act="trip-go" data-trip="' + attr(t.id) + '"><span class="title">' + esc(t.name) + '</span><span class="sub">' + esc([t.when, t.place, t.items + " cosas", state].filter(Boolean).join(" · ")) + "</span></div><span class=\"chev\">›</span></li>";
  }
  return html + "</ul></div>";
}

function tripEditor(t) {
  const isNew = !t;
  openSheet(isNew ? "Viaje nuevo" : "Viaje",
    field("Nombre", input("name", t ? t.name : "", 'autofocus maxlength="80" placeholder="Roma"')) +
    field("Lugar", input("place", t ? t.place : "", 'maxlength="80"')) +
    '<div class="two">' + field("Ida", input("start", t ? t.start : "", 'type="date"')) + field("Vuelta", input("end", t ? t.end : "", 'type="date"')) + "</div>",
    {
      save: async (root) => {
        const body = { id: t ? t.id : undefined, name: val(root, "name").trim(), place: val(root, "place").trim(), start: val(root, "start"), end: val(root, "end") };
        if (!body.name || !body.start || !body.end) { sheetStatus("Faltan el nombre o las fechas", "bad"); return false; }
        busy(true);
        try {
          const r = await api("/api/trip", body);
          forgetTrips();
          location.hash = "#mas/viajes/" + r.id;
          toast("Guardado");
          return true;
        } catch (e) { sheetStatus(e.message, "bad"); return false; } finally { busy(false); }
      },
      del: isNew ? null : async () => {
        if (!sure("¿Borrar el viaje «" + t.name + "» con sus días y sus papeles?")) return false;
        await api("/api/trip/delete", { id: t.id });
        forgetTrips();
        location.hash = "#mas/viajes";
        toast("Borrado");
        return true;
      },
    });
}

function attRow(a, ctx) {
  const bits = [];
  if (a.codes && a.codes.length) { const c = a.codes[0]; bits.push(c.copy ? "código copiado (puede no escanear)" : c.format + (c.verified ? " verificado" : "")); }
  bits.push(a.pages + (a.pages === 1 ? " página" : " páginas"));
  for (const f of (a.fields || []).slice(0, 3)) bits.push(f.label + ": " + f.value);
  return '<li><span class="kind">📄</span><div class="body" data-act="att-open" data-id="' + attr(a.id) + '" data-date="' + attr(ctx.date || "") + '" data-item="' + attr(ctx.itemId || "") + '"><span class="title">' + esc(a.name) + '</span><span class="sub">' + esc(bits.join(" · ")) + (a.warn ? " · " + esc(a.warn) : "") + "</span></div><span class=\"chev\">›</span></li>";
}

function tripView(id) {
  const t = tripCache[id];
  if (!t) { loadTrip(id).then(render).catch((e) => { toast(e.message); location.hash = "#mas/viajes"; }); return '<p class="loading">Cargando…</p>'; }
  let html = '<div class="card"><h2><span class="grow">' + esc(t.name) + '</span><button class="ghost small" data-act="trip-edit">Editar</button></h2><p class="muted">' + esc([t.when, t.place].filter(Boolean).join(" · ")) + "</p></div>";
  for (const d of t.days) {
    html += '<div class="card"><h2><span class="grow">' + esc(d.label) + '</span><button class="ghost small" data-act="titem-new" data-date="' + d.date + '">+</button></h2>';
    if (d.note) html += '<p class="hint">' + esc(d.note) + "</p>";
    html += '<ul class="rows">';
    if (!d.items.length) html += '<li class="empty">Nada cargado. <button class="link small" data-act="tday-note" data-date="' + d.date + '">Nota del día</button></li>';
    for (const it of d.items) {
      const sub = [it.at, it.kindLabel, it.place, it.attachments.length ? it.attachments.length + " papel" + (it.attachments.length === 1 ? "" : "es") : ""].filter(Boolean).join(" · ");
      html += '<li><span class="kind">' + kindIcon(it.kind) + '</span><div class="body" data-act="titem-open" data-date="' + d.date + '" data-id="' + attr(it.id) + '"><span class="title">' + esc(it.title) + '</span><span class="sub">' + esc(sub) + "</span></div><span class=\"chev\">›</span></li>";
    }
    html += "</ul></div>";
  }
  const targets = [["", "Papeles del viaje (sin ítem)"]];
  for (const d of t.days) for (const it of d.items) targets.push([d.date + "|" + it.id, fmtDay(d.date) + " · " + it.title]);
  html += '<div class="card"><h2>Papeles</h2><p class="hint">Sube el PDF del vuelo, la reserva o la entrada tal como llegó al correo. El servidor lo convierte a páginas que el aparato pinta y vuelve a generar el código de barras limpio para que escanee.</p>' +
    field("Colgar de", select("attTarget", targets, "", 'id="attTarget"')) +
    '<input type="file" id="attFile" accept="application/pdf,image/*" hidden><button class="wide ghost" data-act="att-pick">Subir un archivo</button><p class="muted" id="attStatus" style="margin-top:8px"></p>' +
    '<ul class="rows">' + (t.docs.length ? t.docs.map((a) => attRow(a, {})).join("") : '<li class="empty">Sin papeles sueltos.</li>') + "</ul></div>";
  html += '<div class="card"><h2>Para llevar</h2><form class="addbar" data-form="pack-add" data-trip="' + attr(t.id) + '"><input name="text" placeholder="Cargador, pasaporte…" maxlength="120" autocomplete="off"><button>Agregar</button></form><ul class="rows">';
  if (!t.packing.length) html += '<li class="empty">Nada anotado.</li>';
  for (const p of t.packing) html += '<li class="' + (p.done ? "done" : "") + '"><button class="tick' + (p.done ? " on" : "") + '" data-act="pack-tick" data-id="' + attr(p.id) + '" data-done="' + (p.done ? 1 : 0) + '"><i></i></button><div class="body" data-act="pack-open" data-id="' + attr(p.id) + '"><span class="title">' + esc(p.text) + "</span></div><span class=\"chev\">›</span></li>";
  html += "</ul></div>";
  html += '<div class="card"><h2><span class="grow">Sugerencias</span><button class="ghost small" data-act="trip-suggest">Pedir</button></h2><div id="tripSuggest" class="muted">Qué visitar cerca, cuánto se tarda entre los lugares y qué falta en la lista. Usa el modelo y busca en internet: cuenta contra el tope diario.</div></div>';
  return html;
}

function tripItemEditor(t, date, it) {
  const isNew = !it;
  openSheet(isNew ? "Agregar el " + fmtDay(date) : "Ítem del " + fmtDay(date),
    field("Qué", input("title", it ? it.title : "", 'autofocus maxlength="120" placeholder="Tren a Termini"')) +
    '<div class="two">' + field("Hora", input("at", it ? it.at : "", 'type="time"')) + field("Tipo", select("kind", TRIP_KINDS.map((k) => [k[0], k[1]]), it ? it.kind : "other")) + "</div>" +
    field("Día", input("date", date, 'type="date"')) +
    field("Dónde", input("place", it ? it.place : "", 'maxlength="100"')) +
    field("Nota", '<textarea name="note" maxlength="400" style="min-height:60px">' + esc(it ? it.note : "") + "</textarea>") +
    (it && it.attachments.length ? '<p class="muted" style="margin:0 0 4px 2px;font-size:13px">Papeles</p><ul class="rows">' + it.attachments.map((a) => attRow(a, { date, itemId: it.id })).join("") + "</ul>" : ""),
    {
      save: async (root) => {
        const body = { tripId: t.id, id: it ? it.id : undefined, date: val(root, "date"), at: val(root, "at"), kind: val(root, "kind"), title: val(root, "title").trim(), place: val(root, "place"), note: val(root, "note") };
        if (!body.title) { sheetStatus("Falta el título", "bad"); return false; }
        busy(true);
        try {
          if (it && body.date !== date) {
            // Cambió de día: se crea en el nuevo y se borra del viejo (los papeles se vuelven a colgar).
            const r = await api("/api/trip/day/item", Object.assign({}, body, { id: undefined, attachmentIds: it.attachmentIds }));
            await api("/api/trip/day/item/delete", { tripId: t.id, date, id: it.id });
            void r;
          } else await api("/api/trip/day/item", body);
          forgetTrips();
          render();
          toast("Guardado");
          return true;
        } catch (e) { sheetStatus(e.message, "bad"); return false; } finally { busy(false); }
      },
      del: isNew ? null : async () => {
        if (!sure("¿Borrar «" + it.title + "»?")) return false;
        await api("/api/trip/day/item/delete", { tripId: t.id, date, id: it.id });
        forgetTrips(); render(); toast("Borrado");
        return true;
      },
    });
}

function dayNoteEditor(t, date) {
  const d = t.days.find((x) => x.date === date) || { note: "" };
  openSheet("Nota del " + fmtDay(date), '<textarea name="note" autofocus maxlength="300" style="min-height:80px" placeholder="Día libre, hay que estar 2 h antes…">' + esc(d.note) + "</textarea>", {
    save: async (root) => {
      await api("/api/trip/day", { tripId: t.id, date, note: val(root, "note") });
      forgetTrips(); render();
      return true;
    },
  });
}

function packEditor(t, p) {
  openSheet("Para llevar", field("Cosa", input("text", p.text, 'autofocus maxlength="120"')) + '<label class="inline"><input type="checkbox" name="done" ' + (p.done ? "checked" : "") + "> Ya está</label>", {
    save: async (root) => {
      await api("/api/trip/packing", { tripId: t.id, id: p.id, text: val(root, "text").trim(), done: val(root, "done") });
      forgetTrips(); render();
      return true;
    },
    del: async () => {
      await api("/api/trip/packing", { tripId: t.id, id: p.id, action: "delete" });
      forgetTrips(); render();
      return true;
    },
  });
}

async function attachmentSheet(t, id, ctx) {
  let a = null;
  try { a = (await api("/api/attachment/info?id=" + encodeURIComponent(id))).attachment; } catch (e) { toast(e.message); return; }
  const key = "a:" + a.id + ":0";
  let html = '<img class="preview" data-blob="' + key + '" src="' + attSrc(a.id, 0, key) + '" alt="">';
  if (a.pages > 1) html += '<div class="chips">' + Array.from({ length: a.pages }, (_, i) => '<button type="button" class="chip' + (i ? "" : " on") + '" data-act="att-page" data-id="' + attr(a.id) + '" data-page="' + i + '">Página ' + (i + 1) + "</button>").join("") + "</div>";
  if (a.codes.length) html += '<p class="muted">' + a.codes.map((c) => (c.copy ? "Código copiado de la imagen: PUEDE NO ESCANEAR, lleva el original" : "Código " + c.format + (c.verified ? " leído y vuelto a generar (verificado)" : " leído"))).join(" · ") + "</p>";
  if (a.warn) html += '<p class="warn">' + esc(a.warn) + "</p>";
  if (a.fields.length) html += '<ul class="rows">' + a.fields.map((f) => '<li style="min-height:36px"><div class="body"><span class="sub">' + esc(f.label) + '</span><span class="title">' + esc(f.value) + "</span></div></li>").join("") + "</ul>";
  html += '<p class="muted" style="margin-top:8px">' + esc(a.name) + " · " + kb(a.bytes) + " · " + a.pages + " página" + (a.pages === 1 ? "" : "s") + "</p>";
  if (ctx.itemId) html += '<button class="ghost wide" data-act="att-unattach" data-id="' + attr(a.id) + '" data-date="' + attr(ctx.date) + '" data-item="' + attr(ctx.itemId) + '">Descolgar de este ítem</button>';
  openSheet("Papel", html, {
    del: async () => {
      if (!sure("¿Borrar el papel «" + a.name + "» con todas sus páginas?")) return false;
      await api("/api/attachment/delete", { id: a.id });
      forgetTrips(); render(); toast("Borrado");
      return true;
    },
  });
}

function attSrc(id, page, key) {
  if (blobCache[key] && blobCache[key] !== "loading") return blobCache[key];
  loadBlob("/api/attachment/preview?id=" + encodeURIComponent(id) + "&page=" + page, key);
  return "data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==";
}

async function uploadAttachment(t, file) {
  const st = $("attStatus");
  st.textContent = "Subiendo " + file.name + " (" + kb(file.size) + "). Se convierte en el servidor: puede tardar unos segundos.";
  const form = new FormData();
  form.append("file", file);
  try {
    const j = await api("/api/board/attachment?trip=" + encodeURIComponent(t.id), form);
    const a = j.attachment;
    const target = $("attTarget").value;
    if (target) { const [date, itemId] = target.split("|"); await api("/api/trip/attach", { tripId: t.id, date, itemId, attachmentId: a.id }); }
    else await api("/api/trip/attach", { tripId: t.id, attachmentId: a.id });
    const lines = [a.pages + " página" + (a.pages === 1 ? "" : "s") + " para el aparato"];
    if (a.codes.length) { const c = a.codes[0]; lines.push(c.copy ? "El código no se pudo leer: va como copia y PUEDE NO ESCANEAR" : "Código " + c.format + (c.verified ? " verificado" : "")); }
    else if (a.warn) lines.push(a.warn);
    forgetTrips();
    render();
    toast(lines.join(" · "), 5000);
  } catch (e) { st.textContent = "No se pudo subir: " + e.message; }
}

// ── Agregar rápido (el +) ───────────────────────────────────────────────────
function quickAdd(kind) {
  const KINDS = [["reminder", "Recordatorio"], ["task", "Tarea"], ["shop", "Compra"], ["note", "Nota"], ["event", "Evento"]];
  let cur = kind || "reminder";
  openSheet("Agregar",
    '<div class="chips">' + KINDS.map((k) => '<button type="button" class="chip' + (k[0] === cur ? " on" : "") + '" data-kind="' + k[0] + '">' + k[1] + "</button>").join("") + "</div>" +
    '<textarea name="text" autofocus maxlength="20000" style="min-height:80px" placeholder="Qué"></textarea>' +
    '<p class="hint" style="margin-top:6px">Un recordatorio o un evento siguen con la fecha y la hora; el resto se guarda directo.</p>',
    {
      saveLabel: "Seguir",
      onOpen: (root) => {
        root.addEventListener("click", (ev) => {
          const chip = ev.target.closest(".chip[data-kind]");
          if (!chip) return;
          qsa(root, ".chip[data-kind]").forEach((c) => c.classList.toggle("on", c === chip));
          cur = chip.dataset.kind;
          qs(root, "[data-act=sheet-save]").textContent = cur === "reminder" || cur === "event" ? "Seguir" : "Guardar";
        });
      },
      save: async (root) => {
        const text = val(root, "text").trim();
        if (!text) { sheetStatus("Escribe algo", "bad"); return false; }
        if (cur === "reminder") { closeSheet(); setTimeout(() => reminderEditor(null, text), 220); return false; }
        if (cur === "event") { closeSheet(); setTimeout(() => eventEditor(null, calSel || todayIso(), text), 220); return false; }
        if (cur === "note") await change(() => api("/api/board/note", { text }), "Nota guardada");
        else await change(() => api("/api/board/item", { list: cur === "shop" ? "Compras" : "Tareas", text }), cur === "shop" ? "Agregado a compras" : "Agregado a tareas");
        return true;
      },
    });
}

// ── Enrutador ───────────────────────────────────────────────────────────────
function parts() { return (location.hash.replace(/^#/, "") || "hoy").split("/"); }
function screenId() { return parts()[0]; }

const TITLES = { hoy: "Hoy", agenda: "Agenda", listas: "Listas", notas: "Notas", mas: "Más", fotos: "Fotos", noticias: "Noticias", viajes: "Viajes", memoria: "Memoria", ajustes: "Ajustes", aparatos: "Aparatos", ia: "Inteligencia artificial", contenido: "Contenido", log: "Log del aparato" };

function render() {
  if (!entered || !S) return;
  const p = parts();
  const main = $("main");
  let html = "";
  let title = TITLES[p[0]] || "Mi aparato";
  let back = null;
  let fab = true;
  try {
    if (p[0] === "hoy") html = hoyView();
    else if (p[0] === "agenda") {
      const seg = p[1] === "cal" ? "cal" : "rem";
      html = '<div class="seg"><a class="' + (seg === "rem" ? "on" : "") + '" href="#agenda" style="flex:1;text-align:center;text-decoration:none;padding:8px;border-radius:9px;font-weight:600;color:' + (seg === "rem" ? "var(--text)" : "var(--muted)") + (seg === "rem" ? ";background:var(--card)" : "") + '">Recordatorios</a><a class="' + (seg === "cal" ? "on" : "") + '" href="#agenda/cal" style="flex:1;text-align:center;text-decoration:none;padding:8px;border-radius:9px;font-weight:600;color:' + (seg === "cal" ? "var(--text)" : "var(--muted)") + (seg === "cal" ? ";background:var(--card)" : "") + '">Calendario</a></div>';
      html += seg === "cal" ? calendarView() : remindersView();
    }
    else if (p[0] === "listas") html = listsView();
    else if (p[0] === "notas") html = notesView();
    else if (p[0] === "mas") {
      if (!p[1]) { html = masView(); fab = false; }
      else {
        back = "#mas";
        title = TITLES[p[1]] || title;
        if (p[1] === "fotos") html = fotosView();
        else if (p[1] === "noticias") { html = noticiasView(); fab = false; }
        else if (p[1] === "viajes") {
          if (p[2]) { back = "#mas/viajes"; title = (tripCache[p[2]] && tripCache[p[2]].name) || "Viaje"; html = tripView(p[2]); }
          else html = viajesView();
        }
        else if (p[1] === "memoria") { html = memoriaView(); fab = false; }
        else if (p[1] === "ajustes") { html = ajustesView(); fab = false; }
        else if (p[1] === "aparatos") { html = multi() ? aparatosView() : '<p class="loading">Este servidor no tiene cuentas.</p>'; fab = false; }
        else if (p[1] === "ia") { html = isAdmin() ? iaView() : '<p class="loading">Solo el administrador.</p>'; fab = false; }
        else if (p[1] === "contenido") { html = contenidoView(); fab = false; }
        else if (p[1] === "log") { html = logView(); fab = false; }
        else html = '<p class="loading">No existe esa pantalla.</p>';
      }
    }
    else { location.hash = "#hoy"; return; }
  } catch (e) {
    console.error(e);
    html = '<div class="card"><p class="bad">Algo falló al pintar: ' + esc(e.message) + '</p><button class="ghost" data-act="reload">Reintentar</button></div>';
  }
  main.innerHTML = html;
  $("title").textContent = title;
  $("backBtn").hidden = !back;
  $("backBtn").dataset.to = back || "";
  $("fab").hidden = !fab;
  qsa(document, ".nav a").forEach((a) => a.classList.toggle("on", a.dataset.nav === p[0]));
  // Las imágenes que ya se bajaron
  qsa(main, "img[data-blob]").forEach((img) => { const b = blobCache[img.dataset.blob]; if (b && b !== "loading") img.src = b; });
  if (p[0] === "mas" && p[1] === "log" && logText && logText !== "loading") { const box = $("logBox"); if (box) box.scrollTop = box.scrollHeight; }
}

// ── Eventos (delegación) ────────────────────────────────────────────────────
document.addEventListener("click", async (ev) => {
  const b = ev.target.closest("[data-act]");
  if (!b) return;
  const act = b.dataset.act;
  const d = b.dataset;
  const tripId = parts()[0] === "mas" && parts()[1] === "viajes" ? parts()[2] : "";
  const trip = tripId ? tripCache[tripId] : null;
  try {
    switch (act) {
      case "sheet-close": closeSheet(); break;
      case "sheet-save": {
        if (!sheetOpts || !sheetOpts.save) break;
        b.disabled = true;
        try { const ok = await sheetOpts.save($("sheet")); if (ok !== false) closeSheet(); } finally { b.disabled = false; }
        break;
      }
      case "sheet-del": {
        if (!sheetOpts || !sheetOpts.del) break;
        b.disabled = true;
        try { const ok = await sheetOpts.del($("sheet")); if (ok !== false) closeSheet(); } finally { b.disabled = false; }
        break;
      }
      case "reload": refresh(); break;
      case "seg-go": localStorage.setItem("listSeg", d.key); break;

      // Recordatorios
      case "rem-tick": {
        b.classList.add("on");
        await change(() => api("/api/hub/done", { kind: "reminder", id: Number(d.id), at: Number(d.at) || undefined }), "Hecho");
        break;
      }
      case "rem-open": reminderEditor(findReminder(Number(d.id))); break;
      case "rem-restore": await change(() => api("/api/board/reminder", { id: Number(d.id), done: false }), "Reactivado"); break;

      // Calendario
      case "cal-prev": case "cal-next": {
        const dd = dateOf(calMonth + "-01");
        dd.setMonth(dd.getMonth() + (act === "cal-next" ? 1 : -1));
        calMonth = isoOf(dd).slice(0, 7);
        render();
        break;
      }
      case "cal-today": calMonth = todayIso().slice(0, 7); calSel = todayIso(); render(); break;
      case "cal-sel": calSel = d.date; if (d.date.slice(0, 7) !== calMonth) calMonth = d.date.slice(0, 7); render(); break;
      case "ev-new": eventEditor(null, d.date); break;
      case "ev-open": {
        const all = Object.values(CAL).flatMap((m) => m.events).concat(Object.values(DAY).flat());
        const o = all.find((x) => x.key === d.key) || all.find((x) => x.kind === "event" && x.id === Number(d.id));
        if (o) eventEditor(o);
        break;
      }
      case "trip-go": location.hash = "#mas/viajes/" + d.trip; break;

      // Listas
      case "list-seg": localStorage.setItem("listSeg", d.key); render(); break;
      case "item-tick": {
        const done = d.done !== "1";
        b.classList.toggle("on", done);
        await change(() => done ? api("/api/hub/done", { kind: "item", id: Number(d.id) }) : api("/api/board/item", { id: Number(d.id), done: false }));
        break;
      }
      case "item-open": {
        const list = S.lists.find((l) => l.key === d.list);
        const it = list && list.items.find((i) => i.id === Number(d.id));
        if (it) itemEditor(it, list.key);
        break;
      }
      case "items-clear": {
        const list = S.lists.find((l) => l.key === d.key);
        const done = list.items.filter((i) => i.done);
        if (!sure("¿Borrar los " + done.length + " hechos de " + list.name.toLowerCase() + "?")) break;
        await change(async () => { for (const i of done) await api("/api/hub/edit", { kind: "item", id: i.id, action: "delete" }); }, "Limpio");
        break;
      }

      // Notas / memoria / feeds
      case "note-open": { const n = S.notes.find((x) => x.id === Number(d.id)); if (n) noteEditor(n); break; }
      case "memory-open": { const m = S.memories.find((x) => x.id === Number(d.id)); if (m) memorySheet(m); break; }
      case "feed-open": { const f = S.feeds.find((x) => x.id === Number(d.id)); if (f) feedSheet(f); break; }
      case "feed-test": {
        sheetStatus("Probando…");
        try { const r = await api("/api/board/feed/test", { id: Number(d.id) }); sheetStatus(r.count !== undefined ? r.count + " titulares" + (r.error ? " · " + r.error : "") + (r.latest ? " · último: " + r.latest : "") : JSON.stringify(r), r.error ? "bad" : "ok"); }
        catch (e) { sheetStatus(e.message, "bad"); }
        break;
      }
      case "rss-load": case "rss-reload": {
        rssCache = "loading"; render();
        try { rssCache = await api("/api/rss"); } catch (e) { rssCache = null; toast(e.message); }
        render();
        break;
      }

      // Fotos
      case "photo-pick": $("photoFiles").click(); break;
      case "photo-open": { const p = (photos || []).find((x) => x.id === d.id); if (p) photoSheet(p); break; }

      // Ajustes
      case "place-pick": {
        const p = placeResults[Number(d.i)];
        await change(() => api("/api/hub/location?lang=es", p), "Lugar guardado: " + p.label);
        placeResults = null;
        render();
        break;
      }

      // Aparatos / cuenta
      case "dev-open": { const dev = (me.devices || []).find((x) => x.deviceId === d.id); if (dev) deviceSheet(dev); break; }
      case "logout": await fetch("/auth/logout", { method: "POST", credentials: "same-origin" }); location.reload(); break;
      case "token-change": gate("Pon el token nuevo del aparato."); break;

      // IA / contenido / log
      case "ai-test": {
        aiTest = "Probando…"; render();
        try { const r = await api("/api/board/config/test", {}); aiTest = "Modelo: " + r.llm + "\nTranscripción: " + r.stt + "\nBúsqueda: " + r.search; }
        catch (e) { aiTest = "No se pudo probar: " + e.message; }
        render();
        break;
      }
      case "costs-load": { costs = "loading"; render(); try { costs = await api("/api/board/costs"); } catch (e) { costs = null; toast(e.message); } render(); break; }
      case "assets-reload": assets = null; render(); break;
      case "assets-build": await api("/api/assets/build", {}); toast("Generando"); setTimeout(() => { assets = null; render(); }, 1500); break;
      case "log-reload": logText = null; render(); break;
      case "log-copy": await copyText(logText || ""); break;
      case "log-clear": if (sure("¿Vaciar el log del aparato?")) { await apiText("/api/log", "DELETE"); logText = null; await loadState(); render(); } break;

      // Viajes
      case "trip-edit": tripEditor(trip); break;
      case "titem-new": tripItemEditor(trip, d.date, null); break;
      case "titem-open": { const day = trip.days.find((x) => x.date === d.date); const it = day && day.items.find((i) => i.id === d.id); if (it) tripItemEditor(trip, d.date, it); break; }
      case "tday-note": dayNoteEditor(trip, d.date); break;
      case "pack-tick": await api("/api/trip/packing", { tripId: trip.id, id: d.id, done: d.done !== "1" }); forgetTrips(); render(); break;
      case "pack-open": { const p = trip.packing.find((x) => x.id === d.id); if (p) packEditor(trip, p); break; }
      case "att-pick": $("attFile").click(); break;
      case "att-open": attachmentSheet(trip, d.id, { date: d.date, itemId: d.item }); break;
      case "att-page": {
        qsa($("sheet"), ".chip[data-page]").forEach((c) => c.classList.toggle("on", c === b));
        const key = "a:" + d.id + ":" + d.page;
        const img = qs($("sheet"), "img.preview");
        img.dataset.blob = key;
        img.src = attSrc(d.id, Number(d.page), key);
        break;
      }
      case "att-unattach": await api("/api/trip/attach", { tripId: trip.id, date: d.date, itemId: d.item, attachmentId: d.id, action: "remove" }); forgetTrips(); closeSheet(); render(); toast("Descolgado"); break;
      case "trip-suggest": {
        const box = $("tripSuggest");
        box.textContent = "Pensando… (puede tardar medio minuto)";
        try {
          const r = await api("/api/suggest/trip?id=" + encodeURIComponent(trip.id) + "&lang=es");
          box.innerHTML = "<ul class='rows'>" + (r.lines || []).map((l) => "<li style='min-height:0;padding:6px 0'><div class='body'>" + esc(l) + "</div></li>").join("") + "</ul>" +
            (r.packing && r.packing.length ? "<p class='hint'>Falta llevar: " + esc(r.packing.join(", ")) + "</p>" : "") +
            (r.sources && r.sources.length ? "<p class='hint'>Fuentes: " + r.sources.map((s) => "<a href='" + attr(s.url) + "' target='_blank' rel='noopener'>" + esc(s.title || s.url) + "</a>").join(" · ") + "</p>" : "");
        } catch (e) { box.textContent = "No se pudo: " + e.message; }
        break;
      }
      default: break;
    }
  } catch (e) {
    // change() ya avisó; lo demás avisa acá.
    if (!/^No se pudo/.test(String(e.message))) toast("No se pudo: " + e.message, 3500);
  }
});

document.addEventListener("submit", async (ev) => {
  const f = ev.target.closest("form[data-form]");
  if (!f) return;
  ev.preventDefault();
  const kind = f.dataset.form;
  const tripId = parts()[1] === "viajes" ? parts()[2] : "";
  try {
    if (kind === "item-add") {
      const text = f.text.value.trim();
      if (!text) return;
      f.text.value = "";
      await change(() => api("/api/board/item", { list: f.dataset.list, text }));
      const again = qs($("main"), "form[data-form=item-add] input");
      if (again) again.focus();
    } else if (kind === "memory-add") {
      const text = f.text.value.trim();
      if (!text) return;
      await change(() => api("/api/board/memory", { text }), "Guardado");
    } else if (kind === "feed-add") {
      const url = f.url.value.trim();
      if (!url) return;
      toast("Buscando el feed…");
      const r = await change(() => api("/api/board/feed", { url, name: f.name.value }));
      rssCache = null;
      toast(r.name + ": " + r.count + " titulares", 4000);
    } else if (kind === "settings") {
      await change(() => api("/api/board/settings", {
        lang: f.lang.value, speak: f.speak.value, uiSound: f.uiSound.value, translatorLang: f.translatorLang.value, musicVolume: Number(f.musicVolume.value),
      }), "Guardado. El aparato lo toma al sincronizar.");
    } else if (kind === "place-search") {
      const q = f.q.value.trim();
      if (!q) return;
      placeResults = "loading"; render();
      try { placeResults = (await api("/api/hub/location/search?q=" + encodeURIComponent(q))).results; } catch (e) { placeResults = []; toast(e.message); }
      render();
    } else if (kind === "pair") {
      const r = await api("/api/account/pair", { code: f.code.value.trim(), name: f.name.value.trim() });
      await loadMe(); render();
      toast("Aparato vinculado: " + r.deviceId);
    } else if (kind === "password") {
      await api("/api/account/password", { current: f.current.value, password: f.password.value });
      f.reset();
      toast("Contraseña cambiada");
    } else if (kind === "ai") {
      await saveAi(f);
    } else if (kind === "token-set") {
      const v = f.token.value.trim();
      if (v.length < 8) { toast("El token necesita 8 caracteres o más"); return; }
      if (!sure("El aparato va a necesitar este token nuevo (web UI del aparato → Servidor). El del entorno sigue valiendo. ¿Seguimos?")) return;
      await api("/api/board/config", { deviceToken: v });
      cfg = null; render();
      toast("Token guardado");
    } else if (kind === "pack-add") {
      const text = f.text.value.trim();
      if (!text) return;
      await api("/api/trip/packing", { tripId: f.dataset.trip, text });
      forgetTrips(); render();
    }
    void tripId;
  } catch (e) {
    if (!/^No se pudo/.test(String(e.message))) toast("No se pudo: " + e.message, 3500);
  }
});

document.addEventListener("change", (ev) => {
  const t = ev.target;
  if (t.id === "photoFiles" && t.files.length) uploadPhotos(Array.from(t.files));
  if (t.id === "attFile" && t.files[0]) { const trip = tripCache[parts()[2]]; if (trip) uploadAttachment(trip, t.files[0]); }
  if (t.name === "musicVolume") { const out = qs(t.closest("form"), ".volOut"); if (out) out.textContent = t.value + " %"; }
  if (t.name === "preset" && cfg) onPresetChange(t.closest("form"));
});
document.addEventListener("input", (ev) => {
  const t = ev.target;
  if (t.name === "musicVolume") { const out = qs(t.closest("form"), ".volOut"); if (out) out.textContent = t.value + " %"; }
});

$("sheetBg").addEventListener("click", closeSheet);
document.addEventListener("keydown", (ev) => { if (ev.key === "Escape" && sheetOpts) closeSheet(); });
$("backBtn").addEventListener("click", () => { location.hash = $("backBtn").dataset.to || "#mas"; });
$("reloadBtn").addEventListener("click", () => { clearCal(); photos = null; trips = null; tripCache = {}; rssCache = null; refresh(); });
$("fab").addEventListener("click", () => {
  const p = parts();
  if (p[0] === "listas") quickAdd(listSeg() === "Compras" ? "shop" : "task");
  else if (p[0] === "notas") noteEditor(null);
  else if (p[0] === "agenda" && p[1] === "cal") eventEditor(null, calSel || todayIso());
  else if (p[0] === "agenda") reminderEditor(null);
  else if (p[0] === "mas" && p[1] === "fotos") $("photoFiles").click();
  else if (p[0] === "mas" && p[1] === "viajes" && p[2]) { const t = tripCache[p[2]]; if (t) tripItemEditor(t, t.days[0] ? t.days[0].date : t.start, null); }
  else if (p[0] === "mas" && p[1] === "viajes") tripEditor(null);
  else quickAdd("reminder");
});
window.addEventListener("hashchange", () => { window.scrollTo(0, 0); render(); });
document.addEventListener("visibilitychange", () => { if (!document.hidden && entered && S) refresh(); });

// ── Entrada ─────────────────────────────────────────────────────────────────
function screen(which, msg) {
  entered = which === "app";
  $("gate").hidden = which !== "token";
  $("login").hidden = which !== "login";
  $("app").hidden = which !== "app";
  document.body.classList.toggle("bare", which !== "app");
  if (which === "token") { $("gateMsg").textContent = msg || ""; $("tokenInput").value = token; }
  if (which === "login") $("loginMsg").textContent = msg || "";
}
function gate(msg) { screen("token", msg); }
function showLogin(msg) { screen("login", msg); }

async function loadMe() {
  try { me = await (await fetch("/auth/me", { credentials: "same-origin" })).json(); } catch (e) { me = null; }
  return me;
}

async function authPost(path, body) {
  const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" }, credentials: "same-origin", body: JSON.stringify(body || {}) });
  let j = {};
  try { j = await r.json(); } catch (e) {}
  if (!r.ok || !j.ok) throw new Error(j.error || ("error " + r.status));
  return j;
}

async function enter() {
  if (me && me.multi) {
    token = "";
    localStorage.removeItem("deviceToken");
    if (!me.ok) { showLogin(""); return; }
  } else if (!token) { gate(""); return; }
  screen("app");
  $("main").innerHTML = '<div class="loading">Cargando…</div>';
  try {
    await loadState();
    render();
  } catch (e) {
    if (multi()) showLogin("No se pudo conectar: " + e.message);
    else gate("No se pudo conectar: " + e.message);
  }
}

$("tokenSave").addEventListener("click", () => {
  const v = $("tokenInput").value.trim();
  if (!v) { $("gateMsg").textContent = "Pon el token"; return; }
  token = v;
  localStorage.setItem("deviceToken", token);
  enter();
});
$("tokenInput").addEventListener("keydown", (ev) => { if (ev.key === "Enter") $("tokenSave").click(); });
$("loginForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const creating = $("login").dataset.mode === "register";
  try {
    await authPost(creating ? "/auth/register" : "/auth/login", { email: $("loginEmail").value.trim(), password: $("loginPass").value });
    $("loginPass").value = "";
    await loadMe();
    await enter();
  } catch (e) { $("loginMsg").textContent = e.message; }
});
$("loginSwitch").addEventListener("click", () => {
  const creating = $("login").dataset.mode === "register";
  $("login").dataset.mode = creating ? "login" : "register";
  $("loginTitle").textContent = creating ? "Entrar" : "Crear cuenta";
  $("loginGo").textContent = creating ? "Entrar" : "Crear cuenta";
  $("loginSwitch").textContent = creating ? "Crear una cuenta" : "Ya tengo cuenta";
  $("loginMsg").textContent = "";
});

(async function start() {
  await loadMe();
  await enter();
})();
