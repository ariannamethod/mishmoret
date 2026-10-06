"use strict";

const $ = (id) => document.getElementById(id);
function itzikFace() { return $("itzik-face-template").content.cloneNode(true); }
document.querySelectorAll("[data-itzik-avatar]").forEach((slot) => slot.append(itzikFace()));
const labels = {
  periods: { morning: "בוקר", afternoon: "אחר הצהריים", full: "יום מלא" },
  locations: { center: "בחממה", home: "מהבית", all: "כל המקומות" },
  roles: { admin: "ניהול", member: "משתתף / משתתפת" },
};
const errors = {
  invalid_credentials: "שם המשתמש או הסיסמה אינם נכונים.",
  unauthorized: "יש להיכנס לחשבון כדי להמשיך.",
  unauthenticated: "יש להיכנס לחשבון כדי להמשיך.",
  forbidden: "אין לחשבון שלך הרשאה לפעולה הזאת.",
  csrf: "החיבור התעדכן. יש לרענן את העמוד ולנסות שוב.",
  invalid_csrf: "החיבור התעדכן. יש לרענן את העמוד ולנסות שוב.",
  password_change_required: "יש לבחור סיסמה אישית לפני שממשיכים.",
  must_change_password: "יש לבחור סיסמה אישית לפני שממשיכים.",
  rate_limited: "בוצעו ניסיונות רבים. אפשר לנסות שוב בעוד מעט זמן.",
  too_many_requests: "בוצעו ניסיונות רבים. אפשר לנסות שוב בעוד מעט זמן.",
  resource_conflict: "המקום כבר תפוס בחלק מהזמן שבחרת. כדאי לבחור מקום או זמן אחר.",
  resource_busy: "המקום כבר תפוס בחלק מהזמן שבחרת. כדאי לבחור מקום או זמן אחר.",
  booking_conflict: "כבר קיימת הזמנה חופפת למקום הזה.",
  shift_closed: "המשמרת הזאת סגורה להרשמה.",
  closed: "המשמרת הזאת סגורה להרשמה.",
  last_admin: "נדרש לפחות מנהל פעיל אחד.",
  last_active_admin: "נדרש לפחות מנהל פעיל אחד.",
  login_exists: "שם המשתמש הזה כבר קיים.",
  duplicate_login: "שם המשתמש הזה כבר קיים.",
  resource_in_use: "למקום הזה יש הזמנות עתידיות. יש לטפל בהן לפני השבתתו.",
  resource_booked: "למקום הזה יש הזמנות עתידיות. יש לטפל בהן לפני השבתתו.",
  resource_exists: "המקום הזה כבר קיים.",
  invalid_resource: "המקום אינו זמין, או שחסרים פרטים. יש לבדוק את הבחירה.",
  invalid_booking: "יש לבחור תאריך מהיום והלאה, בימים ראשון עד חמישי, זמן ומקום השתתפות.",
  invalid_user: "יש למלא שם לתצוגה ושם משתמש באותיות אנגליות קטנות, ספרות, נקודה, מקף או קו תחתון.",
  invalid_date: "יש לבחור תאריך תקין.",
  invalid_closure: "יש לבדוק את התאריך, הזמן ומקום ההשתתפות של הסגירה.",
  invalid_announcement: "יש לבדוק את תאריכי ההודעה ואת הטקסט.",
  invalid_origin: "הכתובת הזאת אינה מוגדרת לכניסה. יש לפתוח את הקישור שנמסר לך.",
  database_error: "לא ניתן לשמור את הפעולה כרגע. יש לנסות שוב או לפנות לצוות.",
  home_resource: "בהשתתפות מהבית אין צורך להזמין חדר או שולחן.",
  not_found: "הפריט לא נמצא. ייתכן שכבר השתנה או נמחק.",
  invalid_password: "הסיסמה הנוכחית אינה נכונה, או שהסיסמה החדשה אינה עומדת בדרישות.",
};

function jerusalemToday() {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Jerusalem", year: "numeric", month: "2-digit", day: "2-digit",
  }).formatToParts(new Date());
  const part = (name) => parts.find((item) => item.type === name).value;
  return `${part("year")}-${part("month")}-${part("day")}`;
}
function dateObject(value) { return new Date(`${value}T12:00:00Z`); }
function addDays(value, days) {
  const date = dateObject(value);
  date.setUTCDate(date.getUTCDate() + days);
  return date.toISOString().slice(0, 10);
}
function sunday(value) { return addDays(value, -dateObject(value).getUTCDay()); }
function dateLabel(value, options = { day: "numeric", month: "long" }) {
  return new Intl.DateTimeFormat("he-IL", { ...options, timeZone: "UTC" }).format(dateObject(value));
}
function rangeLabel(start, end = addDays(start, 4)) {
  return `${dateLabel(start)} – ${dateLabel(end, { day: "numeric", month: "long", year: "numeric" })}`;
}
function formValues(form) { return Object.fromEntries(new FormData(form).entries()); }
function node(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}
function button(text, className, callback, accessibleName) {
  const element = node("button", className, text);
  element.type = "button";
  if (accessibleName) element.setAttribute("aria-label", accessibleName);
  element.addEventListener("click", callback);
  return element;
}
function textError(id, message = "") {
  $(id).textContent = message;
  $(id).hidden = !message;
}
function toast(message) {
  clearTimeout(state.toastTimer);
  $("status").textContent = message;
  $("status").hidden = false;
  state.toastTimer = setTimeout(() => { $("status").hidden = true; }, 5000);
}

