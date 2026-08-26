import { Picoboot } from 'https://cdn.jsdelivr.net/gh/piersfinlayson/picoflash@678355430aff0ee9efa6d552fb81832a91d89ef4/pkg/picoboot.js';
import { Target } from 'https://cdn.jsdelivr.net/gh/piersfinlayson/picoflash@678355430aff0ee9efa6d552fb81832a91d89ef4/pkg/target.js';
import { uf2ToFlashBuffer } from 'https://cdn.jsdelivr.net/gh/piersfinlayson/picoflash@678355430aff0ee9efa6d552fb81832a91d89ef4/js/uf2/uf2.js';

const appsEl = document.querySelector('#apps');
const searchEl = document.querySelector('#search');
const groupFilterEl = document.querySelector('#group-filter');
const categoryFilterEl = document.querySelector('#category-filter');
const statusEl = document.querySelector('#status');
const warningEl = document.querySelector('#browser-warning');
const overlayEl = document.querySelector('#flash-overlay');
const stageEl = document.querySelector('#flash-stage');
const progressEl = document.querySelector('#progress-bar');

let apps = [];
let busy = false;

const supportsWebUsb = 'usb' in navigator;
if (!supportsWebUsb) warningEl.classList.remove('hidden');

function setStatus(text, kind = '') {
  statusEl.textContent = text;
  statusEl.className = `status ${kind}`.trim();
}

function setStage(text, progress) {
  stageEl.textContent = text;
  progressEl.style.width = `${progress}%`;
}

function setBusy(value) {
  busy = value;
  document.querySelectorAll('.flash-btn').forEach((button) => {
    button.disabled = value || !supportsWebUsb;
  });
  overlayEl.classList.toggle('hidden', !value);
}

function firmwareUrl(app) {
  return `./firmware/${app.id}.uf2`;
}

function updateCategoryFilter() {
  const group = groupFilterEl.value;
  const categories = [...new Set(
    apps
      .filter((app) => group === 'all' || app.group === group)
      .map((app) => app.category)
  )].sort();

  const previous = categoryFilterEl.value;
  categoryFilterEl.replaceChildren(new Option('All categories', 'all'));
  for (const category of categories) {
    categoryFilterEl.append(new Option(category, category));
  }
  categoryFilterEl.value = categories.includes(previous) ? previous : 'all';
}

function render() {
  const needle = searchEl.value.trim().toLowerCase();
  const group = groupFilterEl.value;
  const category = categoryFilterEl.value;

  const visible = apps.filter((app) => {
    const matchesSearch = `${app.name} ${app.group} ${app.category}`.toLowerCase().includes(needle);
    const matchesGroup = group === 'all' || app.group === group;
    const matchesCategory = category === 'all' || app.category === category;
    return matchesSearch && matchesGroup && matchesCategory;
  });

  appsEl.replaceChildren();
  if (!visible.length) {
    const empty = document.createElement('div');
    empty.className = 'empty';
    empty.textContent = 'No results';
    appsEl.append(empty);
    return;
  }

  for (const app of visible) {
    const card = document.createElement('article');
    card.className = 'card';

    const head = document.createElement('div');
    head.className = 'card-head';

    const title = document.createElement('h2');
    title.textContent = app.name;

    const tag = document.createElement('span');
    tag.className = `tag${app.tested ? '' : ' untested'}`;
    tag.textContent = app.tested ? `${app.group} · ${app.category}` : `${app.group} · ${app.category} · Untested`;
    head.append(title, tag);

    const actions = document.createElement('div');
    actions.className = 'actions';

    const flash = document.createElement('button');
    flash.className = 'flash-btn';
    flash.type = 'button';
    flash.textContent = 'Flash';
    flash.disabled = busy || !supportsWebUsb;
    flash.addEventListener('click', () => flashApp(app));

    const download = document.createElement('a');
    download.className = 'download';
    download.href = firmwareUrl(app);
    download.download = `${app.id}.uf2`;
    download.title = 'Download UF2';
    download.setAttribute('aria-label', `Download ${app.name} UF2`);
    download.textContent = '↓';

    actions.append(flash, download);
    card.append(head, actions);
    appsEl.append(card);
  }
}

function validateFlashRange(address, data) {
  const flashStart = 0x10000000;
  const flashEnd = 0x10200000;
  if (address < flashStart || address >= flashEnd) throw new Error(`Unexpected flash address 0x${address.toString(16)}`);
  if (address + data.length > flashEnd) throw new Error('Firmware is larger than the XIAO RP2350 flash range');
  if (address % 4096 !== 0) throw new Error('Firmware start address is not flash-sector aligned');
}

async function flashApp(app) {
  if (busy || !supportsWebUsb) return;

  let picoboot = null;
  let connection = null;
  setBusy(true);
  setStatus(app.name);
  setStage('Loading…', 10);

  try {
    const response = await fetch(firmwareUrl(app), { cache: 'no-store' });
    if (!response.ok) throw new Error(`Firmware unavailable (${response.status})`);

    const uf2 = new Uint8Array(await response.arrayBuffer());
    if (uf2.length === 0 || uf2.length % 512 !== 0) throw new Error('Invalid UF2');

    const { address, data } = uf2ToFlashBuffer(uf2);
    validateFlashRange(address, data);

    setStage('Select RP2 Boot', 22);
    picoboot = await Picoboot.requestDevice([new Target('RP2350')]);

    setStage('Connecting…', 35);
    connection = await picoboot.connect();
    if (picoboot.getTarget().type !== 'RP2350') throw new Error('Not RP2350');

    setStage('Writing…', 55);
    await picoboot.flashEraseAndWrite(address, data);

    setStage('Rebooting…', 92);
    try { await connection.reboot(100); } catch (error) { console.debug(error); }
    try { await picoboot.disconnect(); } catch (error) { console.debug(error); }

    setStage('Done', 100);
    setStatus('Done', 'success');
    await new Promise((resolve) => setTimeout(resolve, 650));
  } catch (error) {
    console.error(error);
    const cancelled = error?.name === 'NotFoundError' || /cancelled|no device selected/i.test(error?.message || '');
    setStatus(cancelled ? 'Cancelled' : 'Error', cancelled ? '' : 'error');
    try { if (picoboot) await picoboot.disconnect(); } catch (disconnectError) { console.debug(disconnectError); }
  } finally {
    setBusy(false);
    progressEl.style.width = '8%';
  }
}

searchEl.addEventListener('input', render);
groupFilterEl.addEventListener('change', () => {
  updateCategoryFilter();
  render();
});
categoryFilterEl.addEventListener('change', render);

try {
  const response = await fetch('./manifest.json', { cache: 'no-store' });
  if (!response.ok) throw new Error(`manifest.json: ${response.status}`);
  apps = await response.json();
  updateCategoryFilter();
  render();
  if (!supportsWebUsb) setStatus('WebUSB unavailable', 'error');
} catch (error) {
  console.error(error);
  appsEl.innerHTML = '<div class="empty">Unavailable</div>';
  setStatus('Error', 'error');
}
