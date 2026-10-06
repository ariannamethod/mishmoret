// Run after make; Playwright and Chromium are installed only in the CI test environment.
const assert = require('node:assert/strict');
const { execFileSync, spawn } = require('node:child_process');
const { randomUUID } = require('node:crypto');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');
const { chromium } = require('playwright');

const root = path.resolve(__dirname, '..');
const binary = path.join(root, 'build', 'mishmeret');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mishmoret-browser-'));
const artifacts = process.env.BROWSER_ARTIFACTS || path.join(temporary, 'screenshots');
fs.mkdirSync(artifacts, { recursive: true, mode: 0o700 });
let server, browser, page;
let serverLog = '';
const errors = [];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function freePort() {
  const listener = net.createServer();
  listener.listen(0, '127.0.0.1');
  await once(listener, 'listening');
  const port = listener.address().port;
  await new Promise((resolve, reject) => listener.close((error) => error ? reject(error) : resolve()));
  return port;
}

async function waitForServer(base) {
  for (let attempt = 0; attempt < 100; attempt++) {
    assert.equal(server.exitCode, null, `Server exited during startup: ${serverLog}`);
    try {
      if ((await fetch(`${base}/healthz`)).ok) return;
    } catch {}
    await sleep(100);
  }
  throw new Error(`Server did not become ready: ${serverLog}`);
}

async function screenshot(name) {
  await page.screenshot({ path: path.join(artifacts, `${name}.png`), fullPage: true });
}

async function noOverflow(label) {
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth),
    `Horizontal overflow: ${label}`);
}

async function calendarReady() {
  await page.waitForFunction(() => document.querySelectorAll('.day-column').length === 5 &&
    document.querySelector('#week-loading').hidden && !document.querySelector('#new-booking').disabled);
}

async function weekData(base, start) {
  const response = await page.request.get(`${base}/api/week?start=${start}`);
  assert.equal(response.status(), 200);
  return response.json();
}

function sunday(date) {
  const day = new Date(`${date}T12:00:00Z`);
  day.setUTCDate(day.getUTCDate() - day.getUTCDay());
  return day.toISOString().slice(0, 10);
}