const state = {
  user: null, csrf: "", start: sunday(jerusalemToday()), mineOnly: false,
  week: null, users: [], view: "week", adminTab: "people", weekRequest: 0,
  editingUser: null, credentialAfterClose: null, toastTimer: null,
  wolfeRequest: 0, wolfeProposal: false, wolfePendingBubble: null, wolfeIncompleteText: "", wolfeMissing: [],
};

async function api(path, { method = "GET", body } = {}) {
  const requestUser = state.user?.id;
  const requestCsrf = state.csrf;
  const headers = { Accept: "application/json" };
  if (method !== "GET") {
    headers["Content-Type"] = "application/json";
    if (state.csrf) headers["X-CSRF-Token"] = state.csrf;
  }
  let response;
  try {
    response = await fetch(path, {
      method, headers, credentials: "same-origin", cache: "no-store",
      ...(body !== undefined ? { body: JSON.stringify(body) } : {}),
    });
  } catch {
    throw new Error("לא הצלחנו להתחבר לשרת. כדאי לבדוק את החיבור ולנסות שוב.");
  }
  let data;
  try { data = await response.json(); } catch { throw new Error("השרת החזיר תשובה לא צפויה. אפשר לנסות שוב."); }
  if (!response.ok) {
    if (response.status === 401 && path !== "/api/login" && path !== "/api/password"
        && state.user?.id === requestUser && state.csrf === requestCsrf) {
      showLogin();
    }
    const error = new Error(errors[data.error] || data.message || "הפעולה לא הושלמה. אפשר לנסות שוב.");
    error.code = data.error;
    error.status = response.status;
    throw error;
  }
  return data;
}

function closeDialogs() {
  document.querySelectorAll("dialog[open]").forEach((dialog) => dialog.close());
}
function showLogin() {
  resetWolfe();
  closeWolfeChat(false);
  state.user = null;
  state.csrf = "";
  state.week = null;
  state.users = [];
  state.weekRequest += 1;
  closeDialogs();
  $("app-screen").hidden = true;
  $("first-password-screen").hidden = true;
  $("auth-screen").hidden = false;
  $("initial-loading").hidden = true;
  $("calendar").replaceChildren();
  for (const id of ["users-list", "resources-list", "closures-list", "notices-list", "announcement-list"]) $(id).replaceChildren();
  $("login-password").value = "";
}
async function acceptSession(session) {
  if (state.user?.id !== session.user?.id || state.csrf !== (session.csrf || "")) {
    resetWolfe();
    closeWolfeChat(false);
  }
  state.user = session.user;
  state.csrf = session.csrf || "";
  $("initial-loading").hidden = true;
  if (!state.user) { showLogin(); return; }
  $("auth-screen").hidden = true;
  const mustChange = !!state.user.must_change_password;
  $("first-password-screen").hidden = !mustChange;
  $("app-screen").hidden = mustChange;
  if (mustChange) {
    $("first-old-password").focus();
    return;
  }
  $("account-name").textContent = state.user.name;
  $("nav-admin").hidden = state.user.role !== "admin";
  if (state.user.role !== "admin") showView("week");
  await refreshWeek();
}
async function refreshSession() { await acceptSession(await api("/api/session")); }

function setFormBusy(form, busy) {
  form.setAttribute("aria-busy", String(busy));
  const dialog = form.closest("dialog");
  if (dialog) dialog.dataset.busy = String(busy);
  for (const field of form.elements) {
    if (busy) {
      field.dataset.wasDisabled = String(field.disabled);
      field.disabled = true;
    } else {
      field.disabled = field.dataset.wasDisabled === "true";
      delete field.dataset.wasDisabled;
    }
  }
}
function bindForm(id, errorId, handler) {
  const form = $(id);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (form.getAttribute("aria-busy") === "true") return;
    textError(errorId);
    const values = formValues(form);
    setFormBusy(form, true);
    try { await handler(values, form); }
    catch (error) { textError(errorId, error.message); }
    finally { setFormBusy(form, false); }
  });
}
async function runAction(element, handler, errorId = "page-error") {
  if (element.disabled) return;
  const form = element.closest("form");
  if (form) setFormBusy(form, true);
  else element.disabled = true;
  textError(errorId);
  try { await handler(); }
  catch (error) { textError(errorId, error.message); }
  finally {
    if (form) setFormBusy(form, false);
    else element.disabled = false;
  }
}
function openDialog(id, errorId) {
  if (id !== "booking-dialog") resetWolfe(false);
  if (errorId) textError(errorId);
  $(id).showModal();
}
function defaultBookingDate() {
  const today = jerusalemToday();
  return today >= state.start && today <= addDays(state.start, 4) ? today : state.start;
}
function requireWeekday(value) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value) || Number.isNaN(dateObject(value).getTime())) throw new Error("יש לבחור תאריך תקין.");
  if (dateObject(value).getUTCDay() > 4) throw new Error("ההרשמה פתוחה לימים ראשון עד חמישי.");
}

function renderAnnouncements() {
  const target = $("announcement-list");
  target.replaceChildren();
  for (const announcement of state.week.announcements) {
    const article = node("article", "announcement");
    const symbol = node("span", "announcement-symbol", "i");
    symbol.setAttribute("aria-hidden", "true");
    const content = node("div");
    content.append(node("h3", "", announcement.title), node("p", "", announcement.body));
    content.append(node("p", "notice-date", announcement.date === announcement.end_date ? dateLabel(announcement.date) : rangeLabel(announcement.date, announcement.end_date)));
    article.append(symbol, content);
    target.append(article);
  }
}
function renderBooking(booking) {
  const own = booking.user_id === state.user.id;
  const card = node("article", `booking-card ${booking.location}${own ? " mine" : ""}${booking.blocked ? " blocked" : ""}`);
  const top = node("div", "booking-top");
  top.append(node("span", "booking-name", booking.name));
  if (own) top.append(node("span", "mine-label", "שלי"));
  card.append(top, node("span", "booking-location", labels.locations[booking.location] || booking.location));
  if (booking.resource_name) {
    card.append(node("span", "booking-resource", `${booking.resource_name}${booking.room ? ` · ${booking.room}` : ""}`));
  }
  if (booking.blocked) card.append(node("span", "cancelled-label", "מבוטלת כרגע · המשמרת סגורה"));
  if (own) {
    const actions = node("div", "booking-actions");
    actions.append(button("עדכון", "booking-edit", () => openBooking(booking.date), `עדכון ההרשמה שלך ל${dateLabel(booking.date)}`));
    card.append(actions);
  } else if (state.user.role === "admin") {
    const actions = node("div", "booking-actions");
    actions.append(button("ביטול הרשמה", "booking-edit", (event) => runAction(event.currentTarget, async () => {
      if (!window.confirm(`לבטל את ההרשמה של ${booking.name} ל${dateLabel(booking.date)}?`)) return;
      await api(`/api/bookings?id=${booking.id}`, { method: "DELETE", body: {} });
      toast("ההרשמה בוטלה.");
      await refreshWeek();
    })));
    card.append(actions);
  }
  return card;
}
function renderCalendar() {
  if (!state.week || !state.user) return;
  $("week-range").textContent = rangeLabel(state.start);
  const calendar = $("calendar");
  calendar.replaceChildren();
  const today = jerusalemToday();
  for (let offset = 0; offset < 5; offset += 1) {
    const date = addDays(state.start, offset);
    const day = node("section", `day-column${date === today ? " is-today" : ""}`);
    const heading = node("div", "day-header");
    const number = node("span", "day-number", String(dateObject(date).getUTCDate()));
    const title = node("h3", "day-label", dateLabel(date, { weekday: "long" }));
    if (date === today) title.append(node("span", "today-label", "היום"));
    heading.append(number, title);
    const body = node("div", "day-body");
    for (const closure of state.week.closures.filter((item) => item.date === date)) {
      const closed = node("div", "closure-card");
      closed.append(node("strong", "", `סגור · ${labels.periods[closure.period]} · ${labels.locations[closure.location]}`));
      if (closure.reason) closed.append(node("span", "", closure.reason));
      body.append(closed);
    }
    const bookings = state.week.bookings.filter((item) => item.date === date && (!state.mineOnly || item.user_id === state.user.id));
    for (const period of ["full", "morning", "afternoon"]) {
      const items = bookings.filter((item) => item.period === period);
      if (!items.length) continue;
      const group = node("section", "period-group");
      group.append(node("h3", "period-heading", labels.periods[period]));
      for (const booking of items) group.append(renderBooking(booking));
      body.append(group);
    }
    if (!bookings.length) body.append(node("p", "empty-day", state.mineOnly ? "עוד לא נרשמת ליום הזה" : "עוד אין הרשמות"));
    const existing = state.week.bookings.some((item) => item.date === date && item.user_id === state.user.id);
    body.append(button(existing ? "עדכון המשמרת שלי" : "＋ הרשמה ליום הזה", "day-add", () => openBooking(date), `${existing ? "עדכון הרשמה" : "הרשמה"} ל${dateLabel(date)}`));
    day.append(heading, body);
    calendar.append(day);
  }
  const active = state.week.bookings.filter((item) => !item.blocked);
  const mine = active.filter((item) => item.user_id === state.user.id).length;
  $("week-summary").textContent = `${active.length} הרשמות השבוע · ${mine} שלך`;
  $("filter-all").classList.toggle("selected", !state.mineOnly);
  $("filter-all").setAttribute("aria-pressed", String(!state.mineOnly));
  $("filter-mine").classList.toggle("selected", state.mineOnly);
  $("filter-mine").setAttribute("aria-pressed", String(state.mineOnly));
  renderAnnouncements();
}