async function main() {
  const db = path.join(temporary, 'test.sqlite');
  // Initial credentials stay in memory; artifact uploads contain screenshots only.
  const initialized = JSON.parse(execFileSync(binary, ['--init', '--db', db], {
    cwd: root, encoding: 'utf8', timeout: 30_000,
  }));
  assert.equal(initialized.accounts.length, 4);
  const account = initialized.accounts[0];
  const base = `http://127.0.0.1:${await freePort()}`;
  server = spawn(binary, ['--db', db, '--web', path.join(root, 'web'),
    '--port', new URL(base).port, '--origin', base], {
    cwd: root, stdio: ['ignore', 'pipe', 'pipe'],
  });
  server.stdout.on('data', (chunk) => { serverLog += chunk; });
  server.stderr.on('data', (chunk) => { serverLog += chunk; });
  server.on('error', (error) => { serverLog += error.message; });
  await waitForServer(base);
  browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    viewport: { width: 1365, height: 1000 }, locale: 'he-IL', timezoneId: 'Asia/Jerusalem',
  });
  await context.addInitScript(() => {
    window.__cspViolations = [];
    document.addEventListener('securitypolicyviolation', (event) => {
      window.__cspViolations.push(`${event.violatedDirective}: ${event.blockedURI}`);
    });
  });
  page = await context.newPage();
  page.setDefaultTimeout(15_000);
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => {
    if (message.type() === 'error' && !message.location().url.endsWith('/favicon.ico'))
      errors.push(message.text());
  });
  page.on('dialog', (dialog) => dialog.accept());
  await page.goto(base);
  await page.locator('#auth-screen').waitFor({ state: 'visible' });
  assert(await page.locator('#wolfe-toggle').isHidden());
  await noOverflow('desktop login');
  await screenshot('login-desktop');
  await page.locator('#login').fill(account.login);
  await page.locator('#login-password').fill(account.password);
  await page.locator('#login-form button[type=submit]').click();
  await page.locator('#first-password-screen').waitFor({ state: 'visible' });
  assert(await page.locator('#wolfe-toggle').isHidden());
  const password = `Browser-${randomUUID()}`;
  await page.locator('#first-old-password').fill(account.password);
  await page.locator('#first-new-password').fill(password);
  await page.locator('#first-confirm-password').fill(password);
  await page.locator('#first-password-form button[type=submit]').click();
  await calendarReady();
  await page.locator('#nav-admin').click();
  await page.locator('#users-list .list-row').first().waitFor();
  assert.equal(await page.locator('#users-list .list-row').count(), 4);
  const usersResponse = await page.request.get(`${base}/api/admin/users`);
  assert.equal(usersResponse.status(), 200);
  const users = (await usersResponse.json()).users;
  assert.equal(users.length, 4);
  assert(users.every((user) => user.role === 'admin'), 'All four bootstrap accounts administer the app');
  await page.locator('#nav-week').click();
  const oldRange = await page.locator('#week-range').textContent();
  await page.locator('#next-week').click();
  await page.waitForFunction((previous) => document.querySelector('#week-range').textContent !== previous,
    oldRange);
  await calendarReady();

  // A Hebrew request must open a proposal without storing a booking.
  await page.locator('#wolfe-toggle').click();
  await page.locator('#wolfe-chat').waitFor({ state: 'visible' });
  await page.locator('#wolfe-text').fill('תרשמי אותי ליום שלישי בבוקר מהבית');
  await page.locator('#wolfe-submit').click();
  await page.locator('#booking-dialog').waitFor({ state: 'visible' });
  const date = await page.locator('#booking-date').inputValue();
  const start = sunday(date);
  assert.equal(new Date(`${date}T12:00:00Z`).getUTCDay(), 2, 'Wolfe selected Tuesday');
  assert.equal(await page.locator('#booking-period').inputValue(), 'morning');
  assert.equal(await page.locator('#booking-location').inputValue(), 'home');
  assert((await page.locator('#wolfe-booking-summary').textContent()).includes('יום שלישי'));
  assert.equal((await weekData(base, start)).bookings.length, 0, 'Proposal performed no write');
  await screenshot('wolfe-confirmation-desktop');
  await page.locator('#booking-form button[type=submit]').click();
  await page.locator('#booking-dialog').waitFor({ state: 'hidden' });
  await calendarReady();
  const saved = (await weekData(base, start)).bookings;
  assert.equal(saved.length, 1);
  assert.equal(saved[0].date, date);
  assert.equal(saved[0].period, 'morning');
  assert.equal(saved[0].location, 'home');
  await page.locator('.booking-card.home').waitFor();

  if (await page.locator('#wolfe-chat').isHidden()) await page.locator('#wolfe-toggle').click();
  await page.locator('#wolfe-text').fill('תראי לי את המשמרות שלי');
  await page.locator('#wolfe-submit').click();
  await page.waitForFunction(() => document.querySelector('#filter-mine').getAttribute('aria-pressed') === 'true');
  const history = await page.locator('#wolfe-history').textContent();
  assert(history.includes('תראי לי את המשמרות שלי'));
  await page.locator('#wolfe-close').click();
  await page.locator('#wolfe-chat').waitFor({ state: 'hidden' });
  await page.locator('#wolfe-toggle').click();
  assert.equal(await page.locator('#wolfe-history').textContent(), history, 'Collapse preserves conversation');
  await noOverflow('desktop chat');
  await screenshot('wolfe-desktop');
  await page.setViewportSize({ width: 390, height: 844 });
  await noOverflow('mobile chat');
  await screenshot('wolfe-mobile');
  await page.locator('#wolfe-close').click();
  await noOverflow('mobile calendar');
  await screenshot('week-mobile');

  // Closing an existing shift marks it blocked; reopening keeps the original signup.
  await page.locator('#nav-admin').click();
  await page.locator('[data-admin-tab=schedule]').click();
  await page.locator('#new-closure').click();
  await page.locator('#closure-date').fill(date);
  await page.locator('#closure-period').selectOption('morning');
  await page.locator('#closure-location').selectOption('home');
  await page.locator('#closure-reason').fill('הודעת בדיקה — סגירה זמנית');
  await page.locator('#closure-form button[type=submit]').click();
  await page.locator('#closure-dialog').waitFor({ state: 'hidden' });
  await calendarReady();
  await page.locator('#nav-week').click();
  await page.locator('.booking-card.blocked').waitFor();
  assert.equal((await weekData(base, start)).bookings[0].blocked, true);
  await page.locator('#nav-admin').click();
  await page.locator('#closures-list .list-row button').click();
  await page.locator('#closures-list .empty-list').waitFor();
  await page.locator('#nav-week').click();
  await calendarReady();
  assert.equal(await page.locator('.booking-card.blocked').count(), 0);
  assert.equal((await weekData(base, start)).bookings.length, 1);
  await page.locator('#nav-admin').click();
  await page.locator('[data-admin-tab=people]').click();
  await noOverflow('mobile administration');
  await screenshot('admin-mobile');
  await page.setViewportSize({ width: 1365, height: 1000 });
  await noOverflow('desktop administration');
  await screenshot('admin-desktop');
  await page.locator('#nav-week').click();
  await screenshot('week-desktop');

  // Supplementary-plane characters count as one character in both the form and API.
  let announcementPosts = 0;
  const countAnnouncementPosts = (request) => {
    if (request.method() === 'POST' && new URL(request.url()).pathname === '/api/admin/announcements')
      announcementPosts++;
  };
  page.on('request', countAnnouncementPosts);
  await page.locator('#nav-admin').click();
  await page.locator('[data-admin-tab=notices]').click();
  await page.locator('#new-announcement').click();
  await page.locator('#announcement-date').fill(date);
  await page.locator('#announcement-end').fill(date);
  const title = '🙂'.repeat(120);
  const body = '🙂'.repeat(1000);
  await page.locator('#announcement-heading').fill(title);
  await page.locator('#announcement-body').fill(body);
  assert.equal(await page.locator('#announcement-heading').inputValue(), title);
  assert.equal(await page.locator('#announcement-body').inputValue(), body);
  await page.locator('#announcement-form button[type=submit]').click();
  await page.locator('#announcement-dialog').waitFor({ state: 'hidden' });
  await calendarReady();
  const notices = (await weekData(base, start)).announcements;
  assert.equal(announcementPosts, 1);
  assert.equal(notices.length, 1);
  assert.equal(notices[0].title, title);
  assert.equal(notices[0].body, body);
  await page.locator('#new-announcement').click();
  await page.locator('#announcement-date').fill(date);
  await page.locator('#announcement-end').fill(date);
  // These are limit+1 code points while still fitting native HTML UTF-16 bounds.
  for (const invalid of [
    { title: '🙂'.repeat(119) + 'aa', body, message: 'הכותרת יכולה להכיל עד 120 תווים.' },
    { title, body: '🙂'.repeat(999) + 'aa', message: 'ההודעה יכולה להכיל עד 1000 תווים.' },
  ]) {
    await page.locator('#announcement-heading').fill(invalid.title);
    await page.locator('#announcement-body').fill(invalid.body);
    assert.equal(await page.locator('#announcement-heading').inputValue(), invalid.title);
    assert.equal(await page.locator('#announcement-body').inputValue(), invalid.body);
    await page.locator('#announcement-form button[type=submit]').click();
    await page.locator('#announcement-error').waitFor({ state: 'visible' });
    assert.equal(await page.locator('#announcement-error').textContent(), invalid.message);
    assert.equal(announcementPosts, 1, 'Over-limit announcement was rejected before POST');
    assert.deepEqual((await weekData(base, start)).announcements, notices, 'Stored notices unchanged');
  }
  await page.locator('#announcement-dialog [data-close]').first().click();
  page.off('request', countAnnouncementPosts);
  await page.locator('#logout').click();
  await page.locator('#auth-screen').waitFor({ state: 'visible' });
  assert(await page.locator('#wolfe-toggle').isHidden());
  assert.equal(await page.locator('#wolfe-text').inputValue(), '');
  assert(!(await page.locator('#wolfe-history').textContent()).includes('תראי לי את המשמרות שלי'));
  assert.deepEqual(await page.evaluate(() => window.__cspViolations), [], 'CSP violations');
  assert.deepEqual(errors, [], 'Browser console errors');
  console.log('PASS: login, password change, four admins, Hebrew Wolfe proposal and confirmed save, chat collapse, shift closure/reopening, desktop/mobile layout, announcement Unicode limits and roundtrip, logout and CSP.');
}

main().catch(async (error) => {
  if (page) await screenshot('failure').catch(() => {});
  console.error(error.message);
  process.exitCode = 1;
}).finally(async () => {
  if (browser) await browser.close().catch(() => {});
  if (server && server.exitCode === null) {
    const stopped = once(server, 'exit');
    server.kill('SIGTERM');
    const timer = setTimeout(() => server.kill('SIGKILL'), 5000);
    await stopped;
    clearTimeout(timer);
  }
  fs.rmSync(temporary, { recursive: true, force: true });
});