async function refreshWeek() {
  if (!state.user || state.user.must_change_password) return;
  const request = ++state.weekRequest;
  const requestedStart = state.start;
  $("week-loading").hidden = false;
  $("new-booking").disabled = true;
  $("wolfe-submit").disabled = true;
  textError("page-error");
  try {
    const week = await api(`/api/week?start=${encodeURIComponent(requestedStart)}`);
    if (request !== state.weekRequest || !state.user) return;
    state.week = week;
    renderCalendar();
    renderAdminData();
  } catch (error) {
    if (request === state.weekRequest) {
      state.week = null;
      $("calendar").replaceChildren();
      $("announcement-list").replaceChildren();
      textError("page-error", error.message);
    }
  } finally {
    if (request === state.weekRequest) {
      $("week-loading").hidden = true;
      $("new-booking").disabled = !state.week;
      if ($("wolfe-form").getAttribute("aria-busy") !== "true") $("wolfe-submit").disabled = !state.week;
    }
  }
}
function showView(view, { preserveWolfe = false } = {}) {
  if (view === "admin" && state.user?.role !== "admin") return;
  if (state.view !== view && !preserveWolfe) resetWolfe(false);
  state.view = view;
  $("week-view").hidden = view !== "week";
  $("admin-view").hidden = view !== "admin";
  for (const name of ["week", "admin"]) {
    const nav = $(`nav-${name}`);
    nav.classList.toggle("active", name === view);
    if (name === view) nav.setAttribute("aria-current", "page");
    else nav.removeAttribute("aria-current");
  }
}
function showAdminTab(tab) {
  state.adminTab = tab;
  document.querySelectorAll("[data-admin-tab]").forEach((item) => {
    const selected = item.dataset.adminTab === tab;
    item.classList.toggle("active", selected);
    item.setAttribute("aria-pressed", String(selected));
  });
  for (const name of ["people", "places", "schedule", "notices"]) $( `admin-${name}`).hidden = name !== tab;
}
function emptyList(target, message) {
  target.replaceChildren(node("p", "empty-list", message));
}
function listRow(title, description, inactive = false) {
  const row = node("div", `list-row${inactive ? " inactive" : ""}`);
  const main = node("div", "row-main");
  main.append(node("div", "row-title", title), node("p", "row-details", description));
  const actions = node("div", "row-actions");
  row.append(main, actions);
  return { row, actions };
}
async function refreshUsers() {
  if (state.user?.role !== "admin") return;
  const requestingUser = state.user.id;
  const result = await api("/api/admin/users");
  if (state.user?.id !== requestingUser || state.user.role !== "admin") return;
  state.users = result.users;
  const target = $("users-list");
  target.replaceChildren();
  for (const user of state.users) {
    // Reut's nickname is display text; authorization depends only on the role.
    const roleLabel = user.role === "admin" && user.login === "reut"
      ? "ניהול · דיקטטרורית" : labels.roles[user.role];
    const details = [user.login, roleLabel, user.active ? "פעיל" : "לא פעיל"];
    if (user.must_change_password) details.push("נדרשת החלפת סיסמה");
    const { row, actions } = listRow(user.name, details.join(" · "), !user.active);
    actions.append(button("עריכה", "button subtle small-button", () => openUser(user), `עריכת המשתמש ${user.name}`));
    target.append(row);
  }
  if (!state.users.length) emptyList(target, "עדיין אין משתמשים.");
}
function renderAdminData() {
  if (!state.week || state.user?.role !== "admin") return;
  $("closures-range").textContent = `השבוע המוצג: ${rangeLabel(state.start)}. בחירת שבוע אחר נעשית בלוח השבועי.`;
  $("notices-range").textContent = $("closures-range").textContent;
  const resources = $("resources-list");
  resources.replaceChildren();
  for (const resource of state.week.resources) {
    const { row, actions } = listRow(resource.name, `${resource.room} · ${resource.kind === "room" ? "חדר שלם" : "שולחן"} · ${resource.active ? "פעיל" : "לא פעיל"}`, !resource.active);
    actions.append(button(resource.active ? "השבתה" : "הפעלה", "button subtle small-button", (event) => runAction(event.currentTarget, async () => {
      if (resource.active && !window.confirm(`להשבית את ${resource.name}?`)) return;
      await api("/api/admin/resources", { method: "PATCH", body: { id: resource.id, active: !resource.active } });
      toast(resource.active ? "המקום הושבת." : "המקום הופעל.");
      await refreshWeek();
    })));
    resources.append(row);
  }
  if (!state.week.resources.length) emptyList(resources, "כאן מוסיפים חדרים ושולחנות שאפשר להזמין.");

  const closures = $("closures-list");
  closures.replaceChildren();
  for (const closure of state.week.closures) {
    const { row, actions } = listRow(`${dateLabel(closure.date)} · ${labels.periods[closure.period]} · ${labels.locations[closure.location]}`, closure.reason);
    actions.append(button("פתיחה מחדש", "button subtle small-button", (event) => runAction(event.currentTarget, async () => {
      if (!window.confirm("לפתוח מחדש את המשמרת? ההרשמות שנשמרו יופעלו שוב אם אין סגירה נוספת.")) return;
      await api(`/api/admin/closures?id=${closure.id}`, { method: "DELETE", body: {} });
      toast("הסגירה הוסרה.");
      await refreshWeek();
    })));
    closures.append(row);
  }
  if (!state.week.closures.length) emptyList(closures, "אין משמרות סגורות בשבוע הזה.");

  const notices = $("notices-list");
  notices.replaceChildren();
  for (const notice of state.week.announcements) {
    const { row, actions } = listRow(notice.title, `${rangeLabel(notice.date, notice.end_date)}\n${notice.body}`);
    actions.append(button("מחיקה", "button danger-text small-button", (event) => runAction(event.currentTarget, async () => {
      if (!window.confirm(`למחוק את ההודעה ״${notice.title}״?`)) return;
      await api(`/api/admin/announcements?id=${notice.id}`, { method: "DELETE", body: {} });
      toast("ההודעה נמחקה.");
      await refreshWeek();
    })));
    notices.append(row);
  }
  if (!state.week.announcements.length) emptyList(notices, "אין הודעות לשבוע הזה.");
}

function updateResourceVisibility() {
  const isHome = $("booking-location").value === "home";
  $("booking-resource-field").hidden = isHome;
  if (isHome) $("booking-resource").value = "";
}
function updateBookingForDate() {
  const booking = state.week?.bookings.find((item) => item.date === $("booking-date").value && item.user_id === state.user.id);
  $("booking-existing").hidden = !booking;
  $("booking-existing").textContent = $("booking-date").readOnly
    ? "שמירה תעדכן את המשמרת ביום הזה. לשינוי היום, יש לבטל את ההרשמה ולהירשם ביום החדש."
    : "שמירה תעדכן את ההרשמה הקיימת שלך ביום הזה.";
  $("cancel-own-booking").hidden = !booking;
  $("cancel-own-booking").dataset.bookingId = booking ? String(booking.id) : "";
  $("booking-title").textContent = booking ? "עדכון המשמרת שלי" : "הרשמה למשמרת";
  if (booking) {
    $("booking-period").value = booking.period;
    $("booking-location").value = booking.location;
    $("booking-resource").value = booking.resource_id ? String(booking.resource_id) : "";
  }
  updateResourceVisibility();
}
function openBooking(date = defaultBookingDate(), proposal = null) {
  if (!state.week) return;
  if (!proposal) resetWolfe(false);
  state.wolfeProposal = !!proposal;
  $("wolfe-booking-summary").hidden = !proposal;
  $("booking-form").reset();
  $("booking-date").value = date;
  $("booking-date").readOnly = state.week.bookings.some((item) => item.date === date && item.user_id === state.user.id);
  $("booking-date").min = jerusalemToday();
  const select = $("booking-resource");
  select.replaceChildren(new Option("ללא מקום קבוע", ""));
  for (const resource of state.week.resources.filter((item) => item.active)) {
    select.add(new Option(`${resource.name} · ${resource.room}${resource.kind === "room" ? " · חדר שלם" : ""}`, String(resource.id)));
  }
  updateBookingForDate();
  if (proposal) {
    $("booking-period").value = proposal.period;
    $("booking-location").value = proposal.location;
    $("booking-resource").value = "";
    updateResourceVisibility();
    updateWolfeSummary();
  }
  openDialog("booking-dialog", "booking-error");
}

function resetWolfe(clearText = true) {
  state.wolfeIncompleteText = "";
  state.wolfeMissing = [];
  $("wolfe-chat").dataset.mood = "idle";
  state.wolfeRequest += 1;
  const form = $("wolfe-form");
  if (form.getAttribute("aria-busy") === "true") setFormBusy(form, false);
  if (clearText) {
    form.reset();
    $("wolfe-history").replaceChildren();
  } else if (state.wolfePendingBubble) {
    state.wolfePendingBubble.text.textContent = "הבקשה הופסקה. אפשר לשלוח אותה שוב.";
  }
  state.wolfePendingBubble = null;
  $("wolfe-result").textContent = "";
  $("wolfe-result").hidden = true;
  textError("wolfe-error");
  if (state.wolfeProposal && $("booking-dialog").open) $("booking-dialog").close();
  state.wolfeProposal = false;
  $("wolfe-booking-summary").textContent = "";
  $("wolfe-booking-summary").hidden = true;
}
function setWeek(start) {
  const changed = state.start !== start;
  if (changed) resetWolfe();
  state.start = start;
  if (changed && state.user) appendWolfeMessage("context", `עברנו לשבוע ${rangeLabel(start)}. מתחילים שיחה לשבוע הזה.`);
}
function appendWolfeMessage(role, text) {
  const message = node("article", `wolfe-message ${role}`);
  if (role !== "context") {
    const speaker = node("span", "wolfe-speaker", role === "user" ? "אני" : "איציק");
    if (role === "assistant") speaker.prepend(itzikFace());
    message.append(speaker);
  }
  const paragraph = node("p", "wolfe-text", text);
  paragraph.dir = "auto";
  message.append(paragraph);
  $("wolfe-history").append(message);
  $("wolfe-history").scrollTop = $("wolfe-history").scrollHeight;
  return { element: message, text: paragraph };
}
function openWolfeChat() {
  if (!state.user || state.user.must_change_password) return;
  $("wolfe-chat").hidden = false;
  $("wolfe-toggle").setAttribute("aria-expanded", "true");
  if (!$("wolfe-history").children.length) appendWolfeMessage("assistant", "אהלן, אני איציק. מתי ואיפה לומדים?");
  ($("wolfe-text").disabled ? $("wolfe-close") : $("wolfe-text")).focus();
}
function closeWolfeChat(restoreFocus = true) {
  $("wolfe-chat").hidden = true;
  $("wolfe-toggle").setAttribute("aria-expanded", "false");
  if (restoreFocus) $("wolfe-toggle").focus();
}
function proposalSummary(proposal) {
  const date = dateLabel(proposal.date, { weekday: "long", day: "numeric", month: "long", year: "numeric" });
  return `${date} · ${labels.periods[proposal.period]} · ${labels.locations[proposal.location]}${proposal.location === "center" ? " · ללא מקום קבוע" : ""}`;
}
function updateWolfeSummary() {
  if (!state.wolfeProposal) return;
  const date = $("booking-date").value;
  if (!date || Number.isNaN(dateObject(date).getTime())) return;
  const period = labels.periods[$("booking-period").value];
  const location = labels.locations[$("booking-location").value];
  const resource = $("booking-location").value === "center"
    ? ` · ${$("booking-resource").selectedOptions[0]?.textContent || "ללא מקום קבוע"}` : "";
  $("wolfe-booking-summary").textContent = `לאישור שלך: ${dateLabel(date, { weekday: "long", day: "numeric", month: "long", year: "numeric" })} · ${period} · ${location}${resource}. לשמירה יש ללחוץ על ״שמירת משמרת״.`;
}

$("wolfe-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  if (form.getAttribute("aria-busy") === "true") return;
  textError("wolfe-error");
  if (!state.user || state.user.must_change_password || !state.week || state.week.start !== state.start || !$("week-loading").hidden) {
    textError("wolfe-error", "יש להמתין לטעינת השבוע ולנסות שוב.");
    return;
  }
  const text = $("wolfe-text").value.trim();
  if (!text) { textError("wolfe-error", "מה תרצה לעשות? אפשר לכתוב בקשה קצרה."); return; }
  if (new TextEncoder().encode(text).length > 512) {
    textError("wolfe-error", "הבקשה ארוכה מדי. כדאי לנסח אותה במשפט קצר.");
    return;
  }
  const request = ++state.wolfeRequest;
  const userId = state.user.id;
  const csrf = state.csrf;
  const week = state.start;
  const incomplete = state.wolfeIncompleteText;
  const previousMissing = state.wolfeMissing;
  const current = () => request === state.wolfeRequest && state.user?.id === userId && state.csrf === csrf && state.start === week;
  setFormBusy(form, true);
  appendWolfeMessage("user", text);
  $("wolfe-text").value = "";
  const reply = appendWolfeMessage("assistant", "בודק את הבקשה…");
  $("wolfe-chat").dataset.mood = "thinking";
  state.wolfePendingBubble = reply;
  $("wolfe-result").textContent = "איציק בודק את הבקשה…";
  $("wolfe-result").hidden = false;
  try {
    let interpretedText = text;
    let combined = false;
    let result = await api("/api/wolfe", { method: "POST", body: { text, week_start: week } });
    if (!current()) return;
    // A new recognizable command takes precedence. Only an otherwise unhandled
    // reply can complete the previous unfinished request, within this session/week.
    if (result.status === "no_call" && incomplete) {
      // A short correction answers the pending location question. A bare
      // negation or a negative request still passes through Wolfe unchanged.
      const correction = previousMissing.includes("location")
        ? text.match(/^לא\s*[,،]\s*(מהבית|בבית|בית|בחממה|מהחממה|חממה)[.!]?$/u) : null;
      interpretedText = `${incomplete} ${correction ? correction[1] : text}`;
      combined = true;
      if (new TextEncoder().encode(interpretedText).length > 512) {
        state.wolfeIncompleteText = "";
        throw new Error("הבקשה וההשלמה ארוכות מדי. כתבו בקשה חדשה וקצרה עם יום, זמן ומקום.");
      }
      result = await api("/api/wolfe", { method: "POST", body: { text: interpretedText, week_start: week } });
    }
    if (!current()) return;
    const missing = Array.isArray(result.missing) ? result.missing : [];
    const progressed = missing.length < previousMissing.length && missing.every(key => previousMissing.includes(key));
    const keep = result.status === "missing_arguments" && (!combined || progressed);
    state.wolfeIncompleteText = keep ? interpretedText : "";
    state.wolfeMissing = keep ? missing : [];
    if (combined && result.status === "missing_arguments" && !progressed)
      result.message = "הפרטים לא מסתדרים יחד. נתחיל מחדש — איזה יום, זמן ומקום?";
    $("wolfe-chat").dataset.mood = result.status === "call" ? "ready" : "puzzled";
    reply.text.textContent = result.message || "אפשר לנסח את הבקשה שוב עם יום, זמן ומקום.";
    state.wolfePendingBubble = null;
    $("wolfe-result").textContent = "";
    $("wolfe-result").hidden = true;
    if (result.status !== "call") return;
    if (result.action === "propose_booking") {
      const proposal = result.proposal;
      if (!proposal || !/^\d{4}-\d{2}-\d{2}$/.test(proposal.date)
          || !Object.hasOwn(labels.periods, proposal.period) || !["home", "center"].includes(proposal.location)
          || proposal.resource_id !== null) throw new Error("לא התקבלה הצעת הרשמה מלאה. אפשר לנסח את הבקשה שוב.");
      requireWeekday(proposal.date);
      reply.text.textContent = `${reply.text.textContent}\n${proposalSummary(proposal)}`;
      reply.element.append(button("בדיקת ההצעה", "button subtle small-button wolfe-review-proposal", () => {
        if (state.user?.id === userId && state.csrf === csrf && state.start === week) openBooking(proposal.date, proposal);
      }));
      if (!$("wolfe-chat").hidden) openBooking(proposal.date, proposal);
    } else if (["show_schedule", "show_my_bookings"].includes(result.action)) {
      state.mineOnly = result.action === "show_my_bookings";
      showView("week", { preserveWolfe: true });
      renderCalendar();
    }
  } catch (error) {
    if (current()) {
      state.wolfeIncompleteText = "";
      $("wolfe-chat").dataset.mood = "puzzled";
      reply.text.textContent = error.message;
      state.wolfePendingBubble = null;
      $("wolfe-result").hidden = true;
      textError("wolfe-error", error.message);
    }
  } finally {
    if (current()) setFormBusy(form, false);
  }
});
$("wolfe-toggle").addEventListener("click", () => {
  if ($("wolfe-chat").hidden) openWolfeChat();
  else closeWolfeChat();
});
$("wolfe-close").addEventListener("click", () => closeWolfeChat());
$("wolfe-chat").addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    event.preventDefault();
    closeWolfeChat();
  }
});
function openUser(user = null) {
  state.editingUser = user;
  $("user-form").reset();
  $("user-title").textContent = user ? "עריכת משתמש" : "משתמש חדש";
  $("user-login").disabled = !!user;
  $("user-active-label").hidden = !user;
  $("reset-user-password").hidden = !user;
  if (user) {
    $("user-login").value = user.login;
    $("user-name").value = user.name;
    $("user-role").value = user.role;
    $("user-active").checked = !!user.active;
  }
  openDialog("user-dialog", "user-error");
}
function showCredential(user, password, afterClose = null) {
  $("credential-user").textContent = `${user.name} · ${user.login}`;
  $("temporary-password").value = password;
  $("credential-copy-status").textContent = "";
  state.credentialAfterClose = afterClose;
  openDialog("credential-dialog");
  $("temporary-password").focus();
  $("temporary-password").select();
}

bindForm("login-form", "login-error", async (values, form) => {
  const session = await api("/api/login", { method: "POST", body: values });
  form.reset();
  textError("page-error");
  await acceptSession(session);
});
for (const prefix of ["first-password", "password"]) {
  bindForm(`${prefix}-form`, `${prefix}-error`, async (values, form) => {
    if (values.new_password !== values.confirm_password) throw new Error("הסיסמאות החדשות אינן זהות.");
    const size = new TextEncoder().encode(values.new_password).length;
    if (Array.from(values.new_password).length < 12) throw new Error("יש לבחור סיסמה עם לפחות 12 תווים.");
    if (size > 128) throw new Error("הסיסמה ארוכה מדי. יש לקצר אותה מעט.");
    await api("/api/password", { method: "POST", body: { old_password: values.old_password, new_password: values.new_password } });
    form.reset();
    if (prefix === "password") $("password-dialog").close();
    toast("הסיסמה עודכנה.");
    await refreshSession();
  });
}
bindForm("booking-form", "booking-error", async (values) => {
  requireWeekday(values.date);
  await api("/api/bookings", { method: "POST", body: {
    date: values.date, period: values.period, location: values.location,
    resource_id: values.location === "home" || !values.resource_id ? null : Number(values.resource_id),
  } });
  $("booking-dialog").close();
  setWeek(sunday(values.date));
  toast("המשמרת נשמרה.");
  await refreshWeek();
});
bindForm("user-form", "user-error", async (values) => {
  const user = state.editingUser;
  if (user) {
    await api("/api/admin/users", { method: "PATCH", body: { id: user.id, name: values.name, role: values.role, active: values.active === "on" } });
    $("user-dialog").close();
    toast("פרטי המשתמש עודכנו.");
    if (user.id === state.user.id) await refreshSession();
    await refreshUsers();
  } else {
    const result = await api("/api/admin/users", { method: "POST", body: { login: values.login, name: values.name, role: values.role } });
    $("user-dialog").close();
    showCredential(result.user, result.temporary_password);
    await refreshUsers();
  }
});
bindForm("resource-form", "resource-error", async (values, form) => {
  await api("/api/admin/resources", { method: "POST", body: values });
  $("resource-dialog").close();
  form.reset();
  toast("המקום נוסף.");
  await refreshWeek();
});
bindForm("closure-form", "closure-error", async (values, form) => {
  requireWeekday(values.date);
  await api("/api/admin/closures", { method: "POST", body: values });
  $("closure-dialog").close();
  form.reset();
  setWeek(sunday(values.date));
  toast("הסגירה נשמרה וההרשמות עודכנו בלוח.");
  await refreshWeek();
});
bindForm("announcement-form", "announcement-error", async (values, form) => {
  if (values.end_date < values.date) throw new Error("תאריך הסיום צריך להיות ביום ההתחלה או אחריו.");
  if (Array.from(values.title).length > 120) throw new Error("הכותרת יכולה להכיל עד 120 תווים.");
  if (Array.from(values.body).length > 1000) throw new Error("ההודעה יכולה להכיל עד 1000 תווים.");
  await api("/api/admin/announcements", { method: "POST", body: values });
  $("announcement-dialog").close();
  form.reset();
  setWeek(sunday(values.date));
  toast("ההודעה פורסמה.");
  await refreshWeek();
});

for (const id of ["logout", "first-logout"]) {
  $(id).addEventListener("click", (event) => runAction(event.currentTarget, async () => {
    await api("/api/logout", { method: "POST", body: {} });
    showLogin();
    $("login").focus();
  }, id === "first-logout" ? "first-password-error" : "page-error"));
}
$("open-password").addEventListener("click", () => {
  $("password-form").reset();
  openDialog("password-dialog", "password-error");
});
$("nav-week").addEventListener("click", () => showView("week"));
$("nav-admin").addEventListener("click", (event) => runAction(event.currentTarget, async () => {
  showView("admin");
  renderAdminData();
  await refreshUsers();
}));
document.querySelectorAll("[data-admin-tab]").forEach((item) => item.addEventListener("click", () => showAdminTab(item.dataset.adminTab)));
$("previous-week").addEventListener("click", () => { setWeek(addDays(state.start, -7)); refreshWeek(); });
$("next-week").addEventListener("click", () => { setWeek(addDays(state.start, 7)); refreshWeek(); });
$("current-week").addEventListener("click", () => { setWeek(sunday(jerusalemToday())); refreshWeek(); });
$("filter-all").addEventListener("click", () => { state.mineOnly = false; renderCalendar(); });
$("filter-mine").addEventListener("click", () => { state.mineOnly = true; renderCalendar(); });
$("new-booking").addEventListener("click", () => openBooking());
$("booking-location").addEventListener("change", updateResourceVisibility);
$("booking-date").addEventListener("change", updateBookingForDate);
for (const id of ["booking-date", "booking-period", "booking-location", "booking-resource"]) $(id).addEventListener("change", updateWolfeSummary);
$("cancel-own-booking").addEventListener("click", (event) => runAction(event.currentTarget, async () => {
  const id = Number($("cancel-own-booking").dataset.bookingId);
  if (!id || !window.confirm("לבטל את ההרשמה שלך ליום הזה?")) return;
  await api(`/api/bookings?id=${id}`, { method: "DELETE", body: {} });
  $("booking-dialog").close();
  toast("ההרשמה בוטלה.");
  await refreshWeek();
}, "booking-error"));
$("new-user").addEventListener("click", () => openUser());
$("reset-user-password").addEventListener("click", (event) => runAction(event.currentTarget, async () => {
  const user = state.editingUser;
  if (!window.confirm(`לאפס את הסיסמה של ${user.name}? החיבורים הקיימים שלו ייסגרו.`)) return;
  const self = user.id === state.user.id;
  const result = await api("/api/admin/reset-password", { method: "POST", body: { id: user.id } });
  $("user-dialog").close();
  showCredential(user, result.temporary_password, self ? () => { showLogin(); toast("הסיסמה אופסה. יש להיכנס עם הסיסמה הזמנית."); } : null);
  if (!self) await refreshUsers();
}, "user-error"));
$("new-resource").addEventListener("click", () => { $("resource-form").reset(); openDialog("resource-dialog", "resource-error"); });
$("new-closure").addEventListener("click", () => { $("closure-form").reset(); $("closure-date").value = defaultBookingDate(); openDialog("closure-dialog", "closure-error"); });
$("new-announcement").addEventListener("click", () => {
  $("announcement-form").reset();
  $("announcement-date").value = defaultBookingDate();
  $("announcement-end").value = defaultBookingDate();
  openDialog("announcement-dialog", "announcement-error");
});
$("announcement-date").addEventListener("change", () => {
  $("announcement-end").min = $("announcement-date").value;
  if ($("announcement-end").value < $("announcement-date").value) $("announcement-end").value = $("announcement-date").value;
});
document.querySelectorAll("[data-close]").forEach((item) => item.addEventListener("click", () => {
  const dialog = $(item.dataset.close);
  if (dialog.dataset.busy !== "true") dialog.close();
}));
document.querySelectorAll("dialog").forEach((dialog) => dialog.addEventListener("cancel", (event) => {
  if (dialog.dataset.busy === "true") event.preventDefault();
}));
$("credential-dialog").addEventListener("close", () => {
  $("temporary-password").value = "";
  $("credential-user").textContent = "";
  const afterClose = state.credentialAfterClose;
  state.credentialAfterClose = null;
  if (afterClose) afterClose();
});
$("copy-credential").addEventListener("click", async () => {
  const input = $("temporary-password");
  try {
    await navigator.clipboard.writeText(input.value);
    $("credential-copy-status").textContent = "הסיסמה הועתקה.";
  } catch {
    input.focus();
    input.select();
    $("credential-copy-status").textContent = "אפשר להעתיק את הסיסמה המסומנת באמצעות תפריט ההעתקה.";
  }
});

refreshSession().catch((error) => {
  showLogin();
  textError("login-error", error.message);
});
